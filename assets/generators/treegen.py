"""Procedural tree geometry for the vegetation species set (EPIC_5_PLAN B3 v2).

Pure Python (no Unreal, no numpy) so it runs inside the editor's embedded Python *and* under
pytest / the offline preview. Every tree is built from growth-form parameters - trunk taper,
branch whorls, branch angle and droop, crown profile, needle sprays - with a fixed seed, so the
same parameters always give the same mesh.

Foliage is geometry, not alpha cards: flat needle sprays / leaf clusters are thin opaque
triangles with gaps between them, which Nanite rasterises cheaply (masked materials are its
expensive path) and which let light into a stand.

Conventions: Z up, centimetres, the tree stands on (0, 0, 0) and is H = 1000 cm tall (the
renderer fits every mesh's bounds to each instance's height and crown diameter, so only
proportions matter). Vertex colour RGB = a shading multiplier (the species colour comes from
its material instance); alpha = 1 foliage / 0 wood (M_Veg lerps bark -> foliage by it, and the
wind adds twig flutter to foliage only).
"""

from __future__ import annotations

import math
import random
from dataclasses import dataclass, field, replace

H = 1000.0  # nominal tree height, cm
GOLDEN_ANGLE = math.pi * (3.0 - math.sqrt(5.0))

Vec = tuple[float, float, float]


# --------------------------------------------------------------------------- vector helpers
def add(a: Vec, b: Vec) -> Vec:
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def sub(a: Vec, b: Vec) -> Vec:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def mul(a: Vec, s: float) -> Vec:
    return (a[0] * s, a[1] * s, a[2] * s)


def dot(a: Vec, b: Vec) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a: Vec, b: Vec) -> Vec:
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm(a: Vec) -> Vec:
    n = math.sqrt(dot(a, a))
    return (0.0, 0.0, 1.0) if n < 1e-12 else mul(a, 1.0 / n)


def perp(d: Vec) -> Vec:
    """Any unit vector perpendicular to d."""
    ref = (0.0, 0.0, 1.0) if abs(d[2]) < 0.9 else (1.0, 0.0, 0.0)
    return norm(cross(d, ref))


def direction(azimuth: float, elevation: float) -> Vec:
    ce = math.cos(elevation)
    return (ce * math.cos(azimuth), ce * math.sin(azimuth), math.sin(elevation))


# --------------------------------------------------------------------------- mesh
@dataclass
class Mesh:
    verts: list[Vec] = field(default_factory=list)
    tris: list[tuple[int, int, int]] = field(default_factory=list)
    cols: list[tuple[float, float, float, float]] = field(default_factory=list)

    def vert(self, p: Vec, shade: float, foliage: bool) -> int:
        self.verts.append(p)
        s = max(0.0, min(1.0, shade))
        self.cols.append((s, s, s, 1.0 if foliage else 0.0))
        return len(self.verts) - 1

    def tri(self, a: int, b: int, c: int) -> None:
        self.tris.append((a, b, c))

    def bounds(self) -> tuple[Vec, Vec]:
        xs, ys, zs = zip(*self.verts, strict=True)
        return (min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))


def tube(m: Mesh, path: list[Vec], radii: list[float], sides: int, shade: float,
         foliage: bool = False) -> None:
    """Tapered tube along a polyline (parallel-transported frames), closed at the tip."""
    n = len(path)
    t0 = norm(sub(path[1], path[0]))
    u = perp(t0)
    rings = []
    for i in range(n):
        t = norm(sub(path[min(i + 1, n - 1)], path[max(i - 1, 0)]))
        u = norm(sub(u, mul(t, dot(u, t))))  # transport the frame
        v = cross(t, u)
        ring = []
        for k in range(sides):
            a = 2.0 * math.pi * k / sides
            off = add(mul(u, math.cos(a) * radii[i]), mul(v, math.sin(a) * radii[i]))
            ring.append(m.vert(add(path[i], off), shade * (0.8 + 0.2 * math.cos(a)), foliage))
        rings.append(ring)
    for i in range(n - 1):
        for k in range(sides):
            a, b = rings[i][k], rings[i][(k + 1) % sides]
            c, d = rings[i + 1][k], rings[i + 1][(k + 1) % sides]
            m.tri(a, c, b)
            m.tri(b, c, d)
    tip = m.vert(add(path[-1], mul(norm(sub(path[-1], path[-2])), radii[-1])), shade, foliage)
    for k in range(sides):
        m.tri(rings[-1][k], tip, rings[-1][(k + 1) % sides])


