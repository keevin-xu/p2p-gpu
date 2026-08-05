// One virtual worker — steps 2.1-2.3, 2.5. See virtual_worker.hpp.
//
// N of these run in ONE process on one loop, not N processes. Independent
// identity, capabilities, and simulated throughput from a seeded distribution.

#include "virtual_worker.hpp"

#include <blake3.h>
#include <spdlog/spdlog.h>

#include <array>

#include <algorithm>
#include <iterator>
#include <cstring>
#include <utility>

#include "p2pgpu/kernels/params.hpp"
#include "p2pgpu/kernels/reference.hpp"
#include "p2pgpu/protocol/encode.hpp"
#include "p2pgpu/protocol/verify.hpp"

namespace p2pgpu::mock {
namespace {

using Clock = std::chrono::steady_clock;

/// BLAKE3-64, matching the coordinator's copy byte for byte (invariant 9).
/// Duplicated rather than shared because the coordinator's lives in
/// p2pgpu-coordinator-core and the worker's in worker-core, and neither is a
/// dependency the mock should take.
std::uint64_t Blake3_64(std::span<const std::byte> bytes) noexcept {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, bytes.data(), bytes.size());
    std::array<std::uint8_t, 8> digest{};
    blake3_hasher_finalize(&h, digest.data(), digest.size());
    std::uint64_t out = 0;
    for (std::size_t i = 0; i < digest.size(); ++i) {
        out |= static_cast<std::uint64_t>(digest[i]) << (8U * i);
    }
    return out;
}

/// Nominal wall time a task "should" take on a reference device, before the
/// worker's own speed factor. Settable with `--ms-per-mega-unit`.
///
/// ── WHY THE DEFAULT IS 600 AND NOT 4 ─────────────────────────────────────
/// Mocks compute REAL answers (D-0042) and every virtual worker is polled on
/// ONE thread, so the entire fleet shares a single core. Measured on an M4 Pro,
/// `BruteSearchReference` runs at ~6.6e8 units/s — that is the fleet's TOTAL
/// budget, not each worker's.
///
/// The original 4.0 declared 2.5e8 units/s PER worker. Twelve of those demand
/// 3e9 units/s from a core that can supply 6.6e8: 4.5x oversubscribed, and the
/// symptom was a 90 s run completing eleven tasks with nothing in the log to
/// say why. 600 ms/mega-unit declares ~1.7e6 units/s each, so even a
/// 100-worker sweep (E1) stays inside a quarter of the budget.
///
/// It is a CONSTANT rather than something scaled by fleet size on purpose: E1
/// sweeps the worker count 1 -> 100, and per-worker speed that changed with the
/// fleet would make its throughput curve meaningless.
double g_nominal_ms_per_mega_unit = 600.0;

/// Measured single-thread throughput of `BruteSearchReference`, units/sec.
/// Used only to warn when a run is oversubscribed — see `WarnIfOversubscribed`.
constexpr double kReferenceUnitsPerSec = 6.6e8;

/// How often a working worker renews. Deliberately well under any sane lease:
/// the point of renewal is to prove liveness *before* expiry, not to race it.
constexpr auto kRenewInterval = std::chrono::milliseconds(2000);

}  // namespace

void SetNominalMsPerMegaUnit(double ms) {
    if (ms > 0.0) {
        g_nominal_ms_per_mega_unit = ms;
    }
}

bool WarnIfOversubscribed(std::uint32_t fleet_size) {
    const double per_worker = 1.0e9 / g_nominal_ms_per_mega_unit;
    const double demanded = per_worker * static_cast<double>(fleet_size);
    // Half the measured budget, not all of it: the same thread also runs the
    // event loop, framing, and BLAKE3 over every result.
    if (demanded <= 0.5 * kReferenceUnitsPerSec) {
        return false;
    }
    spdlog::warn("OVERSUBSCRIBED: {} workers x {:.3g} units/s = {:.3g} units/s demanded, "
                 "but one thread supplies ~{:.3g}. Mocks compute REAL answers on a "
                 "SINGLE shared thread, so this run will measure the harness's own "
                 "backlog rather than scheduling. Raise --ms-per-mega-unit or lower "
                 "--count.",
                 fleet_size, per_worker, demanded, kReferenceUnitsPerSec);
    return true;
}

