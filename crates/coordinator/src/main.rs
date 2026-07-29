//! # p2pgpu coordinator
//!
//! **The only component in the system that makes decisions** (rule R1).
//! Scheduling, task sizing, validation, reputation, and retry logic all live
//! here. Workers execute and report facts; they never decide.
//!
//! Architecture: `docs/ARCHITECTURE.md` §2.
//!
//! ## Two rules that bite early
//!
//! - **No `unwrap()` / `expect()` outside tests and pre-serving startup.**
//!   A malicious worker panicking the coordinator is a total system failure
//!   (`docs/CONVENTIONS.md` §1).
//! - **Never trust worker clocks or `TaskStats`.** Lease expiry is decided
//!   solely here, on the coordinator clock (`docs/PROTOCOL.md` §5); stats are
//!   telemetry and never an input to correctness or credit.
//!
//! ## Module plan
//!
//! Built across phases 1–3; see the phase files for numbered steps.
//!
//! | Module      | Responsibility                                    | Phase |
//! |-------------|---------------------------------------------------|-------|
//! | `job`       | Job lifecycle, decomposition, completion detection | 1     |
//! | `queue`     | Task queue, priority, cache-affinity assignment    | 1 / 2 |
//! | `kernels`   | Manifest loading, WGSL serving, descriptors        | 1     |
//! | `lease`     | Grant / renew / expire, heartbeats, requeue        | 2     |
//! | `sizer`     | Adaptive task sizing, EWMA correction              | 2     |
//! | `store`     | SQLite persistence, crash recovery                 | 2     |
//! | `metrics`   | Counters, histograms, SSE feed                     | 2     |
//! | `signaling` | WebRTC SDP/ICE relay, peer lists                   | 6     |
//! | `validator` | Replication, determinism-class comparison, reputation | 3  |

fn main() {
    // Phase 1 step 1.9: axum server, WebSocket endpoint, tracing with the
    // correlation fields from CONVENTIONS.md §4 (worker_id, task_id, job_id,
    // phase), clap config, health endpoint.
    println!("p2pgpu coordinator — not yet implemented. See docs/phases/PHASE_1.md");
}
