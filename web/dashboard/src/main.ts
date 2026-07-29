/**
 * p2pgpu dashboard — read-only live view of the fleet.
 *
 * Consumes the coordinator's SSE metrics feed. Shows fleet size, per-worker
 * throughput, task state breakdown, queue depth, validation stats, and egress.
 *
 * Keep it simple. This is a debugging tool that happens to demo well — resist
 * turning it into an application. It writes nothing; the coordinator is the
 * only component that makes decisions (rule R1).
 *
 * From Phase 5 it also renders the progressively converging path-traced image,
 * which is the project's demo (step 5.14).
 *
 * Built in Phase 2, step 2.21; see docs/phases/PHASE_2.md.
 */

const app = document.querySelector<HTMLElement>("#app");
if (app) {
  app.innerHTML = `
    <h1>p2pgpu dashboard</h1>
    <p>Scaffolding only — not yet implemented.</p>
    <p>See <code>docs/phases/PHASE_2.md</code> step 2.21.</p>
  `;
}

export {};
