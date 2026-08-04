#pragma once
//
// Task lifecycle state machine — step 1.13. THE CORRECTNESS CORE.
//
// Shape is authoritative in ARCHITECTURE.md §5:
//
//     Queued --grant--> Leased --submit--> Validating --accept--> Accepted
//       ^                 |  ^                  |    \--reject--> Rejected
//       |                 |  \--renew--/        |
//       |                 |                     \--disagreement--> NeedsReplica
//       \--expiry/release-/                                             |
//       \----------------------------- issue replica -------------------/
//
// ── WHY DOUBLE-SWITCH RATHER THAN A TABLE ────────────────────────────────
// Both switches are exhaustive with NO `default:` arm. That is the entire
// point: adding a TaskState or a TaskEvent becomes a BUILD FAILURE at every
// place that must handle it. A transition table would be shorter and would
// silently accept a new state as "unhandled" — `-Werror=switch` is the
// mechanism, and a `default:` arm defeats it (ARCHITECTURE.md §5).
//
// ── WHY Result RATHER THAN assert() ──────────────────────────────────────
// ARCHITECTURE.md §5 says illegal transitions "assert; they never no-op
// silently". Taken literally that collides with CONVENTIONS.md §1 and R11:
// **never assert() on anything reachable from attacker input** — and a worker's
// `Submit` drives a transition, so these events ARE reachable.
//
// Returning `[[nodiscard]] Result<TaskState>` honours the intent better than
// assert does. It cannot be ignored, it never no-ops, and unlike an assertion
// it still holds in release — where an assert would have compiled out and let
// a hostile worker drive the machine into an unreachable state unchecked.

#include <cstdint>
#include <string_view>

#include "p2pgpu/protocol/error.hpp"

