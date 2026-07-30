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

#include "p2pgpu/worker/bench.hpp"
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

/// Step 0.15, browser half — the chunking spike, and the responsiveness test.
///
/// The native run (D-0020) measured a 0.469 ms fixed cost per chunk. The browser
/// number should be HIGHER: every Yield() here unwinds and rewinds the whole
/// call stack through ASYNCIFY, which has no native equivalent. Measuring that
/// gap is the point.
///
/// The other half of the step is qualitative but not optional: the tab must
/// stay responsive while this runs. ui.js drives a counter on a timer, which
/// can only keep ticking if we are genuinely returning to the event loop
/// between chunks rather than hogging the main thread.
extern "C" EMSCRIPTEN_KEEPALIVE int p2pgpu_run_chunking() {
    namespace platform = p2pgpu::worker::platform;

    const std::string wgsl = ReadFile("/kernels/calibrate.wgsl");
    if (wgsl.empty()) {
        platform::Log("error", "could not read embedded calibrate.wgsl");
        return 1;
    }

    platform::GpuContext ctx;
    if (!platform::AcquireDevice(ctx)) {
        platform::Log("error", "no WebGPU device available");
        return 1;
    }

    // Same total work as the native run so the two are directly comparable.
    constexpr std::uint32_t kInvocations = 1u << 21;
    constexpr std::uint32_t kTotalIterations = 32768;
    const std::vector<std::uint32_t> counts{1, 2, 4, 8, 16, 32, 64};

    const auto samples = p2pgpu::worker::RunChunkingSpike(
        ctx, wgsl, kInvocations, kTotalIterations, counts);
    platform::ReleaseDevice(ctx);

    if (samples.empty()) {
        platform::Log("error", "chunking spike produced no samples");
        return 1;
    }

    std::string csv =
        "# p2pgpu step 0.15 - chunking spike (browser)\n"
        "# total work held CONSTANT: 2097152 invocations x 32768 iterations\n"
        "chunks,iterations_per_chunk,wall_ms,max_chunk_ms,overhead_pct\n";
    for (const auto& s : samples) {
        csv += std::to_string(s.chunks) + "," +
               std::to_string(s.iterations_per_chunk) + "," +
               std::to_string(s.wall_ms) + "," +
               std::to_string(s.max_chunk_ms) + "," +
               std::to_string(s.overhead_pct) + "\n";
        platform::Log("info", "chunks=" + std::to_string(s.chunks) +
                                  " wall_ms=" + std::to_string(s.wall_ms) +
                                  " max_chunk_ms=" + std::to_string(s.max_chunk_ms) +
                                  " overhead_pct=" + std::to_string(s.overhead_pct));
    }

    const auto& base = samples.front();
    const auto& finest = samples.back();
    if (finest.chunks > base.chunks) {
        const double per_chunk_ms = (finest.wall_ms - base.wall_ms) /
                                    static_cast<double>(finest.chunks - base.chunks);
        csv += "# per_chunk_fixed_cost_ms=" + std::to_string(per_chunk_ms) + "\n";
        platform::Log("info", "per_chunk_fixed_cost_ms=" + std::to_string(per_chunk_ms) +
                                  "  (native was 0.469)");
    }

    g_report = csv;
    return 0;
}

/// Step 0.11 in the browser (reached via step 0.13's Safari run).
///
/// The native number is 1,874 GFLOP/s, and the browser measured *faster* than
/// native on the chunking baseline (D-0021) — likely Dawn/Tint generating
/// better Metal than wgpu-native's naga, but unconfirmed. This produces the
/// browser's own GFLOP/s so that question can be settled with a number instead
/// of a hypothesis, and so E7 can separate implementation from environment.
extern "C" EMSCRIPTEN_KEEPALIVE int p2pgpu_run_bench() {
    namespace platform = p2pgpu::worker::platform;

    const std::string wgsl = ReadFile("/kernels/calibrate.wgsl");
    if (wgsl.empty()) {
        platform::Log("error", "could not read embedded calibrate.wgsl");
        return 1;
    }

    platform::GpuContext ctx;
    if (!platform::AcquireDevice(ctx)) {
        platform::Log("error", "no WebGPU device available");
        return 1;
    }

    const platform::AdapterDescription a = platform::DescribeAdapter(ctx);
    const bool has_ts = platform::HasFeature(ctx, WGPUFeatureName_TimestampQuery);

    std::string features;
    for (std::size_t i = 0; i < a.features.size(); ++i) {
        features += (i ? "," : "") + a.features[i];
    }

    // Same sizes and iteration count as the native run, so the two CSVs are
    // directly comparable row for row.
    const std::vector<std::uint32_t> sizes{
        1u << 12, 1u << 14, 1u << 16, 1u << 18, 1u << 20, 1u << 22,
    };
    const auto samples = p2pgpu::worker::RunCalibration(ctx, wgsl, sizes, 2048);
    const double dispatch_us =
        p2pgpu::worker::MeasureDispatchOverheadUs(ctx, wgsl, 1000);
    const double submit_us =
        p2pgpu::worker::MeasureSubmitOverheadUs(ctx, wgsl, 200);
    platform::ReleaseDevice(ctx);

    if (samples.empty()) {
        platform::Log("error", "calibration produced no samples");
        return 1;
    }

    std::string csv = "# p2pgpu step 0.11 - calibrate_v1 throughput (browser)\n";
    csv += "# adapter=" + a.vendor + "/" + a.architecture + "/" + a.device +
           " backend=" + a.backend + "\n";
    csv += "# features=" + features + " timestamp_query=" +
           (has_ts ? "yes" : "no") + "\n";
    csv += "invocations,iterations,dispatches,flops,wall_ms,gflops\n";

    double best = 0.0;
    for (const auto& s : samples) {
        csv += std::to_string(s.invocations) + "," + std::to_string(s.iterations) +
               "," + std::to_string(s.dispatches) + "," + std::to_string(s.flops) +
               "," + std::to_string(s.wall_ms) + "," + std::to_string(s.gflops) + "\n";
        if (s.gflops > best) { best = s.gflops; }
    }
    csv += "# per_dispatch_overhead_us=" + std::to_string(dispatch_us) + "\n";
    csv += "# per_submit_overhead_us=" + std::to_string(submit_us) + "\n";

    platform::Log("info", "adapter : " + a.vendor + " / " + a.architecture +
                              " / " + a.device + "  backend=" + a.backend);
    platform::Log("info", "features: " + features);
    platform::Log("info", "peak GFLOP/s = " + std::to_string(best) +
                              "   (native measured 1874)");
    platform::Log("info", "dispatch_us = " + std::to_string(dispatch_us) +
                              "  submit_us = " + std::to_string(submit_us));

    g_report = csv;
    return 0;
}

int main() {
    // Deliberately does NO GPU work — that is the whole point of R7. The
    // runtime stays alive after main returns (-sEXIT_RUNTIME=0), and the
    // module factory's promise resolving is what tells the page we are ready.
    p2pgpu::worker::platform::Log("info", "worker ready — waiting for opt-in click");
    return 0;
}
