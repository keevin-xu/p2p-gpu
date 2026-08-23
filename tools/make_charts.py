#!/usr/bin/env python3
"""Regenerate every chart from the committed CSVs — steps 7.4 and 7.6.

    python3 tools/make_charts.py                 # all charts -> results/charts/
    python3 tools/make_charts.py --list          # what would be produced

Reads ONLY committed CSVs. It runs no experiments and starts no processes, so
it is safe on a busy machine and its output depends on nothing but the files in
`results/`. Re-running the experiments is a different command
(`tools/experiment.py`), deliberately — 7.6 asks for two, and conflating them
means a "regenerate the charts" invocation could silently overwrite the data it
was supposed to be drawing.

────────────────────────────────────────────────────────────────────────────
EVERY TITLE CARRIES REAL / MIXED / SIMULATED (7.4)

Not a footnote, not a caption, not the filename — the title, where a chart
lifted into a slide keeps it. `results/README.md` defines them:

    REAL       physically distinct GPUs
    MIXED      some real GPUs, some mock workers
    SIMULATED  mock workers only

The label lives in `PROVENANCE` below, one entry per chart, and a chart with no
entry **refuses to render**. That is the point: the failure mode this guards
against is a new chart quietly shipping unlabelled, and defaulting to
"SIMULATED" would be a guess presented as a fact.

**E1, E3, E4, E5 are SIMULATED.** Mock workers compute real answers but simulate
device speed (D-0042, D-0049), so no number in them is evidence about what a GPU
can do. Presenting that scaling curve as measured would be the single most
damaging thing this project could do to its own credibility.
"""

import argparse
import csv
import os
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

OUT_DIR = os.path.join("results", "charts")

# One entry per chart. A chart absent from here does not render (see module doc).
PROVENANCE = {
    "e1_scaling": "SIMULATED",
    "e3_fault_tolerance": "SIMULATED",
    "e4_byzantine": "SIMULATED",
    "e4_collusion": "SIMULATED",
    "e5_completion_cdf": "SIMULATED",
    "e6_egress": "REAL",          # real GPU workers on one host
    "e6_sources": "REAL",
    "e6_time_to_ready": "REAL",
}


def rows(name):
    path = os.path.join("results", name)
    if not os.path.exists(path):
        return None
    with open(path) as fh:
        return list(csv.DictReader(r for r in fh if not r.startswith("#")))


def figure(key, title):
    """A titled figure, or None when the data is missing.

    The label is prefixed rather than appended so it survives truncation in a
    slide deck or a thumbnail — the end of a long title is the first thing to
    be cut, and that is exactly the word that must not be.
    """
    if key not in PROVENANCE:
        raise SystemExit(f"{key}: no PROVENANCE entry — refusing to render an "
                         f"unlabelled chart (7.4)")
    fig, ax = plt.subplots(figsize=(7.2, 4.4))
    ax.set_title(f"[{PROVENANCE[key]}]  {title}", fontsize=11, loc="left")
    ax.grid(alpha=0.25, linewidth=0.6)
    return fig, ax


def save(fig, key):
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, f"{key}.png")
    fig.tight_layout()
    fig.savefig(path, dpi=140)
    plt.close(fig)
    print(f"  wrote {path}")
    return path


def chart_e1():
    r = rows("E1_scaling.csv")
    if not r:
        return 0
    n = [int(x["workers"]) for x in r]
    ups = [float(x["units_per_sec"]) for x in r]
    fig, ax = figure("e1_scaling", "E1 — throughput vs. worker count")
    ax.plot(n, ups, "o-", label="measured")
    # Ideal line anchored at the SMALLEST N, not a fit. A fitted line can be
    # dragged up by the same superlinearity it is meant to expose.
    ax.plot(n, [ups[0] * (k / n[0]) for k in n], "--", alpha=0.6, label="linear from N=1")
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xlabel("workers")
    ax.set_ylabel("units/sec")
    ax.legend(fontsize=9)
    save(fig, "e1_scaling")
    return 1


def chart_e3():
    r = rows("E3_fault_tolerance.csv")
    if not r:
        return 0
    kf = [100 * float(x["kill_fraction"]) for x in r]
    lost = [int(x["units_lost"]) for x in r]
    gaps = [int(x["keyspace_gaps"]) for x in r]
    fig, ax = figure("e3_fault_tolerance", "E3 — units lost and keyspace gaps vs. fleet kill")
    ax.bar([k - 1.5 for k in kf], lost, width=3, label="units lost")
    ax.bar([k + 1.5 for k in kf], gaps, width=3, label="keyspace gaps")
    ax.set_xlabel("% of fleet killed")
    ax.set_ylabel("count")
    ax.legend(fontsize=9)
    save(fig, "e3_fault_tolerance")
    return 1


