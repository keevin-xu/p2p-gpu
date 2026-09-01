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
import ssl
import subprocess
import sys
import time
import urllib.error
import urllib.request

RUNNING = True


def stop(_s, _f):
    global RUNNING
    RUNNING = False


def _ctx():
    # A python.org build on macOS carries no CA bundle and ignores the system
    # keychain, so HTTPS fails here while curl succeeds on the same URL.
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        return None


def metrics(base):
    try:
        with urllib.request.urlopen(f"{base}/metrics", timeout=10,
                                    context=_ctx()) as r:
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
    # A DEPLOYED coordinator is the realistic setting for this measurement: it
    # is reachable from a phone on cellular, which a laptop on the same wifi is
    # not. With --url the script records rather than launches, and the ICE
    # servers come from that deployment's own configuration.
    ap.add_argument("--url", default=None,
                    help="record a REMOTE coordinator instead of launching one")
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
    last = None
    # EVERY worker seen during the run, merged across polls.
    #
    # `last` is the final poll, and a worker that disconnected before Ctrl-C is
    # simply absent from it — which erased the per-worker ICE state twice, the
    # second time after it had been added specifically to answer the open
    # question. A fleet snapshot taken at the end records who was still there,
    # not who took part.
    #
    # Flags are OR-ed because ICE gathers progressively: a later poll with fewer
    # bits means gathering had not finished, not that a path disappeared.
    seen = {}

    def absorb(snapshot):
        for w in snapshot.get("fleet", []):
            wid = w["worker_id"]
            prev = seen.get(wid)
            if prev is None:
                seen[wid] = dict(w)
                continue
            for flag in ("ice_host", "ice_srflx", "ice_relay"):
                prev[flag] = bool(prev.get(flag)) or bool(w.get(flag))
            for hi in ("tasks_completed", "ice_gathered"):
                prev[hi] = max(prev.get(hi) or 0, w.get(hi) or 0)
            for k in ("adapter_vendor", "adapter_device", "adapter_backend"):
                prev[k] = w.get(k) or prev.get(k)

    if args.url:
        # RECORD-ONLY. The deployment supplies its own --ice-server flags, so
        # this arm is defined by how the coordinator was deployed, not by what
        # is typed here — and the snapshot records which, so the two arms
        # cannot be mixed up afterwards.
        base = args.url.rstrip("/")
        print(f"recording {base}  (arm: {args.label})")
        print("join from the OTHER machine now; Ctrl-C when the render is done")
        while RUNNING:
            m = metrics(base)
            if m is not None:
                last = m
                absorb(m)
                # Per-worker relay state LIVE, so the run can be judged while it
                # is happening rather than from a file afterwards.
                who = " ".join(f"w{w['worker_id']}:"
                               f"{'H' if w.get('ice_host') else '-'}"
                               f"{'S' if w.get('ice_srflx') else '-'}"
                               f"{'R' if w.get('ice_relay') else '-'}"
                               for w in seen.values())
                print(f"\r  workers={m['workers']} tasks={m['tasks_by_state'][4]} "
                      f"attempts={m['ice_fetches']} connected={m['ice_connected']} "
                      f"| {who}      ", end="", flush=True)
            time.sleep(1.0)
    else:
        with open(log_path, "w") as lf:
            proc = subprocess.Popen(cmd, stdout=lf, stderr=subprocess.STDOUT)
            print(f"coordinator up on :{args.port}  (arm: {args.label})")
            print("join from the OTHER machine now; Ctrl-C when the render is done")
            base = f"http://localhost:{args.port}"
            try:
                while RUNNING and proc.poll() is None:
                    m = metrics(base)
                    if m is not None:
                        last = m
                        absorb(m)
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
    # THE WHOLE SNAPSHOT, not a curated subset.
    #
    # This used to keep nine hand-picked keys plus a trimmed `adapters` list.
    # Then `ice_relay` was added PER WORKER to answer whether the answering side
    # had a relay path — the exact question the measurement was stuck on — and
    # the recorder silently dropped it, because nobody remembered to add it
    # here too. A whole arm was run and thrown away for that.
    #
    # Curating fields means every new metric needs a second, separate edit in a
    # file nobody thinks to open. Keeping everything costs a few KB and cannot
    # forget.
    snap = dict(last)
    # The merged view REPLACES the final poll's fleet, which lists only whoever
    # happened to still be connected.
    snap["fleet"] = list(seen.values())
    # REDACTED BEFORE IT TOUCHES DISK. `results/` is a TRACKED directory, and a
    # TURN URL carries `user:password@`. Writing the raw flags here would commit
    # live relay credentials to git — recoverable from history even after a
    # later deletion. What the arm needs on record is WHICH KIND of server was
    # configured, which survives redaction intact.
    def _redact(url):
        if "@" not in url:
            return url                      # a bare stun: URL has no secret
        scheme, rest = url.split(":", 1)
        return f"{scheme}:<redacted>@{rest.split('@', 1)[1]}"

    snap["ice_servers"] = ([_redact(u) for u in args.ice_server]
                           if args.ice_server else "(from the deployment)")
    snap["source"] = args.url or f"localhost:{args.port}"
    # `fleet` is already in `snap` verbatim, per-worker ICE state included.
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
