#pragma once
//
// BLAKE3-64 — the `ResultHeader.checksum` of invariant 9. Step 1.20.
//
// ── WHY THIS IS NOT IN p2pgpu-protocol ───────────────────────────────────
// That library must link NOTHING that vcpkg compiled: the fuzz preset builds it
// with Homebrew clang because Apple Clang has no libFuzzer (D-0015). So the
// protocol layer states the RULE (`CheckPayloadChecksum` takes an
// already-computed hash) and its callers supply the evidence. This is the
// worker's supplier; the coordinator has its own in session.hpp.
//
// ── TWO IMPLEMENTATIONS, ONE VALUE ───────────────────────────────────────
// Native links vcpkg's blake3; the browser links upstream via FetchContent,
// because the vcpkg port hardcodes x86 SIMD and does not build for
// wasm32-emscripten (D-0034 — measured, not assumed). Both are the same
// upstream source and BLAKE3 is specified to be implementation-independent, so
// they cannot disagree. tests/unit/test_checksum.cpp pins the value anyway:
// what a disagreement would look like is "every result from browser workers is
// corrupt", with nothing in the logs pointing at the hash.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace p2pgpu::worker {

/// First 8 bytes of the BLAKE3 digest, assembled little-endian so the value is
/// stable across hosts — it travels on the wire and both ends must agree byte
/// for byte.
[[nodiscard]] std::uint64_t Blake3_64(std::span<const std::byte> bytes) noexcept;

/// Full BLAKE3-256 as lowercase hex — a CONTENT ADDRESS (5.4).
///
/// Must produce exactly the same string as `scene::ContentAddress`, which the
/// coordinator uses to name the same bytes. They are separate functions because
/// they live in libraries with different dependency rules (see the header note
/// above, and D-0069 on why `p2pgpu-scene` links no vcpkg `native` package).
/// `tests/unit/test_scene.cpp` pins them against each other for the D-0034
/// reason: a disagreement would present as "every asset fetch is corrupt", with
/// nothing in any log pointing at the hash.
///
/// 64 characters, lowercase. Case is part of the contract — see
/// `AssetStore::IsWellFormedAddress`.
[[nodiscard]] std::string Blake3Hex(std::span<const std::byte> bytes);

}  // namespace p2pgpu::worker
