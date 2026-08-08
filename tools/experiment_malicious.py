#!/usr/bin/env python3
"""6.17 — a peer that serves WRONG BYTES for a valid hash.

    python3 tools/experiment_malicious.py

────────────────────────────────────────────────────────────────────────────
THE SETUP IS ARRANGED SO THE VICTIM CANNOT AVOID THE ATTACKER

The attacker joins FIRST and is left alone until it has completed a task, which
is what makes the coordinator advertise it as a holder (D-0095). Only then does
the victim join. At that moment the attacker is the *only* known holder, so the
victim's peer list contains exactly one entry and there is no honest peer to get
lucky with.

Without that sequencing the experiment proves nothing: with two holders the
victim may simply pick the honest one and complete, and the run would look like
a pass while the defence was never exercised.

The attacker flips a byte in the MIDDLE of each chunk it serves. Every length
is right, every index is right, the chunk count is right — **only BLAKE3 can
catch it.** Corrupting a length would be caught by cheaper checks and would not
test the path this experiment is about.

────────────────────────────────────────────────────────────────────────────
WHAT COUNTS AS A PASS

1. the victim REJECTS the bytes (hash mismatch, logged)
2. the victim FALLS BACK to the coordinator rather than losing the task
3. the victim COMPLETES tasks afterwards
4. the coordinator serves one extra copy — the measurable cost of the attack

All four, or it is not a pass. (1) alone is what the code did before D-0097:
it detected the attack perfectly and then dropped the task, which is the denial
of service the attacker wanted.
"""

import argparse
import json
import os
import re
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


def wait_for(fn, timeout, what):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.5)
    print(f"  TIMED OUT waiting for {what}")
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default="build/native-release")
    ap.add_argument("--scene", default="scenes/dense.scene")
    ap.add_argument("--spp", type=int, default=32768)
    ap.add_argument("--port", type=int, default=8800)
    ap.add_argument("--logs", default="/tmp/e617")
    ap.add_argument("--seconds", type=float, default=40.0)
    args = ap.parse_args()

    os.makedirs(args.logs, exist_ok=True)
    atk_log = os.path.join(args.logs, "attacker.log")
    vic_log = os.path.join(args.logs, "victim.log")
    procs = []

    coord = subprocess.Popen(
        [os.path.join(args.build, "coordinator"), "--seed-render", args.scene,
         "--render-size", "384x288", "--render-spp", str(args.spp),
         "--port", str(args.port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    procs.append(coord)
    time.sleep(2.0)

    worker = os.path.join(args.build, "worker-native")
    try:
        # ── the attacker, alone, until it is an advertised holder ──────────
        with open(atk_log, "w") as af, open(vic_log, "w") as vf:
            procs.append(subprocess.Popen(
                [worker, "--coordinator", f"ws://localhost:{args.port}/ws",
                 "--serve-corrupt-assets"], stdout=af, stderr=subprocess.STDOUT))
            print("  attacker joined (serving corrupted assets)")

            if not wait_for(lambda: (metrics(args.port) or {}).get("tasks_by_state",
                                                                   [0, 0, 0, 0, 0])[4] > 0,
                            60, "the attacker to complete a task (advertisement)"):
                return 1
            before = metrics(args.port)
            print(f"  attacker is now a holder; coordinator egress so far "
                  f"{before['coordinator_asset_egress']:,} B")

            # ── the victim, whose only peer option is the attacker ─────────
            procs.append(subprocess.Popen(
                [worker, "--coordinator", f"ws://localhost:{args.port}/ws"],
                stdout=vf, stderr=subprocess.STDOUT))
            print("  victim joined")
            time.sleep(args.seconds)
            after = metrics(args.port)
    finally:
        for p in procs:
            if p.poll() is None:
                p.terminate()
        time.sleep(0.5)
        for p in procs:
            if p.poll() is None:
                p.kill()

    victim = open(vic_log).read()
    print("\n" + "=" * 68)

    rejected = "rejecting asset bytes from a peer" in victim
    fell_back = "falling back to the coordinator" in victim
    ready = "asset ready" in victim
    # Results the coordinator ACCEPTED, not the victim's own claim.
    completed = 0
    if after:
        for w in after["fleet"]:
            completed += w["tasks_completed"]
    extra = (after["coordinator_asset_egress"] - before["coordinator_asset_egress"]
             if after and before else 0)
    tried_peer = "trying peer" in victim
    # THE DISCRIMINATOR. Everything above can pass with the pre-D-0097 code,
    # which detected the attack, abandoned the fetch, released the task, and
    # recovered on a LATER grant. The first version of this harness scored that
    # as a pass — it checked that the victim ended up working, and the victim
    # did, one wasted task later.
    #
    # `peer_attempt_` resets to 0 at the start of every fetch, so the retry can
    # be handed the same attacker again. Escaping is luck, not logic.
    abandoned = victim.count("asset fetch abandoned")
    fetch_rounds = victim.count("fetching asset ")

    checks = [
        ("victim attempted the peer at all", tried_peer),
        ("victim REJECTED the corrupt bytes", rejected),
        ("victim FELL BACK to the coordinator", fell_back),
        ("victim obtained a valid asset", ready),
        ("coordinator served one extra copy", extra > 0),
        ("fleet kept completing tasks", completed > 1),
        ("victim NEVER abandoned a fetch", abandoned == 0),
        ("victim needed exactly ONE fetch round", fetch_rounds == 1),
    ]
    for label, ok in checks:
        print(f"  [{'PASS' if ok else 'FAIL'}] {label}")
    print(f"\n  fetch rounds: {fetch_rounds}   abandoned: {abandoned}")
    print(f"  extra coordinator egress: {extra:,} B  "
          f"(the measurable cost of the attack)")
    print(f"  logs: {vic_log}")

    for line in victim.splitlines():
        if re.search(r"rejecting|falling back|asset ready|trying peer", line):
            print("    " + line.strip())

    ok = all(v for _, v in checks)
    print("\n  " + ("PASS — a lying peer costs one wasted transfer, not a task"
                    if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
