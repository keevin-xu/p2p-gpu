# Phase 2 — Scheduling, Leases & the Mock Harness

**Objective:** many workers, adaptive sizing, lease-based fault tolerance, speculative re-execution — and the mock harness that generates most of the project's evidence.

**Why now:** the mock harness unlocks every later measurement. Building it here turns week-long fleet experiments into 30-second test runs for the rest of the project.

**Entry criteria:** G1 approved.

---

## Steps

### Mock harness (build this first)

- [ ] **2.1 — `src/mock-worker/` skeleton.**
  Full protocol client, never touches a GPU. Sleeps a computed duration, returns canned results. CLI11: `--count`, `--coordinator`, `--seed`, `--chaos <profile>`.

- [ ] **2.2 — N virtual workers in one process.**
  N coroutines/tasks on one event loop, not N processes. Independent identity, capabilities, and simulated throughput drawn from a configurable distribution (default: log-normal, ~20× spread).

- [ ] **2.3 — Injectable behaviors.**
  `slow` · `dies_mid_task` · `returns_garbage` · `lies_probabilistically(p)` · `high_latency(ms)` · `never_renews_lease` · `duplicate_submit` · `flaps`.
  Independently toggleable and seeded. These are the experimental instruments for Phases 3–7 — keep their semantics clean and orthogonal.

- [ ] **2.4 — Chaos profiles.**
  Named presets in a config file: `default`, `heterogeneous`, `byzantine_10pct`, `flaky_network`, `mass_departure`. Referenced by name from experiments so runs are reproducible.

- [ ] **2.5 — A hostile profile.**
  `malformed_frames` — sends truncated headers, oversized `fb_len`, garbage FlatBuffers, valid frames for tasks it does not hold. The coordinator must reject every one and stay up.
  This is the live-fire counterpart to the Phase 1 fuzzer: fuzzing proves the parser is safe in isolation, this proves the *server* is safe under sustained abuse.

### Leases

- [ ] **2.6 — Lease manager.**
  Grant with absolute `expires_at_ms` on the coordinator clock — never trust workers (`PROTOCOL.md` §5). Renew on `Progress{request_renew}`. Track per-worker held leases.

- [ ] **2.7 — Expiry sweep.**
  One timer on the event loop, **not** per-task timers (`CONVENTIONS.md` §4). Expired ⇒ task returns to `Queued`, **no reputation penalty** (R8, absence ≠ malice).

- [ ] **2.8 — Heartbeat + worker loss detection.**
  Missed heartbeats ⇒ worker `Lost` ⇒ all its leases released immediately.

- [ ] **2.9 — Clean drain path.**
  `Release` and `Goodbye` release leases immediately. Tab-hidden and user-stop both route here.

- [ ] **2.10 — Idempotent submission.**
  Duplicate `ResultHeader` for the same `task_id`: first accepted wins, later discarded silently (not an error). Test with the `duplicate_submit` behavior — this matters once speculation exists.

### Adaptive sizing

- [ ] **2.11 — Join-time benchmark.**
  Coordinator sends `BenchmarkRequest`; worker runs the Phase 0 calibration kernel for `target_ms`; returns a normalized score. Mock workers return a synthetic score matching their simulated throughput.

- [ ] **2.12 — Sizer.**
  `work_units = target_duration_ms × score × correction_factor`, target 1–3 s, clamped, never exceeding lease duration (`ARCHITECTURE.md` §7).

- [ ] **2.13 — EWMA correction.**
  Track predicted vs. actual duration per worker; correct future grants. Emit both to metrics so convergence is plottable (G2 evidence).

- [ ] **2.14 — Throttle handling.**
  `Throttle{level}` scales that worker's effective score. Applied without argument (R7) — the user's setting is authoritative.

### Scheduling

- [ ] **2.15 — Pull-based grant with `max_tasks`.**
  Workers may hold a small backlog to hide RTT. Jittered backoff on empty queue to avoid thundering herd (`RISKS.md` §2).

