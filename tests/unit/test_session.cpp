// Steps 1.15-1.16 — handshake and result ingestion.
//
// Session is transport-free by design, so these tests drive it with real
// encoded frames and no socket at all. Every frame goes through VerifyFrame
// first, exactly as net.cpp does — testing Session against hand-built Envelope
// objects would skip the one path R11 cares about.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <vector>

#include "p2pgpu/coordinator/job.hpp"
#include "p2pgpu/coordinator/session.hpp"
#include "p2pgpu/protocol/encode.hpp"
#include "p2pgpu/protocol/limits.hpp"
#include "p2pgpu/protocol/verify.hpp"

using namespace p2pgpu;
using namespace p2pgpu::coordinator;

namespace {

/// A kernel that is actually in the shipped manifest.
constexpr const char* kKernel = "brute_search_v1";

/// `brute_search_v1` declares `r5_min_units = 4.0e5` in the manifest, and the
/// sizer floors at it (D-0029) — so with a tiny benchmark score every task
/// comes out at exactly this size. Tests that want N tasks size the keyspace
/// as N x this, which also documents the floor being load-bearing.
constexpr std::uint64_t kR5Floor = 400'000;

std::vector<std::byte> HelloFrame(std::uint32_t version) {
    return protocol::EncodeMessage(wire::Body::Hello,
                                   [&](flatbuffers::FlatBufferBuilder& fbb) {
                                       wire::HelloBuilder b(fbb);
                                       b.add_protocol_version(version);
                                       return b.Finish();
                                   });
}

std::vector<std::byte> LeaseRequestFrame(std::uint32_t max_tasks) {
    return protocol::EncodeMessage(wire::Body::LeaseRequest,
                                   [&](flatbuffers::FlatBufferBuilder& fbb) {
                                       wire::LeaseRequestBuilder b(fbb);
                                       b.add_max_tasks(max_tasks);
                                       return b.Finish();
                                   });
}

std::vector<std::byte> ResultFrame(TaskId task, std::span<const std::byte> payload,
                                   std::uint32_t declared_bytes, std::uint64_t checksum) {
    return protocol::EncodeMessage(
        wire::Body::ResultHeader,
        [&](flatbuffers::FlatBufferBuilder& fbb) {
            const wire::Uuid tid = task.to_wire();
            wire::ResultHeaderBuilder b(fbb);
            b.add_task_id(&tid);
            b.add_payload_bytes(declared_bytes);
            b.add_checksum(checksum);
            return b.Finish();
        },
        payload);
}

/// Feed one encoded frame through the real verification path.
///
/// `frame` must outlive the call — VerifiedFrame is a view, not a copy.
Reaction Feed(Session& s, std::span<const std::byte> frame, std::uint64_t now_ms = 1000) {
    const auto verified = protocol::VerifyFrame(frame);
    REQUIRE(verified);
    return s.OnMessage(*verified, now_ms);
}

/// Decode a reply frame far enough to assert on it. Replies are our own output,
/// so this is a convenience rather than a trust boundary — it still goes through
/// VerifyFrame, which is a free self-check that we emit frames we would accept.
/// Verify and decode ONE reply frame. Replies are a list — one frame per
/// WebSocket message — so this takes the frame, not the whole reaction.
const wire::Envelope* ParseReply(std::span<const std::byte> reply) {
    const auto verified = protocol::VerifyFrame(reply);
    REQUIRE(verified);
    // Deliberately leaks the view's lifetime onto the caller's `reply` buffer;
    // every use below keeps that buffer alive.
    return verified->envelope();
}

/// The first reply frame, which is what almost every assertion wants.
const wire::Envelope* FirstReply(const Reaction& r) {
    REQUIRE_FALSE(r.replies.empty());
    return ParseReply(r.replies.front());
}

/// The REAL manifest, not a stub.
///
/// Granting a task now requires the job's kernel to be in the registry and its
/// params to be buildable (R1 — the coordinator owns both), so a fake registry
/// would test a path production never takes. Loading the shipped manifest also
/// means a manifest that stops parsing fails here rather than at startup on a
/// server.
const KernelRegistry& Registry() {
    static const auto reg = KernelRegistry::Load(
        std::filesystem::path(P2PGPU_KERNEL_DIR) / "manifest.toml",
        std::filesystem::path(P2PGPU_KERNEL_DIR));
    REQUIRE(reg);
    return *reg;
}

/// One coordinator plus a handshaked session, which most cases need.
struct Fixture {
    JobManager jobs;
    Fleet fleet;
    const KernelRegistry& kernels = Registry();
    Session session{jobs, kernels, fleet, /*conn_id=*/7, /*lease_ms=*/30000};

