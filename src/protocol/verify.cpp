// The trust boundary.
//
// This file is the ONLY sanctioned path from network bytes to typed data
// (rule R11). Everything else in the codebase calls into here and nothing
// else parses a frame itself.
//
// The coordinator's entire input surface is attacker-controlled: anyone can
// connect as a worker, that is the product. Three concrete bug shapes live in
// this file's problem space, all with attacker-supplied lengths:
//
//   1. Declared length > frame size      → heap overread (Heartbleed's shape),
//                                          leaks adjacent memory
//   2. Declared length > destination     → heap overflow → potential RCE
//   3. index * kChunkBytes overflows u32 → writes land at a wrong offset
//
// Constraints in this translation unit (docs/CONVENTIONS.md §2):
//   - no raw pointer arithmetic, memcpy, C arrays, or reinterpret_cast
//   - std::span / std::string_view only
//   - every length checked against its kMax* constant BEFORE allocating
//   - checked arithmetic on every attacker-controlled index
//   - never assert() on input — assertions compile out; use a real check
//
// Implemented in Phase 1, steps 1.2–1.4. Fuzzed in step 1.5.

#include "p2pgpu/protocol/verify.hpp"

#include <type_traits>

namespace p2pgpu::protocol {

// ── Step 0.5 / 1.1 build checks ─────────────────────────────────────────
// These prove the generated header is genuinely usable, not merely produced:
// they fail at compile time if flatc did not run, wrote somewhere unexpected,
// or emitted a shape we cannot consume.
//
// Checking the VERIFIER specifically is the point. It is the single sanctioned
// bytes->fields path (R11), so a build where the type exists but the verifier
// is missing would be useless — and would not be noticed until runtime.
static_assert(sizeof(wire::Envelope) > 0, "flatc did not generate the root table");

// The verifier is a template (VerifierTemplate<B>), not a plain function, so
// this checks invocability with the concrete flatbuffers::Verifier rather than
// matching an exact signature — which would break on a flatbuffers upgrade for
// no good reason.
static_assert(std::is_invocable_r_v<bool, decltype(wire::VerifyEnvelopeBuffer<false>),
                                    ::flatbuffers::Verifier&>,
              "generated verifier is missing or not callable with a Verifier");

// Structs are fixed-layout and inline, so their sizes are part of the wire
// format. If flatc ever lays these out differently the frame decoder's
// assumptions break silently, which is the expensive kind of protocol bug.
static_assert(sizeof(wire::Uuid) == 16, "Uuid must be two u64");
static_assert(sizeof(wire::Hash32) == 32, "Hash32 must be four u64");

// The Body union is wire-numbered by declaration order (NONE = 0). Pinning the
// first and last members catches an accidental reorder at COMPILE time rather
// than as mysterious cross-version misrouting. Appending a member moves `Error`
// and will trip this deliberately — when that happens, confirm the change was
// an append, then update the expected value.
static_assert(static_cast<int>(wire::Body::NONE) == 0);
static_assert(static_cast<int>(wire::Body::Hello) == 1);
static_assert(static_cast<int>(wire::Body::Error) == 17,
              "Body union member order changed — this is a WIRE BREAK unless "
              "the change was a pure append (see p2pgpu.fbs)");

namespace {

/// THE SINGLE SANCTIONED byte* → uint8_t* CONVERSION IN THE PROJECT.
///
/// flatbuffers::Verifier's constructor is `(const uint8_t*, size_t)`. There is
/// no span overload, so handing it a `std::span<const std::byte>` requires one
/// conversion somewhere. R11 bans `reinterpret_cast` in boundary code, so this
/// deliberately does NOT use one: a `static_cast` through `const void*` is
/// well-defined for byte-like types, whereas `reinterpret_cast` is the spelling
/// that also permits genuine type punning. Same machine code, different set of
/// things it lets you say.
///
/// Note what is NOT happening here. R11 exists to stop us reinterpreting
/// network bytes as our own structs and reading fields out of them. This does
/// the opposite: it hands an already-bounds-checked region to the library whose
/// entire job is to decide whether those bytes are safe to read. The bounds
/// come from the span and are passed alongside; nothing downstream sees a raw
/// pointer.
///
/// Keep this the only such conversion. If a second one is ever wanted, that is
/// the moment to stop and ask (WORKFLOW.md §3), not to copy this comment.
[[nodiscard]] const std::uint8_t* AsVerifierBytes(std::span<const std::byte> s) noexcept {
    return static_cast<const std::uint8_t*>(static_cast<const void*>(s.data()));
}

}  // namespace

Result<VerifiedFrame> VerifyFrame(std::span<const std::byte> frame) noexcept {
    // Steps 1-6 of docs/PROTOCOL.md §1: size, header, magic, version, fb_len
    // bound, and region split. Nothing below this line has read a schema field.
    const Result<FrameRegions> regions = SplitFrame(frame);
    if (!regions) {
        return regions.error();
    }

    // 7. THE VERIFIER.
    //
    // kMaxVerifyDepth and kMaxVerifyTables are not decoration: an attacker can
    // otherwise hand us a deeply nested buffer and exhaust the stack during
    // verification. FlatBuffers' own defence against nesting-based resource
    // exhaustion is exactly these two bounds, and they only work if we pass them.
    ::flatbuffers::Verifier verifier(AsVerifierBytes(regions->fb), regions->fb.size(),
                                     kMaxVerifyDepth, kMaxVerifyTables);

    if (!wire::VerifyEnvelopeBuffer(verifier)) {
        return MakeError(ErrorCode::MalformedMessage, "flatbuffers verification failed");
    }

    // 8. ONLY NOW may fields be read.
    const wire::Envelope* env = wire::GetEnvelope(regions->fb.data());
    if (env == nullptr) {
        // Defensive: a verified buffer should always yield a root. Cheap to
        // check, and never assert() on anything attacker-influenced (R11) —
        // assertions compile out, and this must hold in release too.
        return MakeError(ErrorCode::MalformedMessage, "verified buffer has no root");
    }

    // A union whose type is NONE carries no body. Every message we route on
    // must have one, so reject it here rather than making every consumer
    // remember to check.
    if (env->body_type() == wire::Body::NONE) {
        return MakeError(ErrorCode::MalformedMessage, "envelope has no body");
    }

    return VerifiedFrame{env, regions->payload};
}

Result<const wire::PeerSignal*> VerifyPeerSignal(
    std::span<const std::byte> bytes) noexcept {
    // Relayed through the coordinator from another worker, so this is peer bytes
    // that took a detour — no more trusted for having passed through.
    if (bytes.empty()) {
        return MakeError(ErrorCode::MalformedMessage, "empty peer signal");
    }
    if (bytes.size() > kMaxSignalBytes) {
        return MakeError(ErrorCode::PayloadTooLarge, "peer signal exceeds kMaxSignalBytes");
    }
    ::flatbuffers::Verifier verifier(AsVerifierBytes(bytes), bytes.size(),
                                     kMaxVerifyDepth, kMaxVerifyTables);
    if (!verifier.VerifyBuffer<wire::PeerSignal>(nullptr)) {
        return MakeError(ErrorCode::MalformedMessage, "peer signal verification failed");
    }
    const auto* sig = ::flatbuffers::GetRoot<wire::PeerSignal>(bytes.data());
    if (sig == nullptr || sig->kind() == nullptr || sig->text() == nullptr) {
        return MakeError(ErrorCode::MalformedMessage, "peer signal missing kind or text");
    }
    return sig;
}

Result<const wire::AssetMsg*> VerifyAssetMsg(std::span<const std::byte> bytes) noexcept {
    // Peer-supplied bytes reach this path with NO coordinator mediation, so the
    // same bounds apply and for stronger reasons.
    if (bytes.empty()) {
        return MakeError(ErrorCode::MalformedMessage, "empty asset message");
    }
    if (bytes.size() > kMaxEnvelopeBytes) {
        return MakeError(ErrorCode::PayloadTooLarge, "asset message exceeds kMaxEnvelopeBytes");
    }

    ::flatbuffers::Verifier verifier(AsVerifierBytes(bytes), bytes.size(),
                                     kMaxVerifyDepth, kMaxVerifyTables);

    // flatc emits VerifyXBuffer only for the declared root_type (Envelope), and
    // a schema may declare only one. This is what that generated helper does
    // internally, applied to a different root.
    if (!verifier.VerifyBuffer<wire::AssetMsg>(nullptr)) {
        return MakeError(ErrorCode::MalformedMessage, "asset message verification failed");
    }

    // GetRoot<T>, not GetAssetMsg: flatc emits a Get* helper only for the
    // declared root_type, so a non-root table needs the generic form. This is
    // the same accessor the generated helper is a thin wrapper around.
    const auto* msg = ::flatbuffers::GetRoot<wire::AssetMsg>(bytes.data());
    if (msg == nullptr || msg->body_type() == wire::AssetBody::NONE) {
        return MakeError(ErrorCode::MalformedMessage, "asset message has no body");
    }
    return msg;
}

}  // namespace p2pgpu::protocol
