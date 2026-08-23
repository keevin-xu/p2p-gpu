#!/usr/bin/env python3
"""6.15 — what fraction of peer connections NEEDED TURN relay.

    # arm 1, on the Mac, with a second machine joining from another network
    python3 tools/experiment_nat.py --label stun-only \
        --ice-server stun:stun.l.google.com:19302

    # arm 2, same session shape, TURN added
    python3 tools/experiment_nat.py --label with-turn \
        --ice-server stun:stun.l.google.com:19302 \
        --ice-server turn:USER:PASS@turn.example.com:3478

    # then
    python3 tools/experiment_nat.py --compare stun-only with-turn

────────────────────────────────────────────────────────────────────────────
THE MEASUREMENT IS A DIFFERENCE, NOT A READING (D-0102)

"Needed relay" is behavioural: **a connection needed TURN if it fails without
TURN and succeeds with it.** So the headline comes from two runs:

    relay-needed = connect_rate(with turn) - connect_rate(stun only)

`getSelectedCandidatePair()` — "which pair did ICE pick" — is a proxy for this
and is also native-only (D-0088), which would force a `#ifdef` into shared
transport code that R2 forbids. Worse, it structurally cannot count the
failures: a connection that never established has no selected pair to inspect,
and those are exactly the connections that needed relay.

`ice_relay` IS reported, as supporting detail. **It counts fetches where TURN
was REACHABLE, never where it was used** — ICE prefers host and srflx pairs. A
run showing `relay=0` with no TURN configured means the experiment was not run,
not that nothing needed relay, and this script refuses to report a ratio in
that case rather than printing a zero someone could quote.

────────────────────────────────────────────────────────────────────────────
IT TAKES TWO NETWORKS, AND LOOPBACK PROVES NOTHING

Both peers on one host connect over host candidates every time, so a local run
reports 100% success and measures the configuration, not the network. **Run it
with a second machine on a DIFFERENT network** — a phone on cellular is ideal,
because carrier CGNAT is the symmetric case that actually needs TURN.

The script does not enforce this; it records `workers` and the adapter of each,
so a single-machine run is visible as such afterwards.
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request

RUNNING = True


def stop(_s, _f):
    global RUNNING
    RUNNING = False


def metrics(port):
    try:
        with urllib.request.urlopen(f"http://localhost:{port}/metrics", timeout=5) as r:
            return json.load(r)
    except (urllib.error.URLError, TimeoutError, ConnectionError, json.JSONDecodeError):
        return None


def compare(out_dir, a_label, b_label):
    def load(lbl):
        path = os.path.join(out_dir, f"6.15-{lbl}.json")
        if not os.path.exists(path):
            print(f"missing {path}", file=sys.stderr)
            return None
        return json.load(open(path))

    a, b = load(a_label), load(b_label)
    if a is None or b is None:
        return 1

    def rate(m):
        n = m["ice_fetches"]
        return (m["ice_connected"] / n if n else None), n

    ra, na = rate(a)
    rb, nb = rate(b)
    print(f"\n{'arm':<14} {'attempts':>9} {'connected':>10} {'rate':>8}  "
          f"{'host':>5} {'srflx':>6} {'relay':>6}")
    for lbl, m in ((a_label, a), (b_label, b)):
        r, n = rate(m)
        print(f"{lbl:<14} {n:>9} {m['ice_connected']:>10} "
              f"{('%.1f%%' % (100 * r)) if r is not None else '   n/a':>8}  "
              f"{m['ice_host']:>5} {m['ice_srflx']:>6} {m['ice_relay']:>6}")

    if not na or not nb:
        print("\nNO RATIO: an arm had zero peer attempts, so there is nothing to "
              "difference. Both arms need a second machine actually fetching.")
        return 1
    if a["ice_relay"] == 0 and b["ice_relay"] == 0:
        print("\nNO RATIO: neither arm gathered a relay candidate, so TURN was "
              "never reachable in either. This measures the configuration, not "
              "the network — do not report it as 'no connection needed relay'.")
        return 1

    delta = rb - ra
    print(f"\n  relay-needed fraction = {100 * delta:+.1f}%"
          f"   ({b_label} minus {a_label})")
    if delta <= 0:
        print("  <= 0 means TURN did not improve connectivity in this sample. "
              "With few attempts that is as likely to be noise as a result — "
              "report the attempt counts beside it, always.")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--label", default="stun-only")
    ap.add_argument("--compare", nargs=2, metavar=("ARM_A", "ARM_B"))
    ap.add_argument("--build", default="build/native-release")
    ap.add_argument("--scene", default="scenes/dense.scene")
    ap.add_argument("--size", default="384x288")
    ap.add_argument("--spp", type=int, default=262144)
    ap.add_argument("--ice-server", action="append", default=[],
                    help="repeatable; STUN and/or TURN URLs")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--out", default="results")
    args = ap.parse_args()

    if args.compare:
        return compare(args.out, *args.compare)

    signal.signal(signal.SIGINT, stop)
    os.makedirs(args.out, exist_ok=True)
    cmd = [os.path.join(args.build, "coordinator"), "--seed-render", args.scene,
           "--render-size", args.size, "--render-spp", str(args.spp),
           "--port", str(args.port), "--log-level", "debug"]
    for u in args.ice_server:
        cmd += ["--ice-server", u]
    if not args.ice_server:
        print("WARNING: no --ice-server. Peers will offer only host candidates, "
              "so two machines on different networks cannot connect at all — "
              "this is what made cross-machine session 1 unusable.")

    log_path = os.path.join(args.out, f"6.15-{args.label}-coordinator.log")
    with open(log_path, "w") as lf:
        proc = subprocess.Popen(cmd, stdout=lf, stderr=subprocess.STDOUT)
        print(f"coordinator up on :{args.port}  (arm: {args.label})")
        print("join from the OTHER machine now; Ctrl-C when the render is done")
        last = None
        try:
            while RUNNING and proc.poll() is None:
                m = metrics(args.port)
                if m is not None:
                    last = m
                    print(f"\r  workers={m['workers']} tasks={m['tasks_by_state'][4]} "
                          f"ice: attempts={m['ice_fetches']} "
                          f"connected={m['ice_connected']} "
                          f"srflx={m['ice_srflx']} relay={m['ice_relay']}   ",
                          end="", flush=True)
                time.sleep(1.0)
        finally:
            if proc.poll() is None:
                proc.terminate()
                time.sleep(0.5)
            if proc.poll() is None:
                proc.kill()

    if last is None:
        print("\nno metrics collected")
        return 1
    snap = {k: last[k] for k in ("workers", "ice_fetches", "ice_connected",
                                 "ice_host", "ice_srflx", "ice_relay",
                                 "asset_from_peer", "asset_from_coordinator",
                                 "coordinator_asset_egress")}
    snap["ice_servers"] = args.ice_server
    # Which machines produced this, so the arm is attributable (D-0101).
    snap["adapters"] = [
        {"worker_id": w["worker_id"], "vendor": w.get("adapter_vendor", ""),
         "device": w.get("adapter_device", ""),
         "backend": w.get("adapter_backend", "")}
        for w in last.get("fleet", [])]
    path = os.path.join(args.out, f"6.15-{args.label}.json")
    with open(path, "w") as fh:
        json.dump(snap, fh, indent=2)
    print(f"\nwrote {path}")
    if snap["ice_fetches"] == 0:
        print("  ZERO peer attempts — nothing to measure. Either only one "
              "machine ever held the asset, or discovery never offered a peer.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
