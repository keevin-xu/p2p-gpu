// Handshake (1.15) and result ingestion (1.16). Transport-free; see session.hpp.

#include "p2pgpu/coordinator/session.hpp"

#include <blake3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>

#include "p2pgpu/coordinator/params.hpp"
#include "p2pgpu/protocol/encode.hpp"
#include "p2pgpu/protocol/invariants.hpp"

namespace p2pgpu::coordinator {
namespace {

constexpr std::uint32_t kHeartbeatMs = 15000;

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
        protocol::EncodeMessage(wire::Body::Error,
                                [&](flatbuffers::FlatBufferBuilder& fbb) {
                                    auto msg = fbb.CreateString(message);
                                    wire::ErrorBuilder b(fbb);
                                    b.add_code(code);
                                    b.add_message(msg);
                                    b.add_fatal(true);
                                    return b.Finish();
                                }),
        /*close=*/true};
}

Reaction Session::NonFatal(wire::ErrorCode code, const char* message) {
    return Reaction{
        protocol::EncodeMessage(wire::Body::Error,
                                [&](flatbuffers::FlatBufferBuilder& fbb) {
                                    auto msg = fbb.CreateString(message);
                                    wire::ErrorBuilder b(fbb);
                                    b.add_code(code);
                                    b.add_message(msg);
                                    b.add_fatal(false);
                                    return b.Finish();
                                }),
        /*close=*/false};
}

Reaction Session::OnMessage(const protocol::VerifiedFrame& frame, std::uint64_t now_ms) {
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

        // Not yet implemented, but explicitly listed so that adding a Body
        // variant breaks THIS switch too (-Werror=switch). A silent default
        // would let a new message type be ignored without anyone noticing.
        case wire::Body::Throttle:
        case wire::Body::BenchmarkResult:
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
    fleet_.Join(worker_id_, conn_id_, /*now_ms=*/0);   // stamped on the next frame

    spdlog::info("handshake conn_id={} worker_id={} kernels={}", conn_id_,
                 worker_id_.hi(), kernels_.size());

    return Reaction{protocol::EncodeMessage(
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
                    reinterpret_cast<const std::uint8_t*>(init.data()), init.size());

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
            auto token = fbb.CreateString("");
            const wire::Uuid wid = worker_id_.to_wire();

            wire::WelcomeBuilder wb(fbb);
            wb.add_worker_id(&wid);
            wb.add_session_token(token);
            wb.add_heartbeat_ms(kHeartbeatMs);
            wb.add_kernels(dv);
            return wb.Finish();
        })};
}

Reaction Session::OnLeaseRequest(const wire::LeaseRequest& req, std::uint64_t now_ms) {
    // The worker asks WHETHER, never HOW MUCH — sizing is the coordinator's
    // (R1, D-0005). `max_tasks` is a hint we may grant fewer than, and is
    // clamped so a worker cannot ask for an unbounded backlog.
    const std::uint32_t want = std::min(std::max(req.max_tasks(), 1U), 4U);

    std::vector<std::byte> out;
    std::uint32_t granted = 0;
    for (std::uint32_t i = 0; i < want; ++i) {
        const auto task = jobs_.Grant(worker_id_, now_ms, lease_ms_);
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
                    reinterpret_cast<const std::uint8_t*>(params->data()),
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

                wire::TaskEnvelopeBuilder tb(fbb);
                tb.add_task_id(&tid);
                tb.add_job_id(&jid);
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
        out.insert(out.end(), frame.begin(), frame.end());
        ++granted;
    }

    spdlog::debug("lease conn_id={} asked={} granted={} queued={}", conn_id_,
                  req.max_tasks(), granted, jobs_.queued());
    return Reaction{std::move(out)};
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

    // Real validation (replication, quorum, reputation) is Phase 3. With one
    // worker and no replicas there is nothing to compare against, so a
    // checksum-clean result is accepted — and saying that plainly here is
    // better than implying a check that does not exist yet.
    if (const auto s = jobs_.Finish(task_id, /*accepted=*/true); !s) {
        return NonFatal(wire::ErrorCode::Internal, "task could not be finalised");
    }

    completed_.push_back(task_id);
    fleet_.RecordCompletion(worker_id_);
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
