// Copyright 2020 Toyota Connected North America
// @copyright Copyright (c) 2022 Woven Alpha, Inc.
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

#include <filesystem>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <stdexcept>

#include "config/common.h"
#include "engine.h"
#include "hexdump.h"
#include "utils.h"

extern void EngineOnFlutterPlatformMessage(
    const FlutterPlatformMessage* engine_message,
    void* user_data);

Engine::Engine(FlutterView* view,
               const size_t index,
               const std::vector<const char*>& vm_args_c,
               const std::string& bundle_path,
               const int32_t accessibility_features)
    : m_index(index),
      m_running(false),
      m_backend(view->GetBackend()),
      m_egl_window(view->GetWindow()),
      m_view(view),
      m_cache_path(GetFilePath(index)),
      m_prev_height(0),
      m_prev_width(0),
      m_prev_pixel_ratio(1.0),
      m_accessibility_features(accessibility_features),
      m_flutter_engine(nullptr) {
  SPDLOG_TRACE("({}) +Engine::Engine", m_index);

  m_args.struct_size = sizeof(FlutterProjectArgs);
  m_args.command_line_argc = static_cast<int>(vm_args_c.size());
  m_args.command_line_argv = vm_args_c.data();
  m_args.dart_entrypoint_argc = static_cast<int>(vm_args_c.size());
  m_args.dart_entrypoint_argv = vm_args_c.data();
  m_args.platform_message_callback = OnFlutterPlatformMessage;
  m_args.persistent_cache_path = m_cache_path.c_str();
  m_args.is_persistent_cache_read_only = false;
  m_args.log_message_callback = onLogMessageCallback;
  m_args.update_semantics_callback2 = onSemanticsUpdateCallback;
  m_args.log_tag = "flutter";

  /// Task Runner
  m_platform_task_runner =
      std::make_shared<TaskRunner>("Platform", m_flutter_engine);

  // Pre-allocate pointer event buffer.  The constructor runs single-threaded
  // before Run() is called, so no lock is required here.
  m_pointer_events.reserve(kMaxPointerEvent);

  ///
  /// libflutter_engine.so loading
  ///
  if (bundle_path.empty()) {
    // throw instead of exit(): exit() is called from the Engine constructor,
    // bypassing destructors for all already-constructed members.
    throw std::invalid_argument(
        "Engine: bundle_path must not be empty — use --b= to specify it");
  }

  // override path
  std::filesystem::path engine_file_path(bundle_path);
  engine_file_path /= kBundleEngine;
  if (std::filesystem::exists(engine_file_path)) {
    SPDLOG_DEBUG("({}) libflutter_engine.so: {}", m_index,
                 engine_file_path.c_str());
  } else {
    engine_file_path = kSystemEngine;
  }

  if (!LibFlutterEngine::IsPresent(engine_file_path.c_str())) {
    const auto dl_err = dlerror();
    const auto msg = fmt::format(
        "({}) Engine: libflutter_engine.so not found "
        "at {}: {}",
        m_index, engine_file_path.string(), dl_err ? dl_err : "unknown error");
    throw std::runtime_error(msg);
  }

  ///
  /// flutter_assets folder
  ///

  m_assets_path = bundle_path;
  m_assets_path /= kBundleFlutterAssets;
  SPDLOG_DEBUG("({}) flutter_assets: {}", m_index, m_assets_path.c_str());
  m_args.assets_path = m_assets_path.c_str();

  ///
  /// icudtl.dat file
  ///
  m_icu_data_path = bundle_path;
  m_icu_data_path /= kBundleIcudtl;
  if (!exists(m_icu_data_path)) {
    m_icu_data_path = kPathPrefix;
    m_icu_data_path /= kSystemIcudtl;
  }
  if (!exists(m_icu_data_path)) {
    const auto msg = fmt::format(
        "({}) icudtl.dat not found at {}; "
        "cannot initialise ICU",
        m_index, m_icu_data_path.string());
    spdlog::critical(msg);
    // throw instead of assert(false): assert is stripped in -DNDEBUG builds,
    // allowing the constructor to continue with an invalid icu_data_path and
    // crash opaquely inside Run().  The exception unwinds the stack cleanly
    // in all build configurations.
    throw std::logic_error(msg);
  }
  SPDLOG_DEBUG("({}) icudtl.dat: {}", m_index, m_icu_data_path.c_str());
  m_args.icu_data_path = m_icu_data_path.c_str();

  ///
  /// AOT loading
  ///
  if (LibFlutterEngine->RunsAOTCompiledDartCode()) {
    m_args.aot_data = nullptr;
    m_aot_data = LoadAotData(bundle_path);
    if (m_aot_data) {
      m_args.aot_data = m_aot_data;
    }
  } else {
    spdlog::info("({}) Runtime=debug", m_index);
    std::filesystem::path kernel_snapshot = m_assets_path;
    kernel_snapshot /= "kernel_blob.bin";
    if (!exists(kernel_snapshot)) {
      // \0 removed from format string — it truncated the message in fmt v10.
      const auto msg = fmt::format("({}) Engine: missing Flutter kernel: {}",
                                   m_index, kernel_snapshot.string());
      spdlog::critical(msg);
      throw std::runtime_error(msg);
    }
  }

  /// Configure task runner interop
  m_platform_task_runner_description = {};
  m_platform_task_runner_description.struct_size =
      sizeof(FlutterTaskRunnerDescription);
  m_platform_task_runner_description.user_data = this;
  m_platform_task_runner_description.runs_task_on_current_thread_callback =
      [](void* context) -> bool {
    const auto engine = static_cast<Engine*>(context);
    return engine->m_platform_task_runner->IsThreadEqual(pthread_self());
  };
  m_platform_task_runner_description.post_task_callback =
      [](const FlutterTask task, const uint64_t target_time,
         void* context) -> void {
    const auto engine = static_cast<Engine*>(context);
    engine->m_platform_task_runner->QueueFlutterTask(
        engine->m_index, target_time, task, context);
  };

  m_custom_task_runners = {};
  m_custom_task_runners.struct_size = sizeof(FlutterCustomTaskRunners);
  m_custom_task_runners.platform_task_runner =
      &m_platform_task_runner_description;

  m_args.custom_task_runners = &m_custom_task_runners;

  SPDLOG_TRACE("({}) -Engine::Engine", m_index);
}