    void Handshake() {
        const auto hello = HelloFrame(protocol::kProtocolVersion);
        const Reaction r = Feed(session, hello);
        REQUIRE_FALSE(r.close);
        REQUIRE(session.handshaked());
    }

    /// Report a join-time benchmark score (2.11).
    ///
    /// REQUIRED before any grant. A worker with no score gets no work — the
    /// coordinator will not guess one, because a guess would then feed the
    /// correction factor and 2.13 would be correcting toward a fiction.
    /// Deliberately tiny. With target_ms = 2000 the sizer wants
    /// `2.0 s x score` units, so a score of 50 gives ~100-unit tasks — small
    /// enough that a test can reason about how many fit in a job.
    ///
    /// `brute_search_v1` has an R5 floor of 4e5 in the manifest, which would
    /// swamp that; these tests use a job whose keyspace is smaller than the
    /// floor, so the "cap at remaining" rule decides the size instead. That is
    /// the same path the LAST task of any real job takes (D-0043).
    void Benchmark(double score = 50.0) {
        const auto frame = protocol::EncodeMessage(
            wire::Body::BenchmarkResult, [&](flatbuffers::FlatBufferBuilder& fbb) {
                auto kid = fbb.CreateString("calibrate_v1");
                wire::BenchmarkResultBuilder b(fbb);
                b.add_kernel_id(kid);
                b.add_score(score);
                b.add_samples(4);
                return b.Finish();
            });
        const Reaction r = Feed(session, frame);
        REQUIRE(r.replies.empty());
    }

    void Ready() {
        Handshake();
        Benchmark();
    }
};

}  // namespace

TEST_CASE("Hello with the right version yields Welcome", "[session]") {
    Fixture f;
    const auto hello = HelloFrame(protocol::kProtocolVersion);
    const Reaction r = Feed(f.session, hello);

    CHECK_FALSE(r.close);
    REQUIRE_FALSE(r.replies.empty());
    const auto* env = FirstReply(r);
    REQUIRE(env->body_type() == wire::Body::Welcome);
    const auto* welcome = env->body_as_Welcome();
    REQUIRE(welcome != nullptr);
    REQUIRE(welcome->worker_id() != nullptr);
    CHECK(welcome->heartbeat_ms() > 0);
    CHECK(f.session.handshaked());
}

TEST_CASE("Version mismatch is FATAL and closes the connection", "[session]") {
    Fixture f;
    // The frame HEADER still carries our version — SplitFrame would have
    // rejected it otherwise, and this test is about the in-band check inside
    // the verified region.
    const auto hello = HelloFrame(protocol::kProtocolVersion + 1000);
    const Reaction r = Feed(f.session, hello);

    // Fatal, because retrying cannot succeed. A peer left reconnecting forever
    // against a version it can never satisfy is worse than a clean refusal.
    CHECK(r.close);
    REQUIRE_FALSE(r.replies.empty());
    const auto* env = FirstReply(r);
    REQUIRE(env->body_type() == wire::Body::Error);
    const auto* err = env->body_as_Error();
    REQUIRE(err != nullptr);
    CHECK(err->code() == wire::ErrorCode::VersionMismatch);
    CHECK(err->fatal());
    CHECK_FALSE(f.session.handshaked());
}

TEST_CASE("Work-bearing messages before Hello are refused", "[session]") {
    Fixture f;
    const auto lease = LeaseRequestFrame(1);
    const Reaction r = Feed(f.session, lease);

    // Without this, anyone could submit results without ever claiming an
    // identity — and reputation only means something if it attaches to someone.
    CHECK(r.close);
    CHECK_FALSE(f.session.handshaked());
}

