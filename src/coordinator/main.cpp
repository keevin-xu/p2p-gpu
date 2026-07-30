// p2pgpu coordinator.
//
// The only decision-maker in the system (R1): scheduling, sizing, validation,
// reputation, retry. Workers execute and report facts.
//
// Two rules that bite early:
//   - No exceptions, no crash paths on worker input. A hostile worker taking
//     down the coordinator is total system failure (CONVENTIONS.md §1, R11).
//   - Never trust worker clocks or TaskStats. Lease expiry is decided here
//     (PROTOCOL.md §5); stats are telemetry only (invariant 8).
//
// Implement in Phase 1 step 1.11.
#include <cstdio>
int main() { std::puts("p2pgpu coordinator - see docs/phases/PHASE_1.md"); }
