// Copyright 2020 Toyota Connected North America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>

#include "config/common.h"

#include "app.h"
#include "configuration/configuration.h"
#include "logging/logging.h"

#if BUILD_CRASH_HANDLER
#include "crash_handler.h"
#endif

std::atomic<bool> running{true};

std::unique_ptr<Logging> gLogger;

/**
 * @brief Signal handler for SIGINT / SIGTERM.
 *
 * POSIX async-signal-safety constraints:
 *   - Only async-signal-safe functions may be called from a signal handler.
 *   - spdlog (mutex, heap, I/O) and exit() (atexit handlers, stdio flush) are
 *     NOT async-signal-safe and must not be called here.
 *   - std::atomic::store with memory_order_relaxed is safe: the atomic
 *     operation itself does not call any library functions.
 *   - The main loop observes the flag on its next iteration and exits
 *     cleanly, running all destructors and flushing the logger there.
 *
 * @param signal  The signal number (unused).
 */
void SignalHandler(int /* signal */) {
  running.store(false, std::memory_order_relaxed);
}

/**
 * @brief Main function
 * @param[in] argc Number of arguments
 * @param[in] argv Arguments passed to the program
 * @return int
 * @retval 0 Normal end
 * @retval Non-zero Abnormal end
 * @relation
 * wayland, flutter
 */
int main(const int argc, char** argv) {
#if BUILD_CRASH_HANDLER
  auto crash_handler = std::make_unique<CrashHandler>();
#endif

  gLogger = std::make_unique<Logging>();

  const auto configs = Configuration::ParseArgcArgv(argc, argv);
  assert(!configs.empty());

  const App app(configs);

  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  // run the application
  int ret = 0;
  while (running.load(std::memory_order_acquire) && ret != -1) {
    ret = app.Loop();
  }

  // Log the shutdown reason here, in the safe main-thread context, rather
  // than from the signal handler where spdlog is not async-signal-safe.
  if (!running.load(std::memory_order_relaxed)) {
    spdlog::info("Signal received — shutting down cleanly");
  }

  gLogger.reset();

#if BUILD_CRASH_HANDLER
  (void)crash_handler.release();
#endif

  return EXIT_SUCCESS;
}
