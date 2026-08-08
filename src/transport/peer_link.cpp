// One WebRTC peer connection and its data channel — step 6.4.
//
// PORTABLE. Every API used here is in the BOTH column of D-0088, which diffed
// libdatachannel against datachannel-wasm before this file was written. Nothing
// from the native-only column may appear, and only the wasm build would tell you
// if it did.

#include "p2pgpu/transport/peer_link.hpp"

#include <rtc/rtc.hpp>

#include <string>
#include <utility>
#include <variant>

namespace p2pgpu::transport {

PeerLink::PeerLink(std::vector<std::string> ice_servers) {
    rtc::Configuration config;
    // `iceServers` is the ONLY field of Configuration present on both targets
    // (D-0088). Port ranges, MTU and the `disable*` flags are native-only, so
    // portable code sets this and nothing else — which is all 6.5 needs.
    for (const std::string& url : ice_servers) {
        config.iceServers.emplace_back(url);
    }
    pc_ = std::make_unique<rtc::PeerConnection>(config);

    // Local description: the offer or the answer, depending on which side we
    // are. Both arrive through the same callback because negotiation is
    // implicit — `setLocalDescription` is native-only (D-0088), so there is no
    // portable way to drive it and no need for one.
    pc_->onLocalDescription([this](rtc::Description desc) {
        if (on_signal_) {
            on_signal_(SignalOut{desc.typeString(), std::string(desc), ""});
        }
    });

    pc_->onLocalCandidate([this](rtc::Candidate candidate) {
        const std::string text = candidate.candidate();
        // An ICE candidate line names its own type. Parsed from the STRING
        // rather than asked of the connection, because the API that answers it
        // properly is native-only (D-0088/D-0089) — and this is a lower bound,
        // not the selected pair.
        if (text.find("typ relay") != std::string::npos) {
            gathered_.relay = true;
        } else if (text.find("typ srflx") != std::string::npos) {
            gathered_.server_reflexive = true;
        } else if (text.find("typ host") != std::string::npos) {
            gathered_.host = true;
        }
        if (on_signal_) {
            on_signal_(SignalOut{"candidate", text, candidate.mid()});
        }
    });

    // The ANSWERER receives the channel rather than creating it.
    pc_->onDataChannel([this](std::shared_ptr<rtc::DataChannel> channel) {
        WireChannel(std::move(channel));
    });
}

PeerLink::~PeerLink() { Shutdown(); }

void PeerLink::WireChannel(std::shared_ptr<rtc::DataChannel> channel) {
    channel_ = std::move(channel);

    channel_->onOpen([this] {
        if (on_state_) {
            on_state_(true);
        }
    });
    channel_->onClosed([this] {
        if (on_state_) {
            on_state_(false);
        }
    });
    channel_->onMessage([this](rtc::message_variant data) {
        if (!on_message_) {
            return;
        }
        // BINARY ONLY. A peer sending text is either confused or probing; either
        // way nothing above this expects it, and quietly converting would hand
        // the asset path bytes it never agreed to accept (R11).
        //
        // `rtc::binary` IS `std::vector<std::byte>` on both targets (D-0032), so
        // this reaches the caller with no cast.
        if (const auto* bin = std::get_if<rtc::binary>(&data)) {
            on_message_(std::span<const std::byte>(*bin));
        }
    });
}

void PeerLink::Offer(const std::string& label) {
    if (!pc_) {
        return;
    }
    // Creating the channel is what triggers the offer. There is no explicit
    // `createOffer` in the portable subset, and none is wanted (D-0088).
    WireChannel(pc_->createDataChannel(label));
}

void PeerLink::AcceptOffer(const std::string& sdp) {
    if (!pc_) {
        return;
    }
    // Setting the remote offer is what triggers the answer. The data channel
    // arrives later, through `onDataChannel`.
    pc_->setRemoteDescription(rtc::Description(sdp, "offer"));
}

void PeerLink::AcceptAnswer(const std::string& sdp) {
    if (!pc_) {
        return;
    }
    pc_->setRemoteDescription(rtc::Description(sdp, "answer"));
}

void PeerLink::AddRemoteCandidate(const std::string& candidate,
                                  const std::string& mid) {
    if (!pc_) {
        return;
    }
    pc_->addRemoteCandidate(rtc::Candidate(candidate, mid));
}

bool PeerLink::Send(std::span<const std::byte> bytes) {
    if (!channel_ || !channel_->isOpen()) {
        return false;
    }
    // `send(const byte*, size_t)` takes std::byte on both targets, so frames
    // reach the wire with no cast — the same property D-0032 found for the
    // control plane.
    //
    // NOT chunked here. `maxMessageSize()` is native-only (D-0088), so a
    // portable implementation cannot ask how much it may send; `kChunkBytes`
    // is a fixed 16 KiB decided above this layer, which is what invariant 10
    // already assumes.
    return channel_->send(bytes.data(), bytes.size());
}

bool PeerLink::IsOpen() const { return channel_ && channel_->isOpen(); }

void PeerLink::Shutdown() {
    // DESTROY, do not merely close. `close()` does not guarantee an in-flight
    // callback has returned, and every callback above captures `this` — that is
    // exactly the race D-0060 found in the control transport, where reverse
    // declaration order tore down a queue while the socket was still live.
    channel_.reset();
    pc_.reset();
}

}  // namespace p2pgpu::transport
