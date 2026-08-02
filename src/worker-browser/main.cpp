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

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>   // emscripten_set_interval (step 1.24)

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

// ── Step 1.24: the loop runs on its own thread ───────────────────────────
//
// Browsers throttle backgrounded tabs hard — timers clamp to ~1 Hz and
// requestAnimationFrame stops entirely. For a volunteer grid, "the user
// switched tabs" is the normal case, so a main-thread loop means most
// volunteers stop contributing while still holding leases. See D-0037.

std::atomic<bool> g_running{false};
std::thread g_thread;

/// THE ONLY THING SHARED BETWEEN THE TWO THREADS.
///
/// The worker thread cannot touch the DOM — `document` does not exist on a
/// pthread — and it must not block on the main thread either, because in a
/// backgrounded tab the main thread is exactly what is throttled. So the loop
/// publishes here, and a MAIN-THREAD interval reads it and draws (D-0037).
///
/// Plain mutex rather than atomics: `message` is a std::string, and the fields
/// must be read as one consistent set or the badge can disagree with the
/// counters. It is contended twice a second over a handful of words.
struct Snapshot {
    std::mutex mutex;
    bool connected = false;
    bool contributing = false;
    std::uint32_t completed = 0;
    std::uint32_t failed = 0;
    std::uint32_t recoveries = 0;
    std::string message = "idle";
};
Snapshot g_snapshot;

/// Worker thread -> snapshot. Never touches the DOM.
void PublishSnapshot() {
    if (!g_loop) {
        return;
    }
    const auto& st = g_loop->status();
    const std::lock_guard<std::mutex> lock(g_snapshot.mutex);
    g_snapshot.connected = st.connected;
    // R7: the indicator reflects the loop's own record of whether it is
    // executing. Not a guess by the page, and not sticky.
    g_snapshot.contributing = st.contributing;
    g_snapshot.completed = st.tasks_completed;
    g_snapshot.failed = st.tasks_failed;
    g_snapshot.recoveries = st.device_recoveries;
    g_snapshot.message = st.last_message;
}

/// Snapshot -> DOM. **Runs on the MAIN thread**, driven by an interval.
///
/// emscripten_set_interval, deliberately NOT requestAnimationFrame: a
/// backgrounded tab stops delivering rAF entirely, and 1.24 forbids depending
/// on it. A throttled interval only makes the READOUT stale — the work carries
/// on regardless, which is the whole point of moving it.
void DrawStatus(void*) {
    bool connected = false;
    bool contributing = false;
    std::uint32_t completed = 0;
    std::uint32_t failed = 0;
    std::uint32_t recoveries = 0;
    std::string message;
    {
        const std::lock_guard<std::mutex> lock(g_snapshot.mutex);
        connected = g_snapshot.connected;
        contributing = g_snapshot.contributing;
        completed = g_snapshot.completed;
        failed = g_snapshot.failed;
        recoveries = g_snapshot.recoveries;
        message = g_snapshot.message;   // copied, so the DOM call sees a stable
    }                                   // buffer even if the loop moves on

    p2pgpu::worker::ui::SetContributing(contributing);
    p2pgpu::worker::ui::SetConnected(connected);
    p2pgpu::worker::ui::SetCounters(static_cast<int>(completed),
                                    static_cast<int>(failed),
                                    static_cast<int>(recoveries));
    p2pgpu::worker::ui::SetStatus(message.c_str());
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

    const std::string url = coordinator_url != nullptr ? coordinator_url : "";
    worker::ui::SetRunning(true);

    // ── THE LOOP, ON ITS OWN THREAD (step 1.24) ──────────────────────────
    //
    // EVERYTHING that touches the device, the socket, or the task loop happens
    // on this thread — acquisition included. Splitting ownership (device here,
    // work there) is how you get objects used from a thread that does not own
    // them, and WebGPU objects are not documented as thread-safe.
    //
    // The body is the same `while { Poll(); sleep }` the native worker runs.
    // One loop shape, two hosts, which is why TaskLoop exposes Poll() rather
    // than a blocking Run().
    //
    // The thread is a REAL Web Worker (-pthread), so a backgrounded tab
    // throttling the main thread does not throttle this. That is the entire
    // point of the step (D-0037).
    g_running = true;
    g_thread = std::thread([url] {
        namespace worker = p2pgpu::worker;

        g_device = std::make_unique<worker::DeviceSession>();
        if (!g_device->Start()) {
            // Not a crash — no WebGPU is a CAPABILITY we report, and the page
            // must say so plainly rather than appearing to work (RISKS.md §1).
            {
                const std::lock_guard<std::mutex> lock(g_snapshot.mutex);
                g_snapshot.message = "no WebGPU device available on this browser";
            }
            g_device.reset();
            g_running = false;
            return;
        }

        const std::string http_base = HttpBaseFromWs(url);
        worker::TaskLoopConfig cfg;
        cfg.coordinator_url = url;

        g_loop = std::make_unique<worker::TaskLoop>(
            cfg, *g_device,
            [http_base](std::string_view id) { return FetchKernel(http_base, id); });
        g_loop->Start();

        while (g_running) {
            g_loop->Poll();
            PublishSnapshot();
            // Still emscripten_sleep rather than std::this_thread::sleep_for:
            // ASYNCIFY stays on for now, and mixing a real blocking sleep into
            // a stack that ASYNCIFY may unwind is not a combination worth
            // discovering the hard way. Removing ASYNCIFY is its own measurable
            // change — D-0037(c).
            emscripten_sleep(5);
        }

        // Torn down HERE, on the owning thread, rather than in p2pgpu_stop.
        // The stop handler runs on the main thread and must not destroy a
        // device this thread may still be inside.
        g_loop->Stop();          // releases every held lease first (R8)
        g_loop.reset();
        g_device->Stop();
        g_device.reset();
        PublishSnapshot();
    });

    return 0;
}

