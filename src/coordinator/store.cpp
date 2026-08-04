// coordinator/store — durable state, steps 2.19/2.20. See store.hpp.
// The coordinator is the ONLY component that makes decisions (rule R1).
// No unwrap-equivalent: never crash on worker input (docs/CONVENTIONS.md §1).

#include "p2pgpu/coordinator/store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <string_view>

namespace p2pgpu::coordinator {
namespace {

using protocol::ErrorCode;
using protocol::MakeError;

/// The schema, applied on every open. `IF NOT EXISTS` makes opening an existing
/// file a no-op rather than a special case.
///
/// Ids are two INTEGER columns rather than a blob or a string: they ARE two
/// 64-bit halves (`protocol::Id`), and any other representation is a conversion
/// that can disagree with itself. `prior_workers` is the one exception — a
/// variable-length list, kept in its own table because a comma-joined column
/// would be a parser, and this project does not add parsers to storage.
///
/// NOTE what `tasks` deliberately does NOT store: `holder` and
/// `lease_expires_at_ms`. Recovery discards in-flight leases (2.20), so
/// persisting them would write values whose only use is to be thrown away — and
/// a column that exists invites a later reader to trust it.
constexpr const char* kSchema = R"(
CREATE TABLE IF NOT EXISTS jobs (
  id_hi        INTEGER NOT NULL,
  id_lo        INTEGER NOT NULL,
  kernel_id    TEXT    NOT NULL,
  seed         INTEGER NOT NULL,
  total_units  INTEGER NOT NULL,
  next_unit    INTEGER NOT NULL,
  PRIMARY KEY (id_hi, id_lo)
);
CREATE TABLE IF NOT EXISTS tasks (
  id_hi        INTEGER NOT NULL,
  id_lo        INTEGER NOT NULL,
  job_hi       INTEGER NOT NULL,
  job_lo       INTEGER NOT NULL,
  state        INTEGER NOT NULL,
  start_unit   INTEGER NOT NULL,
  unit_count   INTEGER NOT NULL,
  replica_hi   INTEGER NOT NULL,
  replica_lo   INTEGER NOT NULL,
  PRIMARY KEY (id_hi, id_lo)
);
CREATE TABLE IF NOT EXISTS task_prior_workers (
  task_hi      INTEGER NOT NULL,
  task_lo      INTEGER NOT NULL,
  worker_hi    INTEGER NOT NULL,
  worker_lo    INTEGER NOT NULL,
  PRIMARY KEY (task_hi, task_lo, worker_hi, worker_lo)
);
)";

protocol::Status Exec(sqlite3* db, const char* sql) {
    char* msg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        // sqlite owns `msg`; copy what we need before freeing it.
        const std::string detail = msg != nullptr ? msg : "unknown sqlite error";
        sqlite3_free(msg);
        return MakeError(ErrorCode::Internal, "sqlite exec failed: " + detail);
    }
    return {};
}

protocol::Result<sqlite3_stmt*> Prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return MakeError(ErrorCode::Internal,
                         std::string("sqlite prepare failed: ") + sqlite3_errmsg(db));
    }
    return stmt;
}

/// Bind a 64-bit id half. SQLite INTEGER is signed 64-bit and our ids are
/// unsigned, so this is a documented reinterpretation of the SAME 64 bits
/// rather than a value conversion — and `static_cast` back on read is exact for
/// every input, both directions being well-defined since C++20.
void BindU64(sqlite3_stmt* s, int idx, std::uint64_t v) {
    sqlite3_bind_int64(s, idx, static_cast<std::int64_t>(v));
}

std::uint64_t ColumnU64(sqlite3_stmt* s, int idx) {
    return static_cast<std::uint64_t>(sqlite3_column_int64(s, idx));
}

}  // namespace

