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

#include "presentation.h"

#include "common/common.h"

Presentation& Presentation::GetInstance(wl_registry* registry,
                                        const uint32_t name,
                                        const uint32_t version) {
  static Presentation instance(registry, name, version);
  return instance;
}

Presentation::Presentation(wl_registry* registry,
                           const uint32_t name,
                           const uint32_t version)
    : presentation_(nullptr) {
  presentation_ = static_cast<wp_presentation*>(
      wl_registry_bind(registry, name, &wp_presentation_interface, version));

  wp_presentation_add_listener(presentation_, &clkid_listener_, this);
}

Presentation::~Presentation() {
  for (auto& [feedback, _] : feedbacks_) {
    wp_presentation_feedback_destroy(feedback);
  }
  feedbacks_.clear();
  if (presentation_) {
    wp_presentation_destroy(presentation_);
  }
}

void Presentation::clockid_handler(void* data,
                                   wp_presentation* /*wp_presentation */,
                                   uint32_t clk_id) {
  auto* self = static_cast<Presentation*>(data);
  self->clock_id_ = clk_id;
  spdlog::debug("[Presentation] Received clock_id: {}", clk_id);
}

void Presentation::RequestFeedback(wl_surface* surface,
                                   const FeedbackCallback& cb,
                                   void* user_data) {
  auto* feedback = wp_presentation_feedback(presentation_, surface);
  feedbacks_[feedback] = {cb, user_data};
  wp_presentation_feedback_add_listener(feedback, &feedback_listener_, this);
}

void Presentation::feedback_sync_output(
    void* /* data */,
    struct wp_presentation_feedback* /* wp_presentation_feedback */,
    wl_output* /* output */) {}

void Presentation::feedback_presented(
    void* data,
    struct wp_presentation_feedback* wp_presentation_feedback,
    uint32_t tv_sec_hi,
    uint32_t tv_sec_lo,
    uint32_t tv_nsec,
    uint32_t refresh,
    uint32_t seq_hi,
    uint32_t seq_lo,
    uint32_t flags) {
  auto* self = static_cast<Presentation*>(data);
  const uint64_t seq = (static_cast<uint64_t>(seq_hi) << 32) | seq_lo;
  const uint64_t tv_sec = (static_cast<uint64_t>(tv_sec_hi) << 32) | tv_sec_lo;
  if (const auto it = self->feedbacks_.find(wp_presentation_feedback);
      it != self->feedbacks_.end()) {
    if (auto& [callback, user_data] = it->second; callback) {
      callback(user_data, tv_sec, tv_nsec, seq, refresh, flags, false);
    }
    wp_presentation_feedback_destroy(wp_presentation_feedback);
    self->feedbacks_.erase(it);
  }
}

void Presentation::feedback_discarded(
    void* data,
    struct wp_presentation_feedback* wp_presentation_feedback) {
  auto* self = static_cast<Presentation*>(data);
  if (const auto it = self->feedbacks_.find(wp_presentation_feedback);
      it != self->feedbacks_.end()) {
    if (auto& [callback, user_data] = it->second; callback) {
      callback(user_data, 0, 0, 0, 0, 0, true);
    }
    wp_presentation_feedback_destroy(wp_presentation_feedback);
    self->feedbacks_.erase(it);
  }
}
