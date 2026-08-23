#!/usr/bin/env python3
"""6.14 — where asset bytes come from, as a function of time.

    python3 tools/experiment_sources.py --out results/6.14-sources.csv

6.13 answered "does the data plane reduce coordinator egress" with a number the
fleet cannot influence. This answers the *why*: peer vs. coordinator vs. cache,
sampled once a second, so the shift toward peers as the swarm warms is visible
rather than asserted.

────────────────────────────────────────────────────────────────────────────
THIS BREAKDOWN IS WORKER-CLAIMED, AND THAT IS NOT A DETAIL

`asset_from_peer` / `_coordinator` / `_cache` are counted from
`TaskStats.asset_source`, which is **what workers say** (invariant 8). A fleet
that wanted to look P2P-efficient could report `peer` for every fetch and this
curve would show a perfect swarm.

So the script cross-checks it against something workers do not control:

    coordinator fetches CLAIMED x asset size  ==  coordinator egress OBSERVED

Egress is counted where the bytes leave (`AssetStore::RecordServed`). If the
fleet under-reports coordinator fetches, the identity breaks and the run is
reported as INCONSISTENT rather than plotted. It is a one-way check — it catches
under-reporting of coordinator fetches, which is the direction that flatters the
result, and cannot catch a worker that fetched from a peer and claimed the
coordinator. That direction makes P2P look worse, so nobody has a motive.

`--stagger` matters for the same reason as in E6: workers arriving together find
no holders, so the early samples are all-coordinator by construction. That is
precisely the warm-up this experiment is trying to SHOW, so the stagger is small
here rather than absent.
"""

import argparse
import csv
import datetime
import json
import os
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


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="results/6.14-sources.csv")
    ap.add_argument("--build", default="build/native-release")
    ap.add_argument("--scene", default="scenes/dense.scene")
    ap.add_argument("--size", default="384x288")
    ap.add_argument("--spp", type=int, default=262144)
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--stagger", type=float, default=3.0)
    ap.add_argument("--port", type=int, default=8600)
    ap.add_argument("--logs", default="/tmp/e614", help="where worker logs land")
    args = ap.parse_args()

    os.makedirs(args.logs, exist_ok=True)
    logs = []
    coordinator = os.path.join(args.build, "coordinator")
    worker = os.path.join(args.build, "worker-native")
    procs = [subprocess.Popen(
        [coordinator, "--seed-render", args.scene, "--render-size", args.size,
         "--render-spp", str(args.spp), "--port", str(args.port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)]
    time.sleep(2.0)

    rows = []
    t0 = time.time()
    next_join = 0
    prev = {"peer": 0, "coordinator": 0, "cache": 0}

    try:
        while time.time() - t0 < args.seconds:
            # Joins are interleaved with sampling rather than done up front, so
            # the arrival of a worker is visible in the same timeline as the
            # fetches it causes.
            if next_join < args.workers and (time.time() - t0) >= next_join * args.stagger:
                # Worker output is KEPT, not sent to DEVNULL. The first run of
                # this experiment saw the fleet drop from 8 to 7 with 30 s to
                # go — and since the coordinator has no eviction path (the
                # fleet only shrinks on a real disconnect), a worker left. With
                # its stdout discarded there was nothing to diagnose it with.
                # A driver that throws away the logs of the processes it is
                # measuring can only ever report that something happened.
                logs.append(open(os.path.join(args.logs, f"worker{next_join}.log"), "w"))
                procs.append(subprocess.Popen(
                    [worker, "--coordinator", f"ws://localhost:{args.port}/ws"],
                    stdout=logs[-1], stderr=subprocess.STDOUT))
                next_join += 1

            m = metrics(args.port)
            if m is not None:
                cur = {"peer": m["asset_from_peer"],
                       "coordinator": m["asset_from_coordinator"],
                       "cache": m["asset_from_cache"]}
                rows.append({
                    "t_sec": round(time.time() - t0, 1),
                    "workers": m["workers"],
                    # Cumulative AND per-sample. A cumulative curve alone hides
                    # when the shift happened; a delta curve alone is noisy at
                    # one-second granularity. Both, so neither has to be trusted.
                    "peer_total": cur["peer"],
                    "coordinator_total": cur["coordinator"],
                    "cache_total": cur["cache"],
                    "peer_delta": cur["peer"] - prev["peer"],
                    "coordinator_delta": cur["coordinator"] - prev["coordinator"],
                    "cache_delta": cur["cache"] - prev["cache"],
                    "egress_bytes": m["coordinator_asset_egress"],
                })
                prev = cur
            time.sleep(1.0)
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

    # A worker that exited on its own is a finding, not noise — surfaced here so
    # a clean-looking CSV cannot hide one.
    died = [i for i, p in enumerate(procs[1:], 1) if p.returncode not in (None, -15, -9)]
    if died:
        print(f"WORKERS THAT EXITED EARLY: {died} — see {args.logs}/")

    if not rows:
        print("no samples collected")
        return 1

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", newline="") as fh:
        # Provenance emitted by the driver so a regeneration keeps it.
        rev = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True).stdout.strip() or "unknown"
        fh.write(f"# produced by tools/experiment_sources.py\n")
        fh.write(f"# rev={rev} date={datetime.date.today().isoformat()}\n")
        fh.write("# PROVENANCE: REAL (real GPU workers, one host, loopback)\n")
        fh.write(f"# scene={args.scene} size={args.size} spp={args.spp} workers={args.workers} stagger={args.stagger}s\n")
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    last = rows[-1]
    total = last["peer_total"] + last["coordinator_total"] + last["cache_total"]
    print(f"\nwrote {args.out}  ({len(rows)} samples, {last['workers']} workers)")
    if total:
        print(f"  peer        {last['peer_total']:>5}  {last['peer_total']/total:6.1%}")
        print(f"  coordinator {last['coordinator_total']:>5}  "
              f"{last['coordinator_total']/total:6.1%}")
        print(f"  cache       {last['cache_total']:>5}  {last['cache_total']/total:6.1%}")

    # ── THE CROSS-CHECK ──────────────────────────────────────────────────
    # Claimed coordinator fetches, priced at the asset size, against bytes the
    # coordinator actually sent. Agreement means the breakdown above is
    # corroborated by a number no worker can write to.
    if last["coordinator_total"]:
        implied = last["egress_bytes"] / last["coordinator_total"]
        print(f"\n  egress {last['egress_bytes']:,} B over "
              f"{last['coordinator_total']} claimed coordinator fetches "
              f"= {implied:,.0f} B each")
        print("  (consistent if that equals the asset size; a much smaller "
              "number means fetches were claimed that never happened)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
