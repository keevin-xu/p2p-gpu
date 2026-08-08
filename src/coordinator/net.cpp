// uWebSockets HTTP + WebSocket server — step 1.11.
//
// The coordinator's entire input surface is attacker-controlled: anyone can
// connect. Nothing in this file interprets a byte itself — every frame goes
// through protocol::VerifyFrame, which is the ONLY sanctioned bytes->typed path
// (R11). This file is transport plumbing and nothing else.

#include "p2pgpu/coordinator/net.hpp"

#include "p2pgpu/coordinator/sizer.hpp"
#include "p2pgpu/protocol/encode.hpp"

#include <chrono>
#include <cstdlib>
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

/// Open SSE connections. Raw `HttpResponse*` because uWebSockets owns them;
/// `onAborted` is what keeps this list from holding a dead one, and it is
/// registered at the same moment the pointer is stored so there is no window
/// where a client is tracked but not watched.
struct Server::SseClients {
    std::vector<uWS::HttpResponse<false>*> open;
};

Server::Server(Config config, const KernelRegistry& kernels, JobManager& jobs,
               Fleet& fleet, ReferenceStats* reference_stats, Store* store,
               EventLog* events)
    : config_(std::move(config)), kernels_(kernels), jobs_(jobs), fleet_(fleet),
      reference_stats_(reference_stats), store_(store), events_(events),
      sse_(std::make_unique<SseClients>()) {
    // Parsed once, here, so an unrecognised value is caught at startup rather
    // than silently behaving as `none` for a whole experiment — which would
    // look exactly like "replication does not help".
    if (config_.replication == "fixed2x") {
        quorum_.policy = ReplicationPolicy::Fixed2x;
    } else if (config_.replication == "adaptive") {
        quorum_.policy = ReplicationPolicy::Adaptive;
    } else if (config_.replication != "none") {
        spdlog::error("unknown --replication '{}'; using none", config_.replication);
    }
    spdlog::info("replication policy={} spot_checks={}", ToString(quorum_.policy),
                 config_.spot_checks ? "on" : "off");
    if (quorum_.policy == ReplicationPolicy::Adaptive && !config_.spot_checks) {
        // Said out loud, because the combination is a real hole rather than a
        // tuning choice: at `trusted_at` a worker's result is accepted from a
        // single submission, so a worker that behaves until trusted and then
        // defects is invisible. Measured at 155 corrupted results accepted
        // with zero blacklists (D-0059).
        spdlog::warn("adaptive replication WITHOUT spot-checks: trusted workers are "
                     "not validated at all. Add --spot-checks.");
    }
}

Server::~Server() = default;

void Server::PublishMetrics(std::uint64_t now_ms) {
    if (sse_->open.empty()) {
        return;
    }
    // Collected ONCE for all subscribers. Two dashboards must not see two
    // different instants and disagree about the same moment.
    const std::string payload =
        "data: " + ToJson(Collect(jobs_, fleet_, rejected_frames_total_, now_ms)) + "\n\n";
    for (auto* res : sse_->open) {
        res->write(payload);
    }
}

