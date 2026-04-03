#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>

namespace chttp2 {

enum class LogLevel : std::uint8_t { DEBUG, INFO, WARN, ERR };

using LogHandler = std::function<void(LogLevel level, const char* msg)>;

inline LogHandler& getLogHandler() {
  static LogHandler handler = nullptr;
  return handler;
}

inline void setLogHandler(const LogHandler& handler) {
  getLogHandler() = handler;
}

#ifndef CHTTP2_DISABLE_LOGGING

inline void log(LogLevel level, const char* fmt, ...) {
  auto& handler = getLogHandler();
  if (!handler) {
    return;
  }
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  handler(level, buf);
}

#define CHTTP2_LOG_DEBUG(...) ::chttp2::log(::chttp2::LogLevel::DEBUG, __VA_ARGS__)
#define CHTTP2_LOG_INFO(...) ::chttp2::log(::chttp2::LogLevel::INFO, __VA_ARGS__)
#define CHTTP2_LOG_WARN(...) ::chttp2::log(::chttp2::LogLevel::WARN, __VA_ARGS__)
#define CHTTP2_LOG_ERROR(...) ::chttp2::log(::chttp2::LogLevel::ERR, __VA_ARGS__)

#else

#define CHTTP2_LOG_DEBUG(...) (void) 0
#define CHTTP2_LOG_INFO(...) (void) 0
#define CHTTP2_LOG_WARN(...) (void) 0
#define CHTTP2_LOG_ERROR(...) (void) 0

#endif

}  // namespace chttp2
