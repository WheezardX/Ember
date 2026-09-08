"""DISPOSABLE 2D debug renderer for sim runs (Epic 4 plan D6 Tier 1 / workstream G1).

This is NOT Epic 5. It exists so every checkpoint yields an archivable, CI-diffable artifact
(PNG frames -> MP4) and so a reviewer can judge truth, not beauty: hillshade + fuel-class tint
underlay, fire phases/intensity, suppression overlays, spot arcs, and a HUD strip. numpy +
Pillow only in the frame path; ffmpeg (on PATH) for encoding. Output is deterministic (no
timestamps in PNG metadata). Nothing here may grow into a renderer.
"""

from __future__ import annotations

import math
import shutil
import subprocess
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from ember.sim.stream import Frame, StreamHeader, iter_frames
from ember.sim.worldpack import (
    CLASS_NAMES,
    INT32_MIN,
    WorldPack,
    fuel_class_array,
    hillshade_from_elevation,
    load_worldpack,
)

MAX_WIDTH = 1600
HUD_H = 52
LEGEND_H = 64

# Fuel class tints: NB grey, GR yellow, GS olive, SH orange-brown, TU/TL greens, SB brown.
FUEL_CLASS_RGB = np.array([
    [120, 120, 120], [232, 208, 84], [156, 164, 70], [196, 122, 58],
    [76, 140, 72], [36, 104, 56], [124, 82, 48],
], dtype=np.uint8)
FUEL_TINT_ALPHA = 0.38
PHASE_BURNED = np.array([38, 34, 34], np.uint8)
INTENSITY_RGB = np.array([[255, 150, 0], [255, 190, 40], [255, 110, 0], [232, 24, 0]], np.uint8)
OVERLAY_RGB = {1: (0, 230, 230), 2: (220, 40, 200), 3: (60, 110, 255), 4: (255, 255, 255),
               5: (160, 210, 255)}
PERSISTENT_OVERLAY = (1, 2, 3)

_RAMP_ANCHORS = np.array([[68, 1, 84], [59, 82, 139], [33, 145, 140], [94, 201, 98],
                          [253, 231, 37]], dtype=np.float64)  # viridis-ish


def _ramp(n: int = 256) -> np.ndarray:
    xs = np.linspace(0, 1, len(_RAMP_ANCHORS))
    t = np.linspace(0, 1, n)
    return np.stack([np.interp(t, xs, _RAMP_ANCHORS[:, c]) for c in range(3)], axis=1).astype(
        np.uint8)


RAMP = _ramp()
_TERRAIN_ANCHORS = np.array([[40, 90, 40], [120, 160, 70], [200, 190, 120], [140, 100, 70],
                             [245, 245, 245]], dtype=np.float64)


def _terrain_ramp(n: int = 256) -> np.ndarray:
    xs = np.linspace(0, 1, len(_TERRAIN_ANCHORS))
    t = np.linspace(0, 1, n)
    return np.stack([np.interp(t, xs, _TERRAIN_ANCHORS[:, c]) for c in range(3)],
                    axis=1).astype(np.uint8)


TERRAIN_RAMP = _terrain_ramp()


def _font(size: int = 13) -> ImageFont.ImageFont:
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # older Pillow
        return ImageFont.load_default()


def downsample_factor(nx: int, max_width: int = MAX_WIDTH) -> int:
    return max(1, math.ceil(nx / max_width))


def _ds(a: np.ndarray, f: int) -> np.ndarray:
    return a if f == 1 else a[::f, ::f]


# ---- underlay ----------------------------------------------------------------------- #
def hillshade_layer(wp: WorldPack) -> np.ndarray:
    if wp.has("hillshade") and int(wp.layer("hillshade").max()) > 0:
        return np.asarray(wp.layer("hillshade"))
    elev = np.asarray(wp.layer("elevation_cm"))
    nodata = elev == INT32_MIN
    return hillshade_from_elevation(elev.astype(np.float64) / 100.0, wp.cell_size_m,
                                    nodata_mask=nodata)