protocol::Result<std::unique_ptr<Store>> Store::Open(const std::string& path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        const std::string detail = db != nullptr ? sqlite3_errmsg(db) : "out of memory";
        sqlite3_close(db);
        return MakeError(ErrorCode::Internal, "could not open store: " + detail);
    }

    // WAL + NORMAL (D-0048): the write transaction does not block readers, and
    // commits skip fsync. That risks power loss only, which is outside this
    // project's threat model, and it is what keeps the flush off the event
    // loop's critical path.
    //
    // WAL is unavailable for ":memory:" and sqlite declines the pragma rather
    // than failing, which is why its result is deliberately not checked.
    (void)Exec(db, "PRAGMA journal_mode = WAL;");
    if (const auto s = Exec(db, "PRAGMA synchronous = NORMAL;"); !s) {
        sqlite3_close(db);
        return s.error();
    }
    if (const auto s = Exec(db, kSchema); !s) {
        sqlite3_close(db);
        return s.error();
    }

    // `new` with a private constructor: make_unique cannot reach it, which
    // keeps `Open` the only way to obtain a Store — and `Open` is the only path
    // that applies the schema.
    std::unique_ptr<Store> store(new Store(db));

    auto job_stmt = Prepare(db, "INSERT INTO jobs "
                                "(id_hi,id_lo,kernel_id,seed,total_units,next_unit) "
                                "VALUES (?,?,?,?,?,?) "
                                "ON CONFLICT(id_hi,id_lo) DO UPDATE SET "
                                "next_unit=excluded.next_unit");
    if (!job_stmt) {
        return job_stmt.error();
    }
    store->upsert_job_ = *job_stmt;

    auto task_stmt = Prepare(db, "INSERT INTO tasks "
                                 "(id_hi,id_lo,job_hi,job_lo,state,start_unit,"
                                 "unit_count,replica_hi,replica_lo) "
                                 "VALUES (?,?,?,?,?,?,?,?,?) "
                                 "ON CONFLICT(id_hi,id_lo) DO UPDATE SET "
                                 "state=excluded.state");
    if (!task_stmt) {
        return task_stmt.error();
    }
    store->upsert_task_ = *task_stmt;

    return store;
}

Store::~Store() {
    sqlite3_finalize(upsert_job_);
    sqlite3_finalize(upsert_task_);
    sqlite3_close(db_);
}

protocol::Status Store::Flush(const std::vector<Job>& jobs,
                              const std::vector<Task>& tasks) {
    if (jobs.empty() && tasks.empty()) {
        return {};
    }

    // ONE transaction for the whole sweep (D-0048), which is also what makes a
    // partial failure harmless: the file never holds half a batch.
    if (const auto s = Exec(db_, "BEGIN IMMEDIATE;"); !s) {
        return s;
    }

    for (const Job& job : jobs) {
        sqlite3_reset(upsert_job_);
        BindU64(upsert_job_, 1, job.id.hi());
        BindU64(upsert_job_, 2, job.id.lo());
        // SQLITE_TRANSIENT: sqlite copies the string. STATIC would promise the
        // buffer outlives the step, and `job` is a reference into a caller's
        // vector whose lifetime we do not control.
        sqlite3_bind_text(upsert_job_, 3, job.kernel_id.c_str(), -1, SQLITE_TRANSIENT);
        BindU64(upsert_job_, 4, job.seed);
        BindU64(upsert_job_, 5, job.total_units);
        BindU64(upsert_job_, 6, job.next_unit);
        if (sqlite3_step(upsert_job_) != SQLITE_DONE) {
            const std::string detail = sqlite3_errmsg(db_);
            (void)Exec(db_, "ROLLBACK;");
            return MakeError(ErrorCode::Internal, "job write failed: " + detail);
        }
    }

    for (const Task& task : tasks) {
        sqlite3_reset(upsert_task_);
        BindU64(upsert_task_, 1, task.id.hi());
        BindU64(upsert_task_, 2, task.id.lo());
        BindU64(upsert_task_, 3, task.job.hi());
        BindU64(upsert_task_, 4, task.job.lo());
        BindU64(upsert_task_, 5, static_cast<std::uint64_t>(task.state));
        BindU64(upsert_task_, 6, task.start_unit);
        BindU64(upsert_task_, 7, task.unit_count);
        BindU64(upsert_task_, 8, task.replica_of.hi());
        BindU64(upsert_task_, 9, task.replica_of.lo());
        if (sqlite3_step(upsert_task_) != SQLITE_DONE) {
            const std::string detail = sqlite3_errmsg(db_);
            (void)Exec(db_, "ROLLBACK;");
            return MakeError(ErrorCode::Internal, "task write failed: " + detail);
        }

        // `prior_workers` only ever grows, so INSERT OR IGNORE against the
        // composite key makes re-writing a task idempotent without first
        // DELETEing its rows — which would briefly leave invariant 6 with
        // nothing to check if we died mid-flush.
        for (const WorkerId& w : task.prior_workers) {
            auto stmt = Prepare(db_, "INSERT OR IGNORE INTO task_prior_workers "
                                     "(task_hi,task_lo,worker_hi,worker_lo) "
                                     "VALUES (?,?,?,?)");
            if (!stmt) {
                (void)Exec(db_, "ROLLBACK;");
                return stmt.error();
            }
            BindU64(*stmt, 1, task.id.hi());
            BindU64(*stmt, 2, task.id.lo());
            BindU64(*stmt, 3, w.hi());
            BindU64(*stmt, 4, w.lo());
            const int rc = sqlite3_step(*stmt);
            sqlite3_finalize(*stmt);
            if (rc != SQLITE_DONE) {
                (void)Exec(db_, "ROLLBACK;");
                return MakeError(ErrorCode::Internal, "prior_worker write failed");
            }
        }
    }

    return Exec(db_, "COMMIT;");
}

