// PLATFORM SEAM (browser). Clock, Yield, and Log — step 1.17.
//
// The native twin is timer_native.cpp. NEVER busy-wait in here: the browser
// cannot be blocked, and a spin loop freezes the tab rather than slowing it.

#if defined(__EMSCRIPTEN__)

#include "p2pgpu/worker/platform.hpp"

#include <emscripten/console.h>
#include <emscripten/emscripten.h>

#include <string>

namespace p2pgpu::worker::platform {

void Yield() {
    // Return to the event loop between dispatches so the tab stays responsive
    // and the browser's own watchdogs never fire (R4/K1). Requires ASYNCIFY,
    // which the wasm preset enables.
    emscripten_sleep(0);
}

std::chrono::steady_clock::time_point Now() {
    return std::chrono::steady_clock::now();
}

void Log(std::string_view level, std::string_view message) {
    // Same field names as the native sink so correlation IDs line up across
    // targets (docs/CONVENTIONS.md §6).
    const std::string line = "[" + std::string{level} + "] " + std::string{message};
    if (level == "error") {
        emscripten_console_error(line.c_str());
    } else if (level == "warn") {
        emscripten_console_warn(line.c_str());
    } else {
        emscripten_console_log(line.c_str());
    }
}

}  // namespace p2pgpu::worker::platform

#endif  // __EMSCRIPTEN__
