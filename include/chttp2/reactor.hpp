#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

#include "chttp2/platform.hpp"

namespace chttp2 {

class Reactor {
 public:
  using Task = std::function<void()>;
  using FdHandler = std::function<void()>;
  using TimerHandler = std::function<void()>;
  using TimerId = std::uint64_t;

  Reactor() = default;
  ~Reactor() { stop(); }

  Reactor(const Reactor&) = delete;
  Reactor& operator=(const Reactor&) = delete;

  bool start();
  void stop();
  bool isRunning() const;
  std::thread::id threadId() const { return worker.get_id(); }

  bool post(const Task& task);

  bool registerFd(socket_t fd,
                  const FdHandler& readHandler,
                  const FdHandler& writeHandler = FdHandler());
  bool unregisterFd(socket_t fd);
  bool enableWrite(socket_t fd, bool enable);

  TimerId scheduleOnce(int timeoutMs, const TimerHandler& handler);
  bool cancelTimer(TimerId timerId);

 private:
  struct TimerItem {
    std::chrono::steady_clock::time_point due;
    TimerId id;
    TimerHandler handler;
  };

  struct TimerItemCompare {
    bool operator()(const TimerItem& lhs, const TimerItem& rhs) const { return lhs.due > rhs.due; }
  };

  void runLoop();
  void drainPostedTasks();

  struct FdContext {
    FdHandler readHandler;
    FdHandler writeHandler;
    bool writeEnabled{false};
  };

  bool setupPlatformPrimitives();
  void teardownPlatformPrimitives();
  void wakeup();
  void drainWakeupFd();
  void handleFdEvent(socket_t fd, bool readable, bool writable, bool error);
  void fireDueTimers();
  void armNextTimerLocked();

  std::unordered_map<socket_t, FdContext> fdContexts;

#if defined(__linux__)
  int epollFd{-1};
  int wakeupFd{-1};
  int timerFd{-1};
#elif defined(__APPLE__)
  int kqueueFd{-1};
  int wakeupPipe[2]{-1, -1};
  static const std::uintptr_t kTimerIdent = 0xFFFFFFFF;
#elif defined(_WIN32)
  socket_t wakeupPair[2]{INVALID_SOCKET, INVALID_SOCKET};
#endif

  std::atomic<bool> running{false};
  std::thread worker;

  mutable std::mutex mutex;
  std::queue<Task> tasks;
  std::priority_queue<TimerItem, std::vector<TimerItem>, TimerItemCompare> timers;
  std::unordered_map<TimerId, bool> canceledTimers;
  std::atomic<TimerId> nextTimerId{1};
};

}  // namespace chttp2
