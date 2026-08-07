// BVH build, serialization, and the HOSTILE loader — steps 5.3 / 5.5.
// Design and threat model: D-0069.

#include "p2pgpu/scene/bvh.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>

namespace p2pgpu::scene {
namespace {

using protocol::ErrorCode;
using protocol::MakeError;

constexpr float kInf = std::numeric_limits<float>::infinity();

struct Aabb {
    float mn[3]{kInf, kInf, kInf};
    float mx[3]{-kInf, -kInf, -kInf};

    void Extend(const float p[3]) {
        for (int i = 0; i < 3; ++i) {
            mn[i] = std::min(mn[i], p[i]);
            mx[i] = std::max(mx[i], p[i]);
        }
    }
    void Extend(const Aabb& o) {
        for (int i = 0; i < 3; ++i) {
            mn[i] = std::min(mn[i], o.mn[i]);
            mx[i] = std::max(mx[i], o.mx[i]);
        }
    }
    [[nodiscard]] float SurfaceArea() const {
        const float dx = mx[0] - mn[0];
        const float dy = mx[1] - mn[1];
        const float dz = mx[2] - mn[2];
        if (dx < 0.0F || dy < 0.0F || dz < 0.0F) {
            return 0.0F;  // empty
        }
        return 2.0F * (dx * dy + dy * dz + dz * dx);
    }
    [[nodiscard]] int LongestAxis() const {
        const float dx = mx[0] - mn[0];
        const float dy = mx[1] - mn[1];
        const float dz = mx[2] - mn[2];
        if (dx > dy && dx > dz) return 0;
        return dy > dz ? 1 : 2;
    }
};

struct BuildPrim {
    BvhPrim prim;
    Aabb box;
    float centroid[3];
};

Aabb SphereBox(const Sphere& s) {
    Aabb b;
    const float p0[3]{s.center.x - s.radius, s.center.y - s.radius, s.center.z - s.radius};
    const float p1[3]{s.center.x + s.radius, s.center.y + s.radius, s.center.z + s.radius};
    b.Extend(p0);
    b.Extend(p1);
    return b;
}

Aabb TriangleBox(const Triangle& t) {
    Aabb b;
    const float a[3]{t.a.x, t.a.y, t.a.z};
    const float bb[3]{t.b.x, t.b.y, t.b.z};
    const float c[3]{t.c.x, t.c.y, t.c.z};
    b.Extend(a);
    b.Extend(bb);
    b.Extend(c);
    return b;
}

constexpr int kSahBins = 12;
constexpr std::uint32_t kMaxLeafPrims = 4;

/// ── WHY BINNED SAH AND NOT A MEDIAN SPLIT ────────────────────────────────
/// A median split is ~15 lines and renders identical images, so it is the
/// obvious choice. It is rejected because **D-0068's F estimate is dominated by
/// BVH traversal depth**, and the R5 floor — hence `min_iterations`, hence the
/// whole accumulation policy — is derived from F. A median split over a scene
/// with a large ground quad and clustered spheres produces a materially deeper
/// tree, so measuring F on one would overstate the work per sample and let a
/// too-small `min_iterations` look justified.
///
/// In other words the tree quality is not a rendering nicety here; it feeds a
/// number the project's central claim rests on. Same reasoning as 5.2's ground
/// being two triangles rather than one enormous sphere.
struct Builder {
    std::vector<BuildPrim>& items;
    Bvh& out;