void Server::Sweep() {
    const auto now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    // 2.7 — expiry. ONE linear pass over the tasks, from ONE timer. At 10k
    // tasks that is a scan every second versus 10k live timers
    // (CONVENTIONS.md §4).
    const auto expired = jobs_.SweepExpiredLeases(now_ms);
    for (const auto& e : expired) {
        // AN EXPIRY IS EVIDENCE, not just a requeue (D-0044). The task took at
        // least a full lease, so it was sized too large — feed that back or the
        // correction factor only ever learns from tasks that COMPLETED, which
        // is exactly the set that excludes "we got this badly wrong".
        //
        // Without this a worker whose first task is wildly oversized expires
        // forever: it never completes anything, so nothing ever corrects.
        if (WorkerRecord* rec = fleet_.Mutable(e.holder);
            rec != nullptr && rec->predicted_ms > 0.0) {
            // A LOWER BOUND. We know it took at least the lease; we do not know
            // how much more. Honest, and enough to push the correction the
            // right way.
            rec->correction = UpdateCorrection(rec->correction, rec->predicted_ms,
                                               static_cast<double>(config_.lease_ms));
            rec->predicted_ms = 0.0;
        }
        if (events_ != nullptr) {
            events_->Expire(now_ms, e.task, e.holder, e.unit_count);
        }
    }
    if (!expired.empty()) {
        // info, not warn. An expired lease is the NORMAL case for a volunteer
        // grid (R8) — logging it as a warning would train everyone to ignore
        // warnings.
        spdlog::info("lease_expiry count={} requeued penalty=none", expired.size());
    }

    // 2.8 — worker loss. Detected on OUR clock from when we last received a
    // frame, never from a worker's self-report: a worker that claims to be
    // alive is exactly what a hung worker also does (PROTOCOL.md §5).
    for (const WorkerId lost : fleet_.FindLost(now_ms, config_.worker_timeout_ms)) {
        const std::size_t released = jobs_.ReleaseAllHeldBy(lost);
        fleet_.Leave(lost);
        Unregister(lost);
        if (events_ != nullptr) {
            events_->WorkerLost(now_ms, lost);
        }
        spdlog::info("worker_lost worker={} released={} penalty=none", lost.hi(),
                     released);
    }

    // 2.17 — PUSH pending revokes (D-0046).
    //
    // This must happen here rather than on the loser's next inbound message,
    // because a worker executing a task it has already lost the race for sends
    // nothing at all. Piggybacking delivered the stop AFTER the wasted work was
    // finished, which is a mechanism that reports a saving it does not make.
    //
    // Same reasoning as the expiry pass directly above: anything the
    // coordinator must say on its own initiative belongs on the timer.
    std::size_t revoked = 0;
    for (auto& [worker, conn] : live_) {
        if (conn.session == nullptr) {
            continue;
        }
        for (const auto& frame : conn.session->DrainRevokes()) {
            conn.send(frame);
            ++revoked;
        }
    }
    if (revoked > 0) {
        spdlog::info("revoke_push count={}", revoked);
    }

    // ── STALL DETECTION ──────────────────────────────────────────────────
    //
    // Work is outstanding, workers are connected, and NOT ONE task is leased.
    // That combination is not slowness — it means every remaining task is
    // ineligible for every connected worker, and the fleet will sit there
    // forever without a word.
    //
    // Reproduced deliberately: one worker under `fixed2x`. It computes a task,
    // needs a second opinion, and invariant 6 forbids giving it its own replica
    // — so it asks every ~3.4 s, is refused every time, and the job can never
    // complete. The worker is behaving correctly and the coordinator is
    // behaving correctly; the run is still dead. Before this it was visible
    // only as `granted=0` at DEBUG level, which is why the same symptom got
    // misfiled as "the worker went silent" (docs/PENDING.md).
    //
    // Warn ONCE per stall, not per sweep: a message repeated three times a
    // second is one nobody reads. `stalled_sweeps_` resets the moment anything
    // is leased again, so a recovered stall reports again if it recurs.
    {
        const auto by_state = jobs_.CountByState();
        const auto leased = by_state[static_cast<std::size_t>(TaskState::Leased)];
        const bool work_outstanding = !jobs_.AllComplete();
        if (!live_.empty() && work_outstanding && leased == 0) {
            ++stalled_sweeps_;
            if (stalled_sweeps_ == kStallSweeps) {
                spdlog::warn(
                    "STALLED workers={} queued={} needs_replica={} validating={} "
                    "units_remaining={} — work is outstanding and nothing is "
                    "leased; every remaining task is ineligible for every "
                    "connected worker (commonly: a replica this fleet is too "
                    "small to supply, invariant 6)",
                    live_.size(), by_state[static_cast<std::size_t>(TaskState::Queued)],
                    by_state[static_cast<std::size_t>(TaskState::NeedsReplica)],
                    by_state[static_cast<std::size_t>(TaskState::Validating)],
                    jobs_.remaining_units());
            }
        } else {
            if (stalled_sweeps_ >= kStallSweeps) {
                spdlog::info("stall cleared after {} sweeps", stalled_sweeps_);
            }
            stalled_sweeps_ = 0;
        }
    }

    // Signalling windows reset per sweep (6.1). Counted per SESSION rather than
    // fleet-wide: the limit exists so one worker cannot flood another, and a
    // shared budget would let a busy honest negotiation starve everyone else.
    for (auto& [worker, conn] : live_) {
        if (conn.session != nullptr) {
            conn.session->ResetSignalWindow();
        }
    }

    // 2.19 — durability, ONE transaction per sweep (D-0048). Last in the pass,
    // deliberately: expiry and revocation both mutate task state, so flushing
    // first would write rows this same tick is about to change and leave the
    // file a sweep behind for no reason.
    if (store_ != nullptr && jobs_.has_dirty()) {
        if (const auto s = store_->Flush(jobs_.DirtyJobs(), jobs_.DirtyTasks()); !s) {
            // NOT cleared. The rows stay dirty and go out next sweep; dropping
            // them here would let the file diverge from memory with nothing to
            // show for it. Warn rather than fatal — the coordinator is still
            // correct in memory, it has just lost its ability to survive a
            // restart, and killing a live fleet over that is worse.
            spdlog::warn("store flush failed: {}", s.error().message);
        } else {
            jobs_.ClearDirty();
        }
    }

    // 2.21 — publish LAST, so a subscriber sees the state after this whole tick
    // rather than a half-applied one.
    PublishMetrics(now_ms);
}

