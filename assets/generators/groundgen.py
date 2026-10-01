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
    """Down log (ground feel, Kachess stand photos: grey, furrowed, mossy on top, a pale cut or
    broken end - not the smooth open pipe of v1): rings with bark furrows and lumps, a capped butt
    with a pale cut face, a ragged broken tip, moss patches (foliage verts = the moss colour) on
    the upward-facing bark."""
    rng = random.Random(seed)
    m = Mesh()
    length = rng.uniform(380, 520)
    r = rng.uniform(18, 28)
    sag = rng.uniform(-6, 6)
    sides, rings_n = 14, 18
    furrow = [rng.uniform(0.0, 2 * math.pi) for _ in range(3)]
    moss_c = [(rng.uniform(0.15, 0.85), rng.uniform(-0.6, 0.6), rng.uniform(0.08, 0.2)) for _ in range(rng.randint(2, 4))]
    rings = []
    for i in range(rings_n + 1):
        t = i / rings_n
        cx, cz = length * t, r * 0.8 + sag * math.sin(math.pi * t)
        rr = r * (1.0 - 0.3 * t) * (1.0 + 0.05 * math.sin(t * 9.0 + furrow[0]))
        ring = []
        for k in range(sides):
            a = 2.0 * math.pi * k / sides
            ca, sa = math.cos(a), math.sin(a)          # sa > 0: the upper side
            bump = 0.06 * math.sin(a * 7 + furrow[1] + t * 2.0) + 0.03 * math.sin(a * 13 + furrow[2])
            if i == rings_n:                             # broken tip: splintered, uneven
                bump -= rng.uniform(0.0, 0.35)
            rk = rr * (1.0 + bump + rng.uniform(-0.03, 0.03))
            pos = (cx + (rng.uniform(-6, 10) if i == rings_n else 0.0), ca * rk, cz + sa * rk)
            moss = sa > 0.35 and any((t - mt) ** 2 + (ca - my) ** 2 * 0.02 < ms ** 2 for mt, my, ms in moss_c)
            shade = 0.45 + 0.15 * sa + 0.1 * bump * 3.0
            ring.append(m.vert(pos, shade, moss))
        rings.append(ring)
    for i in range(rings_n):
        for k in range(sides):
            a, b = rings[i][k], rings[i][(k + 1) % sides]
            c, d = rings[i + 1][k], rings[i + 1][(k + 1) % sides]
            m.tri(a, c, b)
            m.tri(b, c, d)
    centre = m.vert((0.0, 0.0, r * 0.8), 0.85, False)  # butt: a pale cut face, closed
    for k in range(sides):
        m.tri(rings[0][k], rings[0][(k + 1) % sides], centre)
    tip = m.vert((length + rng.uniform(5, 25), 0.0, r * 0.8 + rng.uniform(-3, 3)), 0.7, False)
    for k in range(sides):
        m.tri(rings[-1][k], tip, rings[-1][(k + 1) % sides])
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


def snag(seed: int) -> Mesh:
    """A standing dead tree (8g): a tapering bare trunk, a jagged broken top, a few dead branch
    stubs angled down. Stands on 0,0,0; the renderer scales it to the rule's height."""
    rng = random.Random(seed)
    m = Mesh()
    h = rng.uniform(600, 800)
    r = rng.uniform(20, 30)
    lean = (rng.uniform(-8, 8), rng.uniform(-8, 8))
    path = [(lean[0] * t * t, lean[1] * t * t, h * t) for t in (0, 0.25, 0.5, 0.75, 1.0)]
    tube(m, path, [r * 1.15, r * 0.95, r * 0.8, r * 0.65, r * 0.5], 9, 0.5)
    for _ in range(rng.randint(3, 5)):          # broken top: splinters
        a = rng.uniform(0, 2 * math.pi)
        p = (path[-1][0] + math.cos(a) * r * 0.25, path[-1][1] + math.sin(a) * r * 0.25, h * 0.98)
        tube(m, [p, add(p, (math.cos(a) * r * 0.2, math.sin(a) * r * 0.2, rng.uniform(r, 3 * r)))],
             [r * 0.14, r * 0.02], 4, 0.45)
    for _ in range(rng.randint(3, 6)):          # dead branch stubs, drooping
        t = rng.uniform(0.35, 0.9)
        a = rng.uniform(0, 2 * math.pi)
        p = (lean[0] * t * t, lean[1] * t * t, h * t)
        d = norm((math.cos(a), math.sin(a), rng.uniform(-0.5, 0.1)))
        tube(m, [p, add(p, mul(d, rng.uniform(40, 120)))], [r * 0.16, r * 0.05], 4, 0.45)
    return m


