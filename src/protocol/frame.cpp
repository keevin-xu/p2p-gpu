// Frame header encode/decode. See docs/PROTOCOL.md §1.
// R11 applies: no pointer arithmetic, no memcpy. std::span only.
// Implemented in Phase 1 step 1.2.

#include "p2pgpu/protocol/frame.hpp"

namespace p2pgpu::protocol {
namespace {

/// Little-endian loads over FIXED-EXTENT spans.
///
/// The extent in `std::span<const std::byte, N>` is part of the type, so the
/// compiler proves each index is in range — there is no runtime bounds check to
/// forget and no pointer to advance. This is what R11 means by "use std::span"
/// rather than "be careful with pointers".
[[nodiscard]] constexpr std::uint16_t ReadU16LE(std::span<const std::byte, 2> b) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<unsigned>(std::to_integer<std::uint8_t>(b[0])) |
        (static_cast<unsigned>(std::to_integer<std::uint8_t>(b[1])) << 8U));
}

[[nodiscard]] constexpr std::uint32_t ReadU32LE(std::span<const std::byte, 4> b) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[0])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[3])) << 24U);
}

constexpr void WriteU16LE(std::uint16_t v, std::span<std::byte, 2> out) noexcept {
    out[0] = static_cast<std::byte>(v & 0xFFU);
    out[1] = static_cast<std::byte>((v >> 8U) & 0xFFU);
}

constexpr void WriteU32LE(std::uint32_t v, std::span<std::byte, 4> out) noexcept {
    out[0] = static_cast<std::byte>(v & 0xFFU);
    out[1] = static_cast<std::byte>((v >> 8U) & 0xFFU);
    out[2] = static_cast<std::byte>((v >> 16U) & 0xFFU);
    out[3] = static_cast<std::byte>((v >> 24U) & 0xFFU);
}

}  // namespace

std::optional<Header> ParseHeader(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() != kHeaderBytes) {
        return std::nullopt;
    }
    // subspan<Offset, Count>() yields a FIXED-extent span, so every read below
    // is statically in bounds.
    Header h;
    h.magic        = ReadU32LE(bytes.subspan<0, 4>());
    h.protocol_ver = ReadU16LE(bytes.subspan<4, 2>());
    h.flags        = ReadU16LE(bytes.subspan<6, 2>());
    h.fb_len       = ReadU32LE(bytes.subspan<8, 4>());
    h.reserved     = ReadU32LE(bytes.subspan<12, 4>());
    return h;
}

void EncodeHeader(const Header& h, std::span<std::byte, kHeaderBytes> out) noexcept {
    WriteU32LE(h.magic, out.subspan<0, 4>());
    WriteU16LE(h.protocol_ver, out.subspan<4, 2>());
    WriteU16LE(h.flags, out.subspan<6, 2>());
    WriteU32LE(h.fb_len, out.subspan<8, 4>());
    WriteU32LE(h.reserved, out.subspan<12, 4>());
}

Result<FrameRegions> SplitFrame(std::span<const std::byte> frame) noexcept {
    // 0. OUR precondition, not the peer's. The Envelope begins at
    //    `frame + kHeaderBytes` and requires 8-byte alignment; kHeaderBytes is
    //    a multiple of 8, so an aligned frame guarantees an aligned Envelope.
    //    A transport handing us an unaligned buffer is a bug in our code, and
    //    it must fail LOUDLY rather than produce silent UB inside FlatBuffers
    //    (D-0027). Hence Internal, not a peer-facing error code.
    //
    //    Casting a pointer to uintptr_t to inspect alignment is not pointer
    //    arithmetic on attacker data — no offset is computed and nothing is
    //    dereferenced. It is the check that PREVENTS unsound access.
    if (!frame.empty() &&
        (static_cast<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(frame.data())) %
         kFrameAlignment) != 0) {
        return MakeError(ErrorCode::Internal, "frame buffer is not 8-byte aligned");
    }

    // 1. Header is fixed-size. Reject anything shorter before touching it.
    if (frame.size() < kHeaderBytes) {
        return MakeError(ErrorCode::MalformedMessage, "frame shorter than header");
    }

    // 2. Parse from a bounds-checked span. No pointer arithmetic.
    const std::optional<Header> hdr = ParseHeader(frame.first(kHeaderBytes));
    if (!hdr) {
        return MakeError(ErrorCode::MalformedMessage, "header parse failed");
    }

    // 3. Cheap wrong-protocol rejection before any real work.
    if (hdr->magic != kFrameMagic) {
        return MakeError(ErrorCode::MalformedMessage, "bad magic");
    }

    // 4. Version mismatch is FATAL: no negotiation, no compatibility shims
    //    (docs/PROTOCOL.md §5). Retrying with the same state cannot help.
    if (hdr->protocol_ver != kProtocolVersion) {
        return MakeError(ErrorCode::VersionMismatch, "protocol version mismatch",
                         /*fatal=*/true);
    }

    // 5. Bound fb_len BEFORE it is used in any arithmetic. This ordering is the
    //    entire defence against a wrap in step 6: with fb_len proven <= 64 KiB,
    //    `kHeaderBytes + fb_len` cannot overflow size_t on any supported target.
    //    Swapping steps 5 and 6 reintroduces the classic overflow.
    if (hdr->fb_len > kMaxEnvelopeBytes) {
        return MakeError(ErrorCode::PayloadTooLarge, "fb_len exceeds kMaxEnvelopeBytes");
    }

    // 6. Now the addition is provably safe.
    const std::size_t fb_end = kHeaderBytes + static_cast<std::size_t>(hdr->fb_len);
    if (frame.size() < fb_end) {
        return MakeError(ErrorCode::MalformedMessage, "frame shorter than declared fb_len");
    }

    FrameRegions out;
    out.header = *hdr;
    out.fb = frame.subspan(kHeaderBytes, hdr->fb_len);
    out.payload = frame.subspan(fb_end);

    // 7. The flag and the bytes must agree in BOTH directions. A payload with no
    //    flag is unclaimed trailing data; a flag with no payload invites a
    //    reader to look at nothing. Both are OrphanPayload.
    if (hdr->payload_follows() && out.payload.empty()) {
        return MakeError(ErrorCode::OrphanPayload, "payload_follows set but no payload");
    }
    if (!hdr->payload_follows() && !out.payload.empty()) {
        return MakeError(ErrorCode::OrphanPayload, "trailing bytes without payload_follows");
    }

    // 8. The reserved word must be zero. Rejecting non-zero is what keeps it
    //    genuinely available later: a peer that scribbles in it today would
    //    otherwise become a compatibility constraint tomorrow.
    if (hdr->reserved != 0) {
        return MakeError(ErrorCode::MalformedMessage, "reserved header field is not zero");
    }

    // Bound the payload as well. Without this an attacker hands us an
    // arbitrarily large frame and we describe all of it — every region needs a
    // limit, not just the verified one.
    if (out.payload.size() > kMaxOutputBytes) {
        return MakeError(ErrorCode::PayloadTooLarge, "payload exceeds kMaxOutputBytes");
    }

    return out;
}

}  // namespace p2pgpu::protocol