Engine::~Engine() {
  if (m_running) {
    LibFlutterEngine->Deinitialize(m_flutter_engine);
    LibFlutterEngine->Shutdown(m_flutter_engine);
    if (m_aot_data) {
      if (LibFlutterEngine->CollectAOTData) {
        LibFlutterEngine->CollectAOTData(m_aot_data);
      } else {
        spdlog::error(
            "({}) CollectAOTData function pointer is null — "
            "AOT data will not be released",
            m_index);
      }
    }
  }
  m_platform_task_runner.reset();
}

FlutterEngineResult Engine::Shutdown() const {
  if (!m_flutter_engine) {
    return kSuccess;
  }
  return LibFlutterEngine->Shutdown(m_flutter_engine);
}

bool Engine::IsRunning() const {
  return m_running;
}

FlutterEngineResult Engine::Run(FlutterDesktopEngineState* state) {
  SPDLOG_TRACE("({}) +Engine::Run", m_index);

  const auto config = m_backend->GetRenderConfig();
  FlutterEngineResult result = LibFlutterEngine->Initialize(
      FLUTTER_ENGINE_VERSION, &config, &m_args, state, &m_flutter_engine);
  if (result != kSuccess) {
    spdlog::error("({}) FlutterEngineRun failed or engine is null", m_index);
    return result;
  }

  result = LibFlutterEngine->RunInitialized(m_flutter_engine);
  if (result == kSuccess) {
    m_running = true;
    SPDLOG_DEBUG("({}) Engine::m_running = {}", m_index, m_running.load());
  }

  // Set available system locales
  SetUpLocales();

  // Set Accessibility Features
  LibFlutterEngine->UpdateAccessibilityFeatures(
      m_flutter_engine,
      static_cast<FlutterAccessibilityFeature>(m_accessibility_features));

  // Enable Semantics
  LibFlutterEngine->UpdateSemanticsEnabled(m_flutter_engine, true);

  SPDLOG_TRACE("({}) -Engine::Run", m_index);
  return result;
}

