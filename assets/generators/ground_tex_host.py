"""Host-side texture synthesis for the ground detail sets (ground plane v1, EPIC_5_PLAN 8f GP3).

Runs in the terrain env (numpy + Pillow), not inside UE: `t_ground.py` (the UE generator) calls it
with an output folder, then imports the PNGs. Deterministic (fixed seeds), no source art.

Per set <name> in {litter, grass, rock, shrub}, two tileable RGBA8 PNGs, SIZE x SIZE:
    T_Ground_<Name>_C   sRGB   RGB = colour, normalised so its mean is mid-grey (0.5) per channel:
                               M_Terrain multiplies the macro colour by 2 x C near the camera, so
                               the map colour stays the average and the detail only varies it.
                        A     = height 0..1 (height-blended transitions between sets)
    T_Ground_<Name>_N   linear R, G = surface slope x, y packed 0..1 (world-planar normal offset),
                               B = roughness, A = ambient occlusion

Tileable by construction: every field is synthesised on a torus (periodic spectral noise, stamps
wrapped around the edges).

    python ground_tex_host.py <out_dir> [--size 1024] [--preview]
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

SIZE = 1024


# ---------------------------------------------------------------- periodic building blocks ----
def spectral(n: int, rng, beta: float = 2.0, lo: float = 1.0, hi: float | None = None,
             aniso: tuple[float, float] = (1.0, 1.0)) -> np.ndarray:
    """Periodic noise with a 1/f^beta amplitude spectrum between frequencies lo..hi (cycles per
    tile), standardised to mean 0, std 1. `aniso` stretches the spectrum (streaks)."""
    fx = np.fft.fftfreq(n) * n
    fy = np.fft.fftfreq(n) * n
    kx, ky = np.meshgrid(fx * aniso[0], fy * aniso[1])
    k = np.sqrt(kx * kx + ky * ky)
    amp = np.where(k > 0, 1.0 / np.maximum(k, 1e-6) ** (beta / 2.0), 0.0)
    amp[k < lo] = 0.0
    if hi is not None:
        amp *= np.exp(-(k / hi) ** 4)
    phase = rng.uniform(0, 2 * np.pi, (n, n))
    f = np.real(np.fft.ifft2(amp * np.exp(1j * phase)))
    return (f - f.mean()) / (f.std() + 1e-12)


def stamp_lines(n: int, rng, count: int, length: tuple[float, float], width: float,
                angle=None) -> tuple[np.ndarray, np.ndarray]:
    """Short line segments (needles, blades, twigs) splatted on a torus. Returns (height, id)
    where id is a per-segment random 0..1 (for colour variety), last writer wins."""
    h = np.zeros((n, n), np.float32)
    ident = np.zeros((n, n), np.float32)
    for _ in range(count):
        x0, y0 = rng.uniform(0, n, 2)
        a = rng.uniform(0, np.pi) if angle is None else angle + rng.normal(0, 0.25)
        L = rng.uniform(*length)
        steps = max(2, int(L * 1.5))
        t = np.linspace(0, 1, steps)
        xs = (x0 + np.cos(a) * L * t).astype(int) % n
        ys = (y0 + np.sin(a) * L * t).astype(int) % n
        prof = np.sin(np.pi * t) ** 0.5  # thicker in the middle
        v = rng.uniform(0, 1)
        lift = rng.uniform(0.6, 1.0)
        for dx in range(-int(width), int(width) + 1):
            for dy in range(-int(width), int(width) + 1):
                if dx * dx + dy * dy > width * width + 0.5:
                    continue
                xx, yy = (xs + dx) % n, (ys + dy) % n
                val = prof * lift * (1.0 - (dx * dx + dy * dy) / (width * width + 1.0))
                upd = val > h[yy, xx]
                h[yy[upd], xx[upd]] = val[upd]
                ident[yy[upd], xx[upd]] = v
    return h, ident


def voronoi(n: int, rng, cells: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Periodic Voronoi: (distance to nearest site, edge distance F2-F1, site id 0..1)."""
    pts = rng.uniform(0, n, (cells, 2))
    ids = rng.uniform(0, 1, cells)
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float32)
    f1 = np.full((n, n), 1e9, np.float32)
    f2 = np.full((n, n), 1e9, np.float32)
    idm = np.zeros((n, n), np.float32)
    for (px, py), sid in zip(pts, ids, strict=True):
        dx = np.abs(xx - px)
        dx = np.minimum(dx, n - dx)
        dy = np.abs(yy - py)
        dy = np.minimum(dy, n - dy)
        d = np.sqrt(dx * dx + dy * dy)
        closer = d < f1
        f2 = np.where(closer, f1, np.minimum(f2, d))
        idm = np.where(closer, sid, idm)
        f1 = np.where(closer, d, f1)
    return f1, f2 - f1, idm


