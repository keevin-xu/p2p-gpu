#pragma once
//
// Coordinator transport — step 1.11. uWebSockets HTTP + WebSocket.
//
// Transport plumbing only. Nothing here interprets a byte: every frame goes
// through protocol::VerifyFrame (R11), and message routing lands in 1.15.

#include <cstdint>
#include <string>
#include <string_view>

#include <functional>
#include <span>

#include "p2pgpu/coordinator/job.hpp"
#include "p2pgpu/coordinator/kernel_registry.hpp"
#include "p2pgpu/coordinator/fleet.hpp"
#include "p2pgpu/coordinator/reference_check.hpp"
#include "p2pgpu/coordinator/session.hpp"
#include "p2pgpu/protocol/verify.hpp"

namespace p2pgpu::coordinator {

/// Largest frame the transport will assemble at all. Generous relative to the
/// protocol's own caps (64 KiB envelope + 8 MiB payload) because those are
/// enforced precisely by SplitFrame; this exists so the transport refuses to
/// buffer something absurd before our checks ever run.
inline constexpr std::uint32_t kMaxFrameBytes = 16 * 1024 * 1024;

struct Config {
    int port = 8080;
    /// How long a granted lease lives, on the COORDINATOR's clock. Configurable
    /// because E3's fault-tolerance experiments need expiry to fire inside a
    /// test run rather than 30 s later.
    std::uint32_t lease_ms = 30000;
    /// Silence after which a worker is declared lost and its leases released
    /// (2.8). Not a fault — R8, absence is not malice.
    std::uint32_t worker_timeout_ms = 45000;
    /// How often the ONE sweep timer runs. Not a timer per task
    /// (CONVENTIONS.md §4).
    std::uint32_t sweep_interval_ms = 1000;
    /// DEV ONLY (step 1.26): stop the event loop once every seeded task has
    /// reached a terminal state, so a scripted end-to-end run terminates and
    /// can print its summary. A real coordinator serves indefinitely.
    bool exit_when_complete = false;
    std::string manifest = "kernels/manifest.toml";
    std::string kernel_dir = "kernels";
    std::string log_level = "info";
};

class Server {
public:
    /// `reference_stats` is non-null only under --verify-reference (step
    /// 1.26). A TEST HARNESS, not validation — see reference_check.hpp.
    Server(Config config, const KernelRegistry& kernels, JobManager& jobs,
           Fleet& fleet, ReferenceStats* reference_stats = nullptr);

    /// DEV ONLY (step 1.26): invoked once when every task is terminal, under
    /// --exit-when-complete. Returns the process exit code.
    using OnCompleteFn = std::function<int()>;
    void SetOnComplete(OnCompleteFn fn) { on_complete_ = std::move(fn); }

    /// Blocks, running the uWebSockets event loop.
    ///
    /// Single-threaded by design (CONVENTIONS.md §4): one loop for I/O, and a
    /// thread pool later for CPU-bound validation. Nothing may block this loop.
    void Run();

    using SendFn = std::function<void(std::span<const std::byte>)>;
    using CloseFn = std::function<void()>;

private:
    void OnFrame(Session& session, std::uint64_t conn_id, std::uint32_t& rejected,
                 std::string_view bytes, const SendFn& send, const CloseFn& close);

    /// One pass of expiry + loss detection. Called from the single sweep timer.
    void Sweep();

    Config config_;
    const KernelRegistry& kernels_;
    JobManager& jobs_;
    Fleet& fleet_;
    ReferenceStats* reference_stats_ = nullptr;
    OnCompleteFn on_complete_;
};

}  // namespace p2pgpu::coordinator
