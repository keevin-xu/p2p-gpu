#pragma once
//
// Strong ID types — step 1.6.
//
// `WorkerId`, `TaskId`, and `JobId` are all 128-bit values with identical
// layout, which is precisely why they must not be the same C++ type. Before
// this, invariant 5 (may this worker act on this task?) and invariant 6 (may
// this worker receive this replica?) both took bare `wire::Uuid`, so passing a
// worker list where a task list belonged compiled silently.
//
// Those are the two AUTHORIZATION checks in the protocol. A crossed argument
// there is a security bug wearing the costume of a typo — exactly the bug class
// CONVENTIONS.md §3 says to design out rather than test for.
//
// The tag-template approach costs nothing at runtime: each instantiation is two
// u64s with the same layout as wire::Uuid, and every operation is constexpr.

#include <cstddef>
#include <cstdint>
#include <functional>

#include "p2pgpu/p2pgpu_generated.h"

namespace p2pgpu::protocol {

/// A 128-bit identifier, distinguished at compile time by its tag.
///
/// `Tag` is only ever an incomplete type — it exists to make `Id<TaskTag>` and
/// `Id<WorkerTag>` unrelated types, and is never instantiated.
template <typename Tag>
class Id {
public:
    /// Default-constructs to nil. Nil is meaningful: FlatBuffers struct fields
    /// are optional, so an absent `replica_of` reads as "no replica" and maps
    /// naturally onto a nil TaskId.
    constexpr Id() noexcept = default;

    constexpr Id(std::uint64_t hi, std::uint64_t lo) noexcept : hi_(hi), lo_(lo) {}

    /// EXPLICIT, deliberately. An implicit conversion from `wire::Uuid` would
    /// re-open the exact hole this type exists to close: every wire ID would
    /// silently become any strong ID at the call site.
    /// NOT constexpr: flatc's generated accessors are not, because they route
    /// through EndianScalar. That is also why this goes through hi()/lo()
    /// rather than reading the members — on a big-endian host the stored bytes
    /// differ from the logical value, and only the accessors know that.
    explicit Id(const wire::Uuid& u) noexcept : hi_(u.hi()), lo_(u.lo()) {}

    [[nodiscard]] constexpr std::uint64_t hi() const noexcept { return hi_; }
    [[nodiscard]] constexpr std::uint64_t lo() const noexcept { return lo_; }

    /// Nil (0,0) means "absent". Reserved as a sentinel: never mint a real ID
    /// with this value, or "no task" becomes indistinguishable from a task.
    [[nodiscard]] constexpr bool is_nil() const noexcept { return hi_ == 0 && lo_ == 0; }

    /// Convert back for serialization. Also explicit in spirit — named rather
    /// than an `operator wire::Uuid()` so a crossing is visible at the call site.
    /// NOT constexpr, same reason: wire::Uuid's constructor applies
    /// EndianScalar and is not a literal type.
    [[nodiscard]] wire::Uuid to_wire() const noexcept { return wire::Uuid{hi_, lo_}; }

    // Compares BOTH halves. Comparing only `hi` would let any two IDs sharing a
    // high word collide — a 2^64 shortcut to another worker's task, and the
    // failure case test_invariants.cpp pins explicitly.
    friend constexpr bool operator==(const Id&, const Id&) noexcept = default;
    friend constexpr auto operator<=>(const Id&, const Id&) noexcept = default;

private:
    std::uint64_t hi_ = 0;
    std::uint64_t lo_ = 0;
};

// Tags are declared, never defined. They carry no data and cannot be
// instantiated — their only job is to make the Id instantiations distinct.
struct WorkerTag;
struct TaskTag;
struct JobTag;

using WorkerId = Id<WorkerTag>;
using TaskId = Id<TaskTag>;
using JobId = Id<JobTag>;

// The property this step exists to guarantee. Asserted here rather than only in
// tests, so it holds for anyone who includes the header.
static_assert(!std::is_convertible_v<TaskId, WorkerId>);
static_assert(!std::is_convertible_v<WorkerId, TaskId>);
static_assert(!std::is_convertible_v<JobId, TaskId>);
static_assert(!std::is_constructible_v<TaskId, WorkerId>);
// And no accidental widening from the wire type either.
static_assert(!std::is_convertible_v<wire::Uuid, TaskId>);

// Same size and alignment as the wire representation, so a strong ID costs
// nothing to hold or pass.
static_assert(sizeof(TaskId) == sizeof(wire::Uuid));
static_assert(alignof(TaskId) == alignof(wire::Uuid));

}  // namespace p2pgpu::protocol

// Hashing, so IDs work as keys in the coordinator's lease and reputation maps
// (Phase 2). Mixes both halves — hashing only `lo` would collide every ID
// sharing a low word, which is attacker-reachable if IDs are ever predictable.
template <typename Tag>
struct std::hash<p2pgpu::protocol::Id<Tag>> {
    [[nodiscard]] std::size_t operator()(
        const p2pgpu::protocol::Id<Tag>& id) const noexcept {
        const std::size_t h1 = std::hash<std::uint64_t>{}(id.hi());
        const std::size_t h2 = std::hash<std::uint64_t>{}(id.lo());
        // Boost-style combine: cheap, and adequate for a hash map. NOT a
        // security primitive — anything needing collision resistance uses
        // BLAKE3 instead.
        return h1 ^ (h2 + 0x9E3779B97F4A7C15ULL + (h1 << 6U) + (h1 >> 2U));
    }
};