TEST_CASE("Duplicate Hello is refused but not fatal", "[session]") {
    Fixture f;
    f.Handshake();
    const auto again = HelloFrame(protocol::kProtocolVersion);
    const Reaction r = Feed(f.session, again);

    CHECK_FALSE(r.close);
    const auto* env = FirstReply(r);
    REQUIRE(env->body_type() == wire::Body::Error);
    CHECK_FALSE(env->body_as_Error()->fatal());
}

TEST_CASE("Coordinator-to-worker messages are rejected inbound", "[session]") {
    Fixture f;
    f.Handshake();
    // A worker sending us a Welcome is confused or probing.
    const auto welcome = protocol::EncodeMessage(
        wire::Body::Welcome, [](flatbuffers::FlatBufferBuilder& fbb) {
            wire::WelcomeBuilder b(fbb);
            return b.Finish();
        });
    const Reaction r = Feed(f.session, welcome);
    const auto* env = FirstReply(r);
    REQUIRE(env->body_type() == wire::Body::Error);
    CHECK(env->body_as_Error()->code() == wire::ErrorCode::MalformedMessage);
    CHECK_FALSE(r.close);
}

TEST_CASE("LeaseRequest is clamped and bounded by the keyspace", "[session]") {
    Fixture f;
    (void)f.jobs.CreateJob(kKernel, 2 * kR5Floor, 7);
    f.Ready();

    // The worker asks WHETHER, never HOW MUCH (R1/D-0005): a request for 10000
    // tasks must not produce an unbounded backlog. `max_tasks` is clamped to 4,
    // and the keyspace here only holds 2.
    const auto req = LeaseRequestFrame(10000);
    const Reaction r = Feed(f.session, req);
    CHECK_FALSE(r.close);
    CHECK(r.replies.size() == 2);
    CHECK(f.jobs.HeldBy(f.session.worker_id()).size() == 2);
    CHECK(f.jobs.remaining_units() == 0);

    // Exhausted: a well-formed request that yields nothing is not an error.
    const auto again = LeaseRequestFrame(1);
    const Reaction r2 = Feed(f.session, again);
    CHECK(r2.replies.empty());
    CHECK_FALSE(r2.close);
}

TEST_CASE("A checksum-clean result is accepted end to end", "[session]") {
    Fixture f;
    const JobId job = f.jobs.CreateJob(kKernel, 1000, 7);
    f.Ready();
    {
        const auto req = LeaseRequestFrame(1);
        (void)Feed(f.session, req);
    }
    const auto held = f.jobs.HeldBy(f.session.worker_id());
    REQUIRE(held.size() == 1);
    const TaskId task = held.front();

    const std::vector<std::byte> payload(32, std::byte{0xAB});
    const auto frame = ResultFrame(task, payload, 32, Blake3_64(payload));
    const Reaction r = Feed(f.session, frame);

    CHECK(r.replies.empty());
    CHECK_FALSE(r.close);
    CHECK(f.jobs.Find(task)->state == TaskState::Accepted);
    CHECK(f.jobs.JobComplete(job));
}

TEST_CASE("Checksum mismatch requeues WITHOUT a penalty", "[session]") {
    Fixture f;
    (void)f.jobs.CreateJob(kKernel, kR5Floor, 7);
    f.Ready();
    {
        const auto req = LeaseRequestFrame(1);
        (void)Feed(f.session, req);
    }
    const TaskId task = f.jobs.HeldBy(f.session.worker_id()).front();

    const std::vector<std::byte> payload(32, std::byte{0xAB});
    const auto frame = ResultFrame(task, payload, 32, Blake3_64(payload) ^ 1U);
    const Reaction r = Feed(f.session, frame);

    const auto* env = FirstReply(r);
    REQUIRE(env->body_type() == wire::Body::Error);
    CHECK(env->body_as_Error()->code() == wire::ErrorCode::ChecksumMismatch);
    CHECK_FALSE(r.close);

    // Back to Queued, NOT Rejected. Corruption is not malice — this result was
    // never readable enough to be judged wrong, and penalising it would
    // blacklist honest workers on flaky networks.
    CHECK(f.jobs.Find(task)->state == TaskState::Queued);
    CHECK(f.jobs.queued() == 1);
}

