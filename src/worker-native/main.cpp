// Thin main() over worker-core. CLI11 args, then hand off.
//
// If this file grows past ~100 lines, logic is leaking out of worker-core
// and rule R1 is being violated.
//
// Phase 4 step 4.19 packages this so a borrowed machine can join with one
// command (docs/RISKS.md R-D).
#include <cstdio>
int main() { std::puts("p2pgpu worker-native - see docs/phases/PHASE_1.md"); }
