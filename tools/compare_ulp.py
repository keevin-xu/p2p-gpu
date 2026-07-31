#!/usr/bin/env python3
"""Measure float divergence between two machines, in ULPs (step 0.16 / R6).

    python3 tools/compare_ulp.py results/0.9-native.txt results/0.9-browser-nvidia.txt

WHY ULPs AND NOT RELATIVE ERROR

A ULP ("unit in the last place") is the gap between one representable float and
the next. Expressing divergence in ULPs is scale-free: "3 ULPs apart" means the
same thing at 1e-30 and at 1e30, whereas "1e-9 apart" is catastrophic at one
scale and meaningless at the other. Since a kernel's outputs span a wide dynamic
range, relative error would hide exactly the disagreements we care about.

WHAT THIS IS FOR

R6 claims two HONEST GPUs from different vendors disagree in the last few ULPs,
which is why replication cannot compare floats bitwise. This turns that claim
into a number. The measured spread is what sets the `Tolerant` epsilons in step
3.2 — otherwise those are guesses, and a guessed epsilon either misses real
cheating (too loose) or blacklists honest workers (too tight).

Run the same build on two machines, then compare:
  · `divergence_probe`  -> expect NON-ZERO across vendors. That IS the evidence.
  · `smoke_hash`        -> expect EXACTLY ZERO. Integer, DeterminismClass::Exact.
                           Any divergence here is a real bug, not R6.
"""

import argparse
import struct
import sys


def parse(path):
    """-> {kernel_name: [raw float bits]}"""
    out, current = {}, None
    try:
        lines = open(path).read().splitlines()
    except FileNotFoundError:
        print(f"error: {path} not found")
        return None

    for line in lines:
        if line.startswith("kernel="):
            fields = dict(t.split("=", 1) for t in line.split() if "=" in t)
            current = fields.get("kernel")
            out[current] = []
        elif current and line and all(c in "0123456789abcdef" for c in line):
            for i in range(0, len(line), 8):
                word = line[i:i + 8]
                if len(word) == 8:
                    # Hex is little-endian bytes as emitted; recover the u32.
                    b = bytes.fromhex(word)
                    out[current].append(struct.unpack("<I", b)[0])
    return out


def ulp_distance(a_bits, b_bits):
    """Distance in representable floats between two f32 bit patterns.

    The monotone-integer trick: for non-negative floats the IEEE-754 bit pattern
    increases monotonically, so subtracting the patterns counts the floats
    between them. Negatives are folded to a mirrored ordering first.
    """
    def order(bits):
        return bits if bits < 0x80000000 else 0x100000000 - bits
    a, b = order(a_bits), order(b_bits)
    return abs(a - b)


def is_nan(bits):
    return (bits & 0x7F800000) == 0x7F800000 and (bits & 0x007FFFFF) != 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a", help="first report (e.g. the Mac)")
    ap.add_argument("b", help="second report (e.g. the borrowed machine)")
    args = ap.parse_args()

    A, B = parse(args.a), parse(args.b)
    if A is None or B is None:
        return 2

    shared = [k for k in A if k in B]
    if not shared:
        print("error: no kernels in common — did both runs complete?")
        return 2

    print(f"A = {args.a}\nB = {args.b}\n")
    exit_code = 0

    for k in shared:
        a, b = A[k], B[k]
        if len(a) != len(b):
            print(f"{k}: LENGTH MISMATCH ({len(a)} vs {len(b)}) — skipping")
            exit_code = 1
            continue

        # Integer kernels are compared as INTEGERS, never as floats.
        #
        # Reading a u32 as an f32 makes some perfectly ordinary hash outputs
        # look like NaN, and NaN handling would then skip them — silently
        # excluding elements from the one comparison that must be exhaustive.
        # The first version of this script dropped 6 of 1000 hash elements
        # exactly this way.
        integer_kernel = "hash" in k

        dists, nans, identical = [], 0, 0
        for x, y in zip(a, b):
            if integer_kernel:
                dists.append(0 if x == y else 1)
                identical += (x == y)
                continue
            if is_nan(x) or is_nan(y):
                nans += 1
                continue
            d = ulp_distance(x, y)
            dists.append(d)
            if d == 0:
                identical += 1

        if not dists:
            print(f"{k}: all NaN, nothing to compare")
            continue

        mx, mean = max(dists), sum(dists) / len(dists)
        pct = 100.0 * identical / len(dists)
        exact = (mx == 0)

        unit = "differing elements" if integer_kernel else "ULP"
        print(f"{k}{'  [integer, exact comparison]' if integer_kernel else ''}")
        print(f"  bitwise identical : {identical}/{len(dists)} ({pct:.1f}%)")
        print(f"  max divergence    : {mx} {unit}")
        if not integer_kernel:
            print(f"  mean divergence   : {mean:.2f} ULP")
            if nans:
                print(f"  NaN skipped       : {nans}")

        # An integer kernel diverging is a BUG, not R6. Say so loudly — the
        # whole point of DeterminismClass::Exact is that it cannot legitimately
        # differ, so this must never be filed under "expected float noise".
        if "hash" in k and not exact:
            print("  *** ERROR: an Exact (integer) kernel MUST be bitwise "
                  "identical. This is a bug, NOT R6 divergence. ***")
            exit_code = 1
        elif "divergence" in k:
            if exact:
                print("  NOTE: zero divergence. Either the same implementation ran "
                      "on both, or these vendors genuinely agree here.")
            else:
                # This is the number Phase 3 needs.
                print(f"  => R6 CONFIRMED. Tolerant epsilon must exceed {mx} ULP; "
                      "see step 3.2.")

    return exit_code


if __name__ == "__main__":
    sys.exit(main())
