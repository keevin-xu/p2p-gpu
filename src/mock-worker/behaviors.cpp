// Injectable misbehavior — the experimental instruments for Phases 3-7.
// Keep semantics clean and orthogonal; every one is seeded and reproducible.
//
//   slow · dies_mid_task · returns_garbage · lies_probabilistically(p)
//   high_latency(ms) · never_renews_lease · duplicate_submit · flaps
//   malformed_frames   <- live-fire counterpart to the Phase 1 fuzzer
//
// Implement in Phase 2 steps 2.3 and 2.5.
