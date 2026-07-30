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
inline constexpr std::size_t kHeaderBytes = 12;

/// Max FlatBuffers Envelope size. Checked BEFORE the Verifier runs.
inline constexpr std::uint32_t kMaxEnvelopeBytes = 64u * 1024u;

/// TaskEnvelope::params (invariant 1).
inline constexpr std::uint32_t kMaxParamsBytes = 4u * 1024u;

/// OutputSpec::bytes and any single result payload (invariant 2).
inline constexpr std::uint32_t kMaxOutputBytes = 8u * 1024u * 1024u;

/// Data-plane chunk size. 16 KiB is the safe cross-browser floor —
/// do NOT assume 256 KiB EOR support.
inline constexpr std::uint32_t kChunkBytes = 16u * 1024u;

/// Verifier recursion and table bounds. FlatBuffers' defense against
/// nesting-based resource exhaustion.
inline constexpr std::size_t kMaxVerifyDepth  = 16;
inline constexpr std::size_t kMaxVerifyTables = 4096;

/// Minimum accumulation upload interval (invariant 7). Uploading more often
/// defeats the arithmetic-intensity design that the whole project rests on
/// (rule R5, decision D-0001).
inline constexpr std::uint32_t kMinUploadIntervalMs = 500;

}  // namespace p2pgpu::protocol
