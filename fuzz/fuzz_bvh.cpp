// libFuzzer harness for `scene::LoadBvh` — step 5.5.
//
// ── WHY THIS TARGET EXISTS AT ALL ────────────────────────────────────────
// `LoadBvh` is what stands in for `flatbuffers::Verifier` on the asset path
// (D-0069). Every other network input in this project is checked by a Verifier
// that someone else wrote and that libFuzzer has already hammered through
// `fuzz_protocol`. This one we wrote, and it guards the most dangerous input in
// the system: integers that become GPU array indices inside a traversal loop.
//
// Fuzzed BEFORE Phase 6 gives it hostile callers, on exactly the 4.13 reasoning
// — a crasher is cheap now and expensive once a data plane depends on the path.
//
// ── AN ORACLE, NOT A CRASH CHECK (the 4.13 lesson) ───────────────────────
// `fuzz_asset`'s first version consumed fields and watched for crashes, and it
// replayed CLEAN against a deliberately broken `ChunkOffset` — because unsigned
// multiplication wraps by definition, so no sanitizer fires and the result is a
// wrong VALUE rather than a fault.
//
// The same trap is here in a worse form. A `LoadBvh` that returned success on a
// malformed tree would not crash on the CPU either: the damage happens later,
// in a shader, where WGSL bounds-CLAMPS rather than faulting and the only
// symptom is a plausible wrong image.
//
// So this harness asserts the CONTRACT instead:
//
//     If LoadBvh returns success, EVERY index it returns is in range and the
//     node graph is acyclic.
//
// That is the exact promise the header makes to the shader. Re-derived here
// independently, so a check deleted from the loader is caught rather than
// mirrored.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

#include "p2pgpu/scene/bvh.hpp"

namespace {

using namespace p2pgpu::scene;

/// Out-of-line so the abort is a plain call rather than an inlined trap
/// sequence — the inlined form produced `ld: invalid r_symbolnum` on arm64
/// with Homebrew LLVM (D-0015's toolchain friction, same as `fuzz_asset`).
[[noreturn]] __attribute__((noinline)) void OracleFailed(const char* what) {
    std::fprintf(stderr, "ORACLE VIOLATED: %s\n", what);
    std::fflush(nullptr);
    std::abort();
}

/// The contract, re-derived. Deliberately does NOT call into the loader's own
/// helpers: a check that shares code with the thing it audits agrees with it by
/// construction, including when both are wrong.
void AssertContractHolds(const Bvh& bvh) {
    const auto nodes = static_cast<std::uint64_t>(bvh.nodes.size());
    const auto prims = static_cast<std::uint64_t>(bvh.prims.size());
    const auto mats = static_cast<std::uint64_t>(bvh.materials.size());

    if (nodes == 0 || prims == 0 || mats == 0) {
        OracleFailed("accepted a BVH with an empty section");
    }

    for (std::uint64_t i = 0; i < nodes; ++i) {
        const BvhNode& n = bvh.nodes[i];
        const bool leaf = (n.count_and_leaf & kLeafFlag) != 0;
        const std::uint64_t count = n.count_and_leaf & ~kLeafFlag;
        if (leaf) {
            // A leaf range must lie wholly inside the primitive array. 64-bit
            // arithmetic on purpose: `first + count` in 32 bits is where the
            // wrap the loader must prevent would hide.
            if (count == 0) {
                OracleFailed("accepted an empty leaf");
            }
            if (static_cast<std::uint64_t>(n.left_or_first) + count > prims) {
                OracleFailed("accepted a leaf spanning past the primitive array");
            }
        } else {
            if (count != 0) {
                OracleFailed("accepted an internal node with a primitive count");
            }
            // STRICTLY increasing children is what makes a cycle
            // unrepresentable. A cycle would spin the GPU traversal loop
            // forever, and R4's chunking cannot interrupt a running shader —
            // the worker would hang with no way back (D-0069).
            const auto left = static_cast<std::uint64_t>(n.left_or_first);
            if (left <= i) {
                OracleFailed("accepted a non-increasing child index (cycle possible)");
            }
            if (left + 1 >= nodes) {
                OracleFailed("accepted a child index past the node array");
            }
        }
    }

    for (std::uint64_t i = 0; i < prims; ++i) {
        const BvhPrim& p = bvh.prims[i];
        if (p.kind != kPrimSphere && p.kind != kPrimTriangle) {
            OracleFailed("accepted a primitive with an unknown kind");
        }
        if (static_cast<std::uint64_t>(p.material) >= mats) {
            OracleFailed("accepted a primitive referencing a missing material");
        }
    }

    // Independently walk the graph. Reachability is not something the loader
    // checks directly, so this asserts the stronger property the shader relies
    // on: starting at the root and following children terminates.
    std::vector<std::uint8_t> seen(bvh.nodes.size(), 0);
    std::vector<std::uint32_t> stack{0};
    std::uint64_t visits = 0;
    while (!stack.empty()) {
        const std::uint32_t at = stack.back();
        stack.pop_back();
        if (++visits > nodes) {
            OracleFailed("traversal visited more nodes than exist (cycle)");
        }
        if (seen[at] != 0) {
            OracleFailed("a node is reachable by two paths (not a tree)");
        }
        seen[at] = 1;
        const BvhNode& n = bvh.nodes[at];
        if ((n.count_and_leaf & kLeafFlag) == 0) {
            stack.push_back(n.left_or_first);
            stack.push_back(n.left_or_first + 1);
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> bytes{
        reinterpret_cast<const std::byte*>(data), size};

    auto loaded = LoadBvh(bytes);
    if (!loaded) {
        // Rejection is the expected outcome for almost every input, and it is
        // never a failure — the loader's job is to reject.
        return 0;
    }
    AssertContractHolds(*loaded);
    return 0;
}
