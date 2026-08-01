#!/usr/bin/env python3
"""R2 enforcement: no platform conditionals outside the seam — step 1.17.

`worker-core` compiles to native and to WebAssembly from IDENTICAL source. The
only place the two targets may diverge is `src/worker-core/platform/`, whose
files are selected by CMake per target.

R2 exists because the failure it prevents is silent. Nothing breaks the day
someone adds `#ifdef __EMSCRIPTEN__` to a portable file — it compiles, it runs,
and the two targets simply stop being the same program. By the time that
matters you are debugging a divergence that has been accumulating for weeks.

So this is a test, not a review note. A rule enforced by remembering to look is
a rule that holds until the session that forgets.

Run standalone or via `ctest` (registered as `r2_platform_seam`).
"""

import pathlib
import re
import sys

# Files whose whole job is to be one target's half of the seam.
SEAM_DIR = pathlib.Path("src/worker-core/platform")

# Anything that makes code compile differently per target. `__EMSCRIPTEN__` is
# the obvious one; the others are the ways people reach for the same effect
# once they know the obvious one is checked.
CONDITIONALS = re.compile(
    r"^\s*#\s*(if|ifdef|ifndef|elif).*\b("
    r"__EMSCRIPTEN__"
    r"|EMSCRIPTEN"
    r"|__wasm__"
    r"|__wasm32__"
    r"|P2PGPU_WASM"
    r")\b"
)

SEARCH_ROOTS = ["src", "include"]
SUFFIXES = {".cpp", ".hpp", ".h", ".cc"}


def strip_comments(text: str) -> str:
    """Remove // and /* */ comments.

    Necessary because the rule is DISCUSSED in comments all over this codebase
    — platform.hpp states it, kernel_host.hpp explains why its design avoids it.
    A checker that flags its own documentation gets disabled within a week.
    """
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    seam = (root / SEAM_DIR).resolve()

    violations = []
    for search_root in SEARCH_ROOTS:
        for path in sorted((root / search_root).rglob("*")):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            if seam in path.resolve().parents:
                continue  # the seam is where this is allowed
            body = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
            for lineno, line in enumerate(body.splitlines(), start=1):
                if CONDITIONALS.search(line):
                    violations.append((path.relative_to(root), lineno, line.strip()))

    if violations:
        print("R2 VIOLATION — platform conditional outside the seam:\n")
        for path, lineno, line in violations:
            print(f"  {path}:{lineno}: {line}")
        print(
            "\nThe fix is NOT to move the #ifdef somewhere quieter. If a portable\n"
            "file appears to need one, the platform seam interface is wrong —\n"
            "widen include/p2pgpu/worker/platform.hpp instead. Forking the caller\n"
            "is exactly what R2 exists to prevent."
        )
        return 1

    print(f"R2 ok — no platform conditionals outside {SEAM_DIR}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
