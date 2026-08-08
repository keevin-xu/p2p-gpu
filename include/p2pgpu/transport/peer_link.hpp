#pragma once
//
// One WebRTC peer connection and its data channel — step 6.4.
//
// PORTABLE, and this is the second load-bearing claim after `transport.cpp`.
// libdatachannel (native) and datachannel-wasm (browser) expose the same
// `rtc::PeerConnection` and `rtc::DataChannel` to both targets, so the data
// plane is written ONCE here. **The shared subset is measured, not assumed:
// D-0088 diffs both headers** — D-0032 diffed only `WebSocket` and said in
// terms that these two had to be checked the same way before Phase 6 relied on
// them.
//
// ── NEGOTIATION IS IMPLICIT, BECAUSE IT HAS TO BE ────────────────────────
// `setLocalDescription`, `createOffer` and `createAnswer` are NATIVE ONLY
// (D-0088), so portable code cannot drive negotiation explicitly. Both libraries
// auto-negotiate, and the shared surface is exactly what that needs:
//
//   offerer:  construct -> createDataChannel(label) -> onLocalDescription(offer)
//   answerer: construct -> setRemoteDescription(offer) -> onLocalDescription(answer)
//
// If you find yourself reaching for `createOffer`, that is the seam D-0008 exists
// to avoid — and only the wasm build will tell you, since it compiles natively.
//
// ── EVERY BYTE THAT ARRIVES HERE IS HOSTILE ──────────────────────────────
// A peer is not the coordinator, and the coordinator was never trusted either
// (R11). This class does no interpretation whatsoever: it hands the caller
// bytes. Framing, verification and asset validation happen above it, in the code
// that already does them for the control plane.

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace rtc {
class PeerConnection;
class DataChannel;
}  // namespace rtc

namespace p2pgpu::transport {

/// What the local end must send to the remote end through the coordinator's
/// signalling relay (6.1). Opaque to the relay; meaningful only to the peer.
struct SignalOut {
    /// "offer", "answer", or "candidate".
    std::string kind;
    /// SDP for a description; the candidate string for a candidate.
    std::string text;
    /// Media id, for candidates only. Empty otherwise.
    std::string mid;
};

/// One connection to one peer.
///
/// Deliberately NOT a manager of many: a link that knew about its siblings would
/// have to own the connection cap, and that cap is a scheduling decision
/// (6.2/6.12). One object, one peer, no policy.
class PeerLink {
public:
    using SignalHandler = std::function<void(const SignalOut&)>;
    using MessageHandler = std::function<void(std::span<const std::byte>)>;
    using StateHandler = std::function<void(bool open)>;

    /// `ice_servers` are STUN/TURN URLs (6.5). `iceServers` is the only field
    /// of `Configuration` present on both targets (D-0088).
    explicit PeerLink(std::vector<std::string> ice_servers = {});
    ~PeerLink();

    PeerLink(const PeerLink&) = delete;
    PeerLink& operator=(const PeerLink&) = delete;

    /// Signalling to hand to the coordinator's relay. Set BEFORE connecting:
    /// negotiation begins the moment `Offer()` or `AcceptOffer()` is called, and
    /// a description produced before the handler exists is lost.
    void OnSignal(SignalHandler h) { on_signal_ = std::move(h); }
    /// Bytes from the peer. Never interpreted here.
    void OnMessage(MessageHandler h) { on_message_ = std::move(h); }
    /// Data channel opened or closed.
    void OnOpen(StateHandler h) { on_state_ = std::move(h); }

    /// Become the OFFERER: create the data channel, which triggers the offer.
    void Offer(const std::string& label = "p2pgpu-asset");

    /// Become the ANSWERER: accept a remote offer. The answer arrives through
    /// `OnSignal`, and the data channel through `OnMessage` once the remote end
    /// opens it.
    void AcceptOffer(const std::string& sdp);

    /// Apply the answer to an offer we made.
    void AcceptAnswer(const std::string& sdp);

    /// Apply a remote ICE candidate.
    void AddRemoteCandidate(const std::string& candidate, const std::string& mid);

    /// Send bytes to the peer. False if the channel is not open — a caller that
    /// ignores this silently drops data.
    [[nodiscard]] bool Send(std::span<const std::byte> bytes);

    [[nodiscard]] bool IsOpen() const;

    /// ICE candidate types this end GATHERED (6.5). Portable, and weaker than
    /// it looks.
    ///
    /// `getSelectedCandidatePair()` — which answers "was this connection
    /// relayed?" exactly — is NATIVE ONLY (D-0088). What both targets expose is
    /// the candidate strings, and an ICE candidate line carries its own type.
    ///
    /// **Gathering a relay candidate means TURN was reachable and offered a
    /// path. It does NOT mean the connection used one**, because ICE prefers
    /// host and server-reflexive pairs and only falls back to relay. So this is
    /// a LOWER BOUND: no relay candidate gathered means definitely not relayed,
    /// and the definitive ratio 6.15 wants needs the native-only call from
    /// `worker-native` (D-0089).
    struct CandidateTypes {
        bool host = false;
        bool server_reflexive = false;   ///< STUN worked
        bool relay = false;              ///< TURN was reachable
    };
    [[nodiscard]] CandidateTypes GatheredTypes() const noexcept { return gathered_; }

    /// Tear down. Idempotent, and called by the destructor.
    ///
    /// Destroys rather than merely closes, for the D-0060 reason: `close()` does
    /// not guarantee an in-flight callback has returned, and every callback here
    /// captures `this`.
    void Shutdown();

private:
    void WireChannel(std::shared_ptr<rtc::DataChannel> channel);

    std::unique_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::DataChannel> channel_;
    SignalHandler on_signal_;
    MessageHandler on_message_;
    StateHandler on_state_;
    CandidateTypes gathered_{};
};

}  // namespace p2pgpu::transport