void Server::Register(WorkerId id, Session* session, SendFn send) {
    live_[id] = LiveConn{session, std::move(send)};
}

void Server::Unregister(WorkerId id) { live_.erase(id); }

void Server::Run() {
    std::uint64_t next_conn_id = 1;
    us_listen_socket_t* listen_socket = nullptr;

    // DEV ONLY (step 1.26). Checked after every loop iteration: once the
    // seeded work is done, close the listen socket so run() returns and main()
    // can print its summary. Without this the harness would have to be killed,
    // and a killed process prints nothing.
    //
    // Guarded on having had at least one task, so an empty queue at startup is
    // not mistaken for "finished".
    // THE ONE TIMER (2.7/2.8). A post-handler was considered and rejected: it
    // runs only when the loop wakes, so a fleet that has gone completely silent
    // — precisely the case expiry exists for — would never be swept.
    // R11 AUDIT (4.16): the three reinterpret_casts below are uWebSockets
    // plumbing — a loop handle and a pointer stashed in the timer's user-data
    // slot. They touch NO network bytes, and the library's C API offers no
    // other way to carry `this` into a C callback. Kept, and justified here
    // rather than removed.
    us_timer_t* sweep_timer = us_create_timer(
        reinterpret_cast<us_loop_t*>(uWS::Loop::get()), 0, sizeof(Server*));
    *reinterpret_cast<Server**>(us_timer_ext(sweep_timer)) = this;
    us_timer_set(
        sweep_timer,
        [](us_timer_t* t) { (*reinterpret_cast<Server**>(us_timer_ext(t)))->Sweep(); },
        static_cast<int>(config_.sweep_interval_ms),
        static_cast<int>(config_.sweep_interval_ms));

    if (config_.exit_when_complete) {
        bool fired = false;
        uWS::Loop::get()->addPostHandler(this, [this, &listen_socket, &fired](uWS::Loop*) {
            // ONCE. Without the guard this fires on every loop iteration for as
            // long as the process lives — observed, and it buried the log.
            if (fired || jobs_.total_tasks() == 0 || !jobs_.AllComplete()) {
                return;
            }
            fired = true;
            spdlog::info("all {} tasks terminal; shutting down (--exit-when-complete)",
                         jobs_.total_tasks());
            if (listen_socket != nullptr) {
                us_listen_socket_close(0, listen_socket);
                listen_socket = nullptr;
            }

            // Final flush. This path calls std::exit below, so without it the
            // last sweep interval of transitions never reaches the file —
            // measured as 410 tasks terminal in memory against 408 rows on
            // disk. That gap is exactly the loss D-0048 permits and it is
            // harmless (the work is redone), but a dev harness whose summary
            // does not reconcile with its own database invites an hour of
            // hunting for data loss that is not there.
            if (store_ != nullptr && jobs_.has_dirty()) {
                if (const auto s = store_->Flush(jobs_.DirtyJobs(), jobs_.DirtyTasks());
                    !s) {
                    spdlog::warn("final store flush failed: {}", s.error().message);
                } else {
                    jobs_.ClearDirty();
                }
            }

            const int rc = on_complete_ ? on_complete_() : 0;

            // std::exit, and it is a deliberate shortcut CONFINED TO THIS
            // DEV-ONLY FLAG. Closing the listen socket stops new connections
            // but does not end the loop while a worker is still attached, and
            // the harness must terminate to report its verdict. Ending the
            // fleet cleanly means sending Shutdown to every socket and waiting,
            // which is real work (2.x) and not worth building for a test
            // harness that has already finished measuring.
            //
            // A real coordinator NEVER takes this path: exit_when_complete is
            // off by default and this is unreachable without it.
            std::exit(rc);
        });
    }

    uWS::App()
        // Liveness only — deliberately says nothing about readiness or fleet
        // state. A health endpoint that can fail for interesting reasons is a
        // health endpoint that will page you for uninteresting ones.
        .get("/health",
             [](auto* res, auto* /*req*/) {
                 res->writeHeader("Content-Type", "text/plain")->end("ok");
             })

        // 2.21 — metrics stream. Server-Sent Events, not a WebSocket: this is
        // one-directional and read-only, and SSE reconnects on its own, so the
        // dashboard needs no retry logic of its own.
        //
        // CORS is REQUIRED here for the same reason it is on /kernel/:id (1.23):
        // the dashboard is served from a different origin (tools/serve.py on
        // :8000) than this coordinator, and without the header the fetch fails
        // with a console error that looks nothing like its cause.
        .get("/metrics/stream",
             [this](auto* res, auto* /*req*/) {
                 res->writeHeader("Content-Type", "text/event-stream")
                     ->writeHeader("Cache-Control", "no-cache")
                     ->writeHeader("Connection", "keep-alive")
                     ->writeHeader("Access-Control-Allow-Origin", "*");

                 // Registered together with onAborted, so there is never a
                 // moment where this pointer is in the list but unwatched — the
                 // sweep would then write to a freed response.
                 this->sse_->open.push_back(res);
                 res->onAborted([this, res] {
                     std::erase(this->sse_->open, res);
                 });

                 // One immediate snapshot so a dashboard opened between sweeps
                 // is not blank for up to a full interval.
                 res->write("data: " +
                            ToJson(Collect(this->jobs_, this->fleet_,
                                           this->rejected_frames_total_, 0)) +
                            "\n\n");
             })

        // Point-in-time metrics, for a script or a curl. Same collector as the
        // stream, so the two cannot report different things.
        .get("/metrics",
             [this](auto* res, auto* /*req*/) {
                 res->writeHeader("Content-Type", "application/json")
                     ->writeHeader("Access-Control-Allow-Origin", "*")
                     ->end(ToJson(Collect(this->jobs_, this->fleet_,
                                          this->rejected_frames_total_, 0)));
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

        // Serve a bulk asset by CONTENT ADDRESS (step 5.4). Phase 6 puts a
        // peer-to-peer plane in front of this; it stays as the fallback, which
        // is only sound because the name of the thing is its hash — a peer and
        // the coordinator are interchangeable sources of a verified blob
        // (D-0007, D-0069).
        .get("/asset/:hash",
             [this](auto* res, auto* req) {
                 const std::string_view hash = req->getParameter(0);
                 // `Find` validates the shape before touching the container, so
                 // a malformed key cannot express anything at all (R11). One
                 // response for "absent" and "malformed" together, and no echo
                 // of the requested hash — reflecting attacker text is a habit
                 // worth not forming, and distinguishing the two cases would
                 // leak whether an asset exists.
                 const std::vector<std::byte>* blob = assets_.Find(hash);
                 if (blob == nullptr) {
                     res->writeStatus("404 Not Found")->end("unknown asset");
                     return;
                 }
                 // Same CORS + CORP pair as /kernel/:id, and required for the
                 // same reason: the worker page is cross-origin isolated, so
                 // under `COEP: require-corp` the browser blocks any
                 // cross-origin subresource that does not opt in. Without both
                 // headers this fails as a console CORS error and surfaces as
                 // "asset unavailable", nowhere near the cause (1.23, RISKS.md).
                 //
                 // IMMUTABLE, and safely so: the URL contains the hash of the
                 // body, so the bytes at an address can never change. This is
                 // the one place in the system where an infinite cache lifetime
                 // is a theorem rather than a gamble.
                 res->writeHeader("Content-Type", "application/octet-stream")
                     ->writeHeader("Access-Control-Allow-Origin", "*")
                     ->writeHeader("Cross-Origin-Resource-Policy", "cross-origin")
                     ->writeHeader("Cache-Control", "public, max-age=31536000, immutable")
                     ->end(std::string_view(reinterpret_cast<const char*>(blob->data()),
                                            blob->size()));
             })

        // The composited image (5.15), for the dashboard's progressive display
        // (5.16) and the demo capture (5.23).
        //
        // RAW RGBA8 behind a 16-byte header rather than PNG: a PNG encoder is a
        // dependency (or ~200 lines of DEFLATE) to produce something the page
        // immediately decodes back to exactly these bytes for a canvas. The
        // stack table is short on purpose (D-0008).
        //
        // Explicitly NOT cacheable — the opposite of /asset/{hash}, and for the
        // opposite reason: that URL names its own content and can never change,
        // this one names a moment and changes every sweep.
        .get("/render",
             [this](auto* res, auto*) {
                 const auto rgba = compositor_.RenderRgba();
                 const auto& g = compositor_.grid();
                 std::array<std::uint32_t, 4> head{0x50324752U, g.image_w, g.image_h, 0};
                 std::string body(sizeof(head) + rgba.size(), '\0');
                 std::memcpy(body.data(), head.data(), sizeof(head));
                 if (!rgba.empty()) {
                     std::memcpy(body.data() + sizeof(head), rgba.data(), rgba.size());
                 }
                 res->writeHeader("Content-Type", "application/octet-stream")
                     ->writeHeader("Access-Control-Allow-Origin", "*")
                     ->writeHeader("Cross-Origin-Resource-Policy", "cross-origin")
                     ->writeHeader("Cache-Control", "no-store")
                     ->end(body);
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
                    data->session = std::make_unique<Session>(
                        this->jobs_, this->kernels_, this->fleet_, data->conn_id,
                        this->config_.lease_ms, this->reference_stats_);
                    data->session->SetEventLog(this->events_);
                    data->session->SetSpeculation(this->config_.speculation);
                    data->session->SetReputation(&this->reputation_);
                    data->session->SetQuorum(this->quorum_);
                    data->session->SetCompositor(&this->compositor_);
                    data->session->SetAssetStore(&this->assets_);
                    data->session->SetIceServers(this->config_.ice_servers);
                    // Cross-connection delivery for signalling (6.1, D-0085).
                    // `live_` is the WorkerId -> connection map D-0046 already
                    // built for revoke pushes; this reuses it rather than
                    // keeping a second registry that could disagree.
                    data->session->SetPeerRelay(
                        [this](WorkerId to, std::span<const std::byte> frame) {
                            const auto it = this->live_.find(to);
                            if (it == this->live_.end() || it->second.send == nullptr) {
                                return false;
                            }
                            it->second.send(frame);
                            return true;
                        });
                    if (this->config_.spot_checks) {
                        data->session->SetSpotChecks(&this->spot_checks_);
                    }
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
                    // ws->end() DESTROYS the socket and its user data, so
                    // nothing below may touch `data` or `ws` once the close
                    // callback has run. Tracked explicitly rather than
                    // inferred: 4.15's soak segfaulted the coordinator here
                    // (rc=139) the first time the 3.12 rate limiter evicted a
                    // connection, because the D-0046 registration below then
                    // dereferenced a freed Session.
                    //
                    // Neither change was wrong alone — 3.12 added the eviction,
                    // D-0046 added the post-frame read — and only a sustained
                    // hostile run reaches 64 malformed frames on one connection
                    // to put them together.
                    bool closed = false;
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
                                  [ws, &closed] {
                                      closed = true;
                                      ws->end(1002, "fatal");
                                  });

                    if (closed) {
                        return;   // `data` and `ws` are gone
                    }

                    // Register once the handshake has established an identity
                    // (D-0046). Done AFTER the frame, not in `.open`, because
                    // `worker_id` is unknown until `Hello` arrives — and keyed
                    // by worker rather than connection because that is what a
                    // pending revoke is addressed to.
                    if (const WorkerId id = data->session->worker_id();
                        id != WorkerId{}) {
                        this->Register(id, data->session.get(),
                                       [ws](std::span<const std::byte> frame) {
                                           ws->send(std::string_view(
                                                        static_cast<const char*>(
                                                            static_cast<const void*>(
                                                                frame.data())),
                                                        frame.size()),
                                                    uWS::OpCode::BINARY);
                                       });
                    }
                },

                .close = [this](auto* ws, int code, std::string_view /*message*/) {
                    // Unregister BEFORE the session is destroyed. This handler
                    // runs for every way a connection can end, which is the
                    // whole reason `live_` may hold a raw `Session*` at all
                    // (D-0046) — miss a path here and the sweep writes through
                    // a dangling pointer.
                    if (ws->getUserData()->session) {
                        this->Unregister(ws->getUserData()->session->worker_id());
                    }
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
                [this, &listen_socket](auto* token) {
                    if (token != nullptr) {
                        listen_socket = token;
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
        ++rejected_frames_total_;

        // 3.12 — escalate. Nothing here touches reputation: the peer may be a
        // broken client, and blaming its RESULTS for its framing is how an
        // honest-but-buggy worker gets blacklisted (3.11).
        // Tier 1: refuse work, keep the connection. A broken client may yet
        // start framing correctly, and evicting it immediately would punish a
        // bug the same as an attack.
        if (rejected >= kRejectedFramesNoWork) {
            if (rejected == kRejectedFramesNoWork) {
                spdlog::warn("rate_limit conn_id={} rejected={} action=no_work",
                             conn_id, rejected);
            }
            session.SetThrottledForAbuse(true);
        }

        // Tier 2: evict.
        if (rejected >= kRejectedFramesDisconnect) {
            spdlog::warn("rate_limit conn_id={} rejected={} action=disconnect",
                         conn_id, rejected);
            const auto frame = protocol::EncodeMessage(
                wire::Body::Error, [](flatbuffers::FlatBufferBuilder& fbb) {
                    auto msg = fbb.CreateString("too many malformed frames");
                    wire::ErrorBuilder eb(fbb);
                    eb.add_code(wire::ErrorCode::RateLimited);
                    eb.add_message(msg);
                    eb.add_fatal(true);
                    return eb.Finish();
                });
            send(frame);
            close();
            return;
        }
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
    // ONE WebSocket message per frame. Our framing is one-frame-per-message, so
    // concatenating would make the receiver read the second frame as a trailing
    // payload and reject it as an orphan.
    for (const auto& out : reaction.replies) {
        // `out`, not `frame` — the inbound frame is still in scope above, and
        // -Wshadow is on precisely so "which frame?" is never a question.
        send(out);
    }
    if (reaction.close) {
        // Fatal errors close the connection. A peer that cannot succeed by
        // retrying must be told to stop rather than left reconnecting forever
        // (PROTOCOL.md §5).
        close();
    }
}

}  // namespace p2pgpu::coordinator