/// INSTANT STOP (R7). Not "stop after the current task".
///
/// It cannot cancel a dispatch already submitted to the GPU — nothing can — but
/// R4's chunking bounds that to one chunk, and nothing further is submitted.
/// That bound is a second thing chunking buys beyond avoiding TDR.
extern "C" EMSCRIPTEN_KEEPALIVE void p2pgpu_stop() {
    namespace worker = p2pgpu::worker;

    // ASKS the loop to stop; it tears itself down on its own thread. This
    // handler runs on the MAIN thread and deliberately destroys nothing —
    // reaching across to free a device the loop thread may be inside is a
    // use-after-free with a plausible-looking call site (D-0037).
    g_running = false;

    if (g_thread.joinable()) {
        // Bounded by one loop iteration (~5 ms) plus, at worst, one in-flight
        // chunk. That the wait is short at all is another thing R4's chunking
        // buys: "instant stop" is a promise to the user, and an unchunked task
        // would make it a lie.
        g_thread.join();
    }

    worker::ui::SetRunning(false);
    worker::ui::SetContributing(false);
    worker::ui::SetConnected(false);
    worker::ui::SetStatus("stopped");
}

/// User throttle, 0.0-1.0 (R7). 0 stops taking new work WITHOUT disconnecting —
/// the user asked to pause, and dropping the socket would look to the
/// coordinator like a worker that vanished.
///
/// Called from the main thread while the loop runs on another. `SetThrottle`
/// writes one float that the loop only reads, so this is a benign race in
/// practice and a real one in the standard — `throttle_` is atomic for that
/// reason, not for ordering.
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

    // The only thing main() sets up: a MAIN-THREAD interval that copies the
    // loop's snapshot into the DOM (step 1.24 / D-0037). It runs from here
    // rather than from the loop thread because `document` does not exist on a
    // pthread, and because a worker that blocked on the main thread to draw
    // would re-acquire the dependency this whole step exists to remove.
    //
    // emscripten_set_interval, NOT requestAnimationFrame — a backgrounded tab
    // stops delivering rAF entirely, and 1.24 forbids depending on it. If the
    // browser throttles this interval, only the READOUT goes stale; the work
    // carries on.
    //
    // 250 ms: fast enough that the R7 contributing indicator tracks reality,
    // slow enough to be free. It is registered before any loop exists and
    // DrawStatus is a no-op until one does.
    emscripten_set_interval(DrawStatus, 250, nullptr);
    return 0;
}
