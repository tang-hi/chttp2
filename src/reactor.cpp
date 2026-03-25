#include "chttp2/reactor.hpp"

#include <algorithm>
#include <cerrno>

#include "chttp2/log.hpp"

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace chttp2 {

bool Reactor::start() {
  if (running.load()) {
    return true;
  }

  if (!setupPlatformPrimitives()) {
    CHTTP2_LOG_ERROR("reactor: failed to setup platform primitives");
    return false;
  }
  running.store(true);

  worker = std::thread(&Reactor::runLoop, this);
  CHTTP2_LOG_DEBUG("reactor: started");
  return true;
}

void Reactor::stop() {
  if (!running.load()) {
    return;
  }

  CHTTP2_LOG_DEBUG("reactor: stopping");
  running.store(false);
  wakeup();

  if (worker.joinable()) {
    worker.join();
  }

  teardownPlatformPrimitives();

  std::queue<Task> emptyTasks;
  {
    std::lock_guard<std::mutex> lock(mutex);
    tasks.swap(emptyTasks);
    while (!timers.empty()) {
      timers.pop();
    }
    canceledTimers.clear();
  }
}

bool Reactor::isRunning() const {
  return running.load();
}

bool Reactor::post(const Task& task) {
  if (!task) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!running.load()) {
      return false;
    }
    tasks.push(task);
  }

  wakeup();
  return true;
}

bool Reactor::registerFd(int fd, const FdHandler& readHandler, const FdHandler& writeHandler) {
  if (fd < 0 || !readHandler) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex);
  if (!running.load()) {
    return false;
  }
  if (fdContexts.find(fd) != fdContexts.end()) {
    return false;
  }

#if defined(__linux__)
  epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = fd;
  if (epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &ev) != 0) {
    return false;
  }
#elif defined(__APPLE__)
  struct kevent ev;
  EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
  if (kevent(kqueueFd, &ev, 1, nullptr, 0, nullptr) != 0) {
    return false;
  }
#endif

  FdContext ctx;
  ctx.readHandler = readHandler;
  ctx.writeHandler = writeHandler;
  fdContexts[fd] = ctx;
  return true;
}

bool Reactor::unregisterFd(int fd) {
  std::lock_guard<std::mutex> lock(mutex);
  auto it = fdContexts.find(fd);
  if (it == fdContexts.end()) {
    return false;
  }

#if defined(__linux__)
  epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, nullptr);
#elif defined(__APPLE__)
  struct kevent evs[2];
  EV_SET(&evs[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
  kevent(kqueueFd, &evs[0], 1, nullptr, 0, nullptr);
  if (it->second.writeEnabled) {
    EV_SET(&evs[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    kevent(kqueueFd, &evs[1], 1, nullptr, 0, nullptr);
  }
#endif

  fdContexts.erase(it);
  return true;
}

bool Reactor::enableWrite(int fd, bool enable) {
  std::lock_guard<std::mutex> lock(mutex);
  auto it = fdContexts.find(fd);
  if (it == fdContexts.end()) {
    return false;
  }

  it->second.writeEnabled = enable && static_cast<bool>(it->second.writeHandler);

#if defined(__linux__)
  epoll_event ev;
  ev.events = EPOLLIN;
  if (it->second.writeEnabled) {
    ev.events |= EPOLLOUT;
  }
  ev.data.fd = fd;

  if (epoll_ctl(epollFd, EPOLL_CTL_MOD, fd, &ev) != 0) {
    return false;
  }
#elif defined(__APPLE__)
  struct kevent ev;
  if (it->second.writeEnabled) {
    EV_SET(&ev, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
  } else {
    EV_SET(&ev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
  }
  kevent(kqueueFd, &ev, 1, nullptr, 0, nullptr);
#endif

  return true;
}

Reactor::TimerId Reactor::scheduleOnce(int timeoutMs, const TimerHandler& handler) {
  if (!handler) {
    return 0;
  }

  TimerId timerId = nextTimerId.fetch_add(1);
  auto due = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 0));

  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!running.load()) {
      return 0;
    }

    TimerItem item;
    item.due = due;
    item.id = timerId;
    item.handler = handler;
    timers.push(item);
    canceledTimers[timerId] = false;
    armNextTimerLocked();
  }

  return timerId;
}

bool Reactor::cancelTimer(TimerId timerId) {
  std::lock_guard<std::mutex> lock(mutex);
  auto it = canceledTimers.find(timerId);
  if (it == canceledTimers.end()) {
    return false;
  }
  it->second = true;
  armNextTimerLocked();
  return true;
}

void Reactor::runLoop() {
#if defined(__linux__)
  static const int maxEvents = 32;
  epoll_event events[maxEvents];
#elif defined(__APPLE__)
  static const int maxEvents = 32;
  struct kevent events[maxEvents];
#endif

  while (running.load()) {
#if defined(__linux__)
    int eventCount = epoll_wait(epollFd, events, maxEvents, -1);
#elif defined(__APPLE__)
    int eventCount = kevent(kqueueFd, nullptr, 0, events, maxEvents, nullptr);
#endif
    if (eventCount < 0) {
      if (errno == EINTR) {
        continue;
      }
      CHTTP2_LOG_ERROR("reactor: event loop error, errno=%d", errno);
      break;
    }

    for (int i = 0; i < eventCount; ++i) {
#if defined(__linux__)
      int fd = events[i].data.fd;
      if (fd == wakeupFd) {
        drainWakeupFd();
      } else if (fd == timerFd) {
        fireDueTimers();
      } else {
        bool readable = (events[i].events & EPOLLIN) != 0;
        bool writable = (events[i].events & EPOLLOUT) != 0;
        bool error = (events[i].events & (EPOLLERR | EPOLLHUP)) != 0;
        handleFdEvent(fd, readable, writable, error);
      }
#elif defined(__APPLE__)
      auto& ev = events[i];
      if (ev.filter == EVFILT_READ && static_cast<int>(ev.ident) == wakeupPipe[0]) {
        drainWakeupFd();
      } else if (ev.filter == EVFILT_TIMER && ev.ident == kTimerIdent) {
        fireDueTimers();
      } else {
        int fd = static_cast<int>(ev.ident);
        bool readable = (ev.filter == EVFILT_READ);
        bool writable = (ev.filter == EVFILT_WRITE);
        bool error = (ev.flags & EV_EOF) != 0 && ev.fflags != 0;
        handleFdEvent(fd, readable, writable, error);
      }
#endif
    }

    drainPostedTasks();
  }

  drainPostedTasks();
}

void Reactor::drainPostedTasks() {
  while (true) {
    Task task;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (tasks.empty()) {
        break;
      }
      task = tasks.front();
      tasks.pop();
    }

    if (task) {
      task();
    }
  }
}

