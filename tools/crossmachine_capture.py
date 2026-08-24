#!/usr/bin/env python3
"""Record a cross-machine session so nothing has to be pasted back by hand.

    python3 tools/crossmachine_capture.py --label crossmachine

Runs on the MAC, alongside the coordinator. Polls `/metrics` once a second and
writes two files under `results/`:

    <label>-timeline.csv    one row per second — workers, tasks, asset sources,
                            egress, per-worker time-to-ready
    <label>-final.json      the last full snapshot, including the fleet array

Stop it with Ctrl-C when the session ends; both files are written on the way
out, so an interrupted run still leaves usable data.

WHY THIS EXISTS
The browser's four diagnostic buttons already POST their own reports back to
`serve.py`, so those land in `results/` on their own. **The contributing run
does not** — a worker rendering tiles produces no report, and the evidence for
the cross-machine render lives in coordinator state that nobody is writing
down. This writes it down.

What it CANNOT capture, and you still have to bring back by hand:
  - anything printed on the borrowed machine (Part B native output)
  - screenshots
  - the Windows event log
"""

import argparse
import csv
import datetime
import json
import os
import subprocess
import signal
import ssl
import sys
import time
import urllib.error
import urllib.request

RUNNING = True


def stop(_sig, _frame):
    global RUNNING
    RUNNING = False


# HTTPS from a python.org build on macOS fails with CERTIFICATE_VERIFY_FAILED:
# that Python ships no CA bundle and does not read the system keychain, so
# `curl` succeeds against the same URL and this does not. certifi when present,
# the default context otherwise — a Linux box or a Homebrew Python needs no help.
def _ssl_context():
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        return None


_FIRST_ERROR_REPORTED = False


def metrics(base):
    """Fetch /metrics, or None — and SAY WHY the first time it fails.

    This used to swallow every exception and the caller printed "is it
    reachable?". It was reachable: `curl` returned 200 and the failure was a
    missing CA bundle. An error path that hides its own cause turns a
    one-line fix into a diagnosis.
    """
    global _FIRST_ERROR_REPORTED
    try:
        return json.load(urllib.request.urlopen(f"{base}/metrics", timeout=10,
                                                context=_ssl_context()))
    except Exception as exc:   # noqa: BLE001 — report anything, then continue
        if not _FIRST_ERROR_REPORTED:
            _FIRST_ERROR_REPORTED = True
            print(f"\n  fetch failed: {type(exc).__name__}: {exc}")
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8080)
    # A deployed coordinator is not on localhost, and its numbers are the ones
    # worth keeping. Same recorder either way, so a remote session lands in
    # results/ in the same shape as a local one.
    ap.add_argument("--url", default=None,
                    help="base URL of a REMOTE coordinator, e.g. "
                         "https://p2pgpu.fly.dev (overrides --port)")
    ap.add_argument("--label", default="crossmachine")
    ap.add_argument("--out", default="results")
    args = ap.parse_args()

    base = args.url.rstrip("/") if args.url else f"http://localhost:{args.port}"
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    os.makedirs(args.out, exist_ok=True)
    csv_path = os.path.join(args.out, f"{args.label}-timeline.csv")
    json_path = os.path.join(args.out, f"{args.label}-final.json")

    rows = []
    last = None
    t0 = time.time()
    print(f"recording {base} -> {csv_path}   (Ctrl-C to stop)")

    while RUNNING:
        m = metrics(base)
        if m is not None:
            last = m
            done = m["tasks_by_state"][4] if m.get("tasks_by_state") else 0
            # Slowest worker still counts: a fleet member that has never
            # produced anything is not ready, and averaging it out would hide
            # exactly the machine this session is about.
            ready = [w["ms_to_first_result"] for w in m["fleet"]
                     if w["ms_to_first_result"] >= 0]
            rows.append({
                "t_sec": round(time.time() - t0, 1),
                "workers": m["workers"],
                "queue_depth": m["queue_depth"],
                "tasks_done": done,
                "asset_from_peer": m["asset_from_peer"],
                "asset_from_coordinator": m["asset_from_coordinator"],
                "asset_from_cache": m["asset_from_cache"],
                "coordinator_asset_egress": m["coordinator_asset_egress"],
                "rejected_frames": m["rejected_frames"],
                "workers_ready": len(ready),
                "slowest_ms_to_ready": round(max(ready), 1) if ready else -1,
            })
            if len(rows) % 15 == 0:
                print(f"  t={rows[-1]['t_sec']:>6}s workers={m['workers']} "
                      f"done={done} peer={m['asset_from_peer']} "
                      f"coord={m['asset_from_coordinator']} "
                      f"egress={m['coordinator_asset_egress']:,}")
        time.sleep(1.0)

    if not rows:
        print(f"\nno samples — is {base} reachable?")
        return 1
    with open(csv_path, "w", newline="") as fh:
        # Provenance emitted by the driver so a regeneration keeps it.
        rev = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True).stdout.strip() or "unknown"
        fh.write(f"# produced by tools/crossmachine_capture.py\n")
        fh.write(f"# rev={rev} date={datetime.date.today().isoformat()}\n")
        fh.write("# PROVENANCE: REAL (live cross-machine session; see the adapters block in the -final.json)\n")
        fh.write(f"# source={base} label={args.label}\n")
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    with open(json_path, "w") as fh:
        json.dump(last, fh, indent=2)

    f = rows[-1]
    print(f"\nwrote {csv_path} ({len(rows)} samples)")
    print(f"wrote {json_path}")
    print(f"\n  workers seen      {f['workers']}")
    print(f"  tasks completed   {f['tasks_done']}")
    print(f"  asset  peer={f['asset_from_peer']} "
          f"coordinator={f['asset_from_coordinator']} cache={f['asset_from_cache']}")
    print(f"  coordinator egress {f['coordinator_asset_egress']:,} B")
    print(f"  slowest time-to-ready {f['slowest_ms_to_ready']} ms")

    # WHICH GPUs produced this. Printed and stored because a run that cannot
    # say what hardware it ran on is not evidence: the 2026-08-11 `r4-load`
    # capture had to be scored as not-met purely because nothing recorded the
    # machine (D-0101).
    print("  adapters:")
    for w in (last.get("fleet") or []):
        print(f"    worker {w['worker_id']}: "
              f"{w.get('adapter_vendor') or '?'} / "
              f"{w.get('adapter_device') or '?'} / "
              f"{w.get('adapter_backend') or '?'}")
    if not any((w.get("adapter_device") or w.get("adapter_vendor"))
               for w in (last.get("fleet") or [])):
        print("    NONE REPORTED — this run is not attributable to hardware.")
    if f["asset_from_peer"] == 0:
        print("\n  NOTE: zero peer fetches. Either only one machine ever held the "
              "asset, or the data plane did not work across the link — which is "
              "itself the finding. Keep the worker logs.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
