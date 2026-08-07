// Handshake (1.15) and result ingestion (1.16). Transport-free; see session.hpp.

#include "p2pgpu/coordinator/session.hpp"

#include <blake3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>

#include <cmath>

#include "p2pgpu/coordinator/params.hpp"
#include "p2pgpu/coordinator/sizer.hpp"
#include "p2pgpu/protocol/encode.hpp"
#include "p2pgpu/protocol/invariants.hpp"

namespace p2pgpu::coordinator {
namespace {

constexpr std::uint32_t kHeartbeatMs = 15000;

/// The join-time benchmark (2.11) runs the Phase 0 calibration kernel, whose
/// FLOP count is exactly known (D-0018) — which is what makes its score
/// comparable across devices at all.
constexpr const char* kCalibrationKernel = "calibrate_v1";
constexpr std::uint32_t kBenchmarkTargetMs = 200;

/// How long a task should take (ARCHITECTURE.md §7 says 1-3 s).
constexpr std::uint32_t kTargetTaskMs = 2000;

/// Ceiling on a self-reported score, in arithmetic ops/sec.
///
/// 1e15. The first version was 1e12 and **an Apple M4 Pro exceeded it on the
/// first real run** — 1.86e12 ops/s, matching D-0019's measured 1874 GFLOP/s.
/// The cap silently clipped a truthful score, which is the worst kind of
/// mis-tuned guard: it degrades an honest worker and reports nothing.
///
/// 1e15 is ~500x the fastest device measured here and still bounds a liar to
/// something finite. The point is refusing an obviously fabricated number
/// (NaN, 1e300), not second-guessing plausible hardware — reputation for
/// accuracy is Phase 3's job, and the EWMA correction already pulls an
/// optimistic score back toward reality.
constexpr double kMaxBenchmarkScore = 1.0e15;

/// Map the manifest's determinism to the wire union. No `default:` arm — adding
/// a class must break this build (ARCHITECTURE.md §5).
flatbuffers::Offset<void> BuildDeterminism(flatbuffers::FlatBufferBuilder& fbb,
                                           const KernelSpec& spec,
                                           wire::DeterminismClass& out_type) {
    switch (spec.determinism) {
        case Determinism::Exact: {
            out_type = wire::DeterminismClass::Exact;
            wire::ExactBuilder b(fbb);
            return b.Finish().Union();
        }
        case Determinism::Tolerant: {
            out_type = wire::DeterminismClass::Tolerant;
            wire::TolerantBuilder b(fbb);
            b.add_rel_eps(spec.rel_eps);
            b.add_abs_eps(spec.abs_eps);
            return b.Finish().Union();
        }
        case Determinism::Statistical: {
            out_type = wire::DeterminismClass::Statistical;
            wire::StatisticalBuilder b(fbb);
            return b.Finish().Union();
        }
    }
    out_type = wire::DeterminismClass::NONE;
    return {};
}

}  // namespace

std::uint64_t Blake3_64(std::span<const std::byte> bytes) noexcept {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, bytes.data(), bytes.size());
    std::array<std::uint8_t, 8> digest{};
    blake3_hasher_finalize(&h, digest.data(), digest.size());

    // Little-endian assembly so the value is stable across hosts — the checksum
    // travels on the wire and both ends must agree byte for byte.
    std::uint64_t out = 0;
    for (std::size_t i = 0; i < digest.size(); ++i) {
        out |= static_cast<std::uint64_t>(digest[i]) << (8U * i);
    }
    return out;
}

Session::Session(JobManager& jobs, const KernelRegistry& kernels, Fleet& fleet,
                 std::uint64_t conn_id, std::uint32_t lease_ms,
                 ReferenceStats* reference_stats)
    : jobs_(jobs), kernels_(kernels), fleet_(fleet), conn_id_(conn_id),
      lease_ms_(lease_ms), reference_stats_(reference_stats) {}

Reaction Session::Fatal(wire::ErrorCode code, const char* message) {
    return Reaction{
        {protocol::EncodeMessage(wire::Body::Error,
                                 [&](flatbuffers::FlatBufferBuilder& fbb) {
                                     auto msg = fbb.CreateString(message);
                                     wire::ErrorBuilder b(fbb);
                                     b.add_code(code);
                                     b.add_message(msg);
                                     b.add_fatal(true);
                                     return b.Finish();
                                 })},
        /*close=*/true};
}

Reaction Session::NonFatal(wire::ErrorCode code, const char* message) {
    return Reaction{
        {protocol::EncodeMessage(wire::Body::Error,
                                 [&](flatbuffers::FlatBufferBuilder& fbb) {
                                     auto msg = fbb.CreateString(message);
                                     wire::ErrorBuilder b(fbb);
                                     b.add_code(code);
                                     b.add_message(msg);
                                     b.add_fatal(false);
                                     return b.Finish();
                                 })},
        /*close=*/false};
}

Reaction Session::OnMessage(const protocol::VerifiedFrame& frame, std::uint64_t now_ms) {
    Reaction r = Dispatch(frame, now_ms);
    // Opportunistic ONLY. The sweep timer is what actually delivers revokes
    // (D-0046); if this worker happens to be talking to us anyway, it gets them
    // a little sooner. This was once the sole delivery path, which meant the
    // stop reached a worker only after it finished the work being cancelled —
    // a busy worker sends nothing, and that is precisely the worker being
    // revoked. Draining twice is safe: whichever runs first empties the queue.
    if (handshaked_) {
        for (auto& rev : DrainRevokes()) {
            r.replies.push_back(std::move(rev));
        }
    }
    return r;
}

