/*
 * Copyright 2020 Toyota Connected North America
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

#pragma once

#include <EGL/egl.h>
#include <memory>

#include "configuration/configuration.h"
#include "view/flutter_view.h"
#include "watchdog.h"

class Display;

class WaylandWindow;

class App final {
 public:
  explicit App(const std::vector<Configuration::Config>& configs);
  ~App();

  App(const App&) = delete;
  const App& operator=(const App&) = delete;

  /**
   * @brief One frame in the loop
   * @return int
   * @retval Number of dispatched events
   * @relation
   * wayland, flutter
   */
  [[nodiscard]] int Loop() const;

#if BUILD_BACKEND_HEADLESS_EGL
  GLubyte* getViewRenderBuf(int i) const;
#endif

 private:
  /**
   * @brief Validates that @p configs is non-empty and returns a const-ref to
   *        configs[0].  Called from the member-initializer list so that
   *        configs[0] is never accessed when the vector is empty.
   * @throws std::invalid_argument if configs is empty.
   */
  static const Configuration::Config& ValidatedFirst(
      const std::vector<Configuration::Config>& configs);

  std::shared_ptr<Display> m_wayland_display;
  std::vector<std::unique_ptr<FlutterView>> m_views;
  std::unique_ptr<Watchdog> m_watch_dog;
};
