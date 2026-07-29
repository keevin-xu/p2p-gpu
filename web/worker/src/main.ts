/**
 * p2pgpu browser worker — UI thread entry point.
 *
 * This file owns the opt-in UI and nothing else. The task loop lives in
 * `src/task-worker.ts`, running inside a Web Worker: background tabs throttle
 * the main thread and `requestAnimationFrame` stops firing, so a main-thread
 * task loop silently stops contributing the moment the user switches tabs
 * (docs/RISKS.md §1).
 *
 * RULE R1 — DUMB WORKER. This client makes no decisions. Task sizing,
 * scheduling, validation, and retry policy all live in the coordinator. If you
 * are about to write an algorithm here that the coordinator also has, stop:
 * that is rule R2, and it is the thing that makes the Rust/TypeScript split
 * cost nothing.
 *
 * All wire types come from `@bindings/*` — generated from `crates/protocol/`.
 * Never hand-declare a wire type (rule R3).
 *
 * Built in Phase 1; see docs/phases/PHASE_1.md steps 1.15–1.19.
 */

// Phase 1 module plan:
//
//   src/main.ts          this file — opt-in UI, indicator, throttle, stop (1.15)
//   src/task-worker.ts   Web Worker: the task loop (1.17)
//   src/gpu/device.ts    adapter/device acquisition, capability reporting,
//                        device.lost recovery (1.16, 1.19)
//   src/gpu/kernel.ts    WGSL fetch + compile, buffer allocation against
//                        queried limits (K4), ≤250 ms chunked dispatch (R4/K1),
//                        TaskStats population (1.18)
//   src/net/control.ts   WebSocket client, handshake, lease loop (1.16)
//   src/net/peer.ts      WebRTC data plane — Phase 6
//   src/state.ts         worker lifecycle: Connecting → Registered →
//                        Benchmarking → Active ⇄ Throttled → Draining | Lost

const app = document.querySelector<HTMLElement>("#app");
if (app) {
  app.innerHTML = `
    <h1>p2pgpu</h1>
    <p>Scaffolding only — not yet implemented.</p>
    <p>See <code>docs/phases/PHASE_1.md</code> step 1.15.</p>
  `;
}

export {};
