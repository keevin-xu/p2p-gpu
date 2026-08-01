#pragma once
//
// Control-plane transport — step 1.18. PORTABLE, and this is the load-bearing
// one.
//
// libdatachannel (native) and datachannel-wasm (browser) expose the same
// `rtc::WebSocket` to both targets, so this is written once for both with NO
// platform seam. That property is the whole reason a single-language stack is
// possible here (D-0008) — if transport needed a seam, the argument for C++
// everywhere would be materially weaker.
//
// **The shared subset is measured, not assumed: D-0032 lists it.** Some of
// libdatachannel's API is native-only (`maxMessageSize`, `onAvailable`,
// `resetCallbacks`, the `Configuration` constructor). Using one of those here
// compiles natively and breaks the browser build, and only the wasm build will
// tell you. Stay inside the table in D-0032.
//
// If you find yourself adding an `#ifdef` to transport.cpp, re-read D-0008
// first — and note that tools/check_seam.py will fail the build anyway (R2).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace rtc {
class WebSocket;
}

namespace p2pgpu::worker {

/// WebSocket client to the coordinator. Owns the socket and the callbacks.
///
/// Deliberately knows nothing about the protocol: it moves framed byte blobs.
/// Verification, routing, and every decision belong above it — the worker makes
/// no decisions at all (R1), and this class makes fewer.
class Transport {
public:
    Transport();
    ~Transport();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    /// Fires when the socket is open and messages may be sent.
    void OnOpen(std::function<void()> handler);
    /// Fires on close, whether ours or the peer's. Also fires after an error.
    void OnClosed(std::function<void()> handler);
    /// One complete binary message. A text frame is not our protocol and is
    /// dropped before reaching this.
    void OnMessage(std::function<void(std::span<const std::byte>)> handler);
    void OnError(std::function<void(const std::string&)> handler);

    /// Connect to `url` (`ws://host:port/ws`). Returns immediately — connection
    /// completes asynchronously and OnOpen reports it.
    ///
    /// Both targets are asynchronous here and neither may block: the browser
    /// cannot block at all, and blocking natively would only hide that.
    void Connect(const std::string& url);

    /// Send one framed message. False if the socket is not open or the library
    /// refused it.
    ///
    /// Takes `std::span<const std::byte>` because that is what `EncodeFrame`
    /// produces AND what `rtc::WebSocket::send` accepts on both targets — the
    /// one place the two libraries could have forced a conversion, they do not
    /// (D-0032).
    [[nodiscard]] bool Send(std::span<const std::byte> frame);

    void Close();

    [[nodiscard]] bool IsOpen() const;

private:
    std::unique_ptr<rtc::WebSocket> ws_;

    // Held here rather than only inside the library so they outlive any
    // internal callback bookkeeping and so Close() can be called from within a
    // handler without destroying what is currently running.
    std::function<void()> on_open_;
    std::function<void()> on_closed_;
    std::function<void(std::span<const std::byte>)> on_message_;
    std::function<void(const std::string&)> on_error_;
};

}  // namespace p2pgpu::worker
