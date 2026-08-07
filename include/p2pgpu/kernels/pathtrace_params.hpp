#pragma once
//
// `PathTraceParams` — the C++ side of `kernels/pathtrace_tile.wgsl`. Steps 5.6
// and 5.11.
//
// ── WHY EVERY OFFSET IS ASSERTED AND NOT JUST THE SIZE ───────────────────
// `BruteSearchParams` taught this: a struct whose fields are the right types in
// the wrong order has the RIGHT SIZE. Two swapped `u32`s produce a well-formed
// buffer that the kernel reads as different values, and nothing at runtime can
// notice — the bytes are valid either way (CONVENTIONS.md §5).
//
// This struct is much larger than that one, which is exactly why 5.11 calls it
// out as a real risk rather than a formality.
//
// ── WGSL LAYOUT RULES THIS ENCODES ───────────────────────────────────────
// `vec3<f32>` has ALIGNMENT 16 and SIZE 12. So a vec3 must start at a multiple
// of 16, and the 4 bytes after it are free for a scalar. Every scalar following
// a vec3 below is deliberately placed in that gap rather than being padding —
// if a field is ever removed, its slot must become an explicit `pad` rather
// than letting the next field slide up.
//
// ── K1: THE CHUNK WINDOW IS BYTES 0-7 ────────────────────────────────────
// `start_unit` and `unit_count` first, always (D-0033). The host rewrites
// exactly those 8 bytes per dispatch with no kernel-specific knowledge. A vec3
// placed first would push them off byte 0 and the host would overwrite camera
// data with chunk bounds — producing a well-formed result computed over a
// garbage range, which no runtime check can catch.

#include <cstddef>
#include <cstdint>

namespace p2pgpu::kernels {

/// A 3-vector laid out as WGSL's `vec3<f32>`: 12 bytes of data, 16-byte
/// alignment. Declared explicitly rather than using a library type so the
/// static_asserts below are about OUR layout and not a dependency's.
struct alignas(16) Vec3Padded {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    // The 4 bytes the alignment reserves are NOT declared here — the following
    // struct member occupies them. See the header note.
};

struct PathTraceParams {
    // ── THE CHUNK WINDOW. Do not move (K1 / D-0033). ────────────────────
    std::uint32_t start_unit = 0;    ///< first sample index
    std::uint32_t unit_count = 0;    ///< samples in this dispatch

    std::uint32_t tile_x = 0;
    std::uint32_t tile_y = 0;
    std::uint32_t tile_w = 0;
    std::uint32_t tile_h = 0;
    std::uint32_t image_w = 0;
    std::uint32_t image_h = 0;

    // Each vec3 is followed by a scalar occupying the alignment gap.
    float cam_origin[3]{};
    std::uint32_t seed = 0;

    float cam_lower_left[3]{};
    std::uint32_t max_bounces = 8;

    float cam_horizontal[3]{};
    std::uint32_t node_count = 0;

    float cam_vertical[3]{};
    std::uint32_t prim_count = 0;

    std::uint32_t material_count = 0;
    /// Bounce at which Russian roulette begins. Before it, paths always
    /// continue; after it, survivors are divided by their survival probability
    /// so the estimator stays unbiased.
    std::uint32_t rr_start_bounce = 3;
    std::uint32_t pad0 = 0;
    std::uint32_t pad1 = 0;
};

// ── PARITY WITH THE WGSL (5.11) ──────────────────────────────────────────
// Every offset, not just the size. Compare against the offset comments in
// kernels/pathtrace_tile.wgsl — the two lists must be read together.
static_assert(sizeof(PathTraceParams) == 112, "params size must match the WGSL struct");
static_assert(alignof(PathTraceParams) == 4,
              "no over-alignment: the host writes this as a flat byte span");

static_assert(offsetof(PathTraceParams, start_unit) == 0,
              "K1: the chunk window MUST start at byte 0 (D-0033)");
static_assert(offsetof(PathTraceParams, unit_count) == 4,
              "K1: unit_count MUST be at byte 4 (D-0033)");
static_assert(offsetof(PathTraceParams, tile_x) == 8);
static_assert(offsetof(PathTraceParams, tile_y) == 12);
static_assert(offsetof(PathTraceParams, tile_w) == 16);
static_assert(offsetof(PathTraceParams, tile_h) == 20);
static_assert(offsetof(PathTraceParams, image_w) == 24);
static_assert(offsetof(PathTraceParams, image_h) == 28);
static_assert(offsetof(PathTraceParams, cam_origin) == 32,
              "vec3<f32> must land on a 16-byte boundary");
static_assert(offsetof(PathTraceParams, seed) == 44,
              "seed occupies the gap the vec3's alignment reserves");
static_assert(offsetof(PathTraceParams, cam_lower_left) == 48);
static_assert(offsetof(PathTraceParams, max_bounces) == 60);
static_assert(offsetof(PathTraceParams, cam_horizontal) == 64);
static_assert(offsetof(PathTraceParams, node_count) == 76);
static_assert(offsetof(PathTraceParams, cam_vertical) == 80);
static_assert(offsetof(PathTraceParams, prim_count) == 92);
static_assert(offsetof(PathTraceParams, material_count) == 96);
static_assert(offsetof(PathTraceParams, rr_start_bounce) == 100);
static_assert(offsetof(PathTraceParams, pad0) == 104);
static_assert(offsetof(PathTraceParams, pad1) == 108);

/// One accumulator element: running RGB SUM plus the sample count that produced
/// it. A sum-and-count rather than a running average, because averaging on the
/// worker discards the weight the coordinator needs to merge two partial
/// results from workers that did different amounts of work (5.13/5.15).
struct AccumPixel {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float samples = 0.0F;
};
static_assert(sizeof(AccumPixel) == 16, "accumulator stride is part of the format");

/// Output bytes for one tile. LOCKED BY THE R5 GATE (D-0068): the ratio is
/// `spp * F / bytes_per_pixel`, so growing this weakens the gate
/// proportionally. Adding per-pixel variance for adaptive sampling would
/// HALVE it and must re-run the calculation first.
[[nodiscard]] constexpr std::uint32_t TileOutputBytes(std::uint32_t w,
                                                      std::uint32_t h) noexcept {
    return w * h * static_cast<std::uint32_t>(sizeof(AccumPixel));
}
static_assert(TileOutputBytes(64, 64) == 65536, "D-0068 assumes a 64KiB tile");

}  // namespace p2pgpu::kernels
