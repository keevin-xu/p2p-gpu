// Emscripten entry point. Thin wrapper over worker-core.
// Task loop runs off the main thread (Phase 1 step 1.24) — background tabs
// throttle the main thread and rAF stops firing.
//
// Steps 0.6/0.9: the browser twin of worker-native. Same seam, same
// kernel_host.cpp, same smoke.cpp, same WGSL bytes — the ONLY difference is
// which platform/ translation unit got linked. That is the dual-target thesis
// (R2), and if this file ever needs an #ifdef, the thesis is in trouble.
//
// The verification logic is deliberately NOT here. It lives in worker-core so
// both targets check results with identical code; otherwise step 0.9 would only
// prove that two hand-written verifiers agree.
//
// ── R7 ───────────────────────────────────────────────────────────────────
// NO GPU WORK HAPPENS AT STARTUP. main() only logs readiness; the run begins
// when the user clicks. Explicit opt-in is a hard rule from day one, not polish
// (Coinhive, RESEARCH.md §4). The rest of the R7 surface — contributing
// indicator, throttle, instant stop — lands in step 1.23.

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <emscripten/emscripten.h>

#include "p2pgpu/worker/platform.hpp"
#include "p2pgpu/worker/smoke.hpp"

namespace {

std::string ReadFile(const std::string& path) {
    // The WGSL is embedded into the module by --embed-file at build time, so
    // ordinary file I/O works and the browser sees the SAME BYTES the native
    // worker reads from disk — which is what makes step 0.9's comparison
    // meaningful. Step 1.12 replaces this with an HTTP fetch by kernel id.
    std::ifstream f(path);
    if (!f) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string g_report;   // outlives the call; ui.js reads it via the pointer below

}  // namespace

// Called from web/ui.js on the R7 opt-in click. Not called by main().
extern "C" EMSCRIPTEN_KEEPALIVE int p2pgpu_run_smoke_test() {
    namespace platform = p2pgpu::worker::platform;

    const std::string double_src = ReadFile("/kernels/smoke_double.wgsl");
    const std::string hash_src   = ReadFile("/kernels/smoke_hash.wgsl");
    if (double_src.empty() || hash_src.empty()) {
        platform::Log("error", "could not read embedded kernels");
        return 1;
    }

    platform::GpuContext ctx;
    if (!platform::AcquireDevice(ctx)) {
        platform::Log("error", "no WebGPU device available; declining to run");
        return 1;
    }

    const platform::AdapterDescription info = platform::DescribeAdapter(ctx);
    platform::Log("info", "target  : browser");
    platform::Log("info", "adapter : " + info.vendor + " / " + info.architecture +
                              " / " + info.device);
    platform::Log("info", "backend : " + info.backend);

    const std::vector<p2pgpu::worker::KernelSource> kernels{
        {"smoke_double", double_src},
        {"smoke_hash", hash_src},
    };
    const auto report = p2pgpu::worker::RunSmokeSuite(ctx, kernels);
    platform::ReleaseDevice(ctx);

    g_report = "target  : browser\nadapter : " + info.vendor + " / " +
               info.architecture + " / " + info.device + "\nbackend : " +
               info.backend + "\n" + report.report;

    platform::Log("info", report.passed ? "PASS" : "FAIL");
    return report.passed ? 0 : 1;
}

/// The canonical report text, for ui.js to POST back for cross-target diffing.
extern "C" EMSCRIPTEN_KEEPALIVE const char* p2pgpu_report() {
    return g_report.c_str();
}

int main() {
    // Deliberately does NO GPU work — that is the whole point of R7. The
    // runtime stays alive after main returns (-sEXIT_RUNTIME=0), and the
    // module factory's promise resolving is what tells the page we are ready.
    p2pgpu::worker::platform::Log("info", "worker ready — waiting for opt-in click");
    return 0;
}
