// uWebSockets HTTP + WebSocket server — step 1.11.
//
// The coordinator's entire input surface is attacker-controlled: anyone can
// connect. Nothing in this file interprets a byte itself — every frame goes
// through protocol::VerifyFrame, which is the ONLY sanctioned bytes->typed path
// (R11). This file is transport plumbing and nothing else.

#include "p2pgpu/coordinator/net.hpp"

#include <spdlog/spdlog.h>
#include <uwebsockets/App.h>

#include <string_view>

namespace p2pgpu::coordinator {
namespace {

/// Per-connection state hung off the socket by uWebSockets.
struct SocketData {
    std::uint64_t conn_id = 0;
    /// Consecutive rejected frames. Phase 3 step 3.12 turns this into escalating
    /// backoff then disconnect. It is CONNECTION hygiene, deliberately separate
    /// from task reputation — conflating them is how honest-but-buggy clients
    /// get blacklisted (step 3.11).
    std::uint32_t rejected_frames = 0;
};

}  // namespace

Server::Server(Config config, const KernelRegistry& kernels)
    : config_(std::move(config)), kernels_(kernels) {}

void Server::Run() {
    std::uint64_t next_conn_id = 1;

    uWS::App()
        // Liveness only — deliberately says nothing about readiness or fleet
        // state. A health endpoint that can fail for interesting reasons is a
        // health endpoint that will page you for uninteresting ones.
        .get("/health",
             [](auto* res, auto* /*req*/) {
                 res->writeHeader("Content-Type", "text/plain")->end("ok");
             })

        // Serve WGSL by kernel id (step 1.12). Workers fetch the source they
        // were told about in `Welcome`; the manifest is the single registry.
        .get("/kernel/:id",
             [this](auto* res, auto* req) {
                 const std::string_view id = req->getParameter(0);
                 const KernelSpec* spec = kernels_.Find(id);
                 if (spec == nullptr) {
                     // 404 with no echo of the requested id: reflecting
                     // attacker-supplied text into a response is a habit worth
                     // not forming, even where it is currently harmless.
                     res->writeStatus("404 Not Found")->end("unknown kernel");
                     return;
                 }
                 res->writeHeader("Content-Type", "text/plain; charset=utf-8")
                     ->end(spec->wgsl);
             })

        .ws<SocketData>("/ws",
            {
                .compression = uWS::DISABLED,
                // Bounds the largest frame we will assemble at all. The
                // protocol's own caps are enforced later by SplitFrame; this is
                // the transport refusing to buffer something absurd before we
                // ever get a chance to reject it.
                .maxPayloadLength = kMaxFrameBytes,
                .idleTimeout = 120,
                .maxBackpressure = 16 * 1024 * 1024,

                .open = [&next_conn_id](auto* ws) {
                    auto* data = ws->getUserData();
                    data->conn_id = next_conn_id++;
                    // Correlation fields from CONVENTIONS.md §6. worker_id is
                    // unknown until Hello arrives, so conn_id carries the trace
                    // until then — without it, a frame rejected during the
                    // handshake is untraceable to a connection.
                    spdlog::info("conn_open conn_id={}", data->conn_id);
                },

                .message = [this](auto* ws, std::string_view msg, uWS::OpCode op) {
                    auto* data = ws->getUserData();

                    // Binary only. A text frame is either a confused client or a
                    // probe; either way it is not our protocol.
                    if (op != uWS::OpCode::BINARY) {
                        ++data->rejected_frames;
                        spdlog::warn("reject conn_id={} reason=non_binary_frame",
                                     data->conn_id);
                        return;
                    }
                    this->OnFrame(data->conn_id, data->rejected_frames, msg);
                },

                .close = [](auto* ws, int code, std::string_view /*message*/) {
                    // A worker vanishing is the NORMAL case (R8), so this is
                    // info, not warn. Lease release on disconnect lands in 2.8.
                    spdlog::info("conn_close conn_id={} code={}",
                                 ws->getUserData()->conn_id, code);
                },
            })

        .listen(config_.port,
                [this](auto* token) {
                    if (token != nullptr) {
                        spdlog::info("coordinator listening port={} kernels={}",
                                     config_.port, kernels_.size());
                    } else {
                        spdlog::error("failed to bind port={}", config_.port);
                    }
                })
        .run();
}

void Server::OnFrame(std::uint64_t conn_id, std::uint32_t& rejected,
                     std::string_view bytes) {
    // THE ONLY ROUTE FROM BYTES TO FIELDS (R11). Nothing above this line has
    // looked at the contents, and nothing below it may look at them any other
    // way.
    const std::span<const std::byte> frame{
        static_cast<const std::byte*>(static_cast<const void*>(bytes.data())),
        bytes.size()};

    const auto verified = protocol::VerifyFrame(frame);
    if (!verified) {
        ++rejected;
        // A Verifier rejection is always warn WITH the diagnostic detail
        // (CONVENTIONS.md §6): it is the primary signal for protocol bugs and
        // the first sign of an actual attack.
        spdlog::warn("reject conn_id={} code={} detail=\"{}\" len={} rejected_total={}",
                     conn_id, static_cast<int>(verified.error().code),
                     verified.error().message, bytes.size(), rejected);
        return;
    }

    // Routing arrives with the handshake in step 1.15. Until then, verifying and
    // counting is the whole job — and is already enough to satisfy Phase 2's
    // `malformed_frames` chaos profile, which only requires that the coordinator
    // reject every hostile frame and stay up.
    spdlog::debug("frame conn_id={} body_type={}", conn_id,
                  static_cast<int>(verified->body_type()));
}

}  // namespace p2pgpu::coordinator
