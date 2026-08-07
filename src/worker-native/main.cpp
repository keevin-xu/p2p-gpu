// Thin main() over worker-core. CLI11 args, then hand off.
//
// If this file grows past ~100 lines, logic is leaking out of worker-core
// and rule R1 is being violated.
//
// Phase 4 step 4.19 packages this so a borrowed machine can join with one
// command (docs/RISKS.md R-D).
//
// Right now it is the native side of steps 0.8/0.9: load the WGSL, run the
// shared smoke suite, write the report. NOTE THE VERIFICATION IS NOT HERE — it
// lives in worker-core/smoke.cpp so the browser runs byte-identical checking
// code. That is what makes step 0.9's cross-target comparison meaningful.

#include <CLI/CLI.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "p2pgpu/worker/bench.hpp"
#include "p2pgpu/worker/kernel_host.hpp"
#include "p2pgpu/worker/recovery.hpp"
#include "p2pgpu/worker/platform.hpp"
#include "p2pgpu/worker/smoke.hpp"
#include "p2pgpu/worker/task_loop.hpp"

namespace {

namespace platform = p2pgpu::worker::platform;

std::string ReadFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string Escape(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); }
        out.push_back(c);
    }
    return out;
}

// ── The real thing: join the grid (step 1.22) ────────────────────────────
//
// Everything below is nine lines of glue over worker-core. That is the point —
// worker-native and worker-browser differ ONLY in how they get WGSL and how
// they drive the loop, and both of those are two lines each. If this function
// starts growing, logic is leaking out of worker-core (R1).
int RunGrid(const std::string& url, const std::string& dir, std::uint64_t chunk) {
    p2pgpu::worker::DeviceSession device;
    if (!device.Start()) {
        std::fprintf(stderr, "no WebGPU device available; declining to join\n");
        return 1;
    }

    p2pgpu::worker::TaskLoopConfig cfg;
    cfg.coordinator_url = url;
    cfg.units_per_chunk = chunk;

    // THE ONLY THING THE NATIVE TARGET DOES DIFFERENTLY: read WGSL from disk.
    // The browser fetches the same text over HTTP from the coordinator (1.12).
    // Keeping acquisition out here rather than inside worker-core is what stops
    // an `#ifdef __EMSCRIPTEN__` appearing in portable code (R2).
    p2pgpu::worker::TaskLoop loop(cfg, device, [dir](std::string_view id) {
        // Kernel ids come off the wire. Refuse anything that could escape the
        // directory before it reaches the filesystem — the coordinator is not
        // more trusted than any other peer (R11), and "../.." is the cheapest
        // possible attack on a worker that reads files by name.
        if (id.empty() || id.find('/') != std::string_view::npos ||
            id.find('\\') != std::string_view::npos ||
            id.find("..") != std::string_view::npos) {
            return std::optional<std::string>{};
        }
        // Manifest ids map to `<id minus version suffix>.wgsl`; brute_search_v1
        // lives in brute_search.wgsl.
        std::string base{id};
        const auto us = base.rfind("_v");
        if (us != std::string::npos) {
            base = base.substr(0, us);
        }
        auto text = ReadFile(dir + "/" + base + ".wgsl");
        if (text.empty()) {
            return std::optional<std::string>{};
        }
        return std::optional<std::string>{std::move(text)};
    });

    loop.Start();

    // Native's event loop. The browser's is emscripten_set_main_loop over the
    // same Poll() — one loop body, two hosts, which is why Poll() exists rather
    // than a blocking Run().
    while (loop.status().connected || loop.status().last_message == "connecting") {
        loop.Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    loop.Stop();
    device.Stop();
    return 0;
}

/// Step 0.7 — adapter capability dump, the first row of the E7 table.
int DumpAdapter(const platform::GpuContext& ctx) {
    const platform::AdapterDescription a = platform::DescribeAdapter(ctx);

    std::string features;
    for (std::size_t i = 0; i < a.features.size(); ++i) {
        features += (i ? ", \"" : "\"") + a.features[i] + "\"";
    }

    std::printf(R"({
  "target": "native",
  "vendor": "%s",
  "architecture": "%s",
  "device": "%s",
  "description": "%s",
  "backend": "%s",
  "is_software": %s,
  "features": [%s],
  "limits": {
    "maxStorageBufferBindingSize": %llu,
    "maxBufferSize": %llu,
    "maxComputeWorkgroupsPerDimension": %u,
    "maxComputeInvocationsPerWorkgroup": %u,
    "maxComputeWorkgroupStorageSize": %u
  }
}
)",
                Escape(a.vendor).c_str(), Escape(a.architecture).c_str(),
                Escape(a.device).c_str(), Escape(a.description).c_str(),
                Escape(a.backend).c_str(), a.is_software ? "true" : "false",
                features.c_str(),
                static_cast<unsigned long long>(a.max_storage_buffer_binding_size),
                static_cast<unsigned long long>(a.max_buffer_size),
                a.max_compute_workgroups_per_dim,
                a.max_compute_invocations_per_workgroup,
                a.max_compute_workgroup_storage_size);
    return 0;
}

