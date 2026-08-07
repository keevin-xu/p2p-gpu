// `pathtrace_tile_v1` on a real GPU — steps 5.6 / 5.8 / 5.12.
//
// ── WHAT THIS CAN AND CANNOT ESTABLISH ───────────────────────────────────
// There is no CPU reference for the path tracer yet (5.19), so nothing here
// proves the image is CORRECT. 1.26 is the standing warning: the first
// end-to-end run was 1000/1000 wrong because every check compared the GPU
// against itself.
//
// So these assert properties that a wrong-but-self-consistent kernel would
// still FAIL:
//
//   · chunk invariance for an ACCUMULATING kernel — the property 5.8's
//     "zero once per task, never per chunk" exists to protect, and the one
//     most likely to break silently;
//   · determinism across runs, which K2 requires and replication depends on;
//   · the sample counter matching the samples actually requested, which is
//     what 5.15's compositing weights tiles by;
//   · energy bounds, which catch a black or NaN image.
//
// Correctness against ground truth is 5.19/5.20 and is not claimed here.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "p2pgpu/kernels/pathtrace_params.hpp"
#include "p2pgpu/scene/bvh.hpp"
#include "p2pgpu/scene/scene.hpp"
#include "p2pgpu/worker/kernel_host.hpp"
#include "p2pgpu/worker/platform.hpp"

namespace platform = p2pgpu::worker::platform;
using namespace p2pgpu::kernels;
using namespace p2pgpu::scene;

namespace {

constexpr std::uint32_t kTile = 16;   // 16x16 keeps the test quick; the R5
                                      // algebra is tile-size independent
                                      // anyway (D-0068), so a small tile tests
                                      // exactly the same code paths.

std::string KernelSource() {
    const std::string path = std::string(P2PGPU_KERNEL_DIR) + "/pathtrace_tile.wgsl";
    std::FILE* f = std::fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);
    std::string out;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return out;
}

const char* kScene = R"(version 1
camera origin 0 1.4 4 target 0 0.4 0 up 0 1 0 vfov_deg 38
material 0 lambertian 0.72 0.72 0.70
material 1 metal 0.88 0.88 0.92 0.02
material 2 emissive 9 8.4 7.2
material 3 lambertian 0.80 0.25 0.25
tri -9 0 -9  9 0 -9  9 0 9  0
tri -9 0 -9  9 0 9  -9 0 9  0
sphere 0 0.75 0 0.75 1
sphere -1.7 0.5 0.35 0.5 3
sphere 1.7 0.5 0.35 0.5 3
sphere 2.6 4.2 2.2 1.1 2
)";

struct Fixture {
    Bvh bvh;
    std::vector<std::byte> nodes;
    std::vector<std::byte> prims;
    std::vector<std::byte> materials;
    std::vector<std::span<const std::byte>> inputs;

    Fixture() {
        auto sc = ParseScene(kScene);
        REQUIRE(sc.has_value());
        // Round-trip through the ASSET, not the builder's output. This is the
        // path a real worker takes, and `LoadBvh` is what guarantees every
        // index is in range before any of it reaches a GPU (5.5, D-0070).
        const auto blob = SerializeBvh(BuildBvh(*sc));
        auto loaded = LoadBvh(blob);
        REQUIRE(loaded.has_value());
        bvh = std::move(*loaded);

        const auto to_bytes = [](const auto& vec) {
            const auto* p = reinterpret_cast<const std::byte*>(vec.data());
            return std::vector<std::byte>(p, p + vec.size() * sizeof(vec[0]));
        };
        nodes = to_bytes(bvh.nodes);
        prims = to_bytes(bvh.prims);
        materials = to_bytes(bvh.materials);
        inputs = {nodes, prims, materials};
    }

    [[nodiscard]] PathTraceParams Params(std::uint32_t seed) const {
        PathTraceParams p{};
        p.tile_x = 0;
        p.tile_y = 0;
        p.tile_w = kTile;
        p.tile_h = kTile;
        p.image_w = kTile;
        p.image_h = kTile;
        // A simple pinhole frame, computed the way the coordinator will (R1:
        // the coordinator owns params; this mirrors what src/coordinator will
        // produce so the test exercises the real shape).
        p.cam_origin[0] = 0.0F; p.cam_origin[1] = 1.4F; p.cam_origin[2] = 4.0F;
        p.cam_lower_left[0] = -1.0F; p.cam_lower_left[1] = 0.4F; p.cam_lower_left[2] = 3.0F;
        p.cam_horizontal[0] = 2.0F;
        p.cam_vertical[1] = 2.0F;
        p.seed = seed;
        p.max_bounces = 8;
        p.rr_start_bounce = 3;
        p.node_count = static_cast<std::uint32_t>(bvh.nodes.size());
        p.prim_count = static_cast<std::uint32_t>(bvh.prims.size());
        p.material_count = static_cast<std::uint32_t>(bvh.materials.size());
        return p;
    }
};

