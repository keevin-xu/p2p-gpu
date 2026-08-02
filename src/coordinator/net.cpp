// uWebSockets HTTP + WebSocket server — step 1.11.
//
// The coordinator's entire input surface is attacker-controlled: anyone can
// connect. Nothing in this file interprets a byte itself — every frame goes
// through protocol::VerifyFrame, which is the ONLY sanctioned bytes->typed path
// (R11). This file is transport plumbing and nothing else.

#include "p2pgpu/coordinator/net.hpp"

#include <chrono>
#include <memory>
#include <vector>

#include <spdlog/spdlog.h>
#include <uwebsockets/App.h>

#include <string_view>

namespace p2pgpu::coordinator {
namespace {

/// Per-connection state hung off the socket by uWebSockets.
struct SocketData {
    std::uint64_t conn_id = 0;
    /// Owned by the socket so it dies with the connection. unique_ptr because
    /// uWebSockets moves its user-data around and Session is not movable.
    std::unique_ptr<Session> session;
    /// Consecutive rejected frames. Phase 3 step 3.12 turns this into escalating
    /// backoff then disconnect. It is CONNECTION hygiene, deliberately separate
    /// from task reputation — conflating them is how honest-but-buggy clients
    /// get blacklisted (step 3.11).
    std::uint32_t rejected_frames = 0;
};

}  // namespace

Server::Server(Config config, const KernelRegistry& kernels, JobManager& jobs)
    : config_(std::move(config)), kernels_(kernels), jobs_(jobs) {}

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
                 // CORS + CORP, and both are REQUIRED for the browser worker
                 // to fetch this at all (step 1.23).
                 //
                 // The worker page is cross-origin isolated — it must be, for
                 // SharedArrayBuffer — which means it is served with
                 // `Cross-Origin-Embedder-Policy: require-corp`. Under that
                 // policy the browser BLOCKS any cross-origin subresource that
                 // does not explicitly opt in, and the page (a CDN, or
                 // localhost:8000 in development) is a different origin from
                 // this coordinator. Without these two headers the fetch fails
                 // with a console CORS error, the worker reports the kernel
                 // unavailable, and it looks like a registry problem.
                 //
                 // `*` is correct here rather than lax: WGSL source is public,
                 // non-secret, and the entire system depends on arbitrary
                 // browsers being able to read it. Nothing behind this endpoint
                 // is privileged, and it carries no credentials.
                 res->writeHeader("Content-Type", "text/plain; charset=utf-8")
                     ->writeHeader("Access-Control-Allow-Origin", "*")
                     ->writeHeader("Cross-Origin-Resource-Policy", "cross-origin")
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

                .open = [this, &next_conn_id](auto* ws) {
                    auto* data = ws->getUserData();
                    data->conn_id = next_conn_id++;
                    data->session = std::make_unique<Session>(this->jobs_, this->kernels_,
                                                              data->conn_id);
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
                    this->OnFrame(*data->session, data->conn_id,
                                  data->rejected_frames, msg,
                                  [ws](std::span<const std::byte> reply) {
                                      // static_cast via void*, never
                                      // reinterpret_cast (R11): uWebSockets
                                      // takes a string_view and our frames are
                                      // std::byte.
                                      ws->send(std::string_view(
                                                   static_cast<const char*>(
                                                       static_cast<const void*>(reply.data())),
                                                   reply.size()),
                                               uWS::OpCode::BINARY);
                                  },
                                  [ws] { ws->end(1002, "fatal"); });
                },

                .close = [](auto* ws, int code, std::string_view /*message*/) {
                    // Release held leases immediately (R8) rather than waiting
                    // out the lease — we already know the worker is gone.
                    if (ws->getUserData()->session) {
                        ws->getUserData()->session->OnDisconnect();
                    }
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

void Server::OnFrame(Session& session, std::uint64_t conn_id, std::uint32_t& rejected,
                     std::string_view bytes, const SendFn& send, const CloseFn& close) {
    // THE ONLY ROUTE FROM BYTES TO FIELDS (R11). Nothing above this line has
    // looked at the contents, and nothing below it may look at them any other
    // way.
    std::span<const std::byte> frame{
        static_cast<const std::byte*>(static_cast<const void*>(bytes.data())),
        bytes.size()};

    // OUR precondition, not the peer's (D-0027): the Envelope begins at
    // frame + 16 and needs 8-byte alignment, which only holds if the frame
    // itself is aligned. uWebSockets makes no such promise about the view it
    // hands us, so normalise before parsing. `scratch` is written to only on
    // the unaligned path and must outlive `frame`.
    std::vector<std::byte> scratch;
    frame = protocol::AlignFrame(frame, scratch);

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

    const auto now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    const Reaction reaction = session.OnMessage(*verified, now_ms);
    if (!reaction.reply.empty()) {
        send(reaction.reply);
    }
    if (reaction.close) {
        // Fatal errors close the connection. A peer that cannot succeed by
        // retrying must be told to stop rather than left reconnecting forever
        // (PROTOCOL.md §5).
        close();
    }
}

}  // namespace p2pgpu::coordinator
