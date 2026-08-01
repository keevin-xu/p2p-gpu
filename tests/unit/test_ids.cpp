// T1 — strong ID types (step 1.6).
//
// The headline property is NEGATIVE — that certain code does not compile — so
// most of this file is static_assert. A runtime test cannot observe the absence
// of an implicit conversion; if these ever start compiling, the type has stopped
// doing its job and no amount of CHECK() would notice.

#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <unordered_map>
#include <vector>

#include "p2pgpu/protocol/ids.hpp"
#include "p2pgpu/protocol/invariants.hpp"

using namespace p2pgpu::protocol;
namespace wire = p2pgpu::wire;

// ── THE POINT OF THE STEP ───────────────────────────────────────────────
// Every one of these would have compiled before 1.6, in the two functions that
// decide whether a worker is ALLOWED to act (invariants 5 and 6).
static_assert(!std::is_convertible_v<TaskId, WorkerId>);
static_assert(!std::is_convertible_v<WorkerId, TaskId>);
static_assert(!std::is_convertible_v<TaskId, JobId>);
static_assert(!std::is_convertible_v<JobId, WorkerId>);
static_assert(!std::is_constructible_v<TaskId, WorkerId>);
static_assert(!std::is_assignable_v<TaskId&, WorkerId>);

// The wire type must not silently become a strong one either, or the boundary
// leaks the very ambiguity this removes.
static_assert(!std::is_convertible_v<wire::Uuid, TaskId>);
static_assert(std::is_constructible_v<TaskId, wire::Uuid>);   // explicit only

// Same-type operations must still work.
static_assert(std::is_convertible_v<TaskId, TaskId>);
static_assert(std::is_copy_constructible_v<TaskId>);
static_assert(std::is_nothrow_move_constructible_v<TaskId>);

// Zero-cost: identical footprint to the wire representation.
static_assert(sizeof(TaskId) == 16);
static_assert(sizeof(TaskId) == sizeof(wire::Uuid));
static_assert(alignof(TaskId) == alignof(wire::Uuid));

TEST_CASE("IDs compare on both halves", "[ids]") {
    CHECK(TaskId{1, 2} == TaskId{1, 2});
    CHECK(TaskId{1, 2} != TaskId{1, 3});
    // Comparing only `hi` would let one task masquerade as any other sharing a
    // high word — a 2^64 shortcut into someone else's lease.
    CHECK(TaskId{1, 2} != TaskId{9, 2});
    CHECK(TaskId{1, 2} != TaskId{2, 1});
}

TEST_CASE("IDs order totally, so they work in sorted containers", "[ids]") {
    CHECK(TaskId{1, 1} < TaskId{1, 2});
    CHECK(TaskId{1, 9} < TaskId{2, 0});
    CHECK_FALSE(TaskId{1, 1} < TaskId{1, 1});
}

TEST_CASE("nil is the absent sentinel", "[ids]") {
    CHECK(TaskId{}.is_nil());
    CHECK(TaskId{0, 0}.is_nil());
    CHECK_FALSE(TaskId{0, 1}.is_nil());
    CHECK_FALSE(TaskId{1, 0}.is_nil());
}

TEST_CASE("wire round-trip preserves both halves", "[ids]") {
    // Goes through the generated accessors, which apply EndianScalar — so this
    // also pins that we are not reading the raw members and getting it wrong on
    // a big-endian host.
    const TaskId original{0xDEADBEEFCAFEBABEULL, 0x0123456789ABCDEFULL};
    const TaskId back{original.to_wire()};
    CHECK(back == original);
    CHECK(back.hi() == 0xDEADBEEFCAFEBABEULL);
    CHECK(back.lo() == 0x0123456789ABCDEFULL);
}

TEST_CASE("IDs are usable as hash-map keys", "[ids]") {
    // The coordinator's lease and reputation tables need this in Phase 2.
    std::unordered_map<TaskId, int> m;
    m[TaskId{1, 2}] = 10;
    m[TaskId{3, 4}] = 20;
    CHECK(m.at(TaskId{1, 2}) == 10);
    CHECK(m.at(TaskId{3, 4}) == 20);
    CHECK(m.find(TaskId{5, 6}) == m.end());

    // Both halves must feed the hash. If only `lo` did, these would collide and
    // the second insert would overwrite the first.
    std::unordered_map<TaskId, int> h;
    h[TaskId{1, 7}] = 1;
    h[TaskId{2, 7}] = 2;
    CHECK(h.size() == 2);
}

TEST_CASE("strong types reach the authorization invariants", "[ids][invariants]") {
    // The mix-up this step prevents is now a compile error, so what remains
    // testable is that the correctly-typed call still behaves.
    const std::vector<TaskId> held{TaskId{1, 2}};
    CHECK(CheckLeaseHeld(held, TaskId{1, 2}));
    CHECK_FALSE(CheckLeaseHeld(held, TaskId{1, 3}));

    const std::vector<WorkerId> prior{WorkerId{1, 2}};
    CHECK_FALSE(CheckReplicaAssignment(prior, WorkerId{1, 2}));
    CHECK(CheckReplicaAssignment(prior, WorkerId{9, 9}));
}
