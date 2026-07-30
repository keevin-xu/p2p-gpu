// Emscripten entry point. Thin wrapper over worker-core.
// Task loop runs off the main thread (Phase 1 step 1.24) — background tabs
// throttle the main thread and rAF stops firing.
#include <cstdio>
int main() { std::puts("p2pgpu worker-browser - see docs/phases/PHASE_1.md"); }
