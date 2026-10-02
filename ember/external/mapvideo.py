"""A conventional 2D forecast map of a fire timeline, frame-synced with our 3D render (Brad
2026-10-02: show our output beside theirs; PyreCast's viewer no longer has the run, so the left
panel is the forecast drawn the way forecast map viewers draw it - arrival time in colour bands
with isochrone lines, the active front, a time slider - from the same published files).

It is OUR drawing of THEIR data in the conventional style, not footage of their software, and is
labelled so on every frame (no PyreCast branding). PyreCast-derived output is internal-only
(EXTERNAL_SOURCES_PLAN rule 1).
"""
from __future__ import annotations

import shutil
import subprocess
from datetime import datetime, timedelta
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

from ember.external.composite import _pack, sample_onto
from ember.external.pyrecast import ATTRIBUTION
from ember.external.timeline import FireTimeline

# arrival-time bands (h since the run) -> colour: the usual yellow -> red -> purple ramp
BAND_H = 12
RAMP = [(255, 237, 100), (253, 190, 60), (245, 130, 40), (225, 70, 40), (190, 30, 70),
        (140, 20, 110), (90, 20, 130)]


def _band_colour(h: np.ndarray) -> np.ndarray:
    k = np.clip((h // BAND_H).astype(int), 0, len(RAMP) - 1)
    return np.array(RAMP, np.float32)[k]


def _font(size: int):
    from PIL import ImageFont

    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


class ForecastMap:
    def __init__(self, tl: FireTimeline, pack_dir: Path, size=(960, 540), margin_m=1500.0,
                 horizon_h: float = 72.0):
        m, obs, _ = _pack(pack_dir)
        self.grid = m["grid"]
        g = self.grid
        t0 = datetime.fromisoformat(m["t0_utc"].replace("Z", "+00:00"))
        self.run = datetime.fromisoformat(tl.t_ref_utc.replace("Z", "+00:00"))
        R = int(round((self.run - t0).total_seconds()))
        rr, cc, _ = sample_onto(tl, g)
        inside = (rr >= 0) & (cc >= 0)
        r0, c0 = np.where(inside, rr, 0), np.where(inside, cc, 0)
        self.fa_h = np.where(inside, tl.arrival_s[r0, c0], np.nan) / 3600.0
        self.before = (inside & tl.burned_before[r0, c0]) | ((obs >= 0) & (obs <= R))
        hs = np.frombuffer((Path(pack_dir) / "hillshade.bin").read_bytes(), np.uint8)
        hs = hs.reshape(g["ny"], g["nx"]).astype(np.float32)
        # crop: everything burned by the horizon + a margin, widened to the panel's aspect
        burn = self.before | (np.isfinite(self.fa_h) & (self.fa_h <= horizon_h))
        ys, xs = np.nonzero(burn)
        pad = int(margin_m / g["cell_size_m"])
        y0, y1 = max(0, ys.min() - pad), min(g["ny"], ys.max() + pad)
        x0, x1 = max(0, xs.min() - pad), min(g["nx"], xs.max() + pad)
        want = size[0] / size[1]
        h, w = y1 - y0, x1 - x0
        if w / h < want:
            grow = int(h * want) - w
            x0, x1 = max(0, x0 - grow // 2), min(g["nx"], x1 + grow - grow // 2)
        else:
            grow = int(w / want) - h
            y0, y1 = max(0, y0 - grow // 2), min(g["ny"], y1 + grow - grow // 2)
        self.win = (slice(y0, y1), slice(x0, x1))
        self.size = size
        self.horizon_h = horizon_h
        base = 70 + 0.6 * hs[self.win]
        self.base = np.stack([base * 0.95, base * 0.98, base * 1.0], -1)

    def frame(self, t_h: float, member: str) -> Image.Image:
        fa = self.fa_h[self.win]
        img = self.base.copy()
        before = self.before[self.win]
        img[before] = img[before] * 0.35 + np.array([70, 62, 58], np.float32) * 0.65
        lit = np.isfinite(fa) & (fa <= t_h) & ~before
        img[lit] = img[lit] * 0.25 + _band_colour(fa[lit]) * 0.75
        # isochrone lines at every band boundary already reached
        band = np.where(lit, (np.nan_to_num(fa, nan=0.0) // BAND_H).astype(int), -1)
        edge = np.zeros_like(lit)
        for dy, dx in ((0, 1), (1, 0)):
            a, b = band, np.roll(np.roll(band, dy, 0), dx, 1)
            edge |= (a != b) & ((a >= 0) | (b >= 0))
        img[edge & lit] = (40, 25, 30)
        front = np.isfinite(fa) & (fa > t_h - 1.0) & (fa <= t_h)
        img[front] = (255, 255, 255)
        # the fire's outline (all burned now)
        burned = before | lit
        out = burned & ~(np.roll(burned, 1, 0) & np.roll(burned, -1, 0) & np.roll(burned, 1, 1)
                         & np.roll(burned, -1, 1))
        img[out] = (15, 15, 15)
        im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).resize(self.size, Image.NEAREST)
        return self._chrome(im, t_h, member)

    def _chrome(self, im: Image.Image, t_h: float, member: str) -> Image.Image:
        W, H = im.size
        d = ImageDraw.Draw(im, "RGBA")
        now = self.run + timedelta(hours=t_h)
        d.rectangle([0, 0, W, 30], fill=(0, 0, 0, 150))
        d.text((10, 7), f"Forecast {member}  |  run {self.run:%b %d %H:%M} UTC  +{t_h:5.1f} h  "
               f"({now:%b %d %H:%M} UTC)", font=_font(16), fill=(255, 255, 255))
        # legend
        lx, ly = W - 150, 40
        d.rectangle([lx - 8, ly - 6, W - 6, ly + 18 * (len(RAMP) + 2) + 4], fill=(0, 0, 0, 140))
        d.text((lx, ly), "arrival (h after run)", font=_font(12), fill=(230, 230, 230))
        for i, c in enumerate(RAMP):
            y = ly + 18 * (i + 1)
            d.rectangle([lx, y, lx + 16, y + 12], fill=c)
            hi = f"{BAND_H * (i + 1)}" if i < len(RAMP) - 1 else "+"
            d.text((lx + 22, y - 1), f"{BAND_H * i}-{hi}", font=_font(12), fill=(230, 230, 230))
        y = ly + 18 * (len(RAMP) + 1)
        d.rectangle([lx, y, lx + 16, y + 12], fill=(70, 62, 58))
        d.text((lx + 22, y - 1), "burned at run", font=_font(12), fill=(230, 230, 230))
        # time slider
        sy = H - 26
        d.rectangle([0, sy - 8, W, H], fill=(0, 0, 0, 150))
        d.line([20, sy + 4, W - 20, sy + 4], fill=(200, 200, 200), width=3)
        for k in range(0, int(self.horizon_h) + 1, BAND_H):
            x = 20 + (W - 40) * k / self.horizon_h
            d.line([x, sy - 2, x, sy + 10], fill=(200, 200, 200), width=1)
        kx = 20 + (W - 40) * min(t_h, self.horizon_h) / self.horizon_h
        d.ellipse([kx - 7, sy - 3, kx + 7, sy + 11], fill=(255, 200, 80))
        return im


BANNER_H = 96


def _banner(width: int, left: str, right: str, t_h: float, run: datetime) -> Image.Image:
    """The header: the elapsed-since-run counter centred on its own row (Brad 2026-10-02), the
    panel titles under it, and the INTERNAL / attribution line."""
    b = Image.new("RGB", (width, BANNER_H), (14, 14, 18))
    d = ImageDraw.Draw(b)
    half = width // 2
    counter = f"+{t_h:04.1f} h since the run"
    when = (f"run {run:%b %d %H:%M} UTC  ->  now {run + timedelta(hours=t_h):%b %d %H:%M} UTC")
    f = _font(28)
    tw = d.textlength(counter, font=f)
    d.rectangle([half - tw / 2 - 16, 2, half + tw / 2 + 16, 38], fill=(44, 36, 18))
    d.text((half - tw / 2, 4), counter, font=f, fill=(255, 235, 170))
    d.text((half + tw / 2 + 28, 14), when, font=_font(14), fill=(190, 190, 190))
    d.text((12, 46), left, font=_font(18), fill=(255, 220, 120))
    d.text((half + 12, 46), right, font=_font(18), fill=(255, 220, 120))
    d.text((12, 74), f"INTERNAL - not for distribution. {ATTRIBUTION}. Left: drawn by Ember "
           "from PyreCast's published forecast files in the conventional 2D style - not "
           "PyreCast's viewer.", font=_font(13), fill=(255, 160, 160))
    return b


def side_by_side(fmap: ForecastMap, render_mp4: Path, t_from_h: float, t_to_h: float,
                 frames: int, fps: int, member: str, out: Path) -> Path:
    """The 2D map (left) frame-synced to our render's timelapse (right): frame i at
    t_from + i (t_to - t_from) / (frames - 1), as the harness spaces orbit frames."""
    ff = shutil.which("ffmpeg")
    if not ff:
        raise RuntimeError("ffmpeg not found")
    W, H = fmap.size
    # the left panel with the header above it (the header spans both panels: 2W wide)
    left = out.with_name(out.stem + ".map.mp4")
    p = subprocess.Popen([ff, "-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
                          "-s", f"{2 * W}x{H + BANNER_H}", "-r", str(fps), "-i", "-",
                          "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18", str(left)],
                         stdin=subprocess.PIPE)
    for i in range(frames):
        t = t_from_h + i * (t_to_h - t_from_h) / max(1, frames - 1)
        f = Image.new("RGB", (2 * W, H + BANNER_H), (0, 0, 0))
        f.paste(_banner(2 * W, "Forecast map (2D) - the conventional view",
                        "Ember (3D) - the same forecast data", t, fmap.run), (0, 0))
        f.paste(fmap.frame(t, member).convert("RGB"), (0, BANNER_H))
        p.stdin.write(f.tobytes())
    p.stdin.close()
    if p.wait() != 0:
        raise RuntimeError("ffmpeg failed encoding the map")
    subprocess.run([ff, "-y", "-v", "error", "-i", str(left), "-i", str(render_mp4),
                    "-filter_complex", f"[1:v]scale={W}:{H}[r];[0:v][r]overlay={W}:{BANNER_H}",
                    "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", str(out)], check=True)
    left.unlink(missing_ok=True)
    return out
