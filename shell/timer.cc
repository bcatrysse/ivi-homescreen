/*
 * @copyright Copyright (c) 2022 Woven Alpha, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "timer.h"

#include <cerrno>
#include <cstring>
#include <memory>
#include <stdexcept>

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "logging.h"

#define DEBUG_EVENT_TIMER 0

uint32_t EventTimer::watched_fd = 0;
int EventTimer::ev_fd = -1;
std::mutex EventTimer::s_mutex;

EventTimer::EventTimer(const int clock,
                       const evtimer_cb callback,
                       void* callback_data)
    : m_callback(callback), m_callback_data(callback_data) {
  SPDLOG_TRACE("+ EventTimer()");

  // Create the per-instance timerfd before taking the class-wide lock so we
  // minimize the time spent holding it.
  m_timerfd = timerfd_create(clock, TFD_CLOEXEC | TFD_NONBLOCK);
  if (m_timerfd == -1) {
    const auto msg = fmt::format("EventTimer: timerfd_create failed: {}",
                                 strerror(errno));
    spdlog::critical(msg);
    // throw instead of exit(): exit() bypasses destructors for all
    // already-constructed members (m_callback, m_callback_data).
    throw std::runtime_error(msg);
  }

  // Initialize instance-only members before taking the class-wide lock.
  m_task.run = EventTimer::run;
  m_task.data = reinterpret_cast<void*>(this);

  {
    std::lock_guard lock(s_mutex);
    // evfd is the shared epoll fd for all EventTimer instances.  Only the
    // first constructor creates it; subsequent ones reuse it.
    if (ev_fd < 0) {
      ev_fd = epoll_create1(EPOLL_CLOEXEC);
      if (ev_fd < 0) {
        if (errno != EINVAL) {
          const auto msg = fmt::format(
              "EventTimer: epoll_create1(EPOLL_CLOEXEC) failed: {}",
              strerror(errno));
          // throw instead of exit(): exit() here fires while holding s_mutex,
          // permanently deadlocking any subsequent EventTimer construction.
          throw std::runtime_error(msg);
        } else {
          // fallback for kernels that don't support EPOLL_CLOEXEC
          ev_fd = epoll_create(1);
          if (ev_fd < 0) {
            const auto msg = fmt::format(
                "EventTimer: epoll_create(1) failed: {}", strerror(errno));
            throw std::runtime_error(msg);
          }
        }
      }
    }
    // Register our timerfd with the shared epoll instance and increment the
    // reference count.  Both operations must be inside the same lock scope so
    // the destructor's watched_fd == 0 → close_evfd invariant is maintained.
    _watch_fd_locked(m_timerfd, EPOLLIN, &m_task);
  }

  SPDLOG_TRACE("- EventTimer()");
}

EventTimer::~EventTimer() {
  SPDLOG_TRACE("+ ~EventTimer()");

  {
    std::lock_guard lock(s_mutex);
    _unwatch_fd_locked(m_timerfd);
    // Close the shared epoll fd only when the last instance has unregistered.
    // Both the decrement and the close check must be inside the same lock scope
    // to prevent two concurrent destructors from both seeing watched_fd == 0.
    if (watched_fd == 0)
      _close_evfd_locked();
  }

  close(m_timerfd);
  m_timerfd = -1;

  SPDLOG_TRACE("- ~EventTimer()");
}

// ---------------------------------------------------------------------------
// Internal helpers — called with s_mutex already held by the caller.
// ---------------------------------------------------------------------------

void EventTimer::_close_evfd_locked() {
  SPDLOG_TRACE("+ EventTimer::_close_evfd_locked()");
  close(ev_fd);
  ev_fd = -1;
  SPDLOG_TRACE("- EventTimer::_close_evfd_locked()");
}

void EventTimer::_watch_fd_locked(int fd,
                                  uint32_t events,
                                  struct timer_task* task) {
  if (ev_fd < 0) {
    spdlog::critical("_watch_fd_locked: evfd is invalid. Ignored.");
    return;
  }

  struct epoll_event ep{};
  ep.events = events;
  ep.data.ptr = task;
  if (epoll_ctl(ev_fd, EPOLL_CTL_ADD, fd, &ep) < 0) {
    spdlog::critical("_watch_fd: epoll_ctl(EPOLL_CTL_ADD, fd={}) failed: {}",
                     fd, strerror(errno));
    return;
  }
  watched_fd++;
}

void EventTimer::_unwatch_fd_locked(int fd) {
  if (ev_fd < 0) {
    spdlog::critical("_unwatch_fd_locked: evfd is invalid. Ignored.");
    return;
  }
  if (epoll_ctl(ev_fd, EPOLL_CTL_DEL, fd, nullptr) < 0) {
    spdlog::error("_unwatch_fd: epoll_ctl(EPOLL_CTL_DEL, fd={}) failed: {}", fd,
                  strerror(errno));
    return;
  }
  watched_fd--;
}

// ---------------------------------------------------------------------------
// Public / legacy entry-points — acquire s_mutex and delegate to locked impl.
// ---------------------------------------------------------------------------

void EventTimer::close_evfd() {
  std::lock_guard lock(s_mutex);
  _close_evfd_locked();
}

void EventTimer::_watch_fd(int fd, uint32_t events, struct timer_task* task) {
  std::lock_guard lock(s_mutex);
  _watch_fd_locked(fd, events, task);
}

void EventTimer::_unwatch_fd(int fd) {
  std::lock_guard lock(s_mutex);
  _unwatch_fd_locked(fd);
}

void EventTimer::watch_timerfd() {
  SPDLOG_TRACE("+ EventTimer::watch_timerfd()");
  m_task.run = EventTimer::run;
  m_task.data = reinterpret_cast<void*>(this);
  _watch_fd(m_timerfd, EPOLLIN, &m_task);
  SPDLOG_TRACE("- EventTimer::watch_timerfd()");
}

void EventTimer::unwatch_timerfd() const {
  SPDLOG_TRACE("+ EventTimer::unwatch_timerfd()");
  _unwatch_fd(m_timerfd);
  SPDLOG_TRACE("- EventTimer::unwatch_timerfd()");
}

void EventTimer::wait_event() {
  // Snapshot evfd under the lock; the actual epoll_wait runs outside to avoid
  // blocking all other EventTimer operations for the duration of the wait.
  int local_evfd;
  {
    std::lock_guard lock(s_mutex);
    local_evfd = ev_fd;
  }
  if (local_evfd < 0)
    return;

  epoll_event ep[10];
  const auto ready = epoll_wait(local_evfd, ep, std::size(ep), 0);
  for (auto i = 0; i < ready; i++) {
    const auto task = static_cast<struct timer_task*>(ep[i].data.ptr);
    task->run(task, ep[i].events);
  }
}

void EventTimer::set_timerspec(int32_t rate, int32_t delay) {
  SPDLOG_TRACE("+ EventTimer::set_timerspec()");

  if (rate == 0)
    return;

  const int32_t repeat_rate_sec = rate / 1000;
  rate -= (repeat_rate_sec * 1000);
  const int32_t repeat_rate_nsec = rate * 1000 * 1000;

  const int32_t repeat_delay_sec = delay / 1000;
  delay -= (repeat_delay_sec * 1000);
  const int32_t repeat_delay_nsec = delay * 1000 * 1000;

  m_timerspec.it_interval.tv_sec = repeat_rate_sec;
  m_timerspec.it_interval.tv_nsec = repeat_rate_nsec;
  m_timerspec.it_value.tv_sec = repeat_delay_sec;
  m_timerspec.it_value.tv_nsec = repeat_delay_nsec;

  SPDLOG_TRACE("- EventTimer::set_timerspec()");
}

void EventTimer::_arm(const int fd, itimerspec const* timerspec) {
  if (timerfd_settime(fd, 0, timerspec, nullptr) < 0) {
    const auto msg = fmt::format("EventTimer::_arm: timerfd_settime failed: {}",
                                 strerror(errno));
    // throw instead of exit(): lets the caller (arm/disarm) propagate the
    // failure up the stack rather than terminating without running destructors.
    throw std::runtime_error(msg);
  }
}

void EventTimer::arm() const {
  SPDLOG_TRACE("+ EventTimer::arm()");

  _arm(m_timerfd, &m_timerspec);

  SPDLOG_TRACE("- EventTimer::arm()");
}

void EventTimer::disarm() const {
  SPDLOG_TRACE("+ EventTimer::disarm()");

  constexpr itimerspec timerspec{};
  _arm(m_timerfd, &timerspec);

  SPDLOG_TRACE("- EventTimer::disarm()");
}

void EventTimer::run(timer_task const* task, uint32_t events) {
  uint64_t event;
  const auto timer = static_cast<EventTimer*>(task->data);

  if (events != EPOLLIN)
    spdlog::critical("Found Unexpected timerfd events: 0x{:x}", events);

  if (!(events & EPOLLIN))
    return;

  if (read(timer->m_timerfd, &event, sizeof(event)) != sizeof(event)) {
    if (errno != EAGAIN)
      spdlog::critical("Failed to read timer. {}", strerror(errno));
    return;
  }

  timer->m_callback(timer->m_callback_data);
}
