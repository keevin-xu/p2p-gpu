#!/usr/bin/env python3
"""E6 — coordinator asset egress vs. worker count, with and without P2P (6.13).

**The measurement that makes "P2P" in the project name defensible rather than
decorative.** The claim is that coordinator egress stays FLAT as the fleet grows
while the no-P2P control grows LINEARLY, and this is what tests it.

    python3 tools/experiment_e6.py --out results/E6_egress.csv

────────────────────────────────────────────────────────────────────────────
WHY EGRESS AND NOT THE WORKERS' OWN REPORTS

`TaskStats.asset_source` says where each worker BELIEVES its asset came from,
and a worker chooses what to report (invariant 8). A fleet could claim peer
fetches for everything.

`coordinator_asset_egress` is what the coordinator ACTUALLY SENT, counted where
the bytes leave, over both transports. It cannot be inflated or deflated by a
worker, and it contradicts a false claim directly: a fleet reporting peer
fetches while egress grows linearly is reporting something the coordinator can
disprove. So egress is the headline and the breakdown is the explanation.

────────────────────────────────────────────────────────────────────────────
WHAT WOULD MAKE THIS MEASUREMENT A LIE

The run must last long enough for every worker to actually need the asset — a
worker that joins after the job finishes fetches nothing and makes the fleet
look cheaper than it is. So the job is sized to outlive every join, and the CSV
records `workers_seen`: a run where that is below `--workers` is not comparable
to one where it is not, and hiding it would be the easiest way to manufacture a
flat line.

Joins are STAGGERED, which the first version got wrong — see `run_one`. Starting
every worker at once means nobody holds the asset yet, so the swarm has nothing
to share and both conditions measure the same thing.
"""

import argparse
import csv
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request


def metrics(port):
    try:
        with urllib.request.urlopen(f"http://localhost:{port}/metrics", timeout=5) as r:
            return json.load(r)
    except (urllib.error.URLError, TimeoutError, ConnectionError, json.JSONDecodeError):
        return None


def run_one(build, scene, size, spp, workers, p2p, port, seconds, stagger):
    """One condition. Returns a dict of measurements, or None if it did not run."""
    coordinator = os.path.join(build, "coordinator")
    worker = os.path.join(build, "worker-native")

    cmd = [coordinator, "--seed-render", scene, "--render-size", size,
           "--render-spp", str(spp), "--port", str(port)]
    if not p2p:
        cmd.append("--no-p2p")

    procs = [subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)]
    time.sleep(2.0)

    # ── STAGGERED, AND THE FIRST VERSION GOT THIS WRONG ──────────────────
    #
    # Workers were started TOGETHER, to avoid a bias where an early worker
    # finishes the job before a later one needs the asset. That reasoning was
    # right about the bias and wrong about the experiment: **when every worker
    # starts at once, nobody holds the asset yet**, so every peer list is empty
    # and all of them fetch from the coordinator. Measured: identical egress in
    # both conditions, `peer=0` throughout.
    #
    # Simultaneous arrival is the WORST CASE for a swarm and is not what a
    # volunteer fleet looks like — workers join continuously, and each new one
    # finds an established set of holders. So joins are staggered, and the bias
    # the original reasoning worried about is handled the honest way instead:
    # the job is sized to outlive every join, and `workers_seen` records whether
    # the coordinator actually engaged them all.
    for i in range(workers):
        if i > 0:
            time.sleep(stagger)
        procs.append(subprocess.Popen(
            [worker, "--coordinator", f"ws://localhost:{port}/ws"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))

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
        time.sleep(0.3)

    if m is None:
        return None
    fetched = m["asset_from_peer"] + m["asset_from_coordinator"]
    return {
        "workers": workers,
        "p2p": "on" if p2p else "off",
        "egress_bytes": m["coordinator_asset_egress"],
        "from_peer": m["asset_from_peer"],
        "from_coordinator": m["asset_from_coordinator"],
        "from_cache": m["asset_from_cache"],
        "fetches": fetched,
        # How many workers the coordinator actually engaged. If this is below
        # `workers`, the run is not comparable — recorded rather than hidden.
        "workers_seen": m["workers"],
        "tasks_done": sum(m["tasks_by_state"][4:5]) if m.get("tasks_by_state") else 0,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="results/E6_egress.csv")
    ap.add_argument("--build", default="build/native-release")
    # The DENSE scene, not the default one: default.scene's BVH is ~6 KB, and a
    # saving measured on 6 KB is indistinguishable from noise in a fleet that
    # also exchanges control frames.
    ap.add_argument("--scene", default="scenes/dense.scene")
    ap.add_argument("--size", default="384x288")
    ap.add_argument("--spp", type=int, default=262144)
    ap.add_argument("--workers", default="1,2,4,6")
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--stagger", type=float, default=4.0,
                    help="seconds between worker joins; 0 starts them together, "
                         "which is the worst case for a swarm and measures "
                         "nothing (see run_one)")
    ap.add_argument("--port", type=int, default=8500)
    args = ap.parse_args()

    counts = [int(x) for x in args.workers.split(",")]
    rows = []
    port = args.port

    for p2p in (False, True):
        for n in counts:
            port += 1
            label = "on " if p2p else "off"
            print(f"  p2p={label} workers={n} ...", end="", flush=True)
            row = run_one(args.build, args.scene, args.size, args.spp, n, p2p,
                          port, args.seconds, args.stagger)
            if row is None:
                print(" FAILED (no metrics)")
                continue
            rows.append(row)
            print(f" egress={row['egress_bytes']:>9,}B  "
                  f"peer={row['from_peer']} coord={row['from_coordinator']} "
                  f"cached={row['from_cache']}  seen={row['workers_seen']}")

    if not rows:
        print("no rows collected", file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"\nwrote {args.out}")

    # The comparison, printed so the result is visible without opening the file.
    print("\nworkers   egress OFF      egress ON     ratio")
    off = {r["workers"]: r for r in rows if r["p2p"] == "off"}
    on = {r["workers"]: r for r in rows if r["p2p"] == "on"}
    for n in counts:
        if n in off and n in on:
            a, b = off[n]["egress_bytes"], on[n]["egress_bytes"]
            print(f"{n:>7}  {a:>12,}  {b:>12,}  {a / b if b else float('inf'):>7.2f}x")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
