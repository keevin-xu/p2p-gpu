// EM_JS glue to web/ui.js — the ONLY JavaScript boundary in the project.
//
// Exists because WASM cannot touch the DOM directly. Carries the R7 opt-in
// surface: start button, contributing indicator, throttle slider, stop.
//
// ── KEEP IT DUMB ─────────────────────────────────────────────────────────
// Every function here is a one-line DOM write. No logic, no state, no
// decisions. The C++ side owns what the state IS; this only makes it visible.
// Anything more violates R1 and R2, and reintroduces the second-language
// boundary the C++ pivot deleted (D-0008).
//
// The test for whether something belongs here: could the page be replaced by a
// plain-text terminal and lose nothing but presentation? If not, the logic is
// in the wrong file.
//
// Direction of travel, stated once so it stays straight:
//   C++ -> DOM   the EM_JS functions below. Status, badge, counters.
//   DOM -> C++   EMSCRIPTEN_KEEPALIVE entry points in main.cpp. Start, stop,
//                throttle — the only three things a user can do.

// NO `#if defined(__EMSCRIPTEN__)` GUARD, deliberately. This file is only ever
// compiled into the `worker-browser` target, which only exists in the wasm
// branch of CMakeLists — a guard here would be dead code that also trips
// tools/check_seam.py, since R2's exemption covers src/worker-core/platform/
// and nothing else. main.cpp includes <emscripten/emscripten.h> unguarded for
// the same reason.

#include <emscripten/emscripten.h>

#include "ui_bridge.hpp"

// A DOM write is best-effort by nature: an element can be absent because
// somebody edited the page, and a worker that died over a missing <div> would
// be absurd. Every function null-checks and does nothing, rather than throwing
// back into WASM where an exception would unwind through the task loop.

EM_JS(void, p2pgpu_ui_status, (const char* text), {
    var el = document.getElementById("status");
    if (el) { el.textContent = UTF8ToString(text); }
});

// THE R7 CONTRIBUTING INDICATOR. Must be visible whenever GPU work is running.
// Driven from the task loop's own record of whether it is executing — never
// inferred by the page, which would be a second source of truth about the one
// fact R7 actually requires be honest.
EM_JS(void, p2pgpu_ui_contributing, (int on), {
    var el = document.getElementById("contributing");
    if (el) {
        el.textContent = on ? "● contributing" : "○ idle";
        el.className = on ? "on" : "";
    }
});

EM_JS(void, p2pgpu_ui_connected, (int on), {
    var el = document.getElementById("conn");
    if (el) {
        el.textContent = on ? "connected" : "disconnected";
        el.className = on ? "on" : "";
    }
});

EM_JS(void, p2pgpu_ui_counters, (int completed, int failed, int recoveries), {
    var el = document.getElementById("counters");
    if (el) {
        el.textContent = completed + " completed · " + failed + " failed · " +
                         recoveries + " device recoveries";
    }
});

// Which controls are legal is decided in C++, because C++ is what knows whether
// the loop is running. Letting the page track that would be a second state
// machine, and two state machines about one fact eventually disagree.
EM_JS(void, p2pgpu_ui_running, (int on), {
    var start = document.getElementById("start");
    var stop = document.getElementById("stop");
    var url = document.getElementById("url");
    if (start) { start.disabled = !!on; }
    if (stop) { stop.disabled = !on; }
    if (url) { url.disabled = !!on; }
});

namespace p2pgpu::worker::ui {

void SetStatus(const char* text) { p2pgpu_ui_status(text); }
void SetContributing(bool on) { p2pgpu_ui_contributing(on ? 1 : 0); }
void SetConnected(bool on) { p2pgpu_ui_connected(on ? 1 : 0); }
void SetRunning(bool on) { p2pgpu_ui_running(on ? 1 : 0); }
void SetCounters(int completed, int failed, int recoveries) {
    p2pgpu_ui_counters(completed, failed, recoveries);
}

}  // namespace p2pgpu::worker::ui
