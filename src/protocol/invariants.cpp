// The ten protocol invariants (docs/PROTOCOL.md §4).
// Each is a length or identity check on attacker-controlled data — these are
// security boundaries, not tidiness. Unit-test every failure case.
// Implemented in Phase 1 step 1.4. See invariants.hpp for the layering.

#include "p2pgpu/protocol/invariants.hpp"

#include <algorithm>
#include <limits>

namespace p2pgpu::protocol {
namespace {

/// Uuid is a FlatBuffers struct and has no operator==.
[[nodiscard]] bool SameUuid(const wire::Uuid& a, const wire::Uuid& b) noexcept {
    return a.hi() == b.hi() && a.lo() == b.lo();
}

}  // namespace

Status CheckParamsSize(std::size_t params_bytes) noexcept {
    if (params_bytes > kMaxParamsBytes) {
        return MakeError(ErrorCode::PayloadTooLarge, "params exceed kMaxParamsBytes");
    }
    return {};
}

Status CheckOutputBytes(std::uint32_t output_bytes) noexcept {
    if (output_bytes > kMaxOutputBytes) {
        return MakeError(ErrorCode::PayloadTooLarge, "output_spec.bytes exceeds kMaxOutputBytes");
    }
    return {};
}

Status CheckEnvelopeSize(std::uint32_t fb_len) noexcept {
    if (fb_len > kMaxEnvelopeBytes) {
        return MakeError(ErrorCode::PayloadTooLarge, "fb_len exceeds kMaxEnvelopeBytes");
    }
    return {};
}

Status CheckSingleInFlightResult(bool already_in_flight) noexcept {
    if (already_in_flight) {
        return MakeError(ErrorCode::OrphanPayload,
                         "result header already in flight for this task");
    }
    return {};
}

Status CheckLeaseHeld(std::span<const wire::Uuid> held_leases,
                      const wire::Uuid& task_id) noexcept {
    const bool held = std::ranges::any_of(
        held_leases, [&](const wire::Uuid& u) { return SameUuid(u, task_id); });
    if (!held) {
        return MakeError(ErrorCode::LeaseNotHeld, "worker does not hold a lease on this task");
    }
    return {};
}

Status CheckReplicaAssignment(std::span<const wire::Uuid> prior_workers,
                              const wire::Uuid& candidate) noexcept {
    const bool seen = std::ranges::any_of(
        prior_workers, [&](const wire::Uuid& u) { return SameUuid(u, candidate); });
    if (seen) {
        return MakeError(ErrorCode::Internal,
                         "replica would go to a worker that computed a sibling");
    }
    return {};
}

Status CheckUploadInterval(std::uint32_t upload_interval_ms) noexcept {
    if (upload_interval_ms < kMinUploadIntervalMs) {
        return MakeError(ErrorCode::Internal, "upload_interval_ms below kMinUploadIntervalMs");
    }
    return {};
}

Status CheckPayloadChecksum(std::uint64_t declared, std::uint64_t computed) noexcept {
    if (declared != computed) {
        // ChecksumMismatch is deliberately NOT a reputation signal. Invariant 9:
        // discard and requeue, no penalty. Corruption is not malice, and
        // treating it as such would blacklist honest workers on flaky networks.
        return MakeError(ErrorCode::ChecksumMismatch, "payload checksum mismatch");
    }
    return {};
}

Status CheckPayloadLength(std::uint32_t declared, std::size_t actual) noexcept {
    if (static_cast<std::size_t>(declared) != actual) {
        // A declared length LARGER than what arrived is the Heartbleed shape.
        // Smaller means unclaimed trailing bytes. Both are refused: the declared
        // length must describe exactly what is present, or nothing downstream
        // can safely rely on either number.
        return MakeError(ErrorCode::MalformedMessage,
                         "declared payload_bytes does not match actual payload");
    }
    return {};
}

std::optional<std::size_t> ChunkOffset(std::uint32_t index) noexcept {
    // Checked multiply. The guard is written as a DIVISION so it cannot itself
    // overflow — testing `index * kChunkBytes > max` would have to compute the
    // very product it is trying to validate.
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
    const auto idx = static_cast<std::size_t>(index);
    if (idx > kMax / kChunkBytes) {
        return std::nullopt;
    }
    return idx * kChunkBytes;
}

Status CheckAssetChunk(std::uint32_t index, std::uint32_t total,
                       std::uint32_t expected_chunks, std::size_t chunk_bytes) noexcept {
    // Order matters. Bound the index against a total we have INDEPENDENTLY
    // confirmed, before that index is used for anything. A peer supplies both
    // `index` and `total`, so `index < total` alone proves nothing — a hostile
    // peer simply sends a large total to make any index look valid.
    if (total != expected_chunks) {
        return MakeError(ErrorCode::MalformedMessage, "chunk total disagrees with expected count");
    }
    if (index >= total) {
        return MakeError(ErrorCode::MalformedMessage, "chunk index out of range");
    }
    if (chunk_bytes > kChunkBytes) {
        return MakeError(ErrorCode::PayloadTooLarge, "chunk larger than kChunkBytes");
    }
    // Only the FINAL chunk may be short. A short chunk elsewhere leaves a hole;
    // the reassembled hash would eventually catch it, but only after we had
    // already written attacker-chosen bytes at a wrong offset.
    if (chunk_bytes < kChunkBytes && index + 1U != total) {
        return MakeError(ErrorCode::MalformedMessage, "short chunk before the final index");
    }
    if (!ChunkOffset(index)) {
        return MakeError(ErrorCode::MalformedMessage, "chunk offset overflows");
    }
    return {};
}

Status CheckTaskEnvelope(const wire::TaskEnvelope& envelope) noexcept {
    if (const auto* params = envelope.params(); params != nullptr) {
        if (const Status s = CheckParamsSize(params->size()); !s) {
            return s;
        }
    }
    if (const auto* spec = envelope.output_spec(); spec != nullptr) {
        if (const Status s = CheckOutputBytes(spec->bytes()); !s) {
            return s;
        }
    }
    if (const auto* acc = envelope.accumulate(); acc != nullptr) {
        // Only when accumulation is actually configured. A zero interval on an
        // absent-by-default spec means "not accumulating", not "upload
        // constantly", and rejecting it would refuse every non-accumulating task.
        if (acc->upload_interval_ms() != 0) {
            if (const Status s = CheckUploadInterval(acc->upload_interval_ms()); !s) {
                return s;
            }
        }
    }
    return {};
}

}  // namespace p2pgpu::protocol
