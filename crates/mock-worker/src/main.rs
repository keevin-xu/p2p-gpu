//! # p2pgpu mock worker — the chaos harness
//!
//! Implements the full protocol; **never touches a GPU**. Sleeps for a computed
//! duration and returns canned results.
//!
//! ## Why this is the most important crate in the repo
//!
//! Most of the project's evidence comes from here. E1 (scaling), E3 (fault
//! tolerance), E4 (Byzantine detection), and E5 (stragglers) are all produced
//! by this harness. It turns week-long fleet experiments into 30-second test
//! runs, which makes it the fast path, not a detour (`docs/WORKFLOW.md` §8).
//!
//! Build it **first** in Phase 2, before the lease manager and sizer — they are
//! far easier to develop against a fleet you can summon on demand.
//!
//! ## Design constraints
//!
//! - N virtual workers as **N tokio tasks in one process**, not N processes.
//! - Every behavior is seeded and reproducible. A chart that cannot be
//!   regenerated from its seed is not evidence (`docs/CONVENTIONS.md` §6).
//! - Simulated throughput drawn from a configurable distribution
//!   (default: log-normal, ~20× spread) so heterogeneity is realistic.
//!
//! ## Injectable behaviors (step 2.3)
//!
//! `slow` · `dies_mid_task` · `returns_garbage` · `lies_probabilistically(p)`
//! `high_latency(ms)` · `never_renews_lease` · `duplicate_submit` · `flaps`
//!
//! Each independently toggleable. These are the experimental instruments for
//! phases 3–7 — keep their semantics clean and orthogonal.
//!
//! ## Chaos profiles (step 2.4)
//!
//! Named presets referenced from experiments so runs are reproducible:
//! `default` · `heterogeneous` · `byzantine_10pct` · `flaky_network`
//! `mass_departure`
//!
//! Built in Phase 2; see `docs/phases/PHASE_2.md`.

fn main() {
    // Phase 2 steps 2.1–2.4.
    // Usage target:
    //   mock-worker --count 200 --coordinator ws://localhost:8080 \
    //               --chaos byzantine_10pct --seed 42
    println!("p2pgpu mock worker — not yet implemented. See docs/phases/PHASE_2.md");
}
