# Phase 7 — Evaluation, Deploy & Writeup

**Objective:** produce the deliverable. The code has existed to enable this phase.

**Entry criteria:** G6 approved.

> **`RISKS.md` R-D:** the borrowed non-Apple hardware is required again here and it is non-negotiable — success criterion #1, E7 across ≥3 vendors, and the E1 real-hardware overlay all depend on it. Step 4.19's one-command join package is what makes this a short favor rather than a setup visit.

---

## Steps

### Complete the experiment set

- [ ] **7.1 — Re-run all seven experiments on final code.**
  Earlier runs were against intermediate versions. E1–E7 re-run against the shipped build, seeded and reproducible, under `native-release` — **never under sanitizers** (`CONVENTIONS.md` §8).

- [ ] **7.2 — E1 with real hardware overlay.**
  Plot the `REAL` small-N curve on the same axes as the `SIMULATED` large-N curve. **Their agreement in the overlapping range is what licenses the simulated numbers** — call this out explicitly.

- [ ] **7.3 — E7 across ≥3 vendors.**
  Complete the device/vendor/backend/peak/achieved/%-of-peak table. Include `shader-f16` and `subgroups` effects where measured.
  Sanity check: 10–20% of peak is expected. Far below means a kernel bug worth finding before shipping.

- [ ] **7.4 — Label every chart REAL / MIXED / SIMULATED.**
  In the title, not a footnote. Non-negotiable.

- [ ] **7.5 — Commit raw CSVs to `results/`.**
  With seeds and conditions in the header. Every number quoted in prose must be traceable to a committed CSV.

- [ ] **7.6 — Reproduction script.**
  One command regenerates every chart from committed CSVs; a second re-runs the experiments from scratch.

### Deploy

- [ ] **7.7 — Deploy the coordinator.**
  Static binary in a slim container on Fly.io or Railway. WebSocket support and a persistent volume for SQLite.
  Ship the **release** build with hardening flags on (`CONVENTIONS.md` §2), not the sanitizer build.

- [ ] **7.8 — Serve the worker page with COOP/COEP headers.**
  Verify cross-origin isolation actually applies in the deployed environment (`RISKS.md` §1 — this breaks late if deferred).

- [ ] **7.9 — TURN in production.**
  Cloudflare TURN or self-hosted coturn, verified from a network behind symmetric NAT.

- [ ] **7.10 — Verify R7 compliance on the deployed site.**
  Explicit opt-in before any GPU work, visible contribution indicator, working throttle, instant stop. Test as a first-time visitor in a clean browser profile.

- [ ] **7.11 — Real multi-machine run.**
  ≥3 physically distinct GPUs from ≥2 vendors on the deployed instance. Capture it.

- [ ] **7.12 — Load-test the deployed coordinator.**
  Point the mock swarm at production, including the `malformed_frames` profile. Find where it breaks and report the number rather than discovering it during a demo.

### Writeup

- [ ] **7.13 — README.**
  Structure:
  1. What it is, in three sentences
  2. **The arithmetic-intensity constraint** — the derivation, prominently. This is the intellectual core (D-0001).
  3. Architecture diagram, including the one-`worker-core`-two-targets property
  4. The measurements, with charts
  5. Honest limitations
  6. How to run it

- [ ] **7.14 — State both taxes explicitly.**
  WebGPU vs. native ≈ 6–8×; distribution overhead ≈ 5–20%. Include the ~8-node break-even against one native CUDA machine. Backed by your own E7 numbers, not just cited literature.

- [ ] **7.15 — Write the security section.**
  Short, factual: the threat model (anonymous unauthenticated workers), the R11 posture, schema-driven deserialization, the fuzzing corpus and exec counts, sanitizer CI, the hostile soak result, and the malicious-peer experiment.
  This is the direct answer to *"your threat model says workers are hostile — what is your parser's memory-safety story?"* Having it written down means you never have to improvise it.

- [ ] **7.16 — Write the limitations section.**
  What was not measured. What remains unknown. Where simulation substituted for real hardware. Collusion vulnerability from 3.16. Single-coordinator scaling limit from D-0006.
  **This section is a credibility asset, not a liability.** A reader who finds a limitation you already named trusts everything else more.

- [ ] **7.17 — Use the approved framing.**
  `PROJECT_OVERVIEW.md` §5 verbatim. Never "decentralized AI training", never "alternative to AWS", never "blockchain".

- [ ] **7.18 — Demo video.**
  Two to three minutes: workers joining, image converging, a fleet kill and recovery, the dashboard under load.

- [ ] **7.19 — Review `DECISIONS.md` as a narrative.**
  Read start to finish. It should tell a coherent story — including the C++ pivot (D-0008) and why D-0004 was superseded. Fill gaps where a decision was clearly made but never logged. Do not rewrite history; append clarifying entries (DL2).

- [ ] **7.20 — Doc consistency pass.**
  Every doc matches shipped code. `PROTOCOL.md` vs `protocol/p2pgpu.fbs`. `ARCHITECTURE.md` target names vs actual CMake targets. `CLAUDE.md` commands table actually runs. Stale docs in a handoff repo are worse than absent ones (`RISKS.md` R-E).

- [ ] **7.21 — Interview prep notes.**
  A short private doc: the §2 derivation reproducible on a whiteboard; answers to "why browsers not native", "why P2P not a server", "what's the real use case", "why C++ and how do you handle memory safety"; the weak points from `RESEARCH.md` §9 with responses.

---

## Deliverables

- Seven experiments, re-run, labeled, with committed CSVs and a reproduction script
- Deployed demo with COOP/COEP, TURN, and verified opt-in
- Real run across ≥3 GPUs from ≥2 vendors
- README with derivation, charts, security section, and limitations
- Demo video
- Coherent decision log

## Exit criteria

Matches `PROJECT_OVERVIEW.md` §4 success criteria:

1. Job completes across ≥3 distinct GPUs from ≥2 vendors
2. All three worker targets run the same job against the same kernels
3. 30% fleet kill loses zero work
4. Byzantine detection measured; adaptive beats naive 2× by a quotable number
5. Egress flat with P2P, linear without
6. All seven measurements exist as reproducible charts
7. README states honestly what was real vs. simulated
8. Security posture documented with evidence, not assertions

---

## → HUMAN GATE G7 — Ship it

**The question being answered:** does this stand up to a skeptical technical reader?

Review as an adversary. Pick the three claims a strong interviewer would attack first and confirm each has a committed CSV behind it. One of those three will be the memory-safety question — check that 7.15 actually answers it.

---

## After G7

Stretch goals only, from `ROADMAP.md`. Do not start any before G7 approval.

- **S2** — Third workload with a different determinism class
- **S3** — WebTransport control plane as a comparison
- **S4** — Collusion-resistant randomized replica placement, measured against Phase 3
- **S5** — Parser isolation: run deserialization in a sandboxed subprocess (seccomp), measured overhead vs. the in-process design
