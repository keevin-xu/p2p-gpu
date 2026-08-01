// T1 — frame header codec and the docs/PROTOCOL.md §1 sequence (steps 1.2/1.3).
//
// Every test here is a rejection test unless it says otherwise. The happy path
// is one case; the attack surface is all the others, and R11 exists because of
// them.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_template_test_macros.hpp>

#include <array>
#include <cstddef>
#include <vector>

#include "p2pgpu/protocol/frame.hpp"

using namespace p2pgpu::protocol;

namespace {

/// Build a syntactically valid frame. `fb` and `payload` are arbitrary bytes —
/// these tests exercise FRAMING, not FlatBuffers validity, which is
/// test_verify.cpp's job.
std::vector<std::byte> MakeFrame(std::vector<std::byte> fb,
                                 std::vector<std::byte> payload = {},
                                 std::uint32_t magic = kFrameMagic,
                                 std::uint16_t ver = kProtocolVersion,
                                 std::optional<std::uint32_t> fb_len_override = std::nullopt) {
    Header h;
    h.magic = magic;
    h.protocol_ver = ver;
    h.flags = payload.empty() ? 0U
                              : static_cast<std::uint16_t>(FrameFlags::kPayloadFollows);
    h.fb_len = fb_len_override.value_or(static_cast<std::uint32_t>(fb.size()));

    std::vector<std::byte> out(kHeaderBytes);
    EncodeHeader(h, std::span<std::byte, kHeaderBytes>(out.data(), kHeaderBytes));
    out.insert(out.end(), fb.begin(), fb.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::byte> Bytes(std::size_t n, std::byte v = std::byte{0xAB}) {
    return std::vector<std::byte>(n, v);
}

}  // namespace

TEST_CASE("ParseHeader round-trips exactly", "[frame]") {
    const Header in{kFrameMagic, kProtocolVersion, 1, 4242};
    std::array<std::byte, kHeaderBytes> buf{};
    EncodeHeader(in, buf);

    const auto out = ParseHeader(buf);
    REQUIRE(out.has_value());
    CHECK(out->magic == in.magic);
    CHECK(out->protocol_ver == in.protocol_ver);
    CHECK(out->flags == in.flags);
    CHECK(out->fb_len == in.fb_len);
    CHECK(out->payload_follows());
}

TEST_CASE("ParseHeader is total over its domain and rejects wrong sizes", "[frame]") {
    // Deliberately exhaustive over every length below the header, plus one
    // above. ParseHeader must have a defined result for all of them and must
    // never read past the span it was given — ASan is the judge of the second
    // part, which is why this loop exists at all.
    for (std::size_t n = 0; n < kHeaderBytes; ++n) {
        const auto b = Bytes(n);
        CHECK_FALSE(ParseHeader(b).has_value());
    }
    CHECK(ParseHeader(Bytes(kHeaderBytes)).has_value());
    CHECK_FALSE(ParseHeader(Bytes(kHeaderBytes + 1)).has_value());
}

TEST_CASE("header decodes little-endian regardless of host byte order", "[frame]") {
    // Hand-written bytes, not encoder output: this must pin the WIRE format,
    // and a round-trip through our own encoder would agree with itself even if
    // both sides were wrong.
    const std::array<std::byte, kHeaderBytes> raw{
        std::byte{0x50}, std::byte{0x47}, std::byte{0x32}, std::byte{0x50},  // "P2GP" LE
        std::byte{0x01}, std::byte{0x00},                                    // ver = 1
        std::byte{0x01}, std::byte{0x00},                                    // flags = 1
        std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01},  // 0x01020304
    };
    const auto h = ParseHeader(raw);
    REQUIRE(h.has_value());
    CHECK(h->magic == kFrameMagic);
    CHECK(h->protocol_ver == 1);
    CHECK(h->fb_len == 0x01020304U);
}

TEST_CASE("SplitFrame accepts a well-formed frame", "[frame]") {
    const auto frame = MakeFrame(Bytes(16), Bytes(8));
    const auto r = SplitFrame(frame);
    REQUIRE(r);
    CHECK(r->fb.size() == 16);
    CHECK(r->payload.size() == 8);
    CHECK(r->header.payload_follows());
}

TEST_CASE("SplitFrame accepts a frame with no payload", "[frame]") {
    const auto frame = MakeFrame(Bytes(16));
    const auto r = SplitFrame(frame);
    REQUIRE(r);
    CHECK(r->payload.empty());
    CHECK_FALSE(r->header.payload_follows());
}

TEST_CASE("SplitFrame rejects short frames", "[frame][reject]") {
    for (std::size_t n = 0; n < kHeaderBytes; ++n) {
        const auto r = SplitFrame(Bytes(n));
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::MalformedMessage);
    }
}

TEST_CASE("SplitFrame rejects a bad magic", "[frame][reject]") {
    const auto frame = MakeFrame(Bytes(8), {}, /*magic=*/0xDEADBEEF);
    const auto r = SplitFrame(frame);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::MalformedMessage);
}

TEST_CASE("version mismatch is rejected AND marked fatal", "[frame][reject]") {
    // The `fatal` flag is the point. docs/PROTOCOL.md §5: no negotiation, no
    // compatibility shims — a peer that retries with the same state can never
    // succeed, so it must be told to stop rather than loop.
    const auto frame = MakeFrame(Bytes(8), {}, kFrameMagic,
                                 static_cast<std::uint16_t>(kProtocolVersion + 1));
    const auto r = SplitFrame(frame);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::VersionMismatch);
    CHECK(r.error().fatal);
}

