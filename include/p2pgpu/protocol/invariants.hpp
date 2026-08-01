#pragma once
//
// The ten invariants from docs/PROTOCOL.md §4 — step 1.4.
//
// **These are security boundaries, not tidiness.** Every one is a length or
// identity check on attacker-controlled data. FlatBuffers stops memory
// corruption; it does not stop a worker declaring a 4 GiB payload and OOM-ing
// the coordinator, or claiming a lease it never held.
//
// ── PURE FUNCTIONS, SUPPLIED STATE ───────────────────────────────────────
// Several invariants are about coordinator state (which leases a worker holds,
// which workers already computed a replica). Those take that state as an
// argument rather than reaching for it. The protocol layer owns the RULE; the
// coordinator owns the STATE.
//
// That split is not stylistic. It keeps `p2pgpu-protocol` free of I/O and
// policy as ARCHITECTURE.md §4 requires, it makes every rule unit-testable with
// no coordinator running, and it is what lets the fuzz target reach these
// checks (D-0015 forbids linking compiled vcpkg libraries into that build).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "p2pgpu/p2pgpu_generated.h"
#include "p2pgpu/protocol/error.hpp"
#include "p2pgpu/protocol/ids.hpp"
#include "p2pgpu/protocol/limits.hpp"

namespace p2pgpu::protocol {

// ── 1. params.size() <= kMaxParamsBytes ─────────────────────────────────
[[nodiscard]] Status CheckParamsSize(std::size_t params_bytes) noexcept;

// ── 2. output_spec.bytes <= kMaxOutputBytes ─────────────────────────────
[[nodiscard]] Status CheckOutputBytes(std::uint32_t output_bytes) noexcept;

// ── 3. fb_len <= kMaxEnvelopeBytes ──────────────────────────────────────
/// Enforced in SplitFrame BEFORE the Verifier runs; exposed here so the rule is
/// testable in isolation and so all ten live in one place.
[[nodiscard]] Status CheckEnvelopeSize(std::uint32_t fb_len) noexcept;

// ── 4. At most one in-flight ResultHeader per task_id per worker ────────
/// `already_in_flight`: does this worker already have an unacknowledged
/// ResultHeader for this task? The coordinator owns that set.
///
/// Without this a worker can announce a 8 MiB payload repeatedly and pin
/// reassembly buffers — a memory-exhaustion lever, not a protocol nicety.
[[nodiscard]] Status CheckSingleInFlightResult(bool already_in_flight) noexcept;

// ── 5. A worker may only act on a task it holds a lease on ──────────────
/// Applies to Progress, ResultHeader, and Release. Violation => LeaseNotHeld.
///
/// This is the authorization check of the whole protocol: without it any worker
/// can submit results for another worker's task, which defeats replication,
/// reputation, and speculation simultaneously.
/// Strongly typed on purpose (step 1.6): both arguments were `wire::Uuid`
/// before, so a worker list could be passed where a task list belonged and it
/// compiled. This is the protocol's authorization check — a crossed argument
/// here is a security bug, not a typo.
[[nodiscard]] Status CheckLeaseHeld(std::span<const TaskId> held_leases,
                                    TaskId task_id) noexcept;

// ── 6. Replicas never go to a worker that computed a sibling ────────────
/// `prior_workers`: everyone who already computed this task or any replica of
/// it. Granting a replica to one of them makes agreement meaningless — the same
/// worker agreeing with itself is not evidence, and a liar would validate its
/// own lie.
[[nodiscard]] Status CheckReplicaAssignment(std::span<const WorkerId> prior_workers,
                                            WorkerId candidate) noexcept;

// ── 7. accumulate.upload_interval_ms >= kMinUploadIntervalMs ────────────
/// Uploading more often defeats the arithmetic-intensity design the whole
/// project rests on (R5, D-0001) — it is a correctness bound on the *economics*,
/// which is why it is enforced rather than merely recommended.
[[nodiscard]] Status CheckUploadInterval(std::uint32_t upload_interval_ms) noexcept;

// ── 8. TaskStats is telemetry, never an input to correctness ────────────
//
// NOT A FUNCTION, AND DELIBERATELY SO. This is a negative constraint: there is
// no call that proves the coordinator ignored a number. A worker controls every
// field of TaskStats and can lie about all of them, so anything derived from it
// — credit, sizing that cannot self-correct, validation — is worker-controlled.
//
// Enforced by review and by the type never appearing in a decision path.
// Writing a no-op `CheckTaskStats()` here would be worse than nothing: it would
// look like the rule was mechanically enforced when it is not.
//
// (Sizing DOES consume observed durations — but the coordinator's own
// measurements of them, not the worker's self-report. Step 2.13.)

// ── 9. checksum is BLAKE3-64 of the payload ─────────────────────────────
/// Takes the ALREADY-COMPUTED hash rather than hashing here: `p2pgpu-protocol`
/// must not link libblake3, because the fuzz preset builds this library with a
/// different compiler and D-0015 forbids it linking anything vcpkg compiled.
/// The constraint pushes hashing to the caller, which is also the correct
/// layering — the protocol states the rule, the caller supplies the evidence.
///
/// Mismatch => discard and requeue with **no reputation penalty** (invariant 9):
/// transport corruption is not malice, and treating it as such would blacklist
/// honest workers on flaky networks.
[[nodiscard]] Status CheckPayloadChecksum(std::uint64_t declared,
                                          std::uint64_t computed) noexcept;

/// The other half of invariant 9: the declared length must match the bytes that
/// actually arrived. This is the Heartbleed shape — a declared length larger
/// than the buffer is exactly how you read adjacent memory.
[[nodiscard]] Status CheckPayloadLength(std::uint32_t declared,
                                        std::size_t actual) noexcept;

// ── 10. Asset chunk reassembly ──────────────────────────────────────────
/// `index < total`, `total == expected_chunks`, and the resulting byte offset
/// computed with CHECKED arithmetic.
///
/// `index * kChunkBytes` is the classic integer-overflow site: a large enough
/// index wraps to a small offset, and the write lands somewhere it should not.
[[nodiscard]] Status CheckAssetChunk(std::uint32_t index, std::uint32_t total,
                                     std::uint32_t expected_chunks,
                                     std::size_t chunk_bytes) noexcept;

/// Byte offset of `index`, or nullopt on overflow. **Never compute this by
/// multiplying.** Returns nullopt rather than a wrapped value so the caller
/// cannot accidentally use a wrong offset.
[[nodiscard]] std::optional<std::size_t> ChunkOffset(std::uint32_t index) noexcept;

// ── Composite ───────────────────────────────────────────────────────────
/// Every stateless invariant that applies to a TaskEnvelope (1, 2, 7).
/// A granted envelope originates from the coordinator, but a *replica of it*
/// can be echoed back by a worker, so it is checked on receipt regardless of
/// who is believed to have sent it.
[[nodiscard]] Status CheckTaskEnvelope(const wire::TaskEnvelope& envelope) noexcept;

}  // namespace p2pgpu::protocol
