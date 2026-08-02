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

#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <emscripten/emscripten.h>

#include "p2pgpu/worker/bench.hpp"
#include "p2pgpu/worker/platform.hpp"
#include "p2pgpu/worker/recovery.hpp"
#include "p2pgpu/worker/smoke.hpp"
#include "p2pgpu/worker/task_loop.hpp"
#include "ui_bridge.hpp"

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

// ── Step 1.23: joining the grid ──────────────────────────────────────────
//
// Deliberately file-scope and deliberately never destroyed. The Emscripten
// runtime outlives main() (-sEXIT_RUNTIME=0) and the page drives us through
// callbacks, so there is no scope that could own these; a local in main() would
// be gone before the first click.
std::unique_ptr<p2pgpu::worker::DeviceSession> g_device;
std::unique_ptr<p2pgpu::worker::TaskLoop> g_loop;

/// Fetch WGSL by kernel id from the coordinator (step 1.12 serves it).
///
/// THE ONLY THING THE BROWSER DOES DIFFERENTLY from the native worker, which
/// reads the same text off disk. Both are two lines, and both live in the thin
/// wrapper rather than in worker-core — that is what keeps an `#ifdef` out of
/// portable code (R2).
///
/// Synchronous, via ASYNCIFY. That is not a blocking call in the browser sense:
/// emscripten unwinds the stack, returns to the event loop, and resumes here
/// when the response lands. Same mechanism `platform::Yield()` already uses, so
/// it costs nothing new.
std::optional<std::string> FetchKernel(const std::string& base_url, std::string_view id) {
    // Kernel ids arrive over the wire. Refuse anything that could escape the
    // path before it reaches a URL — the coordinator is not more trusted than
    // any other peer (R11).
    if (id.empty() || id.find('/') != std::string_view::npos ||
        id.find("..") != std::string_view::npos) {
        return std::nullopt;
    }

    void* data = nullptr;
    int size = 0;
    int error = 0;
    const std::string url = base_url + "/kernel/" + std::string{id};
    emscripten_wget_data(url.c_str(), &data, &size, &error);

    if (error != 0 || data == nullptr || size <= 0) {
        std::free(data);
        return std::nullopt;
    }
    std::string out(static_cast<const char*>(data), static_cast<std::size_t>(size));
    // emscripten_wget_data allocates with malloc and hands ownership over.
    std::free(data);
    return out;
}

/// Turn `ws://host:port/ws` into `http://host:port` for the kernel fetch.
///
/// One coordinator, two protocols: the control plane is a WebSocket, the kernel
/// registry is plain HTTP on the same origin (step 1.12). Deriving one from the
/// other means the page has a single URL field rather than two that can be
/// pointed at different machines.
std::string HttpBaseFromWs(std::string_view ws_url) {
    std::string s{ws_url};
    if (s.rfind("wss://", 0) == 0) {
        s = "https://" + s.substr(6);
    } else if (s.rfind("ws://", 0) == 0) {
        s = "http://" + s.substr(5);
    }
    // Drop the "/ws" path; the kernel route is at the root.
    const auto slash = s.find('/', s.find("//") + 2);
    if (slash != std::string::npos) {
        s = s.substr(0, slash);
    }
    return s;
}

/// Set by p2pgpu_stop to break the loop below.
bool g_running = false;

