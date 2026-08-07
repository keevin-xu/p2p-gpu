#pragma once
//
// The R7 opt-in surface, as C++ sees it — step 1.23.
//
// Browser-only by nature, so it lives in src/worker-browser/ and NOT in
// include/p2pgpu/worker/. That placement is the point: this is not part of
// worker-core's interface, and worker-core must not know a DOM exists (R2).
// `TaskLoop` reports its state through `status()` rather than by drawing
// anything; the browser wrapper reads that status and calls these.
//
// Five setters and nothing else. If this header grows a getter, some decision
// has moved into the page — which is exactly what R1 forbids.

namespace p2pgpu::worker::ui {

void SetStatus(const char* text);

/// THE R7 REQUIREMENT: visible whenever GPU work is running. Driven by the task
/// loop's own record of whether it is executing.
void SetContributing(bool on);

void SetConnected(bool on);

/// Which controls are legal. Decided in C++ because C++ knows whether the loop
/// is running; the page tracking that separately would be a second state
/// machine to disagree with the first.
void SetRunning(bool on);

void SetCounters(int completed, int failed, int recoveries);

/// Reveal the "GPU lost — reload to rejoin" panel (D-0065).
///
/// Reveals it only. The reload itself is a user click, because the page is not
/// entitled to reload itself out from under someone whose driver just crashed,
/// and because doing so discards the worker identity and resume token.
void SetGpuUnavailable(bool on);

}  // namespace p2pgpu::worker::ui
