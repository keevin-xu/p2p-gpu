#!/usr/bin/env python3
"""Experiment driver — steps 2.23-2.26. Produces the CSVs in results/.

Every experiment here is computed from ONE instrument: the coordinator's
per-event CSV (`--events-csv`, see event_log.hpp). E1, E3, E5 and the 2.26
convergence plot are four analyses of the same data rather than four bespoke
harnesses — four instruments measuring one system is four chances for them to
disagree, and the disagreement would surface as a contradiction between plots
nobody could resolve afterwards.

Usage:
    python3 tools/experiment.py e1           # scaling curve
    python3 tools/experiment.py e3           # fault tolerance
    python3 tools/experiment.py e5           # straggler mitigation
    python3 tools/experiment.py convergence  # sizing convergence (2.26)
    python3 tools/experiment.py all

RUN UNDER native-release, NEVER under sanitizers (CONVENTIONS.md §8) — ASan
costs 2-10x and would be measuring the sanitizer.

THESE MEASURE SCHEDULING, NEVER THROUGHPUT. Mock workers compute real answers
but simulate device speed (D-0042, D-0049). No number here is evidence about
what a GPU can do.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import signal
import sqlite3
import re
import statistics
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build" / "native-release"
RESULTS = ROOT / "results"
COORD = BUILD / "coordinator"
MOCK = BUILD / "mock-worker"
PORT = 8080
BASE = f"http://localhost:{PORT}"

# Per-worker keyspace. Chosen so a run lasts ~30-60 s at the default
# --ms-per-mega-unit of 600 (~1.7e6 units/s per worker), which is long enough
# for the adaptive sizer to converge and short enough to sweep 8 fleet sizes.
UNITS_PER_WORKER = 25_000_000

# Per-experiment keyspace multipliers. E1 wants every point to run for about
# the same wall time; E5 and 2.26 want MANY tasks per worker, because a CDF
# built from two samples per worker is not a distribution and a convergence
# curve needs more points than the thing it is converging from.
E5_SCALE = 3
CONVERGENCE_SCALE = 8

# The fleet's real CPU is ONE core (D-0049), so this bounds every fleet size in
# the sweep. `mock-worker` warns if a run exceeds it; the sweep must stay silent.
MAX_WORKERS = 100


# ── process plumbing ─────────────────────────────────────────────────────

def wait_for_port(timeout=10.0):
    """Block until the coordinator answers, so a run never starts measuring
    before there is anything to measure."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            urllib.request.urlopen(f"{BASE}/health", timeout=0.5).read()
            return True
        except Exception:
            time.sleep(0.1)
    return False


def metrics():
    with urllib.request.urlopen(f"{BASE}/metrics", timeout=2.0) as r:
        return json.load(r)


def kill_quietly(proc):
    if isinstance(proc, list):
        for p in proc:
            kill_quietly(p)
        return
    if proc is None or proc.poll() is not None:
        return
    proc.send_signal(signal.SIGKILL)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass


def start_coordinator(units, events_csv, store=None, speculation=True, extra=(),
                      replication=None, verify=False):
    cmd = [
        str(COORD),
        "--seed-job", "brute_search_v1",
        "--seed-units", str(units),
        "--lease-ms", "20000",
        "--sweep-ms", "300",
        "--exit-when-complete",
        "--events-csv", str(events_csv),
        "--log-level", "warn",
    ]
    if store:
        cmd += ["--store", str(store)]
    if not speculation:
        cmd += ["--no-speculation"]
    if replication:
        cmd += ["--replication", replication]
    if verify:
        cmd += ["--verify-reference"]
    cmd += list(extra)
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not wait_for_port():
        kill_quietly(proc)
        raise RuntimeError("coordinator did not come up")
    return proc


# Virtual workers per mock PROCESS. The fleet is sharded across processes
# because a mock process polls every one of its workers on a single thread and
# computes real answers there (D-0049) — so one process is one core.
#
# MEASURED, not assumed: 100 workers in 1 process ran at 1.13e8 units/s; the
# same 100 in 4 processes ran at 1.51e8, i.e. **34% of the apparent scaling
# limit at N=100 was the measurement apparatus**. An E1 curve gathered from a
# single process would have reported the harness's ceiling as the
# coordinator's.
WORKERS_PER_PROCESS = 25