/// Steps 0.11 / 0.12 — throughput sweep and per-dispatch overhead, as CSV.
int RunBench(const platform::GpuContext& ctx, const std::string& dir) {
    const std::string wgsl = ReadFile(dir + "/calibrate.wgsl");
    if (wgsl.empty()) {
        std::fprintf(stderr, "could not read calibrate.wgsl\n");
        return 1;
    }

    const platform::AdapterDescription a = platform::DescribeAdapter(ctx);
    const bool has_ts = platform::HasFeature(ctx, WGPUFeatureName_TimestampQuery);

    // CSV header carries the conditions, per CONVENTIONS.md §9 — a chart that
    // cannot be traced back to how it was produced is not evidence.
    std::printf("# p2pgpu step 0.11/0.12 - calibrate_v1 throughput\n");
    std::printf("# device=%s backend=%s vendor=%s arch=%s\n",
                a.device.c_str(), a.backend.c_str(), a.vendor.c_str(),
                a.architecture.c_str());
    std::printf("# timing=wall_clock timestamp_query_available=%s\n",
                has_ts ? "yes" : "no");
    std::printf("# NOTE: build under native-release; sanitizers corrupt timings\n");
    std::printf("invocations,iterations,dispatches,flops,wall_ms,gflops\n");

    const std::vector<std::uint32_t> sizes{
        1u << 12, 1u << 14, 1u << 16, 1u << 18, 1u << 20, 1u << 22,
    };
    const auto samples = p2pgpu::worker::RunCalibration(ctx, wgsl, sizes, 2048);
    for (const auto& s : samples) {
        std::printf("%u,%u,%u,%llu,%.3f,%.1f\n", s.invocations, s.iterations,
                    s.dispatches, static_cast<unsigned long long>(s.flops),
                    s.wall_ms, s.gflops);
    }

    // Two DIFFERENT numbers; conflating them would mis-size every task.
    //   dispatch = marginal cost of one more dispatch in the same command
    //              buffer. Bounds R4 chunking, where a task is many short
    //              dispatches encoded together.
    //   submit   = full encode/submit/wait round trip. Every task pays this at
    //              least once, so it is the real floor on task size and the
    //              figure the Phase 2 sizer needs. Published 24-71 us numbers
    //              are this one.
    const double dispatch_us =
        p2pgpu::worker::MeasureDispatchOverheadUs(ctx, wgsl, 1000);
    const double submit_us =
        p2pgpu::worker::MeasureSubmitOverheadUs(ctx, wgsl, 200);
    std::printf("# per_dispatch_overhead_us=%.2f  (marginal, same command buffer)\n",
                dispatch_us);
    std::printf("# per_submit_overhead_us=%.2f  (full round trip, median)\n",
                submit_us);

    double best = 0.0;
    for (const auto& s : samples) { best = std::max(best, s.gflops); }
    std::fprintf(stderr,
                 "peak %.1f GFLOP/s | dispatch %.2f us | submit %.2f us\n",
                 best, dispatch_us, submit_us);
    return samples.empty() ? 1 : 0;
}

