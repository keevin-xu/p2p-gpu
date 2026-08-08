// Signalling relay — step 6.1, D-0085.
//
// The relay reaches a DIFFERENT connection than the one that sent the frame, so
// `Session` takes it as an injected callback rather than through `Reaction`
// (D-0085). That is what lets this file test routing with no socket anywhere:
// the stub below captures what would have been sent and to whom.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include <filesystem>
#include <memory>

#include "p2pgpu/coordinator/fleet.hpp"
#include "p2pgpu/coordinator/kernel_registry.hpp"
#include "p2pgpu/coordinator/session.hpp"
#include "p2pgpu/protocol/encode.hpp"
#include "p2pgpu/protocol/verify.hpp"

using namespace p2pgpu::coordinator;
using p2pgpu::protocol::WorkerId;
namespace wire = p2pgpu::wire;

namespace {

/// The real manifest, same reasoning as test_session.cpp: a fake registry would
/// exercise a path production never takes.
const KernelRegistry& Registry() {
    static const auto reg = KernelRegistry::Load(
        std::filesystem::path(P2PGPU_KERNEL_DIR) / "manifest.toml",
        std::filesystem::path(P2PGPU_KERNEL_DIR));
    REQUIRE(reg);
    return *reg;
}

std::vector<std::byte> HelloFrame(std::uint32_t version) {
    return p2pgpu::protocol::EncodeMessage(
        wire::Body::Hello, [&](flatbuffers::FlatBufferBuilder& fbb) {
            wire::HelloBuilder b(fbb);
            b.add_protocol_version(version);
            return b.Finish();
        });
}

Reaction Feed(Session& s, std::span<const std::byte> frame,
              std::uint64_t now_ms = 1000) {
    const auto verified = p2pgpu::protocol::VerifyFrame(frame);
    REQUIRE(verified);
    return s.OnMessage(*verified, now_ms);
}

struct Relayed {
    WorkerId to;
    std::vector<std::byte> frame;
};

std::vector<std::byte> SignalFrame(WorkerId peer, std::size_t payload_bytes,
                                   unsigned char fill = 0xAB) {
    return p2pgpu::protocol::EncodeMessage(
        wire::Body::Signal, [&](flatbuffers::FlatBufferBuilder& fbb) {
            const std::vector<std::uint8_t> bytes(payload_bytes, fill);
            auto pv = fbb.CreateVector(bytes);
            const wire::Uuid id = peer.to_wire();
            wire::SignalBuilder b(fbb);
            b.add_peer(&id);
            b.add_payload(pv);
            return b.Finish();
        });
}

/// Decode a relayed frame back to (peer, payload) so the test asserts on what a
/// worker would actually receive, not on internal state.
struct Decoded {
    WorkerId peer;
    std::vector<std::byte> payload;
};
std::optional<Decoded> DecodeSignal(std::span<const std::byte> frame) {
    auto verified = p2pgpu::protocol::VerifyFrame(frame);
    if (!verified) {
        return std::nullopt;
    }
    const auto* env = verified->envelope();
    const auto* sig = env != nullptr ? env->body_as_Signal() : nullptr;
    if (sig == nullptr || sig->peer() == nullptr || sig->payload() == nullptr) {
        return std::nullopt;
    }
    Decoded d;
    d.peer = WorkerId{*sig->peer()};
    const auto* p = sig->payload();
    d.payload.assign(reinterpret_cast<const std::byte*>(p->data()),
                     reinterpret_cast<const std::byte*>(p->data()) + p->size());
    return d;
}

/// A handshaked session with the peer relay stubbed.
///
/// The stub is the whole reason this file needs no socket: `Session` takes
/// cross-connection delivery as an injected callback (D-0085), so the test
/// captures what would have gone out and to whom.
struct SignalFixture {
    JobManager jobs;
    Fleet fleet;
    const KernelRegistry& kernels = Registry();
    std::unique_ptr<Session> session =
        std::make_unique<Session>(jobs, kernels, fleet, /*conn_id=*/7,
                                  /*lease_ms=*/30000);

    std::vector<Relayed> relayed;
    /// Whether the destination has a live connection. False models a peer that
    /// has departed, which must be a SILENT drop.
    bool deliverable = true;

    WorkerId self;
    WorkerId other{99, 99};

    SignalFixture() {
        session->SetPeerRelay([this](WorkerId to, std::span<const std::byte> f) {
            if (!deliverable) {
                return false;
            }
            relayed.push_back(Relayed{to, {f.begin(), f.end()}});
            return true;
        });
        const auto hello = HelloFrame(p2pgpu::protocol::kProtocolVersion);
        const auto r = Feed(*session, hello);
        REQUIRE_FALSE(r.close);
        REQUIRE(session->handshaked());
        self = session->worker_id();
    }

    Reaction Deliver(const std::vector<std::byte>& frame) {
        return Feed(*session, frame);
    }
};

}  // namespace

TEST_CASE("a signal is relayed to its destination with peer rewritten to the sender",
          "[signal]") {
    // `peer` is DESTINATION inbound and SOURCE outbound — one symmetric table
    // (G1). Getting this backwards would have both peers signalling themselves,
    // and every WebRTC negotiation would silently never complete.
    SignalFixture fx;
    const auto frame = SignalFrame(fx.other, 128);
    const auto r = fx.Deliver(frame);
    CHECK_FALSE(r.close);

    REQUIRE(fx.relayed.size() == 1);
    CHECK(fx.relayed[0].to == fx.other);
    const auto decoded = DecodeSignal(fx.relayed[0].frame);
    REQUIRE(decoded.has_value());
    CHECK(decoded->peer == fx.self);          // the SENDER, not the destination
    CHECK(decoded->payload.size() == 128);
}

