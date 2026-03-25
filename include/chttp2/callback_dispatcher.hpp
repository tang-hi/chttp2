#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

namespace chttp2 {

class CallbackDispatcher {
 public:
  using Task = std::function<void()>;

  CallbackDispatcher() = default;
  ~CallbackDispatcher() { stop(); }

  CallbackDispatcher(const CallbackDispatcher&) = delete;
  CallbackDispatcher& operator=(const CallbackDispatcher&) = delete;

  bool start();
  void stop();
  bool post(const Task& task);
  bool isRunning() const { return running.load(); }

 private:
  void runLoop();

  std::atomic<bool> running{false};
  bool stopRequested{false};
  std::thread worker;
  mutable std::mutex mutex;
  std::condition_variable cv;
  std::queue<Task> tasks;
};

}  // namespace chttp2