// ============================================================================
// Linux platform implementation (epoll + eventfd + timerfd)
// ============================================================================
#if defined(__linux__)

bool Reactor::setupPlatformPrimitives() {
  epollFd = epoll_create1(EPOLL_CLOEXEC);
  if (epollFd < 0) {
    return false;
  }

  wakeupFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wakeupFd < 0) {
    teardownPlatformPrimitives();
    return false;
  }

  timerFd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (timerFd < 0) {
    teardownPlatformPrimitives();
    return false;
  }

  epoll_event wakeEvent;
  wakeEvent.events = EPOLLIN;
  wakeEvent.data.fd = wakeupFd;
  if (epoll_ctl(epollFd, EPOLL_CTL_ADD, wakeupFd, &wakeEvent) != 0) {
    teardownPlatformPrimitives();
    return false;
  }

  epoll_event timerEvent;
  timerEvent.events = EPOLLIN;
  timerEvent.data.fd = timerFd;
  if (epoll_ctl(epollFd, EPOLL_CTL_ADD, timerFd, &timerEvent) != 0) {
    teardownPlatformPrimitives();
    return false;
  }

  return true;
}

void Reactor::teardownPlatformPrimitives() {
  if (timerFd >= 0) {
    close(timerFd);
    timerFd = -1;
  }
  if (wakeupFd >= 0) {
    close(wakeupFd);
    wakeupFd = -1;
  }
  if (epollFd >= 0) {
    close(epollFd);
    epollFd = -1;
  }

  std::lock_guard<std::mutex> lock(mutex);
  fdContexts.clear();
}

void Reactor::wakeup() {
  if (wakeupFd < 0) {
    return;
  }

  std::uint64_t one = 1;
  ssize_t ret = write(wakeupFd, &one, sizeof(one));
  (void) ret;
}

void Reactor::drainWakeupFd() {
  std::uint64_t value = 0;
  while (read(wakeupFd, &value, sizeof(value)) > 0) {}
}

void Reactor::armNextTimerLocked() {
  if (timerFd < 0) {
    return;
  }

  while (!timers.empty()) {
    const TimerItem top = timers.top();
    auto it = canceledTimers.find(top.id);
    // If the timer is canceled, remove it and continue to the next one
    if (it != canceledTimers.end() && it->second) {
      canceledTimers.erase(it);
      timers.pop();
      continue;
    }
    break;
  }

  itimerspec spec;
  spec.it_interval.tv_sec = 0;
  spec.it_interval.tv_nsec = 0;
  spec.it_value.tv_sec = 0;
  spec.it_value.tv_nsec = 0;

  if (!timers.empty()) {
    auto now = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timers.top().due - now);
    if (ns.count() < 1000000) {
      ns = std::chrono::milliseconds(1);
    }

    auto sec = std::chrono::duration_cast<std::chrono::seconds>(ns);
    auto nsec = ns - sec;

    spec.it_value.tv_sec = sec.count();
    spec.it_value.tv_nsec = nsec.count();
  }

  timerfd_settime(timerFd, 0, &spec, nullptr);
}

