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
const wire::Envelope* ParseReply(std::span<const std::byte> reply) {
    const auto verified = protocol::VerifyFrame(reply);
    REQUIRE(verified);
    // Deliberately leaks the view's lifetime onto the caller's `reply` buffer;
    // every use below keeps that buffer alive.
    return verified->envelope();
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
    const KernelRegistry& kernels = Registry();
    Session session{jobs, kernels, /*conn_id=*/7};

    void Handshake() {
        const auto hello = HelloFrame(protocol::kProtocolVersion);
        const Reaction r = Feed(session, hello);
        REQUIRE_FALSE(r.close);
        REQUIRE(session.handshaked());
    }
};

}  // namespace

TEST_CASE("Hello with the right version yields Welcome", "[session]") {
    Fixture f;
    const auto hello = HelloFrame(protocol::kProtocolVersion);
    const Reaction r = Feed(f.session, hello);

    CHECK_FALSE(r.close);
    REQUIRE_FALSE(r.reply.empty());
    const auto* env = ParseReply(r.reply);
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
    REQUIRE_FALSE(r.reply.empty());
    const auto* env = ParseReply(r.reply);
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
    const auto* env = ParseReply(r.reply);
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
    const auto* env = ParseReply(r.reply);
    REQUIRE(env->body_type() == wire::Body::Error);
    CHECK(env->body_as_Error()->code() == wire::ErrorCode::MalformedMessage);
    CHECK_FALSE(r.close);
}

TEST_CASE("LeaseRequest grants at most what is queued and clamps the ask", "[session]") {
    Fixture f;
    (void)f.jobs.CreateJob(kKernel, 1000, 2, 7);
    f.Handshake();

    // The worker asks WHETHER, never HOW MUCH (R1/D-0005): a request for 10000
    // tasks must not produce an unbounded backlog.
    const auto req = LeaseRequestFrame(10000);
    const Reaction r = Feed(f.session, req);
    CHECK_FALSE(r.close);
    CHECK_FALSE(r.reply.empty());
    CHECK(f.jobs.queued() == 0);
    CHECK(f.jobs.HeldBy(f.session.worker_id()).size() == 2);

    // Empty queue: a well-formed request that yields nothing is not an error.
    const auto again = LeaseRequestFrame(1);
    const Reaction r2 = Feed(f.session, again);
    CHECK(r2.reply.empty());
    CHECK_FALSE(r2.close);
}

TEST_CASE("A checksum-clean result is accepted end to end", "[session]") {
    Fixture f;
    const JobId job = f.jobs.CreateJob(kKernel, 1000, 1, 7);
    f.Handshake();
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

    CHECK(r.reply.empty());
    CHECK_FALSE(r.close);
    CHECK(f.jobs.Find(task)->state == TaskState::Accepted);
    CHECK(f.jobs.JobComplete(job));
}

TEST_CASE("Checksum mismatch requeues WITHOUT a penalty", "[session]") {
    Fixture f;
    (void)f.jobs.CreateJob(kKernel, 1000, 1, 7);
    f.Handshake();
    {
        const auto req = LeaseRequestFrame(1);
        (void)Feed(f.session, req);
    }
    const TaskId task = f.jobs.HeldBy(f.session.worker_id()).front();

    const std::vector<std::byte> payload(32, std::byte{0xAB});
    const auto frame = ResultFrame(task, payload, 32, Blake3_64(payload) ^ 1U);
    const Reaction r = Feed(f.session, frame);

    const auto* env = ParseReply(r.reply);
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
    (void)f.jobs.CreateJob(kKernel, 1000, 1, 7);
    f.Handshake();
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

    const auto* env = ParseReply(r.reply);
    REQUIRE(env->body_type() == wire::Body::Error);
    CHECK(env->body_as_Error()->code() == wire::ErrorCode::MalformedMessage);
    // The task is untouched: the length check runs before invariant 5.
    CHECK(f.jobs.Find(task)->state == TaskState::Leased);
}

TEST_CASE("A result for an unleased task is refused", "[session]") {
    Fixture f;
    (void)f.jobs.CreateJob(kKernel, 1000, 1, 7);
    f.Handshake();

    // Never requested a lease. The task exists and is Queued; submitting for it
    // is exactly what invariant 5 exists to stop.
    JobManager probe;
    const std::vector<std::byte> payload(8, std::byte{0x01});
    const auto frame = ResultFrame(TaskId{0, 1}, payload, 8, Blake3_64(payload));
    const Reaction r = Feed(f.session, frame);

    const auto* env = ParseReply(r.reply);
    REQUIRE(env->body_type() == wire::Body::Error);
    CHECK(env->body_as_Error()->code() == wire::ErrorCode::LeaseNotHeld);
    CHECK_FALSE(r.close);
    CHECK(probe.total_tasks() == 0);
}

TEST_CASE("Disconnect releases every held lease immediately", "[session]") {
    Fixture f;
    (void)f.jobs.CreateJob(kKernel, 1000, 3, 7);
    f.Handshake();
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
    (void)f.jobs.CreateJob(kKernel, 1000, 1, 7);
    f.session.OnDisconnect();
    CHECK(f.jobs.queued() == 1);
}
