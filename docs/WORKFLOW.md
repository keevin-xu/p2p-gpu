# Agent Workflow

**Read this at the start of every session, before writing code.** It defines how the implementing agent (Claude Code CLI) must operate on this repo.

---

## 1. Session start checklist

Run this every time, in order:

1. Read `CLAUDE.md` (hard rules R1–R10).
2. Read the **Current state** section at the bottom of `CLAUDE.md` to find the active phase.
3. Read `docs/phases/PHASE_<n>.md` for that phase.
4. Skim the last 5 entries of `docs/DECISIONS.md` — they contain recent context that may not be in code yet.
5. Run the build and test suite. **Know whether you are starting from green or red.** If red, fixing that is the session's first task.
6. State in one line: current phase, next step number, and whether the tree is green.

Do not begin implementing before completing 1–6.

---

## 2. The implementation loop

For each numbered step in the active phase file:

```
  read step
    ↓
  does it require a DECISION entry?  ──yes──►  append to DECISIONS.md FIRST
    ↓ no                                              ↓
  does it violate a hard rule R1–R10?  ──yes──►  STOP. Ask the human.
    ↓ no
  implement
    ↓
  write/extend tests for it
    ↓
  run: cmake --build build/native-debug && ctest --preset native-debug
       (ASan + UBSan are on in this preset — a sanitizer finding is a failure)
    ↓
  touched the protocol or asset path? ──► run the fuzzer for a few minutes too
    ↓
  green? ──no──► fix. Do not proceed with a red tree.
    ↓ yes
  commit (see §5)
    ↓
  tick the step's checkbox in the phase file
    ↓
  next step
```

**Never batch multiple steps into one commit.** Each step is independently reviewable and revertable. This matters because a human is reviewing at gates and needs to see the progression.

---

## 3. When to STOP and ask the human

Stop immediately, do not work around, in these cases:

- **A hard rule (R1–R10) appears to require violation.** These encode reasoning that is not obvious from the code. If a rule seems wrong, say why and wait — do not unilaterally override.
- **A roadmap gate is reached** (R10). Produce the gate evidence and wait for written approval.
- **A decision meets the "requires human sign-off" criteria** in `DECISIONS.md` §3.
- **A new dependency is needed** that is not already in the `CONVENTIONS.md` §10 baseline. Propose it with justification; do not just add it to `vcpkg.json`.
- **A hard rule R11 constraint is inconvenient.** Wanting a `memcpy` or a `reinterpret_cast` in boundary code is exactly the moment to stop and ask, not to make an exception.
- **The phase file's step is ambiguous or appears wrong** given what you have learned. Say what you learned and propose an amendment.
- **A measurement contradicts a documented assumption.** E.g. measured throughput is 10× off from `RESEARCH.md` estimates. This is important information, not an inconvenience to route around.
- **Scope is growing.** If a step is turning into three steps, stop and propose splitting it rather than silently expanding.

When stopping, produce: what you were doing, what blocked you, 2–3 concrete options with tradeoffs, and your recommendation.

---

## 4. What you may do without asking

To avoid over-blocking — proceed freely on:

- Anything explicitly listed as a step in the active phase file.
- Refactoring within a module that does not change a public interface or the protocol.
- Adding tests, fixtures, logging, error-message improvements.
- Fixing a failing test, a `clang-tidy` warning, or a sanitizer finding.
- Adding a fuzz corpus seed.
- Correcting a typo or inaccuracy in a doc, **including** updating the commands table in `CLAUDE.md` when a command becomes real.
- Choosing between two implementations that are genuinely equivalent in consequence (naming, iteration style, file split within a module).

---

## 5. Commits

One commit per completed step. Format:

```
<phase>.<step> <imperative summary>

<why, if not obvious>

Decisions: D-00NN            (omit if none)
Gate: none | evidence-for-G<n>
```

Example:

```
2.7 Add lease expiry sweep with requeue

Sweep runs on a 1s loop timer rather than per-task timers; at 10k
tasks the timer-per-task approach was measured at ~40MB of timer
state for no latency benefit.

Decisions: D-0031
```

Never commit: generated `flatc` headers, secrets, large binary fixtures, a red tree, or a sanitizer suppression without a `DECISIONS.md` entry.

**Fuzz corpus is an exception to the binary-fixtures rule** — seeds are tiny and every crash-triggering input belongs in `fuzz/corpus/` as a regression seed.

---

## 6. Documentation is part of "done"

A step is not complete until:

- Code exists and is tested.
- Tests pass under ASan/UBSan; `clang-tidy` is clean.
- If it changed the protocol → `protocol/p2pgpu.fbs` **and** `PROTOCOL.md` updated in the same commit, plus a fuzz corpus seed for the new shape.
- If it changed module boundaries or a state machine → `ARCHITECTURE.md` updated.
- If it made a consequential choice → `DECISIONS.md` entry exists (which per §2 was written *before* the code).
- If it added or changed a command → `CLAUDE.md` commands table updated.
- The step's checkbox in the phase file is ticked.

Docs drifting from code is the primary failure mode of a handoff repo like this. Treat a drifted doc as a bug of equal severity to a failing test.

---

## 7. Session end

Before ending a session, always:

1. Update **Current state** at the bottom of `CLAUDE.md`: phase, next step number, tree status.
2. If anything is in a half-finished state, write a `## In progress` note there describing exactly what is incomplete and what the next action is.
3. Ensure the tree is committed and green, or clearly note that it is not and why.

Assume the next session starts with **zero memory** of this one. Everything needed must be on disk.

---

## 8. Working style for this repo

- **Measure before optimizing.** This project's whole thesis is that intuitions about distributed performance are wrong. Do not tune anything without a number. `timestamp-query` and `TaskStats` exist for this.
- **Measure under `native-release`, never under sanitizers.** ASan's ~2× slowdown corrupts every timing number. Correctness under sanitizers, performance under release, never confused (`CONVENTIONS.md` §8).
- **Prefer the boring implementation.** SQLite over a queue service, one loop timer over per-task timers, coordinator-brokered peers over a DHT. Complexity must be earned by a measurement.
- **Write portable code by default.** A platform `#ifdef` outside `src/worker-core/platform/` is a defect (R2). If a portable file seems to need one, the seam interface is wrong — fix the interface.
- **The mock harness is not optional infrastructure.** Most of the project's evidence comes from it. When tempted to skip it to move faster, remember that it *is* the fast path — it turns week-long fleet experiments into 30-second test runs.
- **Failure paths are the product.** In a normal CRUD app, the happy path is the feature. Here, lease expiry, straggler re-issue, Byzantine detection, and device loss *are* the features. Give them first-class tests, not afterthought ones.
- **Do not add features to look impressive.** The impressive part is the measurements in `EVALUATION.md`. A feature that produces no measurement is probably scope creep.