def pole(seed: int) -> Mesh:
    """A long fallen stem (8g hung-up tree): lies along +X from its foot, tapering to a thin
    top, a few branch stubs; the renderer pitches it up into the neighbours' crowns."""
    rng = random.Random(seed)
    m = Mesh()
    length = rng.uniform(1100, 1500)
    r = rng.uniform(11, 15)
    path = [(length * t, 0.0, r + rng.uniform(-3, 3) * math.sin(math.pi * t))
            for t in (0, 0.25, 0.5, 0.75, 1.0)]
    tube(m, path, [r, r * 0.85, r * 0.65, r * 0.45, r * 0.25], 7, 0.5)
    for _ in range(rng.randint(3, 6)):
        t = rng.uniform(0.3, 0.95)
        p = (length * t, 0.0, r)
        d = norm((rng.uniform(-0.2, 0.4), rng.choice((-1, 1)) * 1.0, rng.uniform(-0.3, 0.6)))
        tube(m, [p, add(p, mul(d, rng.uniform(30, 90)))], [r * 0.2, r * 0.06], 4, 0.45)
    return m


def stick(seed: int) -> Mesh:
    """Down branch (ground v2 volume, 8g): a crooked 40-140 cm stem along +X from its butt at
    x = 0 (the renderer lays it on the ground like a log), a few side twigs, one or two lifted
    clear of the ground so the stick casts a broken shadow."""
    rng = random.Random(seed)
    m = Mesh()
    length = rng.uniform(40, 140)
    r = rng.uniform(1.2, 2.8)
    pts = [(0.0, 0.0, r)]
    y = 0.0
    for k in range(1, 6):
        y += rng.uniform(-4, 4)
        pts.append((length * k / 5, y, r + rng.uniform(-0.5, 1.5)))
    tube(m, pts, [r, r * 0.9, r * 0.75, r * 0.6, r * 0.45, r * 0.3], 5, 0.5)
    for _ in range(rng.randint(2, 5)):
        k = rng.randint(1, 4)
        p = pts[k]
        d = norm((rng.uniform(0.2, 0.9), rng.choice((-1, 1)) * rng.uniform(0.5, 1.0), rng.uniform(0.0, 0.6)))
        L = rng.uniform(8, 30)
        tube(m, [p, add(p, mul(d, L))], [r * 0.4, r * 0.15], 3, 0.5)
    return m


def branch(seed: int) -> Mesh:
    """Fallen limb (Kachess stand photos: the floor is criss-crossed with 1-3 m dead limbs): a
    crooked 150-340 cm stem along +X, sub-branches angled forward off both sides, some forked,
    a few lifted off the ground so they cast broken shadows."""
    rng = random.Random(seed)
    m = Mesh()
    length = rng.uniform(150, 340)
    r = rng.uniform(2.0, 4.5)
    n = 8
    pts = [(0.0, 0.0, r)]
    y = 0.0
    for k in range(1, n + 1):
        y += rng.uniform(-7, 7)
        pts.append((length * k / n, y, r + rng.uniform(-0.5, 2.5) * k / n))
    tube(m, pts, [r * (1.0 - 0.75 * k / n) for k in range(n + 1)], 6, 0.5)
    for _ in range(rng.randint(5, 10)):
        k = rng.randint(1, n - 1)
        p = pts[k]
        side = rng.choice((-1, 1))
        d = norm((rng.uniform(0.4, 1.2), side * rng.uniform(0.5, 1.0), rng.uniform(-0.05, 0.5)))
        L = rng.uniform(20, 75) * (1.0 - 0.5 * k / n)
        rk = r * (1.0 - 0.75 * k / n) * 0.45
        tip = add(p, mul(d, L))
        tube(m, [p, add(p, mul(d, L * 0.5)), tip], [rk, rk * 0.6, rk * 0.2], 4, 0.5)
        if rng.random() < 0.5:                   # a fork near the tip
            d2 = norm(add(d, (0.0, side * 0.6, rng.uniform(0.0, 0.3))))
            mid = add(p, mul(d, L * 0.6))
            tube(m, [mid, add(mid, mul(d2, L * 0.45))], [rk * 0.4, rk * 0.12], 3, 0.5)
    return m