Reaction Session::Dispatch(const protocol::VerifiedFrame& frame, std::uint64_t now_ms) {
    now_ms_ = now_ms;
    const wire::Envelope& env = *frame.envelope();

    // Hello must come first. Accepting work-bearing messages from an
    // unidentified connection would let anyone submit results without ever
    // claiming an identity — and reputation only means something if there is
    // someone to attach it to.
    if (!handshaked_ && env.body_type() != wire::Body::Hello) {
        return Fatal(wire::ErrorCode::Internal, "first message must be Hello");
    }

    // EVERY inbound frame is proof of life (2.8), not just an explicit
    // heartbeat. A worker returning results is self-evidently alive, and
    // requiring a separate keepalive from a busy one would be a way to declare
    // our fastest contributors dead.
    if (handshaked_) {
        fleet_.Touch(worker_id_, now_ms);
    }

    switch (env.body_type()) {
        case wire::Body::Hello:
            if (const auto* m = env.body_as_Hello()) {
                return OnHello(*m);
            }
            break;
        case wire::Body::LeaseRequest:
            if (const auto* m = env.body_as_LeaseRequest()) {
                return OnLeaseRequest(*m, now_ms);
            }
            break;
        case wire::Body::ResultHeader:
            if (const auto* m = env.body_as_ResultHeader()) {
                return OnResultHeader(*m, frame.payload());
            }
            break;

        case wire::Body::Release:
            if (const auto* m = env.body_as_Release()) {
                return OnRelease(*m);
            }
            break;
        case wire::Body::Progress:
            if (const auto* m = env.body_as_Progress()) {
                return OnProgress(*m, now_ms);
            }
            break;
        case wire::Body::Goodbye:
            return OnGoodbye();
        case wire::Body::AssetRequest:
            if (const auto* m = env.body_as_AssetRequest()) {
                return OnAssetRequest(*m);
            }
            break;
        case wire::Body::AssetChunk:
        case wire::Body::AssetMiss:
            // A worker does not serve assets to the coordinator. Rejected
            // rather than ignored: an unexpected message is a peer that
            // disagrees with us about the protocol, and silence would let it
            // keep sending them (3.12 counts rejections per connection).
            return NonFatal(wire::ErrorCode::MalformedMessage,
                            "workers do not send asset data to the coordinator");

        case wire::Body::BenchmarkResult:
            if (const auto* m = env.body_as_BenchmarkResult()) {
                return OnBenchmarkResult(*m);
            }
            break;
        case wire::Body::Throttle:
            if (const auto* m = env.body_as_Throttle()) {
                return OnThrottle(*m);
            }
            break;

        // Not yet implemented, but explicitly listed so that adding a Body
        // variant breaks THIS switch too (-Werror=switch). A silent default
        // would let a new message type be ignored without anyone noticing.
        case wire::Body::Signal:
            spdlog::debug("unhandled conn_id={} body_type={}", conn_id_,
                          static_cast<int>(env.body_type()));
            return {};

        // Coordinator -> worker messages. A worker sending one of these is
        // confused or probing; either way it is not something we accept.
        case wire::Body::Welcome:
        case wire::Body::BenchmarkRequest:
        case wire::Body::TaskGrant:
        case wire::Body::LeaseAck:
        case wire::Body::Revoke:
        case wire::Body::PeerList:
        case wire::Body::Shutdown:
        case wire::Body::Error:
        case wire::Body::NONE:
            return NonFatal(wire::ErrorCode::MalformedMessage,
                            "message is coordinator-to-worker only");
    }
    return {};
}

Reaction Session::OnHello(const wire::Hello& hello) {
    if (handshaked_) {
        return NonFatal(wire::ErrorCode::MalformedMessage, "duplicate Hello");
    }

    // Version check (1.15). FATAL: no negotiation, no compatibility shims
    // (PROTOCOL.md §5). Retrying with the same state cannot succeed, so the
    // peer is told to stop rather than left reconnecting forever.
    //
    // The frame header carries a version too and SplitFrame already checked it;
    // this is the in-band copy, inside the verified region. Deliberate
    // redundancy — the header check is the cheap pre-parse gate.
    if (hello.protocol_version() != protocol::kProtocolVersion) {
        spdlog::warn("handshake_reject conn_id={} reason=version_mismatch theirs={} ours={}",
                     conn_id_, hello.protocol_version(), protocol::kProtocolVersion);
        return Fatal(wire::ErrorCode::VersionMismatch, "protocol version mismatch");
    }

    handshaked_ = true;
    worker_id_ = WorkerId{conn_id_, 0};

    // 3.13 — resume a prior identity, if the worker offers a token we minted.
    //
    // An unknown or absent token is NOT an error: tokens expire, coordinators
    // restart, and a browser tab that has been closed for a week simply starts
    // fresh. Rejecting it would break exactly the honest reconnects this
    // exists for.
    //
    // AND IT DOES NOT MAKE BANS ENFORCEABLE (D-0056). A token is presented
    // voluntarily — an honest worker offers it to keep a hundred correct
    // results, a blacklisted one just omits it and arrives as a newcomer. What
    // stops that being profitable is that a newcomer is worth nothing: it
    // scores the prior and 3.8 replicates it until it has a record.
    if (reputation_ != nullptr && hello.resume_token() != nullptr) {
        const std::string token = hello.resume_token()->str();
        if (const auto prior = reputation_->ResolveToken(token)) {
            worker_id_ = *prior;
            spdlog::info("handshake conn_id={} RESUMED worker={} score={:.2f}",
                         conn_id_, worker_id_.hi(), reputation_->ScoreOf(worker_id_));
        }
    }
    // Stamp with the CURRENT time, not 0. A zero here means
    // `0 + timeout < now` on the very first sweep, so every worker is declared
    // lost within a second of connecting — and because it holds no leases yet,
    // `released=0` makes the log look harmless while the worker is silently
    // removed from the fleet and can never be granted work.
    fleet_.Join(worker_id_, conn_id_, now_ms_);

    spdlog::info("handshake conn_id={} worker_id={} kernels={}", conn_id_,
                 worker_id_.hi(), kernels_.size());

    // Welcome, then immediately BenchmarkRequest (2.11). Two frames rather than
    // folding the request into Welcome: the benchmark is a REQUEST the worker
    // answers, and a worker that ignores it simply gets no work — which is the
    // correct outcome and would be awkward to express as an unanswered field.
    auto welcome = protocol::EncodeMessage(
        wire::Body::Welcome, [&](flatbuffers::FlatBufferBuilder& fbb) {
            std::vector<flatbuffers::Offset<wire::KernelDescriptor>> descs;
            for (const auto* spec : kernels_.All()) {
                auto id = fbb.CreateString(spec->id);
                auto entry = fbb.CreateString(spec->entry_point);
                wire::DeterminismClass det_type = wire::DeterminismClass::NONE;
                auto det = BuildDeterminism(fbb, *spec, det_type);

                // D-0040: the bytes the worker must write into the result
                // buffer once per task. A reduction's identity element is part
                // of what the work IS, so the coordinator states it (R1) — the
                // worker knowing that brute_search wants 0xFFFFFFFF at offset 4
                // would be the first of many per-kernel branches in portable
                // code. A kernel property, so it travels here in Welcome rather
                // than in every TaskEnvelope.
                const auto init = BuildOutputInit(*spec);
                auto init_vec = fbb.CreateVector(
                    static_cast<const std::uint8_t*>(static_cast<const void*>(init.data())),
                    init.size());

                wire::OutputSpecBuilder ob(fbb);
                ob.add_bytes(spec->output_bytes);
                ob.add_init(init_vec);
                auto out = ob.Finish();

                const wire::WorkgroupSize wg{spec->workgroup_size[0],
                                             spec->workgroup_size[1],
                                             spec->workgroup_size[2]};
                wire::KernelDescriptorBuilder kb(fbb);
                kb.add_kernel_id(id);
                kb.add_entry_point(entry);
                kb.add_workgroup_size(&wg);
                kb.add_determinism_type(det_type);
                kb.add_determinism(det);
                kb.add_output(out);
                kb.add_accumulates(spec->accumulates);
                kb.add_min_iterations(spec->min_iterations);
                kb.add_flop_per_unit(spec->flop_per_unit);
                descs.push_back(kb.Finish());
            }
            auto dv = fbb.CreateVector(descs);
            // Session token generation lands with reconnect-resume in 2.x. It
            // MUST be from a CSPRNG when it arrives: a guessable token is a
            // reputation-theft primitive, since resume restores the worker's
            // accumulated standing.
            // 3.13 — a real 128-bit CSPRNG token now. Minted per handshake and
            // bound to this worker id, so a reconnect that presents it keeps
            // its standing rather than starting over.
            auto token = fbb.CreateString(
                reputation_ != nullptr ? reputation_->MintToken(worker_id_) : std::string{});
            const wire::Uuid wid = worker_id_.to_wire();

            wire::WelcomeBuilder wb(fbb);
            wb.add_worker_id(&wid);
            wb.add_session_token(token);
            wb.add_heartbeat_ms(kHeartbeatMs);
            wb.add_kernels(dv);
            return wb.Finish();
        });

    auto bench = protocol::EncodeMessage(
        wire::Body::BenchmarkRequest, [&](flatbuffers::FlatBufferBuilder& fbb) {
            auto kid = fbb.CreateString(kCalibrationKernel);
            wire::BenchmarkRequestBuilder b(fbb);
            b.add_kernel_id(kid);
            b.add_target_ms(kBenchmarkTargetMs);
            return b.Finish();
        });
    // Two SEPARATE frames, not one concatenated buffer — see Reaction::replies.
    return Reaction{{std::move(welcome), std::move(bench)}};
}

