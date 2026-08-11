#!/usr/bin/env python3
"""Convert a Wavefront .obj mesh into a p2pgpu .scene file.

    python3 tools/obj_to_scene.py model.obj -o scenes/scene3.scene

The renderer has exactly two primitives, `sphere` and `tri`, and flat
lambertian / metal / emissive materials. A mesh is already triangles, so
importing one is a translation rather than a feature — but it is the only way
to get arbitrary geometry in, since nothing here authors a mesh.

────────────────────────────────────────────────────────────────────────────
WHAT YOU GET, AND WHAT YOU DO NOT

**No textures, no UVs, no vertex normals.** The scene format carries none of
them, and the path tracer shades from geometric normals only. An imported model
renders as clean untextured geometry — correct silhouette and shading, flat
colour per material group. A model whose *appearance* lives in its texture will
look nothing like its reference render, and that is a property of this
renderer, not of the conversion.

Faceting is the visible consequence: without vertex normals a low-poly sphere
looks like a disco ball. Import a reasonably dense mesh, or expect facets.

────────────────────────────────────────────────────────────────────────────
WHY THIS IS A GOOD TEST ASSET

A real mesh is thousands of primitives against `default.scene`'s 70, which
deepens the BVH, raises F (ops per sample), and improves the R5 ratio — the
gate moves in the safe direction. It also makes the serialised BVH large enough
that peer-to-peer asset transfer is doing visible work, which is what the
Phase 6 experiments care about.

────────────────────────────────────────────────────────────────────────────
THE FILE IS UNTRUSTED INPUT AND IS TREATED AS SUCH

Anything downloaded is hostile until parsed. Indices are bounds-checked against
the vertex list, non-finite coordinates are rejected, and **degenerate triangles
are dropped** — a zero-area triangle has no usable normal, and the loader that
consumes this output has already been fuzzed into finding exactly that class of
input (D-0070). Better to drop them here, loudly, than to ship them.
"""

import argparse
import math
import os
import sys

# Same colours as gen_scene.py's vivid palette, so an imported model sits
# alongside a generated scene without a second visual language.
PALETTE = [
    (0.95, 0.10, 0.15), (0.10, 0.45, 0.95), (0.15, 0.90, 0.35),
    (0.98, 0.75, 0.05), (0.75, 0.15, 0.95), (0.05, 0.90, 0.85),
    (0.98, 0.40, 0.05),
]


def f(x):
    return f"{x:.6f}"


def parse_mtl(path):
    """Diffuse colour per material name. Missing file is not an error."""
    colours, current = {}, None
    try:
        with open(path, "r", errors="replace") as fh:
            for line in fh:
                parts = line.split()
                if not parts:
                    continue
                if parts[0] == "newmtl" and len(parts) > 1:
                    current = parts[1]
                elif parts[0] == "Kd" and current and len(parts) >= 4:
                    try:
                        colours[current] = tuple(min(1.0, max(0.0, float(v)))
                                                 for v in parts[1:4])
                    except ValueError:
                        pass
    except OSError:
        pass
    return colours