TEST_CASE("the payload is relayed byte-for-byte and never interpreted",
          "[signal]") {
    // The coordinator brokers and does not join the media path. Bytes that are
    // not valid SDP must pass through unchanged — a relay that validated them
    // would be an SDP parser in the one process the whole fleet depends on.
    SignalFixture fx;
    const auto r = fx.Deliver(SignalFrame(fx.other, 64, 0xFF));
    REQUIRE(fx.relayed.size() == 1);
    const auto decoded = DecodeSignal(fx.relayed[0].frame);
    REQUIRE(decoded.has_value());
    for (const std::byte b : decoded->payload) {
        CHECK(b == std::byte{0xFF});
    }
}

TEST_CASE("an oversized payload is rejected without being relayed", "[signal]") {
    SignalFixture fx;
    const auto r = fx.Deliver(
        SignalFrame(fx.other, p2pgpu::protocol::kMaxSignalBytes + 1));
    CHECK(fx.relayed.empty());
    CHECK_FALSE(r.replies.empty());   // the sender IS told; it is their frame
}

TEST_CASE("an unknown peer is dropped SILENTLY", "[signal]") {
    // No error frame: that would be an enumeration oracle telling an attacker
    // which worker ids exist. WebRTC already handles a peer that never answers.
    SignalFixture fx;
    fx.deliverable = false;           // relay reports "no live connection"
    const auto r = fx.Deliver(SignalFrame(fx.other, 32));
    CHECK(r.replies.empty());
    CHECK_FALSE(r.close);
}

TEST_CASE("a worker cannot signal itself", "[signal]") {
    // Relaying it would make the coordinator echo arbitrary bytes back for free.
    SignalFixture fx;
    const auto r = fx.Deliver(SignalFrame(fx.self, 32));
    CHECK(fx.relayed.empty());
    CHECK_FALSE(r.replies.empty());
}

TEST_CASE("signalling is rate limited per connection and the window resets",
          "[signal]") {
    // Chatty by nature, so the cap is generous — but bounded, or one worker can
    // use the relay to flood another.
    SignalFixture fx;
    std::size_t relayed_before_limit = 0;
    for (int i = 0; i < 200; ++i) {
        const auto r = fx.Deliver(SignalFrame(fx.other, 16));
        if (!r.replies.empty()) {
            break;   // first rejection
        }
        ++relayed_before_limit;
    }
    CHECK(relayed_before_limit == 64);
    CHECK(fx.relayed.size() == 64);

    // The sweep clears it, or a long-lived connection could never negotiate
    // again after one busy second.
    fx.session->ResetSignalWindow();
    const auto after = fx.Deliver(SignalFrame(fx.other, 16));
    CHECK(after.replies.empty());
    CHECK(fx.relayed.size() == 65);
}

// ─────────────────────────────────────────────────────────────────────────
// 6.2 — peer list distribution (D-0086)
// ─────────────────────────────────────────────────────────────────────────

namespace {

AssetId Asset(std::uint8_t fill) {
    AssetId a{};
    a.fill(static_cast<std::byte>(fill));
    return a;
}

WorkerId W(std::uint64_t n) { return WorkerId{n, n}; }

}  // namespace

TEST_CASE("PeersHolding returns only holders, excludes self, and is sorted",
          "[peers]") {
    Fleet fleet;
    for (std::uint64_t i = 1; i <= 6; ++i) {
        fleet.Join(W(i), i, 1000);
    }
    // 5, 2 and 4 hold it; 3 holds something else; 1 holds nothing.
    for (const std::uint64_t i : {5ULL, 2ULL, 4ULL}) {
        fleet.Mutable(W(i))->cached_assets = {Asset(0xAA)};
    }
    fleet.Mutable(W(3))->cached_assets = {Asset(0xBB)};

    const auto peers = fleet.PeersHolding(Asset(0xAA), W(4), 8);
    // 4 is excluded as self; 1 and 3 do not hold it; result is SORTED, which
    // is what makes 6.13's runs comparable rather than a fairness nicety.
    CHECK(peers == std::vector<WorkerId>{W(2), W(5)});
}

TEST_CASE("the peer list is CAPPED, and the cap takes a deterministic prefix",
          "[peers]") {
    // A full mesh at N=50 is 1225 connections. Truncating an unordered set
    // would also make identical runs return different peers.
    Fleet fleet;
    for (std::uint64_t i = 1; i <= 40; ++i) {
        fleet.Join(W(i), i, 1000);
        fleet.Mutable(W(i))->cached_assets = {Asset(0xAA)};
    }
    const auto a = fleet.PeersHolding(Asset(0xAA), W(99), 8);
    const auto b = fleet.PeersHolding(Asset(0xAA), W(99), 8);
    REQUIRE(a.size() == 8);
    CHECK(a == b);
    CHECK(a.front() == W(1));
    CHECK(a.back() == W(8));
}

TEST_CASE("no holders means an empty list, not every worker", "[peers]") {
    // The failure this guards: a filter that falls through to "send everyone"
    // when nothing matches would build the mesh 6.2 exists to avoid, and would
    // do it precisely when the asset is rarest.
    Fleet fleet;
    for (std::uint64_t i = 1; i <= 5; ++i) {
        fleet.Join(W(i), i, 1000);
    }
    CHECK(fleet.PeersHolding(Asset(0xAA), W(99), 8).empty());
}

TEST_CASE("max of zero returns nothing rather than everything", "[peers]") {
    Fleet fleet;
    fleet.Join(W(1), 1, 1000);
    fleet.Mutable(W(1))->cached_assets = {Asset(0xAA)};
    CHECK(fleet.PeersHolding(Asset(0xAA), W(99), 0).empty());
}