std::vector<AccumPixel> Render(const platform::GpuContext& ctx, const Fixture& fx,
                               const std::string& wgsl, std::uint64_t samples,
                               std::uint64_t units_per_chunk, std::uint32_t seed) {
    const PathTraceParams params = fx.Params(seed);
    const auto* pb = reinterpret_cast<const std::byte*>(&params);

    p2pgpu::worker::TaskRequest req{};
    req.wgsl = wgsl;
    req.entry_point = "main";
    req.params = std::span<const std::byte>(pb, sizeof(params));
    req.start_unit = 0;
    req.unit_count = samples;
    req.output_bytes = TileOutputBytes(kTile, kTile);
    // The kernel is @workgroup_size(8,8,1) over a grid of PIXELS. A unit here
    // is one sample of the whole tile, so the grid must NOT be derived from the
    // chunk's unit count — see TaskRequest::invocations_x.
    req.workgroup_size = 8;
    req.workgroup_size_y = 8;
    req.invocations_x = kTile;
    req.invocations_y = kTile;
    req.inputs = fx.inputs;

    const auto outcome = p2pgpu::worker::RunTask(ctx, req, units_per_chunk);
    REQUIRE(outcome.has_value());
    REQUIRE(outcome->output.size() == req.output_bytes);

    std::vector<AccumPixel> px(kTile * kTile);
    std::memcpy(px.data(), outcome->output.data(), outcome->output.size());
    return px;
}

}  // namespace

TEST_CASE("pathtrace accumulates and reports the samples it was asked for",
          "[kernel]") {
    platform::GpuContext ctx;
    REQUIRE(platform::AcquireDevice(ctx));
    const Fixture fx;
    const std::string wgsl = KernelSource();

    constexpr std::uint64_t kSamples = 64;
    const auto img = Render(ctx, fx, wgsl, kSamples, kSamples, 1234);

    double total = 0.0;
    for (const auto& p : img) {
        // The `.w` lane is what 5.15 weights tiles by when compositing. If it
        // disagreed with the samples actually taken, tiles that received
        // different amounts of work would blend at the wrong ratio — a subtly
        // wrong image with nothing failing.
        CHECK(p.samples == static_cast<float>(kSamples));
        CHECK(std::isfinite(p.r));
        CHECK(std::isfinite(p.g));
        CHECK(std::isfinite(p.b));
        CHECK(p.r >= 0.0F);
        total += p.r + p.g + p.b;
    }
    // Not all black. A kernel that traced nothing, or whose BVH bindings were
    // wrong, would return zeros and pass every check above.
    CHECK(total > 0.0);

    platform::ReleaseDevice(ctx);
}

TEST_CASE("pathtrace chunk invariance: 1 dispatch == 4 == 16", "[kernel]") {
    // THE PROPERTY 5.8 EXISTS TO PROTECT. An accumulating kernel is where
    // "zero once per task, never per chunk" can break silently: re-initialising
    // between chunks would show up ONLY here, as a result that depends on how
    // the range was split — which presents as scheduling nondeterminism, the
    // most expensive misdiagnosis in a system whose verification story is
    // determinism (R6).
    //
    // ── TOLERANCE, NOT BITWISE, AND THAT IS NOT A COMPROMISE (D-0073) ────
    // `brute_search_v1`'s chunk invariance is bitwise because its reductions
    // are integer. This kernel accumulates FLOATS: each dispatch sums its
    // samples in registers and adds one value, so four chunks produce
    // `((Σ1+Σ2)+Σ3)+Σ4` where one produces `Σ1..64`. Float addition is not
    // associative — measured at exactly 1 ULP.
    //
    // The bitwise version is achievable only by accumulatingevery sample straight
    // into global memory, which puts a read-modify-write in the inner loop of
    // the workload whose whole purpose is showing that accumulation keeps nodes
    // compute-bound. Rejected in D-0073.
    //
    // The SAMPLE COUNT is still checked exactly: it is integer-valued, and 5.15
    // weights tiles by it when compositing.
    platform::GpuContext ctx;
    REQUIRE(platform::AcquireDevice(ctx));
    const Fixture fx;
    const std::string wgsl = KernelSource();

    constexpr std::uint64_t kSamples = 64;
    const auto one = Render(ctx, fx, wgsl, kSamples, kSamples, 7);
    const auto four = Render(ctx, fx, wgsl, kSamples, kSamples / 4, 7);
    const auto sixteen = Render(ctx, fx, wgsl, kSamples, kSamples / 16, 7);

    // 1e-5 relative is ~80 ULP of headroom at these magnitudes — loose enough
    // for a different summation order, and FAR too tight to hide a re-initialised
    // accumulator, which would drop 3/4 of the energy in the 4-chunk case.
    const auto close = [](float a, float b) {
        return std::abs(a - b) <= 1e-5F * std::max(1.0F, std::max(std::abs(a), std::abs(b)));
    };

    REQUIRE(one.size() == four.size());
    for (std::size_t i = 0; i < one.size(); ++i) {
        INFO("pixel " << i);
        CHECK(close(one[i].r, four[i].r));
        CHECK(close(one[i].g, four[i].g));
        CHECK(close(one[i].b, four[i].b));
        CHECK(close(one[i].r, sixteen[i].r));
        // Exact: an integer quantity carried in a float lane.
        CHECK(one[i].samples == four[i].samples);
        CHECK(one[i].samples == sixteen[i].samples);
    }
    platform::ReleaseDevice(ctx);
}