    /// ── CHILDREN ARE ALLOCATED ADJACENTLY, AND THAT IS THE FORMAT ────────
    /// `BvhNode` stores ONE child index because the right child is defined to
    /// be `left + 1`. That only holds if both children are emitted before
    /// either subtree is, which is what the two `emplace_back` calls below do.
    ///
    /// The obvious shape — recurse left, recurse right, then record where the
    /// left child landed — silently breaks it: the right child then sits at
    /// `left + sizeof(left subtree)`, so every reader following `left + 1`
    /// walks into the MIDDLE OF THE LEFT SUBTREE. It was written that way
    /// first, and the disagreement between the builder's depth (13) and the
    /// loader's recomputed depth (11) is what exposed it. The image would
    /// still have rendered — just of a tree nobody built.
    void Build(std::uint32_t node_index, std::uint32_t begin, std::uint32_t end,
               std::uint32_t depth) {
        out.max_depth = std::max(out.max_depth, depth + 1);

        Aabb bounds;
        Aabb centroid_bounds;
        for (std::uint32_t i = begin; i < end; ++i) {
            bounds.Extend(items[i].box);
            centroid_bounds.Extend(items[i].centroid);
        }

        const std::uint32_t count = end - begin;
        const auto make_leaf = [&] {
            BvhNode& n = out.nodes[node_index];
            std::copy_n(bounds.mn, 3, n.bmin);
            std::copy_n(bounds.mx, 3, n.bmax);
            n.left_or_first = begin;
            n.count_and_leaf = kLeafFlag | count;
            out.max_leaf_prims = std::max(out.max_leaf_prims, count);
        };

        if (count <= kMaxLeafPrims) {
            make_leaf();
            return;
        }

        const int axis = centroid_bounds.LongestAxis();
        const float lo = centroid_bounds.mn[axis];
        const float hi = centroid_bounds.mx[axis];
        if (!(hi > lo)) {
            // Every centroid coincident on this axis: no split can separate
            // them, and recursing would not terminate.
            make_leaf();
            return;
        }

        std::array<Aabb, kSahBins> bin_box{};
        std::array<std::uint32_t, kSahBins> bin_count{};
        const float scale = static_cast<float>(kSahBins) / (hi - lo);
        const auto bin_of = [&](const BuildPrim& p) {
            const auto b = static_cast<int>((p.centroid[axis] - lo) * scale);
            return std::clamp(b, 0, kSahBins - 1);
        };
        for (std::uint32_t i = begin; i < end; ++i) {
            const int b = bin_of(items[i]);
            bin_box[static_cast<std::size_t>(b)].Extend(items[i].box);
            ++bin_count[static_cast<std::size_t>(b)];
        }

        // Sweep from each side accumulating area x count, then take the
        // cheapest of the 11 candidate planes.
        std::array<float, kSahBins - 1> left_area{};
        std::array<std::uint32_t, kSahBins - 1> left_count{};
        Aabb acc;
        std::uint32_t acc_n = 0;
        for (int i = 0; i < kSahBins - 1; ++i) {
            acc.Extend(bin_box[static_cast<std::size_t>(i)]);
            acc_n += bin_count[static_cast<std::size_t>(i)];
            left_area[static_cast<std::size_t>(i)] = acc.SurfaceArea();
            left_count[static_cast<std::size_t>(i)] = acc_n;
        }
        float best_cost = kInf;
        int best_plane = -1;
        acc = Aabb{};
        acc_n = 0;
        for (int i = kSahBins - 1; i >= 1; --i) {
            acc.Extend(bin_box[static_cast<std::size_t>(i)]);
            acc_n += bin_count[static_cast<std::size_t>(i)];
            const auto li = static_cast<std::size_t>(i - 1);
            const float cost = left_area[li] * static_cast<float>(left_count[li]) +
                               acc.SurfaceArea() * static_cast<float>(acc_n);
            if (cost < best_cost && left_count[li] > 0 && acc_n > 0) {
                best_cost = cost;
                best_plane = i;
            }
        }

        // A leaf can be cheaper than any split. Without this the tree keeps
        // subdividing clusters that gain nothing and grows depth for free.
        const float leaf_cost = bounds.SurfaceArea() * static_cast<float>(count);
        if (best_plane < 0 || leaf_cost <= best_cost) {
            make_leaf();
            return;
        }

        const auto mid = std::partition(items.begin() + begin, items.begin() + end,
                                        [&](const BuildPrim& p) {
                                            return bin_of(p) < best_plane;
                                        });
        const auto split = static_cast<std::uint32_t>(mid - items.begin());
        if (split == begin || split == end) {
            make_leaf();
            return;
        }

        // BOTH children first, adjacent — see the note on this function.
        const auto left = static_cast<std::uint32_t>(out.nodes.size());
        out.nodes.emplace_back();
        out.nodes.emplace_back();
        {
            // Re-index AFTER the emplace_backs: a reference taken before them
            // would dangle on reallocation.
            BvhNode& n = out.nodes[node_index];
            std::copy_n(bounds.mn, 3, n.bmin);
            std::copy_n(bounds.mx, 3, n.bmax);
            n.left_or_first = left;
            n.count_and_leaf = 0;
        }
        Build(left, begin, split, depth + 1);
        Build(left + 1, split, end, depth + 1);
    }
};

}  // namespace

Bvh BuildBvh(const Scene& scene) {
    Bvh bvh;
    bvh.materials.reserve(scene.materials.size());
    for (const auto& m : scene.materials) {
        BvhMaterial bm{};
        bm.albedo[0] = m.albedo.x;
        bm.albedo[1] = m.albedo.y;
        bm.albedo[2] = m.albedo.z;
        bm.fuzz = m.fuzz;
        bm.kind = static_cast<std::uint32_t>(m.kind);
        bvh.materials.push_back(bm);
    }

    std::vector<BuildPrim> items;
    items.reserve(scene.primitive_count());

    for (const auto& s : scene.spheres) {
        BuildPrim bp{};
        bp.prim.kind = kPrimSphere;
        bp.prim.material = s.material;
        bp.prim.a[0] = s.center.x;
        bp.prim.a[1] = s.center.y;
        bp.prim.a[2] = s.center.z;
        bp.prim.radius = s.radius;
        bp.box = SphereBox(s);
        bp.centroid[0] = s.center.x;
        bp.centroid[1] = s.center.y;
        bp.centroid[2] = s.center.z;
        items.push_back(bp);
    }
    for (const auto& t : scene.triangles) {
        BuildPrim bp{};
        bp.prim.kind = kPrimTriangle;
        bp.prim.material = t.material;
        bp.prim.a[0] = t.a.x; bp.prim.a[1] = t.a.y; bp.prim.a[2] = t.a.z;
        bp.prim.b[0] = t.b.x; bp.prim.b[1] = t.b.y; bp.prim.b[2] = t.b.z;
        bp.prim.c[0] = t.c.x; bp.prim.c[1] = t.c.y; bp.prim.c[2] = t.c.z;
        bp.box = TriangleBox(t);
        for (int i = 0; i < 3; ++i) {
            bp.centroid[i] = 0.5F * (bp.box.mn[i] + bp.box.mx[i]);
        }
        items.push_back(bp);
    }

    if (items.empty()) {
        return bvh;
    }

    bvh.nodes.reserve(items.size() * 2);
    bvh.nodes.emplace_back();  // root
    Builder builder{items, bvh};
    builder.Build(0, 0, static_cast<std::uint32_t>(items.size()), 0);

    // Primitives are emitted in the order the partitioning left them, so a
    // leaf's range is contiguous.
    bvh.prims.reserve(items.size());
    for (const auto& it : items) {
        bvh.prims.push_back(it.prim);
    }
    return bvh;
}

std::vector<std::byte> SerializeBvh(const Bvh& bvh) {
    const auto node_bytes = bvh.nodes.size() * sizeof(BvhNode);
    const auto prim_bytes = bvh.prims.size() * sizeof(BvhPrim);
    const auto mat_bytes = bvh.materials.size() * sizeof(BvhMaterial);

    BvhHeader h{};
    h.magic = kBvhMagic;
    h.version = kBvhVersion;
    h.node_count = static_cast<std::uint32_t>(bvh.nodes.size());
    h.prim_count = static_cast<std::uint32_t>(bvh.prims.size());
    h.material_count = static_cast<std::uint32_t>(bvh.materials.size());
    h.node_offset = sizeof(BvhHeader);
    h.prim_offset = h.node_offset + static_cast<std::uint32_t>(node_bytes);
    h.material_offset = h.prim_offset + static_cast<std::uint32_t>(prim_bytes);
    h.total_bytes = h.material_offset + static_cast<std::uint32_t>(mat_bytes);
    // reserved[] stays zero — the loader rejects anything else, which keeps the
    // field genuinely free for a later version (D-0027's pattern).

    std::vector<std::byte> out(h.total_bytes);
    const auto put = [&out](std::size_t at, const void* src, std::size_t n) {
        if (n != 0) {
            std::memcpy(out.data() + at, src, n);
        }
    };
    put(0, &h, sizeof(h));
    put(h.node_offset, bvh.nodes.data(), node_bytes);
    put(h.prim_offset, bvh.prims.data(), prim_bytes);
    put(h.material_offset, bvh.materials.data(), mat_bytes);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────
// THE HOSTILE PATH (5.5). Everything above builds our own data; everything
// below assumes an adversary wrote the bytes.
//
// Messages are STATIC LITERALS, never composed. `protocol::Error::message` is a
// non-owning string_view documented "static text only", so `"node " +
// std::to_string(i)` would hand it a view of a temporary that dies at the end
// of the expression — a dangling read in the one function whose entire job is
// to be safe against hostile input. Each literal names WHICH check failed,
// which is what a log actually needs.
// ─────────────────────────────────────────────────────────────────────────

protocol::Result<Bvh> LoadBvh(std::span<const std::byte> bytes) {
    if (bytes.size() < sizeof(BvhHeader)) {
        return MakeError(ErrorCode::Internal, "bvh: shorter than its header");
    }

    // Copied out, never read in place: the span has no alignment guarantee, and
    // reading a struct through a misaligned pointer is UB that ran correctly on
    // arm64 and x86 for weeks last time (D-0027).
    BvhHeader h{};
    std::memcpy(&h, bytes.data(), sizeof(h));

    if (h.magic != kBvhMagic) {
        return MakeError(ErrorCode::Internal, "bvh: bad magic");
    }
    if (h.version != kBvhVersion) {
        return MakeError(ErrorCode::Internal,
                         "bvh: unsupported version");
    }
    for (const std::uint32_t r : h.reserved) {
        if (r != 0) {
            return MakeError(ErrorCode::Internal, "bvh: reserved field is non-zero");
        }
    }

    // Declared sizes BEFORE any allocation (R11): FlatBuffers would not have
    // saved us from a 4 GB declaration and there is no FlatBuffers here at all.
    if (h.node_count == 0 || h.node_count > kMaxBvhNodes) {
        return MakeError(ErrorCode::Internal,
                         "bvh: node_count is zero or over kMaxBvhNodes");
    }
    if (h.prim_count == 0 || h.prim_count > kMaxBvhPrims) {
        return MakeError(ErrorCode::Internal,
                         "bvh: prim_count is zero or over kMaxBvhPrims");
    }
    if (h.material_count == 0 || h.material_count > kMaxBvhMaterials) {
        return MakeError(ErrorCode::Internal,
                         "bvh: material_count is zero or over kMaxBvhMaterials");
    }

    // Section arithmetic in 64-bit so it cannot wrap. This is exactly the bug
    // `fuzz_asset` exists to catch on the chunk path (4.13): unsigned
    // multiplication wraps by definition, no sanitizer fires, and the result is
    // a wrong VALUE rather than a crash.
    const auto n64 = static_cast<std::uint64_t>(h.node_count) * sizeof(BvhNode);
    const auto p64 = static_cast<std::uint64_t>(h.prim_count) * sizeof(BvhPrim);
    const auto m64 = static_cast<std::uint64_t>(h.material_count) * sizeof(BvhMaterial);

    const auto want_node_off = static_cast<std::uint64_t>(sizeof(BvhHeader));
    const std::uint64_t want_prim_off = want_node_off + n64;
    const std::uint64_t want_mat_off = want_prim_off + p64;
    const std::uint64_t want_total = want_mat_off + m64;

    // Offsets are DECLARED and also DERIVABLE, and both must agree. A blob
    // whose header says one thing while its counts imply another is malformed
    // regardless of which the reader would have believed.
    if (h.node_offset != want_node_off || h.prim_offset != want_prim_off ||
        h.material_offset != want_mat_off || h.total_bytes != want_total) {
        return MakeError(ErrorCode::Internal,
                         "bvh: section offsets disagree with the declared counts");
    }
    if (bytes.size() != want_total) {
        return MakeError(ErrorCode::Internal,
                         "bvh: buffer length disagrees with the declared total_bytes");
    }

    Bvh bvh;
    bvh.nodes.resize(h.node_count);
    bvh.prims.resize(h.prim_count);
    bvh.materials.resize(h.material_count);
    std::memcpy(bvh.nodes.data(), bytes.data() + h.node_offset, n64);
    std::memcpy(bvh.prims.data(), bytes.data() + h.prim_offset, p64);
    std::memcpy(bvh.materials.data(), bytes.data() + h.material_offset, m64);

    // ── Every index, once, on the CPU ────────────────────────────────────
    // After this loop the shader may index freely. That guarantee is the whole
    // product of this function.
    for (std::uint32_t i = 0; i < h.node_count; ++i) {
        const BvhNode& n = bvh.nodes[i];
        const bool leaf = (n.count_and_leaf & kLeafFlag) != 0;
        const std::uint32_t count = n.count_and_leaf & ~kLeafFlag;
        if (leaf) {
            const auto first = static_cast<std::uint64_t>(n.left_or_first);
            if (count == 0 || first + count > h.prim_count) {
                return MakeError(ErrorCode::Internal,
                                 "bvh: a leaf spans outside the primitive array");
            }
            bvh.max_leaf_prims = std::max(bvh.max_leaf_prims, count);
        } else {
            if (count != 0) {
                return MakeError(ErrorCode::Internal,
                                 "bvh: an internal node declares a primitive count");
            }
            // STRICTLY GREATER, and this is what makes cycles unrepresentable
            // rather than merely unlikely. A cycle would spin the GPU traversal
            // loop forever, and R4's chunking cannot interrupt a running shader
            // — the worker would hang with no path back (D-0069).
            // 64-BIT, and this is not defensive style — it is the bug.
            // `n.left_or_first + 1` in 32 bits WRAPS at 0xFFFFFFFF, so the
            // comparison became `0 >= node_count`, which is false, and a child
            // index of 0xFFFFFFFF passed validation and was then used to index
            // `in_degree`. Found by `fuzz_bvh` in a 120-second campaign.
            //
            // THIRD TIME THIS CLASS HAS APPEARED: unsigned arithmetic wraps by
            // definition, so it is not UB, no sanitizer fires at the wrap, and
            // a wrong VALUE flows onward to be trusted. `ChunkOffset` (4.13) and
            // the section arithmetic above are the other two. Any expression
            // combining an attacker-supplied u32 with a bound belongs in 64-bit.
            const auto left = static_cast<std::uint64_t>(n.left_or_first);
            if (left <= i || left + 1 >= h.node_count) {
                return MakeError(ErrorCode::Internal,
                                 "bvh: a child index is out of range or does not increase");
            }
        }
    }
    for (std::uint32_t i = 0; i < h.prim_count; ++i) {
        const BvhPrim& p = bvh.prims[i];
        if (p.kind != kPrimSphere && p.kind != kPrimTriangle) {
            return MakeError(ErrorCode::Internal,
                             "bvh: a primitive has an unknown kind");
        }
        if (p.material >= h.material_count) {
            return MakeError(ErrorCode::Internal,
                             "bvh: a primitive references an out-of-range material");
        }
    }
    for (std::uint32_t i = 0; i < h.material_count; ++i) {
        if (bvh.materials[i].kind > static_cast<std::uint32_t>(MaterialKind::Emissive)) {
            return MakeError(ErrorCode::Internal,
                             "bvh: a material has an unknown kind");
        }
    }

    // ── IT MUST BE A TREE, NOT MERELY ACYCLIC ────────────────────────────
    // Strictly-increasing child indices make a CYCLE unrepresentable, and that
    // was originally assumed to be enough. It is not: nothing above stops two
    // parents naming the SAME child. Indices still increase, traversal still
    // terminates, no read goes out of bounds, and every check above passes.
    //
    // What it costs is exponential: a diamond-shaped DAG where nodes share
    // children makes a traversal visit 2^depth paths. That is a GPU hang by a
    // different route than a cycle, and R4's chunking cannot interrupt this one
    // either — a shader is not preemptible once dispatched.
    //
    // FOUND BY `fuzz_bvh`, in the first 60-second campaign against the finished
    // loader, by the contract oracle rather than by a sanitizer — there is
    // nothing here for ASan to see. In-degree exactly 0 for the root and at
    // most 1 for everything else is what makes it a tree.
    std::vector<std::uint8_t> in_degree(h.node_count, 0);
    for (std::uint32_t i = 0; i < h.node_count; ++i) {
        const BvhNode& n = bvh.nodes[i];
        if ((n.count_and_leaf & kLeafFlag) != 0) {
            continue;
        }
        for (const std::uint32_t child : {n.left_or_first, n.left_or_first + 1}) {
            if (child == 0 || in_degree[child] != 0) {
                return MakeError(ErrorCode::Internal,
                                 "bvh: a node has two parents, or the root is a "
                                 "child — the graph is not a tree");
            }
            in_degree[child] = 1;
        }
    }
    // Every node except the root must be reachable. An unreferenced node is not
    // dangerous, but it means the blob carries something the tree does not use,
    // which is a decoded-size discrepancy an attacker chose — reject rather than
    // wonder about it later.
    for (std::uint32_t i = 1; i < h.node_count; ++i) {
        if (in_degree[i] == 0) {
            return MakeError(ErrorCode::Internal,
                             "bvh: a node is unreachable from the root");
        }
    }

    // Depth is recomputed rather than trusted from the blob — it is a REPORTED
    // statistic feeding D-0068's F, and a number an attacker can set is not a
    // measurement. Iterative, because a hostile tree could be node_count deep
    // and recursion would blow the stack before any check fired.
    std::vector<std::uint32_t> depth(h.node_count, 0);
    depth[0] = 1;
    for (std::uint32_t i = 0; i < h.node_count; ++i) {
        const BvhNode& n = bvh.nodes[i];
        bvh.max_depth = std::max(bvh.max_depth, depth[i]);
        if ((n.count_and_leaf & kLeafFlag) == 0) {
            // Safe: children were bounds-checked above, and are > i, so their
            // depths are still unwritten when we get here.
            depth[n.left_or_first] = depth[i] + 1;
            depth[n.left_or_first + 1] = depth[i] + 1;
        }
    }

    return bvh;
}

}  // namespace p2pgpu::scene