TEST_CASE("A lied-about payload length is rejected before the state changes", "[session]") {
    Fixture f;
    (void)f.jobs.CreateJob(kKernel, kR5Floor, 7);
    f.Ready();
    {
        const auto req = LeaseRequestFrame(1);
        (void)Feed(f.session, req);
    }
    const TaskId task = f.jobs.HeldBy(f.session.worker_id()).front();

    // Declares 4 KiB, sends 32 bytes. The Heartbleed shape — SplitFrame already
    // bounded the buffer, so what this catches is the LIE, not an overread.
    const std::vector<std::byte> payload(32, std::byte{0xAB});
    const auto frame = ResultFrame(task, payload, 4096, Blake3_64(payload));
    const Reaction r = Feed(f.session, frame);

    const auto* env = FirstReply(r);
    REQUIRE(env->body_type() == wire::Body::Error);
    CHECK(env->body_as_Error()->code() == wire::ErrorCode::MalformedMessage);
    // The task is untouched: the length check runs before invariant 5.
    CHECK(f.jobs.Find(task)->state == TaskState::Leased);
}

TEST_CASE("A result for an unleased task is refused", "[session]") {
    Fixture f;
    (void)f.jobs.CreateJob(kKernel, kR5Floor, 7);
    f.Handshake();

    // Never requested a lease. The task exists and is Queued; submitting for it
    // is exactly what invariant 5 exists to stop.
    JobManager probe;
    const std::vector<std::byte> payload(8, std::byte{0x01});
    const auto frame = ResultFrame(TaskId{0, 1}, payload, 8, Blake3_64(payload));
    const Reaction r = Feed(f.session, frame);

    const auto* env = FirstReply(r);
    REQUIRE(env->body_type() == wire::Body::Error);
    CHECK(env->body_as_Error()->code() == wire::ErrorCode::LeaseNotHeld);
    CHECK_FALSE(r.close);
    CHECK(probe.total_tasks() == 0);
}

TEST_CASE("Disconnect releases every held lease immediately", "[session]") {
    Fixture f;
    // Three tasks' worth: every grant lands on the R5 floor.
    (void)f.jobs.CreateJob(kKernel, 3 * kR5Floor, 7);
    f.Ready();
    {
        const auto req = LeaseRequestFrame(3);
        (void)Feed(f.session, req);
    }
    REQUIRE(f.jobs.queued() == 0);

    f.session.OnDisconnect();

    // A worker vanishing is the NORMAL case (R8). Making the queue wait out a
    // 30-second lease for a socket we already know is gone stalls the job for
    // no reason.
    CHECK(f.jobs.queued() == 3);
    CHECK(f.jobs.HeldBy(f.session.worker_id()).empty());
}

TEST_CASE("Disconnect before the handshake is harmless", "[session]") {
    Fixture f;
    (void)f.jobs.CreateJob(kKernel, kR5Floor, 7);
    f.session.OnDisconnect();
    // Nothing was ever granted, so nothing comes back — and with tasks carved
    // on demand there is no pre-split task list to be disturbed either.
    CHECK(f.jobs.queued() == 0);
    CHECK(f.jobs.remaining_units() == kR5Floor);
}

// ── Step 2.10 — idempotent submission ────────────────────────────────────

TEST_CASE("a duplicate result is discarded SILENTLY", "[session]") {
    Fixture f;
    (void)f.jobs.CreateJob(kKernel, kR5Floor, 7);
    f.Ready();
    {
        const auto req = LeaseRequestFrame(1);
        (void)Feed(f.session, req);
    }
    const TaskId task = f.jobs.HeldBy(f.session.worker_id()).front();

    const std::vector<std::byte> payload(32, std::byte{0xAB});
    const auto frame = ResultFrame(task, payload, 32, Blake3_64(payload));

    const Reaction first = Feed(f.session, frame);
    CHECK(first.replies.empty());
    CHECK(f.jobs.Find(task)->state == TaskState::Accepted);

    // SILENT. Not an error, not a reply, not a state change. Once speculation
    // exists (2.17) a duplicate is the normal outcome of a race, and the worker
    // that lost has done nothing wrong — returning an error would teach it to
    // retry, or show up in whatever counts protocol errors.
    const Reaction second = Feed(f.session, frame);
    CHECK(second.replies.empty());
    CHECK_FALSE(second.close);
    CHECK(f.jobs.Find(task)->state == TaskState::Accepted);
}

