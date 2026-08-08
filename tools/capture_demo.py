#!/usr/bin/env python3
"""Capture a converging render as workers join and leave — step 5.23.

Runs a real coordinator and real workers, polls `GET /render`, and writes one
frame per poll. It does NOT record a screen: the frames come from the same
endpoint the dashboard reads, so what gets captured is what the system actually
published, not what a compositor happened to draw.

Assembling the frames into a GIF or MP4 is left to ffmpeg — the command is
printed at the end. Shelling out to it here would make this script fail on a
machine that has everything it needs to produce the frames.

    python3 tools/capture_demo.py --out results/demo

────────────────────────────────────────────────────────────────────────────
WORKERS JOIN AND LEAVE ON PURPOSE

A render that simply converges shows nothing a single machine could not do. The
schedule below starts workers at staggered times and KILLS one mid-run, because
the claim being illustrated is R8's — a worker disappearing mid-task is the
normal case, and the image must keep converging through it rather than stalling
or losing the tile that worker held.

The frame where a worker dies is the interesting one, and it is deliberately not
smoothed over.
"""

import argparse
import os
import shutil
import signal
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request

RENDER_MAGIC = 0x50324752   # "RG2P"


def fetch_frame(url):
    """One frame from /render, or None if the coordinator has nothing yet."""
    try:
        with urllib.request.urlopen(url + "/render", timeout=5) as r:
            data = r.read()
    except (urllib.error.URLError, TimeoutError, ConnectionError):
        return None
    if len(data) < 16:
        return None
    magic, w, h, _ = struct.unpack("<4I", data[:16])
    # The declared size must match what arrived. Trusting the header and
    # slicing past the end would produce a truncated frame that looks like a
    # rendering bug rather than a transport one.
    if magic != RENDER_MAGIC or w == 0 or h == 0 or len(data) != 16 + w * h * 4:
        return None
    return w, h, data[16:]


def write_ppm(path, w, h, rgba):
    """RGBA8 -> binary PPM. No dependency, and every tool reads it."""
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(bytes(b for i in range(0, len(rgba), 4) for b in rgba[i:i + 3]))


def coverage(rgba):
    """Fraction of non-black pixels — how much of the image has any samples."""
    total = len(rgba) // 4
    if total == 0:
        return 0.0
    lit = sum(1 for i in range(0, len(rgba), 4)
              if rgba[i] or rgba[i + 1] or rgba[i + 2])
    return lit / total


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="results/demo", help="frame output directory")
    ap.add_argument("--build", default="build/native-release",
                    help="build directory holding coordinator and worker-native")
    ap.add_argument("--scene", default="scenes/default.scene")
    ap.add_argument("--size", default="256x192")
    ap.add_argument("--spp", type=int, default=8192)
    ap.add_argument("--port", type=int, default=8077)
    ap.add_argument("--seconds", type=float, default=40.0)
    ap.add_argument("--fps", type=float, default=4.0, help="capture rate")
    args = ap.parse_args()

    coordinator = os.path.join(args.build, "coordinator")
    worker = os.path.join(args.build, "worker-native")
    for binary in (coordinator, worker):
        if not os.path.exists(binary):
            print(f"missing {binary} — build {args.build} first", file=sys.stderr)
            return 1

    if os.path.isdir(args.out):
        shutil.rmtree(args.out)
    os.makedirs(args.out, exist_ok=True)

    url = f"http://localhost:{args.port}"
    procs = []

    def spawn(cmd):
        p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        procs.append(p)
        return p

    print(f"coordinator on :{args.port}, {args.size} @ {args.spp} spp")
    spawn([coordinator, "--seed-render", args.scene, "--render-size", args.size,
           "--render-spp", str(args.spp), "--port", str(args.port)])
    time.sleep(2.0)

    # (time in seconds, action). Two workers join early, a third joins late so
    # the convergence rate visibly changes, and one is killed to show the image
    # surviving a departure (R8).
    schedule = [
        (0.0, "join"),
        (3.0, "join"),
        (12.0, "join"),
        (22.0, "kill"),
    ]
    workers = []
    events = []

    start = time.time()
    interval = 1.0 / args.fps
    frame = 0
    next_capture = start

    try:
        while True:
            now = time.time()
            elapsed = now - start
            if elapsed > args.seconds:
                break

            while schedule and elapsed >= schedule[0][0]:
                _, action = schedule.pop(0)
                if action == "join":
                    workers.append(spawn([worker, "--coordinator",
                                          f"ws://localhost:{args.port}/ws"]))
                    events.append((frame, f"worker {len(workers)} joined"))
                    print(f"  t={elapsed:5.1f}s  worker {len(workers)} joined")
                elif action == "kill" and workers:
                    victim = workers.pop()
                    victim.send_signal(signal.SIGKILL)
                    events.append((frame, "worker killed mid-task"))
                    print(f"  t={elapsed:5.1f}s  worker KILLED mid-task")

            if now >= next_capture:
                got = fetch_frame(url)
                if got:
                    w, h, rgba = got
                    write_ppm(os.path.join(args.out, f"frame_{frame:04d}.ppm"),
                              w, h, rgba)
                    if frame % 8 == 0:
                        print(f"  t={elapsed:5.1f}s  frame {frame:4d}  "
                              f"coverage {100 * coverage(rgba):5.1f}%")
                    frame += 1
                next_capture += interval
            time.sleep(0.02)
    finally:
        for p in procs:
            if p.poll() is None:
                p.terminate()
        time.sleep(0.5)
        for p in procs:
            if p.poll() is None:
                p.kill()

    print(f"\n{frame} frames -> {args.out}/")
    for at, what in events:
        print(f"  frame {at:4d}: {what}")
    print("\nAssemble with:")
    print(f"  ffmpeg -framerate {args.fps:g} -i {args.out}/frame_%04d.ppm "
          f"-vf scale=512:-1:flags=neighbor -loop 0 {args.out}/demo.gif")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
