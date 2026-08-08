// Two PeerLinks connecting to each other — step 6.4.
//
// A REAL WebRTC negotiation over loopback: two peer connections, host
// candidates, a real SCTP data channel. The signalling relay is stubbed by
// handing each link's output straight to the other, which is exactly what the
// coordinator does at 6.1 minus the socket.
//
// Tagged [net] so it can be excluded where there is no usable local network
// stack. It is NOT excluded by default: a data plane that has never opened a
// connection is not evidence of anything, and this is the cheapest place to
// find out that it does not.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "p2pgpu/transport/peer_link.hpp"

using namespace p2pgpu::transport;

namespace {

/// Poll until `done` or the deadline. libdatachannel runs its own threads
/// natively, so callbacks land off this one — there is nothing to pump, only to
/// wait for.
bool WaitFor(const std::function<bool()>& done,
             std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return done();
}

}  // namespace

TEST_CASE("two peers negotiate and exchange bytes over a data channel",
          "[peer][net]") {
    // No ICE servers: host candidates over loopback are enough, and depending on
    // a public STUN server would make this a network-availability check rather
    // than a correctness one. 6.5 is where STUN/TURN gets measured.
    PeerLink a;
    PeerLink b;

    std::atomic<bool> a_open{false};
    std::atomic<bool> b_open{false};
    std::vector<std::byte> received;
    std::atomic<bool> got_message{false};

    // The stub relay. Each side's signalling goes straight to the other, which
    // is what the coordinator's 6.1 relay does minus the socket — and it is why
    // this test can exist without one.
    a.OnSignal([&](const SignalOut& s) {
        if (s.kind == "offer") {
            b.AcceptOffer(s.text);
        } else if (s.kind == "candidate") {
            b.AddRemoteCandidate(s.text, s.mid);
        }
    });
    b.OnSignal([&](const SignalOut& s) {
        if (s.kind == "answer") {
            a.AcceptAnswer(s.text);
        } else if (s.kind == "candidate") {
            a.AddRemoteCandidate(s.text, s.mid);
        }
    });

    a.OnOpen([&](bool open) { a_open = open; });
    b.OnOpen([&](bool open) { b_open = open; });
    b.OnMessage([&](std::span<const std::byte> bytes) {
        received.assign(bytes.begin(), bytes.end());
        got_message = true;
    });

    // Handlers first, THEN offer: negotiation starts the moment the channel is
    // created, and a description produced before the handler exists is lost.
    a.Offer();

    REQUIRE(WaitFor([&] { return a_open.load() && b_open.load(); }));
    CHECK(a.IsOpen());
    CHECK(b.IsOpen());

    // Non-ASCII on purpose: the channel must be binary end to end. A path that
    // quietly went through a text conversion would mangle these.
    const std::vector<std::byte> payload{
        std::byte{0x00}, std::byte{0xFF}, std::byte{0x7F},
        std::byte{0x80}, std::byte{0x0A}, std::byte{0x0D}};
    CHECK(a.Send(payload));

    REQUIRE(WaitFor([&] { return got_message.load(); }));
    CHECK(received == payload);
}

TEST_CASE("sending before the channel opens fails rather than dropping silently",
          "[peer][net]") {
    // A caller that ignores the return value drops data. Returning void here
    // would make that impossible to notice.
    PeerLink a;
    CHECK_FALSE(a.IsOpen());
    const std::vector<std::byte> bytes{std::byte{1}, std::byte{2}};
    CHECK_FALSE(a.Send(bytes));
}

TEST_CASE("shutdown is idempotent and safe with callbacks registered",
          "[peer][net]") {
    // Every callback captures `this`. D-0060 found the control transport tearing
    // down a queue while its socket was still live; the same shape would be
    // worse here, because a data channel callback can arrive from an ICE thread
    // at any moment.
    PeerLink a;
    a.OnMessage([](std::span<const std::byte>) {});
    a.OnOpen([](bool) {});
    a.Offer();
    a.Shutdown();
    a.Shutdown();
    CHECK_FALSE(a.IsOpen());
}
