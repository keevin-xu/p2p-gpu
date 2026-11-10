# Primers

A textbook set for learning this project deeply enough to explain it under
questioning. Eight documents, ~5,700 lines.

**Start with `00-MAIN.md`.** Every other primer assumes it and cross-references
back to it rather than repeating shared material.

| # | Primer | The question it answers |
|---|---|---|
| **0** | [`00-MAIN.md`](00-MAIN.md) | How do you get useful GPU work out of machines you don't own, can't trust, and can't rely on staying online? |
| 1 | [`01-scheduling.md`](01-scheduling.md) | How do you hand work to nodes that vanish mid-task and run at 20× different speeds? |
| 2 | [`02-trust.md`](02-trust.md) | How do you verify results when workers may lie and honest GPUs disagree in the last few bits? |
| 3 | [`03-gpu-layer.md`](03-gpu-layer.md) | What does it take to run a kernel on a GPU you've never seen without hanging the user's desktop? |
| 4 | [`04-portability.md`](04-portability.md) | How do you compile one worker to native and browser without it becoming two programs? |
| 5 | [`05-data-plane.md`](05-data-plane.md) | Can the fleet distribute its own data so coordinator bandwidth stops growing with fleet size? |
| 6 | [`06-trust-boundary.md`](06-trust-boundary.md) | You chose a memory-unsafe language for a public endpoint. Defend that. |
| 7 | [`07-measurement.md`](07-measurement.md) | How do you stop a measurement from telling you what you want to hear? |

Each primer has: the organising question, a how-to-use table, progressive
parts, a vocabulary table, drills that cannot be answered by skimming, and a
where-to-go-deeper section with external sources and repo files in reading
order.

---

## Reading orders

**Cold start, never seen the project** — 0 → 1 → 2 → 5. That is the
distributed-systems spine.

**Interview tomorrow** — `00-MAIN.md` Part H (what is proven and what is not),
then `07-measurement.md` in full, then the "what went wrong" part of each
domain primer: 1 §E, 2 §G, 3 §F, 4 §E, 5 §D–F, 6 §F.

**Going into the code** — `00-MAIN.md` Part C is the code map: what each
directory is for, which file to open first, and where the security checks live.

**Asked about GPUs specifically** — 3 → 4.

**Asked about security specifically** — 6 → 2.

---

## Relationship to `docs/PRIMER.md`

`docs/PRIMER.md` is a separate, older document and is **not** superseded. It
teaches GPU, build-system and distributed-systems vocabulary from zero — what a
kernel is, what a workgroup is, what CMake does.

This set assumes that material. If a term is unfamiliar here, look there first.

---

## Conventions used throughout

- Every claim is grounded in this repo — real file paths, real line numbers,
  real measured numbers.
- Measurements carry their provenance: **REAL** or **SIMULATED**. Never
  blurred.
- Opinions are labelled as opinions.
- Where a decision had a real alternative, the alternative is named along with
  why it lost.
- **Mistakes are included deliberately.** They are the most useful material
  here, because they are what gets probed — and because a negative result
  stated plainly is worth more than a positive one that can be dismantled.