def bark(seed: int) -> Mesh:
    """A slab of shed ponderosa / fir bark (ground v2): a curled plate 15-35 cm long along +X,
    edges thick enough to read, lying concave side down so it stands a few cm proud."""
    rng = random.Random(seed)
    m = Mesh()
    L, W = rng.uniform(15, 35), rng.uniform(8, 16)
    curl, th = rng.uniform(2.0, 5.0), rng.uniform(1.0, 2.0)
    nu, nv = 6, 4
    top, bot = [], []
    for i in range(nu + 1):
        for j in range(nv + 1):
            x = L * i / nu
            y = W * (j / nv - 0.5) * (1.0 - 0.25 * abs(i / nu - 0.5))
            z = curl * (1.0 - (2.0 * j / nv - 1.0) ** 2) + rng.uniform(-0.3, 0.3)
            top.append(m.vert((x, y, z + th), 0.55 + 0.1 * rng.random(), False))
            bot.append(m.vert((x, y, z), 0.35, False))
    for i in range(nu):
        for j in range(nv):
            a, b = i * (nv + 1) + j, (i + 1) * (nv + 1) + j
            m.tri(top[a], top[b], top[a + 1]); m.tri(top[a + 1], top[b], top[b + 1])
            m.tri(bot[a], bot[a + 1], bot[b]); m.tri(bot[a + 1], bot[b + 1], bot[b])
    for i in range(nu):                      # the two long edges, so the slab has thickness
        for j in (0, nv):
            a, b = i * (nv + 1) + j, (i + 1) * (nv + 1) + j
            m.tri(top[a], bot[a], top[b]); m.tri(top[b], bot[a], bot[b])
    return m


def cones(seed: int) -> Mesh:
    """A small cluster of fallen cones (ground v2): 2-6 scaled, slightly tilted ovoids built from
    scale rings (stacked short tubes), ~6-10 cm long."""
    rng = random.Random(seed)
    m = Mesh()
    for _ in range(rng.randint(2, 6)):
        cx, cy = rng.uniform(-12, 12), rng.uniform(-12, 12)
        a = rng.uniform(0, 2 * math.pi)
        L, R = rng.uniform(6, 10), rng.uniform(2.0, 3.2)
        d = (math.cos(a), math.sin(a), rng.uniform(-0.15, 0.15))
        base = (cx, cy, R)
        rings = 5
        for k in range(rings):               # scales: rings swelling then tapering
            t0, t1 = k / rings, (k + 1) / rings
            r0 = R * math.sin(math.pi * (0.15 + 0.7 * t0))
            r1 = R * math.sin(math.pi * (0.15 + 0.7 * t1)) * 0.85
            tube(m, [add(base, mul(d, L * t0)), add(base, mul(d, L * t1))], [r0, r1], 6, 0.45 + 0.1 * (k % 2))
    return m


_FALLEN_SPECIES = ("abies_grandis", "pseudotsuga_menziesii", "tsuga_heterophylla")