/// Step 0.15 — the chunking spike. Validates rule R4 empirically.
int RunChunking(const platform::GpuContext& ctx, const std::string& dir) {
    const std::string wgsl = ReadFile(dir + "/calibrate.wgsl");
    if (wgsl.empty()) {
        std::fprintf(stderr, "could not read calibrate.wgsl\n");
        return 1;
    }

    const platform::AdapterDescription a = platform::DescribeAdapter(ctx);

    // Sized so the 1-chunk baseline OVERSHOOTS R4's 250 ms ceiling. That is the
    // regime the rule exists for: a task too long to ship as one dispatch, which
    // must be split. Measuring at 30 ms would answer a question nobody asked.
    constexpr std::uint32_t kInvocations = 1u << 21;
    constexpr std::uint32_t kTotalIterations = 32768;

    std::printf("# p2pgpu step 0.15 - chunking spike (rule R4)\n");
    std::printf("# device=%s backend=%s\n", a.device.c_str(), a.backend.c_str());
    std::printf("# total work held CONSTANT: %u invocations x %u iterations\n",
                kInvocations, kTotalIterations);
    std::printf("# each chunk submitted separately with a yield between\n");
    std::printf("# NOTE: build under native-release; sanitizers corrupt timings\n");
    std::printf("chunks,iterations_per_chunk,wall_ms,max_chunk_ms,overhead_pct\n");

    const std::vector<std::uint32_t> counts{1, 2, 4, 8, 16, 32, 64};
    const auto samples = p2pgpu::worker::RunChunkingSpike(
        ctx, wgsl, kInvocations, kTotalIterations, counts);

    for (const auto& s : samples) {
        std::printf("%u,%u,%.3f,%.3f,%+.2f\n", s.chunks, s.iterations_per_chunk,
                    s.wall_ms, s.max_chunk_ms, s.overhead_pct);
    }

    if (samples.empty()) { return 1; }

    const auto& base = samples.front();
    const auto& finest = samples.back();

    // Fixed cost per extra chunk, derived from the two endpoints. This is the
    // number that actually drives the policy: whether chunking is cheap depends
    // entirely on how it compares to the chunk's own duration.
    const double per_chunk_ms =
        finest.chunks > base.chunks
            ? (finest.wall_ms - base.wall_ms) /
                  static_cast<double>(finest.chunks - base.chunks)
            : 0.0;

    // How many chunks are needed to bring the baseline under R4's ceiling, and
    // what that costs.
    std::printf("# per_chunk_fixed_cost_ms=%.3f\n", per_chunk_ms);
    for (const auto& s : samples) {
        if (s.max_chunk_ms <= 250.0) {
            std::printf("# minimum_chunks_for_r4=%u max_chunk_ms=%.1f cost=%+.2f%%\n",
                        s.chunks, s.max_chunk_ms, s.overhead_pct);
            break;
        }
    }
    const bool r4_ok = finest.max_chunk_ms <= 250.0;
    std::printf("# finest_split=%u chunks max_chunk_ms=%.1f r4_250ms_ceiling=%s\n",
                finest.chunks, finest.max_chunk_ms, r4_ok ? "PASS" : "FAIL");

    std::fprintf(stderr,
                 "baseline %.1f ms | %u chunks: %.1f ms (%+.2f%%) | "
                 "max chunk %.1f ms | R4 %s\n",
                 samples.front().wall_ms, finest.chunks, finest.wall_ms,
                 finest.overhead_pct, finest.max_chunk_ms,
                 r4_ok ? "PASS" : "FAIL");
    return r4_ok ? 0 : 1;
}