TEST_CASE("pathtrace is deterministic across runs and sensitive to the seed",
          "[kernel]") {
    // Determinism is what replication and speculative re-execution rest on
    // (K2). The seed-sensitivity half is the falsifiability check: a kernel
    // that ignored its seed would satisfy determinism perfectly.
    platform::GpuContext ctx;
    REQUIRE(platform::AcquireDevice(ctx));
    const Fixture fx;
    const std::string wgsl = KernelSource();

    const auto a = Render(ctx, fx, wgsl, 32, 32, 99);
    const auto b = Render(ctx, fx, wgsl, 32, 32, 99);
    const auto c = Render(ctx, fx, wgsl, 32, 32, 100);

    bool differs = false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].r == b[i].r);
        CHECK(a[i].g == b[i].g);
        differs = differs || (a[i].r != c[i].r);
    }
    CHECK(differs);
    platform::ReleaseDevice(ctx);
}

TEST_CASE("a disjoint sample range adds to the accumulator", "[kernel]") {
    // 5.13 will have a worker upload partial results and continue. The property
    // that makes that sound: samples [0,32) then [32,64) must total the same as
    // [0,64) in one go, because the RNG is keyed on the ABSOLUTE sample index
    // (K2) rather than on position within a dispatch.
    platform::GpuContext ctx;
    REQUIRE(platform::AcquireDevice(ctx));
    const Fixture fx;
    const std::string wgsl = KernelSource();

    const auto whole = Render(ctx, fx, wgsl, 64, 64, 5);

    // Two halves, into one accumulator, by running a task whose range starts
    // partway through. Done via two RunTask calls means two zeroed buffers, so
    // this compares the SUM of the halves against the whole.
    const PathTraceParams base = fx.Params(5);
    const auto render_range = [&](std::uint64_t start, std::uint64_t count) {
        PathTraceParams p = base;
        const auto* pb = reinterpret_cast<const std::byte*>(&p);
        p2pgpu::worker::TaskRequest req{};
        req.wgsl = wgsl;
        req.entry_point = "main";
        req.params = std::span<const std::byte>(pb, sizeof(p));
        req.start_unit = start;
        req.unit_count = count;
        req.output_bytes = TileOutputBytes(kTile, kTile);
        req.workgroup_size = 8;
        req.workgroup_size_y = 8;
        req.invocations_x = kTile;
        req.invocations_y = kTile;
        req.inputs = fx.inputs;
        const auto outcome = p2pgpu::worker::RunTask(ctx, req, count);
        REQUIRE(outcome.has_value());
        std::vector<AccumPixel> px(kTile * kTile);
        std::memcpy(px.data(), outcome->output.data(), outcome->output.size());
        return px;
    };

    const auto first = render_range(0, 32);
    const auto second = render_range(32, 32);

    for (std::size_t i = 0; i < whole.size(); ++i) {
        INFO("pixel " << i);
        CHECK(first[i].samples + second[i].samples == whole[i].samples);
        // Float addition is not associative, so the sums agree to tolerance
        // rather than bitwise — this is a genuinely different summation order,
        // unlike the chunk-invariance case where the order is identical.
        const float sum = first[i].r + second[i].r;
        CHECK(std::abs(sum - whole[i].r) <= 1e-4F * std::max(1.0F, std::abs(whole[i].r)));
    }
    platform::ReleaseDevice(ctx);
}
