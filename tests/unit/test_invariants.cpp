// T1 — the ten invariants from docs/PROTOCOL.md §4 (step 1.4).
//
// Step 1.4 requires EVERY failure case, not just the happy path. These are
// security boundaries: a missing rejection test is a rule nobody is enforcing.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <vector>

#include "p2pgpu/protocol/invariants.hpp"

using namespace p2pgpu::protocol;
namespace wire = p2pgpu::wire;   // sibling namespace, not nested

namespace {
TaskId T(std::uint64_t hi, std::uint64_t lo) { return TaskId{hi, lo}; }
WorkerId W(std::uint64_t hi, std::uint64_t lo) { return WorkerId{hi, lo}; }
}  // namespace

// ── 1 ───────────────────────────────────────────────────────────────────
TEST_CASE("invariant 1: params size", "[invariants]") {
    CHECK(CheckParamsSize(0));
    CHECK(CheckParamsSize(kMaxParamsBytes));            // boundary: allowed
    CHECK_FALSE(CheckParamsSize(kMaxParamsBytes + 1));  // boundary: rejected
    CHECK_FALSE(CheckParamsSize(std::numeric_limits<std::size_t>::max()));
    CHECK(CheckParamsSize(kMaxParamsBytes + 1).error().code == ErrorCode::PayloadTooLarge);
}

// ── 2 ───────────────────────────────────────────────────────────────────
TEST_CASE("invariant 2: output bytes", "[invariants]") {
    CHECK(CheckOutputBytes(0));
    CHECK(CheckOutputBytes(kMaxOutputBytes));
    CHECK_FALSE(CheckOutputBytes(kMaxOutputBytes + 1));
    CHECK_FALSE(CheckOutputBytes(0xFFFFFFFFU));
}

// ── 3 ───────────────────────────────────────────────────────────────────
TEST_CASE("invariant 3: envelope size", "[invariants]") {
    CHECK(CheckEnvelopeSize(0));
    CHECK(CheckEnvelopeSize(kMaxEnvelopeBytes));
    CHECK_FALSE(CheckEnvelopeSize(kMaxEnvelopeBytes + 1));
    CHECK_FALSE(CheckEnvelopeSize(0xFFFFFFFFU));
}

// ── 4 ───────────────────────────────────────────────────────────────────
TEST_CASE("invariant 4: one in-flight result header per task", "[invariants]") {
    CHECK(CheckSingleInFlightResult(false));
    CHECK_FALSE(CheckSingleInFlightResult(true));
}

// ── 5 ───────────────────────────────────────────────────────────────────
TEST_CASE("invariant 5: lease must be held", "[invariants]") {
    const std::array<TaskId, 2> held{T(1, 2), T(3, 4)};

    SECTION("held task is accepted") {
        CHECK(CheckLeaseHeld(held, T(1, 2)));
        CHECK(CheckLeaseHeld(held, T(3, 4)));
    }
    SECTION("unheld task is rejected as LeaseNotHeld") {
        const auto s = CheckLeaseHeld(held, T(5, 6));
        REQUIRE_FALSE(s);
        CHECK(s.error().code == ErrorCode::LeaseNotHeld);
    }
    SECTION("a worker holding nothing may act on nothing") {
        CHECK_FALSE(CheckLeaseHeld({}, T(1, 2)));
    }
    SECTION("both halves of the Uuid must match") {
        // Comparing only `hi` would let a worker claim any task sharing a high
        // word — a 2^64 shortcut to another worker's task.
        CHECK_FALSE(CheckLeaseHeld(held, T(1, 99)));
        CHECK_FALSE(CheckLeaseHeld(held, T(99, 2)));
    }
}

// ── 6 ───────────────────────────────────────────────────────────────────
TEST_CASE("invariant 6: replicas never revisit a prior worker", "[invariants]") {
    const std::array<WorkerId, 2> prior{W(10, 10), W(20, 20)};
    CHECK(CheckReplicaAssignment(prior, W(30, 30)));
    CHECK_FALSE(CheckReplicaAssignment(prior, W(10, 10)));
    CHECK_FALSE(CheckReplicaAssignment(prior, W(20, 20)));
    // First replica of a fresh task: nobody has computed it yet.
    CHECK(CheckReplicaAssignment({}, W(1, 1)));
}

// ── 7 ───────────────────────────────────────────────────────────────────
TEST_CASE("invariant 7: upload interval floor", "[invariants]") {
    CHECK(CheckUploadInterval(kMinUploadIntervalMs));
    CHECK(CheckUploadInterval(kMinUploadIntervalMs + 1));
    CHECK_FALSE(CheckUploadInterval(kMinUploadIntervalMs - 1));
    CHECK_FALSE(CheckUploadInterval(0));
    CHECK_FALSE(CheckUploadInterval(1));
}

// ── 8 is a negative constraint with no callable form; see invariants.hpp ──

// ── 9 ───────────────────────────────────────────────────────────────────
TEST_CASE("invariant 9: checksum mismatch is NOT a malice signal", "[invariants]") {
    CHECK(CheckPayloadChecksum(0xDEADBEEFCAFEBABEULL, 0xDEADBEEFCAFEBABEULL));
    CHECK(CheckPayloadChecksum(0, 0));

    const auto s = CheckPayloadChecksum(1, 2);
    REQUIRE_FALSE(s);
    // ChecksumMismatch specifically — NOT a reputation-bearing code. Invariant 9
    // requires requeue with no penalty, because corruption is not malice and
    // penalising it would blacklist honest workers on flaky networks.
    CHECK(s.error().code == ErrorCode::ChecksumMismatch);
    CHECK_FALSE(s.error().fatal);
}

