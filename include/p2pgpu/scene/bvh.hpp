#pragma once
//
// BVH build + the flat asset layout — steps 5.3 and 5.5. Design: D-0069.
//
// ── THIS IS THE MOST DANGEROUS INPUT IN THE PROJECT ──────────────────────
// A serialized BVH is uploaded into a GPU storage buffer and its integers are
// used as ARRAY INDICES by the traversal loop. In Phase 6 those bytes come from
// an arbitrary peer. There is no `flatbuffers::Verifier` for a raw array, so
// R11's first clause is satisfied here by `LoadBvh` instead, which must reject
// a buffer before any field of it is trusted.
//
// Checked at load, on the CPU, exactly once — NOT in the shader. WGSL
// bounds-CLAMPS rather than faulting, so an out-of-range index there yields a
// silently wrong image that passes validation, which is the D-0040 failure and
// the expensive one.
//
// ── THE LAYOUT IS MIRRORED IN WGSL (5.6/5.11) ────────────────────────────
// Every struct below gets `static_assert` parity on size AND every offset, per
// CONVENTIONS.md §5. Drift does not crash: it misreads the tree and renders a
// plausible wrong picture.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "p2pgpu/protocol/error.hpp"
#include "p2pgpu/scene/scene.hpp"

namespace p2pgpu::scene {

/// "P2BV". Cheap, and the first thing that rejects a buffer aimed at the wrong
/// parser — a WGSL file or a FlatBuffers frame arriving here should fail on
/// byte 0, not on a plausible-looking node count.
inline constexpr std::uint32_t kBvhMagic = 0x5642'3250U;
inline constexpr std::uint32_t kBvhVersion = 1U;

/// Bounds every allocation this loader will perform. R11: FlatBuffers prevents
/// corruption but not an attacker declaring 4 GB of nodes, and the same applies
/// with more force where there is no Verifier at all.
inline constexpr std::uint32_t kMaxBvhNodes = 8U << 20;      // 8M nodes, 256 MB
inline constexpr std::uint32_t kMaxBvhPrims = 4U << 20;      // 4M prims, 256 MB
inline constexpr std::uint32_t kMaxBvhMaterials = 1U << 16;

/// 64 bytes: 9 used, 7 reserved. Section offsets are explicit rather than
/// implied by the counts, so the loader validates layout and content
/// separately and a mismatch between them is itself detectable.
struct BvhHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t node_count;
    std::uint32_t prim_count;
    std::uint32_t material_count;
    std::uint32_t node_offset;
    std::uint32_t prim_offset;
    std::uint32_t material_offset;
    std::uint32_t total_bytes;
    /// MUST BE ZERO. Keeps the extension point free and stops a future non-zero
    /// value meaning two things at once — the D-0027 reserved-bytes pattern,
    /// which is also what supplied that fix's alignment.
    std::uint32_t reserved[7];
};

/// 32 bytes = two `vec4<f32>` on the GPU, which is the natural storage-buffer
/// stride and needs no padding games in WGSL.
struct BvhNode {
    float bmin[3];
    /// Internal: index of the LEFT child (right is left+1, so only one index is
    /// stored). Leaf: index of its first primitive.
    ///
    /// A child index MUST be strictly greater than its parent's — acyclicity is
    /// enforced structurally rather than by a graph walk. A cycle would hang the
    /// traversal loop on the GPU, and R4's chunking cannot interrupt a spinning
    /// shader; the worker would look hung with no way back (D-0069).
    std::uint32_t left_or_first;
    float bmax[3];
    /// Bit 31 set = leaf. Bits 0-30 = primitive count for a leaf, 0 otherwise.
    std::uint32_t count_and_leaf;
};

inline constexpr std::uint32_t kLeafFlag = 0x8000'0000U;

/// 64 bytes for every primitive, sphere or triangle, and the waste is bought
/// deliberately (D-0069): uniform stride keeps the traversal fetch
/// non-dependent and makes the bounds check a multiplication rather than a
/// table walk. A validator that is trivial to write correctly is worth the
/// bytes here.
struct BvhPrim {
    /// 0 = sphere, 1 = triangle.
    std::uint32_t kind;
    std::uint32_t material;
    std::uint32_t pad0;
    std::uint32_t pad1;
    /// Sphere: centre. Triangle: vertex A.
    float a[3];
    /// Sphere: radius. Triangle: unused.
    float radius;
    float b[3];
    float pad2;
    float c[3];
    float pad3;
};

inline constexpr std::uint32_t kPrimSphere = 0U;
inline constexpr std::uint32_t kPrimTriangle = 1U;

/// 32 bytes.
struct BvhMaterial {
    float albedo[3];
    float fuzz;
    std::uint32_t kind;
    std::uint32_t pad[3];
};

// Sizes are load-bearing: WGSL mirrors them at 5.6, and the loader's bounds
// arithmetic is `count * sizeof(T)`.
static_assert(sizeof(BvhHeader) == 64, "header size is part of the format");
static_assert(sizeof(BvhNode) == 32, "node must be two vec4s");
static_assert(sizeof(BvhPrim) == 64, "prim stride is part of the format");
static_assert(sizeof(BvhMaterial) == 32, "material size is part of the format");
static_assert(alignof(BvhNode) == 4 && alignof(BvhPrim) == 4,
              "the blob is uploaded verbatim; no implicit padding may appear");

/// A built tree, before serialization.
struct Bvh {
    std::vector<BvhNode> nodes;
    std::vector<BvhPrim> prims;
    std::vector<BvhMaterial> materials;

    /// Deepest path from the root, in nodes. Reported rather than merely
    /// computed: D-0068's F estimate is dominated by traversal depth, and the
    /// R5 floor derived from F is only as honest as this number.
    std::uint32_t max_depth = 0;
    /// Largest leaf, in primitives.
    std::uint32_t max_leaf_prims = 0;
};

/// Build over the scene's primitives. Binned SAH — see the .cpp for why the
/// obvious median split was not good enough for a number D-0068 depends on.
[[nodiscard]] Bvh BuildBvh(const Scene& scene);

/// Serialize to the flat layout. The bytes are what gets content-addressed.
[[nodiscard]] std::vector<std::byte> SerializeBvh(const Bvh& bvh);

/// BLAKE3-256 of the blob, lowercase hex. THE CONTENT ADDRESS: this is the
/// `{hash}` in `GET /asset/{hash}` (5.4) and what 5.5 verifies before parsing.
[[nodiscard]] std::string ContentAddress(std::span<const std::byte> bytes);

/// Parse and FULLY VALIDATE an untrusted blob (5.5).
///
/// Every index in the returned structure is guaranteed in range, so the caller
/// — and the shader — may use them without further checking. That guarantee is
/// the entire product of this function; if it cannot be made, it returns an
/// error and nothing is parsed.
[[nodiscard]] protocol::Result<Bvh> LoadBvh(std::span<const std::byte> bytes);

}  // namespace p2pgpu::scene
