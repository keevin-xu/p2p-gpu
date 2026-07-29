//! # p2pgpu native worker
//!
//! Headless `wgpu` worker. Speaks the identical protocol as the browser worker
//! and loads the **same WGSL source** from `kernels/` (`docs/KERNELS.md`).
//!
//! Two jobs:
//! 1. Real-GPU load testing on cloud VMs, and cross-vendor measurement (E7).
//! 2. Headless kernel testing in CI — golden, chunk-invariance, limits, and
//!    cross-implementation tests all run through this binary
//!    (`docs/KERNELS.md` §5), because CI has no browser.
//!
//! ## Parity requirement
//!
//! This host and `web/worker` must implement chunking, accumulation, and
//! `TaskStats` population **identically**. Where they diverge, one is wrong.
//! Note that the *policy* (how big a task is, how often to upload) comes from
//! the coordinator per rule R1 — only the mechanism lives here. That is what
//! keeps the divergence surface small.
//!
//! Built in Phase 4; see `docs/phases/PHASE_4.md`.

fn main() {
    // Phase 4 steps 4.1–4.6: wgpu device acquisition (headless, no surface),
    // capability reporting matching the browser's shape, WebSocket client,
    // kernel execution host with ≤250 ms chunked dispatch (R4/K1),
    // device-loss recovery.
    println!("p2pgpu native worker — not yet implemented. See docs/phases/PHASE_4.md");
}
