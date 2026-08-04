#pragma once
//
// Durable coordinator state — steps 2.19 (store) and 2.20 (recovery).
//
// ── MEMORY IS THE HOT PATH; THIS IS THE DURABILITY LAYER (D-0006) ────────
// Nothing here is consulted to make a scheduling decision. `JobManager` answers
// every question from memory, marks what changed, and `Server::Sweep` flushes
// the changed rows in ONE transaction per tick (D-0048). A restart replays this
// file back into a `JobManager` and the job resumes.
//
// ── WHAT A RESTART LOSES, ON PURPOSE ─────────────────────────────────────
// Up to one sweep interval. Safe because in-flight leases are discarded on
// recovery anyway, and a lost acceptance costs redundant work rather than a
// wrong answer (R8). The durable state is always BEHIND memory, never ahead —
// which is the ordering that can only ever repeat work, not lose it.
//
// ── NO SQL IS EVER BUILT BY CONCATENATION ────────────────────────────────
// Every statement is prepared once and bound with `sqlite3_bind_*`. Not because
// a job id is hostile today, but because "this value is internal" is the
// reasoning that precedes the first injection, and the coordinator's input
// surface is hostile by definition (R11).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "p2pgpu/coordinator/job.hpp"
#include "p2pgpu/protocol/error.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace p2pgpu::coordinator {

/// Everything needed to rebuild a `JobManager` after a restart.
///
/// Deliberately plain data rather than a live `JobManager`: recovery has to
/// apply a POLICY to what it reads (in-flight leases become expired), and that
/// policy belongs in one visible place rather than buried in a loader.
struct RecoveredState {
    std::vector<Job> jobs;
    std::vector<Task> tasks;
    /// Highest id seen. Ids must not be reissued after a restart or a new task
    /// could collide with a persisted one and silently inherit its history.
    std::uint64_t next_id = 1;
};

/// A SQLite-backed store. One file, one connection, single-threaded — the same
/// thread that runs the event loop (CONVENTIONS.md §4).
class Store {
public:
    /// Open (creating if absent) and apply the schema.
    ///
    /// `path` may be ":memory:", which the tests use so they neither touch the
    /// filesystem nor leak state between cases.
    [[nodiscard]] static protocol::Result<std::unique_ptr<Store>> Open(
        const std::string& path);

    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    /// Write these jobs and tasks in ONE transaction (D-0048).
    ///
    /// Upserts: the caller passes whatever is dirty and does not track whether
    /// a row already exists. A partial failure rolls the whole batch back, so
    /// the file never holds half a sweep.
    [[nodiscard]] protocol::Status Flush(const std::vector<Job>& jobs,
                                         const std::vector<Task>& tasks);

    /// Read everything back.
    ///
    /// Applies NO policy — leases come back exactly as they were written, and
    /// it is `RecoverInto`'s job to decide they are expired. Keeping the reader
    /// literal means a test can assert what was actually stored.
    [[nodiscard]] protocol::Result<RecoveredState> LoadAll();

private:
    explicit Store(sqlite3* db) : db_(db) {}

    sqlite3* db_ = nullptr;

    /// Prepared once, reused for the life of the process. Recreating them per
    /// flush would re-parse the same two statements every 300 ms.
    sqlite3_stmt* upsert_job_ = nullptr;
    sqlite3_stmt* upsert_task_ = nullptr;
};

/// Rebuild a `JobManager` from `state`, applying the recovery policy (2.20).
///
/// THE POLICY, in one place: a task that was `Leased` or `Validating` when the
/// coordinator died is requeued. Its worker is gone — it was talking to a
/// process that no longer exists — and R8 says a worker disappearing mid-task
/// is the normal case, not an error. Terminal tasks stay terminal.
///
/// `prior_workers` is preserved, so invariant 6 still holds across a restart: a
/// worker that already computed a task does not get handed its replica after we
/// come back up.
void RecoverInto(JobManager& jobs, const RecoveredState& state);

}  // namespace p2pgpu::coordinator
