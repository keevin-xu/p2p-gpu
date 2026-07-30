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
// Implement in Phase 1, steps 1.2–1.4. Fuzz it in step 1.5, before moving on.

#include "p2pgpu/protocol/limits.hpp"

namespace p2pgpu::protocol {

// TODO(1.2) ParseHeader(std::span<const std::byte>) -> std::optional<Header>
//
// TODO(1.3) VerifyEnvelope(std::span<const std::byte>) -> Result<const Envelope*>
//
//   Required sequence — deviating from it is a defect even if it appears to
//   work (docs/PROTOCOL.md §1):
//
//     1. size >= kHeaderBytes                    else MalformedMessage
//     2. ParseHeader on a bounds-checked span    else MalformedMessage
//     3. magic == kFrameMagic                    else MalformedMessage
//     4. protocol_ver == kProtocolVersion        else VersionMismatch (fatal)
//     5. fb_len <= kMaxEnvelopeBytes             else PayloadTooLarge
//     6. size >= kHeaderBytes + fb_len           else MalformedMessage
//     7. flatbuffers::Verifier with kMaxVerifyDepth / kMaxVerifyTables
//     8. ONLY NOW may any schema field be read
//
// TODO(1.4) the ten invariants from docs/PROTOCOL.md §4

}  // namespace p2pgpu::protocol
