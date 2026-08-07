// Second fuzz target — step 4.13. Asset chunk reassembly.
//
// ── WHY THIS PATH SPECIFICALLY ───────────────────────────────────────────
// `PROTOCOL.md` invariant 10 names it: `index * kChunkBytes` is the classic
// integer-overflow site. A large enough index wraps to a small offset and the
// write lands somewhere it should not — the same shape as the alignment bugs
// D-0027 and D-0028, which also ran correctly on two architectures for weeks
// before a fuzzer disagreed.
//
// ── BEFORE PHASE 6 MAKES IT LIVE, NOT AFTER ──────────────────────────────
// Nothing sends an `AssetMsg` today. That is exactly the argument for fuzzing
// it now: the corpus and any crashers are cheap to fix while the path has no
// callers, and Phase 6 arrives with the parser already hostile-tested rather
// than discovering this under a working data plane.
//
// ── THE HARNESS MUST ACTUALLY READ ───────────────────────────────────────
// Verification alone proves nothing about later field access. Every accessor is
// touched through `Consume` so the optimiser cannot delete the reads, which
// would leave a harness that looks busy in the exec counter and finds nothing.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <vector>

#include "p2pgpu/protocol/invariants.hpp"
#include "p2pgpu/protocol/limits.hpp"
#include "p2pgpu/protocol/verify.hpp"

namespace {

using namespace p2pgpu::protocol;
namespace wire = p2pgpu::wire;

/// Keep a read alive without inline asm.
///
/// `fuzz_protocol.cpp` uses an `asm volatile` barrier; here that produced
/// `ld: invalid r_symbolnum` on arm64 with Homebrew LLVM — the same toolchain
/// friction D-0015 records. A volatile sink is portable and does the same job:
/// the compiler may not elide a store to a volatile object, so the field really
/// is read.
volatile std::uint64_t g_sink = 0;

/// Out-of-line so the abort is a plain call rather than an inlined trap
/// sequence — the inlined form produced `ld: invalid r_symbolnum` on arm64
/// with Homebrew LLVM.
[[noreturn]] __attribute__((noinline)) void AbortNow() {
    std::fflush(nullptr);
    std::abort();
}

template <typename T>
void Consume(const T& v) {
    g_sink = static_cast<std::uint64_t>(v);
}

void TouchHash(const wire::Hash32* h) {
    if (h != nullptr) {
        Consume(h->a());
        Consume(h->b());
        Consume(h->c());
        Consume(h->d());
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // static_cast via void*, never reinterpret_cast (R11) — the same rule the
    // production path follows, so the harness exercises what ships.
    const std::span<const std::byte> bytes{
        static_cast<const std::byte*>(static_cast<const void*>(data)), size};

    const auto verified = VerifyAssetMsg(bytes);
    if (!verified) {
        return 0;   // rejected before any field was read: the intended path
    }
    const wire::AssetMsg* msg = *verified;
    if (msg == nullptr) {
        return 0;
    }

    switch (msg->body_type()) {
        case wire::AssetBody::AssetRequest:
            if (const auto* r = msg->body_as_AssetRequest()) {
                TouchHash(r->hash());
                Consume(r->chunk_from());
                Consume(r->chunk_to());
                // A range whose end precedes its start, or which spans more
                // than an asset can hold, must not become a loop bound.
                Consume(r->chunk_to() >= r->chunk_from());
            }
            break;

        case wire::AssetBody::AssetChunk:
            if (const auto* c = msg->body_as_AssetChunk()) {
                TouchHash(c->hash());
                const std::uint32_t index = c->index();
                const std::uint32_t total = c->total();
                const auto* payload = c->bytes();
                const std::size_t len = payload != nullptr ? payload->size() : 0;
                Consume(index);
                Consume(total);
                Consume(len);

                // THE INVARIANT UNDER TEST. Driven with attacker-chosen index
                // and total, which is the whole point — a fuzzer reaches
                // index = 0xFFFFFFFF far sooner than a test author thinks to.
                const auto ok = CheckAssetChunk(index, total, total, len);
                Consume(static_cast<bool>(ok));

                // ── AN ORACLE, NOT JUST A CRASH CHECK ──────────────────
                //
                // `index * kChunkBytes` is UNSIGNED arithmetic, so it wraps by
                // definition — it is not undefined behaviour and NO SANITIZER
                // FIRES. Verified: replacing ChunkOffset with the multiplication
                // its own comment forbids, the corpus replayed clean.
                //
                // A wrapped offset is a WRONG VALUE, not a crash, so the harness
                // has to state the property and abort when it breaks. Computed
                // here in 64-bit, where the product cannot wrap, and compared.
                // std::abort, not __builtin_trap: the trap intrinsic emits a
                // relocation Homebrew LLVM's ld rejects on arm64 (the same
                // toolchain friction D-0015 records). abort() is what libFuzzer
                // reports as a crash anyway.
                const std::uint64_t expected =
                    static_cast<std::uint64_t>(index) *
                    static_cast<std::uint64_t>(kChunkBytes);
                const auto off = ChunkOffset(index);

                if (expected > static_cast<std::uint64_t>(
                                   std::numeric_limits<std::size_t>::max())) {
                    // Not representable: must be refused, never wrapped.
                    if (off.has_value()) {
                        AbortNow();
                    }
                } else if (off.has_value()) {
                    if (static_cast<std::uint64_t>(*off) != expected) {
                        AbortNow();
                    }
                    Consume(*off);
                }

                // And an ACCEPTED chunk must land inside the reassembly buffer:
                // a valid offset plus a payload no larger than one chunk. This
                // is the write Phase 6 will actually perform.
                if (ok) {
                    if (!off.has_value() || len > kChunkBytes) {
                        AbortNow();
                    }
                    Consume(*off + len);
                }

                // Touch the bytes themselves — a length that disagrees with the
                // buffer is the Heartbleed shape, and only a read finds it.
                if (payload != nullptr) {
                    for (std::size_t i = 0; i < payload->size(); ++i) {
                        Consume(payload->Get(i));
                    }
                }
            }
            break;

        case wire::AssetBody::AssetMiss:
            if (const auto* m = msg->body_as_AssetMiss()) {
                TouchHash(m->hash());
            }
            break;

        case wire::AssetBody::NONE:
            break;
    }
    return 0;
}