def start_mock(count, profile, seed, seconds, liar_fraction=None, collude=False):
    """Start however many processes it takes to host `count` workers.

    Returns a list, so callers must treat the fleet as a group.
    """
    procs = []
    remaining, shard = count, 0
    while remaining > 0:
        n = min(remaining, WORKERS_PER_PROCESS)
        cmd = [
            str(MOCK),
            "--count", str(n),
            "--chaos", profile,
            # Distinct per shard: identical seeds would make every shard the
            # same fleet, collapsing a 100-worker sweep into 4 copies of 25.
            "--seed", str(seed + 1000 * shard),
            "--seconds", str(seconds),
            "--coordinator", f"ws://localhost:{PORT}/ws",
        ]
        if liar_fraction is not None:
            cmd += ["--liar-fraction", str(liar_fraction)]
        if collude:
            cmd += ["--collude"]
        procs.append(subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL))
        remaining -= n
        shard += 1
    return procs


# ── the one instrument ───────────────────────────────────────────────────

def read_events(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            r["t_ms"] = int(r["t_ms"])
            r["unit_count"] = int(r["unit_count"])
            r["duration_ms"] = float(r["duration_ms"])
            r["predicted_ms"] = float(r["predicted_ms"])
            r["correction"] = float(r["correction"])
            r["speculative"] = int(r["speculative"])
            rows.append(r)
    return rows


def summarize(rows):
    """Aggregate one run. `wall_ms` spans first grant to last accept, NOT
    process lifetime — startup and teardown are not the system under test."""
    accepts = [r for r in rows if r["event"] == "accept"]
    grants = [r for r in rows if r["event"] == "grant"]
    if not accepts or not grants:
        return None
    t0 = min(r["t_ms"] for r in grants)
    t1 = max(r["t_ms"] for r in accepts)
    wall_ms = max(1, t1 - t0)
    units = sum(r["unit_count"] for r in accepts)
    durations = sorted(r["duration_ms"] for r in accepts)
    return {
        "tasks": len(accepts),
        "units": units,
        "wall_ms": wall_ms,
        "units_per_sec": 1000.0 * units / wall_ms,
        "p50_ms": statistics.median(durations),
        "p95_ms": durations[min(len(durations) - 1, int(0.95 * len(durations)))],
        "p99_ms": durations[min(len(durations) - 1, int(0.99 * len(durations)))],
        "max_ms": durations[-1],
        "expiries": sum(1 for r in rows if r["event"] == "expire"),
        "cancels": sum(1 for r in rows if r["event"] == "cancel"),
        "speculative_grants": sum(1 for r in grants if r["speculative"]),
        "wasted_units": sum(r["unit_count"] for r in rows if r["event"] == "cancel"),
    }


def audit_keyspace(db_path, expected_units):
    """Coverage audited against the DATABASE, not the process that wrote it.

    Returns (covered, gaps). This is the assertion E3 exists for: 'zero tasks
    lost' is not 'the job said it finished', it is 'every unit of the keyspace
    is accounted for exactly once'.
    """
    con = sqlite3.connect(db_path)
    rows = con.execute(
        "SELECT start_unit, unit_count FROM tasks "
        "WHERE replica_hi=0 AND replica_lo=0 ORDER BY start_unit"
    ).fetchall()
    con.close()
    expect, gaps = 0, 0
    for start, count in rows:
        if start != expect:
            gaps += 1
        expect = start + count
    return expect, gaps


# ── E1 — scaling curve ───────────────────────────────────────────────────

def e1(args):
    sizes = [1, 2, 4, 8, 16, 32, 64, MAX_WORKERS]
    out = RESULTS / "E1_scaling.csv"
    rows = []
    for n in sizes:
        # Keyspace scales WITH the fleet so every point runs for about the same
        # wall time. A fixed total would make N=1 take 100x longer than N=100
        # and put the two ends of the curve in different regimes.
        units = UNITS_PER_WORKER * n
        ev = RESULTS / f".e1_{n}.csv"
        print(f"  E1 N={n:3d} units={units:,} ...", end="", flush=True)
        coord = start_coordinator(units, ev)
        mock = start_mock(n, "default", seed=100 + n, seconds=600)
        try:
            coord.wait(timeout=900)
        except subprocess.TimeoutExpired:
            print(" TIMEOUT")
        kill_quietly(mock)
        kill_quietly(coord)
        s = summarize(read_events(ev))
        ev.unlink(missing_ok=True)
        if s is None:
            print(" no data")
            continue
        rows.append({"workers": n, **s})
        print(f" {s['units_per_sec']:.3g} units/s over {s['wall_ms']/1000:.1f}s")

    base = next((r["units_per_sec"] for r in rows if r["workers"] == 1), None)
    for r in rows:
        # Parallel efficiency: throughput(N) / (N x throughput(1)). 1.0 is
        # perfect scaling; the BEND is the interesting part (EVALUATION.md E1).
        r["efficiency"] = (r["units_per_sec"] / (r["workers"] * base)) if base else ""
    write_csv(out, rows)
    print(f"  -> {out}")


# ── E3 — fault tolerance ─────────────────────────────────────────────────

def e3(args):
    out = RESULTS / "E3_fault_tolerance.csv"
    rows = []
    fleet = 50
    for frac in (0.10, 0.30, 0.50, 0.80):
        victims = max(1, int(round(fleet * frac)))
        survivors = fleet - victims
        units = UNITS_PER_WORKER * fleet
        ev = RESULTS / f".e3_{int(frac*100)}.csv"
        db = RESULTS / f".e3_{int(frac*100)}.db"
        for p in (db, Path(str(db) + "-wal"), Path(str(db) + "-shm")):
            p.unlink(missing_ok=True)

        print(f"  E3 kill {int(frac*100)}% ({victims}/{fleet}) ...", end="", flush=True)
        coord = start_coordinator(units, ev, store=db)
        # TWO mock processes so a fraction of the fleet can be killed for real.
        # Killing the whole process is the honest model of a region going away
        # — SIGKILL, no goodbye, leases still held (R8).
        surv = start_mock(survivors, "default", seed=201, seconds=900)
        vict = start_mock(victims, "default", seed=202, seconds=900)

        killed_at = None
        deadline = time.time() + 900
        while time.time() < deadline and coord.poll() is None:
            try:
                m = metrics()
            except Exception:
                break
            done = m["units_total"] - m["units_remaining"]
            if killed_at is None and m["units_total"] > 0 and done >= 0.5 * m["units_total"]:
                kill_quietly(vict)
                killed_at = time.time()
            time.sleep(0.2)

        try:
            coord.wait(timeout=300)
        except subprocess.TimeoutExpired:
            pass
        kill_quietly(surv)
        kill_quietly(vict)
        kill_quietly(coord)

        s = summarize(read_events(ev)) or {}
        covered, gaps = audit_keyspace(db, units)
        # ZERO TASKS LOST is the assertion, and it is checked against the
        # database rather than against a log line saying the job finished.
        lost = units - covered
        rows.append({
            "kill_fraction": frac, "fleet": fleet, "killed": victims,
            "units_total": units, "units_covered": covered,
            "units_lost": lost, "keyspace_gaps": gaps,
            "expiries": s.get("expiries", ""), "tasks": s.get("tasks", ""),
            "wall_ms": s.get("wall_ms", ""),
        })
        print(f" covered={covered:,} lost={lost} gaps={gaps} expiries={s.get('expiries')}")
        for p in (ev, db, Path(str(db) + "-wal"), Path(str(db) + "-shm")):
            p.unlink(missing_ok=True)
    write_csv(out, rows)
    print(f"  -> {out}")


# ── E5 — straggler mitigation ────────────────────────────────────────────

def e5(args):
    out = RESULTS / "E5_stragglers.csv"
    cdf_out = RESULTS / "E5_completion_cdf.csv"
    rows, cdfs = [], {}
    fleet = 24
    units = UNITS_PER_WORKER * fleet * E5_SCALE

    # REPEATS, because the effect is small. A single pair of runs put the
    # control ahead once and behind once — anything claimed from n=1 here would
    # be a claim about scheduling noise. Three runs per condition is still few,
    # so the writeup reports the SPREAD and not just the mean.
    repeats = 3
    for spec in (False, True):
        label = "speculation" if spec else "control"
        for rep in range(repeats):
            ev = RESULTS / f".e5_{label}_{rep}.csv"
            print(f"  E5 {label:12s} run {rep+1}/{repeats} ...", end="", flush=True)
            coord = start_coordinator(units, ev, speculation=spec)
            # `stragglers`, not `heterogeneous`: adaptive sizing removes the slow
            # tail from an honest fleet by construction, so a heterogeneous fleet
            # has nothing for speculation to mitigate (see 2.15-2.18 results).
            # Seed varies per repeat so the runs are independent samples rather
            # than three copies of one fleet.
            mock = start_mock(fleet, "stragglers", seed=303 + rep, seconds=900)
            try:
                coord.wait(timeout=900)
            except subprocess.TimeoutExpired:
                print(" TIMEOUT", end="")
            kill_quietly(mock)
            kill_quietly(coord)

            evs = read_events(ev)
            s = summarize(evs)
            ev.unlink(missing_ok=True)
            if s is None:
                print(" no data")
                continue
            rows.append({"condition": label, "run": rep, **s})
            cdfs.setdefault(label, []).extend(
                r["duration_ms"] for r in evs if r["event"] == "accept")
            print(f" total={s['wall_ms']/1000:.1f}s p99={s['p99_ms']:.0f}ms "
                  f"wasted={s['wasted_units']:,}")

    # Pooled completion CDF per condition — the tail is the whole story.
    if cdfs:
        for v in cdfs.values():
            v.sort()
        n = max(len(v) for v in cdfs.values())
        cdf_rows = []
        for i in range(n):
            row = {"percentile": 100.0 * (i + 1) / n}
            for label, ds in cdfs.items():
                row[label + "_ms"] = ds[min(int(i * len(ds) / n), len(ds) - 1)]
            cdf_rows.append(row)
        write_csv(cdf_out, cdf_rows)
        print(f"  -> {cdf_out}")

    for label in ("control", "speculation"):
        sel = [r for r in rows if r["condition"] == label]
        if sel:
            tot = [r["wall_ms"] / 1000 for r in sel]
            print(f"  {label:12s} total {min(tot):.1f}-{max(tot):.1f}s "
                  f"(mean {statistics.mean(tot):.1f}s)")
    write_csv(out, rows)
    print(f"  -> {out}")


# ── E4 — Byzantine detection (3.14-3.16) ────────────────────────────────

def _e4_run(fleet, units, liar_fraction, policy, collude, seed, tag):
    """One cell of the sweep. Returns the row, or None if it produced no data."""
    ev = RESULTS / f".e4_{tag}.csv"
    log = RESULTS / f".e4_{tag}.log"
    coord = subprocess.Popen(
        [str(COORD), "--seed-job", "brute_search_v1", "--seed-units", str(units),
         "--lease-ms", "20000", "--sweep-ms", "300", "--exit-when-complete",
         "--verify-reference", "--replication", policy,
         "--events-csv", str(ev), "--log-level", "info"],
        stdout=open(log, "w"), stderr=subprocess.STDOUT)
    if not wait_for_port():
        kill_quietly(coord)
        return None
    mock = start_mock(fleet, "byzantine_10pct", seed, 900,
                      liar_fraction=liar_fraction, collude=collude)
    try:
        coord.wait(timeout=900)
    except subprocess.TimeoutExpired:
        pass
    kill_quietly(mock)
    kill_quietly(coord)

    text = log.read_text(errors="replace")
    row = {"policy": policy, "liar_fraction": liar_fraction, "collude": int(collude)}

    # The two numbers the whole phase turns on. `mismatched` is lies TOLD;
    # `accepted_wrong` is lies that SURVIVED validation. Only the second is a
    # statement about whether the validator works.
    m = re.search(r"mismatched=(\d+).*ACCEPTED: checked=(\d+) wrong=(\d+)", text)
    if not m:
        return None
    row["lies_submitted"] = int(m.group(1))
    row["accepted_checked"] = int(m.group(2))
    row["accepted_wrong"] = int(m.group(3))

    evs = read_events(ev)
    accepts = [r for r in evs if r["event"] == "accept"]
    grants = [r for r in evs if r["event"] == "grant"]
    tasks = len({r["task_id"] for r in accepts})
    row["tasks"] = tasks
    row["grants"] = len(grants)

    # OVERHEAD FACTOR = GRANTS per accepted task. 1.0 is no replication, 2.0 is
    # the naive baseline, and this is the number 3.17 has to quote.
    #
    # Counted from `grant`, NOT `accept`: an accept event is logged once per
    # task when it is finally accepted, so accepts-per-task is 1.0 by
    # construction and reported a flat 1.0 even for fixed2x, whose wall clock
    # had visibly doubled. Every dispatch to a worker is a grant; that is what
    # replication costs.
    row["overhead_factor"] = round(len(grants) / tasks, 3) if tasks else 0.0
    row["detected"] = row["lies_submitted"] - row["accepted_wrong"]
    row["detection_rate"] = (round(row["detected"] / row["lies_submitted"], 3)
                             if row["lies_submitted"] else 1.0)
    row["blacklisted"] = len(re.findall(r"blacklisted worker=", text))
    row["inconclusive"] = len(re.findall(r"validation_inconclusive", text))
    row["wall_s"] = round(summarize(evs)["wall_ms"] / 1000, 1) if accepts else 0

    ev.unlink(missing_ok=True)
    log.unlink(missing_ok=True)
    return row


def e4(args):
    out = RESULTS / "E4_byzantine.csv"
    fleet = 20
    units = UNITS_PER_WORKER * fleet
    rows = []
    for policy in ("none", "fixed2x", "adaptive"):
        for frac in (0.05, 0.10, 0.20, 0.40):
            print(f"  E4 {policy:8s} liars={frac:.0%} ...", end="", flush=True)
            r = _e4_run(fleet, units, frac, policy, False, 700,
                        f"{policy}_{int(frac*100)}")
            if r is None:
                print(" no data")
                continue
            rows.append(r)
            print(f" told={r['lies_submitted']:3d} survived={r['accepted_wrong']:3d} "
                  f"detect={r['detection_rate']:.0%} overhead={r['overhead_factor']}")
    write_csv(out, rows)
    print(f"  -> {out}")


def e4_control(args):
    """3.15 — ZERO liars on a heterogeneous fleet. THE R6 evidence.

    0.16 measured honest workers differing by up to 5 ULP across vendors. If the
    Tolerant comparator or reputation punishes that, this run shows it as
    blacklists and rejections with nobody lying.
    """
    out = RESULTS / "E4_false_positive_control.csv"
    fleet = 20
    units = UNITS_PER_WORKER * fleet
    rows = []
    for policy in ("fixed2x", "adaptive"):
        print(f"  E4-control {policy:8s} liars=0 ...", end="", flush=True)
        r = _e4_run(fleet, units, 0.0, policy, False, 800, f"ctl_{policy}")
        if r is None:
            print(" no data")
            continue
        rows.append(r)
        print(f" wrong_accepted={r['accepted_wrong']} blacklisted={r['blacklisted']} "
              f"inconclusive={r['inconclusive']}")
    write_csv(out, rows)
    print(f"  -> {out}")


def e4_collusion(args):
    """3.16 — liars returning IDENTICAL wrong answers, defeating naive quorum."""
    out = RESULTS / "E4_collusion.csv"
    fleet = 20
    units = UNITS_PER_WORKER * fleet
    rows = []
    for frac in (0.20, 0.40):
        for collude in (False, True):
            tag = f"col_{int(frac*100)}_{int(collude)}"
            print(f"  E4-collusion liars={frac:.0%} collude={collude} ...",
                  end="", flush=True)
            r = _e4_run(fleet, units, frac, "fixed2x", collude, 900, tag)
            if r is None:
                print(" no data")
                continue
            rows.append(r)
            print(f" told={r['lies_submitted']:3d} survived={r['accepted_wrong']:3d} "
                  f"detect={r['detection_rate']:.0%}")
    write_csv(out, rows)
    print(f"  -> {out}")


# ── 2.26 — sizing convergence ────────────────────────────────────────────

def convergence(args):
    out = RESULTS / "2.26_sizing_convergence.csv"
    fleet = 12
    units = UNITS_PER_WORKER * fleet * CONVERGENCE_SCALE
    ev = RESULTS / ".conv.csv"
    print("  2.26 convergence ...", end="", flush=True)
    coord = start_coordinator(units, ev)
    mock = start_mock(fleet, "stragglers", seed=404, seconds=900)
    try:
        coord.wait(timeout=900)
    except subprocess.TimeoutExpired:
        pass
    kill_quietly(mock)
    kill_quietly(coord)

    evs = read_events(ev)
    ev.unlink(missing_ok=True)
    # Per worker, in order: task index vs the ratio the EWMA is chasing.
    seen, rows = {}, []
    for r in evs:
        if r["event"] != "accept" or r["predicted_ms"] <= 0:
            continue
        w = r["worker_id"]
        seen[w] = seen.get(w, 0) + 1
        rows.append({
            "worker_id": w,
            "task_index": seen[w],
            "predicted_ms": r["predicted_ms"],
            "actual_ms": r["duration_ms"],
            "ratio": r["duration_ms"] / r["predicted_ms"],
            "correction": r["correction"],
        })
    write_csv(out, rows)
    finals = {}
    for r in rows:
        finals[r["worker_id"]] = r["correction"]
    if finals:
        vals = sorted(finals.values())
        print(f" {len(finals)} workers, final correction "
              f"min={vals[0]:.2f} median={statistics.median(vals):.2f} max={vals[-1]:.2f}")
    print(f"  -> {out}")


# ── plumbing ─────────────────────────────────────────────────────────────

def write_csv(path, rows):
    if not rows:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("experiment",
                    choices=["e1", "e3", "e5", "convergence", "e4", "e4_control",
                             "e4_collusion", "all"])
    args = ap.parse_args()

    for exe in (COORD, MOCK):
        if not exe.exists():
            sys.exit(f"missing {exe} — build native-release first")

    RESULTS.mkdir(exist_ok=True)
    todo = (["e1", "e3", "e5", "convergence", "e4", "e4_control", "e4_collusion"]
            if args.experiment == "all" else [args.experiment])
    for name in todo:
        print(f"[{name}]")
        globals()[name](args)


if __name__ == "__main__":
    main()