def blur(a: np.ndarray, r: float) -> np.ndarray:
    """Periodic gaussian blur via FFT (sigma r texels)."""
    n = a.shape[0]
    f = np.fft.fftfreq(n)
    kx, ky = np.meshgrid(f, f)
    g = np.exp(-2 * (np.pi ** 2) * (r ** 2) * (kx * kx + ky * ky))
    return np.real(np.fft.ifft2(np.fft.fft2(a) * g))


def palette(t: np.ndarray, stops: list[tuple[float, str]]) -> np.ndarray:
    """Map t in 0..1 through sRGB hex colour stops -> float RGB 0..1 (sRGB)."""
    xs = [s for s, _ in stops]
    cols = np.array([[int(h[i:i + 2], 16) / 255.0 for i in (1, 3, 5)] for _, h in stops])
    return np.stack([np.interp(t, xs, cols[:, c]) for c in range(3)], -1)


CHROMA = 0.45    # keep this much of each texel's colour deviation from its own luminance
CONTRAST = 0.8   # and this much of the luminance contrast (the macro colour carries the hue)


def finish(rgb: np.ndarray, height: np.ndarray, rough: np.ndarray, relief: float):
    """-> (C RGBA uint8, N RGBA uint8). Colour: chroma and contrast softened, then mean-normalised
    to 0.5 per channel (sRGB) so 2 x C is a multiplier with mean 1; normal offset from the
    periodic height gradient; AO from local height."""
    rgb = np.clip(rgb, 0, 1)
    luma = rgb @ np.array([0.2126, 0.7152, 0.0722])
    lm = luma.mean()
    luma2 = lm + (luma - lm) * CONTRAST
    rgb = luma2[..., None] + (rgb - luma[..., None]) * CHROMA
    rgb = np.clip(rgb * (0.5 / rgb.reshape(-1, 3).mean(0)), 0, 1)
    h = (height - height.min()) / (np.ptp(height) + 1e-9)
    gx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5 * relief
    gy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5 * relief
    ao = np.clip(1.0 - np.clip(blur(h, 6) - h, 0, None) * 3.0, 0.35, 1.0)
    c = np.dstack([rgb, h[..., None]])
    nrm = np.dstack([np.clip(-gx * 0.5 + 0.5, 0, 1), np.clip(-gy * 0.5 + 0.5, 0, 1),
                     np.clip(rough, 0, 1), ao])
    return (np.round(c * 255).astype(np.uint8), np.round(nrm * 255).astype(np.uint8))


# ---------------------------------------------------------------------------- the sets ----
def litter(n: int):
    """Conifer forest floor: dark duff, a mat of needles (rust / tan / grey), twigs, cones, moss."""
    rng = np.random.default_rng(101)
    base = spectral(n, rng, 2.2, 1, 48)
    duff = palette(np.clip(base * 0.18 + 0.5, 0, 1),
                   [(0, "#2A1E14"), (0.5, "#3D2B1C"), (1, "#5A4430")])
    s = n / 1024
    nh, nid = stamp_lines(n, rng, int(26000 * s * s), (10 * s, 22 * s), 0.8)
    needle_col = palette(nid, [(0, "#6B4A2C"), (0.35, "#8C6A42"), (0.6, "#A38356"),
                               (0.8, "#7A5E3E"),
                               (1, "#5E5446")])
    th, _ = stamp_lines(n, rng, int(220 * s * s), (40 * s, 120 * s), 2.2 * s)
    twig_col = np.broadcast_to(np.array([0.30, 0.22, 0.15]), (n, n, 3))
    moss_m = np.clip(spectral(n, rng, 3.0, 2, 24) * 0.9 - 0.9, 0, 1)
    moss_col = palette(np.clip(spectral(n, rng, 2.0, 4, 200) * 0.2 + 0.5, 0, 1),
                       [(0, "#3F5226"), (1, "#6B7A36")])
    rgb = duff.copy()
    m = (nh > 0.05)[..., None]
    rgb = np.where(m, needle_col * (0.75 + 0.35 * nh[..., None]), rgb)
    rgb = rgb * (1 - moss_m[..., None]) + moss_col * moss_m[..., None]
    tm = (th > 0.1)[..., None]
    rgb = np.where(tm, twig_col * (0.8 + 0.3 * th[..., None]), rgb)
    height = base * 0.15 + nh * 0.5 + th * 1.2 + moss_m * 0.6
    rough = 0.82 + 0.1 * (1 - nh) - 0.1 * moss_m
    return finish(rgb, height, rough, relief=6.0)