def curve(start: Vec, d0: Vec, length: float, segments: int, droop: float,
          upturn: float) -> list[Vec]:
    """A branch path: leaves `start` along d0, sags by `droop` (fraction of length, mid-branch)
    and lifts its tip by `upturn`."""
    pts = [start]
    for i in range(1, segments + 1):
        t = i / segments
        p = add(start, mul(d0, length * t))
        sag = -droop * length * math.sin(math.pi * t) * 0.5 - droop * length * t * t * 0.5
        lift = upturn * length * t ** 3
        pts.append((p[0], p[1], p[2] + sag + lift))
    return pts


def spray(m: Mesh, rng: random.Random, base: Vec, d: Vec, length: float, width: float,
          leaflets: int, shade: float, droop: float, needle_len: float) -> None:
    """A flat conifer spray: a rachis with alternating side twigs, each a thin needle blade.
    Lies roughly in the horizontal plane (conifers hold foliage in flat sprays)."""
    side = norm(cross((0.0, 0.0, 1.0), d)) if abs(d[2]) < 0.95 else perp(d)
    up = norm(cross(d, side))
    tilt = rng.uniform(-0.25, 0.25)
    side = norm(add(side, mul(up, tilt)))
    prev = base
    for i in range(leaflets):
        t = (i + 1) / (leaflets + 1)
        c = add(base, mul(d, length * t))
        c = (c[0], c[1], c[2] - droop * length * t * t)
        taper = 1.0 - 0.6 * t
        for sgn in (-1.0, 1.0):
            tipdir = norm(add(mul(side, sgn), mul(d, 0.55)))
            tip = add(c, mul(tipdir, width * taper * rng.uniform(0.8, 1.15)))
            tip = (tip[0], tip[1], tip[2] - 0.15 * width * taper)
            w = mul(d, needle_len * taper)
            a = m.vert(sub(c, w), shade * rng.uniform(0.85, 1.0), True)
            b = m.vert(add(c, w), shade * rng.uniform(0.85, 1.0), True)
            e = m.vert(tip, shade * rng.uniform(0.9, 1.1), True)
            m.tri(a, e, b)
        # rachis segment as a thin blade
        n1 = m.vert(add(prev, mul(side, needle_len * 0.4)), shade * 0.8, True)
        n2 = m.vert(sub(prev, mul(side, needle_len * 0.4)), shade * 0.8, True)
        n3 = m.vert(c, shade * 0.8, True)
        m.tri(n1, n3, n2)
        prev = c


def leaf_cluster(m: Mesh, rng: random.Random, centre: Vec, radius: float, leaves: int,
                 leaf_len: float, shade: float) -> None:
    """Broadleaf foliage: a loose ball of small oval leaves (two triangles each)."""
    for _ in range(leaves):
        az = rng.uniform(0, 2 * math.pi)
        el = rng.uniform(-0.6, 1.2)
        r = radius * rng.random() ** 0.5
        p = add(centre, mul(direction(az, el), r))
        d = direction(rng.uniform(0, 2 * math.pi), rng.uniform(-0.9, 0.3))
        s = perp(d)
        w = leaf_len * 0.45
        a = m.vert(p, shade, True)
        half = mul(d, leaf_len * 0.5)
        b = m.vert(add(p, add(half, mul(s, w))), shade * rng.uniform(0.9, 1.1), True)
        c = m.vert(add(p, mul(d, leaf_len)), shade * rng.uniform(0.9, 1.1), True)
        e = m.vert(add(p, sub(half, mul(s, w))), shade * rng.uniform(0.9, 1.1), True)
        m.tri(a, b, c)
        m.tri(a, c, e)


