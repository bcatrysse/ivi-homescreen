/*
 * Copyright 2023 Toyota Connected North America
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

#include <atomic>
#include <future>
#include <memory>
#include <pthread.h>

#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"
#include "asio/io_context_strand.hpp"

#include "flutter/shell/platform/embedder/embedder.h"

#include "config/common.h"
#include "handler_priority_queue.h"

class TaskRunner {
 public:
  explicit TaskRunner(std::string name, FlutterEngine& engine);

  ~TaskRunner();

  static pthread_t GetThreadId() { return pthread_self(); };

  [[nodiscard]] bool IsThreadEqual(const pthread_t threadid) const {
    return pthread_equal(threadid,
                         pthread_self_.load(std::memory_order_acquire)) != 0;
  };

  void QueueFlutterTask(size_t index,
                        uint64_t target_time,
                        FlutterTask task,
                        void* context);

  std::future<FlutterEngineResult> QueuePlatformMessage(
      const char* channel,
      std::unique_ptr<std::vector<uint8_t>> message,
      FlutterPlatformMessageResponseHandle* handle = nullptr) const;

  [[nodiscard]] std::future<FlutterEngineResult> QueueUpdateLocales(
      std::vector<const FlutterLocale*> locales) const;

  std::string GetName() { return name_; }

  [[nodiscard]] asio::io_context::strand* GetStrandContext() const {
    return strand_.get();
  }

 private:
  std::string name_;
  FlutterEngine& engine_;
  // pthread_self_ is written once by the worker thread immediately on startup
  // and read from arbitrary threads via IsThreadEqual.  It must be atomic to
  // avoid a data race, and is value-initialized to 0 so IsThreadEqual always
  // returns a defined (wrong) answer rather than UB before the first store.
  std::atomic<pthread_t> pthread_self_;
  std::thread thread_;
  std::unique_ptr<asio::io_context> io_context_;
  asio::executor_work_guard<decltype(io_context_->get_executor())> work_;
  std::unique_ptr<asio::io_context::strand> strand_;
  std::unique_ptr<handler_priority_queue> pri_queue_;
};