VirtualWorker::VirtualWorker(std::uint32_t index, std::string url, Behaviors behaviors,
                             std::uint64_t run_seed)
    : index_(index),
      url_(std::move(url)),
      behaviors_(behaviors),
      dice_(run_seed, index),
      next_action_(Clock::now()) {}

VirtualWorker::~VirtualWorker() = default;

void VirtualWorker::Start() {
    if (running_) {
        return;
    }
    running_ = true;

    transport_.OnOpen([this] {
        connected_ = true;
        SendHello();
    });
    transport_.OnClosed([this] {
        connected_ = false;
        handshaked_ = false;
        lease_outstanding_ = false;
        // Anything in flight is abandoned. The coordinator releases our leases
        // on disconnect (R8) and must requeue them without penalty — that
        // requeue is exactly what E3 counts.
        stats_.tasks_abandoned += static_cast<std::uint32_t>(in_flight_.size());
        in_flight_.clear();
    });
    transport_.OnError([](const std::string&) { /* counted via OnClosed */ });
    transport_.OnMessage([this](std::span<const std::byte> bytes) {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        inbox_.emplace_back(bytes.begin(), bytes.end());
    });

    transport_.Connect(url_);
}

void VirtualWorker::Stop() {
    running_ = false;
    transport_.Close();
}

void VirtualWorker::Poll() {
    for (;;) {
        std::vector<std::byte> frame;
        {
            std::lock_guard<std::mutex> lock(inbox_mutex_);
            if (inbox_.empty()) {
                break;
            }
            frame = std::move(inbox_.front());
            inbox_.pop_front();
        }
        OnFrame(frame);
    }

    if (!running_) {
        return;
    }

    const auto now = Clock::now();

    // Finish any task whose simulated duration has elapsed. The ANSWER was
    // computed at grant time; this is only the clock catching up, which is what
    // decouples "slow worker" from "busy CPU" (D-0042).
    // Keep the lease alive while working (2.6). `never_renews_lease` is what
    // makes this a real behaviour rather than a declared one: a flagged worker
    // stalls with the task held and the sweep takes it back, which is exactly
    // the case expiry exists for and could not otherwise be tested.
    if (!behaviors_.never_renews_lease) {
        for (auto& task : in_flight_) {
            if (task.renew_at <= now) {
                SendRenew(task);
                task.renew_at = now + kRenewInterval;
            }
        }
    }

    while (!in_flight_.empty() && in_flight_.front().finish_at <= now) {
        const Pending task = std::move(in_flight_.front());
        in_flight_.pop_front();
        FinishTask(task);
    }

    if (!handshaked_ || now < next_action_) {
        return;
    }

    // Hostile profile (2.5). Interleaved with ordinary traffic on purpose: a
    // coordinator that survives pure garbage but drops good frames alongside it
    // has not passed.
    if (behaviors_.malformed_frames) {
        SendMalformed();
        next_action_ = now + std::chrono::milliseconds(20);
        return;
    }

    // Flapping: drop the connection and let the fleet loop restart us. Distinct
    // from dying — this worker keeps coming BACK, with a new identity each
    // time, which is what makes reputation hard (Phase 3).
    if (behaviors_.flaps > 0.0 && in_flight_.empty() && dice_.chance(behaviors_.flaps * 0.02)) {
        ++stats_.reconnects;
        transport_.Close();
        next_action_ = now + std::chrono::milliseconds(100 + (dice_.next() % 400));
        return;
    }

    if (in_flight_.empty() && !lease_outstanding_) {
        RequestLease();
    }
}

