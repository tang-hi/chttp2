#include "chttp2/callback_dispatcher.hpp"

namespace chttp2 {

bool CallbackDispatcher::start() {
  if (running.load()) {
    return true;
  }

  {
    std::lock_guard<std::mutex> lock(mutex);
    stopRequested = false;
  }

  running.store(true);
  worker = std::thread(&CallbackDispatcher::runLoop, this);
  return true;
}

void CallbackDispatcher::stop() {
  if (!running.load()) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mutex);
    stopRequested = true;
  }
  cv.notify_all();

  if (worker.joinable()) {
    worker.join();
  }

  running.store(false);
  std::queue<Task> empty;
  {
    std::lock_guard<std::mutex> lock(mutex);
    tasks.swap(empty);
  }
}

bool CallbackDispatcher::post(const Task& task) {
  if (!task) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!running.load() || stopRequested) {
      return false;
    }
    tasks.push(task);
  }

  cv.notify_one();
  return true;
}

void CallbackDispatcher::runLoop() {
  while (true) {
    Task task;
    {
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait(lock, [this] { return stopRequested || !tasks.empty(); });

      if (stopRequested && tasks.empty()) {
        return;
      }

      task = tasks.front();
      tasks.pop();
    }

    if (task) {
      task();
    }
  }
}

}  // namespace chttp2