Reaction Session::OnBenchmarkResult(const wire::BenchmarkResult& result) {
    WorkerRecord* rec = fleet_.Mutable(worker_id_);
    if (rec == nullptr) {
        return {};
    }
    const double score = result.score();
    // Refuse a score that is not a usable positive number. A worker controls
    // this value, and it propagates into every future grant — a NaN or a
    // fabricated 1e300 would size a task larger than the keyspace, or poison
    // the correction factor permanently. Untrusted input, treated as such (R11
    // in spirit: the wire is not a source of truth about our own scheduling).
    if (!(score > 0.0) || !std::isfinite(score)) {
        spdlog::warn("benchmark_reject conn_id={} score={} reason=not_a_usable_number",
                     conn_id_, score);
        return NonFatal(wire::ErrorCode::MalformedMessage, "unusable benchmark score");
    }
    // Cap it. A worker cannot make itself arbitrarily important by claiming a
    // huge score — the cap is generous enough that no real device hits it and
    // tight enough that a liar gains little. Reputation for accuracy is Phase 3;
    // this is just refusing to be obviously gamed.
    rec->score_ops_per_sec = std::min(score, kMaxBenchmarkScore);
    spdlog::info("benchmark conn_id={} ops/s={:.3g} samples={}", conn_id_,
                 rec->score_ops_per_sec, result.samples());
    return {};
}

Reaction Session::OnThrottle(const wire::Throttle& throttle) {
    WorkerRecord* rec = fleet_.Mutable(worker_id_);
    if (rec == nullptr) {
        return {};
    }
    // R7: applied WITHOUT argument. The user's setting is authoritative and the
    // coordinator does not get to decide it knows better — that is the whole
    // point of a consent control.
    rec->throttle = std::clamp(static_cast<double>(throttle.level()), 0.0, 1.0);
    spdlog::info("throttle conn_id={} level={:.2f}", conn_id_, rec->throttle);
    return {};
}