void VirtualWorker::OnFrame(std::span<const std::byte> bytes) {
    // Same sanctioned path as everyone else (R11). The mock is not exempt: a
    // harness that parses frames its own way would stop testing the real one.
    std::vector<std::byte> scratch;
    const auto aligned = protocol::AlignFrame(bytes, scratch);
    const auto verified = protocol::VerifyFrame(aligned);
    if (!verified) {
        return;
    }

    const wire::Envelope& env = *verified->envelope();
    if (env.body_type() == wire::Body::Welcome) {
        handshaked_ = true;
        // 3.13 — keep the resume token across a flap, so a reconnecting mock
        // reclaims its reputation instead of arriving as a stranger.
        //
        // The mock is a SEPARATE protocol client from `worker-core` (D-0042),
        // so it does not inherit `TaskLoop`'s handling — and without this the
        // only fleet large enough to exercise reconnection could not exercise
        // it at all. Exit criterion 6 says "survives restart AND reconnect";
        // this is the reconnect half.
        if (const auto* w = env.body_as_Welcome();
            w != nullptr && w->session_token() != nullptr) {
            resume_token_ = w->session_token()->str();
        }
        return;
    }
    if (env.body_type() == wire::Body::BenchmarkRequest) {
        // A SYNTHETIC score matching this worker's simulated speed (2.11). It
        // must be consistent with `slow_factor`, or the sizer would be
        // correcting against a fiction and 2.13's convergence plot would show
        // the harness disagreeing with itself rather than anything real.
        //
        // Derived from the same constant the task simulation uses, so "score"
        // and "how long tasks actually take" cannot drift apart.
        // ARITHMETIC OPS PER SECOND, matching what a real worker reports —
        // the coordinator divides by the kernel's flop_per_unit. Reporting
        // units/sec here would work by accident today and break the moment a
        // second kernel exists.
        //
        // Derived from the same constant the task simulation uses, so "score"
        // and "how long tasks actually take" cannot drift apart.
        constexpr double kOpsPerUnit = 80.0;   // brute_search_v1, manifest

        // 1e6 units take `g_nominal_ms_per_mega_unit * slow_factor` MILLISECONDS,
        // so the rate is 1e6 units per (that / 1000) seconds = 1e9 / (kNom *
        // slow). Written as one expression deliberately: the previous version
        // was `1e6 / kNom * 1000 / slow / 1000`, whose `* 1000` and `/ 1000`
        // cancelled, making every worker report a score **1000x too slow**.
        //
        // How it hid: the coordinator's EWMA correction absorbs a mis-scaled
        // score by design, so the fleet still worked — it just sized every task
        // 1000x too small. The tell was that every worker's correction sat at
        // 0.12-0.17, pinned against the 0.1 clamp, trying to express 0.001.
        // Nothing failed; the numbers were simply fiction, which is exactly
        // what the comment above swears must not happen.
        const double units_per_sec =
            1.0e9 / g_nominal_ms_per_mega_unit / behaviors_.slow_factor;
        // Inflated for straggler simulation: the coordinator sizes for a
        // machine faster than this one actually is (E5).
        const double ops_per_sec =
            units_per_sec * kOpsPerUnit * behaviors_.score_inflation;
        auto frame = protocol::EncodeMessage(
            wire::Body::BenchmarkResult, [&](flatbuffers::FlatBufferBuilder& fbb) {
                auto kid = fbb.CreateString("calibrate_v1");
                wire::BenchmarkResultBuilder b(fbb);
                b.add_kernel_id(kid);
                b.add_score(ops_per_sec);
                b.add_samples(4);
                return b.Finish();
            });
        (void)transport_.Send(frame);
        return;
    }
    if (env.body_type() == wire::Body::Revoke) {
        // Stop promptly (2.17). A speculative loser that keeps going burns
        // compute on a range already finished — and the wasted-work metric E5
        // reports would understate the cost if workers ignored this.
        if (const auto* rev = env.body_as_Revoke(); rev != nullptr &&
                                                    rev->task_id() != nullptr) {
            const protocol::TaskId id{*rev->task_id()};
            const auto before = in_flight_.size();
            std::erase_if(in_flight_, [&](const Pending& p) { return p.id == id; });
            stats_.tasks_revoked += static_cast<std::uint32_t>(before - in_flight_.size());
        }
        return;
    }
    if (env.body_type() != wire::Body::TaskGrant) {
        return;
    }

    const auto* grant = env.body_as_TaskGrant();
    const auto* tenv = grant != nullptr ? grant->envelope() : nullptr;
    if (tenv == nullptr || tenv->task_id() == nullptr) {
        return;
    }
    lease_outstanding_ = false;

    Pending task;
    task.id = protocol::TaskId{*tenv->task_id()};
    task.start_unit = tenv->start_unit();
    task.work_units = tenv->work_units();
    if (const auto* p = tenv->params()) {
        task.params.assign(reinterpret_cast<const std::byte*>(p->data()),
                           reinterpret_cast<const std::byte*>(p->data()) + p->size());
    }
    if (const auto* out = tenv->output_spec()) {
        task.output_bytes = out->bytes();
    }
    BeginTask(std::move(task));
}