// ============================================================================
// macOS platform implementation (kqueue + pipe + EVFILT_TIMER)
// ============================================================================
#elif defined(__APPLE__)

bool Reactor::setupPlatformPrimitives() {
  kqueueFd = kqueue();
  if (kqueueFd < 0) {
    return false;
  }

  if (pipe(wakeupPipe) != 0) {
    teardownPlatformPrimitives();
    return false;
  }

  // Set both ends non-blocking
  for (int i = 0; i < 2; ++i) {
    int flags = fcntl(wakeupPipe[i], F_GETFL, 0);
    fcntl(wakeupPipe[i], F_SETFL, flags | O_NONBLOCK);
    fcntl(wakeupPipe[i], F_SETFD, FD_CLOEXEC);
  }

  // Register read end of pipe for wakeup
  struct kevent ev;
  EV_SET(&ev, wakeupPipe[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
  if (kevent(kqueueFd, &ev, 1, nullptr, 0, nullptr) != 0) {
    teardownPlatformPrimitives();
    return false;
  }

  return true;
}

void Reactor::teardownPlatformPrimitives() {
  if (wakeupPipe[0] >= 0) {
    close(wakeupPipe[0]);
    wakeupPipe[0] = -1;
  }
  if (wakeupPipe[1] >= 0) {
    close(wakeupPipe[1]);
    wakeupPipe[1] = -1;
  }
  if (kqueueFd >= 0) {
    close(kqueueFd);
    kqueueFd = -1;
  }

  std::lock_guard<std::mutex> lock(mutex);
  fdContexts.clear();
}

void Reactor::wakeup() {
  if (wakeupPipe[1] < 0) {
    return;
  }

  char one = 1;
  ssize_t ret = write(wakeupPipe[1], &one, sizeof(one));
  (void) ret;
}

void Reactor::drainWakeupFd() {
  char buf[64];
  while (read(wakeupPipe[0], buf, sizeof(buf)) > 0) {}
}

void Reactor::armNextTimerLocked() {
  if (kqueueFd < 0) {
    return;
  }

  while (!timers.empty()) {
    const TimerItem top = timers.top();
    auto it = canceledTimers.find(top.id);
    if (it != canceledTimers.end() && it->second) {
      canceledTimers.erase(it);
      timers.pop();
      continue;
    }
    break;
  }

  struct kevent ev;
  if (timers.empty()) {
    // Disable the timer
    EV_SET(&ev, kTimerIdent, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
    kevent(kqueueFd, &ev, 1, nullptr, 0, nullptr);
  } else {
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timers.top().due - now);
    if (ms.count() < 1) {
      ms = std::chrono::milliseconds(1);
    }

    EV_SET(&ev, kTimerIdent, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, ms.count(), nullptr);
    kevent(kqueueFd, &ev, 1, nullptr, 0, nullptr);
  }
}

#endif

void Reactor::fireDueTimers() {
#if defined(__linux__)
  // Drain the timerfd
  std::uint64_t expirations = 0;
  while (read(timerFd, &expirations, sizeof(expirations)) > 0) {}
#endif

  std::vector<TimerHandler> dueHandlers;

  {
    std::lock_guard<std::mutex> lock(mutex);
    auto now = std::chrono::steady_clock::now();

    while (!timers.empty()) {
      const TimerItem top = timers.top();
      auto it = canceledTimers.find(top.id);
      // If the timer is canceled, remove it and continue to the next one
      if (it != canceledTimers.end() && it->second) {
        canceledTimers.erase(it);
        timers.pop();
        continue;
      }
      if (top.due > now) {
        break;
      }

      dueHandlers.push_back(top.handler);
      canceledTimers.erase(top.id);
      timers.pop();
    }

    armNextTimerLocked();
  }

  for (const auto& dueHandler : dueHandlers) {
    if (dueHandler) {
      dueHandler();
    }
  }
}

void Reactor::handleFdEvent(int fd, bool readable, bool writable, bool error) {
  FdHandler readHandler;
  FdHandler writeHandler;
  bool writeEnabled = false;

  {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = fdContexts.find(fd);
    if (it == fdContexts.end()) {
      return;
    }
    readHandler = it->second.readHandler;
    writeHandler = it->second.writeHandler;
    writeEnabled = it->second.writeEnabled;
  }

  if (error && readHandler) {
    readHandler();
    return;
  }

  if (readable && readHandler) {
    readHandler();
  }

  if (writable && writeEnabled && writeHandler) {
    writeHandler();
  }
}

}  // namespace chttp2
