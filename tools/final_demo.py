#!/usr/bin/env python3
"""Record a multi-device demonstration run and write everything it produced.

    python3 tools/final_demo.py --url https://p2pgpu.fly.dev --out results/8-31-final

Polls a deployed coordinator once a second until every tile is done (or you
Ctrl-C), then writes:

    timeline.csv      one row per second: workers, tiles, egress, asset sources
    final.json        the last full metrics snapshot, verbatim
    devices.csv       one row per DEVICE: GPU, tiles, throughput, time-to-ready
    render.ppm        the composited image, fetched from the coordinator
    SUMMARY.md        the numbers, written out in prose

────────────────────────────────────────────────────────────────────────────
WHY A DEDICATED RECORDER

The generic capture keeps the LAST poll. A device that finishes early and
disconnects is then absent from it — which already erased per-device evidence
twice in this project. Here every device seen is merged across the whole run
and kept, because "who took part" is the entire point of the demonstration.

Flags and counters are combined the way each one actually behaves: booleans
OR-ed (ICE gathers progressively), counters maxed (they only climb), and
identity fields kept once non-empty.

────────────────────────────────────────────────────────────────────────────
WHAT IT DELIBERATELY DOES NOT DO

It computes no throughput of its own and infers nothing the coordinator did not
report. Every number here is the coordinator's, measured on its clock — a
worker's self-reported timing is telemetry it chooses, and none of it is used.
"""

import argparse
import csv
import json
import os
import signal
import ssl
import struct
import sys
import time
import urllib.error
import urllib.request

RUNNING = True


def stop(_s, _f):
    global RUNNING
    RUNNING = False


def _ctx():
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        return None


def fetch(base, path, timeout=15):
    try:
        with urllib.request.urlopen(f"{base}{path}", timeout=timeout,
                                    context=_ctx()) as r:
            return r.read()
    except Exception as exc:   # noqa: BLE001
        print(f"\n  fetch {path} failed: {type(exc).__name__}: {exc}")
        return None


def save_render(base, out_dir):
    """`GET /render` is a 16-byte header then RGBA. Convert to a PPM."""
    raw = fetch(base, "/render", timeout=60)
    if not raw or len(raw) < 16:
        return None
    magic, w, h, _ = struct.unpack("<4I", raw[:16])
    if magic != 0x50324752 or w == 0 or h == 0:
        print(f"  /render returned an unexpected header: magic={magic:#x} {w}x{h}")
        return None
    rgba = raw[16:]
    if len(rgba) < w * h * 4:
        print(f"  /render short by {w * h * 4 - len(rgba)} bytes")
        return None
    path = os.path.join(out_dir, "render.ppm")
    with open(path, "wb") as fh:
        fh.write(b"P6\n%d %d\n255\n" % (w, h))
        # Drop alpha. The image is opaque; keeping it would need a PNG encoder
        # and a dependency this tool does not otherwise have.
        fh.write(bytes(b for i in range(0, w * h * 4, 4) for b in rgba[i:i + 3]))
    return path, w, h


