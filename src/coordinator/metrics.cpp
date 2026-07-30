// coordinator/metrics — see docs/ARCHITECTURE.md §4 for responsibility.
// The coordinator is the ONLY component that makes decisions (rule R1).
// No unwrap-equivalent: never crash on worker input (docs/CONVENTIONS.md §1).
