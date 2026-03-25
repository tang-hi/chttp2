#pragma once

#include <fcntl.h>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "chttp2/client.hpp"
#include "chttp2/endpoint.hpp"

#ifndef TESTSERVER_BIN
#error "TESTSERVER_BIN must be defined (path to the Go test server binary)" // NOLINT(pp_hash_error)
#endif

class GoServerProcess {
 public:
  GoServerProcess() = default;
  ~GoServerProcess() { stop(); }

  GoServerProcess(const GoServerProcess&) = delete;
  GoServerProcess& operator=(const GoServerProcess&) = delete;

  bool start(bool useTLS = false) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
      return false;
    }

    goPid = fork();
    if (goPid < 0) {
      close(pipefd[0]);
      close(pipefd[1]);
      return false;
    }

    if (goPid == 0) {
      // Child: redirect stdout to pipe.
      close(pipefd[0]);
      dup2(pipefd[1], STDOUT_FILENO);
      close(pipefd[1]);

      // Close all inherited FDs (e.g. Make jobserver pipes) except
      // stdin/stdout/stderr to avoid interfering with the Go server.
      for (int fd = 3; fd < 256; fd++) {
        close(fd);
      }

      std::string bin = TESTSERVER_BIN; // NOLINT(undeclared_var_use)
      if (useTLS) {
        execl(bin.c_str(), bin.c_str(), "--tls", nullptr);
      } else {
        execl(bin.c_str(), bin.c_str(), nullptr);
      }
      _exit(1);
    }

    // Parent: read server output.
    close(pipefd[1]);
    rfd = pipefd[0];

    // Read "LISTENING :PORT"
    std::string line = readLine();
    if (line.substr(0, 10) != "LISTENING ") {
      stop();
      return false;
    }
    auto colonPos = line.find(':');
    if (colonPos == std::string::npos) {
      stop();
      return false;
    }
    goPort = std::stoi(line.substr(colonPos + 1));

    // For TLS, read "CERT /path/to/cert.pem"
    if (useTLS) {
      std::string certLine = readLine();
      if (certLine.substr(0, 5) != "CERT ") {
        stop();
        return false;
      }
      cert = certLine.substr(5);
    }

    return true;
  }

  void stop() {
    if (goPid > 0) {
      kill(goPid, SIGTERM);
      int status = 0;
      waitpid(goPid, &status, 0);
      goPid = -1;
    }
    if (rfd >= 0) {
      close(rfd);
      rfd = -1;
    }
  }

  int port() const { return goPort; }
  pid_t pid() const { return goPid; }
  const std::string& certPath() const { return cert; }

  chttp2::Endpoint endpoint() const {
    chttp2::Endpoint ep;
    ep.host = "127.0.0.1";
    ep.port = static_cast<uint16_t>(goPort);
    return ep;
  }

  chttp2::Endpoint tlsEndpoint() const {
    chttp2::Endpoint ep;
    ep.host = "127.0.0.1";
    ep.port = static_cast<uint16_t>(goPort);
    ep.useSSL = true;
    ep.certFile = cert;
    return ep;
  }

 private:
  std::string readLine() {
    std::string line;
    char ch;
    while (true) {
      ssize_t n = read(rfd, &ch, 1);
      if (n <= 0) {
        break;
      }
      if (ch == '\n') {
        break;
      }
      line += ch;
    }
    // Trim trailing \r if present.
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    return line;
  }

  pid_t goPid{-1};
  int rfd{-1};
  int goPort{0};
  std::string cert;
};
