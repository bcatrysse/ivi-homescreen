/*
 * Copyright 2025 Toyota Connected North America
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

#ifndef SHELL_WAYLAND_PRESENTATION_H_
#define SHELL_WAYLAND_PRESENTATION_H_

#include <functional>
#include <unordered_map>

#include <presentation-time-client-protocol.h>
#include <wayland-client.h>

class Presentation {
 public:
  using FeedbackCallback = std::function<void(void* user_data,
                                              uint64_t tv_sec,
                                              uint32_t tv_nsec,
                                              uint64_t seq,
                                              uint32_t refresh,
                                              uint32_t flags,
                                              bool discarded)>;

  static Presentation& GetInstance(wl_registry* registry = nullptr,
                                   uint32_t name = 0,
                                   uint32_t version = 0);

  void RequestFeedback(wl_surface* surface,
                       const FeedbackCallback& cb,
                       void* user_data);

  // Delete copy/move
  Presentation(const Presentation&) = delete;
  Presentation& operator=(const Presentation&) = delete;
  Presentation(Presentation&&) = delete;
  Presentation& operator=(Presentation&&) = delete;

 private:
  struct FeedbackData {
    FeedbackCallback callback;
    void* user_data;
  };

  Presentation(wl_registry* registry, uint32_t name, uint32_t version);
  ~Presentation();

  wp_presentation* presentation_;

  std::unordered_map<struct wp_presentation_feedback*, FeedbackData> feedbacks_;

  static void feedback_sync_output(void*,
                                   struct wp_presentation_feedback*,
                                   wl_output*);
  static void feedback_presented(void*,
                                 struct wp_presentation_feedback*,
                                 uint32_t,
                                 uint32_t,
                                 uint32_t,
                                 uint32_t,
                                 uint32_t,
                                 uint32_t,
                                 uint32_t);
  static void feedback_discarded(void*, struct wp_presentation_feedback*);
  static constexpr wp_presentation_feedback_listener feedback_listener_{
      .sync_output = feedback_sync_output,
      .presented = feedback_presented,
      .discarded = feedback_discarded,
  };

  uint32_t clock_id_;

  static void clockid_handler(void* data,
                              wp_presentation* wp_presentation,
                              uint32_t clk_id);

  static constexpr wp_presentation_listener clkid_listener_{
      .clock_id = clockid_handler};
};

#endif  // SHELL_WAYLAND_PRESENTATION_H_