namespace p2pgpu::coordinator {

/// NEVER write a `default:` arm on a switch over this type.
enum class TaskState : std::uint8_t {
    Queued,        ///< available for granting; the only state a worker can be given
    Leased,        ///< held by a worker under a time-bounded lease (R8)
    Validating,    ///< result received, comparison in progress
    NeedsReplica,  ///< disagreement or low confidence; needs another opinion
    Accepted,      ///< terminal
    Rejected,      ///< terminal; the worker's reputation is penalised
    /// Terminal. Work superseded through NO FAULT of the worker — a
    /// speculative replica whose sibling finished first (2.17, D-0045).
    ///
    /// Deliberately NOT `Rejected`, which means a wrong answer and costs
    /// reputation. A straggler that loses a race it was entered into without
    /// being asked has done nothing wrong, and a fleet where the slowest
    /// workers accumulate rejections would drive off exactly the volunteers R8
    /// exists to keep.
    Cancelled,
};

/// What can happen to a task. Also switch-exhaustive; adding one breaks the
/// build in every state that must decide about it.
enum class TaskEvent : std::uint8_t {
    Grant,         ///< coordinator hands the task to a worker
    Renew,         ///< Progress{request_renew}; extends the lease
    Submit,        ///< ResultHeader arrived
    LeaseExpired,  ///< swept by the coordinator. NO reputation penalty (R8)
    Release,       ///< voluntary give-back. Also no penalty
    Accept,        ///< validator is satisfied
    Disagreement,  ///< replicas differ, or confidence is too low
    IssueReplica,  ///< a replica is being dispatched
    Reject,        ///< validator concluded the result is wrong
    Cancel,        ///< superseded by a speculative sibling; no penalty (D-0045)
};

[[nodiscard]] constexpr std::string_view ToString(TaskState s) noexcept {
    switch (s) {
        case TaskState::Queued:       return "Queued";
        case TaskState::Leased:       return "Leased";
        case TaskState::Validating:   return "Validating";
        case TaskState::NeedsReplica: return "NeedsReplica";
        case TaskState::Accepted:     return "Accepted";
        case TaskState::Rejected:     return "Rejected";
        case TaskState::Cancelled:    return "Cancelled";
    }
    return "?";  // unreachable for a valid enumerator; keeps the compiler happy
}

[[nodiscard]] constexpr std::string_view ToString(TaskEvent e) noexcept {
    switch (e) {
        case TaskEvent::Grant:        return "Grant";
        case TaskEvent::Renew:        return "Renew";
        case TaskEvent::Submit:       return "Submit";
        case TaskEvent::LeaseExpired: return "LeaseExpired";
        case TaskEvent::Release:      return "Release";
        case TaskEvent::Accept:       return "Accept";
        case TaskEvent::Disagreement: return "Disagreement";
        case TaskEvent::IssueReplica: return "IssueReplica";
        case TaskEvent::Reject:       return "Reject";
        case TaskEvent::Cancel:       return "Cancel";
    }
    return "?";
}

/// Terminal states accept no further events. Completion detection (step 2.18)
/// counts these, so "is this task done" has exactly one definition.
[[nodiscard]] constexpr bool IsTerminal(TaskState s) noexcept {
    switch (s) {
        case TaskState::Accepted:
        case TaskState::Rejected:
        case TaskState::Cancelled:
            return true;
        case TaskState::Queued:
        case TaskState::Leased:
        case TaskState::Validating:
        case TaskState::NeedsReplica:
            return false;
    }
    return false;
}

/// Apply `ev` to `from`. Returns the new state, or an error if the transition
/// is not in ARCHITECTURE.md §5.
///
/// `constexpr`, so transitions with statically-known operands are checked at
/// compile time and the test suite can assert most of the table without running.
[[nodiscard]] constexpr protocol::Result<TaskState> Advance(TaskState from,
                                                            TaskEvent ev) noexcept {
    // Every illegal (state, event) pair funnels here rather than being ignored.
    // ErrorCode::Internal because reaching one means OUR bookkeeping is wrong:
    // a worker cannot select a transition directly, only send a message that
    // the caller has already authorized (invariant 5) before advancing state.
    const auto illegal = [] {
        return protocol::MakeError(protocol::ErrorCode::Internal,
                                   "illegal task state transition");
    };

    // NO `default:` ANYWHERE BELOW. Adding a state or an event must break this
    // build — that is the whole mechanism (ARCHITECTURE.md §5).
    switch (from) {
        case TaskState::Queued:
            switch (ev) {
                case TaskEvent::Grant:
                    return TaskState::Leased;
                // A queued task is held by nobody, so nothing else can happen to
                // it. In particular Submit must fail: a worker submitting for an
                // unleased task is exactly what invariant 5 rejects upstream,
                // and this is the second line of that defence.
                case TaskEvent::Renew:
                case TaskEvent::Submit:
                case TaskEvent::LeaseExpired:
                case TaskEvent::Release:
                case TaskEvent::Accept:
                case TaskEvent::Disagreement:
                case TaskEvent::IssueReplica:
                case TaskEvent::Reject:
                case TaskEvent::Cancel:
                    return illegal();
            }
            break;

        case TaskState::Leased:
            switch (ev) {
                // Renew keeps the task Leased. The lease DEADLINE moves, which
                // the lease manager owns (2.6) — the state does not change, and
                // that is why renewal cannot be confused with re-granting.
                case TaskEvent::Renew:
                    return TaskState::Leased;
                case TaskEvent::Submit:
                    return TaskState::Validating;
                // Expiry and release are the SAME transition and deliberately
                // distinct events: neither penalises reputation (R8 — absence is
                // not malice), but 2.7 and 2.9 need to tell them apart in
                // metrics, and merging them would erase that.
                case TaskEvent::LeaseExpired:
                case TaskEvent::Release:
                    return TaskState::Queued;
                // A speculative sibling finished first. The work is done; this
                // holder is simply too late, and is not penalised (D-0045).
                case TaskEvent::Cancel:
                    return TaskState::Cancelled;
                case TaskEvent::Grant:
                case TaskEvent::Accept:
                case TaskEvent::Disagreement:
                case TaskEvent::IssueReplica:
                case TaskEvent::Reject:
                    return illegal();
            }
            break;

        case TaskState::Validating:
            switch (ev) {
                case TaskEvent::Accept:
                    return TaskState::Accepted;
                case TaskEvent::Reject:
                    return TaskState::Rejected;
                case TaskEvent::Disagreement:
                    return TaskState::NeedsReplica;
                // Superseded while being validated: a sibling was accepted
                // first, so this result is no longer needed. Not Rejected —
                // nobody said it was wrong.
                case TaskEvent::Cancel:
                    return TaskState::Cancelled;
                // No lease is held while validating, so expiry cannot apply and
                // a second Submit is not a state transition — duplicate submits
                // are discarded silently by the caller (2.10), not errored.
                case TaskEvent::Grant:
                case TaskEvent::Renew:
                case TaskEvent::Submit:
                case TaskEvent::LeaseExpired:
                case TaskEvent::Release:
                case TaskEvent::IssueReplica:
                    return illegal();
            }
            break;

        case TaskState::NeedsReplica:
            switch (ev) {
                // Back to Queued with replica_of set, so a different worker
                // picks it up — invariant 6 enforces "different" (D-0029).
                case TaskEvent::IssueReplica:
                    return TaskState::Queued;
                // The job finished while this was waiting for a second opinion.
                case TaskEvent::Cancel:
                    return TaskState::Cancelled;
                case TaskEvent::Grant:
                case TaskEvent::Renew:
                case TaskEvent::Submit:
                case TaskEvent::LeaseExpired:
                case TaskEvent::Release:
                case TaskEvent::Accept:
                case TaskEvent::Disagreement:
                case TaskEvent::Reject:
                    return illegal();
            }
            break;

        // Terminal. Every event is illegal, including a late Submit from a
        // speculative loser — that result is discarded by the caller, and
        // reviving a finished task would double-count the work.
        case TaskState::Accepted:
        case TaskState::Rejected:
        case TaskState::Cancelled:
            switch (ev) {
                case TaskEvent::Grant:
                case TaskEvent::Renew:
                case TaskEvent::Submit:
                case TaskEvent::LeaseExpired:
                case TaskEvent::Release:
                case TaskEvent::Accept:
                case TaskEvent::Disagreement:
                case TaskEvent::IssueReplica:
                case TaskEvent::Reject:
                // Cancelling an already-terminal task is a no-op the caller
                // must not treat as success — a sibling that expires after the
                // group finished is already done, not cancellable.
                case TaskEvent::Cancel:
                    return illegal();
            }
            break;
    }
    return illegal();
}

}  // namespace p2pgpu::coordinator
