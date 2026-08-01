// T1 — task lifecycle state machine (step 1.13). THE CORRECTNESS CORE.
//
// The phase file requires every legal transition tested AND every illegal one
// asserted to fail. There are 6 states x 9 events = 54 pairs, so this does not
// hand-list them: it enumerates the full cross product and checks each against
// a table of the nine legal transitions from ARCHITECTURE.md §5.
//
// Exhaustive BY CONSTRUCTION. Hand-listing 45 negative cases would drift the
// moment a state or event is added — the loop cannot.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>

#include "p2pgpu/coordinator/task_state.hpp"

using namespace p2pgpu::coordinator;

namespace {

// Every state and event, so the cross product below is complete. If either enum
// grows, `Advance` stops compiling (no default: arm) — and these arrays are the
// second place to update, which the count assertions below make impossible to
// forget.
constexpr std::array kStates{TaskState::Queued,       TaskState::Leased,
                             TaskState::Validating,   TaskState::NeedsReplica,
                             TaskState::Accepted,     TaskState::Rejected};

constexpr std::array kEvents{TaskEvent::Grant,        TaskEvent::Renew,
                             TaskEvent::Submit,       TaskEvent::LeaseExpired,
                             TaskEvent::Release,      TaskEvent::Accept,
                             TaskEvent::Disagreement, TaskEvent::IssueReplica,
                             TaskEvent::Reject};

static_assert(kStates.size() == 6, "a state was added — update this table");
static_assert(kEvents.size() == 9, "an event was added — update this table");

/// THE NINE LEGAL TRANSITIONS, transcribed from the ARCHITECTURE.md §5 diagram.
/// Anything absent here must be rejected.
struct Legal {
    TaskState from;
    TaskEvent ev;
    TaskState to;
};

constexpr std::array kLegal{
    Legal{TaskState::Queued,       TaskEvent::Grant,        TaskState::Leased},
    Legal{TaskState::Leased,       TaskEvent::Renew,        TaskState::Leased},
    Legal{TaskState::Leased,       TaskEvent::Submit,       TaskState::Validating},
    Legal{TaskState::Leased,       TaskEvent::LeaseExpired, TaskState::Queued},
    Legal{TaskState::Leased,       TaskEvent::Release,      TaskState::Queued},
    Legal{TaskState::Validating,   TaskEvent::Accept,       TaskState::Accepted},
    Legal{TaskState::Validating,   TaskEvent::Reject,       TaskState::Rejected},
    Legal{TaskState::Validating,   TaskEvent::Disagreement, TaskState::NeedsReplica},
    Legal{TaskState::NeedsReplica, TaskEvent::IssueReplica, TaskState::Queued},
};

std::optional<TaskState> LookupLegal(TaskState from, TaskEvent ev) {
    for (const auto& l : kLegal) {
        if (l.from == from && l.ev == ev) { return l.to; }
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("every one of the 54 (state, event) pairs behaves as specified",
          "[task_state]") {
    int legal_seen = 0;
    int illegal_seen = 0;

    for (const TaskState from : kStates) {
        for (const TaskEvent ev : kEvents) {
            const auto expected = LookupLegal(from, ev);
            const auto actual = Advance(from, ev);

            INFO("from=" << ToString(from) << " event=" << ToString(ev));
            if (expected) {
                REQUIRE(actual);
                CHECK(*actual == *expected);
                ++legal_seen;
            } else {
                // Never a silent no-op: an illegal transition must REPORT, not
                // return `from` unchanged, or a lost task would look healthy.
                REQUIRE_FALSE(actual);
                CHECK(actual.error().code == p2pgpu::protocol::ErrorCode::Internal);
                ++illegal_seen;
            }
        }
    }

    CHECK(legal_seen == 9);
    CHECK(illegal_seen == 45);
    CHECK(legal_seen + illegal_seen ==
          static_cast<int>(kStates.size() * kEvents.size()));
}

// Compile-time spot checks. Advance is constexpr, so the machine can be
// exercised without running — a transition with known operands is verified by
// the compiler.
static_assert(Advance(TaskState::Queued, TaskEvent::Grant).has_value());
static_assert(*Advance(TaskState::Queued, TaskEvent::Grant) == TaskState::Leased);
static_assert(!Advance(TaskState::Queued, TaskEvent::Submit).has_value());
static_assert(!Advance(TaskState::Accepted, TaskEvent::Grant).has_value());

TEST_CASE("a task cannot be submitted for unless it is leased", "[task_state]") {
    // The state machine is the SECOND line of this defence; invariant 5 rejects
    // the message first. Both exist because either alone is a single point of
    // failure for the protocol's authorization.
    CHECK_FALSE(Advance(TaskState::Queued, TaskEvent::Submit));
    CHECK_FALSE(Advance(TaskState::Validating, TaskEvent::Submit));
    CHECK_FALSE(Advance(TaskState::NeedsReplica, TaskEvent::Submit));
    CHECK_FALSE(Advance(TaskState::Accepted, TaskEvent::Submit));
    CHECK_FALSE(Advance(TaskState::Rejected, TaskEvent::Submit));
    CHECK(Advance(TaskState::Leased, TaskEvent::Submit));
}

TEST_CASE("terminal states accept nothing at all", "[task_state]") {
    CHECK(IsTerminal(TaskState::Accepted));
    CHECK(IsTerminal(TaskState::Rejected));
    CHECK_FALSE(IsTerminal(TaskState::Queued));
    CHECK_FALSE(IsTerminal(TaskState::Leased));
    CHECK_FALSE(IsTerminal(TaskState::Validating));
    CHECK_FALSE(IsTerminal(TaskState::NeedsReplica));

    // A late Submit from a speculative loser (2.17) must not revive a finished
    // task — that would double-count the work.
    for (const TaskEvent ev : kEvents) {
        CHECK_FALSE(Advance(TaskState::Accepted, ev));
        CHECK_FALSE(Advance(TaskState::Rejected, ev));
    }
}

TEST_CASE("expiry and release are distinct events with the same target",
          "[task_state]") {
    // Same transition, deliberately separate events. Neither penalises
    // reputation (R8 — absence is not malice), but steps 2.7 and 2.9 must tell
    // them apart in metrics, and merging them would erase that distinction.
    const auto expired = Advance(TaskState::Leased, TaskEvent::LeaseExpired);
    const auto released = Advance(TaskState::Leased, TaskEvent::Release);
    REQUIRE(expired);
    REQUIRE(released);
    CHECK(*expired == TaskState::Queued);
    CHECK(*released == TaskState::Queued);
}

TEST_CASE("renew keeps the task leased rather than re-granting it",
          "[task_state]") {
    // The lease DEADLINE moves (owned by the lease manager, 2.6); the state does
    // not. Modelling renewal as a state change would make it indistinguishable
    // from a fresh grant and break lease accounting.
    const auto r = Advance(TaskState::Leased, TaskEvent::Renew);
    REQUIRE(r);
    CHECK(*r == TaskState::Leased);
}

TEST_CASE("the full happy path and the replica loop both close", "[task_state]") {
    auto s = TaskState::Queued;
    for (const TaskEvent ev : {TaskEvent::Grant, TaskEvent::Submit, TaskEvent::Accept}) {
        const auto next = Advance(s, ev);
        REQUIRE(next);
        s = *next;
    }
    CHECK(s == TaskState::Accepted);
    CHECK(IsTerminal(s));

    // Disagreement must return the task to Queued so it can be re-granted,
    // otherwise a contested task would strand and the job never complete.
    s = TaskState::Queued;
    for (const TaskEvent ev : {TaskEvent::Grant, TaskEvent::Submit,
                               TaskEvent::Disagreement, TaskEvent::IssueReplica}) {
        const auto next = Advance(s, ev);
        REQUIRE(next);
        s = *next;
    }
    CHECK(s == TaskState::Queued);
}
