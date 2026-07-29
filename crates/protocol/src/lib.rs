//! # p2pgpu wire protocol
//!
//! **This crate is the single source of truth for everything that crosses the
//! wire** (rule R3). TypeScript types are generated from here into `bindings/`
//! via `ts-rs`; they are never hand-written or hand-edited.
//!
//! The authoritative specification is `docs/PROTOCOL.md`. If this code and that
//! document disagree, one of them is a bug — fix both in the same commit
//! (`docs/WORKFLOW.md` §6).
//!
//! ## Scope
//!
//! Types and validation helpers only. **No I/O, no scheduling, no policy.**
//! All decisions live in the coordinator (rule R1). If you find yourself
//! writing an algorithm here that the coordinator also needs, stop — see R2.
//!
//! ## Implementation order
//!
//! See `docs/phases/PHASE_1.md` steps 1.1–1.5.

#![forbid(unsafe_code)]

/// Bumped on any breaking change to `ClientMsg`, `ServerMsg`, `TaskEnvelope`,
/// or the binary framing. Mismatch is fatal — no negotiation, no compatibility
/// shims (`docs/PROTOCOL.md` §5).
pub const PROTOCOL_VERSION: u16 = 1;

/// Maximum size of `TaskEnvelope::params`. Invariant 1 in `docs/PROTOCOL.md` §4.
pub const MAX_PARAMS_BYTES: usize = 4 * 1024;

/// Maximum size of a single result payload. Invariant 2.
pub const MAX_OUTPUT_BYTES: u32 = 8 * 1024 * 1024;

/// Minimum accumulation upload interval. Uploading more often than this defeats
/// the arithmetic-intensity design (rule R5, decision D-0001). Invariant 6.
pub const MIN_UPLOAD_INTERVAL_MS: u32 = 500;

// ── Phase 1 implementation goes here ──────────────────────────────────────
//
// 1.1  ids            — WorkerId, TaskId, JobId newtypes over Uuid.
//                       Newtypes, not bare Uuid — mixing them up is a bug
//                       class worth designing out (CONVENTIONS.md §1).
// 1.1  task           — TaskEnvelope, OutputSpec, AccumulationSpec, DType,
//                       DeterminismClass
// 1.1  worker         — WorkerCapabilities, AdapterInfo, GpuLimits, TaskStats
// 1.2  msg            — ClientMsg, ServerMsg, ReleaseReason, RevokeReason,
//                       ErrorCode
// 1.4  validate       — the eight invariants from PROTOCOL.md §4
//
// Every wire type gets #[derive(TS)] #[ts(export)] targeting `bindings/`.

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn protocol_version_is_set() {
        assert_eq!(PROTOCOL_VERSION, 1);
    }
}
