// Seed corpus for `fuzz_bvh` — step 5.5.
//
// ── WHY SEEDS ARE NOT OPTIONAL HERE ──────────────────────────────────────
// The loader rejects anything without a 64-byte header carrying the right
// magic, a matching version, seven zero words, and section offsets that agree
// with three counts. A mutation-only fuzzer starting from nothing will never
// construct that by chance, so it would spend a billion executions bouncing off
// the first `if` and report a clean run — the D-0067 shape again, a check that
// cannot fail because it is never reached.
//
// So: emit VALID trees, then emit the boundary cases by hand. Mutation from a
// valid seed is what actually explores the checks past the header.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "p2pgpu/scene/bvh.hpp"
#include "p2pgpu/scene/scene.hpp"

namespace {

using namespace p2pgpu::scene;

void Write(const std::filesystem::path& dir, const std::string& name,
           const std::vector<std::byte>& bytes) {
    std::ofstream f(dir / name, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

void SetU32(std::vector<std::byte>& b, std::size_t at, std::uint32_t v) {
    if (at + 4 <= b.size()) {
        std::memcpy(b.data() + at, &v, sizeof(v));
    }
}

std::string SceneText(int spheres) {
    std::string s =
        "version 1\n"
        "camera origin 0 1 4 target 0 0 0 up 0 1 0 vfov_deg 40\n"
        "material 0 lambertian 0.5 0.5 0.5\n"
        "material 1 metal 0.9 0.9 0.9 0.1\n"
        "material 2 emissive 5 5 5\n"
        "tri -9 0 -9  9 0 -9  9 0 9  0\n"
        "tri -9 0 -9  9 0 9  -9 0 9  0\n";
    for (int i = 0; i < spheres; ++i) {
        // Deterministic, spread out, no overlap — the corpus must be stable
        // across runs or every regeneration churns the committed seeds.
        const float x = static_cast<float>((i % 7) - 3) * 1.3F;
        const float z = static_cast<float>((i / 7) % 7 - 3) * 1.3F;
        s += "sphere " + std::to_string(x) + " 0.4 " + std::to_string(z) +
             " 0.4 " + std::to_string(i % 3) + "\n";
    }
    return s;
}

std::vector<std::byte> Valid(int spheres) {
    auto sc = ParseScene(SceneText(spheres));
    if (!sc) {
        std::fprintf(stderr, "corpus scene failed to parse\n");
        std::abort();
    }
    return SerializeBvh(BuildBvh(*sc));
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path dir =
        argc > 1 ? argv[1] : "fuzz/corpus/bvh";
    std::filesystem::create_directories(dir);

    // Trees of a few shapes: one leaf, a shallow split, and something deep
    // enough to have interior nodes worth corrupting.
    const auto tiny = Valid(1);
    const auto small = Valid(8);
    const auto deep = Valid(120);
    Write(dir, "valid_tiny.bin", tiny);
    Write(dir, "valid_small.bin", small);
    Write(dir, "valid_deep.bin", deep);

    // ── Boundary cases a mutator finds slowly ────────────────────────────
    // Each is one edit away from valid, aimed at a specific check. Mutation
    // will wander from here into combinations no one thought to write down.

    constexpr std::size_t kNodeCount = 8;
    constexpr std::size_t kPrimCount = 12;
    constexpr std::size_t kMaterialCount = 16;
    constexpr std::size_t kNodeOffset = 20;
    constexpr std::size_t kTotalBytes = 32;
    constexpr std::size_t kReserved0 = 36;

    auto b = deep;
    SetU32(b, 0, 0);                       // magic cleared
    Write(dir, "bad_magic.bin", b);

    b = deep;
    SetU32(b, 4, 2);                       // future version
    Write(dir, "bad_version.bin", b);

    b = deep;
    SetU32(b, kReserved0, 1);              // reserved must stay zero
    Write(dir, "reserved_set.bin", b);

    b = deep;
    SetU32(b, kNodeCount, 0xFFFFFFFFU);    // declared size over the cap
    Write(dir, "huge_node_count.bin", b);

    b = deep;
    SetU32(b, kNodeCount, 1U << 27);       // count x stride wraps 32 bits
    Write(dir, "wrapping_node_count.bin", b);

    b = deep;
    SetU32(b, kPrimCount, 0);              // empty section
    Write(dir, "zero_prims.bin", b);

    b = deep;
    SetU32(b, kNodeOffset, 0);             // offsets disagree with counts
    Write(dir, "offset_mismatch.bin", b);

    b = deep;
    SetU32(b, kTotalBytes, static_cast<std::uint32_t>(deep.size() + 64));
    Write(dir, "short_buffer.bin", b);

    b = deep;
    b.resize(64);                          // header only, no sections
    Write(dir, "header_only.bin", b);

    b = deep;
    b.resize(63);                          // one byte short of a header
    Write(dir, "truncated_header.bin", b);

    Write(dir, "empty.bin", {});

    std::printf("wrote seeds to %s\n", dir.string().c_str());
    return 0;
}