Reaction Session::OnLeaseRequest(const wire::LeaseRequest& req, std::uint64_t now_ms) {
    // The worker asks WHETHER, never HOW MUCH — sizing is the coordinator's
    // (R1, D-0005). `max_tasks` is a hint we may grant fewer than, and is
    // clamped so a worker cannot ask for an unbounded backlog.
    const std::uint32_t want = std::min(std::max(req.max_tasks(), 1U), 4U);

    const WorkerRecord* rec = fleet_.Find(worker_id_);
    if (rec == nullptr) {
        return {};
    }
    // A worker with no benchmark score gets NO WORK — not a default-sized task.
    // Guessing a score would mean the first grant to every worker is a fiction
    // that then feeds the correction factor, and 2.13 would be correcting
    // toward a number nobody measured.
    if (!(rec->score_ops_per_sec > 0.0)) {
        spdlog::debug("lease conn_id={} deferred reason=no_benchmark_yet", conn_id_);
        return {};
    }

    // 3.12 — this CONNECTION has been sending malformed frames. No work, but
    // no disconnect and no reputation change: the peer is broken or probing,
    // and neither says anything about the results it computes (3.11).
    if (abusive_) {
        spdlog::debug("lease conn_id={} refused reason=frame_rate_limited", conn_id_);
        return {};
    }

    // 3.10 — blacklisted workers get no work. Never permanent: the cooldown
    // releases them onto probation, where 3.8 replicates them at maximum until
    // they earn a score back.
    if (reputation_ != nullptr && reputation_->IsBlacklisted(worker_id_, now_ms)) {
        spdlog::info("lease conn_id={} refused reason=blacklisted score={:.2f}",
                     conn_id_, reputation_->ScoreOf(worker_id_));
        return {};
    }

    std::vector<std::vector<std::byte>> out;
    std::uint32_t granted = 0;
    for (std::uint32_t i = 0; i < want; ++i) {
        // Sized for THIS worker, now (2.12). The kernel's R5 floor comes from
        // the manifest and is a hard minimum the sizer may not clamp under
        // (D-0029) — which is why the job has to be identified BEFORE sizing.
        //
        // A null `next_job` means no job has keyspace left to carve. That is NOT
        // a reason to stop: it is the precondition for speculation (2.17), which
        // by definition only runs once there is nothing fresh to hand out. An
        // early `break` here made `IssueSpeculative` below structurally
        // unreachable — a 12-worker heterogeneous run issued 1108 tasks and
        // exactly 0 replicas, which is what exposed it.
        std::optional<Task> task;
        const Job* next_job = jobs_.PeekNextJob();
        if (next_job != nullptr) {
            const KernelSpec* next_spec = kernels_.Find(next_job->kernel_id);

            // ops/sec -> units/sec for THIS kernel. `flop_per_unit` is the
            // manifest's count of arithmetic ops per work unit (D-0029(a) rules
            // that the R5 gate counts integer ops too, which is what makes one
            // device number comparable across kernels at all).
            const double ops_per_unit =
                (next_spec != nullptr && next_spec->flop_per_unit > 0)
                    ? static_cast<double>(next_spec->flop_per_unit)
                    : 1.0;

            SizingInputs sizing;
            sizing.score = rec->score_ops_per_sec / ops_per_unit;
            sizing.correction = rec->correction;
            sizing.throttle = rec->throttle;
            sizing.target_ms = kTargetTaskMs;
            sizing.lease_ms = lease_ms_;
            sizing.r5_min_units = next_spec != nullptr ? next_spec->r5_min_units : 0;
            sizing.remaining_units = next_job->remaining_units();
            // D-0050 — cold start until this worker has completed something.
            // `tasks_completed` is OUR count of accepted results, not anything
            // the worker claims, so a worker cannot skip its own probe.
            sizing.cold_start = (rec->tasks_completed == 0);

            // 2.16 — the worker's cached assets ride along so `Grant` can
            // prefer a task whose input it already holds. Empty until Phase 6
            // (D-0047).
            task = jobs_.Grant(worker_id_, now_ms, lease_ms_, ComputeTaskSize(sizing),
                               rec->cached_assets);
        }

        // 3.9 — sometimes hand out a range we already know the answer to.
        //
        // AFTER the normal grant, replacing it: the spot-check must be
        // indistinguishable from real work, so it has to look exactly like the
        // task it displaces. The displaced range is not lost — it was never
        // carved, because the cursor only advances on a real grant.
        if (task && spot_checks_ != nullptr && reputation_ != nullptr) {
            const double score = reputation_->ScoreOf(worker_id_);
            // Seeded from the task id so injection is replayable — an
            // experiment whose injections cannot be replayed produces
            // anecdotes (2.3's rule).
            const std::uint64_t h = task->id.lo() * 2654435761ULL;
            const double roll = static_cast<double>(h % 10000) / 10000.0;
            if (const auto pick = spot_checks_->Maybe(score, roll, h >> 16)) {
                if (const auto probe = jobs_.IssueKnownRange(
                        worker_id_, now_ms, lease_ms_, pick->start_unit,
                        pick->unit_count)) {
                    // Give the real task back before replacing it, or the range
                    // is silently dropped from the job.
                    (void)jobs_.Requeue(task->id, TaskEvent::Release);
                    spot_checks_->MarkIssued(probe->id, pick->expected_checksum);
                    task = probe;
                    spdlog::debug("spot_check issued task={} worker={} score={:.2f}",
                                  probe->id.lo(), worker_id_.hi(), score);
                }
            }
        }

        if (!task) {
            // Nothing left to carve. Near the end of a job, race a straggler
            // instead of idling (2.17) — the tail is what dominates completion
            // time on a heterogeneous fleet.
            task = speculation_ ? jobs_.IssueSpeculative(worker_id_, now_ms, lease_ms_)
                                : std::nullopt;
            if (task) {
                spdlog::info("speculative conn_id={} task={} replica_of={}", conn_id_,
                             task->id.lo(), task->replica_of.lo());
            }
        }
        if (!task) {
            break;
        }
        // The JOB says which kernel; do not reach for "the first one in the
        // registry", which happens to be right with one job and silently wrong
        // with two.
        const Job* job = jobs_.FindJob(task->job);
        const KernelSpec* spec =
            job != nullptr ? kernels_.Find(job->kernel_id) : nullptr;
        if (spec == nullptr) {
            spdlog::error("granted a task whose kernel is not in the registry");
            (void)jobs_.Requeue(task->id, TaskEvent::Release);
            break;
        }

        // R1: the COORDINATOR builds the params. What keyspace, what seed, what
        // difficulty — all of it is "what work to do", and the worker must never
        // invent any of it.
        auto params = BuildParams(*spec, *job, *task);
        if (!params) {
            spdlog::error("could not build params: {}", params.error().message);
            if (const auto r = jobs_.Requeue(task->id, TaskEvent::Release); !r) {
                spdlog::error("requeue_failed task={} detail=\"{}\"", task->id.lo(),
                              r.error().message);
            }
            break;
        }

        // One task per TaskGrant message; granting fewer than requested means
        // sending fewer messages, which keeps lease bookkeeping 1:1.
        auto frame = protocol::EncodeMessage(
            wire::Body::TaskGrant, [&](flatbuffers::FlatBufferBuilder& fbb) {
                auto kid = fbb.CreateString(spec->id);
                auto pv = fbb.CreateVector(
                    static_cast<const std::uint8_t*>(static_cast<const void*>(params->data())),
                    params->size());
                const wire::Uuid tid = task->id.to_wire();
                const wire::Uuid jid = task->job.to_wire();
                wire::OutputSpecBuilder ob(fbb);
                // From the MANIFEST, not a literal. The worker allocates against
                // this number, so a hardcoded 32 would break the first kernel
                // whose output is not 32 bytes — and break it as a garbage
                // result rather than an error.
                ob.add_bytes(spec->output_bytes);
                ob.add_dtype(wire::DType::U32);
                auto os = ob.Finish();

                // The bulk input this task reads, if the job has one. Struct
                // fields must be created BEFORE the table builder opens.
                std::optional<wire::Hash32> input;
                if (job->input_ref) {
                    // AssetId is 32 raw bytes; Hash32 is four u64 lanes. Built
                    // by reading the bytes in a FIXED little-endian order
                    // rather than memcpy-ing the array over the struct — the
                    // struct's layout is the wire's business, and the two
                    // agreeing today on arm64 says nothing about a big-endian
                    // reader (the D-0027 lesson: "it works on our machines" is
                    // not evidence about layout).
                    const auto& id = *job->input_ref;
                    std::array<std::uint64_t, 4> lanes{};
                    for (std::size_t lane = 0; lane < 4; ++lane) {
                        std::uint64_t v = 0;
                        for (std::size_t byte = 0; byte < 8; ++byte) {
                            v |= static_cast<std::uint64_t>(
                                     std::to_integer<std::uint8_t>(id[lane * 8 + byte]))
                                 << (byte * 8);
                        }
                        lanes[lane] = v;
                    }
                    input = wire::Hash32(lanes[0], lanes[1], lanes[2], lanes[3]);
                }

                wire::TaskEnvelopeBuilder tb(fbb);
                tb.add_task_id(&tid);
                tb.add_job_id(&jid);
                if (input) {
                    // TELLING THE WORKER IT NEEDS AN ASSET IS WHAT LETS IT
                    // REFUSE CLEANLY. Without this field a worker runs a kernel
                    // whose WGSL declares storage bindings nothing supplies —
                    // observed at 5.16 bring-up as a Rust panic inside
                    // wgpu-native that aborted the whole process. A worker that
                    // cannot obtain the asset must release with
                    // AssetUnavailable, which is exactly what that
                    // ReleaseReason is for.
                    tb.add_input_ref(&*input);
                }
                tb.add_kernel_id(kid);
                tb.add_seed(job->seed);
                tb.add_params(pv);
                // The authority for this task's range (D-0041). BuildParams
                // also wrote it into the params chunk window, and that copy is
                // still meaningful: the CPU reference recomputes from
                // BuildParams' output, so the two are derived independently
                // from one Task and a divergence surfaces as a reference
                // mismatch rather than as silence.
                tb.add_start_unit(task->start_unit);
                tb.add_work_units(task->unit_count);
                tb.add_output_spec(os);
                tb.add_lease_ms(lease_ms_);
                auto env = tb.Finish();

                wire::TaskGrantBuilder gb(fbb);
                gb.add_envelope(env);
                return gb.Finish();
            });
        out.push_back(std::move(frame));
        ++granted;

        // Remember what we predicted, on OUR clock. 2.13 compares this against
        // the observed duration — never against the worker's self-reported
        // gpu_ms, which is telemetry a worker chooses (invariant 8).
        if (WorkerRecord* mrec = fleet_.Mutable(worker_id_); mrec != nullptr) {
            // From the GRANTED task's kernel, not the one sizing looked at: a
            // speculative replica can belong to a different job than
            // `PeekNextJob` named, and predicting against the wrong
            // `flop_per_unit` would poison the correction EWMA (2.13).
            const double granted_ops_per_unit =
                spec->flop_per_unit > 0 ? static_cast<double>(spec->flop_per_unit) : 1.0;
            mrec->predicted_ms = 1000.0 * static_cast<double>(task->unit_count) /
                                 (rec->score_ops_per_sec / granted_ops_per_unit);
            mrec->granted_at_ms = now_ms;
            if (events_ != nullptr) {
                events_->Grant(now_ms, task->id, worker_id_, task->unit_count,
                               mrec->predicted_ms, task->replica_of != TaskId{});
            }
        }
    }

    spdlog::debug("lease conn_id={} asked={} granted={} ops/s={:.3g} corr={:.2f}",
                  conn_id_, req.max_tasks(), granted, rec->score_ops_per_sec,
                  rec->correction);
    return Reaction{std::move(out)};
}