def grass(n: int):
    """Dry-season grass: cured straw and green blades, streaky, soil showing between tufts."""
    rng = np.random.default_rng(202)
    tuft = np.clip(spectral(n, rng, 2.4, 3, 64) * 0.5 + 0.55, 0, 1)
    soil = palette(np.clip(spectral(n, rng, 2.0, 2, 128) * 0.15 + 0.5, 0, 1),
                   [(0, "#4A3C2A"), (1, "#6E5B40")])
    s = n / 1024
    bh, bid = stamp_lines(n, rng, int(30000 * s * s), (14 * s, 36 * s), 0.9)
    greenness = np.clip(spectral(n, rng, 2.5, 2, 32) * 0.35 + 0.35, 0, 1)
    straw = palette(bid, [(0, "#9C8756"), (0.5, "#B8A268"), (1, "#C9B47C")])
    green = palette(bid, [(0, "#5A6A34"), (1, "#7F8C48")])
    blade = straw * (1 - greenness[..., None]) + green * greenness[..., None]
    cover = (bh > 0.05) & (tuft > 0.4)
    rgb = np.where(cover[..., None], blade * (0.7 + 0.4 * bh[..., None]), soil)
    height = tuft * 0.6 + bh * cover * 0.9
    rough = np.where(cover, 0.75, 0.95)
    return finish(rgb, height, rough, relief=5.0)


def rock(n: int):
    """Talus / scree: separate rounded stones (Voronoi cells inset from their edges, so they read
    as stones, not cracked mud), darker fines and pebbles in the gaps, grain, lichen."""
    rng = np.random.default_rng(303)
    s = n / 1024
    f1, edge, sid = voronoi(n, rng, int(110 * s * s) + 8)
    g1, gedge, gid = voronoi(n, rng, int(500 * s * s) + 30)
    small = spectral(n, rng, 2.0, 2, 16) > 0.25          # patches of finer scree
    edge = np.where(small, gedge * 1.6, edge)
    sid = np.where(small, gid, sid)
    warp = spectral(n, rng, 2.2, 6, 64) * 3.0 * s       # irregular stone outlines
    gapw = 7.0 * s
    body = np.clip((edge + warp - gapw) / (14.0 * s), 0, 1)  # 0 in the gap, 1 inside a stone
    dome = body ** 0.5
    stone_m = body > 0.02
    grain = spectral(n, rng, 1.6, 8, None)
    stone = palette(np.clip(sid * 0.8 + grain * 0.06 + 0.1, 0, 1),
                    [(0, "#5E5A55"), (0.4, "#7D776F"), (0.75, "#948C80"), (1, "#6E6154")])
    fines = palette(np.clip(spectral(n, rng, 2.0, 4, 256) * 0.2 + 0.4, 0, 1),
                    [(0, "#2F2A25"), (1, "#4E463D")])
    peb_h = np.clip(spectral(n, rng, 1.2, 60, 200) * 1.2 - 1.0, 0, 1) * ~stone_m
    lichen = np.clip(spectral(n, rng, 2.8, 6, 96) * 0.8 - 1.1, 0, 1) * (dome > 0.5)
    lichen_col = palette(np.clip(sid, 0, 1), [(0, "#9A9A62"), (0.5, "#B8B090"), (1, "#8C7A3E")])
    rgb = np.where(stone_m[..., None], stone * (0.7 + 0.3 * dome[..., None]),
                   fines * (1.0 + 0.6 * peb_h[..., None]))
    rgb = rgb * (1 - lichen[..., None]) + lichen_col * lichen[..., None]
    height = dome * 1.0 + peb_h * 0.25 + grain * 0.03
    rough = np.where(stone_m, 0.7 + 0.1 * grain, 0.95)
    return finish(rgb, height, rough, relief=10.0)