TEST_CASE("Goodbye releases every held lease immediately", "[session]") {
    Fixture f;
    // Three tasks' worth: every grant lands on the R5 floor.
    (void)f.jobs.CreateJob(kKernel, 3 * kR5Floor, 7);
    f.Ready();
    {
        const auto req = LeaseRequestFrame(3);
        (void)Feed(f.session, req);
    }
    REQUIRE(f.jobs.queued() == 0);

    const auto bye = protocol::EncodeMessage(
        wire::Body::Goodbye, [](flatbuffers::FlatBufferBuilder& fbb) {
            wire::GoodbyeBuilder b(fbb);
            b.add_reason(wire::ReleaseReason::UserStopped);
            return b.Finish();
        });
    const Reaction r = Feed(f.session, bye);

    // Identical handling to a disconnect, deliberately — the only difference is
    // that we were told. Releasing now is what stops a polite worker's tasks
    // waiting out a full lease.
    CHECK(r.replies.empty());
    CHECK(f.jobs.queued() == 3);
    CHECK(f.jobs.HeldBy(f.session.worker_id()).empty());
}

TEST_CASE("Progress renews the lease it names", "[session]") {
    Fixture f;
    (void)f.jobs.CreateJob(kKernel, kR5Floor, 7);
    f.Ready();
    {
        const auto req = LeaseRequestFrame(1);
        (void)Feed(f.session, req, /*now_ms=*/1000);
    }
    const TaskId task = f.jobs.HeldBy(f.session.worker_id()).front();
    const std::uint64_t first_deadline = f.jobs.Find(task)->lease_expires_at_ms;

    const auto prog = protocol::EncodeMessage(
        wire::Body::Progress, [&](flatbuffers::FlatBufferBuilder& fbb) {
            const wire::Uuid tid = task.to_wire();
            wire::ProgressBuilder b(fbb);
            b.add_task_id(&tid);
            b.add_fraction_done(0.5F);
            b.add_request_renew(true);
            return b.Finish();
        });
    const Reaction r = Feed(f.session, prog, /*now_ms=*/9000);

    CHECK(r.replies.empty());
    CHECK(f.jobs.Find(task)->lease_expires_at_ms > first_deadline);
    CHECK(f.jobs.Find(task)->state == TaskState::Leased);
}

TEST_CASE("a heartbeat without request_renew does not extend anything", "[session]") {
    Fixture f;
    (void)f.jobs.CreateJob(kKernel, kR5Floor, 7);
    f.Ready();
    {
        const auto req = LeaseRequestFrame(1);
        (void)Feed(f.session, req, /*now_ms=*/1000);
    }
    const TaskId task = f.jobs.HeldBy(f.session.worker_id()).front();
    const std::uint64_t deadline = f.jobs.Find(task)->lease_expires_at_ms;

    const auto beat = protocol::EncodeMessage(
        wire::Body::Progress, [&](flatbuffers::FlatBufferBuilder& fbb) {
            const wire::Uuid tid = task.to_wire();
            wire::ProgressBuilder b(fbb);
            b.add_task_id(&tid);
            b.add_request_renew(false);
            return b.Finish();
        });
    (void)Feed(f.session, beat, /*now_ms=*/9000);

    // Liveness and lease extension are separate facts. A worker saying "still
    // here" must not silently keep a task it has stopped working on.
    CHECK(f.jobs.Find(task)->lease_expires_at_ms == deadline);
}