std::vector<std::vector<std::byte>> Session::DrainRevokes() {
    std::vector<std::vector<std::byte>> out;
    WorkerRecord* rec = fleet_.Mutable(worker_id_);
    if (rec == nullptr || rec->pending_revokes.empty()) {
        return out;
    }
    for (const TaskId task : rec->pending_revokes) {
        out.push_back(protocol::EncodeMessage(
            wire::Body::Revoke, [&](flatbuffers::FlatBufferBuilder& fbb) {
                const wire::Uuid tid = task.to_wire();
                wire::RevokeBuilder b(fbb);
                b.add_task_id(&tid);
                b.add_reason(wire::RevokeReason::SpeculativeLoser);
                return b.Finish();
            }));
    }
    rec->pending_revokes.clear();
    return out;
}

Reaction Session::OnProgress(const wire::Progress& progress, std::uint64_t now_ms) {
    if (progress.task_id() == nullptr) {
        return NonFatal(wire::ErrorCode::MalformedMessage, "Progress has no task_id");
    }
    // `fraction_done` is TELEMETRY and is deliberately not read here. It is a
    // number the worker chooses, so nothing may depend on it (invariant 8).
    if (!progress.request_renew()) {
        return {};   // pure heartbeat; the Touch above already did the work
    }

    const TaskId task_id{*progress.task_id()};
    if (const auto s = jobs_.RenewLease(worker_id_, task_id, now_ms, lease_ms_); !s) {
        // A renewal for a task this worker does not hold. Common and benign
        // once expiry is real: the sweep took it back while the renewal was in
        // flight. NonFatal so the worker learns and moves on; it must not be
        // able to reclaim a task already granted to somebody else.
        spdlog::debug("renew_rejected conn_id={} task={} reason=lease_not_held",
                      conn_id_, task_id.lo());
        return NonFatal(wire::ErrorCode::LeaseNotHeld, "no lease on that task");
    }

    spdlog::debug("renew conn_id={} task={} until={}", conn_id_, task_id.lo(),
                  now_ms + lease_ms_);
    return {};
}

// ── Serving a bulk asset over the control link (5.16, D-0077) ────────────

Reaction Session::OnAssetRequest(const wire::AssetRequest& req) {
    const auto reply_miss = [&](const wire::Hash32* h) {
        std::vector<std::vector<std::byte>> out;
        out.push_back(protocol::EncodeMessage(
            wire::Body::AssetMiss, [&](flatbuffers::FlatBufferBuilder& fbb) {
                wire::AssetMissBuilder b(fbb);
                if (h != nullptr) {
                    b.add_hash(h);
                }
                return b.Finish();
            }));
        return Reaction{std::move(out)};
    };

    const wire::Hash32* h = req.hash();
    if (h == nullptr) {
        return NonFatal(wire::ErrorCode::MalformedMessage, "AssetRequest has no hash");
    }
    // Hash32's four u64 lanes back to the 64-char lowercase hex the store keys
    // on. Little-endian per lane, matching how the grant encoded it — the two
    // conversions are inverses and live a few hundred lines apart, which is
    // exactly the kind of pairing that drifts, so both spell out the byte order
    // rather than memcpy-ing a struct.
    std::string address;
    address.reserve(64);
    static constexpr char kHex[] = "0123456789abcdef";
    for (const std::uint64_t lane : {h->a(), h->b(), h->c(), h->d()}) {
        for (std::size_t byte = 0; byte < 8; ++byte) {
            const auto v = static_cast<std::uint8_t>((lane >> (byte * 8)) & 0xFFU);
            address.push_back(kHex[v >> 4]);
            address.push_back(kHex[v & 0x0FU]);
        }
    }

    const std::vector<std::byte>* blob =
        asset_store_ != nullptr ? asset_store_->Find(address) : nullptr;
    if (blob == nullptr) {
        spdlog::debug("asset_miss conn_id={} address={}", conn_id_, address);
        return reply_miss(h);
    }

    const std::size_t total_chunks =
        (blob->size() + protocol::kChunkBytes - 1) / protocol::kChunkBytes;
    if (total_chunks == 0 || total_chunks > 0xFFFFFFFFULL) {
        return reply_miss(h);
    }

    // The worker asks for a RANGE. Clamped to what exists rather than trusted:
    // `chunk_to` is attacker-controlled, and a huge value would otherwise turn
    // one request into an unbounded send loop — an amplification attack costing
    // the attacker one frame (R11).
    const std::uint32_t from = req.chunk_from();
    const std::uint32_t to =
        std::min<std::uint64_t>(req.chunk_to(), total_chunks);
    if (from >= to) {
        return reply_miss(h);
    }
    // Bounded per request so one asset cannot monopolise the connection; the
    // worker asks again for the rest.
    constexpr std::uint32_t kMaxChunksPerRequest = 64;
    const std::uint32_t end = std::min(to, from + kMaxChunksPerRequest);

    std::vector<std::vector<std::byte>> out;
    out.reserve(end - from);
    for (std::uint32_t i = from; i < end; ++i) {
        const auto offset = protocol::ChunkOffset(i);
        if (!offset || *offset >= blob->size()) {
            break;
        }
        const std::size_t len =
            std::min<std::size_t>(protocol::kChunkBytes, blob->size() - *offset);
        const std::byte* start = blob->data() + *offset;
        out.push_back(protocol::EncodeMessage(
            wire::Body::AssetChunk, [&](flatbuffers::FlatBufferBuilder& fbb) {
                auto bytes = fbb.CreateVector(
                    static_cast<const std::uint8_t*>(static_cast<const void*>(start)),
                    len);
                wire::AssetChunkBuilder b(fbb);
                b.add_hash(h);
                b.add_index(i);
                b.add_total(static_cast<std::uint32_t>(total_chunks));
                b.add_bytes(bytes);
                return b.Finish();
            }));
    }
    spdlog::debug("asset_serve conn_id={} address={} chunks={}..{} of {}",
                  conn_id_, address, from, end, total_chunks);
    return Reaction{std::move(out)};
}

Reaction Session::OnGoodbye() {
    // Clean drain (2.9). Identical handling to a disconnect, and deliberately
    // so — the difference is only that we were told. Releasing immediately is
    // what stops a polite worker's tasks waiting out a full lease.
    const std::size_t released = jobs_.ReleaseAllHeldBy(worker_id_);
    fleet_.Leave(worker_id_);
    spdlog::info("goodbye conn_id={} released={} penalty=none", conn_id_, released);
    return {};
}