/// Step 0.14 — device-loss recovery.
///
/// The test is deliberately falsifiable. It is not enough to destroy a device,
/// call recover, and see no crash — that would pass even if recovery did
/// nothing. So it asserts the middle step: after the forced loss, real GPU work
/// must FAIL. Only then does a subsequent success prove recovery produced a
/// genuinely working device.
int RunRecovery(const std::string& dir) {
    const std::string wgsl = ReadFile(dir + "/smoke_double.wgsl");
    if (wgsl.empty()) {
        std::fprintf(stderr, "could not read smoke_double.wgsl\n");
        return 1;
    }

    p2pgpu::worker::DeviceSession session;

    // Record the actual event SEQUENCE rather than counters. Counters cannot
    // express ordering across separate incidents: the manual Recover() in step 4
    // legitimately fires ready-without-lost, which would skew any cumulative
    // comparison. 'L' = lost, 'R' = ready.
    std::string events;
    int lost_events = 0;
    int ready_events = 0;
    int gone_events = 0;
    session.OnLost([&] { events.push_back('L'); ++lost_events; });
    session.OnReady([&] { events.push_back('R'); ++ready_events; });
    session.OnUnrecoverable([&] { events.push_back('X'); ++gone_events; });

    if (!session.Start()) {
        std::fprintf(stderr, "no WebGPU device available; declining to run\n");
        return 1;
    }

    std::vector<float> input(1000);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<float>(i) + 1.0F;
    }
    const auto bytes = std::as_bytes(std::span<const float>{input});

    const auto works = [&]() {
        const auto r = p2pgpu::worker::RunUnaryKernel(
            session.context(), wgsl, "main", bytes,
            static_cast<std::uint32_t>(input.size()), 256);
        return r.has_value() && r->size() == bytes.size_bytes();
    };

    std::printf("step 0.14 — device-loss recovery\n");

    // 1. healthy
    const bool before = works();
    std::printf("  1. kernel before loss        : %s\n", before ? "OK" : "FAILED");

    // 2. destroy the device for real
    session.ForceLossForTest();
    std::printf("  2. device destroyed          : healthy=%s\n",
                session.healthy() ? "true" : "false");

    // 3. THE FALSIFIABLE STEP — work must now fail
    const bool during = works();
    std::printf("  3. kernel on dead device     : %s  (expected FAILED)\n",
                during ? "OK" : "FAILED");

    // 4. recover
    const bool recovered = session.Recover();
    std::printf("  4. recovery                  : %s (attempts recorded: %u)\n",
                recovered ? "OK" : "FAILED", session.recovery_count());

    // 5. healthy again
    const bool after = recovered && works();
    std::printf("  5. kernel after recovery     : %s\n", after ? "OK" : "FAILED");

    // 6. The AUTOMATIC path, which is what actually runs in production: a
    //    genuine loss must fire on_lost, recover, and fire on_ready — in that
    //    order. Steps 1-5 exercise recovery; this exercises the ORDERING, and
    //    getting it backwards would leave the coordinator waiting on tasks this
    //    worker can no longer compute until their leases expire.
    const int lost_before = lost_events;
    const int ready_before = ready_events;
    const std::size_t events_before = events.size();
    session.SimulateGenuineLossForTest();
    const bool auto_recovered = session.healthy() && works();

    // The ordering assertion: exactly one lost, then exactly one ready.
    // "RL" would mean leases were released only AFTER re-acquiring, leaving the
    // coordinator waiting on tasks this worker could no longer compute.
    const std::string incident = events.substr(events_before);
    const bool order_ok = (incident == "LR");
    std::printf("  6. automatic loss path       : %s (sequence \"%s\", expected \"LR\")\n",
                auto_recovered ? "OK" : "FAILED", incident.c_str());

    // 7. THE GIVING-UP PATH (D-0065). Until now the only outcome this harness
    //    could produce was successful recovery, so the branch that fires when
    //    recovery EXHAUSTS was reachable on exactly one machine in the world:
    //    a Windows box mid-TDR. That is how it went a whole phase without
    //    anyone noticing it only wrote a log line.
    //
    //    Forcing acquisition to fail exercises our reaction, not the browser's
    //    behaviour — see FailRecoveryForTest. Expect "LX": leases released,
    //    then unrecoverable. An "LR" here would mean the forcing did not take
    //    and the step proved nothing.
    //
    //    Takes ~6.3 s: the real backoff, deliberately not shortened.
    const std::size_t gone_before_events = events.size();
    session.FailRecoveryForTest(true);
    session.SimulateGenuineLossForTest();
    const std::string gone_incident = events.substr(gone_before_events);
    const bool gone_ok = (gone_incident == "LX") && session.unrecoverable() &&
                         !session.healthy() && gone_events == 1;
    std::printf("  7. unrecoverable loss        : %s (sequence \"%s\", expected \"LX\")\n",
                gone_ok ? "OK" : "FAILED", gone_incident.c_str());
    session.FailRecoveryForTest(false);

    session.Stop();

    const bool pass = before && !during && recovered && after && auto_recovered &&
                      (lost_events - lost_before) == 2 &&
                      (ready_events - ready_before) == 1 && order_ok && gone_ok;
    std::printf("  callbacks: lost=%d ready=%d unrecoverable=%d\n",
                lost_events, ready_events, gone_events);
    std::printf("%s\n", pass ? "PASS" : "FAIL");
    if (during) {
        std::fprintf(stderr,
                     "NOTE: work SUCCEEDED on a destroyed device — the test cannot "
                     "distinguish recovery from no-op. Investigate before trusting it.\n");
    }
    return pass ? 0 : 1;
}