def chart_e4():
    r = rows("E4_byzantine.csv")
    if not r:
        return 0
    by = defaultdict(list)
    for x in r:
        by[x["policy"]].append((100 * float(x["liar_fraction"]),
                                100 * float(x["detection_rate"]),
                                float(x["overhead_factor"])))
    fig, ax = figure("e4_byzantine", "E4 — detection rate vs. liar fraction")
    for pol, pts in sorted(by.items()):
        pts.sort()
        ax.plot([p[0] for p in pts], [p[1] for p in pts], "o-",
                label=f"{pol} (x{pts[0][2]:.2f} work)")
    ax.set_xlabel("% of fleet lying")
    ax.set_ylabel("detection rate (%)")
    ax.set_ylim(-5, 105)
    ax.legend(fontsize=9)
    save(fig, "e4_byzantine")

    c = rows("E4_collusion.csv")
    if not c:
        return 1
    fig, ax = figure("e4_collusion", "E4 — collusion defeats detection")
    for flag, style in ((False, "o-"), (True, "s--")):
        pts = sorted((100 * float(x["liar_fraction"]),
                      100 * float(x["detection_rate"]))
                     for x in c if (x["collude"].lower() == "true") == flag)
        if pts:
            ax.plot([p[0] for p in pts], [p[1] for p in pts], style,
                    label="colluding" if flag else "independent")
    ax.set_xlabel("% of fleet lying")
    ax.set_ylabel("detection rate (%)")
    ax.set_ylim(-5, 105)
    ax.legend(fontsize=9)
    save(fig, "e4_collusion")
    return 2


def chart_e5():
    r = rows("E5_completion_cdf.csv")
    if not r:
        return 0
    p = [float(x["percentile"]) for x in r]
    fig, ax = figure("e5_completion_cdf", "E5 — task completion CDF, speculation on vs. off")
    ax.plot([float(x["control_ms"]) for x in r], p, label="control")
    ax.plot([float(x["speculation_ms"]) for x in r], p, label="speculation")
    ax.set_xlabel("completion time (ms)")
    ax.set_ylabel("percentile")
    ax.legend(fontsize=9)
    save(fig, "e5_completion_cdf")
    return 1


def chart_e6():
    made = 0
    r = rows("E6_egress.csv")
    if r:
        fig, ax = figure("e6_egress", "E6 — coordinator egress vs. fleet size")
        for arm, style in (("off", "o--"), ("on", "s-")):
            pts = sorted((int(x["workers"]), int(x["egress_bytes"]) / 1024.0)
                         for x in r if x["p2p"] == arm)
            if pts:
                ax.plot([p[0] for p in pts], [p[1] for p in pts], style,
                        label=f"data plane {arm}")
        ax.set_xlabel("workers")
        ax.set_ylabel("coordinator asset egress (KiB)")
        ax.legend(fontsize=9)
        save(fig, "e6_egress")
        made += 1

    s = rows("6.14-sources.csv")
    if s:
        fig, ax = figure("e6_sources", "6.14 — asset source over time, and egress stays flat")
        t = [float(x["t_sec"]) for x in s]
        ax.plot(t, [int(x["peer_total"]) for x in s], label="peer")
        ax.plot(t, [int(x["coordinator_total"]) for x in s], label="coordinator")
        ax.plot(t, [int(x["cache_total"]) for x in s], label="cache")
        ax.set_xlabel("seconds")
        ax.set_ylabel("cumulative fetches")
        ax2 = ax.twinx()
        ax2.plot(t, [int(x["egress_bytes"]) / 1024.0 for x in s], "k:",
                 label="egress (KiB)")
        ax2.set_ylabel("coordinator egress (KiB)")
        ax.legend(fontsize=9, loc="upper left")
        ax2.legend(fontsize=9, loc="lower right")
        save(fig, "e6_sources")
        made += 1

    t2r = rows("6.16-time-to-ready.csv")
    if t2r:
        # The COST side of E6, and it is charted for that reason. A P2P result
        # shown without it is a result with its trade-off cropped out.
        fig, ax = figure("e6_time_to_ready", "6.16 — time-to-ready is WORSE with the data plane")
        for arm, style in (("off", "o--"), ("on", "s-")):
            agg = defaultdict(list)
            for x in t2r:
                if x["p2p"] == arm and float(x["median_ms"]) >= 0:
                    agg[int(x["workers"])].append(float(x["median_ms"]))
            pts = sorted((k, sorted(v)[len(v) // 2]) for k, v in agg.items())
            if pts:
                ax.plot([p[0] for p in pts], [p[1] for p in pts], style,
                        label=f"data plane {arm}")
        ax.set_xlabel("workers")
        ax.set_ylabel("median join to first result (ms)")
        ax.legend(fontsize=9)
        save(fig, "e6_time_to_ready")
        made += 1
    return made


CHARTS = (chart_e1, chart_e3, chart_e4, chart_e5, chart_e6)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()
    if args.list:
        for k, v in sorted(PROVENANCE.items()):
            print(f"  {v:<10} {k}.png")
        return 0

    total = sum(fn() for fn in CHARTS)
    print(f"\n{total} chart(s) in {OUT_DIR}/")
    if total == 0:
        print("no CSVs found — run tools/experiment.py first", file=sys.stderr)
        return 1
    # E7 needs >=3 vendors and only Apple has ever been measured on this
    # machine, so there is deliberately no E7 chart. Announced rather than
    # silently absent: a missing chart is easy to mistake for an oversight.
    print("NOT CHARTED: E7 (needs >=3 vendors; step 7.3) and E2 (a table, not "
          "a series — see results/5.21-5.22-e2-utilization.md)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