def base_image(wp: WorldPack, factor: int) -> np.ndarray:
    """Hillshade grey + fuel-class tint -> (H, W, 3) uint8."""
    hs = _ds(hillshade_layer(wp), factor).astype(np.float64)
    fb = _ds(np.asarray(wp.layer("fbfm40")), factor)
    cls = fuel_class_array(fb)
    tint = FUEL_CLASS_RGB[cls].astype(np.float64)
    grey = np.clip(hs, 0, 255)[..., None] * np.ones(3)
    img = grey * (1 - FUEL_TINT_ALPHA) + tint * FUEL_TINT_ALPHA
    img[fb == 0] = grey[fb == 0] * 0.6  # nodata fuel: dimmer, untinted
    return np.clip(np.rint(img), 0, 255).astype(np.uint8)


# ---- frames ------------------------------------------------------------------------- #
class RunRenderer:
    """Stateful per-run frame composer (keeps persistent suppression overlays)."""

    def __init__(self, wp: WorldPack, header: StreamHeader, *, max_width: int = MAX_WIDTH):
        if (header.nx, header.ny) != (wp.nx, wp.ny):
            raise ValueError(f"stream grid {header.nx}x{header.ny} != pack {wp.nx}x{wp.ny}")
        self.wp, self.header = wp, header
        self.f = downsample_factor(wp.nx, max_width)
        self.base = base_image(wp, self.f)
        self.persist = np.zeros(wp.ny * wp.nx, np.uint8)  # last persistent overlay kind
        self.font = _font(13)
        self.font_small = _font(11)

    def compose(self, frame: Frame) -> Image.Image:
        f = self.f
        img = self.base.copy()
        phase = _ds(frame.phase, f)
        inten = _ds(frame.intensity, f)
        img[phase == 3] = PHASE_BURNED
        burning = phase == 2
        img[burning] = INTENSITY_RGB[np.clip(inten[burning], 0, 3)]

        ov = frame.overlay
        if ov.size:
            keep = np.isin(ov["kind"], PERSISTENT_OVERLAY)
            self.persist[ov["idx"][keep]] = ov["kind"][keep]
        pers = _ds(self.persist.reshape(self.wp.ny, self.wp.nx), f)
        for k in PERSISTENT_OVERLAY:
            m = pers == k
            if m.any():
                img[m] = OVERLAY_RGB[k]
        if ov.size:
            trans = ~np.isin(ov["kind"], PERSISTENT_OVERLAY)
            for k, idx in zip(ov["kind"][trans], ov["idx"][trans], strict=True):
                y, x = divmod(int(idx), self.wp.nx)
                img[y // f, x // f] = OVERLAY_RGB.get(int(k), (255, 255, 255))

        h, w = img.shape[:2]
        pil = Image.new("RGB", (w, h + HUD_H), (18, 18, 22))
        pil.paste(Image.fromarray(img, "RGB"), (0, 0))
        draw = ImageDraw.Draw(pil)
        for s in frame.spots:
            sy, sx = divmod(int(s["src"]), self.wp.nx)
            dy, dx = divmod(int(s["dst"]), self.wp.nx)
            col = (255, 60, 60) if s["ignited"] else ((255, 230, 90) if s["landed"]
                                                       else (150, 150, 150))
            draw.line([(sx // f, sy // f), (dx // f, dy // f)], fill=col, width=1)
        self._hud(draw, frame, w, h)
        return pil

    def _hud(self, draw: ImageDraw.ImageDraw, frame: Frame, w: int, h: int) -> None:
        t0 = self.header.t0_unix
        clock = datetime.fromtimestamp(t0 + frame.t_s, UTC).strftime("%Y-%m-%d %H:%MZ")
        day = frame.t_s // 86400 + 1
        hh, mm = (frame.t_s % 86400) // 3600, (frame.t_s % 3600) // 60
        m = frame.metrics
        cell_ha = (self.header.cell_mm / 1000.0) ** 2 / 10000.0
        burned_ha = (m.get("burned", 0) + m.get("burning", 0)) * cell_ha
        line1 = (f"{clock}  day {day} {hh:02d}:{mm:02d}  tick {frame.tick}   "
                 f"{self.header.model_id} {self.header.model_version}")
        cont = m.get("containment_permyriad", 0) / 100.0
        line2 = (f"burned {burned_ha:,.0f} ha  burning {m.get('burning', 0):,} cells  "
                 f"contained {cont:.1f}%  cost ${m.get('cost_cents', 0) / 100:,.0f}  "
                 f"busy {m.get('busy_resources', 0)}")
        if m.get("structures_lost", -1) >= 0:
            line2 += f"  structures lost {m['structures_lost']}"
        draw.text((6, h + 4), line1, fill=(235, 235, 235), font=self.font)
        draw.text((6, h + 26), line2, fill=(200, 200, 200), font=self.font_small)
        # wind vane (u east, v north -> screen dy = -v)
        cx, cy, r = w - 30, h + HUD_H // 2, 20
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(90, 90, 100))
        u, v = m.get("wind_u_cms", 0), m.get("wind_v_cms", 0)
        spd = math.hypot(u, v)
        if spd > 0:
            L = min(r - 2, 4 + spd / 40.0)
            ex, ey = cx + u / spd * L, cy - v / spd * L
            draw.line([(cx, cy), (ex, ey)], fill=(120, 200, 255), width=2)
            draw.ellipse([ex - 2, ey - 2, ex + 2, ey + 2], fill=(120, 200, 255))
        draw.text((cx - r - 78, cy - 7), f"{spd / 100:.1f} m/s", fill=(120, 200, 255),
                  font=self.font_small)
        m10 = m.get("m10", 0)
        draw.text((cx - r - 78, cy + 6), f"fm {m10 / 10:.1f}%", fill=(160, 160, 160),
                  font=self.font_small)


def save_png(img: Image.Image, path: str | Path) -> Path:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    img.save(path, format="PNG", optimize=False, compress_level=6)
    return path


def render_run(stream_path: str | Path, worldpack: WorldPack | str | Path,
               out_dir: str | Path, *, every: int = 1, max_width: int = MAX_WIDTH,
               ) -> list[Path]:
    """State stream -> numbered PNG frames in out_dir. Returns frame paths."""
    wp = worldpack if isinstance(worldpack, WorldPack) else load_worldpack(worldpack)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    frames: list[Path] = []
    rr: RunRenderer | None = None
    for header, frame in iter_frames(stream_path, every=every):
        if rr is None:
            rr = RunRenderer(wp, header, max_width=max_width)
        frames.append(save_png(rr.compose(frame), out_dir / f"{len(frames):05d}.png"))
    return frames


def encode_mp4(frames_dir: str | Path, out_mp4: str | Path, fps: int = 12,
               pattern: str = "%05d.png") -> Path:
    """PNG sequence -> H.264 MP4 (yuv420p, even dims) via ffmpeg on PATH."""
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise RuntimeError("ffmpeg not found on PATH")
    out_mp4 = Path(out_mp4)
    out_mp4.parent.mkdir(parents=True, exist_ok=True)
    cmd = [ffmpeg, "-y", "-loglevel", "error", "-framerate", str(fps),
           "-i", str(Path(frames_dir) / pattern), "-c:v", "libx264", "-pix_fmt", "yuv420p",
           "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2", "-movflags", "+faststart", str(out_mp4)]
    subprocess.run(cmd, check=True)
    return out_mp4


def encode_gif(frames_dir: str | Path, out_gif: str | Path, fps: int = 8,
               pattern: str = "%05d.png") -> Path:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise RuntimeError("ffmpeg not found on PATH")
    out_gif = Path(out_gif)
    cmd = [ffmpeg, "-y", "-loglevel", "error", "-framerate", str(fps),
           "-i", str(Path(frames_dir) / pattern),
           "-vf", "split[s0][s1];[s0]palettegen[p];[s1][p]paletteuse", str(out_gif)]
    subprocess.run(cmd, check=True)
    return out_gif


# ---- static layer renders (CP1) ------------------------------------------------------ #
STATIC_LAYERS = ("elevation_cm", "hillshade", "fbfm40", "cc_pct", "ch_dm", "cbh_dm", "cbd_gm3",
                 "evt", "greenness", "structures", "arrival_s", "confidence")


def _legend(img: np.ndarray, title: str, sub: str, ramp: np.ndarray | None,
            swatches: list[tuple[str, tuple[int, int, int]]] | None = None) -> Image.Image:
    h, w = img.shape[:2]
    pil = Image.new("RGB", (w, h + LEGEND_H), (18, 18, 22))
    pil.paste(Image.fromarray(img, "RGB"), (0, 0))
    d = ImageDraw.Draw(pil)
    d.text((6, h + 6), title, fill=(235, 235, 235), font=_font(14))
    d.text((6, h + 28), sub, fill=(190, 190, 190), font=_font(11))
    if ramp is not None:
        bar = np.repeat(ramp[None, :, :], 12, axis=0)
        bar_img = Image.fromarray(bar, "RGB").resize((min(256, w - 12), 12))
        pil.paste(bar_img, (w - bar_img.width - 6, h + 44))
    if swatches:
        x = w - 6
        for label, rgb in reversed(swatches):
            tw = int(d.textlength(label, font=_font(11)))
            x -= tw + 22
            d.rectangle([x, h + 44, x + 12, h + 56], fill=rgb)
            d.text((x + 16, h + 43), label, fill=(210, 210, 210), font=_font(11))
    return pil


def _colorize(values: np.ndarray, valid: np.ndarray, ramp: np.ndarray,
              under: np.ndarray | None = None, vmin: float | None = None,
              vmax: float | None = None) -> tuple[np.ndarray, float, float]:
    v = values.astype(np.float64)
    if valid.any():
        lo = float(v[valid].min()) if vmin is None else vmin
        hi = float(v[valid].max()) if vmax is None else vmax
    else:
        lo, hi = 0.0, 1.0
    span = hi - lo if hi > lo else 1.0
    idx = np.clip(np.rint((v - lo) / span * 255), 0, 255).astype(np.int64)
    img = ramp[idx].copy()
    if under is None:
        img[~valid] = (0, 0, 0)
    else:
        img[~valid] = under[~valid]
    return img, lo, hi


def render_static(worldpack: WorldPack | str | Path, layer: str, out_png: str | Path,
                  *, max_width: int = MAX_WIDTH) -> Path:
    """One world-pack layer -> PNG with legend + min/max (CP1 'the world loads')."""
    wp = worldpack if isinstance(worldpack, WorldPack) else load_worldpack(worldpack)
    if not wp.has(layer):
        raise KeyError(f"pack has no layer {layer!r}")
    f = downsample_factor(wp.nx, max_width)
    a = _ds(np.asarray(wp.layer(layer)), f)
    meta = wp.manifest["layers"][layer]
    st = meta["stats"]
    unit = meta.get("unit", "")
    sub = (f"{a.shape[1]}x{a.shape[0]} shown (x1/{f}) | dtype {meta['dtype']} {unit} | "
           f"min {st['min']} max {st['max']} valid {st['valid']:,} nodata {st['nodata']:,}")
    hs = _ds(hillshade_layer(wp), f).astype(np.uint8)
    grey = np.repeat(hs[..., None], 3, axis=2)
    swatches = None
    ramp: np.ndarray | None = RAMP

    if layer == "elevation_cm":
        valid = a != INT32_MIN
        img, lo, hi = _colorize(a / 100.0, valid, TERRAIN_RAMP)
        shade = (hs.astype(np.float64) / 255.0 * 0.6 + 0.4)[..., None]
        img = np.clip(img * shade, 0, 255).astype(np.uint8)
        img[~valid] = (0, 0, 0)
        sub += f" | ramp {lo:.0f}..{hi:.0f} m"
        ramp = TERRAIN_RAMP
    elif layer == "hillshade":
        img = grey
        ramp = None
    elif layer == "fbfm40":
        cls = fuel_class_array(a)
        img = FUEL_CLASS_RGB[cls].copy()
        img[a == 0] = (0, 0, 0)
        codes, counts = np.unique(a, return_counts=True)
        top = sorted(zip(counts, codes, strict=True), reverse=True)[:6]
        sub += " | top codes " + ", ".join(f"{int(c)}:{int(n):,}" for n, c in top)
        swatches = [(CLASS_NAMES[i], tuple(int(x) for x in FUEL_CLASS_RGB[i])) for i in range(7)]
        ramp = None
    elif layer == "evt":
        # categorical: hashed hue per code, deterministic
        codes = a.astype(np.int64)
        hue = (codes * 2654435761) % 360
        img = _hsv_to_rgb(hue, np.where(codes > 0, 0.55, 0.0), np.where(codes > 0, 0.85, 0.0))
        sub += f" | {len(np.unique(codes))} distinct codes"
        ramp = None
    elif layer == "confidence":
        pal = np.array([[0, 0, 0], [60, 200, 60], [220, 180, 40], [220, 60, 200]], np.uint8)
        img = pal[np.clip(a, 0, 3)].copy()
        img[a == 0] = grey[a == 0] // 2
        swatches = [("1 observed", (60, 200, 60)), ("2 interpolated", (220, 180, 40)),
                    ("3 hotspot", (220, 60, 200))]
        ramp = None
    elif layer == "arrival_s":
        valid = a >= 0
        img, lo, hi = _colorize(a / 3600.0, valid, RAMP, under=grey // 2)
        sub += f" | ramp {lo:.1f}..{hi:.1f} h since t0"
    elif layer == "structures":
        img = grey // 2
        img[a > 0] = (255, 80, 80)
        ramp = None
    else:  # cc_pct, ch_dm, cbh_dm, cbd_gm3, greenness
        valid = np.ones(a.shape, bool)
        img, lo, hi = _colorize(a, valid, RAMP)
        sub += f" | ramp {lo:g}..{hi:g} {unit}"

    pil = _legend(img, f"{wp.manifest['name']} - {layer}", sub, ramp, swatches)
    return save_png(pil, out_png)


def _hsv_to_rgb(h_deg: np.ndarray, s: np.ndarray, v: np.ndarray) -> np.ndarray:
    h = (np.asarray(h_deg, np.float64) % 360) / 60.0
    i = np.floor(h).astype(np.int64) % 6
    fr = h - np.floor(h)
    p, q, t = v * (1 - s), v * (1 - s * fr), v * (1 - s * (1 - fr))
    r = np.select([i == 0, i == 1, i == 2, i == 3, i == 4, i == 5], [v, q, p, p, t, v])
    g = np.select([i == 0, i == 1, i == 2, i == 3, i == 4, i == 5], [t, v, v, q, p, p])
    b = np.select([i == 0, i == 1, i == 2, i == 3, i == 4, i == 5], [p, p, t, v, v, q])
    return np.clip(np.rint(np.stack([r, g, b], axis=-1) * 255), 0, 255).astype(np.uint8)


def render_all_static(worldpack: WorldPack | str | Path, out_dir: str | Path,
                      *, max_width: int = MAX_WIDTH) -> list[Path]:
    wp = worldpack if isinstance(worldpack, WorldPack) else load_worldpack(worldpack)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    outs = [render_static(wp, layer, out_dir / f"{layer}.png", max_width=max_width)
            for layer in STATIC_LAYERS if wp.has(layer)]
    (out_dir / "stats.md").write_text(stats_markdown(wp), encoding="utf-8")
    return outs


def stats_markdown(wp: WorldPack) -> str:
    m = wp.manifest
    g = m["grid"]
    lines = [
        f"# World pack stats — {m['name']}", "",
        f"Grid: {g['nx']}x{g['ny']} @ {g['cell_size_m']} m, {g['crs']}, origin "
        f"({g['origin_x']}, {g['origin_y']}); t0 {m['t0_utc']}; pack_hash `{m['pack_hash']}`",
        "", "| layer | dtype | unit | min | max | valid | nodata |",
        "|---|---|---|---|---|---|---|",
    ]
    for name, meta in m["layers"].items():
        s = meta["stats"]
        lines.append(f"| {name} | {meta['dtype']} | {meta.get('unit', '')} | {s['min']} | "
                     f"{s['max']} | {s['valid']:,} | {s['nodata']:,} |")
    if m.get("arrival"):
        a = m["arrival"]
        sg = a["resampled_from_grid"]
        lines += ["", f"Arrival: `{a.get('algorithm')}` resampled ({a.get('method')}) from "
                  f"{sg['nx']}x{sg['ny']} @ origin ({sg['origin_x']}, {sg['origin_y']}); "
                  f"source aligned: {a.get('source_pixel_aligned')}; burned cells source "
                  f"{a.get('source_burned_cells')} -> pack {a.get('pack_burned_cells')}"]
    if m.get("weather"):
        lines += ["", f"Weather pack: `{m['weather']}`"]
    else:
        lines += ["", "Weather: none attached"]
    return "\n".join(lines) + "\n"


def frame_metadata(wp: WorldPack) -> dict[str, Any]:
    return {"nx": wp.nx, "ny": wp.ny, "factor": downsample_factor(wp.nx)}