TEST_CASE("SplitFrame rejects fb_len over the envelope cap", "[frame][reject]") {
    const auto frame = MakeFrame(Bytes(8), {}, kFrameMagic, kProtocolVersion,
                                 kMaxEnvelopeBytes + 1);
    const auto r = SplitFrame(frame);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::PayloadTooLarge);
}

TEST_CASE("fb_len at UINT32_MAX cannot overflow the bounds check", "[frame][reject]") {
    // THE OVERFLOW TEST. If the size check were written as
    // `frame.size() < kHeaderBytes + fb_len` WITHOUT bounding fb_len first,
    // this input would wrap the addition and the check would pass — handing out
    // a span far past the buffer. The §1 ordering is what prevents it, and this
    // case is why that ordering is not stylistic.
    const auto frame = MakeFrame(Bytes(8), {}, kFrameMagic, kProtocolVersion,
                                 0xFFFFFFFFU);
    const auto r = SplitFrame(frame);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::PayloadTooLarge);
}

TEST_CASE("SplitFrame rejects fb_len larger than the frame", "[frame][reject]") {
    // Heartbleed's exact shape: a declared length that exceeds what arrived.
    const auto frame = MakeFrame(Bytes(8), {}, kFrameMagic, kProtocolVersion,
                                 /*fb_len=*/64);
    const auto r = SplitFrame(frame);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::MalformedMessage);
}

TEST_CASE("payload flag and payload bytes must agree both ways", "[frame][reject]") {
    SECTION("flag set, no payload") {
        auto frame = MakeFrame(Bytes(8));
        // Force the flag on without appending bytes.
        Header h{kFrameMagic, kProtocolVersion,
                 static_cast<std::uint16_t>(FrameFlags::kPayloadFollows), 8};
        EncodeHeader(h, std::span<std::byte, kHeaderBytes>(frame.data(), kHeaderBytes));
        const auto r = SplitFrame(frame);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::OrphanPayload);
    }
    SECTION("payload present, flag clear") {
        auto frame = MakeFrame(Bytes(8), Bytes(4));
        Header h{kFrameMagic, kProtocolVersion, 0, 8};  // clear the flag
        EncodeHeader(h, std::span<std::byte, kHeaderBytes>(frame.data(), kHeaderBytes));
        const auto r = SplitFrame(frame);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::OrphanPayload);
    }
}

TEST_CASE("SplitFrame rejects an oversized payload", "[frame][reject]") {
    // Every region needs a bound, not just the verified one. Without this an
    // attacker sends an arbitrarily large frame and we describe all of it.
    const auto frame = MakeFrame(Bytes(8), Bytes(kMaxOutputBytes + 1));
    const auto r = SplitFrame(frame);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::PayloadTooLarge);
}

TEST_CASE("a zero-length envelope is structurally acceptable", "[frame]") {
    // Framing must not reject it — that is the Verifier's call, and conflating
    // the two layers would make the failure report the wrong cause.
    const auto frame = MakeFrame({});
    const auto r = SplitFrame(frame);
    REQUIRE(r);
    CHECK(r->fb.empty());
}

// ── D-0027: alignment and the reserved word ─────────────────────────────

TEST_CASE("the header keeps the FlatBuffer 8-byte aligned", "[frame][align]") {
    // THE REGRESSION TEST FOR D-0027. A 12-byte header put the Envelope on a
    // 4-byte boundary even when the frame was perfectly aligned, making every
    // Uuid/Hash32 access undefined behaviour. This pins the property rather
    // than the constant, so shrinking the header fails here loudly.
    STATIC_REQUIRE(kHeaderBytes % kFrameAlignment == 0);

    const auto frame = MakeFrame(Bytes(16));
    const auto r = SplitFrame(frame);
    REQUIRE(r);
    const auto fb_addr = reinterpret_cast<std::uintptr_t>(r->fb.data());
    CHECK(fb_addr % kFrameAlignment == 0);
}

TEST_CASE("an unaligned frame is refused as OUR bug, not the peer's",
          "[frame][align][reject]") {
    // Internal, deliberately: a transport handing us a misaligned buffer is a
    // defect in our code. Reporting it as MalformedMessage would blame the peer
    // and, worse, would look like ordinary hostile traffic in the logs.
    alignas(16) std::array<std::byte, 128> storage{};
    const auto frame = MakeFrame(Bytes(16));
    // Deliberately start one byte in, so the base cannot be 8-aligned.
    std::span<std::byte> shifted(storage.data() + 1, frame.size());
    for (std::size_t i = 0; i < frame.size(); ++i) {
        shifted[i] = frame[i];
    }
    const auto r = SplitFrame(shifted);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::Internal);
}

TEST_CASE("a non-zero reserved word is rejected", "[frame][reject]") {
    // Keeps the field genuinely free for later use. A peer scribbling in it
    // today would otherwise become a compatibility constraint tomorrow.
    auto frame = MakeFrame(Bytes(16));
    Header h{kFrameMagic, kProtocolVersion, 0, 16, /*reserved=*/1};
    EncodeHeader(h, std::span<std::byte, kHeaderBytes>(frame.data(), kHeaderBytes));
    const auto r = SplitFrame(frame);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::MalformedMessage);
}

TEST_CASE("reserved round-trips through the codec", "[frame]") {
    const Header in{kFrameMagic, kProtocolVersion, 0, 8, 0xABCD1234};
    std::array<std::byte, kHeaderBytes> buf{};
    EncodeHeader(in, buf);
    const auto out = ParseHeader(buf);
    REQUIRE(out.has_value());
    CHECK(out->reserved == 0xABCD1234);
}
