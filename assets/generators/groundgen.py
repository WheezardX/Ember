"""Ground-cover growth forms (ground plane v1, EPIC_5_PLAN 8f GP4): pure Python geometry, like
treegen.py, loaded into Nanite meshes by ground_cover.py.

Items (the keys the renderer and viz/looks [cover] use):
    fern          sword-fern clump: arching fronds with paired pinnae              ~70 cm
    huckleberry   low shrub in fall colour (red/orange leaves; Three Queens photo)  ~70 cm
    shrub         low green shrub                                                  ~60 cm
    rock          boulder / cobble: a lumpy, flat-bottomed blob                     ~60 cm
    log           down log with a broken end, lying along +X                       ~4.5 m
    stump         cut / broken stump                                               ~70 cm
Grass uses the scatter's bunchgrass meshes (treegen). Sizes are nominal: the renderer scales each
instance from the mesh's own bounds, so only proportions matter. Vertex alpha 1 = foliage (the
material's Color), 0 = wood / stone (TrunkColor); vertex RGB = shading.
"""

from __future__ import annotations

import math
import random

from treegen import Mesh, add, direction, leaf_cluster, mul, norm, perp, sub, tube

VARIANTS = 3


def fern(seed: int) -> Mesh:
    rng = random.Random(seed)
    m = Mesh()
    for _ in range(rng.randint(9, 13)):
        az = rng.uniform(0, 2 * math.pi)
        rise = math.radians(rng.uniform(50, 78))
        length = rng.uniform(55, 85)
        d = direction(az, rise)
        s = perp(d)
        pts = [(0.0, 0.0, 0.0)]
        for k in range(1, 9):  # arching midrib: out and up, then curling down
            t = k / 8
            p = add(mul(d, length * t), (0.0, 0.0, -length * 0.4 * t * t))
            pts.append(p)
        for k in range(1, 8):  # pinnae pairs: narrow blades off the midrib, shorter at the tip
            a, b = pts[k], pts[k + 1]
            along = norm(sub(b, a))
            w = 10.0 * (1.0 - k / 8)
            for side in (-1, 1):
                tip = add(a, add(mul(s, side * w), mul(along, 3.0)))
                i0 = m.vert(a, 0.4 + 0.04 * k, True)
                i1 = m.vert(b, 0.45 + 0.04 * k, True)
                i2 = m.vert(tip, 0.6, True)
                if side > 0:
                    m.tri(i0, i2, i1)
                else:
                    m.tri(i0, i1, i2)
        tube(m, pts, [0.5] * len(pts), 3, 0.5, True)
    return m


def _low_shrub(seed: int, stems: int, height: float, leaves: int) -> Mesh:
    rng = random.Random(seed)
    m = Mesh()
    for _ in range(stems):
        az = rng.uniform(0, 2 * math.pi)
        d = norm(direction(az, math.radians(rng.uniform(50, 80))))
        end = mul(d, height * rng.uniform(0.6, 1.0))
        mid = add(mul(end, 0.5), (rng.uniform(-4, 4), rng.uniform(-4, 4), 0.0))
        tube(m, [(0.0, 0.0, 0.0), mid, end], [1.2, 0.9, 0.5], 4, 0.6)
        leaf_cluster(m, rng, end, height * 0.28, leaves, height * 0.05, 0.85)
        leaf_cluster(m, rng, mid, height * 0.2, leaves // 2, height * 0.045, 0.7)
    return m


def huckleberry(seed: int) -> Mesh:
    return _low_shrub(seed, stems=7, height=70.0, leaves=60)


def shrub(seed: int) -> Mesh:
    return _low_shrub(seed, stems=6, height=60.0, leaves=70)


def rock(seed: int) -> Mesh:
    """A subdivided sphere (shared vertices: smooth shading, no visible triangles) shaped by
    low-frequency lumps, a few planar cleavage cuts (the flat faces broken stone has), a squash,
    and a flattened base that sits in the ground. Shading: darker toward the base, mottled."""
    rng = random.Random(seed)
    base = [(1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)]
    faces = [(0, 2, 4), (2, 1, 4), (1, 3, 4), (3, 0, 4), (2, 0, 5), (1, 2, 5), (3, 1, 5), (0, 3, 5)]
    verts = [norm(v) for v in base]
    cache: dict[tuple[int, int], int] = {}

    def midpoint(a: int, b: int) -> int:
        key = (min(a, b), max(a, b))
        if key not in cache:
            verts.append(norm(mul(add(verts[a], verts[b]), 0.5)))
            cache[key] = len(verts) - 1
        return cache[key]

    for _ in range(4):
        nf = []
        for a, b, c in faces:
            ab, bc, ca = midpoint(a, b), midpoint(b, c), midpoint(c, a)
            nf += [(a, ab, ca), (b, bc, ab), (c, ca, bc), (ab, bc, ca)]
        faces = nf

    def rnd_dir():
        return norm((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-0.4, 1)))

    lumps = [(rnd_dir(), rng.uniform(0.08, 0.2), rng.uniform(1.5, 3.0)) for _ in range(6)]
    cuts = [(rnd_dir(), rng.uniform(0.72, 0.9)) for _ in range(rng.randint(3, 5))]
    waves = [(rnd_dir(), rng.uniform(4.0, 7.0), rng.uniform(0, 6.28)) for _ in range(4)]
    sx, sy, sz = rng.uniform(0.75, 1.25), rng.uniform(0.75, 1.25), rng.uniform(0.5, 0.8)
    r0 = 30.0
    m = Mesh()
    for v in verts:
        r = 1.0
        for d, h, sharp in lumps:
            r += h * max(0.0, sum(a * b for a, b in zip(v, d, strict=True))) ** sharp
        for d, f, ph in waves:  # small undulation, smooth at this vertex density
            r += 0.02 * math.sin(f * sum(a * b for a, b in zip(v, d, strict=True)) + ph)
        for n, off in cuts:     # cleavage planes: the radius stops at the plane
            c = sum(a * b for a, b in zip(v, n, strict=True))
            if c > 1e-3:
                r = min(r, off / c)
        z = max(-0.25, v[2] * r)
        p = (v[0] * r * sx * r0, v[1] * r * sy * r0, z * sz * r0)
        mottle = 0.9 + 0.1 * math.sin(7.0 * v[0] + 3.0) * math.sin(6.0 * v[1] + 1.0)
        m.vert(p, (0.5 + 0.4 * (v[2] * 0.5 + 0.5)) * mottle, False)
    for a, b, c in faces:
        m.tri(a, c, b)
    return m