# --------------------------------------------------------------------------- conifers
@dataclass(frozen=True)
class Conifer:
    """Growth form of a conifer. Fractions are of total height H unless noted."""
    seed: int
    crown_base: float          # live crown starts here (fraction of H)
    crown_width: float         # widest branch reach (fraction of H), one side
    profile: float             # branch length ~ (1 - t)^profile * bulge(t); 1 = cone, <1 = rounder
    bulge: float               # 0 = cone from the base; >0 = widest point lifted (rounder crowns)
    whorls: int                # branch tiers from crown base to top
    per_whorl: int             # branches per tier
    elev_base_deg: float       # branch angle at the crown base (negative = downswept)
    elev_top_deg: float        # ... at the top
    droop: float               # mid-branch sag (fraction of branch length)
    upturn: float              # tip lift (fraction of branch length)
    sprays_per_m: float        # needle sprays per metre of branch (at H = 10 m scale)
    spray_len: float           # spray length (fraction of H)
    spray_width: float         # side-twig reach (fraction of H)
    spray_droop: float         # pendant sprays (cedar, hemlock) > stiff (fir) ~ 0
    trunk_radius: float        # at the base (fraction of H)
    leader_droop: float = 0.0  # hemlocks: the top nods over
    flare: float = 1.0         # butt flare multiplier (redcedar > 1)
    tufts: bool = False        # pines: needles in tufts at branch ends, not flat sprays
    sparse: float = 1.0        # 0..1 fraction of sprays kept (larch is open)
    twig_order: int = 1        # 2 = secondary twigs off each branch (denser crowns)


def conifer(p: Conifer, detail: float = 1.0) -> Mesh:
    """`detail` < 1 keeps every trunk and branch (same seeds, same silhouette) and scales only
    the foliage count - the mid-tier "lite" mesh."""
    rng = random.Random(p.seed)
    m = Mesh()
    fol_n = [0]

    def frng() -> random.Random:  # foliage stream per branch: independent of foliage count
        fol_n[0] += 1
        return random.Random(p.seed * 7919 + fol_n[0])
    top = H
    # trunk: tapered, flared base, slightly wavy; hemlock leaders nod at the top
    pts, radii = [], []
    segs = 14
    for i in range(segs + 1):
        t = i / segs
        z = top * t
        x = rng.uniform(-0.004, 0.004) * H * math.sin(t * math.pi)
        y = rng.uniform(-0.004, 0.004) * H * math.sin(t * math.pi)
        if t > 0.9 and p.leader_droop:
            nod = (t - 0.9) / 0.1
            x += p.leader_droop * H * nod * nod
            z -= p.leader_droop * H * 0.5 * nod * nod
        flare = 1.0 + (p.flare - 1.0 + 0.25) * max(0.0, 1.0 - t / 0.06) ** 2
        pts.append((x, y, z))
        radii.append(max(0.6, p.trunk_radius * H * (1.0 - 0.97 * t) * flare))
    tube(m, pts, radii, 9, 0.9)

    def trunk_at(z: float) -> Vec:
        t = max(0.0, min(1.0, z / top))
        i = min(segs - 1, int(t * segs))
        f = t * segs - i
        a, b = pts[i], pts[i + 1]
        return add(a, mul(sub(b, a), f))

    zb = p.crown_base * H
    az = rng.uniform(0, 2 * math.pi)
    for w in range(p.whorls):
        t = (w + rng.uniform(0.1, 0.9)) / p.whorls  # 0 at crown base, 1 at top
        z = zb + (top * 0.985 - zb) * t
        shape = (1.0 - t) ** p.profile * (1.0 + p.bulge * math.sin(math.pi * min(1.0, t * 1.4)))
        length = max(0.012 * H, p.crown_width * H * shape)
        elev = math.radians(p.elev_base_deg + (p.elev_top_deg - p.elev_base_deg) * t)
        for _ in range(p.per_whorl):
            az += GOLDEN_ANGLE + rng.uniform(-0.3, 0.3)
            L = length * rng.uniform(0.75, 1.1)
            d0 = direction(az, elev + rng.uniform(-0.12, 0.12))
            base = trunk_at(z + rng.uniform(-0.01, 0.01) * H)
            path = curve(base, d0, L, 4, p.droop * rng.uniform(0.7, 1.3), p.upturn)
            r0 = max(0.25, 0.018 * L)
            tube(m, path, [r0 * (1.0 - 0.8 * k / 4) for k in range(5)], 4, 0.75)
            _foliage_along(m, frng(), p, path, L, t, detail)
            if p.twig_order > 1 and L > 0.05 * H:
                for j in range(2):
                    s = rng.uniform(0.3, 0.8)
                    i = min(3, int(s * 4))
                    b0 = add(path[i], mul(sub(path[i + 1], path[i]), s * 4 - i))
                    bd = norm(add(norm(sub(path[i + 1], path[i])),
                                  mul(perp(d0), 0.9 * (1 if j else -1))))
                    tp = curve(b0, bd, L * 0.4, 2, p.droop, p.upturn)
                    tube(m, tp, [r0 * 0.4, r0 * 0.25, r0 * 0.1], 3, 0.75)
                    _foliage_along(m, frng(), p, tp, L * 0.4, t, detail)
    # the leader's own foliage
    _foliage_along(m, frng(), p, [trunk_at(top * 0.9), pts[-1]], 0.1 * H, 1.0, detail)
    return m


