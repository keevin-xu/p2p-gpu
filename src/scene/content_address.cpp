// BLAKE3 content addressing for the BVH asset — step 5.3.
//
// ── WHY THIS IS ITS OWN TRANSLATION UNIT ─────────────────────────────────
// BLAKE3 comes from the vcpkg `native` feature, which the fuzz presets do not
// install (D-0015). `p2pgpu-scene` must stay linkable in a fuzz configure
// because `LoadBvh` — the hostile parser 5.5 fuzzes — lives there. Keeping the
// one BLAKE3 call in a separate target is what lets that hold.
//
// This is the 1.29 break stated as a structure rather than a comment: nothing
// prevents someone adding `#include <blake3.h>` to bvh.cpp, but the split makes
// it obvious why they should not.

#include <array>
#include <string>

#include <blake3.h>

#include "p2pgpu/scene/bvh.hpp"

namespace p2pgpu::scene {

std::string ContentAddress(std::span<const std::byte> bytes) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, bytes.data(), bytes.size());
    std::array<std::uint8_t, 32> digest{};
    blake3_hasher_finalize(&hasher, digest.data(), digest.size());

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (const std::uint8_t b : digest) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

}  // namespace p2pgpu::scene
