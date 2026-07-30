// The chaos harness — most of this project's evidence comes from here.
//
// Full protocol, no GPU. E1 (scaling), E3 (fault tolerance), E4 (Byzantine),
// and E5 (stragglers) are all produced by this binary. It turns week-long
// fleet experiments into 30-second runs, so it is the fast path, not a detour.
//
// Build it FIRST in Phase 2, before the lease manager and sizer.
//
// Usage target:
//   mock-worker --count 200 --coordinator ws://localhost:8080 \
//               --chaos byzantine_10pct --seed 42
#include <cstdio>
int main() { std::puts("p2pgpu mock-worker - see docs/phases/PHASE_2.md"); }
