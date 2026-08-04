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
#include <memory>
#include <unordered_map>

#include "p2pgpu/coordinator/job.hpp"
#include "p2pgpu/coordinator/kernel_registry.hpp"
#include "p2pgpu/coordinator/fleet.hpp"
#include "p2pgpu/coordinator/reference_check.hpp"
#include "p2pgpu/coordinator/session.hpp"
#include "p2pgpu/coordinator/metrics.hpp"
#include "p2pgpu/coordinator/store.hpp"
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

    /// SQLite file for durable state (2.19). Empty disables persistence
    /// entirely, which is what every test and mock run uses — a harness that
    /// silently accumulated a database between runs would make each run depend
    /// on the last.
    std::string store_path;
};

class Server {
public:
    /// `reference_stats` is non-null only under --verify-reference (step
    /// 1.26). A TEST HARNESS, not validation — see reference_check.hpp.
    /// `store` is optional (null disables persistence). Non-owning: `main`
    /// owns it, so the lifetime is obvious at the one place both are created.
    Server(Config config, const KernelRegistry& kernels, JobManager& jobs,
           Fleet& fleet, ReferenceStats* reference_stats = nullptr,
           Store* store = nullptr);

    /// Out-of-line so `SseClients` may stay incomplete in this header.
    ~Server();

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

    /// One pass of expiry + loss detection + revoke delivery. Called from the
    /// single sweep timer.
    void Sweep();

    /// A live connection, once its worker identity is known.
    ///
    /// This exists so the coordinator can SPEAK FIRST (D-0046). Everything else
    /// here is request/response — a worker sends, a session replies — and a
    /// revoke is the one message that must reach a worker that is deliberately
    /// silent, because it is busy computing the work we want it to abandon.
    struct LiveConn {
        Session* session = nullptr;
        SendFn send;
    };

    /// Registered on handshake, erased on close. Raw `Session*` is safe only
    /// because the close handler runs for every way a connection can end and is
    /// the sole owner's teardown — see `Register`/`Unregister`.
    std::unordered_map<protocol::WorkerId, LiveConn> live_;

    void Register(protocol::WorkerId id, Session* session, SendFn send);
    void Unregister(protocol::WorkerId id);

    /// Open SSE connections for the dashboard (2.21). Opaque here so
    /// uWebSockets does not leak into a header that coordinator-core includes —
    /// the same reason `Session` is transport-free.
    struct SseClients;
    std::unique_ptr<SseClients> sse_;

    /// Fleet-wide rejected frames (2.21). CONNECTION hygiene, and deliberately
    /// not task reputation: conflating the two is how an honest-but-buggy
    /// client gets blacklisted (3.11).
    std::uint64_t rejected_frames_total_ = 0;

    /// Push one snapshot to every open SSE connection.
    void PublishMetrics(std::uint64_t now_ms);

    Config config_;
    const KernelRegistry& kernels_;
    JobManager& jobs_;
    Fleet& fleet_;
    ReferenceStats* reference_stats_ = nullptr;
    Store* store_ = nullptr;
    OnCompleteFn on_complete_;
};

}  // namespace p2pgpu::coordinator