void VirtualWorker::BeginTask(Pending task) {
    // Compute the REAL answer now, then sleep. Honest workers must actually
    // agree with each other, or replication has nothing to compare (D-0042).
    kernels::BruteSearchParams p{};
    if (task.params.size() >= sizeof(p)) {
        std::memcpy(&p, task.params.data(), sizeof(p));
        // The coordinator's params carry the task's range; the real worker's
        // kernel host rewrites the chunk window per dispatch and the mock has
        // no chunks, so use the range as granted.
        p.start_lo = static_cast<std::uint32_t>(task.start_unit & 0xFFFFFFFFULL);
        p.unit_count = static_cast<std::uint32_t>(task.work_units);
    }
    auto result = kernels::BruteSearchReference(p);

    const bool lying = dice_.chance(behaviors_.lies_probabilistically);
    const bool garbage = dice_.chance(behaviors_.returns_garbage);
    if (lying || garbage) {
        // PLAUSIBLE, not obviously broken. A liar that returns zeros is caught
        // by inspection; the adversary worth defending against returns
        // something that looks like a result and is wrong (E4).
        result.found_count += 1;
        // A colluding liar derives its lie from the SHARED key rather than its
        // own RNG, so every colluder on this range produces byte-identical
        // output and naive quorum reads them as corroborating each other
        // (3.16). Mixing in the task's start_unit keeps the lie plausible —
        // one constant for every range would be obvious by inspection.
        const std::uint64_t fabricated =
            behaviors_.collusion_key != 0
                ? behaviors_.collusion_key ^ task.start_unit
                : dice_.next();
        result.match_xor ^= static_cast<std::uint32_t>(fabricated);
        if (lying) {
            ++stats_.tasks_lied_about;
        }
    }

    task.result.resize(sizeof(result));
    std::memcpy(task.result.data(), &result, sizeof(result));

    // Simulated duration: nominal work scaled by this worker's speed factor.
    // Real CPU time is unrelated — that is what lets 200 "slow" workers share
    // a handful of cores.
    const double mega_units = static_cast<double>(task.work_units) / 1.0e6;
    const double ms = std::max(1.0, mega_units * g_nominal_ms_per_mega_unit *
                                        behaviors_.slow_factor) +
                      behaviors_.high_latency_ms;
    task.finish_at = Clock::now() + std::chrono::milliseconds(static_cast<long long>(ms));
    task.renew_at = Clock::now() + kRenewInterval;
    in_flight_.push_back(std::move(task));
}

void VirtualWorker::FinishTask(const Pending& task) {
    // Die mid-task: drop the socket with the result computed but unsent. The
    // coordinator must requeue with NO penalty (R8), and E3 asserts zero tasks
    // are lost to this.
    if (dice_.chance(behaviors_.dies_mid_task)) {
        ++stats_.tasks_abandoned;
        transport_.Close();
        return;
    }

    const std::span<const std::byte> payload{task.result};
    // A REAL checksum. Without it every result fails invariant 9 and is
    // requeued, and the harness would measure a retry loop rather than a fleet.
    //
    // Note what this deliberately does NOT do: corrupt the checksum when the
    // worker is lying. A liar sends a valid checksum over false data, because
    // the checksum proves transport integrity and says nothing about truth —
    // conflating the two would let the coordinator "catch" liars for free and
    // make Phase 3's replication look unnecessary.
    auto frame = protocol::EncodeMessage(
        wire::Body::ResultHeader,
        [&](flatbuffers::FlatBufferBuilder& fbb) {
            const wire::Uuid tid = task.id.to_wire();
            wire::ResultHeaderBuilder b(fbb);
            b.add_task_id(&tid);
            b.add_payload_bytes(static_cast<std::uint32_t>(payload.size()));
            b.add_checksum(Blake3_64(payload));
            return b.Finish();
        },
        payload);

    (void)transport_.Send(frame);
    if (behaviors_.duplicate_submit) {
        // Step 2.10: the second must be a silent no-op, not an error. This
        // stops being pathological once speculation makes duplicates routine.
        (void)transport_.Send(frame);
    }
    ++stats_.tasks_completed;
}

