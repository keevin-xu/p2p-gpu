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
    std::string manifest = "kernels/manifest.toml";
    std::string kernel_dir = "kernels";
    std::string log_level = "info";
};

class Server {
public:
    Server(Config config, const KernelRegistry& kernels, JobManager& jobs);

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

    Config config_;
    const KernelRegistry& kernels_;
    JobManager& jobs_;
};

}  // namespace p2pgpu::coordinator