int RunSmoke(const platform::GpuContext& ctx, const std::string& dir) {
    const std::string double_src = ReadFile(dir + "/smoke_double.wgsl");
    const std::string hash_src   = ReadFile(dir + "/smoke_hash.wgsl");
    const std::string probe_src  = ReadFile(dir + "/divergence_probe.wgsl");
    if (double_src.empty() || hash_src.empty()) {
        std::fprintf(stderr, "could not read kernels from %s\n", dir.c_str());
        return 1;
    }

    const platform::AdapterDescription info = platform::DescribeAdapter(ctx);
    std::printf("target  : native\n");
    std::printf("adapter : %s / %s / %s\n", info.vendor.c_str(),
                info.architecture.c_str(), info.device.c_str());
    std::printf("backend : %s\n", info.backend.c_str());

    const std::vector<p2pgpu::worker::KernelSource> kernels{
        {"smoke_double", double_src},
        {"smoke_hash", hash_src},
        // R6 evidence (step 0.16). Dumped, not verified — no CPU reference
        // exists for transcendentals, which is the entire point.
        {"divergence_probe", probe_src},
    };
    const auto report = p2pgpu::worker::RunSmokeSuite(ctx, kernels);

    // Report to stdout (redirect it to a file for the 0.9 comparison), verdict
    // to stderr so a human sees it without polluting the artifact.
    std::fputs(report.report.c_str(), stdout);
    std::fprintf(stderr, "%s\n", report.passed ? "PASS" : "FAIL");
    return report.passed ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string dir = P2PGPU_KERNEL_DIR;
    std::string url = "ws://localhost:8080/ws";
    std::uint64_t chunk = 1u << 20;

    CLI::App app{"p2pgpu native worker"};
    // `run` is the product; the rest are the Phase 0 measurement harnesses that
    // produced the EVALUATION.md numbers and stay because the evidence has to
    // be reproducible.
    std::string mode = "run";
    app.add_option("mode", mode,
                   "run | smoke | bench | chunking | recovery | adapter")
        ->capture_default_str();
    app.add_option("--coordinator", url, "coordinator WebSocket URL")
        ->capture_default_str();
    app.add_option("--kernel-dir", dir, "WGSL directory")->capture_default_str();
    app.add_option("--units-per-chunk", chunk,
                   "bounds ONE dispatch so it stays far under the ~2s TDR limit (R4)")
        ->capture_default_str();
    // Exceptions are fine here: this is startup/config, before any work begins
    // (CONVENTIONS.md §1).
    CLI11_PARSE(app, argc, argv);

    if (mode == "run") {
        return RunGrid(url, dir, chunk);
    }

    if (mode == "recovery") {
        // Runs before the shared acquisition below: DeviceSession owns its own
        // device, and holding a second one would muddy what is being tested.
        return RunRecovery(dir);
    }

    platform::GpuContext ctx;
    if (!platform::AcquireDevice(ctx)) {
        // Not a crash — an unsupported machine is a capability we report.
        std::fprintf(stderr, "no WebGPU device available; declining to run\n");
        return 1;
    }

    int rc = 0;
    if (mode == "adapter") {
        rc = DumpAdapter(ctx);
    } else if (mode == "bench") {
        rc = RunBench(ctx, dir);
    } else if (mode == "chunking") {
        rc = RunChunking(ctx, dir);
    } else if (mode == "smoke") {
        rc = RunSmoke(ctx, dir);
    } else {
        std::fprintf(stderr,
                     "unknown mode: %s\n"
                     "usage: worker-native [run|smoke|bench|chunking|recovery|adapter]\n",
                     mode.c_str());
        rc = 2;
    }

    platform::ReleaseDevice(ctx);
    return rc;
}