def _fallen(seed: int, foliage: bool) -> Mesh:
    """A whole fallen conifer (8g hung-up trees, Brad: 'just logs, no limbs'; foliage-carrying
    ones are ladder fuel): a real generated tree (treegen, lite detail) laid along +X from its
    butt at x = 0, plus a root wad of torn roots at the butt. foliage=False strips the needles
    (old dead stems). The renderer pitches it up from a foot on the ground to a contact point in
    a neighbour's crown."""
    import treegen

    rng = random.Random(seed)
    key = _FALLEN_SPECIES[seed % len(_FALLEN_SPECIES)]
    src = treegen.build(key, 1 + seed % 3, detail=0.45)
    m = Mesh()
    remap = {}
    for i, ((x, y, z), col) in enumerate(zip(src.verts, src.cols, strict=True)):
        if not foliage and col[3] > 0.5:
            continue
        remap[i] = len(m.verts)
        m.verts.append((z, y, -x))           # tree axis +Z -> +X (lying), butt at x = 0
        m.cols.append(col)
    for a, b, c in src.tris:
        if a in remap and b in remap and c in remap:
            m.tris.append((remap[a], remap[b], remap[c]))
    r = 18.0                                  # root wad: torn roots splayed around the butt
    for k in range(rng.randint(6, 10)):
        a = k * 2 * math.pi / 8 + rng.uniform(-0.3, 0.3)
        d = norm((-rng.uniform(0.1, 0.5), math.cos(a), math.sin(a)))
        L = rng.uniform(40, 110)
        tube(m, [(0.0, 0.0, 0.0), mul(d, L * 0.5), add(mul(d, L), (0.0, 0.0, -rng.uniform(0, 20)))],
             [r * 0.35, r * 0.2, r * 0.05], 4, 0.4)
    return m


def fallen(seed: int) -> Mesh:
    return _fallen(seed, True)


def fallenbare(seed: int) -> Mesh:
    return _fallen(seed, False)


# New items go at the END: each item's mesh seed comes from its index here.
ITEMS = {"fern": fern, "huckleberry": huckleberry, "shrub": shrub, "rock": rock, "log": log,
         "stump": stump, "snag": snag, "pole": pole, "stick": stick, "bark": bark, "cones": cones,
         "fallen": fallen, "fallenred": fallen, "fallenbare": fallenbare, "branch": branch}

# Material colours (sRGB hex, no '#'): foliage (Color), wood / stone (TrunkColor).
COLORS = {
    "fern": ("2C4220", "3A3222"),
    "huckleberry": ("A8401E", "4A3526"),   # fall: vine maple / huckleberry reds (field photo)
    "shrub": ("4A5E2C", "4A3A2A"),
    "rock": ("7A766E", "736E66"),
    "log": ("4E6A2C", "5C544A"),           # moss patches; weathered grey-brown bark (was rust 5A4834)
    "stump": ("5A5A40", "5E4A36"),
    "snag": ("5A5A40", "7A746A"),          # weathered silver-grey dead wood
    "pole": ("5A5A40", "5E4E3C"),
    "stick": ("5A5A40", "6A5A48"),        # weathered grey-brown
    "bark": ("5A5A40", "4E3426"),         # weathered dark bark (rust read as orange paint)
    "cones": ("5A5A40", "6A4428"),
    "fallen": ("33482A", "4E3A2C"),       # recently fallen: still-green needles
    "fallenred": ("8A4A22", "4E3A2C"),    # dead a season: red needles (ladder fuel)
    "fallenbare": ("5A5A40", "6A6258"),   # old: no needles, weathered grey
    "branch": ("5A5A40", "5E5246"),       # dead limb: grey-brown, darker than the bleached sticks
}


# Meshes whose origin is the trunk axis at the butt (the renderer anchors them by it), not the
# lowest point: a fallen tree's limbs hang below its stem.
AXIS_ANCHORED = {"fallen", "fallenred", "fallenbare"}


def build(key: str, variant: int) -> Mesh:
    m = ITEMS[key](7001 + 131 * list(ITEMS).index(key) + 977 * variant)
    if key in AXIS_ANCHORED:
        return m
    lo = min(v[2] for v in m.verts)
    m.verts = [(x, y, z - lo) for (x, y, z) in m.verts]  # stands on its lowest point
    return m


def asset_names() -> list[str]:
    out = []
    for k in ITEMS:
        out.append(f"Cover/MI_Cover_{k}")
        out += [f"Cover/SM_Cover_{k}_v{v}" for v in range(VARIANTS)]
    return out