FlutterEngineResult Engine::SetWindowSize(const size_t height,
                                          const size_t width) {
  if (!m_running) {
    return kInternalInconsistency;
  }

  // Build the event using the candidate values; do NOT update cached state
  // yet — we only commit the new dimensions once the engine has accepted them.
  const FlutterWindowMetricsEvent fwme = {
      .struct_size = sizeof(FlutterWindowMetricsEvent),
      .width = width,
      .height = height,
      .pixel_ratio = m_prev_pixel_ratio,
      .left = 0,
      .top = 0,
      .physical_view_inset_top = 0,
      .physical_view_inset_right = 0,
      .physical_view_inset_bottom = 0,
      .physical_view_inset_left = 0,
      .display_id = 0,  // TODO display index
      .view_id = static_cast<int64_t>(m_index)};

  const auto result =
      LibFlutterEngine->SendWindowMetricsEvent(m_flutter_engine, &fwme);
  if (result != kSuccess) {
    spdlog::critical("({}) Failed to send window size to flutter ({}x{})",
                     m_index, width, height);
    return result;
  }

  // Commit cached state only after a confirmed successful send.
  m_prev_height = height;
  m_prev_width = width;

  return kSuccess;
}

FlutterEngineResult Engine::SetPixelRatio(double pixel_ratio) {
  if (!m_running) {
    return kInternalInconsistency;
  }

  // m_prev_width / m_prev_height must have been set by a prior successful
  // SetWindowSize call.  Treat zeroes as an explicit programming error rather
  // than silently sending a degenerate metrics event.
  if (m_prev_width == 0 || m_prev_height == 0) {
    spdlog::error(
        "({}) SetPixelRatio called before window size was established "
        "(width={}, height={})",
        m_index, m_prev_width, m_prev_height);
    return kInternalInconsistency;
  }

  // Build the event with the candidate pixel ratio; do NOT commit it to cached
  // state until the engine has accepted the event.
  const FlutterWindowMetricsEvent fwme = {
      .struct_size = sizeof(FlutterWindowMetricsEvent),
      .width = m_prev_width,
      .height = m_prev_height,
      .pixel_ratio = pixel_ratio,
      .left = 0,
      .top = 0,
      .physical_view_inset_top = 0,
      .physical_view_inset_right = 0,
      .physical_view_inset_bottom = 0,
      .physical_view_inset_left = 0,
      .display_id = 0,  // TODO display index
      .view_id = static_cast<int64_t>(m_index)};

  const auto result =
      LibFlutterEngine->SendWindowMetricsEvent(m_flutter_engine, &fwme);
  if (result != kSuccess) {
    spdlog::critical("({}) Failed to send pixel ratio to flutter (ratio={})",
                     m_index, pixel_ratio);
    return result;
  }

  // Commit cached state only after a confirmed successful send.
  m_prev_pixel_ratio = pixel_ratio;

  SPDLOG_TRACE("({}) SetPixelRatio: width={}, height={}, pixel_ratio={}",
               m_index, m_prev_width, m_prev_height, pixel_ratio);
  return kSuccess;
}

std::string Engine::GetFilePath(size_t index) {
  auto path = Utils::GetConfigHomePath();

  if (!std::filesystem::is_directory(path) || !std::filesystem::exists(path)) {
    if (!std::filesystem::create_directories(path)) {
      const auto msg = fmt::format(
          "({}) Engine::GetFilePath: "
          "create_directories failed: {}",
          index, path);
      spdlog::critical(msg);
      throw std::runtime_error(msg);
    }
  }

  SPDLOG_DEBUG("({}) PersistentCachePath: {}", index, path);

  return path;
}

FlutterEngineResult Engine::SendPlatformMessageResponse(
    const FlutterPlatformMessageResponseHandle* handle,
    const uint8_t* data,
    size_t data_length) const {
  if (!m_running) {
    return kInternalInconsistency;
  }

  if (!m_platform_task_runner->IsThreadEqual(pthread_self())) {
    spdlog::error("Not sending message on Platform Thread");
  }

  return LibFlutterEngine->SendPlatformMessageResponse(m_flutter_engine, handle,
                                                       data, data_length);
}

