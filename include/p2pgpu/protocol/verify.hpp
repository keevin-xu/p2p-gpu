#pragma once
//
// THE TRUST BOUNDARY — step 1.3.
//
// This header declares the ONLY sanctioned path from network bytes to typed
// data (rule R11). Everything else in the codebase calls in here; nothing else
// parses a frame itself, and there is no "internal" path that skips it.
//
// The coordinator's entire input surface is attacker-controlled: anyone can
// connect as a worker — that is the product, not a weakness.
//
// ── WHAT THE VERIFIER DOES AND DOES NOT BUY ──────────────────────────────
// It guarantees a buffer can be READ without out-of-bounds access. It does not
// make the contents true. Measured in step 1.1: flipping a bit inside a padding
// or scalar byte still verifies clean. Semantic limits are the invariants
// (invariants.hpp); payload integrity is the BLAKE3-64 checksum.
//
// Verification is a memory-safety property, not an integrity one. Conflating
// the two is how you end up trusting a verified buffer's numbers.

#include <cstddef>
#include <array>
#include <bit>
#include <optional>
#include <type_traits>
#include <span>

#include "p2pgpu/p2pgpu_generated.h"
#include "p2pgpu/protocol/error.hpp"
#include "p2pgpu/protocol/frame.hpp"

namespace p2pgpu::protocol {

/// A frame that has completed the full docs/PROTOCOL.md §1 sequence.
///
/// Holding one of these is the proof that verification happened: there is no
/// public constructor path that produces it without going through
/// VerifyFrame(), so "did this get verified?" is answered by the type rather
/// than by reviewer memory.
class VerifiedFrame {
public:
    /// Never null once VerifyFrame has returned success.
    [[nodiscard]] const wire::Envelope* envelope() const noexcept { return envelope_; }

    /// Raw, UNVERIFIED result bytes. Deliberately outside the FlatBuffer: its
    /// interpretation is fully determined by OutputSpec, and verifying 8 MiB of
    /// GPU output buys nothing (D-0009). Bounds are checked; contents are not
    /// trusted until the checksum is confirmed (invariant 9).
    [[nodiscard]] std::span<const std::byte> payload() const noexcept { return payload_; }

    [[nodiscard]] wire::Body body_type() const noexcept { return envelope_->body_type(); }

private:
    friend Result<VerifiedFrame> VerifyFrame(std::span<const std::byte>) noexcept;
    VerifiedFrame(const wire::Envelope* e, std::span<const std::byte> p) noexcept
        : envelope_(e), payload_(p) {}

    const wire::Envelope* envelope_ = nullptr;
    std::span<const std::byte> payload_{};
};

/// THE ONE ENTRY POINT. Bytes in, typed message out.
///
/// Performs the exact sequence in docs/PROTOCOL.md §1 — framing checks
/// (SplitFrame), then flatbuffers::Verifier bounded by kMaxVerifyDepth and
/// kMaxVerifyTables, and only then may a schema field be read.
///
/// **Deviating from this sequence is a defect even if it appears to work.**
///
/// The returned spans alias `frame`; the caller must keep that buffer alive for
/// as long as the VerifiedFrame is used. Nothing is copied — that is the point
/// of a zero-copy format, and it is also the lifetime hazard to respect.
[[nodiscard]] Result<VerifiedFrame> VerifyFrame(std::span<const std::byte> frame) noexcept;

/// Data-plane counterpart (Phase 6). Peer-supplied bytes are the least
/// trustworthy input in the system — no coordinator mediation at all.
///
/// flatc emits VerifyXBuffer only for the declared root_type (Envelope), so
/// this verifies AssetMsg with `VerifyBuffer<AssetMsg>` directly, which is
/// exactly what the generated helper does internally.
[[nodiscard]] Result<const wire::AssetMsg*> VerifyAssetMsg(
    std::span<const std::byte> bytes) noexcept;

/// Read a trivially-copyable `T` out of untrusted bytes (R11).
///
/// The sanctioned alternative to `memcpy(&t, span.data(), sizeof(t))`, which
/// R11 bans at the boundary because it reads `sizeof(T)` bytes whether or not
/// they are there — the Heartbleed shape. This checks first and returns
/// nullopt, so a short buffer cannot become a struct full of adjacent memory.
///
/// Copies through `std::array` + `std::bit_cast` rather than a pointer cast:
/// the same C++20 spelling D-0027 settled on, and the reason the alignment
/// bugs could be fixed without reintroducing a cast.
template <typename T>
[[nodiscard]] std::optional<T> ReadStruct(std::span<const std::byte> bytes) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    if (bytes.size() < sizeof(T)) {
        return std::nullopt;
    }
    std::array<std::byte, sizeof(T)> raw{};
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        raw[i] = bytes[i];
    }
    return std::bit_cast<T>(raw);
}

}  // namespace p2pgpu::protocol
