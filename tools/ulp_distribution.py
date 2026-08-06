#!/usr/bin/env python3
"""ULP divergence distribution between two adapter dumps (step 4.7).

`compare_ulp.py` reports max and mean, which is what 0.16 needed to answer
"does R6 happen at all". Setting an epsilon needs the SHAPE: a max of 5 with a
p99 of 3 is a different argument from a max of 5 with a p99 of 5, and only the
distribution distinguishes them.

    python3 tools/ulp_distribution.py A.txt B.txt [kernel]
"""

import collections
import statistics
import struct
import sys


def load(path):
    """Parse the smoke-test dump: `kernel=NAME ...` followed by hex lines."""
    out, cur, name = {}, [], None
    for line in open(path):
        line = line.strip()
        if line.startswith("kernel="):
            if name:
                out[name] = "".join(cur)
            name, cur = line.split("kernel=")[1].split()[0], []
        elif name and line and all(c in "0123456789abcdef" for c in line):
            cur.append(line)
    if name:
        out[name] = "".join(cur)
    return out


def floats(hexs):
    raw = bytes.fromhex(hexs)
    return list(struct.unpack("<%df" % (len(raw) // 4), raw))


def ulp(x, y):
    """Representable steps between two floats, across zero."""
    if x == y:
        return 0
    ix, = struct.unpack("<i", struct.pack("<f", x))
    iy, = struct.unpack("<i", struct.pack("<f", y))
    # Signed-magnitude -> monotonic ordering, so the subtraction is meaningful
    # for opposite signs and +0.0/-0.0 compare equal.
    if ix < 0:
        ix = 0x80000000 - ix
    if iy < 0:
        iy = 0x80000000 - iy
    return abs(ix - iy)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    a, b = load(sys.argv[1]), load(sys.argv[2])
    kernels = [sys.argv[3]] if len(sys.argv) > 3 else sorted(set(a) & set(b))

    for k in kernels:
        if k not in a or k not in b:
            print(f"{k}: missing from one side")
            continue
        fa, fb = floats(a[k]), floats(b[k])
        if len(fa) != len(fb):
            print(f"{k}: length mismatch {len(fa)} vs {len(fb)}")
            continue
        d = [ulp(x, y) for x, y in zip(fa, fb)]
        rel = [abs(x - y) / abs(y) if y else 0.0 for x, y in zip(fa, fb)]
        n = len(d)
        hist = collections.Counter(d)

        print(f"\n{k}: {n} elements")
        print("  ULP  count   cumulative")
        cum = 0
        for u in sorted(hist):
            cum += hist[u]
            print(f"  {u:3}  {hist[u]:5}   {100 * cum / n:6.2f}%")
        print(f"  mean={statistics.mean(d):.3f}  "
              f"p99={sorted(d)[min(n - 1, int(0.99 * n))]}  max={max(d)}")
        print(f"  max relative error = {max(rel):.3e}")
        # The number an epsilon is actually set from. Reported next to the
        # margin so a later reader can see whether a change tightened it.
        print(f"  rel_eps 1e-6 gives {1e-6 / max(rel):.1f}x headroom"
              if max(rel) else "  (identical)")


if __name__ == "__main__":
    main()