def log(seed: int) -> Mesh:
    rng = random.Random(seed)
    m = Mesh()
    length = rng.uniform(380, 520)
    r = rng.uniform(18, 28)
    sag = rng.uniform(-6, 6)
    path = [(length * t, 0.0, r * 0.8 + sag * math.sin(math.pi * t))
            for t in (0, 0.25, 0.5, 0.75, 1.0)]
    tube(m, path, [r, r * 0.95, r * 0.9, r * 0.82, r * 0.7], 9, 0.55)
    for _ in range(rng.randint(2, 4)):  # broken branch stubs
        t = rng.uniform(0.2, 0.9)
        p = (length * t, 0.0, r * 0.8)
        d = norm((rng.uniform(-0.3, 0.3), rng.choice((-1, 1)) * 1.0, rng.uniform(0.2, 1.0)))
        tube(m, [p, add(p, mul(d, r * 1.6))], [r * 0.18, r * 0.1], 4, 0.5)
    return m


def stump(seed: int) -> Mesh:
    rng = random.Random(seed)
    m = Mesh()
    h = rng.uniform(40, 90)
    r = rng.uniform(22, 35)
    tube(m, [(0.0, 0.0, 0.0), (0.0, 0.0, h * 0.5), (0.0, 0.0, h)], [r * 1.25, r * 1.02, r], 10,
         0.55)
    for k in range(4):  # root flare
        a = k * math.pi / 2 + rng.uniform(-0.3, 0.3)
        d = (math.cos(a), math.sin(a), 0.0)
        tube(m, [(d[0] * r * 0.6, d[1] * r * 0.6, h * 0.15), (d[0] * r * 1.7, d[1] * r * 1.7, 0.0)],
             [r * 0.35, r * 0.15], 5, 0.5)
    return m


ITEMS = {"fern": fern, "huckleberry": huckleberry, "shrub": shrub, "rock": rock, "log": log,
         "stump": stump}

# Material colours (sRGB hex, no '#'): foliage (Color), wood / stone (TrunkColor).
COLORS = {
    "fern": ("2C4220", "3A3222"),
    "huckleberry": ("A8401E", "4A3526"),   # fall: vine maple / huckleberry reds (field photo)
    "shrub": ("4A5E2C", "4A3A2A"),
    "rock": ("7A766E", "736E66"),
    "log": ("5A5A40", "5A4834"),           # weathered bark; foliage colour unused
    "stump": ("5A5A40", "5E4A36"),
}


def build(key: str, variant: int) -> Mesh:
    m = ITEMS[key](7001 + 131 * list(ITEMS).index(key) + 977 * variant)
    lo = min(v[2] for v in m.verts)
    m.verts = [(x, y, z - lo) for (x, y, z) in m.verts]  # stands on its lowest point
    return m


def asset_names() -> list[str]:
    out = []
    for k in ITEMS:
        out.append(f"Cover/MI_Cover_{k}")
        out += [f"Cover/SM_Cover_{k}_v{v}" for v in range(VARIANTS)]
    return out
