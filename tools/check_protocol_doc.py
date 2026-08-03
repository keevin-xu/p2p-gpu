#!/usr/bin/env python3
"""PROTOCOL.md must agree with protocol/p2pgpu.fbs — G1 evidence, made standing.

R3 says the schema is the single source of truth and `PROTOCOL.md` is the
authoritative description of the wire contract. Those two claims are only
compatible if the document actually matches the schema, and nothing enforced it.

It did not match. By step 1.28 the schema had moved three times without the doc
following:

  - `OutputSpec` gained `init` (D-0040)
  - `ReleaseReason` gained `KernelUnavailable` and `ExecutionFailed` (D-0035)
  - `Signal` used `to`/`from` in the doc and `peer` in the schema

None of that breaks a build. A stale wire document is worse than no document,
because it is trusted.

── RUN THIS BY HAND. IT IS NOT A CTEST. ─────────────────────────────────
`*.md` is gitignored in this repo, so docs/PROTOCOL.md does not exist on a
fresh clone. Registered as a ctest it passed locally and failed in CI with
FileNotFoundError — a test that cannot run where the code actually lives.

A ctest may only read committed files. Run this before a gate, or any time the
schema changes.

Checks two things the doc actually asserts:
  1. Inline ```` ```flatbuffers ```` blocks reproducing a table must list the
     same fields, in the same order, as the schema.
  2. Every table and enum in the schema must appear somewhere in the doc, so a
     newly added message cannot go undocumented.

Deliberately NOT a byte-for-byte comparison: the doc reproduces a subset with
its own commentary, and demanding textual identity would make it a copy rather
than an explanation.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCHEMA = ROOT / "protocol" / "p2pgpu.fbs"
DOC = ROOT / "docs" / "PROTOCOL.md"


def tables_of(text: str) -> dict[str, list[str]]:
    """table Name { field: type; ... } -> {Name: [field, ...]}"""
    out: dict[str, list[str]] = {}
    for m in re.finditer(r"table (\w+)\s*\{(.*?)\}", text, re.S):
        # `\w+\s*:\s*\[?\w` matches a field declaration and not a comment line.
        out[m.group(1)] = re.findall(r"(\w+)\s*:\s*\[?\w", m.group(2))
    return out


def main() -> int:
    schema = SCHEMA.read_text(encoding="utf-8")
    doc = DOC.read_text(encoding="utf-8")

    problems: list[str] = []

    # 1. Inline blocks must agree field-for-field, in order. Order matters:
    #    FlatBuffers assigns field ids positionally unless they are explicit, so
    #    a reordering in the schema is a WIRE BREAK, and a doc that shows the
    #    old order would actively mislead.
    s_tables, d_tables = tables_of(schema), tables_of(doc)
    shared = sorted(set(s_tables) & set(d_tables))
    for name in shared:
        if s_tables[name] != d_tables[name]:
            problems.append(
                f"table {name} disagrees\n"
                f"     schema: {s_tables[name]}\n"
                f"     doc   : {d_tables[name]}"
            )

    # 2. Nothing in the schema may be entirely absent from the doc.
    declared = re.findall(r"^(?:table|struct|enum|union) (\w+)", schema, re.M)
    for name in sorted(set(declared)):
        if not re.search(rf"\b{re.escape(name)}\b", doc):
            problems.append(f"{name} is in the schema but nowhere in PROTOCOL.md")

    if problems:
        print("PROTOCOL.md does not match protocol/p2pgpu.fbs:\n")
        for p in problems:
            print(f"  - {p}")
        print(
            "\nThe SCHEMA is the source of truth (R3), so the fix is normally to\n"
            "update the doc. If the schema is what is wrong, changing it is a wire\n"
            "change and needs a DECISIONS.md entry."
        )
        return 1

    print(
        f"PROTOCOL.md matches the schema "
        f"({len(shared)} tables compared field-by-field, "
        f"{len(set(declared))} declarations present)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
