#pragma once
//
// Per-connection protocol handling — steps 1.15 (handshake) and 1.16 (result
// ingestion).
//
// Deliberately TRANSPORT-FREE: it takes bytes in and returns bytes out, so the
// whole handshake and submission path is unit-testable with no socket, no
// event loop, and no timing. `net.cpp` is the only thing that knows about
// uWebSockets.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "p2pgpu/coordinator/event_log.hpp"
#include "p2pgpu/coordinator/reputation.hpp"
#include "p2pgpu/coordinator/job.hpp"
#include "p2pgpu/coordinator/fleet.hpp"
#include "p2pgpu/coordinator/kernel_registry.hpp"
#include "p2pgpu/coordinator/reference_check.hpp"
#include "p2pgpu/coordinator/sizer.hpp"
#include "p2pgpu/protocol/verify.hpp"

namespace p2pgpu::coordinator {

/// What the caller should do after handling a frame.
struct Reaction {
    /// Frames to send back, if any — each already encoded and framed.
    ///
    /// A LIST, not one buffer. WebSocket is message-oriented and our framing is
    /// **one frame per message**: `VerifyFrame` on a buffer holding two frames
    /// reads the second as a trailing payload and rejects it as an orphan.
    ///
    /// This was concatenated until step 2.11, and the bug never showed because
    /// every reply so far held exactly one frame — `max_tasks` was always 1.
    /// Welcome + BenchmarkRequest is the first two-frame reply.
    std::vector<std::vector<std::byte>> replies;
    /// Close the connection after sending. Set for fatal errors — a peer that
    /// cannot succeed by retrying must be told to stop, not left looping
    /// (PROTOCOL.md §5).
    bool close = false;
};

class Session {
public:
    /// `reference_stats` is non-null only under the coordinator's dev-only
    /// --verify-reference flag (step 1.26). It is a TEST HARNESS, not
    /// validation — see reference_check.hpp for why the distinction matters.
    Session(JobManager& jobs, const KernelRegistry& kernels, Fleet& fleet,
            std::uint64_t conn_id, std::uint32_t lease_ms,
            ReferenceStats* reference_stats = nullptr);

    /// Handle one already-VERIFIED frame.
    ///
    /// Takes a VerifiedFrame rather than raw bytes so this cannot be called on
    /// unchecked input by accident — the type is the proof that step 1.3's
    /// sequence ran (R11).
    [[nodiscard]] Reaction OnMessage(const protocol::VerifiedFrame& frame,
                                     std::uint64_t now_ms);

    /// Connection dropped. Releases every held lease immediately: a worker
    /// vanishing is the NORMAL case (R8), and the tasks must go back to the
    /// queue rather than wait out their leases.
    void OnDisconnect();

    [[nodiscard]] bool handshaked() const noexcept { return handshaked_; }
    [[nodiscard]] WorkerId worker_id() const noexcept { return worker_id_; }

    /// Take the revokes another connection's win queued for THIS worker (2.17).
    ///
    /// Public because the TRANSPORT drains it on the sweep timer, not this
    /// session on its own reply path (D-0046). A worker that has lost a race is
    /// mid-task and therefore silent, so waiting for it to speak delivers the
    /// stop only after the work it was meant to cancel is already finished.
    ///
    /// The session still only ever QUEUES: it holds no socket and decides no
    /// timing, which is what keeps it transport-free and unit-testable.
    [[nodiscard]] std::vector<std::vector<std::byte>> DrainRevokes();

    /// Dev-only experiment instrumentation (2.23-2.26). Null in every test and
    /// in any coordinator run without `--events-csv`.
    void SetEventLog(EventLog* log) noexcept { events_ = log; }

    /// E5's CONTROL CONDITION (2.25). Speculation on/off is the single variable
    /// that experiment changes, so it is a switch rather than a rebuild — two
    /// binaries differing in more than the thing under test is how a measured
    /// difference gets attributed to the wrong cause.
    void SetSpeculation(bool on) noexcept { speculation_ = on; }

    /// 3.12 — the transport counted enough malformed frames to stop serving
    /// this connection, without disconnecting it yet. Backoff, not eviction:
    /// a broken client that starts framing correctly is served again, and
    /// nothing here reaches reputation (3.11).
    void SetThrottledForAbuse(bool on) noexcept { abusive_ = on; }

    /// 3.7/3.10 — reputation, when the coordinator supplies it. Null in tests
    /// and in any run without replication.
    void SetReputation(ReputationTable* rep) noexcept { reputation_ = rep; }

private:
    /// The actual routing. `OnMessage` wraps this so revokes are appended to
    /// every reply without each handler having to remember.
    [[nodiscard]] Reaction Dispatch(const protocol::VerifiedFrame& frame,
                                    std::uint64_t now_ms);

    [[nodiscard]] Reaction OnHello(const wire::Hello& hello);
    [[nodiscard]] Reaction OnLeaseRequest(const wire::LeaseRequest& req, std::uint64_t now_ms);
    [[nodiscard]] Reaction OnProgress(const wire::Progress& progress, std::uint64_t now_ms);
    [[nodiscard]] Reaction OnRelease(const wire::Release& release);
    [[nodiscard]] Reaction OnGoodbye();

    [[nodiscard]] Reaction OnBenchmarkResult(const wire::BenchmarkResult& result);
    [[nodiscard]] Reaction OnThrottle(const wire::Throttle& throttle);
    [[nodiscard]] Reaction OnResultHeader(const wire::ResultHeader& header,
                                          std::span<const std::byte> payload);

    /// Set at the top of OnMessage so the ingestion path can measure how long a
    /// task actually took. Our clock, never the worker's (invariant 8).
    std::uint64_t now_ms_ = 0;

    [[nodiscard]] static Reaction Fatal(wire::ErrorCode code, const char* message);
    [[nodiscard]] static Reaction NonFatal(wire::ErrorCode code, const char* message);

    JobManager& jobs_;
    const KernelRegistry& kernels_;
    Fleet& fleet_;
    std::uint64_t conn_id_ = 0;
    std::uint32_t lease_ms_ = 30000;
    ReferenceStats* reference_stats_ = nullptr;
    EventLog* events_ = nullptr;
    ReputationTable* reputation_ = nullptr;
    bool abusive_ = false;
    bool speculation_ = true;

    bool handshaked_ = false;
    WorkerId worker_id_;

    /// Tasks whose result has already been ACCEPTED — step 2.10. A duplicate
    /// `ResultHeader` for one of these is discarded SILENTLY, not errored:
    /// speculation (2.17) makes duplicates routine rather than pathological, and
    /// a worker that raced and lost has done nothing wrong.
    std::vector<TaskId> completed_;

    /// Invariant 4: at most one in-flight ResultHeader per task per worker.
    /// Without it a worker can announce an 8 MiB payload repeatedly and pin
    /// reassembly buffers — a memory-exhaustion lever, not a protocol nicety.
    std::vector<TaskId> in_flight_results_;
};

/// BLAKE3-64 of a payload — the first 8 bytes of the BLAKE3 digest, matching
/// `ResultHeader.checksum` (invariant 9).
///
/// Lives in the coordinator rather than p2pgpu-protocol because that library
/// must not link libblake3: the fuzz preset builds it with a different compiler
/// and D-0015 forbids linking anything vcpkg compiled. The invariant takes the
/// already-computed hash for exactly that reason.
[[nodiscard]] std::uint64_t Blake3_64(std::span<const std::byte> bytes) noexcept;

}  // namespace p2pgpu::coordinator