// ── D-0046 — a revoke must be retrievable without the loser saying anything ──
//
// This is the regression test for the defect that made speculation report a
// saving it did not make. The old design appended queued revokes to the reply
// of the loser's NEXT inbound message; a worker executing a task it has already
// lost is silent by definition, so the stop arrived attached to the reply to its
// own result submission — after every unit it was meant to save was spent.
//
// The assertion is therefore specifically about the ABSENCE of a stimulus: the
// loser's session is fed nothing at all between the cancellation and the drain.
// That is what the sweep timer does, and it is the property that a return to
// piggybacking would break.
TEST_CASE("a queued revoke is deliverable with no inbound frame from the loser",
          "[session][speculation]") {
    JobManager jobs;
    Fleet fleet;
    const KernelRegistry& kernels = Registry();

    // Both identities come from real handshakes rather than a test-only
    // setter — the id the coordinator assigns is the one the sweep will look
    // up, and inventing it here could agree with nothing.
    Session winner_session{jobs, kernels, fleet, /*conn_id=*/1, /*lease_ms=*/30000};
    Session loser_session{jobs, kernels, fleet, /*conn_id=*/2, /*lease_ms=*/30000};
    for (Session* s : {&winner_session, &loser_session}) {
        const auto hello = HelloFrame(protocol::kProtocolVersion);
        const Reaction r = Feed(*s, hello);
        REQUIRE_FALSE(r.close);
        REQUIRE(s->handshaked());
    }
    const WorkerId winner = winner_session.worker_id();
    const WorkerId loser = loser_session.worker_id();
    REQUIRE(winner != loser);

    // From here the LOSER's session is handed no frames at all.

    (void)jobs.CreateJob("brute_search_v1", /*total_units=*/1000, /*seed=*/1);
    const auto original = jobs.Grant(winner, 1000, 30000, 500);
    const auto other = jobs.Grant(winner, 1000, 30000, 500);
    REQUIRE(original);
    REQUIRE(other);
    REQUIRE(jobs.remaining_units() == 0);

    // Speculation needs the keyspace exhausted AND the job near done. Finish
    // one of the two so the fraction is 0.5, then pass an explicit threshold
    // rather than carving 20 tasks to reach the 95% default — the delivery
    // path is what is under test here, not the trigger condition (which
    // test_leases.cpp covers directly).
    REQUIRE(jobs.Submit(winner, other->id));
    REQUIRE(jobs.Finish(other->id, /*accepted=*/true));
    const auto replica = jobs.IssueSpeculative(loser, 1000, 30000, /*threshold=*/0.4);
    REQUIRE(replica);
    REQUIRE(replica->replica_of == original->id);

    // The winner finishes; the replica is cancelled and a revoke is queued
    // against the loser's fleet record.
    REQUIRE(jobs.Submit(winner, original->id));
    REQUIRE(jobs.Finish(original->id, /*accepted=*/true));
    const auto cancelled = jobs.CancelSiblingsOf(original->id);
    REQUIRE(cancelled.size() == 1);
    CHECK(cancelled.front().task == replica->id);
    fleet.Mutable(cancelled.front().holder)->pending_revokes.push_back(
        cancelled.front().task);

    // THE POINT: drained with no message from the loser, exactly as the sweep
    // timer does it. Under the old design this returned nothing here and the
    // frame only appeared once the loser spoke.
    const auto frames = loser_session.DrainRevokes();
    REQUIRE(frames.size() == 1);

    // And the queue is emptied, so a second sweep does not re-send it.
    CHECK(loser_session.DrainRevokes().empty());

    // WHAT THIS DOES NOT COVER, stated plainly: it pins the API property (a
    // revoke is retrievable with no stimulus) and would fail to compile if
    // `DrainRevokes` went private again. It does NOT prove `Server::Sweep`
    // still calls it — that is transport code with no unit-test seam, and the
    // evidence for it is the live run in `results/2.15-2.18-speculation.md`
    // where the fleet's `revoked` count went from 0 to non-zero.
}

// ── 3.13 — reputation survives a RECONNECT ───────────────────────────────
//
// Phase 3 exit criterion 6 is "reputation survives restart and reconnect".
// The restart half is `test_store.cpp`; this is the reconnect half, and it is
// tested here rather than against a live fleet because a flap fires with
// probability 0.003 per poll — waiting for one is not a test.

