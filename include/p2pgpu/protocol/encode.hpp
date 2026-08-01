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

}  // namespace p2pgpu::protocol
