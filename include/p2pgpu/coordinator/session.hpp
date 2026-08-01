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

#include "p2pgpu/coordinator/job.hpp"
#include "p2pgpu/coordinator/kernel_registry.hpp"
#include "p2pgpu/protocol/verify.hpp"

namespace p2pgpu::coordinator {

/// What the caller should do after handling a frame.
struct Reaction {
    /// Frame to send back, if any. Already encoded and framed.
    std::vector<std::byte> reply;
    /// Close the connection after sending. Set for fatal errors — a peer that
    /// cannot succeed by retrying must be told to stop, not left looping
    /// (PROTOCOL.md §5).
    bool close = false;
};

class Session {
public:
    Session(JobManager& jobs, const KernelRegistry& kernels, std::uint64_t conn_id);

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

private:
    [[nodiscard]] Reaction OnHello(const wire::Hello& hello);
    [[nodiscard]] Reaction OnLeaseRequest(const wire::LeaseRequest& req, std::uint64_t now_ms);
    [[nodiscard]] Reaction OnRelease(const wire::Release& release);
    [[nodiscard]] Reaction OnResultHeader(const wire::ResultHeader& header,
                                          std::span<const std::byte> payload);

    [[nodiscard]] static Reaction Fatal(wire::ErrorCode code, const char* message);
    [[nodiscard]] static Reaction NonFatal(wire::ErrorCode code, const char* message);

    JobManager& jobs_;
    const KernelRegistry& kernels_;
    std::uint64_t conn_id_ = 0;

    bool handshaked_ = false;
    WorkerId worker_id_;

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
