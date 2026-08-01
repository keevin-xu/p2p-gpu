// PLATFORM SEAM (native). Clock, Yield, and Log — step 1.17.
//
// Split from gpu_native.cpp because these have nothing to do with the GPU and
// everything to do with the host's execution model, which is the OTHER thing
// that genuinely differs between the two targets (platform.hpp). The browser
// twin is timer_wasm.cpp; both satisfy the same declarations and nothing else
// in worker-core knows which it linked.

#if !defined(__EMSCRIPTEN__)

#include "p2pgpu/worker/platform.hpp"

#include <cstdio>

namespace p2pgpu::worker::platform {

void Yield() {
    // Native has no event loop to return to, and dispatch pacing is the
    // caller's business. Deliberately NOT a sleep — that would inflate the
    // idle_ms/gpu_ms split that EVALUATION.md E2 depends on.
}

std::chrono::steady_clock::time_point Now() {
    return std::chrono::steady_clock::now();
}

void Log(std::string_view level, std::string_view message) {
    // stderr rather than spdlog on purpose: worker-core is linked into the WASM
    // target too, and adding a native-only logging dependency to a portable
    // library is how the two targets start to drift (R2). The native binary
    // routes this to spdlog at its own top level.
    std::fprintf(stderr, "[%.*s] %.*s\n", static_cast<int>(level.size()), level.data(),
                 static_cast<int>(message.size()), message.data());
}

}  // namespace p2pgpu::worker::platform

#endif  // !__EMSCRIPTEN__