void VirtualWorker::SendHello() {
    auto frame = protocol::EncodeMessage(
        wire::Body::Hello, [&](flatbuffers::FlatBufferBuilder& fbb) {
            auto ua = fbb.CreateString("p2pgpu-mock-worker");
            wire::WorkerCapabilitiesBuilder cb(fbb);
            cb.add_kind(wire::WorkerKind::Mock);
            cb.add_user_agent(ua);
            cb.add_supports_webrtc(false);
            auto caps = cb.Finish();

            auto resume = fbb.CreateString(resume_token_);

            wire::HelloBuilder hb(fbb);
            hb.add_protocol_version(protocol::kProtocolVersion);
            hb.add_resume_token(resume);
            hb.add_capabilities(caps);
            return hb.Finish();
        });
    (void)transport_.Send(frame);
}

void VirtualWorker::RequestLease() {
    auto frame = protocol::EncodeMessage(
        wire::Body::LeaseRequest, [&](flatbuffers::FlatBufferBuilder& fbb) {
            wire::LeaseRequestBuilder b(fbb);
            b.add_max_tasks(1);
            return b.Finish();
        });
    if (transport_.Send(frame)) {
        lease_outstanding_ = true;
    }
    // Jittered, so a fleet of 200 does not synchronise into a thundering herd
    // against an empty queue (RISKS.md §2, step 2.15).
    next_action_ = Clock::now() + std::chrono::milliseconds(5 + (dice_.next() % 45));
}

void VirtualWorker::SendRenew(const Pending& task) {
    auto frame = protocol::EncodeMessage(
        wire::Body::Progress, [&](flatbuffers::FlatBufferBuilder& fbb) {
            const wire::Uuid tid = task.id.to_wire();
            wire::ProgressBuilder b(fbb);
            b.add_task_id(&tid);
            // fraction_done is TELEMETRY the coordinator must not act on
            // (invariant 8). Reported honestly anyway — a harness that lied
            // here by default would make the dishonest profiles less distinct.
            b.add_fraction_done(0.5F);
            b.add_request_renew(true);
            return b.Finish();
        });
    (void)transport_.Send(frame);
}

void VirtualWorker::SendMalformed() {
    // Step 2.5. Six shapes, cycled — the same families the Phase 1 corpus
    // covers, but arriving on a live socket with connection state behind them.
    // The coordinator must reject every one, count it, and STAY UP.
    std::vector<std::byte> junk;
    switch (stats_.malformed_sent % 6) {
        case 0:   // truncated header
            junk.assign(7, std::byte{0xFF});
            break;
        case 1: { // valid header, fb_len far beyond the frame
            junk.assign(protocol::kHeaderBytes, std::byte{0});
            protocol::Header h;
            h.magic = protocol::kFrameMagic;
            h.protocol_ver = protocol::kProtocolVersion;
            h.fb_len = 0xFFFFFFFFU;
            protocol::EncodeHeader(h, std::span<std::byte, protocol::kHeaderBytes>{
                                          junk.data(), protocol::kHeaderBytes});
            break;
        }
        case 2: { // good header, garbage FlatBuffer
            junk.assign(protocol::kHeaderBytes + 64, std::byte{0xAB});
            protocol::Header h;
            h.magic = protocol::kFrameMagic;
            h.protocol_ver = protocol::kProtocolVersion;
            h.fb_len = 64;
            protocol::EncodeHeader(h, std::span<std::byte, protocol::kHeaderBytes>{
                                          junk.data(), protocol::kHeaderBytes});
            break;
        }
        case 3:   // wrong magic
            junk.assign(protocol::kHeaderBytes + 16, std::byte{0x11});
            break;
        case 4: { // reserved word non-zero — must be refused so it stays free
            junk.assign(protocol::kHeaderBytes + 16, std::byte{0});
            protocol::Header h;
            h.magic = protocol::kFrameMagic;
            h.protocol_ver = protocol::kProtocolVersion;
            h.fb_len = 16;
            h.reserved = 0xDEADBEEF;
            protocol::EncodeHeader(h, std::span<std::byte, protocol::kHeaderBytes>{
                                          junk.data(), protocol::kHeaderBytes});
            break;
        }
        default:  // empty frame
            junk.clear();
            break;
    }
    (void)transport_.Send(junk);
    ++stats_.malformed_sent;
}

}  // namespace p2pgpu::mock
