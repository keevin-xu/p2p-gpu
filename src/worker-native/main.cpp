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
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "p2pgpu/worker/platform.hpp"
#include "p2pgpu/worker/smoke.hpp"

namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

int main() {
    namespace platform = p2pgpu::worker::platform;

    // CLI11 argument handling arrives with the real task loop in step 1.22.
    const std::string dir = P2PGPU_KERNEL_DIR;
    const std::string double_src = ReadFile(dir + "/smoke_double.wgsl");
    const std::string hash_src   = ReadFile(dir + "/smoke_hash.wgsl");
    if (double_src.empty() || hash_src.empty()) {
        std::fprintf(stderr, "could not read kernels from %s\n", dir.c_str());
        return 1;
    }

    platform::GpuContext ctx;
    if (!platform::AcquireDevice(ctx)) {
        // Not a crash — an unsupported machine is a capability we report.
        std::fprintf(stderr, "no WebGPU device available; declining to run\n");
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
    platform::ReleaseDevice(ctx);

    // Report to stdout (redirect it to a file for the 0.9 comparison), verdict
    // to stderr so a human sees it without polluting the artifact.
    std::fputs(report.report.c_str(), stdout);
    std::fprintf(stderr, "%s\n", report.passed ? "PASS" : "FAIL");
    return report.passed ? 0 : 1;
}