Reaction Session::OnRelease(const wire::Release& release) {
    if (release.task_id() == nullptr) {
        return NonFatal(wire::ErrorCode::MalformedMessage, "Release has no task_id");
    }
    const TaskId task_id{*release.task_id()};

    // INVARIANT 5 — a worker may only release what it holds. Without this, any
    // connected peer could hand back another worker's task and stall the job by
    // forcing it to be re-granted repeatedly.
    if (const auto s = jobs_.CheckLease(worker_id_, task_id); !s) {
        spdlog::warn("reject conn_id={} task={} reason=release_without_lease", conn_id_,
                     task_id.lo());
        return NonFatal(wire::ErrorCode::LeaseNotHeld, "no lease on that task");
    }

    // NO REPUTATION PENALTY (R8). A worker giving work back — because the user
    // stopped it, its device died, or it cannot run that kernel — is the normal
    // case, and the whole point of leasing is that it costs nothing.
    if (const auto s = jobs_.Requeue(task_id, TaskEvent::Release); !s) {
        spdlog::error("requeue_failed conn_id={} task={} detail=\"{}\"", conn_id_,
                      task_id.lo(), s.error().message);
        return NonFatal(wire::ErrorCode::Internal, "could not requeue");
    }

    spdlog::info("release conn_id={} task={} reason={} penalty=none", conn_id_,
                 task_id.lo(), static_cast<int>(release.reason()));
    return {};
}