def _foliage_along(m: Mesh, rng: random.Random, p: Conifer, path: list[Vec], length: float,
                   t_crown: float, detail: float = 1.0) -> None:
    shade = 0.72 + 0.28 * t_crown  # lower crown is shaded
    if p.tufts:  # pines: dense bottle-brush tufts of long needles on the outer branch
        for _ in range(max(4, round(7 * detail))):
            s = rng.uniform(0.55, 1.0)
            i = min(len(path) - 2, int(s * (len(path) - 1)))
            f = s * (len(path) - 1) - i
            c = add(path[i], mul(sub(path[i + 1], path[i]), f))
            d = norm(sub(path[i + 1], path[i]))
            for _ in range(9):
                nd = norm(add(d, (rng.uniform(-1.0, 1.0), rng.uniform(-1.0, 1.0),
                                  rng.uniform(0.0, 1.3))))
                spray(m, rng, c, nd, p.spray_len * H * 0.8, p.spray_width * H * 0.5, 3,
                      shade, 0.05, 0.004 * H)
        return
    n = max(1, int(1.25 * detail * p.sprays_per_m * length / 100.0 * rng.uniform(0.8, 1.2)))
    for _ in range(n):
        if rng.random() > p.sparse:
            continue
        s = rng.uniform(0.15, 1.0)
        i = min(len(path) - 2, int(s * (len(path) - 1)))
        f = s * (len(path) - 1) - i
        c = add(path[i], mul(sub(path[i + 1], path[i]), f))
        along = norm(sub(path[i + 1], path[i]))
        out = norm(add(along, mul(norm(cross(along, (0.0, 0.0, 1.0))),
                                  rng.choice((-1.0, 1.0)) * rng.uniform(0.3, 1.1))))
        # lite sprays: fewer, wider side twigs so the crown keeps its coverage
        spray(m, rng, c, out, p.spray_len * H * rng.uniform(0.7, 1.2),
              p.spray_width * H * rng.uniform(0.8, 1.2) * (1.0 + 0.4 * (1.0 - detail)),
              5 if detail >= 0.99 else 3, shade, p.spray_droop, 0.003 * H * (2.0 - detail))


# --------------------------------------------------------------------------- broadleaf
@dataclass(frozen=True)
class Broadleaf:
    seed: int
    bole: float            # clear trunk before the first fork (fraction of H)
    spread: float          # crown half-width (fraction of H)
    ascent_deg: float      # branch angle above horizontal
    orders: int            # branching depth
    children: int          # children per branch
    cluster_r: float       # leaf-cluster radius (fraction of H)
    leaves: int            # leaves per cluster
    leaf_len: float        # leaf length (fraction of H)
    trunk_radius: float
    stems: int = 1         # multi-stemmed shrubs (vine maple)
    stem_lean_deg: float = 0.0


