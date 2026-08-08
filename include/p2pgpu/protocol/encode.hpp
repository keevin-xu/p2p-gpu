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
#include <span>
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
