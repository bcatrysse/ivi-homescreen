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

#include "libflutter_engine.h"

#include <cstdlib>
#include <iostream>

#include <dlfcn.h>

#include "shared_library.h"

LibFlutterEngineExports::LibFlutterEngineExports(void* lib) {
  if (lib != nullptr) {
    ShellGetFuncAddress(lib, "FlutterEngineCreateAOTData", &CreateAOTData);
    ShellGetFuncAddress(lib, "FlutterEngineCollectAOTData", &CollectAOTData);
    ShellGetFuncAddress(lib, "FlutterEngineRun", &Run);
    ShellGetFuncAddress(lib, "FlutterEngineShutdown", &Shutdown);
    ShellGetFuncAddress(lib, "FlutterEngineInitialize", &Initialize);
    ShellGetFuncAddress(lib, "FlutterEngineDeinitialize", &Deinitialize);
    ShellGetFuncAddress(lib, "FlutterEngineRunInitialized", &RunInitialized);
    ShellGetFuncAddress(lib, "FlutterEngineSendWindowMetricsEvent",
                        &SendWindowMetricsEvent);
    ShellGetFuncAddress(lib, "FlutterEngineSendPointerEvent",
                        &SendPointerEvent);
    ShellGetFuncAddress(lib, "FlutterEngineSendKeyEvent", &SendKeyEvent);
    ShellGetFuncAddress(lib, "FlutterEngineSendPlatformMessage",
                        &SendPlatformMessage);
    ShellGetFuncAddress(lib, "FlutterPlatformMessageCreateResponseHandle",
                        &PlatformMessageCreateResponseHandle);
    ShellGetFuncAddress(lib, "FlutterPlatformMessageReleaseResponseHandle",
                        &PlatformMessageReleaseResponseHandle);
    ShellGetFuncAddress(lib, "FlutterEngineSendPlatformMessageResponse",
                        &SendPlatformMessageResponse);
    ShellGetFuncAddress(lib, "FlutterEngineRegisterExternalTexture",
                        &RegisterExternalTexture);
    ShellGetFuncAddress(lib, "FlutterEngineUnregisterExternalTexture",
                        &UnregisterExternalTexture);
    ShellGetFuncAddress(lib, "FlutterEngineMarkExternalTextureFrameAvailable",
                        &MarkExternalTextureFrameAvailable);
    ShellGetFuncAddress(lib, "FlutterEngineUpdateSemanticsEnabled",
                        &UpdateSemanticsEnabled);
    ShellGetFuncAddress(lib, "FlutterEngineUpdateAccessibilityFeatures",
                        &UpdateAccessibilityFeatures);
    ShellGetFuncAddress(lib, "FlutterEngineDispatchSemanticsAction",
                        &DispatchSemanticsAction);
    ShellGetFuncAddress(lib, "FlutterEngineOnVsync", &OnVsync);
    ShellGetFuncAddress(lib, "FlutterEngineReloadSystemFonts",
                        &ReloadSystemFonts);
    ShellGetFuncAddress(lib, "FlutterEngineTraceEventDurationBegin",
                        &TraceEventDurationBegin);
    ShellGetFuncAddress(lib, "FlutterEngineTraceEventDurationEnd",
                        &TraceEventDurationEnd);
    ShellGetFuncAddress(lib, "FlutterEngineTraceEventInstant",
                        &TraceEventInstant);
    ShellGetFuncAddress(lib, "FlutterEnginePostRenderThreadTask",
                        &PostRenderThreadTask);
    ShellGetFuncAddress(lib, "FlutterEngineGetCurrentTime", &GetCurrentTime);
    ShellGetFuncAddress(lib, "FlutterEngineRunTask", &RunTask);
    ShellGetFuncAddress(lib, "FlutterEngineUpdateLocales", &UpdateLocales);
    ShellGetFuncAddress(lib, "FlutterEngineRunsAOTCompiledDartCode",
                        &RunsAOTCompiledDartCode);
    ShellGetFuncAddress(lib, "FlutterEnginePostDartObject", &PostDartObject);
    ShellGetFuncAddress(lib, "FlutterEngineNotifyLowMemoryWarning",
                        &NotifyLowMemoryWarning);
    ShellGetFuncAddress(lib, "FlutterEnginePostCallbackOnAllNativeThreads",
                        &PostCallbackOnAllNativeThreads);
    ShellGetFuncAddress(lib, "FlutterEngineNotifyDisplayUpdate",
                        &NotifyDisplayUpdate);
    ShellGetFuncAddress(lib, "FlutterEngineScheduleFrame", &ScheduleFrame);
    ShellGetFuncAddress(lib, "FlutterEngineSetNextFrameCallback",
                        &SetNextFrameCallback);
  }
}

LibFlutterEngineExports* LibFlutterEngine::operator->() const {
  LibFlutterEngineExports* exports = loadExports(nullptr);
  if (exports == nullptr) {
    // loadExports returns nullptr when libflutter_engine.so could not be
    // opened or when the Initialize symbol is absent (indicating an
    // incompatible or truncated library).  Returning nullptr here would
    // cause a silent null-pointer dereference at whichever of the 30+
    // call sites happens to use operator-> first — producing no actionable
    // diagnostic.  Terminating here, at the single chokepoint, gives a
    // precise error message and a clean abort rather than undefined behavior.
    //
    // std::cerr is used instead of spdlog because the logger may not yet
    // be initialized when this path is hit (e.g. from the TaskRunner or
    // handler_priority_queue before Engine::Engine runs its IsPresent check).
    std::cerr << "[FATAL] libflutter_engine.so could not be loaded or is "
                 "missing the required 'FlutterEngineInitialize' symbol. "
                 "Ensure libflutter_engine.so is present and accessible.\n";
    std::abort();
  }
  return exports;
}

LibFlutterEngineExports* LibFlutterEngine::loadExports(
    const char* library_path = nullptr) {
  static LibFlutterEngineExports exports = [&] {
    void* lib;

    if (ShellGetProcAddress(RTLD_DEFAULT,
                            "Initialize"))  // Search the global scope
                                            // for pre-loaded library.
    {
      lib = RTLD_DEFAULT;
    } else {
      // RTLD_NOW: resolve all symbols in libflutter_engine.so's transitive
      // dependencies immediately at dlopen time.  RTLD_LAZY would defer
      // resolution of those internal relocations until first use, meaning
      // a missing dependency symbol would only surface as a segfault or
      // cryptic PLT error at an arbitrary call site rather than here with a
      // clear dlerror() message.
      //
      // RTLD_LOCAL: keep the library's symbols out of the global namespace to
      // avoid collisions with other loaded libraries.  We resolve every
      // symbol we need explicitly via dlsym / ShellGetFuncAddress, so global
      // visibility is unnecessary.
      lib = dlopen(library_path ? library_path : "libflutter_engine.so",
                   RTLD_NOW | RTLD_LOCAL);
      if (lib == nullptr) {
        // Capture the OS-level failure reason (no such file, missing
        // dependency, permission denied, etc.) before any subsequent call
        // clears dlerror().  std::cerr is used because spdlog may not be
        // initialised at this point (loadExports runs as part of static
        // initialisation via the operator-> / IsPresent call chain).
        std::cerr << "[FATAL] dlopen(libflutter_engine.so) failed: "
                  << dlerror() << "\n";
      }
    }

    return LibFlutterEngineExports(lib);
  }();

  return exports.Initialize ? &exports : nullptr;
}

class LibFlutterEngine LibFlutterEngine;