def parse_obj(path):
    """Vertices, and faces grouped by material name."""
    verts = []
    groups = {}
    current = "default"
    mtllib = None

    with open(path, "r", errors="replace") as fh:
        for line in fh:
            parts = line.split()
            if not parts or parts[0].startswith("#"):
                continue
            tag = parts[0]
            if tag == "v" and len(parts) >= 4:
                try:
                    xyz = tuple(float(v) for v in parts[1:4])
                except ValueError:
                    continue
                # Reject non-finite here rather than letting a NaN reach the
                # BVH builder, where it silently poisons every split above it.
                if all(math.isfinite(c) for c in xyz):
                    verts.append(xyz)
            elif tag == "usemtl" and len(parts) > 1:
                current = parts[1]
            elif tag == "mtllib" and len(parts) > 1:
                mtllib = parts[1]
            elif tag == "f" and len(parts) >= 4:
                idx = []
                for tok in parts[1:]:
                    try:
                        i = int(tok.split("/")[0])
                    except ValueError:
                        idx = []
                        break
                    # OBJ is 1-based, and NEGATIVE indices are relative to the
                    # end of the vertex list so far. Both are in the format;
                    # only handling the positive case silently mangles a
                    # perfectly legal file.
                    i = len(verts) + i if i < 0 else i - 1
                    if not (0 <= i < len(verts)):
                        idx = []
                        break
                    idx.append(i)
                if len(idx) >= 3:
                    # Fan-triangulate. Correct for convex polygons, which is
                    # what exporters emit; a concave n-gon would need ear
                    # clipping and is rare enough to not justify it.
                    for k in range(1, len(idx) - 1):
                        groups.setdefault(current, []).append(
                            (idx[0], idx[k], idx[k + 1]))
    return verts, groups, mtllib


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("obj")
    ap.add_argument("-o", "--out", default="-")
    ap.add_argument("--scale", type=float, default=0.0,
                    help="explicit scale; default 0 auto-fits to --fit-height")
    ap.add_argument("--fit-height", type=float, default=2.0,
                    help="target height in world units when auto-scaling")
    ap.add_argument("--material", choices=("lambertian", "metal"),
                    default="lambertian")
    ap.add_argument("--fuzz", type=float, default=0.10,
                    help="roughness, --material metal only")
    ap.add_argument("--no-mtl", action="store_true",
                    help="ignore the .mtl and use the built-in palette")
    ap.add_argument("--no-ground", action="store_true")
    ap.add_argument("--yaw", type=float, default=0.0,
                    help="degrees to rotate the camera around the model")
    args = ap.parse_args()

    verts, groups, mtllib = parse_obj(args.obj)
    if not verts or not groups:
        print(f"{args.obj}: no usable geometry", file=sys.stderr)
        return 1

    colours = {}
    if mtllib and not args.no_mtl:
        colours = parse_mtl(os.path.join(os.path.dirname(args.obj), mtllib))

    # ── normalise: centre on the origin, sit on y=0, scale to fit ─────────
    lo = [min(v[i] for v in verts) for i in range(3)]
    hi = [max(v[i] for v in verts) for i in range(3)]
    size = [hi[i] - lo[i] for i in range(3)]
    scale = args.scale if args.scale > 0 else (
        args.fit_height / size[1] if size[1] > 1e-9 else 1.0)
    cx = (lo[0] + hi[0]) / 2.0
    cz = (lo[2] + hi[2]) / 2.0

    def place(v):
        # Y is offset by `lo[1]` rather than centred: a model resting ON the
        # ground plane is what every other scene here looks like, and a figure
        # floating half-buried reads as a bug in the renderer.
        return ((v[0] - cx) * scale, (v[1] - lo[1]) * scale, (v[2] - cz) * scale)

    out = []
    out.append(f"# generated by tools/obj_to_scene.py {os.path.basename(args.obj)}")
    out.append("# DO NOT EDIT BY HAND — regenerate from the .obj instead.")
    out.append("# No textures or vertex normals: this renderer has neither.")
    out.append("version 1")
    out.append("")

    h = args.fit_height
    yaw = math.radians(args.yaw)
    dist = h * 2.2
    out.append(f"camera origin {f(math.sin(yaw) * dist)} {f(h * 0.75)} "
               f"{f(math.cos(yaw) * dist)} "
               f"target 0.000000 {f(h * 0.45)} 0.000000 "
               f"up 0.000000 1.000000 0.000000 vfov_deg 40.000000")
    out.append("")

    out.append("material 0 lambertian 0.720000 0.720000 0.700000   # ground")
    out.append("material 1 emissive   9.000000 8.400000 7.200000   # light")
    next_mat = 2
    group_mat = {}
    for n, name in enumerate(sorted(groups)):
        r, g, b = colours.get(name, PALETTE[n % len(PALETTE)])
        if args.material == "metal":
            out.append(f"material {next_mat} metal {f(r)} {f(g)} {f(b)} {f(args.fuzz)}"
                       f"  # {name}")
        else:
            out.append(f"material {next_mat} lambertian {f(r)} {f(g)} {f(b)}  # {name}")
        group_mat[name] = next_mat
        next_mat += 1
    out.append("")

    if not args.no_ground:
        e = h * 20.0
        out.append("# ground: two triangles, same as gen_scene.py")
        out.append(f"tri {f(-e)} 0.000000 {f(-e)} {f(e)} 0.000000 {f(-e)} "
                   f"{f(e)} 0.000000 {f(e)} 0")
        out.append(f"tri {f(-e)} 0.000000 {f(-e)} {f(e)} 0.000000 {f(e)} "
                   f"{f(-e)} 0.000000 {f(e)} 0")
    out.append(f"sphere {f(h * 1.1)} {f(h * 2.4)} {f(h * 1.2)} {f(h * 0.45)} 1")
    out.append("")

    kept = dropped = 0
    for name in sorted(groups):
        out.append(f"# {name}")
        for (ia, ib, ic) in groups[name]:
            a, b, c = place(verts[ia]), place(verts[ib]), place(verts[ic])
            # Drop zero-area triangles. Cross product of two edges; if its
            # length is ~0 the triangle has no normal, and Moller-Trumbore
            # divides by a determinant that goes to zero.
            ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
            vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
            nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
            if math.sqrt(nx * nx + ny * ny + nz * nz) < 1e-12:
                dropped += 1
                continue
            out.append(f"tri {f(a[0])} {f(a[1])} {f(a[2])} "
                       f"{f(b[0])} {f(b[1])} {f(b[2])} "
                       f"{f(c[0])} {f(c[1])} {f(c[2])} {group_mat[name]}")
            kept += 1

    if kept == 0:
        print("every triangle was degenerate — nothing to render", file=sys.stderr)
        return 1

    text = "\n".join(out) + "\n"
    if args.out == "-":
        sys.stdout.write(text)
    else:
        with open(args.out, "w") as fh:
            fh.write(text)
        print(f"wrote {args.out}: {kept} triangles, {len(groups)} material group(s)")
    # Loud, not a footnote: a model that loses most of its faces here is a
    # model that will look wrong, and silence would make that a mystery later.
    if dropped:
        print(f"  dropped {dropped} degenerate triangle(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