Reaction Session::OnResultHeader(const wire::ResultHeader& header,
                                 std::span<const std::byte> payload) {
    if (header.task_id() == nullptr) {
        return NonFatal(wire::ErrorCode::MalformedMessage, "ResultHeader has no task_id");
    }
    const TaskId task_id{*header.task_id()};

    // STEP 2.10 — idempotent submission. A result for a task already accepted
    // is discarded SILENTLY, not errored. Once speculation exists (2.17) a
    // duplicate is the normal outcome of a race, and the worker that lost has
    // done nothing wrong; returning an error would teach it to retry, or worse,
    // look like a fault in whatever counts errors.
    if (std::ranges::find(completed_, task_id) != completed_.end()) {
        spdlog::debug("duplicate_result conn_id={} task={} action=discard",
                      conn_id_, task_id.lo());
        return {};
    }

    // INVARIANT 4 — one in-flight ResultHeader per task per worker.
    const bool already =
        std::ranges::find(in_flight_results_, task_id) != in_flight_results_.end();
    if (const auto s = protocol::CheckSingleInFlightResult(already); !s) {
        return NonFatal(wire::ErrorCode::OrphanPayload, "result already in flight");
    }

    // INVARIANT 9, part one: the declared length must match what arrived. A
    // declared length larger than the buffer is the Heartbleed shape — though
    // SplitFrame has already bounded the payload, so this catches a LIE about
    // the size rather than an overread.
    if (const auto s = protocol::CheckPayloadLength(header.payload_bytes(), payload.size());
        !s) {
        return NonFatal(wire::ErrorCode::MalformedMessage,
                        "payload_bytes disagrees with the payload");
    }

    // INVARIANT 5 — authorization, and it must come before the checksum: hashing
    // is the only expensive thing on this path, and an unauthenticated worker
    // must not be able to make us do it over an 8 MiB payload.
    //
    // CheckLease, not Submit — nothing is mutated yet. See below.
    if (const auto s = jobs_.CheckLease(worker_id_, task_id); !s) {
        spdlog::warn("reject conn_id={} task={} reason=lease_not_held", conn_id_,
                     task_id.lo());
        return NonFatal(wire::ErrorCode::LeaseNotHeld, "no lease on that task");
    }

    // INVARIANT 9, part two: BLAKE3-64 over the payload.
    //
    // A mismatch is requeued with NO REPUTATION PENALTY. Corruption is not
    // malice; penalising it would blacklist honest workers on flaky networks,
    // which is a far worse failure than tolerating a retry.
    //
    // ── WHY THE CHECKSUM IS CHECKED BEFORE Submit ────────────────────────
    // It was the other way round once, and that was a WEDGE: Submit moves the
    // task to Validating, from which `Release` is an ILLEGAL edge, so the
    // requeue silently failed and the task sat in Validating forever. Nothing
    // holds a lease in Validating, so the expiry sweep could not rescue it
    // either — the job simply never completed. The `(void)` on the requeue is
    // what hid it. Caught by the 1.16 test, not by review.
    //
    // Ordering it this way means a corrupt payload causes ZERO state change
    // until we know the bytes are readable, and the requeue below runs from
    // Leased, which is a legal edge.
    const std::uint64_t computed = Blake3_64(payload);
    if (const auto s = protocol::CheckPayloadChecksum(header.checksum(), computed); !s) {
        spdlog::warn("checksum_mismatch conn_id={} task={} declared={:016x} computed={:016x}"
                     " action=requeue penalty=none",
                     conn_id_, task_id.lo(), header.checksum(), computed);
        // Straight back to Queued. Not Rejected — that is for a wrong ANSWER,
        // and this result was never readable enough to be judged wrong. The
        // worker is NOT recorded in prior_workers either: it never delivered a
        // result, so invariant 6 has no reason to exclude it from a retry.
        if (const auto r = jobs_.Requeue(task_id, TaskEvent::Release); !r) {
            // Never discard this silently — see the wedge described above.
            spdlog::error("requeue_failed conn_id={} task={} detail=\"{}\"", conn_id_,
                          task_id.lo(), r.error().message);
        }
        return NonFatal(wire::ErrorCode::ChecksumMismatch, "payload checksum mismatch");
    }

    // ── PARTIAL RESULT? (5.13, D-0074) ──────────────────────────────────
    //
    // AFTER the checksum, BEFORE `Submit`. Same ordering discipline D-0031
    // established: nothing about the task moves until the bytes are known
    // readable, and a partial must not reach `Submit` at all — that would push
    // the task to `Validating`, from which `Release` is not a legal edge, and
    // it would strand with no lease and no worker.
    //
    // `units_done == 0` MEANS COMPLETE (D-0074). Every message written before
    // this field existed omits it, and every one of them is a complete result;
    // a default of literal zero would make every brute_search result claim it
    // did no work.
    {
        const Task* task = jobs_.Find(task_id);
        const std::uint64_t units_done = header.units_done();
        const bool complete =
            units_done == 0 || (task != nullptr && units_done >= task->unit_count);
        if (!complete) {
            if (const auto s = jobs_.RecordPartial(worker_id_, task_id, header.sequence(),
                                                  units_done, payload, now_ms_, lease_ms_);
                !s) {
                spdlog::debug("partial_rejected conn_id={} task={} seq={} reason=\"{}\"",
                              conn_id_, task_id.lo(), header.sequence(),
                              s.error().message);
                // NOT an error to the worker. A stale sequence after a
                // reconnect is normal, and telling the worker to retry would
                // make it resend a snapshot we deliberately discarded.
                return {};
            }
            spdlog::debug("partial conn_id={} task={} seq={} units_done={}/{}",
                          conn_id_, task_id.lo(), header.sequence(), units_done,
                          task != nullptr ? task->unit_count : 0);
            // The lease was renewed by RecordPartial; the task stays Leased and
            // the worker keeps going.
            return {};
        }
    }

    // Only now does the task move. Submit re-checks invariant 5 rather than
    // trusting the check above — the cost is a comparison, and the alternative
    // is an authorization step that a future caller can forget.
    if (const auto s = jobs_.Submit(worker_id_, task_id); !s) {
        return NonFatal(wire::ErrorCode::LeaseNotHeld, "no lease on that task");
    }

    // OPTIONAL, DEV-ONLY (step 1.26): recompute the task on the CPU and compare.
    // A test harness, never a validation strategy — if the coordinator could
    // afford to compute every answer there would be no reason to distribute the
    // work. Affordable here only because 1.26 uses deliberately tiny tasks.
    // See reference_check.hpp.
    //
    // ── WHAT THIS COUNTS, AND WHY IT IS NOT A VALIDATOR SCORE ────────────
    // It checks EVERY SUBMISSION, including a replica's and a liar's. So under
    // replication its `mismatched` count is "how many wrong answers were
    // SUBMITTED", not "how many were ACCEPTED" — and only the second is a
    // statement about whether validation works.
    //
    // With `--replication fixed2x` on `byzantine_10pct`, it reported 7
    // mismatches while all 7 disagreements were escalated and outvoted: the
    // validator did its job and the harness still logged failures. E4 (3.14)
    // needs the accepted-answer count instead, and that is a change to this
    // hook, not to the validator.
    if (reference_stats_ != nullptr) {
        const Task* task = jobs_.Find(task_id);
        const Job* job = task != nullptr ? jobs_.FindJob(task->job) : nullptr;
        const KernelSpec* spec = job != nullptr ? kernels_.Find(job->kernel_id) : nullptr;
        if (spec != nullptr) {
            (void)CheckAgainstReference(*spec, *job, *task, payload, *reference_stats_);
            // Deliberately NOT rejecting on mismatch. This harness reports; the
            // run's verdict is the summary at shutdown. Making it reject would
            // quietly turn a diagnostic into policy — the thing the header
            // spends its length warning against.
        }
    }

    // ── SPOT-CHECK (3.9) ─────────────────────────────────────────────────
    //
    // We already know the answer, so ONE result convicts — no second worker,
    // no replication. That is what makes this the only defence available in
    // the regime adaptive replication creates: at `trusted_at` a worker's
    // result is accepted from a single submission, so a worker that behaves
    // until trusted and then defects is otherwise invisible (D-0059).
    if (spot_checks_ != nullptr) {
        if (const auto expected = spot_checks_->ExpectedFor(task_id)) {
            const bool correct = header.checksum() == *expected;
            spot_checks_->Forget(task_id);
            if (!correct && reputation_ != nullptr) {
                // Doubled penalty (3.9): there is no honest disagreement with
                // an answer we already hold, so this is far stronger evidence
                // than losing a vote.
                reputation_->RecordRejected(worker_id_, 4.0, /*spot_check=*/true);
                spdlog::warn("SPOT-CHECK FAILED task={} worker={} score={:.2f}",
                             task_id.lo(), worker_id_.hi(),
                             reputation_->ScoreOf(worker_id_));
                if (reputation_->MaybeBlacklist(worker_id_, now_ms_)) {
                    spdlog::warn("blacklisted worker={} via spot-check", worker_id_.hi());
                }
            } else if (correct && reputation_ != nullptr) {
                reputation_->RecordAccepted(worker_id_);
            }
        }
    }

    // ── VALIDATION (3.4/3.5) ─────────────────────────────────────────────
    //
    // The checksum proved the bytes arrived intact. It says NOTHING about
    // whether they are the right bytes — a liar computes a wrong answer and
    // checksums it correctly — so this is where the answer is judged.
    {
        const Task* t = jobs_.Find(task_id);
        const Job* j = t != nullptr ? jobs_.FindJob(t->job) : nullptr;
        const KernelSpec* ks = j != nullptr ? kernels_.Find(j->kernel_id) : nullptr;
        if (ks == nullptr) {
            return NonFatal(wire::ErrorCode::Internal, "kernel missing for task");
        }

        // Payload retained ONLY where a hash cannot answer "close enough"
        // (D-0054). For `Exact`, checksum equality and bitwise equality are the
        // same question, and keeping up to 8 MiB per submission would hand an
        // attacker a memory lever (R11).
        std::vector<std::byte> retained;
        if (ks->determinism != Determinism::Exact) {
            retained.assign(payload.begin(), payload.end());
        }
        jobs_.RecordSubmission(task_id, worker_id_, header.checksum(),
                               std::move(retained));

        const Task* after = jobs_.Find(task_id);
        QuorumConfig qcfg = quorum_;
        // 3.8 — how much agreement THIS worker's result needs, from its record.
        if (reputation_ != nullptr) {
            qcfg.required_agreement =
                RequiredAgreementFor(quorum_, *reputation_, worker_id_, now_ms_);
        }
        // EVERY submission, not just the accepting one (4.17).
        //
        // Under replication a task needs k agreeing answers, and only the LAST
        // produced a log line — so a run where the browser did half the work
        // and the native worker finished it looked, in the log, identical to a
        // run where the browser did nothing at all. That ambiguity cost three
        // attempts at the three-way test before anyone noticed the evidence was
        // simply absent rather than negative.
        spdlog::info("submission conn_id={} worker={} task={} have={} need={}",
                     conn_id_, worker_id_.hi(), task_id.lo(),
                     after->submissions.size(), qcfg.required_agreement);

        const QuorumResult q = Decide(*ks, qcfg, after->submissions);

        if (q.action == QuorumAction::NeedMoreReplicas) {
            // Not a verdict on anyone — we simply do not know yet. The task
            // goes back out to a worker that has not seen it (invariant 6).
            if (const auto s = jobs_.RequestReplica(task_id); !s) {
                return NonFatal(wire::ErrorCode::Internal, "could not issue replica");
            }
            // 3.3 — at `warn` WITH the numbers, because a misdeclared
            // determinism class presents as a flood of these from honest
            // workers (RISKS.md §2), and the deviation is what tells that apart
            // from a liar at a glance.
            if (!q.detail.empty()) {
                spdlog::warn("replica_needed task={} {}", task_id.lo(), q.detail);
            }
            in_flight_results_.erase(
                std::remove(in_flight_results_.begin(), in_flight_results_.end(), task_id),
                in_flight_results_.end());
            return {};
        }

        if (q.action == QuorumAction::Inconclusive) {
            // No majority at the cap. NOT a rejection of anybody: with no
            // majority there is no evidence, and penalising half the group for
            // a split we cannot resolve is how honest workers get blacklisted.
            spdlog::warn("validation_inconclusive task={} {}", task_id.lo(), q.detail);
            if (const auto s = jobs_.RestartValidation(task_id); !s) {
                return NonFatal(wire::ErrorCode::Internal, "could not restart validation");
            }
            in_flight_results_.erase(
                std::remove(in_flight_results_.begin(), in_flight_results_.end(), task_id),
                in_flight_results_.end());
            return {};
        }

        // Accepted. Credit the agreeing workers and charge the dissenters —
        // 3.11: this is the ONLY path in the coordinator that touches
        // reputation, and it is reached only when a result was computed,
        // checksummed intact, and judged wrong by its peers.
        if (reputation_ != nullptr) {
            for (const WorkerId w : q.agreeing) {
                reputation_->RecordAccepted(w, q.agreeing_max_ulp);
            }
            for (const WorkerId w : q.dissenting) {
                // Severity from the DEVIATION, not a flat penalty (3.2/D-0055).
                // 0.16 measured 5 ULP between two honest vendors; charging that
                // like a fabricated answer re-introduces the cross-vendor
                // rejection R6 exists to prevent.
                reputation_->RecordRejected(
                    w, SeverityFromDeviation(q.dissent_max_ulp, q.dissent_max_rel));
                if (reputation_->MaybeBlacklist(w, now_ms_)) {
                    spdlog::warn("blacklisted worker={} score={:.2f} accepted={} rejected={}",
                                 w.hi(), reputation_->ScoreOf(w),
                                 reputation_->Find(w)->accepted,
                                 reputation_->Find(w)->rejected);
                }
            }
            if (!q.detail.empty()) {
                spdlog::warn("validation task={} {}", task_id.lo(), q.detail);
            }
        }

        // E4's actual measurement (3.14): was the answer we ACCEPTED correct?
        // `reference_stats_->mismatched` above counts every submission, so it
        // cannot answer this — a caught liar and an undetected one look the
        // same there.
        if (reference_stats_ != nullptr) {
            ++reference_stats_->accepted_checked;
            ReferenceStats probe;
            const Task* t2 = jobs_.Find(task_id);
            const Job* j2 = t2 != nullptr ? jobs_.FindJob(t2->job) : nullptr;
            if (j2 != nullptr && !CheckAgainstReference(*ks, *j2, *t2, payload, probe)) {
                ++reference_stats_->accepted_wrong;
                spdlog::error("ACCEPTED A WRONG ANSWER task={} worker={} — validation "
                              "did not catch this",
                              task_id.lo(), worker_id_.hi());
            }
        }

        // 3.9 — remember this range's verified answer so it can be re-issued
        // as a spot-check later.
        //
        // ONLY when a quorum actually agreed. Seeding the pool from a single
        // unvalidated result would enshrine one worker's answer as ground truth
        // and then convict everyone who disagreed with it — one liar becomes a
        // fleet of blacklisted honest workers.
        if (spot_checks_ != nullptr && q.agreeing.size() >= 2) {
            if (const Task* done = jobs_.Find(task_id); done != nullptr) {
                spot_checks_->Remember(done->start_unit, done->unit_count,
                                       header.checksum());
            }
        }

        // ── INTO THE IMAGE (5.15/5.16) ──────────────────────────────────
        //
        // BEFORE Finish, because Finish is what frees the task record this
        // needs to locate the tile. Only a VALIDATED result reaches here: a
        // tile composited from an unvalidated payload would put a liar's pixels
        // on the demo, and the dashboard is the most persuasive surface in the
        // project.
        if (compositor_ != nullptr) {
            if (const Task* done = jobs_.Find(task_id); done != nullptr) {
                if (const Job* j = jobs_.FindJob(done->job);
                    j != nullptr && j->render && j->render->samples_per_tile > 0) {
                    const auto tile =
                        done->start_unit / j->render->samples_per_tile;
                    if (!compositor_->AcceptTile(static_cast<std::uint32_t>(tile),
                                                 payload)) {
                        // Never silent. A rejected tile means the payload size
                        // disagrees with the grid, which is a real mismatch
                        // between what the coordinator asked for and what came
                        // back — and it would present as one stubbornly black
                        // square in an otherwise converging image.
                        spdlog::warn("composite_rejected task={} tile={} bytes={}",
                                     task_id.lo(), tile, payload.size());
                    }
                }
            }
        }

        if (const auto s = jobs_.Finish(task_id, /*accepted=*/true); !s) {
            return NonFatal(wire::ErrorCode::Internal, "task could not be finalised");
        }
    }

    // FIRST RESULT WINS (2.17). Every live sibling is cancelled — not
    // rejected, and not requeued: the range is done, and the losing worker did
    // nothing wrong.
    for (const auto& rev : jobs_.CancelSiblingsOf(task_id)) {
        if (WorkerRecord* loser = fleet_.Mutable(rev.holder); loser != nullptr) {
            loser->pending_revokes.push_back(rev.task);
        }
        if (events_ != nullptr) {
            events_->Cancel(now_ms_, rev.task, rev.holder, rev.wasted_units);
        }
        spdlog::info("speculation_won task={} cancelled={} wasted_units={}",
                     task_id.lo(), rev.task.lo(), rev.wasted_units);
    }

    completed_.push_back(task_id);
    fleet_.RecordCompletion(worker_id_);

    // 2.13 — correct the prediction from what we OBSERVED. The worker also
    // reports a duration in TaskStats and it is deliberately ignored here:
    // a worker that under-reports would be granted ever-larger tasks, which is
    // a way to be handed the whole keyspace by lying about being fast.
    if (WorkerRecord* rec = fleet_.Mutable(worker_id_);
        rec != nullptr && rec->predicted_ms > 0.0 && rec->granted_at_ms > 0) {
        const double actual_ms = static_cast<double>(now_ms_ - rec->granted_at_ms);
        // Captured BEFORE the reset below. 2.26 plots predicted against actual,
        // and reading `rec->predicted_ms` after it is zeroed would log 0 for
        // every task — a convergence plot made entirely of a bookkeeping
        // artifact, which would look like a finding rather than a bug.
        const double predicted_ms = rec->predicted_ms;
        rec->correction = UpdateCorrection(rec->correction, predicted_ms, actual_ms);
        spdlog::debug("sizing conn_id={} predicted={:.0f}ms actual={:.0f}ms corr={:.2f}",
                      conn_id_, predicted_ms, actual_ms, rec->correction);
        rec->predicted_ms = 0.0;

        // 2.21 — observed throughput, accumulated from the SAME measurement the
        // correction uses. Two numbers derived from one observation cannot
        // disagree; two independently maintained ones eventually do.
        if (const Task* done = jobs_.Find(task_id); done != nullptr) {
            rec->units_completed += done->unit_count;
            rec->observed_ms_total += actual_ms;
            if (events_ != nullptr) {
                events_->Accept(now_ms_, task_id, worker_id_, done->unit_count,
                                actual_ms, predicted_ms, rec->correction);
            }
        }
    }
    spdlog::info("result conn_id={} task={} bytes={} accepted", conn_id_,
                 task_id.lo(), payload.size());
    return {};
}

void Session::OnDisconnect() {
    if (!handshaked_) {
        return;
    }
    fleet_.Leave(worker_id_);
    // Release every held lease IMMEDIATELY rather than waiting for expiry. A
    // worker vanishing is the normal case (R8); making the queue wait out a
    // 30-second lease for a socket we already know is gone would stall the job
    // for no reason. No reputation penalty — absence is not malice.
    for (const TaskId task : jobs_.HeldBy(worker_id_)) {
        if (const auto r = jobs_.Requeue(task, TaskEvent::Release); !r) {
            spdlog::error("requeue_failed conn_id={} task={} detail=\"{}\"", conn_id_,
                          task.lo(), r.error().message);
        }
    }
    spdlog::info("disconnect conn_id={} released_leases", conn_id_);
}

}  // namespace p2pgpu::coordinator
