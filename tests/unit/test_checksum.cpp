// Invariant 9's checksum, pinned — step 1.20 / D-0034.
//
// TWO IMPLEMENTATIONS EXIST IN THIS SYSTEM. The coordinator links vcpkg's
// blake3; the browser worker links upstream BLAKE3 via FetchContent, because
// the vcpkg port hardcodes x86 SIMD and does not build for wasm32-emscripten
// (measured, not assumed — see D-0034).
//
// BLAKE3 is specified to be implementation-independent and both are the same
// upstream source, so these can only agree. That is exactly why pinning them is
// cheap — and what a disagreement would look like is severe and unsearchable:
// "every result from browser workers is corrupt", with nothing in any log
// pointing at the hash.
//
// The digests below are the OFFICIAL BLAKE3 test vectors (input byte i = i%251),
// not values captured from our own build — a self-captured golden proves only
// that we still compute what we computed last week.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "p2pgpu/coordinator/session.hpp"
#include "p2pgpu/worker/checksum.hpp"

namespace {

/// The official vectors' input: byte i is i % 251.
std::vector<std::byte> Vector(std::size_t n) {
    std::vector<std::byte> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::byte>(i % 251);
    }
    return out;
}

struct Case {
    std::size_t length;
    std::uint64_t expected;  ///< first 8 digest bytes, little-endian
};

// From the BLAKE3 reference test_vectors.json. E.g. the empty input hashes to
// af1349b9f5f9a1a6... and the first eight bytes read little-endian give
// 0xa6a1f9f5b94913af.
constexpr Case kCases[] = {
    {0, 0xa6a1f9f5b94913afULL},
    {1, 0xf1611bf1dfde3a2dULL},
    {2, 0x310bcf92bb15707bULL},
    {3, 0x0a56b58a7a4dbee1ULL},
    {64, 0xd45c4aea4171ed4eULL},
    {1024, 0x06a495f039472142ULL},
};

}  // namespace

TEST_CASE("worker and coordinator agree with the BLAKE3 test vectors", "[checksum]") {
    for (const auto& c : kCases) {
        const auto input = Vector(c.length);

        INFO("length " << c.length);
        // The worker computes the checksum...
        CHECK(p2pgpu::worker::Blake3_64(input) == c.expected);
        // ...and the coordinator verifies it. Different libraries, same number,
        // or every submission from that worker is rejected as corrupt.
        CHECK(p2pgpu::coordinator::Blake3_64(input) == c.expected);
    }
}

TEST_CASE("the checksum is little-endian assembled, not memcpy'd", "[checksum]") {
    // A struct overlay or memcpy of the first 8 digest bytes would produce this
    // same value on x86 and arm64 and a byte-reversed one on a big-endian host.
    // The checksum travels on the wire, so the assembly must be explicit —
    // pinning the empty-input case is what would catch a "simplification" back
    // to memcpy on any host we actually test on.
    const std::vector<std::byte> empty;
    CHECK(p2pgpu::worker::Blake3_64(empty) == 0xa6a1f9f5b94913afULL);
    CHECK(p2pgpu::worker::Blake3_64(empty) == p2pgpu::coordinator::Blake3_64(empty));
}
