# Evaluation Plan

**These measurements are the deliverable.** The code exists to produce them. A feature that yields no measurement is probably scope creep (`WORKFLOW.md` §8).

Each experiment: reproducible from a seed, emits CSV to `results/`, renders to a chart, and is described in the README with its conditions stated.

**All experiments run under the `native-release` preset.** Never under ASan/UBSan — the ~2× slowdown would silently corrupt every timing number here (`CONVENTIONS.md` §8). Record the preset in the CSV header alongside the seed.

---

## The honesty rule

Every chart must be labeled with what produced it:

- **REAL** — physically distinct GPUs
- **MIXED** — some real GPUs, some mock workers
- **SIMULATED** — mock workers only

Never present a simulated scaling curve as though it were measured on real hardware. Stating the distinction plainly reads as rigor; being caught blurring it destroys the credibility of every other number in the project. Put the label in the chart title, not a footnote.

---

## E1 — Scaling curve

**Question:** does aggregate throughput scale with fleet size, and where does it bend?

- x: worker count, 1 → 100 (mock) and 1 → 5 (real GPUs)
- y: aggregate work-units/sec
- Second series: parallel efficiency = `throughput(N) / (N × throughput(1))`

**What good looks like:** near-linear to the point where coordinator RTT or queue contention binds. **The bend is the interesting part** — identify what causes it and say so. A perfectly linear curve to N=100 means the experiment was too easy.

Run both `SIMULATED` (large N) and `REAL` (small N), plotted together so the reader can see they agree in the overlapping range. That agreement is what licenses the simulated numbers.

---

## E2 — Utilization breakdown

**Question:** where does worker time actually go?

Stacked area or bar, per task, from `TaskStats`: `gpu_ms` / `transfer_ms` / `idle_ms`.

Break out by workload — brute-force search should be ~99% GPU; the path tracer's number is the direct empirical test of the D-0001 accumulation thesis.

**What good looks like:** ≥85% GPU time for the path tracer with accumulation on. Include a control run with accumulation **disabled** to show the contrast — that pair of bars is the single most persuasive chart in the project.

---

## E3 — Fault tolerance

**Question:** what happens when a third of the fleet vanishes?

Protocol: start a job on N=50 mock workers, kill 30% at t=50% completion.

- Timeline: tasks completed vs. wall clock, with the kill marked
- Metrics: tasks lost (**must be zero**), time-to-recovery, lease-expiry-to-requeue latency
- Sweep kill fractions: 10%, 30%, 50%, 80%

Also run: coordinator restart mid-job → SQLite recovery, job completes.

**What good looks like:** zero lost work at every kill fraction, and recovery time bounded by the lease duration.

---

## E4 — Byzantine detection

**Question:** does reputation-weighted replication beat naive replication?

Inject liar fractions of 5%, 10%, 20%, 40% using the mock harness's `lies_probabilistically(p)` behavior. Compare three policies:

1. No verification (baseline — shows corruption rate)
2. Naive 2× replication
3. Reputation-weighted adaptive replication + spot-checks

Plot: **detection rate vs. replication overhead factor** for each. Also report false-positive rate.

**What good looks like:** policy 3 achieves comparable detection at an overhead factor meaningfully below 2.0, reproducing BOINC's adaptive-replication result. Report the overhead factor as a number, e.g. "1.18× at 10% liars for 99.2% detection."

**Also required:** a run with zero liars but a heterogeneous *real* fleet, showing the `Tolerant` comparator produces **zero** false positives on honest float divergence. This is the R6 evidence.

---

## E5 — Straggler mitigation

**Question:** how much does speculative re-execution buy?

Deliberately heterogeneous fleet (mock workers with a 20× throughput spread, plus a few `slow` behaviors). Job completion time with and without speculative re-issue.

- Plot the completion CDF for both — the tail is the whole story
- Report p50, p95, p99 task completion and total job time
- Report wasted work (duplicated dispatches) as the cost side

**What good looks like:** meaningful p99/total-time reduction for single-digit-percent wasted work. Report both; a speedup with 40% wasted work is not a win.

---

## E6 — P2P egress

**Question:** is the P2P claim real?

Distribute a large asset (path tracer scene BVH) to N workers, with and without the data plane enabled.

- x: worker count · y: total coordinator egress bytes
- Also: peer-fetch success rate, coordinator-fallback rate, TURN-relay rate

**What good looks like:** flat vs. linear, with the flat line's residual explained (the seed copies the coordinator must serve, plus fallbacks). Report what fraction of peers needed TURN — the literature says 10–20%; your number is a real datapoint.

---

## E7 — WebGPU efficiency

**Question:** how much of the GPU are we actually getting?

Achieved GFLOP/s as a fraction of device theoretical peak, across ≥3 GPU vendors, for both kernels.

- Table: device / vendor / backend / peak / achieved / % of peak
- Include the effect of `shader-f16` and `subgroups` where available

**What good looks like:** in the 10–20% band, consistent with published WebGPU-vs-CUDA results (~11–17%). Landing there is confirmation the kernels are reasonable; landing at 2% means there is a kernel bug worth finding.

This chart also supports the honest framing in `PROJECT_OVERVIEW.md` §5 — it is the empirical basis for the "~6–8× WebGPU tax" claim and the ~8-node break-even.

---

---

## E8 — Trust-boundary hardening (reported, not charted)

**Question:** is the memory-safety posture demonstrated or merely asserted?

Not a chart, but it belongs in the deliverable alongside the seven experiments, and it is the evidence behind Phase 7 step 7.15.

Report:
- Fuzz campaign: target, total execs, coverage reached, corpus size, crashes found and fixed
- Sanitizer CI: which sanitizers, which suites, current status
- Hostile soak: `malformed_frames` rate, duration, coordinator uptime, honest-throughput degradation
- Malicious-peer experiment (Phase 6 step 6.17): detection and recovery
- R11 boundary audit result (step 4.16)

**What good looks like:** concrete numbers in every row. "I used FlatBuffers" is not evidence; "40M execs, 12 corpus entries, 2 crashes found and fixed in Phase 1, ASan+UBSan green on every commit, coordinator survived 6 hours of malformed traffic at 2k frames/sec" is.

---

## Reporting checklist (Phase 7)

- [ ] All seven experiments produce charts from committed, seeded scripts
- [ ] Every chart carries a REAL / MIXED / SIMULATED label
- [ ] Every chart was produced under `native-release`, and the CSV header says so
- [ ] Raw CSVs committed under `results/`
- [ ] README states the break-even analysis and both taxes explicitly
- [ ] README states what was **not** measured and what remains unknown
- [ ] E8 hardening evidence written up with real numbers
- [ ] Every number quoted in prose is traceable to a committed CSV