protocol::Result<RecoveredState> Store::LoadAll() {
    RecoveredState state;

    auto job_q = Prepare(db_, "SELECT id_hi,id_lo,kernel_id,seed,total_units,next_unit "
                              "FROM jobs");
    if (!job_q) {
        return job_q.error();
    }
    while (sqlite3_step(*job_q) == SQLITE_ROW) {
        Job job;
        job.id = JobId{ColumnU64(*job_q, 0), ColumnU64(*job_q, 1)};
        const auto* text = sqlite3_column_text(*job_q, 2);
        // static_cast via void*, never reinterpret_cast (R11): sqlite hands
        // back `unsigned char*` and the bytes are UTF-8 either way.
        job.kernel_id =
            text != nullptr
                ? std::string(static_cast<const char*>(static_cast<const void*>(text)))
                : std::string{};
        job.seed = ColumnU64(*job_q, 3);
        job.total_units = ColumnU64(*job_q, 4);
        job.next_unit = ColumnU64(*job_q, 5);
        state.next_id = std::max(state.next_id, job.id.lo() + 1);
        state.jobs.push_back(std::move(job));
    }
    sqlite3_finalize(*job_q);

    auto task_q = Prepare(db_, "SELECT id_hi,id_lo,job_hi,job_lo,state,start_unit,"
                               "unit_count,replica_hi,replica_lo FROM tasks");
    if (!task_q) {
        return task_q.error();
    }
    while (sqlite3_step(*task_q) == SQLITE_ROW) {
        Task task;
        task.id = TaskId{ColumnU64(*task_q, 0), ColumnU64(*task_q, 1)};
        task.job = JobId{ColumnU64(*task_q, 2), ColumnU64(*task_q, 3)};

        // A state outside the enum means a corrupt or newer file. CLAMPED, not
        // cast blindly: an out-of-range TaskState would make every switch in
        // the state machine fall past its arms with no `default:` to catch it
        // (D-0011) — undefined behaviour reached from a file on disk, which is
        // exactly the R11 shape even though this input is not from the network.
        // Validated via ToString, which returns "?" for anything outside the
        // enum and — like every switch over TaskState — has no `default:` arm,
        // so a newly added state cannot silently read as valid here.
        const std::uint64_t raw = ColumnU64(*task_q, 4);
        const auto candidate = static_cast<TaskState>(raw);
        task.state = ToString(candidate) == std::string_view{"?"} ? TaskState::Queued
                                                                  : candidate;

        task.start_unit = ColumnU64(*task_q, 5);
        task.unit_count = ColumnU64(*task_q, 6);
        task.replica_of = TaskId{ColumnU64(*task_q, 7), ColumnU64(*task_q, 8)};
        state.next_id = std::max(state.next_id, task.id.lo() + 1);
        state.tasks.push_back(std::move(task));
    }
    sqlite3_finalize(*task_q);

    auto pw_q = Prepare(db_, "SELECT task_hi,task_lo,worker_hi,worker_lo "
                             "FROM task_prior_workers");
    if (!pw_q) {
        return pw_q.error();
    }
    while (sqlite3_step(*pw_q) == SQLITE_ROW) {
        const TaskId tid{ColumnU64(*pw_q, 0), ColumnU64(*pw_q, 1)};
        const WorkerId wid{ColumnU64(*pw_q, 2), ColumnU64(*pw_q, 3)};
        const auto it =
            std::ranges::find_if(state.tasks, [&](const Task& t) { return t.id == tid; });
        if (it != state.tasks.end()) {
            it->prior_workers.push_back(wid);
        }
    }
    sqlite3_finalize(*pw_q);

    return state;
}

void RecoverInto(JobManager& jobs, const RecoveredState& state) {
    jobs.AdoptRecovered(state.jobs, state.tasks, state.next_id);
}

}  // namespace p2pgpu::coordinator
