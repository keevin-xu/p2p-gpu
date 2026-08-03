// Portable — and this is the load-bearing one.
//
// libdatachannel (native) and datachannel-wasm (browser) expose the SAME
// `rtc::WebSocket`, so the control plane is written once here for both targets.
// No platform seam. This property is why the single-language architecture works
// at all; if you find yourself adding an #ifdef here, re-read D-0008 first —
// and note that tools/check_seam.py fails the build anyway (R2).
//
// The shared subset is MEASURED, not assumed — D-0032 lists it. Everything used
// below is in that table. Data plane (WebRTC DataChannel) is Phase 6.

#include "p2pgpu/transport/transport.hpp"

#include <rtc/rtc.hpp>

#include <cstdio>
#include <string>
#include <utility>
#include <variant>

namespace p2pgpu::worker {

namespace {

/// Default sink. Used by mock-worker and anything else that has no opinion.
void LogToStderr(std::string_view level, std::string_view message) {
    std::fprintf(stderr, "[%.*s] %.*s\n", static_cast<int>(level.size()), level.data(),
                 static_cast<int>(message.size()), message.data());
}

}  // namespace

Transport::Transport(LogFn log)
    : ws_(std::make_unique<rtc::WebSocket>()),
      log_(log ? std::move(log) : LogFn{&LogToStderr}) {}

// Out of line, and it has to be: the header only forward-declares
// rtc::WebSocket, so unique_ptr's deleter needs the complete type here.
Transport::~Transport() = default;

void Transport::OnOpen(std::function<void()> handler) { on_open_ = std::move(handler); }
void Transport::OnClosed(std::function<void()> handler) { on_closed_ = std::move(handler); }
void Transport::OnError(std::function<void(const std::string&)> handler) {
    on_error_ = std::move(handler);
}
void Transport::OnMessage(std::function<void(std::span<const std::byte>)> handler) {
    on_message_ = std::move(handler);
}

void Transport::Connect(const std::string& url) {
    ws_->onOpen([this] {
        log_("info", "transport open");
        if (on_open_) {
            on_open_();
        }
    });

    ws_->onClosed([this] {
        log_("info", "transport closed");
        if (on_closed_) {
            on_closed_();
        }
    });

    ws_->onError([this](std::string error) {
        log_("warn", "transport error: " + error);
        if (on_error_) {
            on_error_(error);
        }
    });

    ws_->onMessage([this](rtc::message_variant data) {
        // BINARY ONLY. `message_variant` is variant<binary, string> on both
        // targets; a text frame is either a confused peer or a probe, and
        // either way it is not our protocol. Dropping it here means the frame
        // parser never sees a shape it was not built for.
        //
        // std::get_if rather than std::visit: an overloaded-lambda visitor
        // would need a text arm that does nothing, which reads like an
        // oversight rather than a decision.
        if (const auto* bin = std::get_if<rtc::binary>(&data)) {
            if (on_message_) {
                // rtc::binary IS std::vector<std::byte> on both targets
                // (D-0032), so this is a span over it, not a conversion.
                on_message_(std::span<const std::byte>{bin->data(), bin->size()});
            }
        } else {
            log_("warn", "transport dropped a non-binary frame");
        }
    });

    ws_->open(url);
}

bool Transport::Send(std::span<const std::byte> frame) {
    if (!ws_->isOpen()) {
        return false;
    }
    // send(const byte*, size_t) takes std::byte on BOTH targets, so our frames
    // reach the wire with no cast at all (D-0032) — worth noting given R11 bans
    // the usual spellings anywhere near network bytes.
    return ws_->send(frame.data(), frame.size());
}

void Transport::Close() {
    if (!ws_->isClosed()) {
        ws_->close();
    }
}

bool Transport::IsOpen() const { return ws_->isOpen(); }

}  // namespace p2pgpu::worker