bool Engine::SendPlatformMessage(
    const char* channel,
    std::unique_ptr<std::vector<uint8_t>> message,
    const FlutterPlatformMessageResponseHandle* response_handle) const {
  if (!m_running) {
    return false;
  }

  FlutterEngineResult result;
  if (!m_platform_task_runner->IsThreadEqual(pthread_self())) {
    auto f = m_platform_task_runner->QueuePlatformMessage(channel,
                                                          std::move(message));
    result = f.get();
  } else {
    const FlutterPlatformMessage msg{sizeof(FlutterPlatformMessage), channel,
                                     message->data(), message->size(),
                                     response_handle};
    result = LibFlutterEngine->SendPlatformMessage(m_flutter_engine, &msg);
  }

  return (result == kSuccess);
}

bool Engine::SendPlatformMessage(const char* channel,
                                 const uint8_t* message,
                                 const size_t message_size) const {
  if (!m_running) {
    return false;
  }

  FlutterEngineResult result;
  if (!m_platform_task_runner->IsThreadEqual(pthread_self())) {
    auto msg =
        std::make_unique<std::vector<uint8_t>>(message, message + message_size);
    auto f =
        m_platform_task_runner->QueuePlatformMessage(channel, std::move(msg));
    result = f.get();
  } else {
    const FlutterPlatformMessage msg{sizeof(FlutterPlatformMessage), channel,
                                     message, message_size, nullptr};
    result = LibFlutterEngine->SendPlatformMessage(m_flutter_engine, &msg);
  }
  return (result == kSuccess);
}

bool Engine::SendPlatformMessage(const char* channel,
                                 const uint8_t* message,
                                 const size_t message_size,
                                 const FlutterDataCallback reply,
                                 void* userdata) const {
  if (!m_running) {
    return false;
  }

  // Create the response handle.  The Flutter engine takes ownership of the
  // handle the moment SendPlatformMessage succeeds, so we must NOT call
  // PlatformMessageReleaseResponseHandle after a successful send — doing so
  // would be a double-release.  We only release it ourselves on the error
  // paths where to send never happened.
  FlutterPlatformMessageResponseHandle* handle = nullptr;
  const FlutterEngineResult create_result =
      LibFlutterEngine->PlatformMessageCreateResponseHandle(
          m_flutter_engine, reply, userdata, &handle);
  if (create_result != kSuccess || handle == nullptr) {
    spdlog::error("({}) Failed to create platform message response handle",
                  m_index);
    return false;
  }

  FlutterEngineResult result;
  if (!m_platform_task_runner->IsThreadEqual(pthread_self())) {
    auto msg =
        std::make_unique<std::vector<uint8_t>>(message, message + message_size);
    // handle ownership is transferred into QueuePlatformMessage / the engine.
    // Do NOT release handle after this point.
    auto f = m_platform_task_runner->QueuePlatformMessage(
        channel, std::move(msg), handle);
    result = f.get();
  } else {
    const FlutterPlatformMessage msg{
        sizeof(FlutterPlatformMessage), channel, message, message_size, handle,
    };
    // handle ownership is transferred to the engine on success.
    // Do NOT release handle after this point.
    result = LibFlutterEngine->SendPlatformMessage(m_flutter_engine, &msg);
  }

  // NOTE: PlatformMessageReleaseResponseHandle is intentionally NOT called
  // here.  Once SendPlatformMessage has been invoked the engine owns the
  // handle and will release it when the reply callback fires or the engine
  // shuts down.  Releasing it here would be a double-free.

  return result == kSuccess;
}

[[maybe_unused]] FlutterEngineResult Engine::UpdateAccessibilityFeatures(
    int32_t value) {
  m_accessibility_features = value;
  return LibFlutterEngine->UpdateAccessibilityFeatures(
      m_flutter_engine, static_cast<FlutterAccessibilityFeature>(value));
}

