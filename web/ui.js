/*
 * THE ONLY JAVASCRIPT IN THIS PROJECT.
 *
 * It exists for exactly one reason: WebAssembly cannot touch the DOM directly.
 * This file is the DOM surface for the R7 opt-in controls, called from
 * src/worker-browser/ui_bridge.cpp via EM_JS.
 *
 * ── KEEP IT DUMB ─────────────────────────────────────────────────────────
 * No logic. No state machine. No decisions. Read a slider, set some text,
 * fire a callback into C++. Anything more violates R1 (dumb worker) and R2
 * (one worker-core, never forked) — and reintroduces the second-language
 * boundary that the C++ pivot deleted (D-0008).
 *
 * If you catch yourself wanting to compute something here, the answer belongs
 * in worker-core, or in the coordinator.
 *
 * Implement in Phase 1 step 1.23.
 */

// Surface required by R7:
//   - start button        -> calls into C++ to begin contributing
//   - contributing badge  -> visible whenever GPU work is running
//   - throttle slider     -> 0.0-1.0, sent as Throttle{level}; the
//                            coordinator applies it without arguing
//   - stop button         -> instant, releases leases cleanly (Draining)

// TODO(1.23): wire these to ui_bridge.cpp via EM_JS.
