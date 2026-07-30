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

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "p2pgpu/worker/bench.hpp"
#include "p2pgpu/worker/platform.hpp"
#include "p2pgpu/worker/smoke.hpp"

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
                Escape(a.backend).c_str(), features.c_str(),
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

int RunSmoke(const platform::GpuContext& ctx, const std::string& dir) {
    const std::string double_src = ReadFile(dir + "/smoke_double.wgsl");
    const std::string hash_src   = ReadFile(dir + "/smoke_hash.wgsl");
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
    // CLI11 argument handling arrives with the real task loop in step 1.22.
    const std::string mode = argc > 1 ? argv[1] : "smoke";
    const std::string dir = P2PGPU_KERNEL_DIR;

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
    } else if (mode == "smoke") {
        rc = RunSmoke(ctx, dir);
    } else {
        std::fprintf(stderr, "usage: worker-native [smoke|bench|adapter]\n");
        rc = 2;
    }

    platform::ReleaseDevice(ctx);
    return rc;
}
