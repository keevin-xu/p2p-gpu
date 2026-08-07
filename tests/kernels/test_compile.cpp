// Every kernel in `kernels/` compiles and builds a pipeline — step 5.6.
//
// ── WHY THIS IS A SEPARATE TEST FROM THE GOLDEN ONES ─────────────────────
// The golden and chunk-invariance tests only exercise kernels something happens
// to RUN. A kernel that no test executes yet — `pathtrace_tile.wgsl` on the day
// it is written, before 5.12 lands the host bindings it needs — has nothing
// checking its WGSL at all, and that is exactly the one whose syntax rots.
//
// Enumerating the DIRECTORY rather than a hardcoded list is deliberate: a new
// kernel is covered the moment it exists, with nobody having to remember to add
// it here.
//
// This runs in CI against lavapipe (4.4), so a construct one backend accepts
// and another rejects shows up on every commit rather than on a stranger's GPU.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "p2pgpu/worker/kernel_host.hpp"
#include "p2pgpu/worker/platform.hpp"

namespace platform = p2pgpu::worker::platform;

namespace {

std::string ReadFile(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::filesystem::path KernelDir() {
    return std::filesystem::path(P2PGPU_KERNEL_DIR);
}

}  // namespace

TEST_CASE("every kernel in kernels/ compiles into a pipeline", "[kernel]") {
    platform::GpuContext ctx;
    REQUIRE(platform::AcquireDevice(ctx));

    std::vector<std::filesystem::path> found;
    for (const auto& entry : std::filesystem::directory_iterator(KernelDir())) {
        if (entry.path().extension() == ".wgsl") {
            found.push_back(entry.path());
        }
    }
    // A directory walk that finds nothing passes every assertion below it. That
    // is the shape of D-0067's CI step and of the mutation test that corrupted
    // nothing — so the count is asserted before anything is checked.
    REQUIRE(found.size() >= 4);

    for (const auto& path : found) {
        const std::string src = ReadFile(path);
        INFO("kernel: " << path.filename().string());
        REQUIRE_FALSE(src.empty());
        // Every kernel in this project uses `main` as its entry point; the
        // manifest records it per-kernel, but the manifest is coordinator-side
        // and this test is deliberately free of it.
        CHECK(p2pgpu::worker::CompileKernel(ctx, src, "main"));
    }

    platform::ReleaseDevice(ctx);
}

TEST_CASE("a deliberately broken kernel FAILS to compile", "[kernel]") {
    // The falsifiability check. Without it, `CompileKernel` returning true
    // unconditionally would leave every assertion above passing.
    platform::GpuContext ctx;
    REQUIRE(platform::AcquireDevice(ctx));

    CHECK_FALSE(p2pgpu::worker::CompileKernel(ctx, "this is not wgsl", "main"));
    // Valid WGSL, wrong entry point — the case a module-only check would miss,
    // and the reason CompileKernel builds the pipeline too.
    CHECK_FALSE(p2pgpu::worker::CompileKernel(
        ctx, "@compute @workgroup_size(1) fn main() {}", "nonexistent"));

    platform::ReleaseDevice(ctx);
}