def broadleaf(p: Broadleaf, detail: float = 1.0) -> Mesh:
    rng = random.Random(p.seed)
    lrng = random.Random(p.seed * 7919)  # leaves: independent of leaf count
    m = Mesh()
    nleaf = max(4, round(p.leaves * detail))
    lscale = 1.0 + 0.6 * (1.0 - detail)  # fewer, bigger leaves keep coverage

    def branch(start: Vec, d: Vec, length: float, radius: float, order: int) -> None:
        end = add(start, mul(d, length))
        mid = add(add(start, mul(d, length * 0.5)), (rng.uniform(-1, 1) * length * 0.05,
                                                      rng.uniform(-1, 1) * length * 0.05, 0.0))
        tube(m, [start, mid, end], [radius, radius * 0.75, radius * 0.5], 5 if order == 0 else 4,
             0.8)
        if order >= p.orders:
            leaf_cluster(m, lrng, end, p.cluster_r * H, nleaf, p.leaf_len * H * lscale,
                         0.7 + 0.3 * min(1.0, end[2] / H))
            return
        for _ in range(p.children):
            az = rng.uniform(0, 2 * math.pi)
            nd = norm(add(mul(d, 0.8), direction(az, math.radians(p.ascent_deg))))
            branch(end, nd, length * rng.uniform(0.55, 0.75), radius * 0.6, order + 1)
            if order >= 1 and rng.random() < 0.6:  # leaves along inner branches too
                leaf_cluster(m, lrng, add(start, mul(d, length * rng.uniform(0.4, 0.9))),
                             p.cluster_r * H * 0.7, max(2, nleaf // 2), p.leaf_len * H * lscale,
                             0.65)

    for s_i in range(p.stems):
        az = 2 * math.pi * s_i / p.stems + rng.uniform(-0.3, 0.3)
        multi = p.stems > 1
        lean = math.radians(90 - p.stem_lean_deg * (1 if multi else 0))
        d = norm(direction(az, lean))
        base = (rng.uniform(-1, 1) * 0.01 * H * multi, rng.uniform(-1, 1) * 0.01 * H * multi, 0.0)
        stem_len = H * (0.85 if not multi else 0.75)
        top = add(base, mul(d, stem_len))
        tube(m, [base, add(base, mul(d, stem_len * 0.5)), top],
             [p.trunk_radius * H, p.trunk_radius * H * 0.7, p.trunk_radius * H * 0.25], 7, 0.85)
        # branch tiers from the bole up the leader; longest in the lower-middle crown
        tiers = 7 if not multi else 3
        for k in range(tiers):
            t = (k + rng.uniform(0.2, 0.8)) / tiers
            at = add(base, mul(d, stem_len * (p.bole + (1.0 - p.bole) * t)))
            reach = p.spread * H * (0.45 + 0.9 * math.sin(math.pi * min(1.0, 0.25 + 0.8 * t)))
            for _ in range(p.children):
                az2 = rng.uniform(0, 2 * math.pi)
                nd = norm(add(mul(d, 0.35), direction(az2, math.radians(p.ascent_deg))))
                branch(at, nd, reach * rng.uniform(0.7, 1.0),
                       p.trunk_radius * H * 0.35 * (1.0 - 0.6 * t), 1)
        leaf_cluster(m, lrng, top, p.cluster_r * H * 1.5, nleaf * 2, p.leaf_len * H * lscale,
                     1.0)
    return m


# --------------------------------------------------------------------------- low plants
def sagebrush(seed: int) -> Mesh:
    rng = random.Random(seed)
    m = Mesh()
    for _ in range(9):  # gnarled stems fanning out, grey-green leaf clumps on top
        az = rng.uniform(0, 2 * math.pi)
        d = norm(direction(az, math.radians(rng.uniform(45, 75))))
        end = mul(d, H * rng.uniform(0.45, 0.75))
        tube(m, [(0.0, 0.0, 0.0), mul(end, 0.5), end], [0.012 * H, 0.009 * H, 0.005 * H], 4, 0.7)
        leaf_cluster(m, rng, end, 0.26 * H, 140, 0.035 * H, 0.9)
    return m


def bunchgrass(seed: int) -> Mesh:
    rng = random.Random(seed)
    m = Mesh()
    for _ in range(70):
        az = rng.uniform(0, 2 * math.pi)
        tilt = math.radians(rng.uniform(55, 88))
        h = H * rng.uniform(0.6, 1.0)
        base = (rng.uniform(-0.06, 0.06) * H, rng.uniform(-0.06, 0.06) * H, 0.0)
        d = direction(az, tilt)
        s = perp(d)
        w = 0.012 * H
        pts = curve(base, d, h, 3, 0.25, 0.0)
        prev_l = m.vert(sub(pts[0], mul(s, w)), 0.6, True)
        prev_r = m.vert(add(pts[0], mul(s, w)), 0.6, True)
        for k, q in enumerate(pts[1:], 1):
            ww = w * (1.0 - k / 3)
            left = m.vert(sub(q, mul(s, ww)), 0.6 + 0.4 * k / 3, True)
            right = m.vert(add(q, mul(s, ww)), 0.6 + 0.4 * k / 3, True)
            m.tri(prev_l, left, prev_r)
            m.tri(prev_r, left, right)
            prev_l, prev_r = left, right
    return m


# --------------------------------------------------------------------------- the species set
# Growth forms from field-guide descriptions of each species' mature crown (Silvics of North
# America; Pojar & MacKinnon). Colours live in the species' material instance (veg_species.py).
CONIFERS: dict[str, Conifer] = {
    # Douglas-fir: conical-to-irregular, horizontal branches that droop then turn up, pendant
    # twigs, self-pruned lower bole in stands
    "pseudotsuga_menziesii": Conifer(11, 0.38, 0.17, 0.95, 0.15, 16, 5, -8, 18, 0.35, 0.25,
                                     22, 0.045, 0.018, 0.25, 0.022, twig_order=2),
    # western hemlock: narrow cone, drooping leader, drooping feathery branches, lacy sprays
    "tsuga_heterophylla": Conifer(12, 0.22, 0.15, 1.0, 0.05, 20, 5, -22, 0, 0.45, 0.15,
                                  26, 0.04, 0.016, 0.5, 0.018, leader_droop=0.035,
                                  twig_order=2),
    # Pacific silver fir: symmetric narrow spire, stiff horizontal branches, dense upswept sprays
    "abies_amabilis": Conifer(13, 0.2, 0.12, 1.05, 0.0, 22, 5, -2, 22, 0.1, 0.2, 30, 0.04,
                              0.014, 0.05, 0.018, twig_order=2),
    # western redcedar: buttressed flared trunk, drooping branches with J-shaped upturned ends,
    # pendant flat lacy sprays
    "thuja_plicata": Conifer(14, 0.2, 0.17, 0.9, 0.2, 18, 5, -20, 5, 0.55, 0.45, 24, 0.05,
                             0.02, 0.7, 0.03, leader_droop=0.02, flare=1.8, twig_order=2),
    # mountain hemlock: narrow, drooping leader and branches, dense short sprays
    "tsuga_mertensiana": Conifer(15, 0.12, 0.13, 0.9, 0.1, 20, 5, -25, -5, 0.5, 0.15, 30,
                                 0.032, 0.013, 0.45, 0.02, leader_droop=0.03, twig_order=2),
    # subalpine fir: very narrow spire to the ground, short stiff branches
    "abies_lasiocarpa": Conifer(16, 0.04, 0.085, 1.1, 0.0, 26, 6, -12, 15, 0.15, 0.1, 44,
                                0.03, 0.013, 0.05, 0.016, twig_order=2),
    # Engelmann spruce: narrow cone, lower branches drooping, dense
    "picea_engelmannii": Conifer(17, 0.08, 0.12, 1.05, 0.0, 24, 6, -25, 10, 0.4, 0.2, 38,
                                 0.035, 0.014, 0.3, 0.018, twig_order=2),
    # grand fir: cone, horizontal slightly drooping branches, flat sprays in two ranks
    "abies_grandis": Conifer(18, 0.25, 0.15, 1.0, 0.1, 18, 5, -8, 12, 0.25, 0.15, 24, 0.045,
                             0.02, 0.15, 0.02, twig_order=2),
    # ponderosa pine: long clear bole, open irregular rounded crown, upturned branch ends with
    # tufts of long needles
    "pinus_ponderosa": Conifer(19, 0.5, 0.17, 0.55, 0.6, 14, 4, 5, 35, 0.2, 0.55, 0, 0.06,
                               0.025, 0.0, 0.025, tufts=True),
    # western larch: sparse open narrow crown, short horizontal branches, soft light foliage
    "larix_occidentalis": Conifer(20, 0.4, 0.1, 1.0, 0.1, 20, 5, -2, 15, 0.15, 0.1, 22,
                                  0.032, 0.013, 0.1, 0.018, sparse=0.8, twig_order=2),
}

BROADLEAF: dict[str, Broadleaf] = {
    # red alder: smooth pale bole, narrow domed crown of mid-size leaves
    "alnus_rubra": Broadleaf(31, 0.3, 0.2, 35, 2, 3, 0.07, 36, 0.022, 0.02),
    # black cottonwood: tall, ascending branches, open upright crown, broad leaves
    "populus_trichocarpa": Broadleaf(32, 0.4, 0.16, 55, 2, 3, 0.07, 34, 0.026, 0.024),
    # vine maple: multi-stemmed sprawling shrub, broad leaves
    "acer_circinatum": Broadleaf(33, 0.3, 0.45, 20, 2, 3, 0.1, 40, 0.05, 0.02, stems=5,
                                 stem_lean_deg=40),
}

LOW_PLANTS = {"artemisia_shrub": sagebrush, "bunchgrass": bunchgrass}

SPECIES = list(CONIFERS) + list(BROADLEAF) + list(LOW_PLANTS)

# Distinct meshes per species. The renderer picks one per instance from a hash of its position
# and adds per-instance scale/yaw/colour/lean on top, so neighbours never repeat.
VARIANTS = 4

# Species colours, sRGB hex: (foliage, bark). Foliage from field photos of each species' needles
# or leaves in summer; bark is a placeholder until licensed bark scans arrive (Brad, 2026-09-28).
COLORS: dict[str, tuple[str, str]] = {
    "pseudotsuga_menziesii": ("35492B", "4A3528"),
    "tsuga_heterophylla": ("33482A", "4B3A30"),
    "abies_amabilis": ("233D29", "6B6760"),
    "thuja_plicata": ("47582A", "6A4130"),
    "tsuga_mertensiana": ("2F4638", "4A3E38"),
    "abies_lasiocarpa": ("2C4336", "7A7870"),
    "picea_engelmannii": ("34494A", "5E4C40"),
    "abies_grandis": ("2F4A28", "5E5850"),
    "pinus_ponderosa": ("48572C", "8A5A3A"),
    "larix_occidentalis": ("5E7436", "6E4A36"),
    "alnus_rubra": ("4E6A2E", "9A9890"),
    "populus_trichocarpa": ("4C6A34", "8A8A80"),
    "acer_circinatum": ("5C7A2C", "5E5040"),
    "artemisia_shrub": ("7E8A72", "5A4E40"),
    "bunchgrass": ("A89E6E", "8A7E58"),
}


def _vary(value: float, rng: random.Random, spread: float) -> float:
    return value * (1.0 + rng.uniform(-spread, spread))


LITE_DETAIL = 0.45  # mid-tier meshes: same trees, under half the foliage


def build(key: str, variant: int = 0, detail: float = 1.0) -> Mesh:
    """Variant 0 is the species' reference form; others jitter it within natural ranges
    (crown width, whorl count, droop, crown base) and re-seed every random choice. Nothing
    goes below the ground plane (the renderer stands each mesh on its lowest point)."""
    m = _build(key, variant, detail)
    m.verts = [(x, y, max(0.0, z)) for (x, y, z) in m.verts]
    return m


def _build(key: str, variant: int, detail: float) -> Mesh:
    rng = random.Random(1009 * (SPECIES.index(key) + 1) + 7919 * variant)
    if key in CONIFERS:
        p = CONIFERS[key]
        if variant:
            p = replace(p, seed=p.seed + 101 * variant,
                        crown_base=min(0.7, _vary(p.crown_base, rng, 0.25)),
                        crown_width=_vary(p.crown_width, rng, 0.15),
                        whorls=max(6, p.whorls + rng.randint(-2, 2)),
                        droop=_vary(p.droop, rng, 0.3), upturn=_vary(p.upturn, rng, 0.3),
                        bulge=max(0.0, p.bulge + rng.uniform(-0.1, 0.15)))
        return conifer(p, detail)
    if key in BROADLEAF:
        p = BROADLEAF[key]
        if variant:
            p = replace(p, seed=p.seed + 101 * variant, bole=_vary(p.bole, rng, 0.2),
                        spread=_vary(p.spread, rng, 0.15),
                        ascent_deg=p.ascent_deg + rng.uniform(-8, 8))
        return broadleaf(p, detail)
    if key in LOW_PLANTS:
        return LOW_PLANTS[key](41 + SPECIES.index(key) + 101 * variant)
    raise KeyError(f"no growth form for species {key!r}")


def has_lite(key: str) -> bool:
    """Trees get a mid-tier lite mesh; shrubs and grass are culled in the mid tier anyway."""
    return key in CONIFERS or key in BROADLEAF


def asset_names() -> list[str]:
    """Every asset veg_species.py writes (the manifest must list exactly these)."""
    out = []
    for k in SPECIES:
        out.append(f"Veg/MI_Veg_{k}")
        out += [f"Veg/SM_{k}_v{v}" for v in range(VARIANTS)]
        if has_lite(k):
            out += [f"Veg/SM_{k}_v{v}_lite" for v in range(VARIANTS)]
    return out
