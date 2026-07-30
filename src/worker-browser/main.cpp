// Emscripten entry point. Thin wrapper over worker-core.
// Task loop runs off the main thread (Phase 1 step 1.24) — background tabs
// throttle the main thread and rAF stops firing.
//
// Step 0.6: the browser twin of worker-native's smoke test. Same seam, same
// kernel_host.cpp, same WGSL file — the ONLY difference is which platform/
// translation unit got linked. That is the dual-target thesis (R2), and if
// this file ever needs an #ifdef, the thesis is in trouble.
//
// ── R7 ───────────────────────────────────────────────────────────────────
// NO GPU WORK HAPPENS AT STARTUP. main() only registers a callback; the run
// begins when the user clicks. Explicit opt-in is a hard rule from day one,
// not polish (Coinhive, RESEARCH.md §4). The rest of the R7 surface —
// contributing indicator, throttle, instant stop — lands in step 1.23.

#include <cstdio>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include <emscripten/emscripten.h>

#include "p2pgpu/worker/kernel_host.hpp"
#include "p2pgpu/worker/platform.hpp"

namespace {

std::string ReadFile(const std::string& path) {
    // The WGSL is embedded into the module by --embed-file at build time, so
    // ordinary file I/O works. Step 1.12 replaces this with an HTTP fetch of
    // the kernel by id from the coordinator; the *bytes* stay identical either
    // way, which is what keeps step 0.9's cross-target comparison meaningful.
    std::ifstream f(path);
    if (!f) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int RunSmokeTest() {
    namespace platform = p2pgpu::worker::platform;

    const std::string wgsl = ReadFile("/kernels/smoke_double.wgsl");
    if (wgsl.empty()) {
        platform::Log("error", "could not read embedded kernel");
        return 1;
    }

    platform::GpuContext ctx;
    if (!platform::AcquireDevice(ctx)) {
        platform::Log("error", "no WebGPU device available; declining to run");
        return 1;
    }

    const platform::AdapterDescription info = platform::DescribeAdapter(ctx);
    platform::Log("info", "adapter : " + info.vendor + " / " + info.architecture +
                              " / " + info.device);
    platform::Log("info", "backend : " + info.backend);

    // Identical to worker-native: 1000 elements against a 256 workgroup, so
    // the dispatch overhangs and the kernel's bounds check is exercised.
    std::vector<float> input(1000);
    std::iota(input.begin(), input.end(), 1.0F);

    const auto result =
        p2pgpu::worker::RunUnaryF32Kernel(ctx, wgsl, "main", input, 256);
    if (!result) {
        platform::Log("error", "kernel execution failed");
        platform::ReleaseDevice(ctx);
        return 1;
    }

    // THE READBACK ASSERTION. A shader that appears to run but writes nothing
    // is the most common early WebGPU bug and looks exactly like success if
    // you only check return codes.
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if ((*result)[i] != input[i] * 2.0F) {
            if (mismatches < 5) {
                platform::Log("error", "  [" + std::to_string(i) + "] expected " +
                                           std::to_string(input[i] * 2.0F) + " got " +
                                           std::to_string((*result)[i]));
            }
            ++mismatches;
        }
    }

    platform::ReleaseDevice(ctx);

    if (mismatches != 0) {
        platform::Log("error", "FAIL: " + std::to_string(mismatches) + "/" +
                                   std::to_string(input.size()) + " elements wrong");
        return 1;
    }
    platform::Log("info", "PASS: " + std::to_string(input.size()) + "/" +
                              std::to_string(input.size()) +
                              " elements correct (out[i] == in[i] * 2.0)");
    return 0;
}

}  // namespace

// Called from web/ui.js when the user clicks Start (R7). Not called by main().
extern "C" EMSCRIPTEN_KEEPALIVE int p2pgpu_run_smoke_test() {
    return RunSmokeTest();
}

int main() {
    // Deliberately does NO GPU work — that is the whole point of R7. The
    // runtime stays alive after main returns (-sEXIT_RUNTIME=0), and the
    // module factory's promise resolving is what tells the page we are ready.
    p2pgpu::worker::platform::Log("info", "worker ready — waiting for opt-in click");
    return 0;
}
