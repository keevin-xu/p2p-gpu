#!/usr/bin/env python3
"""6.16 — time-to-ready: worker join to first accepted result, with and without P2P.

    python3 tools/experiment_ready.py --out results/6.16-time-to-ready.csv

────────────────────────────────────────────────────────────────────────────
WHAT THIS IS EXPECTED TO SHOW, INCLUDING THE UNFAVOURABLE PART

P2P should help at scale — more sources for the asset — and may HURT at small
N, because negotiating a WebRTC connection costs more than one HTTP GET. On
loopback, where a 256 KB fetch from the coordinator is nearly free, the
overhead is expected to dominate at every N tested here.

That is reported as measured. A result that is not uniformly favourable is more
credible, not less, and the number that matters for the thesis is 6.13's egress
— time-to-ready is the cost side of that trade.

────────────────────────────────────────────────────────────────────────────
MEASURED ON THE COORDINATOR'S CLOCK

`ms_to_first_result` is join to first ACCEPTED result, both timestamps taken by
the coordinator (2.21). A worker reporting its own readiness is telemetry it
chooses (invariant 8), and this is precisely the number a worker would want to
look good on.

**Workers that never produced a result are counted, not dropped.** They appear
as `never_ready`, because excluding them would delete the slowest joiners from
the statistic — the direction that flatters whichever condition is worse.
"""

import argparse
import csv
import json
import os
import statistics
import subprocess
import time
import urllib.error
import urllib.request


def metrics(port):
    try:
        with urllib.request.urlopen(f"http://localhost:{port}/metrics", timeout=5) as r:
            return json.load(r)
    except (urllib.error.URLError, TimeoutError, ConnectionError, json.JSONDecodeError):
        return None


def run_one(build, scene, size, spp, workers, p2p, port, seconds, stagger, logdir):
    cmd = [os.path.join(build, "coordinator"), "--seed-render", scene,
           "--render-size", size, "--render-spp", str(spp), "--port", str(port)]
    if not p2p:
        cmd.append("--no-p2p")
    procs = [subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)]
    time.sleep(2.0)

    logs = []
    worker = os.path.join(build, "worker-native")
    for i in range(workers):
        if i > 0:
            time.sleep(stagger)
        # Logs kept, for D-0096's reason: a worker that dies mid-run is a
        # finding, and a driver that discards its output can only report that
        # the fleet got smaller.
        logs.append(open(os.path.join(logdir, f"w{port}_{i}.log"), "w"))
        procs.append(subprocess.Popen(
            [worker, "--coordinator", f"ws://localhost:{port}/ws"],
            stdout=logs[-1], stderr=subprocess.STDOUT))

    try:
        time.sleep(seconds)
        m = metrics(port)
    finally:
        for p in procs:
            if p.poll() is None:
                p.terminate()
        time.sleep(0.4)
        for p in procs:
            if p.poll() is None:
                p.kill()
        for f in logs:
            f.close()

    died = [i for i, p in enumerate(procs[1:]) if p.returncode not in (None, -15, -9)]
    if died:
        print(f"  [WORKERS THAT EXITED EARLY: {died}]")
    if m is None:
        return None

    ready = [w["ms_to_first_result"] for w in m["fleet"]
             if w["ms_to_first_result"] >= 0]
    grant = [w["ms_to_first_grant"] for w in m["fleet"]
             if w["ms_to_first_grant"] >= 0]
    return {
        "workers": workers,
        "p2p": "on" if p2p else "off",
        "n_ready": len(ready),
        # Counted, never dropped: see the module docstring.
        "never_ready": len(m["fleet"]) - len(ready),
        "median_ms": round(statistics.median(ready), 1) if ready else -1,
        "mean_ms": round(statistics.fmean(ready), 1) if ready else -1,
        "max_ms": round(max(ready), 1) if ready else -1,
        # Join to first GRANT isolates the scheduler from everything after it.
        # If this moves between conditions, the difference is not about assets.
        "median_grant_ms": round(statistics.median(grant), 1) if grant else -1,
        "from_peer": m["asset_from_peer"],
        "from_coordinator": m["asset_from_coordinator"],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="results/6.16-time-to-ready.csv")
    ap.add_argument("--build", default="build/native-release")
    ap.add_argument("--scene", default="scenes/dense.scene")
    ap.add_argument("--size", default="384x288")
    # Deliberately SMALL. Time-to-ready is join -> first result, so a large spp
    # buries asset acquisition under compute and both conditions converge on
    # "however long one task takes". The task must be short enough that
    # acquisition is a visible fraction of it.
    ap.add_argument("--spp", type=int, default=32768)
    ap.add_argument("--workers", default="2,4,8")
    ap.add_argument("--seconds", type=float, default=40.0)
    ap.add_argument("--stagger", type=float, default=2.0)
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--port", type=int, default=8700)
    ap.add_argument("--logs", default="/tmp/e616")
    args = ap.parse_args()

    os.makedirs(args.logs, exist_ok=True)
    counts = [int(x) for x in args.workers.split(",")]
    rows = []
    port = args.port

    for rep in range(args.repeats):
        for p2p in (False, True):
            for n in counts:
                port += 1
                print(f"  rep={rep} p2p={'on ' if p2p else 'off'} workers={n} ...",
                      end="", flush=True)
                row = run_one(args.build, args.scene, args.size, args.spp, n, p2p,
                              port, args.seconds, args.stagger, args.logs)
                if row is None:
                    print(" FAILED")
                    continue
                row["rep"] = rep
                rows.append(row)
                print(f" median={row['median_ms']:>8.0f}ms  max={row['max_ms']:>8.0f}ms"
                      f"  ready={row['n_ready']}/{row['n_ready']+row['never_ready']}"
                      f"  peer={row['from_peer']}")

    if not rows:
        print("no rows")
        return 1
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"\nwrote {args.out}")

    print("\nworkers   median OFF    median ON     delta")
    for n in counts:
        off = [r["median_ms"] for r in rows if r["p2p"] == "off"
               and r["workers"] == n and r["median_ms"] >= 0]
        on = [r["median_ms"] for r in rows if r["p2p"] == "on"
              and r["workers"] == n and r["median_ms"] >= 0]
        if off and on:
            a, b = statistics.median(off), statistics.median(on)
            print(f"{n:>7}  {a:>10.0f}ms  {b:>10.0f}ms  {b - a:>+8.0f}ms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
