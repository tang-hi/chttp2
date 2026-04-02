#pragma once

// Platform detection, cross-platform socket types, and unified socket API.
//
// All platform-specific socket logic lives in this header (inline helpers)
// and in src/platform.cpp (non-trivial implementations).  The rest of the
// library should include this header and call the chttp2:: functions below
// instead of using #ifdefs directly.

// ============================================================================
// Platform includes and type definitions
// ============================================================================

#if defined(_WIN32)

#ifndef FD_SETSIZE
#define FD_SETSIZE 256
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#ifdef _MSC_VER
#include <basetsd.h>
typedef SSIZE_T ssize_t;
#endif

using socket_t = SOCKET;
// INVALID_SOCKET is already defined by winsock2.h.

#else  // POSIX (Linux / macOS)

#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

using socket_t = int;
constexpr socket_t INVALID_SOCKET = -1;

#endif

// Common standard headers used alongside the platform API.
#include <cstddef>
#include <cstring>
#include <string>

namespace chttp2 {

// ============================================================================
// Initialization
// ============================================================================

/// Call once (idempotent) before any socket operation.  No-op on POSIX.
inline void ensureWinsockInitialized() {
#if defined(_WIN32)
  struct WinsockInit {
    WinsockInit() {
      WSADATA data;
      WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockInit() { WSACleanup(); }
  };
  static WinsockInit init;
#endif
}

// ============================================================================
// Error handling (inline — hot-path)
// ============================================================================

inline int lastSocketError() {
#if defined(_WIN32)
  return WSAGetLastError();
#else
  return errno;
#endif
}

inline void setLastSocketError(int err) {
#if defined(_WIN32)
  WSASetLastError(err);
#else
  errno = err;
#endif
}

inline bool isWouldBlock(int err) {
#if defined(_WIN32)
  return err == WSAEWOULDBLOCK;
#else
  return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

inline bool isConnectionReset(int err) {
#if defined(_WIN32)
  return err == WSAECONNRESET || err == WSAECONNABORTED;
#else
  return err == EPIPE || err == ECONNRESET;
#endif
}

inline bool isInProgress(int err) {
#if defined(_WIN32)
  return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
  return err == EINPROGRESS;
#endif
}

inline bool isInterrupted(int err) {
#if defined(_WIN32)
  return err == WSAEINTR;
#else
  return err == EINTR;
#endif
}

inline bool isTimedOut(int err) {
#if defined(_WIN32)
  return err == WSAETIMEDOUT;
#else
  return err == ETIMEDOUT;
#endif
}

/// Platform-specific "connection timed out" error code.
#if defined(_WIN32)
static const int SOCKET_TIMED_OUT = WSAETIMEDOUT;
#else
static const int SOCKET_TIMED_OUT = ETIMEDOUT;
#endif

// ============================================================================
// Logging helpers
// ============================================================================

/// Cast socket_t to int for printf-style log format strings.
/// On POSIX socket_t is int, so this avoids a GCC -Wuseless-cast warning.
inline int fdToInt(socket_t fd) {
#if defined(_WIN32)
  return static_cast<int>(fd);
#else
  return fd;
#endif
}

// ============================================================================
// Socket lifecycle (inline — trivial)
// ============================================================================

inline void closeSocket(socket_t fd) {
#if defined(_WIN32)
  ::closesocket(fd);
#else
  ::close(fd);
#endif
}

// ============================================================================
// Socket operations (implemented in platform.cpp)
// ============================================================================

/// Set O_NONBLOCK / FIONBIO on a socket.
bool setNonBlocking(socket_t fd, bool enable);

/// Enable/disable TCP keep-alive.
bool setKeepAlive(socket_t fd, bool enable);

/// Set TCP keep-alive timing parameters.
bool setKeepAliveParams(socket_t fd, int idleSec, int intervalSec, int count);

/// Graceful shutdown (SHUT_RDWR / SD_BOTH).
bool shutdownBoth(socket_t fd);

/// Set SO_NOSIGPIPE (macOS) — no-op on Linux/Windows.
void suppressSigpipe(socket_t fd);

/// Platform-aware recv() wrapper.
ssize_t socketRecv(socket_t fd, void* data, size_t len);

/// Platform-aware send() wrapper (uses MSG_NOSIGNAL on Linux).
ssize_t socketSend(socket_t fd, const void* data, size_t len);

/// Poll a socket for write-readiness (connect completion).
/// Returns: 1 = ready, 0 = timeout, -1 = error.  Retries on EINTR.
int pollForWrite(socket_t fd, int timeoutMs);

/// Read SO_ERROR from a socket.
bool getSocketError(socket_t fd, int* errOut);

/// Human-readable description of a socket error code.
std::string socketErrorString(int err);

}  // namespace chttp2
