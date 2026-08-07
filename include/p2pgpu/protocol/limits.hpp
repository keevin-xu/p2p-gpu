#pragma once
//
// Hard limits on attacker-controlled sizes.
//
// EVERY length field arriving from the network is checked against one of
// these BEFORE it is used to allocate or index (rule R11, docs/PROTOCOL.md §4).
//
// FlatBuffers' Verifier prevents memory corruption. It does NOT prevent an
// attacker declaring a 4 GB payload and OOM-ing the coordinator — that class
// of bug is identical in every language, and these constants are the defense.

#include <cstddef>
#include <cstdint>

namespace p2pgpu::protocol {

/// Bumped on any breaking change to the schema or frame header.
/// Mismatch is fatal — no negotiation, no compatibility shims.
inline constexpr std::uint16_t kProtocolVersion = 1;

/// Frame header magic, "P2GP". Cheap wrong-protocol rejection before any
/// parsing work is done.
inline constexpr std::uint32_t kFrameMagic = 0x50324750u;

/// Fixed frame header size in bytes. See docs/PROTOCOL.md §1.
///
/// SIXTEEN, not twelve, and the four extra bytes are load-bearing (D-0027).
/// The FlatBuffers Envelope begins immediately after this header, and our
/// schema's minalign is 8 (`Uuid` is 2x u64, `Hash32` is 4x u64). A 12-byte
/// header puts the Envelope on a 4-byte boundary even when the frame itself is
/// perfectly aligned, making EVERY struct field access undefined behaviour.
/// Found by the step 1.5 fuzzer on a valid seed, not a hostile one.
///
/// 16 % 8 == 0, so an 8-byte-aligned frame yields an 8-byte-aligned Envelope.
/// Do not shrink this without re-reading D-0027.
inline constexpr std::size_t kHeaderBytes = 16;

/// Alignment the FlatBuffers Envelope requires, and therefore the alignment a
/// caller must give the whole frame. Equals the schema's minalign.
inline constexpr std::size_t kFrameAlignment = 8;

/// Max FlatBuffers Envelope size. Checked BEFORE the Verifier runs.
inline constexpr std::uint32_t kMaxEnvelopeBytes = 64u * 1024u;

/// TaskEnvelope::params (invariant 1).
inline constexpr std::uint32_t kMaxParamsBytes = 4u * 1024u;

/// OutputSpec::bytes and any single result payload (invariant 2).
inline constexpr std::uint32_t kMaxOutputBytes = 8u * 1024u * 1024u;

/// Data-plane chunk size. 16 KiB is the safe cross-browser floor —
/// do NOT assume 256 KiB EOR support.
inline constexpr std::uint32_t kChunkBytes = 16u * 1024u;

/// Largest bulk asset a worker will reassemble (5.16).
///
/// R11: the chunk COUNT is attacker-controlled, and a receiver that allocates
/// `total * kChunkBytes` without a cap can be made to reserve gigabytes by one
/// small frame. This is the bound that check goes against, and it is generous
/// on purpose — a scene BVH is megabytes, and refusing a legitimate one is a
/// worse failure than reserving 256 MB briefly.
inline constexpr std::uint64_t kMaxAssetBytes = 256ull * 1024ull * 1024ull;

/// Verifier recursion and table bounds. FlatBuffers' defense against
/// nesting-based resource exhaustion.
inline constexpr std::size_t kMaxVerifyDepth  = 16;
inline constexpr std::size_t kMaxVerifyTables = 4096;

/// Minimum accumulation upload interval (invariant 7). Uploading more often
/// defeats the arithmetic-intensity design that the whole project rests on
/// (rule R5, decision D-0001).
inline constexpr std::uint32_t kMinUploadIntervalMs = 500;

}  // namespace p2pgpu::protocol