// Passes locale information to the Flutter engine.
void Engine::SetUpLocales() const {
  // static constexpr gives the struct static storage duration so that &locale
  // remains valid indefinitely.  A plain constexpr local has automatic storage
  // duration (stack); its address would become dangling the moment SetUpLocales
  // returns, which is a problem when QueueUpdateLocales posts the call to the
  // task-runner strand — the task may execute after this stack frame is gone.
  // All members point to constexpr char[] constants that already have static
  // storage duration, so this struct is safe to treat as permanently resident.
  static constexpr FlutterLocale kLocale = {
      .struct_size = sizeof(FlutterLocale),
      .language_code = kDefaultLocaleLanguageCode,
      .country_code = kDefaultLocaleCountryCode,
      .script_code = kDefaultLocaleScriptCode,
      .variant_code = nullptr};

  // The vector holds a pointer; kLocale's static lifetime guarantees the
  // pointer remains valid when the strand task eventually dereferences it.
  std::vector<const FlutterLocale*> flutter_locale_list;
  flutter_locale_list.push_back(&kLocale);

  FlutterEngineResult result;
  if (!m_platform_task_runner->IsThreadEqual(pthread_self())) {
    auto f = m_platform_task_runner->QueueUpdateLocales(
        std::move(flutter_locale_list));
    // f.get() already blocks until the task completes; f.wait() beforehand
    // is redundant and was removed.
    result = f.get();
  } else {
    result = LibFlutterEngine->UpdateLocales(m_flutter_engine,
                                             flutter_locale_list.data(),
                                             flutter_locale_list.size());
  }

  if (result != kSuccess) {
    spdlog::error("({}) Failed to set up Flutter locales.", m_index);
  }
}

void Engine::CoalesceMouseEvent(const FlutterPointerSignalKind signal,
                                const FlutterPointerPhase phase,
                                const double x,
                                const double y,
                                const double scroll_delta_x,
                                const double scroll_delta_y,
                                const int64_t buttons) {
  auto timestamp = LibFlutterEngine->GetCurrentTime() / 1000;
  std::scoped_lock lock(m_pointer_mutex);

  FlutterPointerEvent e{};
  e.struct_size = sizeof(FlutterPointerEvent);
  e.phase = phase;
#if ENV64BIT
  e.timestamp = timestamp;
#elif ENV32BIT
  e.timestamp = static_cast<size_t>(timestamp & 0xFFFFFFFFULL);
#endif
  e.x = x;
  e.y = y;
  e.device = 0;
  e.signal_kind = signal;
  e.scroll_delta_x = scroll_delta_x;
  e.scroll_delta_y = scroll_delta_y;
  e.device_kind = kFlutterPointerDeviceKindMouse;
  e.buttons = buttons;
  e.pan_x = 0;
  e.pan_y = 0;
  e.scale = 0;
  e.rotation = 0;

  m_pointer_events.emplace_back(e);
}

void Engine::CoalesceTouchEvent(const FlutterPointerPhase phase,
                                const double x,
                                const double y,
                                const int32_t device) {
  auto timestamp = LibFlutterEngine->GetCurrentTime() / 1000;
  std::scoped_lock lock(m_pointer_mutex);

  FlutterPointerEvent e{};
  e.struct_size = sizeof(FlutterPointerEvent);
  e.phase = phase;
#if ENV64BIT
  e.timestamp = timestamp;
#elif ENV32BIT
  e.timestamp = static_cast<size_t>(timestamp & 0xFFFFFFFFULL);
#endif
  e.x = x;
  e.y = y;
  e.device = device;
  e.signal_kind = kFlutterPointerSignalKindNone;
  e.scroll_delta_x = 0.0;
  e.scroll_delta_y = 0.0;
  e.device_kind = kFlutterPointerDeviceKindTouch;
  e.buttons = 0;
  e.pan_x = 0;
  e.pan_y = 0;
  e.scale = 0;
  e.rotation = 0;

  m_pointer_events.emplace_back(e);
}