/// Push the loop's state to the DOM. Called once per iteration.
void PublishStatus() {
    if (!g_loop) {
        return;
    }
    const auto& st = g_loop->status();
    // R7: the indicator reflects the loop's own record of whether it is
    // executing, every frame. Not a guess by the page, and not sticky.
    p2pgpu::worker::ui::SetContributing(st.contributing);
    p2pgpu::worker::ui::SetConnected(st.connected);
    p2pgpu::worker::ui::SetCounters(static_cast<int>(st.tasks_completed),
                                    static_cast<int>(st.tasks_failed),
                                    static_cast<int>(st.device_recoveries));
    p2pgpu::worker::ui::SetStatus(st.last_message.c_str());
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// THE R7 SURFACE — the only three things a user can do (step 1.23).
//
// Every one of these is reachable ONLY from a click or a slider in
// web/ui.js. Nothing below is called by main(), and main() does no GPU work at
// all. That is the rule, and it is why the Coinhive failure mode (RESEARCH.md
// §4) is structurally impossible here rather than merely avoided.
// ─────────────────────────────────────────────────────────────────────────

/// Affirmative opt-in. The click that starts everything.
extern "C" EMSCRIPTEN_KEEPALIVE int p2pgpu_start(const char* coordinator_url) {
    namespace worker = p2pgpu::worker;

    if (g_loop) {
        return 0;  // already running; the button is disabled but be safe
    }

    g_device = std::make_unique<worker::DeviceSession>();
    if (!g_device->Start()) {
        // Not a crash — no WebGPU is a CAPABILITY we report, and the page must
        // say so plainly rather than appearing to work (docs/RISKS.md §1).
        worker::ui::SetStatus("no WebGPU device available on this browser");
        g_device.reset();
        return 1;
    }

    const std::string url = coordinator_url != nullptr ? coordinator_url : "";
    const std::string http_base = HttpBaseFromWs(url);

    worker::TaskLoopConfig cfg;
    cfg.coordinator_url = url;

    g_loop = std::make_unique<worker::TaskLoop>(
        cfg, *g_device,
        [http_base](std::string_view id) { return FetchKernel(http_base, id); });
    g_loop->Start();
    worker::ui::SetRunning(true);

    // ── THE LOOP ─────────────────────────────────────────────────────────
    //
    // A plain while + emscripten_sleep, which is EXACTLY the shape of the
    // native worker's loop — same Poll(), same cadence, two hosts. That is the
    // whole reason TaskLoop exposes Poll() rather than a blocking Run().
    //
    // NOT emscripten_set_main_loop, which the first draft used. Its callback
    // would suspend through ASYNCIFY (Poll -> Execute -> RunTask -> WaitUntil),
    // and a main-loop callback that unwinds the stack out from under the
    // main-loop machinery is not something that machinery expects. ASYNCIFY
    // handles a sleeping while-loop natively; that is what it is for.
    //
    // This does NOT block the tab: emscripten_sleep returns control to the
    // browser's event loop and resumes here when the timer fires. The 0.15
    // heartbeat on the page is the objective check that it really does.
    //
    // Step 1.24 moves all of this to a Web Worker, at which point real blocking
    // becomes legal and ASYNCIFY can probably go — see the note in CMakeLists.
    g_running = true;
    while (g_running) {
        g_loop->Poll();
        PublishStatus();
        emscripten_sleep(5);
    }

    // Reached only via p2pgpu_stop, which has already torn everything down.
    return 0;
}

/// INSTANT STOP (R7). Not "stop after the current task".
///
/// It cannot cancel a dispatch already submitted to the GPU — nothing can — but
/// R4's chunking bounds that to one chunk, and nothing further is submitted.
/// That bound is a second thing chunking buys beyond avoiding TDR.
extern "C" EMSCRIPTEN_KEEPALIVE void p2pgpu_stop() {
    namespace worker = p2pgpu::worker;

    // Breaks the while-loop in p2pgpu_start. Setting the flag before the
    // teardown below is deliberate: the loop is suspended inside
    // emscripten_sleep right now, and it must not take another turn against
    // objects we are about to destroy.
    g_running = false;

    if (g_loop) {
        g_loop->Stop();     // releases every held lease before closing (R8)
        g_loop.reset();
    }
    if (g_device) {
        g_device->Stop();
        g_device.reset();
    }
    worker::ui::SetRunning(false);
    worker::ui::SetContributing(false);
    worker::ui::SetConnected(false);
    worker::ui::SetStatus("stopped");
}

/// User throttle, 0.0-1.0 (R7). 0 stops taking new work WITHOUT disconnecting —
/// the user asked to pause, and dropping the socket would look to the
/// coordinator like a worker that vanished.
extern "C" EMSCRIPTEN_KEEPALIVE void p2pgpu_set_throttle(float fraction) {
    if (g_loop) {
        g_loop->SetThrottle(fraction);
    }
}

// Called from web/ui.js on the R7 opt-in click. Not called by main().
extern "C" EMSCRIPTEN_KEEPALIVE int p2pgpu_run_smoke_test() {
    namespace platform = p2pgpu::worker::platform;

    const std::string double_src = ReadFile("/kernels/smoke_double.wgsl");
    const std::string hash_src   = ReadFile("/kernels/smoke_hash.wgsl");
    const std::string probe_src  = ReadFile("/kernels/divergence_probe.wgsl");
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
        // R6 evidence (step 0.16). Dumped, not verified — no CPU reference
        // exists for transcendentals, which is the entire point.
        {"divergence_probe", probe_src},
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
