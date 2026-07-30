// Thin main() over worker-core. CLI11 args, then hand off.
//
// If this file grows past ~100 lines, logic is leaking out of worker-core
// and rule R1 is being violated.
//
// Phase 4 step 4.19 packages this so a borrowed machine can join with one
// command (docs/RISKS.md R-D).
//
// Right now it is step 0.8's harness: acquire a device, run the smoke kernel,
// and ASSERT THE READBACK. The assertion is the point — a shader that appears
// to run but writes nothing is the most common early WebGPU bug, and it looks
// exactly like success if you only check return codes.

#include <cstdio>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "p2pgpu/worker/kernel_host.hpp"
#include "p2pgpu/worker/platform.hpp"

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

int main(int argc, char** argv) {
    namespace platform = p2pgpu::worker::platform;

    // CLI11 argument handling arrives with the real task loop in step 1.22.
    const std::string kernel_path =
        argc > 1 ? std::string{argv[1]}
                 : std::string{P2PGPU_KERNEL_DIR} + "/smoke_double.wgsl";

    const std::string wgsl = ReadFile(kernel_path);
    if (wgsl.empty()) {
        std::fprintf(stderr, "could not read kernel: %s\n", kernel_path.c_str());
        return 1;
    }

    platform::GpuContext ctx;
    if (!platform::AcquireDevice(ctx)) {
        // Not a crash — an unsupported machine is a capability we report.
        std::fprintf(stderr, "no WebGPU device available; declining to run\n");
        return 1;
    }

    const platform::AdapterDescription info = platform::DescribeAdapter(ctx);
    std::printf("adapter : %s / %s / %s\n", info.vendor.c_str(),
                info.architecture.c_str(), info.device.c_str());
    std::printf("backend : %s\n", info.backend.c_str());

    // 1000 elements deliberately does NOT divide the 256 workgroup size, so the
    // dispatch overhangs and the kernel's bounds check gets exercised.
    std::vector<float> input(1000);
    std::iota(input.begin(), input.end(), 1.0F);

    const auto result =
        p2pgpu::worker::RunUnaryF32Kernel(ctx, wgsl, "main", input, 256);
    if (!result) {
        std::fprintf(stderr, "kernel execution failed\n");
        platform::ReleaseDevice(ctx);
        return 1;
    }

    // THE READBACK ASSERTION (steps 0.6 / 0.8). Exact comparison is valid here:
    // multiplying by 2.0 is exactly representable in binary floating point, so
    // this is not an R6 tolerance case. Genuine R6 divergence needs real
    // arithmetic across differing vendors.
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if ((*result)[i] != input[i] * 2.0F) {
            if (mismatches < 5) {
                std::fprintf(stderr, "  [%zu] expected %.1f got %.1f\n", i,
                             static_cast<double>(input[i] * 2.0F),
                             static_cast<double>((*result)[i]));
            }
            ++mismatches;
        }
    }

    platform::ReleaseDevice(ctx);

    if (mismatches != 0) {
        std::fprintf(stderr, "FAIL: %zu/%zu elements wrong\n", mismatches, input.size());
        return 1;
    }
    std::printf("PASS: %zu/%zu elements correct (out[i] == in[i] * 2.0)\n",
                input.size(), input.size());
    return 0;
}
