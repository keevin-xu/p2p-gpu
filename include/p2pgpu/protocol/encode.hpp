#pragma once
//
// Frame encoder — the inverse of verify.hpp.
//
// Lives in p2pgpu-protocol so both the coordinator and the worker build frames
// the same way, and so the fuzz corpus generator can produce seeds with the
// REAL encoder (step 1.5) rather than hand-assembled bytes that might be subtly
// wrong and never reach the Verifier.
//
// Header-only dependencies: flatbuffers' builder is header-only, so this does
// not break D-0015's constraint on the fuzz build.

#include <cstddef>
#include <cstdint>
#include <array>
#include <span>
#include <string>
#include <vector>

#include "p2pgpu/p2pgpu_generated.h"
#include "p2pgpu/protocol/frame.hpp"

namespace p2pgpu::protocol {

/// Wrap a finished FlatBuffers Envelope in the 16-byte frame header, optionally
/// with a trailing payload.
///
/// The header is 16 bytes so the Envelope lands 8-byte aligned in a receiver
/// whose buffer is itself aligned (D-0027) — the encoder's half of that
/// contract is simply using kHeaderBytes rather than assuming 12.
[[nodiscard]] std::vector<std::byte> EncodeFrame(
    std::span<const std::byte> envelope, std::span<const std::byte> payload = {});

/// Build `Envelope{body}` and frame it in one step.
///
/// `build` receives the FlatBufferBuilder and returns the body offset; this
/// wraps it in the Envelope union so callers cannot forget to set body_type,
/// which would produce a frame VerifyFrame rejects as "envelope has no body".
template <typename BuildFn>
[[nodiscard]] std::vector<std::byte> EncodeMessage(
    wire::Body type, BuildFn&& build, std::span<const std::byte> payload = {}) {
    flatbuffers::FlatBufferBuilder fbb;
    const auto body = build(fbb);
    wire::EnvelopeBuilder eb(fbb);
    eb.add_body_type(type);
    eb.add_body(body.Union());
    fbb.Finish(eb.Finish());

    const auto* p = static_cast<const std::byte*>(
        static_cast<const void*>(fbb.GetBufferPointer()));
    return EncodeFrame({p, fbb.GetSize()}, payload);
}

/// Build `AssetMsg{body}` for the PEER data channel (6.6).
///
/// ── WHY A DIFFERENT ROOT, AND NOT `Envelope` ─────────────────────────────
/// The same three tables are members of `Body` too (D-0077), so the control
/// link could carry them and does. Using `Envelope` here as well would reuse
/// more code — and would mean a peer could express EVERY control message.
/// A `TaskGrant` arriving over a data channel would then have to be rejected by
/// a check someone remembered to write.
///
/// With `AssetMsg` as the root, **a peer cannot form a control message at all**.
/// The vocabulary is restricted by the type system rather than by a switch arm,
/// which is the same reasoning that makes `VerifiedFrame` a type rather than a
/// convention (R11).
///
/// ── NO FRAME HEADER ──────────────────────────────────────────────────────
/// A DataChannel is message-oriented, like a WebSocket, so one message is one
/// buffer and there is nothing to delimit. `VerifyAssetMsg` bounds the size
/// itself. The 16-byte control-plane header exists to carry magic, version and
/// length AND to guarantee 8-byte alignment for the Envelope (D-0027) — here
/// the receiver checks alignment directly, because what arrives is a
/// `std::vector<std::byte>` whose data is already over-aligned rather than a
/// view into someone else's buffer.
template <typename BuildFn>
[[nodiscard]] std::vector<std::byte> EncodeAssetMsg(wire::AssetBody type,
                                                    BuildFn&& build) {
    flatbuffers::FlatBufferBuilder fbb;
    const auto body = build(fbb);
    wire::AssetMsgBuilder mb(fbb);
    mb.add_body_type(type);
    mb.add_body(body.Union());
    fbb.Finish(mb.Finish());

    const auto* p = static_cast<const std::byte*>(
        static_cast<const void*>(fbb.GetBufferPointer()));
    return std::vector<std::byte>(p, p + fbb.GetSize());
}

}  // namespace p2pgpu::protocol

namespace p2pgpu::protocol {

/// The inner signalling message carried in `Signal.payload` (6.8).
///
/// A standalone root, like `AssetMsg`: the coordinator relays these bytes
/// without reading them (D-0085), and the receiving worker verifies the
/// structure with `VerifyPeerSignal`.
[[nodiscard]] inline std::vector<std::byte> EncodePeerSignal(
    const std::string& kind, const std::string& text, const std::string& mid) {
    flatbuffers::FlatBufferBuilder fbb;
    auto k = fbb.CreateString(kind);
    auto t = fbb.CreateString(text);
    auto m = fbb.CreateString(mid);
    wire::PeerSignalBuilder b(fbb);
    b.add_kind(k);
    b.add_text(t);
    b.add_mid(m);
    fbb.Finish(b.Finish());
    const auto* p = static_cast<const std::byte*>(
        static_cast<const void*>(fbb.GetBufferPointer()));
    return std::vector<std::byte>(p, p + fbb.GetSize());
}

/// 64-char lowercase hex -> `Hash32`'s four little-endian u64 lanes.
///
/// The inverse conversion appears in three places now (grant, asset request,
/// peer request) and is spelled out rather than memcpy-ing a struct, because
/// the byte order is the wire's business and "it agrees on arm64" says nothing
/// (D-0027).
[[nodiscard]] inline wire::Hash32 HashFromHex(const std::string& hex) {
    std::array<std::uint64_t, 4> lanes{};
    if (hex.size() == 64) {
        for (std::size_t lane = 0; lane < 4; ++lane) {
            std::uint64_t v = 0;
            for (std::size_t byte = 0; byte < 8; ++byte) {
                const auto nib = [](char c) -> std::uint64_t {
                    if (c >= '0' && c <= '9') { return static_cast<std::uint64_t>(c - '0'); }
                    if (c >= 'a' && c <= 'f') { return static_cast<std::uint64_t>(c - 'a' + 10); }
                    return 0;
                };
                const std::size_t at = lane * 16 + byte * 2;
                v |= ((nib(hex[at]) << 4) | nib(hex[at + 1])) << (byte * 8);
            }
            lanes[lane] = v;
        }
    }
    return wire::Hash32(lanes[0], lanes[1], lanes[2], lanes[3]);
}

}  // namespace p2pgpu::protocol
