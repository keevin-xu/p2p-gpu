#pragma once
//
// CPU path tracer — step 5.19. GROUND TRUTH.
//
// ── WHY THIS EXISTS, AND WHY A SINGLE-MACHINE GPU RENDER IS NOT ENOUGH ───
// 1.26 is the standing warning: the first end-to-end brute_search run reported
// 1000/1000 correct and every answer was wrong. Chunk invariance, reduction
// agreement and the pipeline tests all passed, because **every one of them
// compared the GPU against itself**. Two defects survived all of them and were
// caught only by 1.25's CPU reference.
//
// The same trap is open here in a worse form: a path tracer produces a
// plausible-looking image from almost any bug. A single-machine GPU render
// (5.19 as literally worded) validates the DISTRIBUTION — tiling, scheduling,
// accumulation, compositing — and says nothing about whether the kernel
// computes light transport correctly.
//
// ── IT DOES NOT REPRODUCE THE GPU'S RNG, ON PURPOSE ──────────────────────
// `BruteSearchReference` had to be bit-identical because that kernel is
// `Exact`. This one must NOT be: an independent implementation that happens to
// draw the same numbers would agree with the GPU even if both integrate the
// wrong function. Written from the rendering equation rather than transliterated
// from the WGSL — a different RNG, a different traversal order, the same
// integral.
//
// So the comparison is STATISTICAL, which is exactly what 5.14's comparator is
// for, and running it against real render data is what D-0075 flagged as
// missing: that comparator was calibrated on Gaussian noise and has never seen
// a real firefly.

#include <cstdint>
#include <span>
#include <vector>

#include "p2pgpu/scene/bvh.hpp"

namespace p2pgpu::kernels {

/// Camera, in the same parameterisation the kernel uses so a comparison renders
/// the same view rather than a similar one.
struct ReferenceCamera {
    float origin[3]{};
    float lower_left[3]{};
    float horizontal[3]{};
    float vertical[3]{};
};

struct ReferenceRequest {
    ReferenceCamera camera;
    std::uint32_t image_w = 0;
    std::uint32_t image_h = 0;
    /// Region to render. The whole image when equal to it.
    std::uint32_t tile_x = 0;
    std::uint32_t tile_y = 0;
    std::uint32_t tile_w = 0;
    std::uint32_t tile_h = 0;
    std::uint64_t samples = 64;
    std::uint32_t seed = 1;
    std::uint32_t max_bounces = 8;
    std::uint32_t rr_start_bounce = 3;
};

/// Render on the CPU. Returns `{r,g,b,samples}` per pixel — the SAME
/// accumulator layout the kernel writes, so the result drops straight into
/// `Compare` with `Determinism::Statistical` and into `Compositor::AcceptTile`.
///
/// Single-threaded and slow by design: a reference that shares an optimisation
/// with the thing it checks is not independent. Minutes at a useful sample
/// count, which is why 5.20 compares a small tile rather than a full image.
[[nodiscard]] std::vector<float> PathTraceReference(const scene::Bvh& bvh,
                                                    const ReferenceRequest& req);

}  // namespace p2pgpu::kernels