def shrub(n: int):
    """Shrub ground: dark soil, low leafy plants, fallen leaves in fall colour (vine maple and
    huckleberry reds and oranges - Three Queens field photo), a few pebbles."""
    rng = np.random.default_rng(404)
    soil = palette(np.clip(spectral(n, rng, 2.2, 2, 96) * 0.15 + 0.5, 0, 1),
                   [(0, "#2E2419"), (1, "#4C3B28")])
    clump = np.clip(spectral(n, rng, 2.6, 3, 40) * 0.6 + 0.45, 0, 1)
    s = n / 1024
    yy, xx = np.mgrid[0:n, 0:n]
    leaf_h = np.zeros((n, n), np.float32)
    leaf_id = np.zeros((n, n), np.float32)
    for _ in range(int(5200 * s * s)):  # fallen leaves: small ellipses, wrapped
        cx, cy = rng.uniform(0, n, 2)
        rx, ry = rng.uniform(4, 9) * s, rng.uniform(2.5, 5) * s
        a = rng.uniform(0, np.pi)
        x0, x1 = int(cx - 10 * s), int(cx + 10 * s) + 1
        y0, y1 = int(cy - 10 * s), int(cy + 10 * s) + 1
        X, Y = np.meshgrid(np.arange(x0, x1), np.arange(y0, y1))
        u = (X - cx) * np.cos(a) + (Y - cy) * np.sin(a)
        v = -(X - cx) * np.sin(a) + (Y - cy) * np.cos(a)
        inside = (u / rx) ** 2 + (v / ry) ** 2 <= 1
        Xi, Yi = X[inside] % n, Y[inside] % n
        leaf_h[Yi, Xi] = rng.uniform(0.5, 1.0)
        leaf_id[Yi, Xi] = rng.uniform(0, 1)
    leaf_col = palette(leaf_id, [(0, "#8E2A18"), (0.3, "#B8481E"), (0.55, "#C87A2A"),
                                 (0.8, "#9C8A34"),
                                 (1, "#5E6A2E")])
    plant = palette(np.clip(spectral(n, rng, 2.0, 8, 256) * 0.2 + 0.5, 0, 1),
                    [(0, "#34431F"), (1, "#566A2E")])
    rgb = soil * (1 - clump[..., None] * 0.7) + plant * clump[..., None] * 0.7
    lm = (leaf_h > 0)[..., None]
    rgb = np.where(lm, leaf_col, rgb)
    height = clump * 0.8 + leaf_h * 0.25
    rough = np.where(leaf_h > 0, 0.7, 0.9)
    return finish(rgb, height, rough, relief=5.0)


SETS = {"Litter": litter, "Grass": grass, "Rock": rock, "Shrub": shrub}


def main(argv: list[str]) -> int:
    from PIL import Image

    out = Path(argv[0])
    size = int(argv[argv.index("--size") + 1]) if "--size" in argv else SIZE
    out.mkdir(parents=True, exist_ok=True)
    previews = []
    for name, fn in SETS.items():
        c, nrm = fn(size)
        Image.fromarray(c, "RGBA").save(out / f"T_Ground_{name}_C.png")
        Image.fromarray(nrm, "RGBA").save(out / f"T_Ground_{name}_N.png")
        previews.append(c[..., :3])
        print(f"T_Ground_{name}: {size}x{size}")
    if "--preview" in argv:
        # as rendered: each tile x its class's macro colour (terrain_default.toml), tiled 2x2 so
        # seams would show
        macro = {"Litter": "#55553B", "Grass": "#A89A6A", "Rock": "#7F7A72", "Shrub": "#6E7148"}
        tiles = []
        for (name, p) in zip(SETS, previews, strict=True):
            m = np.array([int(macro[name][i:i + 2], 16) for i in (1, 3, 5)], np.float32)
            t = np.clip(m * (p.astype(np.float32) / 127.5), 0, 255).astype(np.uint8)
            tiles.append(np.tile(t, (2, 2, 1)))
        Image.fromarray(np.concatenate(tiles, 1)).save(out / "preview.png")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
