#pragma once
//
// The 12-byte frame header (docs/PROTOCOL.md §1) — step 1.2.
//
// This is the ONE piece of hand-written binary parsing in the project. D-0009
// chose FlatBuffers precisely so that everything past this header is
// schema-driven; the remaining hand-written surface is small enough to fuzz
// exhaustively, which step 1.5 does.
//
//   offset  size  field
//   [0]      4    magic          0x50324750 ("P2GP")
//   [4]      2    protocol_ver   u16 LE
//   [6]      2    flags          u16 LE  (bit 0: payload_follows)
//   [8]      4    fb_len         u32 LE
//   [12]     4    reserved       MUST be zero; rejected otherwise
//   [16]     N    fb_bytes       the FlatBuffers Envelope
//   [16+N]   M    payload        opaque result bytes, iff flags bit 0
//
// The header is 16 bytes so that `frame + kHeaderBytes` stays 8-byte aligned,
// which the Envelope requires (D-0027). The reserved word is not padding for
// its own sake — it is the alignment, and it doubles as a zero-cost extension
// point that would otherwise need a version bump.
//
// R11 applies in full: no pointer arithmetic, no memcpy, no reinterpret_cast,
// no C arrays. std::span only, and every length checked before it is used.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "p2pgpu/protocol/error.hpp"
#include "p2pgpu/protocol/limits.hpp"

namespace p2pgpu::protocol {

enum class FrameFlags : std::uint16_t {
    kNone = 0,
    kPayloadFollows = 1U << 0,
};

struct Header {
    std::uint32_t magic = 0;
    std::uint16_t protocol_ver = 0;
    std::uint16_t flags = 0;
    std::uint32_t fb_len = 0;
    /// MUST be zero. Rejecting non-zero keeps the field genuinely free for a
    /// future use: a peer that scribbles in it today would otherwise become a
    /// compatibility constraint tomorrow.
    std::uint32_t reserved = 0;

    [[nodiscard]] constexpr bool payload_follows() const noexcept {
        return (flags & static_cast<std::uint16_t>(FrameFlags::kPayloadFollows)) != 0;
    }
};

/// Decode the fixed header. **Parses only — validates nothing.**
///
/// Returns nullopt iff `bytes` is not exactly header-sized. Magic, version, and
/// length checks are the caller's, in the order docs/PROTOCOL.md §1 specifies:
/// that order is what keeps `fb_len` bounded before it is used in arithmetic.
///
/// Splitting parse from validate keeps this function total — every 12-byte
/// input has a defined result — which is what makes it cheap to fuzz.
[[nodiscard]] std::optional<Header> ParseHeader(std::span<const std::byte> bytes) noexcept;

/// Write a header into a 12-byte destination. Used by the encoder and, more
/// importantly, by the fuzz corpus generator — seeds built by the real encoder
/// are what let the fuzzer reach past the header into the Verifier (step 1.5).
void EncodeHeader(const Header& h, std::span<std::byte, kHeaderBytes> out) noexcept;

/// A frame whose header has been validated and whose regions have been
/// bounds-checked. The spans alias the caller's buffer and do not own it.
///
/// `fb` has NOT been verified as a FlatBuffer yet — that is VerifyEnvelope's
/// job (step 1.3). Holding an unverified region in a typed struct is safe
/// precisely because it is still just bytes.
struct FrameRegions {
    Header header{};
    std::span<const std::byte> fb{};
    std::span<const std::byte> payload{};
};

/// Steps 1–6 of the docs/PROTOCOL.md §1 sequence: everything up to, but not
/// including, the Verifier. Splitting here lets the invariant tests and the
/// fuzzer exercise the framing logic without dragging in schema types.
///
/// Enforced, in this order — and the order is load-bearing:
///   1. size >= kHeaderBytes              else MalformedMessage
///   2. header parses                     else MalformedMessage
///   3. magic == kFrameMagic              else MalformedMessage
///   4. protocol_ver == kProtocolVersion  else VersionMismatch (FATAL)
///   5. fb_len <= kMaxEnvelopeBytes       else PayloadTooLarge
///   6. size >= kHeaderBytes + fb_len     else MalformedMessage
///   7. payload presence matches the flag else OrphanPayload
///   8. reserved == 0                     else MalformedMessage
///
/// Also checks that `frame` is 8-byte aligned and fails with Internal if not.
/// That is OUR bug, never the peer's — a transport handing us an unaligned
/// buffer must fail loudly rather than produce silent UB (D-0027).
///
/// Check 5 before check 6 is not stylistic: it bounds `fb_len` to 64 KiB before
/// it is ever added to anything, so the addition in check 6 cannot overflow.
/// Reversing them reintroduces the classic wrap.
[[nodiscard]] Result<FrameRegions> SplitFrame(std::span<const std::byte> frame) noexcept;

}  // namespace p2pgpu::protocol
