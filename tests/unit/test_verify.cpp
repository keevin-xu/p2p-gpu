// T1 — the sanctioned bytes->typed path (step 1.3).
//
// This is the boundary R11 is about. The tests are weighted accordingly: one
// happy path, and everything else an attempt to get a bad buffer past it.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

#include "p2pgpu/protocol/verify.hpp"

using namespace p2pgpu::protocol;
namespace wire = p2pgpu::wire;   // sibling namespace, not nested

namespace {

/// A minimal valid Envelope carrying a LeaseRequest.
std::vector<std::byte> ValidEnvelopeBytes(std::uint32_t max_tasks = 4) {
    flatbuffers::FlatBufferBuilder fbb;
    wire::LeaseRequestBuilder lrb(fbb);
    lrb.add_max_tasks(max_tasks);
    auto lr = lrb.Finish();
    wire::EnvelopeBuilder eb(fbb);
    eb.add_body_type(wire::Body::LeaseRequest);
    eb.add_body(lr.Union());
    fbb.Finish(eb.Finish());

    const auto* p = static_cast<const std::byte*>(
        static_cast<const void*>(fbb.GetBufferPointer()));
    return std::vector<std::byte>(p, p + fbb.GetSize());
}

std::vector<std::byte> WrapInFrame(const std::vector<std::byte>& fb,
                                   const std::vector<std::byte>& payload = {}) {
    Header h;
    h.magic = kFrameMagic;
    h.protocol_ver = kProtocolVersion;
    h.flags = payload.empty() ? 0U
                              : static_cast<std::uint16_t>(FrameFlags::kPayloadFollows);
    h.fb_len = static_cast<std::uint32_t>(fb.size());

    std::vector<std::byte> out(kHeaderBytes);
    EncodeHeader(h, std::span<std::byte, kHeaderBytes>(out.data(), kHeaderBytes));
    out.insert(out.end(), fb.begin(), fb.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

}  // namespace

TEST_CASE("VerifyFrame accepts a valid frame and exposes the body", "[verify]") {
    const auto frame = WrapInFrame(ValidEnvelopeBytes(7));
    const auto r = VerifyFrame(frame);
    REQUIRE(r);
    CHECK(r->body_type() == wire::Body::LeaseRequest);
    REQUIRE(r->envelope()->body_as_LeaseRequest() != nullptr);
    CHECK(r->envelope()->body_as_LeaseRequest()->max_tasks() == 7);
    CHECK(r->payload().empty());
}

TEST_CASE("VerifyFrame carries the payload through untouched", "[verify]") {
    const std::vector<std::byte> payload(256, std::byte{0x5A});
    const auto frame = WrapInFrame(ValidEnvelopeBytes(), payload);
    const auto r = VerifyFrame(frame);
    REQUIRE(r);
    CHECK(r->payload().size() == 256);
    CHECK(r->payload()[0] == std::byte{0x5A});
}

TEST_CASE("VerifyFrame rejects garbage in the envelope region", "[verify][reject]") {
    // Valid framing, nonsense FlatBuffer. The framing layer cannot catch this;
    // only the Verifier can, which is why both layers exist.
    const std::vector<std::byte> junk(64, std::byte{0xFF});
    const auto frame = WrapInFrame(junk);
    const auto r = VerifyFrame(frame);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::MalformedMessage);
}

TEST_CASE("VerifyFrame rejects a truncated envelope", "[verify][reject]") {
    auto fb = ValidEnvelopeBytes();
    fb.resize(fb.size() / 2);
    const auto r = VerifyFrame(WrapInFrame(fb));
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::MalformedMessage);
}

TEST_CASE("VerifyFrame rejects an empty envelope", "[verify][reject]") {
    const auto r = VerifyFrame(WrapInFrame({}));
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::MalformedMessage);
}

TEST_CASE("VerifyFrame rejects an envelope with no body", "[verify][reject]") {
    // body_type NONE verifies fine structurally but carries nothing to route on.
    // Rejecting it centrally means no downstream consumer has to remember to.
    flatbuffers::FlatBufferBuilder fbb;
    wire::EnvelopeBuilder eb(fbb);
    fbb.Finish(eb.Finish());
    const auto* p = static_cast<const std::byte*>(
        static_cast<const void*>(fbb.GetBufferPointer()));
    const std::vector<std::byte> fb(p, p + fbb.GetSize());

    const auto r = VerifyFrame(WrapInFrame(fb));
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::MalformedMessage);
}

TEST_CASE("every single-byte corruption is survived, not necessarily rejected",
          "[verify][reject]") {
    // The property under test is MEMORY SAFETY, not detection. Under ASan/UBSan
    // this asserts that no corrupted byte anywhere in a frame can make the
    // verifier read out of bounds — whether the result is accepted or rejected
    // is not the point.
    //
    // Some corruptions ARE accepted (a flipped bit in padding or in a scalar),
    // and that is correct behaviour: verification is a safety property, not an
    // integrity one. Integrity is the BLAKE3-64 checksum's job.
    const auto original = WrapInFrame(ValidEnvelopeBytes(), {std::byte{1}, std::byte{2}});
    std::size_t accepted = 0;
    std::size_t rejected = 0;

    for (std::size_t i = 0; i < original.size(); ++i) {
        auto mutated = original;
        mutated[i] = static_cast<std::byte>(std::to_integer<std::uint8_t>(mutated[i]) ^ 0xFF);
        if (VerifyFrame(mutated)) {
            ++accepted;
        } else {
            ++rejected;
        }
    }
    CHECK(accepted + rejected == original.size());
    // Corrupting the magic or the length fields must always be caught, so at
    // least the header bytes are guaranteed rejections.
    CHECK(rejected >= 4);
}

TEST_CASE("VerifyAssetMsg accepts a valid chunk and rejects garbage",
          "[verify][asset]") {
    flatbuffers::FlatBufferBuilder fbb;
    const wire::Hash32 h{1, 2, 3, 4};
    const std::vector<std::uint8_t> data(64, 0x11);
    auto dv = fbb.CreateVector(data);
    wire::AssetChunkBuilder acb(fbb);
    acb.add_hash(&h);
    acb.add_index(0);
    acb.add_total(1);
    acb.add_bytes(dv);
    auto chunk = acb.Finish();
    wire::AssetMsgBuilder amb(fbb);
    amb.add_body_type(wire::AssetBody::AssetChunk);
    amb.add_body(chunk.Union());
    fbb.Finish(amb.Finish());

    const auto* p = static_cast<const std::byte*>(
        static_cast<const void*>(fbb.GetBufferPointer()));
    const std::vector<std::byte> bytes(p, p + fbb.GetSize());

    const auto ok = VerifyAssetMsg(bytes);
    REQUIRE(ok);
    CHECK((*ok)->body_type() == wire::AssetBody::AssetChunk);

    SECTION("garbage rejected") {
        const std::vector<std::byte> junk(bytes.size(), std::byte{0xFF});
        CHECK_FALSE(VerifyAssetMsg(junk));
    }
    SECTION("empty rejected") {
        CHECK_FALSE(VerifyAssetMsg({}));
    }
    SECTION("oversized rejected before verification") {
        const std::vector<std::byte> huge(kMaxEnvelopeBytes + 1, std::byte{0});
        const auto r = VerifyAssetMsg(huge);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::PayloadTooLarge);
    }
}