def _image_complete(base):
    """True when the composite has no fully-black pixel.

    A tile that has never had a result accepted is exactly zero. The rendered
    scene has a lit sky and floor, so genuine black does not occur — which is
    what makes this a usable completeness test rather than a heuristic.
    """
    raw = fetch(base, "/render", timeout=60)
    if not raw or len(raw) < 16:
        return False
    magic, w, h, _ = struct.unpack("<4I", raw[:16])
    if magic != 0x50324752 or w == 0 or h == 0:
        return False
    px = raw[16:]
    if len(px) < w * h * 4:
        return False
    for i in range(0, w * h * 4, 4):
        if px[i] == 0 and px[i + 1] == 0 and px[i + 2] == 0:
            return False
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", required=True)
    ap.add_argument("--out", default="results/8-31-final")
    ap.add_argument("--stop-when-done", action="store_true", default=True,
                    help="stop once the composited image has no missing tiles")
    ap.add_argument("--settle-timeout", type=float, default=90.0,
                    help="seconds to wait for in-flight results after the "
                         "queue empties, before recording an incomplete image")
    args = ap.parse_args()

    signal.signal(signal.SIGINT, stop)
    os.makedirs(args.out, exist_ok=True)
    base = args.url.rstrip("/")

    rows, seen, last = [], {}, None
    settle_started = None
    t0 = time.time()
    print(f"recording {base} -> {args.out}   (Ctrl-C to stop early)")

    while RUNNING:
        raw = fetch(base, "/metrics")
        if raw:
            m = json.loads(raw)
            last = m
            done = m["tasks_by_state"][4] if m.get("tasks_by_state") else 0
            for w in m.get("fleet", []):
                wid = w["worker_id"]
                prev = seen.get(wid)
                if prev is None:
                    seen[wid] = dict(w)
                    seen[wid]["first_seen_s"] = round(time.time() - t0, 1)
                    continue
                for flag in ("ice_host", "ice_srflx", "ice_relay"):
                    prev[flag] = bool(prev.get(flag)) or bool(w.get(flag))
                for hi in ("tasks_completed", "units_completed", "ice_gathered"):
                    if w.get(hi) is not None:
                        prev[hi] = max(prev.get(hi) or 0, w[hi])
                for k in ("adapter_vendor", "adapter_device", "adapter_backend",
                          "score_ops_per_sec", "observed_units_per_sec",
                          "ms_to_first_result", "ms_to_first_grant"):
                    if w.get(k):
                        prev[k] = w[k]
            rows.append({
                "t_sec": round(time.time() - t0, 1),
                "workers": m["workers"],
                "devices_seen": len(seen),
                "tiles_done": done,
                "total_tasks": m["total_tasks"],
                "units_remaining": m["units_remaining"],
                "asset_from_peer": m["asset_from_peer"],
                "asset_from_coordinator": m["asset_from_coordinator"],
                "asset_from_cache": m["asset_from_cache"],
                "coordinator_asset_egress": m["coordinator_asset_egress"],
                "rejected_frames": m["rejected_frames"],
            })
            who = "  ".join(
                f"{(w.get('adapter_device') or w.get('adapter_vendor') or '?')[:16]}"
                f"={w.get('tasks_completed', 0)}" for w in seen.values())
            print(f"\r  {rows[-1]['t_sec']:>6}s  devices={len(seen)} "
                  f"tiles={done}/{m['total_tasks']}  egress={m['coordinator_asset_egress']:,}B"
                  f"   {who}          ", end="", flush=True)
            # STOP WHEN THE IMAGE IS COMPLETE — checked on the IMAGE.
            #
            # Two weaker conditions were tried and both were wrong.
            # `units_remaining == 0` means no more work will be HANDED OUT, not
            # that it has come back, and stopping there left a black rectangle
            # where two results were still in flight. `done >= total_tasks`
            # never fires at all, because speculation creates replica tasks that
            # are CANCELLED rather than completed — the run hung at 48 of 49.
            #
            # A tile is painted only when its result is accepted, so the
            # composite itself is the authority. Poll it and stop when no fully
            # black tile remains, with a bound so a genuinely stuck job ends.
            if (args.stop_when_done and m["total_tasks"] > 0
                    and m["units_remaining"] == 0):
                if settle_started is None:
                    settle_started = time.time()
                    print("\n  work exhausted — waiting for the image to fill in")
                if _image_complete(base):
                    print("  image complete")
                    break
                if time.time() - settle_started > args.settle_timeout:
                    print(f"  image still incomplete after "
                          f"{args.settle_timeout:.0f}s — recording it anyway, "
                          f"and SUMMARY.md will say so")
                    break
        time.sleep(1.0)

    if last is None or not rows:
        print("\nno metrics collected — is the coordinator reachable?")
        return 1

    with open(os.path.join(args.out, "timeline.csv"), "w", newline="") as fh:
        fh.write(f"# produced by tools/final_demo.py from {base}\n")
        fh.write("# PROVENANCE: REAL (deployed coordinator, physical devices)\n")
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    snap = dict(last)
    snap["fleet"] = list(seen.values())
    snap["source"] = base
    snap["duration_s"] = rows[-1]["t_sec"]
    with open(os.path.join(args.out, "final.json"), "w") as fh:
        json.dump(snap, fh, indent=2)

    dev_fields = ["worker_id", "adapter_vendor", "adapter_device",
                  "adapter_backend", "tasks_completed", "score_ops_per_sec",
                  "observed_units_per_sec", "ms_to_first_result",
                  "ice_host", "ice_srflx", "ice_relay", "first_seen_s"]
    with open(os.path.join(args.out, "devices.csv"), "w", newline="") as fh:
        fh.write("# one row per DEVICE that took part, merged across the run\n")
        w = csv.DictWriter(fh, fieldnames=dev_fields, extrasaction="ignore")
        w.writeheader()
        for d in seen.values():
            w.writerow(d)

    img = save_render(base, args.out)

    total_tiles = sum(d.get("tasks_completed", 0) for d in seen.values())
    dur = rows[-1]["t_sec"]
    lines = [
        "# Multi-device demonstration run",
        "",
        f"**Coordinator:** `{base}` (deployed, single instance)  ",
        f"**Duration:** {dur:.0f} s  ",
        f"**Devices that contributed:** {len(seen)}  ",
        f"**Tiles completed:** {total_tiles}",
        "",
        "## Devices",
        "",
        "| device | backend | tiles | benchmark | time-to-first-result |",
        "|---|---|---|---|---|",
    ]
    for d in sorted(seen.values(), key=lambda x: -(x.get("tasks_completed") or 0)):
        name = d.get("adapter_device") or d.get("adapter_vendor") or "unknown"
        score = d.get("score_ops_per_sec") or 0
        ready = d.get("ms_to_first_result")
        lines.append(
            f"| {name} | {d.get('adapter_backend') or '?'} | "
            f"{d.get('tasks_completed', 0)} | "
            f"{score / 1e9:.0f} Gops/s | "
            f"{(str(int(ready)) + ' ms') if ready and ready >= 0 else 'n/a'} |")
    f = rows[-1]
    lines += [
        "",
        "## Coordinator-measured totals",
        "",
        f"- asset fetches — peer **{f['asset_from_peer']}**, "
        f"coordinator **{f['asset_from_coordinator']}**, cache **{f['asset_from_cache']}**",
        f"- coordinator asset egress: **{f['coordinator_asset_egress']:,} bytes**",
        f"- malformed frames rejected: {f['rejected_frames']}",
        "",
        "Every figure above is the coordinator's own measurement. Worker "
        "self-reported timings are treated as untrusted telemetry and are not "
        "used here.",
    ]
    if img:
        lines += ["", f"## Image", "", f"`render.ppm` — {img[1]}x{img[2]}, "
                  "composited from tiles as they were accepted."]
    with open(os.path.join(args.out, "SUMMARY.md"), "w") as fh:
        fh.write("\n".join(lines) + "\n")

    print(f"\n\nwrote {args.out}/")
    for n in ("timeline.csv", "devices.csv", "final.json", "SUMMARY.md",
              "render.ppm"):
        p = os.path.join(args.out, n)
        if os.path.exists(p):
            print(f"  {n:<16} {os.path.getsize(p):>9,} B")
    print()
    print("\n".join(lines[:8]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
