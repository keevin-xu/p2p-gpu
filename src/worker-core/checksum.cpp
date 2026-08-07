// BLAKE3-64 for the worker. Portable: identical source on both targets, only
// the LIBRARY differs, and that difference is resolved in CMake rather than by
// a conditional here (R2). See checksum.hpp and D-0034.

#include "p2pgpu/worker/checksum.hpp"

#include <blake3.h>

#include <array>
#include <string>

namespace p2pgpu::worker {

std::uint64_t Blake3_64(std::span<const std::byte> bytes) noexcept {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, bytes.data(), bytes.size());
    std::array<std::uint8_t, 8> digest{};
    blake3_hasher_finalize(&h, digest.data(), digest.size());

    // Little-endian assembly, matching the coordinator's copy in session.cpp
    // byte for byte. Not memcpy: this must produce the same number on a
    // big-endian host, and a struct overlay would not.
    std::uint64_t out = 0;
    for (std::size_t i = 0; i < digest.size(); ++i) {
        out |= static_cast<std::uint64_t>(digest[i]) << (8U * i);
    }
    return out;
}

}  // namespace p2pgpu::worker

namespace p2pgpu::worker {

std::string Blake3Hex(std::span<const std::byte> bytes) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, bytes.data(), bytes.size());
    std::array<std::uint8_t, 32> digest{};
    blake3_hasher_finalize(&h, digest.data(), digest.size());

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (const std::uint8_t b : digest) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

}  // namespace p2pgpu::worker
