# Phase 3 — Trust & Validation

**Objective:** detect workers returning incorrect results, without falsely accusing honest ones, at overhead meaningfully below 2×.

**Why now:** needs the Phase 2 mock harness to inject Byzantine behavior. Comes before the path tracer because `Exact` (Workload B) is the clean case to build validation against — float complications arrive in Phase 5 with a working validator already in place.

**Entry criteria:** G2 approved.

---

## Steps

### Comparison

- [ ] **3.1 — Determinism-class comparators.**
  Three implementations dispatched on the kernel's declared class:
  - `Exact` — bitwise equality
  - `Tolerant{rel_eps, abs_eps}` — elementwise `|a-b| <= abs_eps + rel_eps*|b|`, reporting max deviation
  - `Statistical` — distributional consistency test (interface now; body in Phase 5)

  Unit-test each, especially `Tolerant` boundary cases.

- [ ] **3.2 — Report deviation, not just verdict.**
  The comparator returns *how far apart* results were, not a boolean. Reputation must treat "off by 2 ULP" very differently from "completely different." This is what makes R6 survivable in practice.
  Use the Phase 0 step 0.16 cross-vendor divergence data to pick starting epsilons from measurement rather than guesswork.

- [ ] **3.3 — Log rejections at `warn` with full comparison detail.**
  Primary diagnostic for misdeclared determinism classes (`RISKS.md` §2).

### Replication

- [ ] **3.4 — Replica issuance.**
  Issue `replica_of` tasks. Enforce `PROTOCOL.md` §4 invariant 6: never grant a replica to a worker that computed the original or a sibling.

- [ ] **3.5 — Quorum logic.**
  k replicas agree (per determinism class) ⇒ accept. Disagreement ⇒ additional replicas up to a cap, then majority-decide; no majority ⇒ inconclusive, requeue with fresh workers.

- [ ] **3.6 — Naive 2× replication baseline.**
  A selectable policy. The control condition for E4 — without it there is nothing to claim improvement over.

### Reputation

- [ ] **3.7 — Reputation score.**
  Per-worker score in [0,1], updated per validated result, weighted by deviation magnitude (3.2). New workers start at a configurable prior.

- [ ] **3.8 — Adaptive replication policy.**
  Replication factor as a function of reputation: low-reputation workers replicated, established high-reputation workers not. BOINC's technique — the thing that pushes overhead from 2× toward 1×.

- [ ] **3.9 — Spot-checking.**
  Coordinator holds tasks with known-correct answers and injects them at a configurable rate, preferentially to unproven workers. Catches liars without full replication.

- [ ] **3.10 — Blacklist + probation.**
  Below threshold ⇒ blacklisted, `ErrorCode::Blacklisted` on connect. Probation path back after cooldown. Never permanent — a flaky overclock is not malice.

- [ ] **3.11 — Separate the fault classes cleanly.**
  Four different things, and only one may penalize reputation:
  - lease expiry ⇒ no penalty (worker vanished; normal)
  - checksum mismatch ⇒ no penalty (transport corruption)
  - **malformed/rejected frame ⇒ connection-level scoring and rate limiting, not task reputation**
  - wrong answer that passes checksum ⇒ penalty

  Test all four distinctly. Conflating them is the fastest way to blacklist your entire honest fleet.

- [ ] **3.12 — Rate limiting on rejected frames.**
  A peer sending repeated Verifier failures is either broken or probing. Escalating backoff, then disconnect with `ErrorCode::RateLimited`. Distinct from reputation — this is connection hygiene.

- [ ] **3.13 — Persist reputation.**
  Survives coordinator restart and worker reconnect via `resume_token`.

### Experiments

- [ ] **3.14 — E4 Byzantine detection experiment.**
  Sweep liar fractions 5/10/20/40% across three policies (none, naive 2×, adaptive+spot-check). Emit CSV: detection rate, false-positive rate, overhead factor, corrupted results accepted.

- [ ] **3.15 — E4 false-positive control run.**
  Zero liars, heterogeneous **real** fleet. Confirm zero false positives. **This is the R6 evidence** — it demonstrates the `Tolerant` comparator does not punish honest float divergence.

- [ ] **3.16 — Collusion sensitivity.**
  Run the case where liars coordinate to return *identical* wrong answers, defeating naive quorum. Document honestly. Full collusion resistance (randomized replica placement, SERENE-style) is stretch goal S4 — measuring the vulnerability without fixing it is a legitimate finding.

- [ ] **3.17 — Record the policy decision.**
  `DECISIONS.md` entry with the measured overhead/detection tradeoff and the chosen operating point, per DL7 (numbers, not adjectives).

---

## Deliverables

- Three determinism-class comparators with deviation reporting
- Replication with quorum and sibling-exclusion
- Reputation-weighted adaptive replication + spot-checking
- Blacklist with probation; separate connection-level rate limiting
- E4 charts including the false-positive control
- Documented collusion sensitivity

## Exit criteria

1. Byzantine workers detected at all injected fractions
2. Adaptive policy achieves comparable detection to naive 2× at overhead factor well below 2.0 — **report the number**
3. Zero false positives in the honest heterogeneous control run
4. The four fault classes are demonstrably distinct in their effects
5. Collusion behavior measured and documented
6. Reputation survives restart and reconnect

---

## → HUMAN GATE G3

Produce for review: detection-rate vs. overhead chart for all three policies at all liar fractions; false-positive control result; collusion finding; the chosen operating point with justification.

**The question being answered:** is the verification scheme sound, and does adaptive replication beat the naive baseline by a number you can quote?

**Stop here.**