namespace {

std::vector<std::byte> HelloWithToken(const std::string& token) {
    return protocol::EncodeMessage(wire::Body::Hello,
                                   [&](flatbuffers::FlatBufferBuilder& fbb) {
                                       auto t = fbb.CreateString(token);
                                       wire::HelloBuilder b(fbb);
                                       b.add_protocol_version(protocol::kProtocolVersion);
                                       b.add_resume_token(t);
                                       return b.Finish();
                                   });
}

/// The `session_token` the coordinator minted, read back off the Welcome.
std::string TokenFromWelcome(const Reaction& r) {
    for (const auto& reply : r.replies) {
        const auto verified = protocol::VerifyFrame(reply);
        if (!verified) {
            continue;
        }
        if (const auto* w = verified->envelope()->body_as_Welcome();
            w != nullptr && w->session_token() != nullptr) {
            return w->session_token()->str();
        }
    }
    return {};
}

}  // namespace

TEST_CASE("a reconnect with a valid token keeps its reputation", "[session]") {
    JobManager jobs;
    Fleet fleet;
    ReputationTable rep;
    const KernelRegistry& kernels = Registry();

    // First connection: handshake, then build a record.
    std::string token;
    WorkerId original;
    {
        Session s{jobs, kernels, fleet, /*conn_id=*/1, /*lease_ms=*/30000};
        s.SetReputation(&rep);
        const Reaction r = Feed(s, HelloFrame(protocol::kProtocolVersion));
        REQUIRE_FALSE(r.close);
        original = s.worker_id();
        token = TokenFromWelcome(r);
        REQUIRE_FALSE(token.empty());
        // A guessable token is a reputation-theft primitive (D-0056), so at
        // minimum it must not be short or constant.
        CHECK(token.size() >= 16);
    }
    for (int i = 0; i < 50; ++i) {
        rep.RecordAccepted(original);
    }
    const double earned = rep.ScoreOf(original);
    REQUIRE(earned > 0.9);

    // Reconnect on a DIFFERENT connection, presenting the token.
    Session again{jobs, kernels, fleet, /*conn_id=*/99, /*lease_ms=*/30000};
    again.SetReputation(&rep);
    const Reaction r2 = Feed(again, HelloWithToken(token));
    REQUIRE_FALSE(r2.close);

    // Same identity, same standing. Without this a browser tab reload throws
    // away fifty correct results and the worker is replicated from scratch.
    CHECK(again.worker_id() == original);
    CHECK(rep.ScoreOf(again.worker_id()) == earned);
}

TEST_CASE("an unknown token is not an error, just a new identity", "[session]") {
    JobManager jobs;
    Fleet fleet;
    ReputationTable rep;
    Session s{jobs, Registry(), fleet, /*conn_id=*/5, /*lease_ms=*/30000};
    s.SetReputation(&rep);

    const Reaction r = Feed(s, HelloWithToken("not-a-real-token"));
    // Tokens expire and coordinators restart. Rejecting a stale one would break
    // exactly the honest reconnects this feature exists for.
    CHECK_FALSE(r.close);
    CHECK(s.handshaked());
}

TEST_CASE("a token cannot be used to steal another worker's standing",
          "[session]") {
    JobManager jobs;
    Fleet fleet;
    ReputationTable rep;
    const KernelRegistry& kernels = Registry();

    Session a{jobs, kernels, fleet, /*conn_id=*/1, /*lease_ms=*/30000};
    a.SetReputation(&rep);
    const std::string tok_a = TokenFromWelcome(Feed(a, HelloFrame(protocol::kProtocolVersion)));

    Session b{jobs, kernels, fleet, /*conn_id=*/2, /*lease_ms=*/30000};
    b.SetReputation(&rep);
    const std::string tok_b = TokenFromWelcome(Feed(b, HelloFrame(protocol::kProtocolVersion)));

    // Two connections, two DISTINCT secrets. Identical tokens would mean every
    // worker could assume every other worker's record.
    CHECK(tok_a != tok_b);
    CHECK(a.worker_id() != b.worker_id());
}