- [ ] **2.16 — Cache-affinity assignment.**
  Track which `input_ref` hashes each worker has cached; prefer tasks reusing them. Stub until Phase 5 provides real assets, but wire the hook now.

- [ ] **2.17 — Speculative re-execution.**
  At ≥95% job completion, re-issue outstanding tasks to idle fast workers. First result wins; loser gets `Revoke{SpeculativeLoser}` and stops promptly. Track wasted work as a metric (E5 reports it as the cost side).

- [ ] **2.18 — Job completion detection.**
  Handle the interaction of speculation, replication, and expiry correctly. Subtle — test directly rather than assuming.

### Persistence & observability

- [ ] **2.19 — SQLite store.**
  Jobs, tasks, workers, reputation. Hot path in memory; SQLite as durability + recovery source (D-0006). Use parameterized statements exclusively — never string-concatenate SQL, even from "internal" values.

- [ ] **2.20 — Crash recovery.**
  Coordinator restart ⇒ reload from SQLite ⇒ in-flight leases treated as expired ⇒ job resumes. Test by killing the process mid-job.

- [ ] **2.21 — Metrics + SSE feed.**
  Counters and histograms: queue depth, fleet size, per-worker throughput, task state counts, sizing prediction error, wasted work, rejected-frame count.

- [ ] **2.22 — Dashboard.**
  `web/dashboard/` — static HTML + minimal JS consuming the SSE feed. Live fleet table, task state breakdown, throughput chart. Read-only.
  Keep it dumb; it is a debugging tool that happens to demo well. No build step, no framework.

### Experiments

- [ ] **2.23 — E1 scaling experiment.**
  Sweep worker count 1 → 100, emit CSV with throughput and efficiency. Seeded and reproducible. **Run under `native-release`, never under sanitizers** (`CONVENTIONS.md` §8).

- [ ] **2.24 — E3 fault-tolerance experiment.**
  Kill 10/30/50/80% of the fleet at 50% job completion. Assert **zero tasks lost**. Emit timeline CSV and recovery times.

- [ ] **2.25 — E5 straggler experiment.**
  Heterogeneous fleet, with and without speculation. Emit completion CDF, p50/p95/p99, total time, and wasted work.

- [ ] **2.26 — Sizing convergence plot.**
  Predicted vs. actual task duration over time per worker. Should converge; if not, the EWMA needs tuning and that is a finding worth a `DECISIONS.md` entry.

---

## Deliverables

- Mock harness with all injectable behaviors, chaos profiles, and the hostile profile
- Lease manager with expiry sweep, heartbeats, clean drain
- Adaptive sizer with measured convergence
- Speculative re-execution with wasted-work accounting
- SQLite persistence with tested crash recovery
- Dashboard
- CSVs + charts for E1, E3, E5

## Exit criteria

1. 50 mock workers complete a job with correct results
2. Killing 30% mid-job loses **zero** tasks; job completes; recovery time measured
3. Speculation measurably reduces p99 and total job time; wasted work quantified
4. Sizing converges — plot shows prediction error shrinking
5. Coordinator restart mid-job recovers and completes
6. **Coordinator survives the `malformed_frames` profile indefinitely with zero crashes**
7. All Phase 2 experiments reproducible from a seed
8. Every failure path in `ARCHITECTURE.md` §6 has a T2 integration test

---

## → HUMAN GATE G2

Produce for review: E1 scaling curve with efficiency line, E3 fault-tolerance timeline at all kill fractions, E5 straggler CDF with/without speculation, sizing convergence plot, dashboard screenshot, and the hostile-profile soak result.

**The question being answered:** does the scheduler work, and is the mock harness good enough to generate the rest of the project's evidence?

Scrutinize: where does the E1 efficiency curve bend, and is the cause understood? A perfectly linear curve to N=100 means the experiment is too easy to be informative.

**Stop here.**
