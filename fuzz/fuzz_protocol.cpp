// libFuzzer harness for the frame parser — the project's primary attack surface.
//
// Every byte here is what an anonymous worker can send to the coordinator.
// This harness is the evidence behind rule R11 and decision D-0010; its exec
// count and corpus get reported in EVALUATION.md E8.
//
// Implement in Phase 1 step 1.5 — BEFORE building anything on top of the
// protocol. Retrofitting fuzzing after three phases of code is how you end up
// with a corpus of two files and nothing to say at an interview.
//
// Seed the corpus with:
//   - valid frames produced by your own encoder (one per message type)
//   - truncated header (0..11 bytes)
//   - wrong magic
//   - wrong protocol_version
//   - fb_len == UINT32_MAX
//   - fb_len larger than the frame
//   - fb_len smaller than the frame (trailing garbage)
//   - valid header + random bytes where the FlatBuffer should be
//   - deeply nested tables (verifier depth exhaustion)
//   - payload_follows set with no payload

#include <cstddef>
#include <cstdint>

// #include "p2pgpu/protocol/verify.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // TODO(1.5):
    //   auto span = std::span<const std::byte>(
    //       reinterpret_cast<const std::byte*>(data), size);
    //   auto result = p2pgpu::protocol::VerifyEnvelope(span);
    //   if (result) {
    //       // Touch every field of the accepted message. A Verifier that
    //       // passes a buffer whose fields then fault is the exact bug this
    //       // harness exists to find — so read them, don't just check the
    //       // return value.
    //   }
    //
    // The reinterpret_cast above is the one permitted exception to
    // CONVENTIONS.md §2: it is the libFuzzer entry signature, not parsing.
    // Nothing downstream of the span may use raw pointers.
    (void)data;
    (void)size;
    return 0;
}