TEST_CASE("invariant 9: declared payload length must match reality", "[invariants]") {
    CHECK(CheckPayloadLength(0, 0));
    CHECK(CheckPayloadLength(1024, 1024));
    // Declared LARGER than arrived — the Heartbleed shape.
    CHECK_FALSE(CheckPayloadLength(1024, 512));
    // Declared SMALLER — unclaimed trailing bytes.
    CHECK_FALSE(CheckPayloadLength(512, 1024));
    CHECK_FALSE(CheckPayloadLength(0xFFFFFFFFU, 8));
}

// ── 10 ──────────────────────────────────────────────────────────────────
TEST_CASE("invariant 10: chunk offset arithmetic is checked", "[invariants]") {
    CHECK(ChunkOffset(0).value() == 0);
    CHECK(ChunkOffset(1).value() == kChunkBytes);
    CHECK(ChunkOffset(100).value() == 100ULL * kChunkBytes);

    // On 64-bit, u32 index x 16 KiB cannot overflow size_t — so the guard is
    // belt-and-braces here and load-bearing on a 32-bit target. wasm32 IS a
    // 32-bit target and runs this same code (R2), which is exactly why the
    // check exists rather than being dismissed as unreachable.
    CHECK(ChunkOffset(0xFFFFFFFFU).has_value() == (sizeof(std::size_t) > 4));
}

TEST_CASE("invariant 10: chunk bounds", "[invariants]") {
    constexpr std::uint32_t kExpected = 10;

    SECTION("a well-formed middle chunk is accepted") {
        CHECK(CheckAssetChunk(0, kExpected, kExpected, kChunkBytes));
        CHECK(CheckAssetChunk(8, kExpected, kExpected, kChunkBytes));
    }
    SECTION("only the final chunk may be short") {
        CHECK(CheckAssetChunk(kExpected - 1, kExpected, kExpected, 1));
        CHECK_FALSE(CheckAssetChunk(0, kExpected, kExpected, 1));
    }
    SECTION("index must be inside total") {
        CHECK_FALSE(CheckAssetChunk(kExpected, kExpected, kExpected, kChunkBytes));
        CHECK_FALSE(CheckAssetChunk(0xFFFFFFFFU, kExpected, kExpected, kChunkBytes));
    }
    SECTION("a peer cannot inflate `total` to legitimise a large index") {
        // Both `index` and `total` come from the peer, so `index < total` alone
        // proves nothing — it is trivially satisfied by sending a bigger total.
        // Validating `total` against an INDEPENDENTLY known chunk count is what
        // makes the index bound mean anything.
        CHECK_FALSE(CheckAssetChunk(500, 1000, kExpected, kChunkBytes));
    }
    SECTION("an oversized chunk is rejected") {
        CHECK_FALSE(CheckAssetChunk(0, kExpected, kExpected, kChunkBytes + 1));
    }
}

// ── composite ───────────────────────────────────────────────────────────
TEST_CASE("CheckTaskEnvelope enforces 1, 2 and 7 together", "[invariants]") {
    auto build = [](std::size_t params_bytes, std::uint32_t out_bytes,
                    std::uint32_t interval) {
        flatbuffers::FlatBufferBuilder fbb;
        const std::vector<std::uint8_t> params(params_bytes, 0);
        auto pv = fbb.CreateVector(params);
        wire::OutputSpecBuilder osb(fbb);
        osb.add_bytes(out_bytes);
        auto os = osb.Finish();
        wire::AccumulationSpecBuilder asb(fbb);
        asb.add_upload_interval_ms(interval);
        auto acc = asb.Finish();
        wire::TaskEnvelopeBuilder teb(fbb);
        teb.add_params(pv);
        teb.add_output_spec(os);
        teb.add_accumulate(acc);
        fbb.Finish(teb.Finish());
        return fbb.Release();
    };

    SECTION("a valid envelope passes") {
        auto buf = build(64, 1024, 500);
        CHECK(CheckTaskEnvelope(*flatbuffers::GetRoot<wire::TaskEnvelope>(buf.data())));
    }
    SECTION("oversized params rejected") {
        auto buf = build(kMaxParamsBytes + 1, 1024, 500);
        CHECK_FALSE(CheckTaskEnvelope(*flatbuffers::GetRoot<wire::TaskEnvelope>(buf.data())));
    }
    SECTION("oversized output rejected") {
        auto buf = build(64, kMaxOutputBytes + 1, 500);
        CHECK_FALSE(CheckTaskEnvelope(*flatbuffers::GetRoot<wire::TaskEnvelope>(buf.data())));
    }
    SECTION("too-frequent upload rejected") {
        auto buf = build(64, 1024, 100);
        CHECK_FALSE(CheckTaskEnvelope(*flatbuffers::GetRoot<wire::TaskEnvelope>(buf.data())));
    }
    SECTION("interval 0 means 'not accumulating', not 'upload constantly'") {
        // Rejecting 0 here would refuse every non-accumulating task, i.e. all of
        // Workload B.
        auto buf = build(64, 1024, 0);
        CHECK(CheckTaskEnvelope(*flatbuffers::GetRoot<wire::TaskEnvelope>(buf.data())));
    }
}
