#!/usr/bin/env python3
"""Step 0.9 — compare native and browser kernel output bitwise.

    python3 tools/compare_targets.py [--native results/0.9-native.txt]
                                     [--browser results/0.9-browser-<browser>.txt]

What this proves, and what it does not:

Both targets run the SAME WGSL bytes through the SAME worker-core host code and
the SAME verification logic; only the platform/ translation unit differs. So a
mismatch here is a bug in host code — a wrong offset, a bad stride, a truncated
size — and NOT R6 float divergence. R6 is about different *vendors* disagreeing
in the last few ULPs; this is one GPU, so bitwise equality is the correct bar.

Both kernels are DeterminismClass::Exact, which is what makes a bitwise
comparison legitimate at all. Never extend this script to a `tolerant` kernel
without switching to an epsilon comparison.

The adapter/backend header lines are expected to differ (the two
implementations populate WGPUAdapterInfo differently — see RISKS.md §1) and are
excluded from the comparison.
"""

import argparse
import sys


def parse(path):
    """-> (dict of kernel -> fingerprint, dict of kernel -> hex body)."""
    fingerprints, bodies, order = {}, {}, []
    current = None
    try:
        with open(path, "r") as f:
            lines = f.read().splitlines()
    except FileNotFoundError:
        print(f"error: {path} not found")
        return None, None, None

    for line in lines:
        if line.startswith("kernel="):
            fields = dict(tok.split("=", 1) for tok in line.split() if "=" in tok)
            current = fields.get("kernel")
            order.append(current)
            fingerprints[current] = fields
            bodies[current] = []
        elif current and line and all(c in "0123456789abcdef" for c in line):
            bodies[current].append(line)
    return fingerprints, bodies, order


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--native", default="results/0.9-native.txt")
    ap.add_argument("--browser", default="results/0.9-browser-chrome.txt")
    args = ap.parse_args()

    n_fp, n_body, n_order = parse(args.native)
    b_fp, b_body, b_order = parse(args.browser)
    if n_fp is None or b_fp is None:
        return 2

    if not n_order or not b_order:
        print("error: no kernel sections found — did both runs complete?")
        return 2

    kernels = [k for k in n_order if k in b_fp]
    missing = set(n_order) ^ set(b_order)
    if missing:
        print(f"warning: kernels present in only one report: {sorted(missing)}")

    failures = 0
    for k in kernels:
        n, b = n_fp[k], b_fp[k]
        same_fp = n.get("fnv1a") == b.get("fnv1a")
        same_body = n_body[k] == b_body[k]
        both_ok = n.get("match") == "yes" and b.get("match") == "yes"

        status = "MATCH" if (same_fp and same_body and both_ok) else "MISMATCH"
        if status == "MISMATCH":
            failures += 1

        nbytes = sum(len(line) for line in n_body[k]) // 2
        print(f"{status:9} {k}")
        print(f"          native  fnv1a={n.get('fnv1a')} vs-cpu-ref={n.get('match')}")
        print(f"          browser fnv1a={b.get('fnv1a')} vs-cpu-ref={b.get('match')}")
        print(f"          {nbytes} bytes compared, bodies "
              f"{'identical' if same_body else 'DIFFER'}")

        if not same_body:
            for i, (nl, bl) in enumerate(zip(n_body[k], b_body[k])):
                if nl != bl:
                    print(f"          first differing line {i}:")
                    print(f"            native  {nl}")
                    print(f"            browser {bl}")
                    break

    print()
    if failures:
        print(f"FAIL — {failures}/{len(kernels)} kernels differ across targets")
        print("This is a HOST-CODE bug, not float divergence: same GPU, same "
              "kernel bytes, Exact determinism class.")
        return 1

    print(f"PASS — {len(kernels)}/{len(kernels)} kernels bitwise-identical across "
          "native and browser, and both match the CPU reference")
    return 0


if __name__ == "__main__":
    sys.exit(main())