void Engine::SendPointerEvents() {
  // Acquire the lock first so that the emptiness check and the drain are a
  // single atomic operation with respect to CoalesceMouseEvent /
  // CoalesceTouchEvent.  Checking empty() before the lock would create a
  // TOCTOU window: a Coalesce* call could push an event between the check and
  // the lock acquisition, or clear the vector, yielding a data race on the
  // vector internals.
  std::vector<FlutterPointerEvent> events_to_send;
  {
    std::scoped_lock lock(m_pointer_mutex);
    if (m_pointer_events.empty() || !m_flutter_engine) {
      return;
    }
    // Swap out the accumulated events so the mutex is released before the
    // engine call.  This keeps the critical section minimal and avoids holding
    // m_pointer_mutex across a potentially-blocking Flutter engine API call.
    events_to_send.swap(m_pointer_events);
    // Restore pre-allocated capacity so future Coalesce* calls don't
    // reallocate.
    m_pointer_events.reserve(kMaxPointerEvent);
  }

  LibFlutterEngine->SendPointerEvent(m_flutter_engine, events_to_send.data(),
                                     events_to_send.size());
}

FlutterEngineAOTData Engine::LoadAotData(const std::string& bundle_path) const {
  std::filesystem::path aot_data_path(bundle_path);
  aot_data_path /= kBundleAot;
  if (!exists(aot_data_path)) {
    SPDLOG_DEBUG("({}) AOT file not present", m_index);
    return nullptr;
  }

  spdlog::info("({}) Loading AOT: {}", m_index, aot_data_path.c_str());

  FlutterEngineAOTDataSource source = {};
  source.type = kFlutterEngineAOTDataSourceTypeElfPath;
  source.elf_path = aot_data_path.c_str();

  FlutterEngineAOTData data;
  if (!LibFlutterEngine->CreateAOTData) {
    spdlog::critical(
        "({}) CreateAOTData function pointer is null — "
        "cannot load AOT data from: {}",
        m_index, aot_data_path.c_str());
    return nullptr;
  }
  if (kSuccess != LibFlutterEngine->CreateAOTData(&source, &data)) {
    spdlog::critical("({}) Failed to load AOT data from: {}", m_index,
                     aot_data_path.c_str());
    return nullptr;
  }
  return data;
}

bool Engine::ActivateSystemCursor(const int32_t device,
                                  const std::string& kind) const {
  return m_egl_window->ActivateSystemCursor(device, kind);
}

void Engine::OnFlutterPlatformMessage(
    const FlutterPlatformMessage* engine_message,
    void* user_data) {
  if (engine_message->struct_size != sizeof(FlutterPlatformMessage)) {
    spdlog::error("Invalid message size received. Expected: {} but received {}",
                  sizeof(FlutterPlatformMessage), engine_message->struct_size);
    return;
  }

  FlutterDesktopEngineState const* engine_state =
      static_cast<FlutterDesktopEngineState*>(user_data);

  auto* view = engine_state->view_controller == nullptr
                   ? nullptr
                   : engine_state->view_controller->view;

#if DEBUG_PLATFORM_MESSAGES
  std::stringstream ss;
  ss << Hexdump(engine_message->message, engine_message->message_size);
  spdlog::debug("Channel: \"{}\"\n{}", engine_message->channel, ss.str());
#endif

  engine_state->message_dispatcher->HandleMessage(
      {.struct_size = sizeof(FlutterDesktopMessage),
       .channel = engine_message->channel,
       .message = engine_message->message,
       .message_size = engine_message->message_size,
       .response_handle = engine_message->response_handle},
      [view] {
        if (view) {
          SPDLOG_TRACE("input_block_cb");
        }
      },
      [view] {
        if (view) {
          SPDLOG_TRACE("input_unblock_cb");
        }
      });
}

void Engine::onLogMessageCallback(const char* tag,
                                  const char* message,
                                  void* /* user_data */) {
  spdlog::info("{}: {}", tag, message);
}

void Engine::onSemanticsUpdateCallback(const FlutterSemanticsUpdate2* update,
                                       void* user_data) {
  FlutterDesktopEngineState const* engine_state =
      static_cast<FlutterDesktopEngineState*>(user_data);

  auto* accessibility_tree = engine_state->accessibility_tree;
  SPDLOG_TRACE(
      "[onSemanticsUpdateCallback] struct_size: {}, node_count: {} "
      "custom_action_count: {}",
      update->struct_size, update->node_count, update->custom_action_count);

  accessibility_tree->HandleFlutterUpdate(update);
}
