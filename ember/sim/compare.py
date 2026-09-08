"""Side-by-side comparison frames (plan G2): N runs on a shared clock in one frame grid.

Each panel is that run's `render.py` frame at the same `t_s` (runs must share `dt_s`; they
may use different world packs — the CP4 matrix compares synthetic worlds). A run that ends
early holds its last frame, greyed, tagged "ended". Disposable, like `render.py`.
"""

from __future__ import annotations

import math
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageOps

from ember.sim.render import (
    MAX_WIDTH,
    Crop,
    RunRenderer,
    _font,
    encode_mp4,
    save_png,
    write_render_meta,
)
from ember.sim.stream import Frame, StreamHeader, iter_frames
from ember.sim.worldpack import WorldPack, load_worldpack

LABEL_H = 22
CLOCK_H = 26
GAP = 4
BG = (18, 18, 22)


@dataclass
class _Run:
    label: str
    frames: Iterator[tuple[StreamHeader, Frame]]
    wp: WorldPack
    renderer: RunRenderer | None = None
    header: StreamHeader | None = None
    last: Image.Image | None = None
    last_t: int = 0
    ended: bool = False

    def step(self) -> None:
        if self.ended:
            return
        try:
            header, frame = next(self.frames)
        except StopIteration:
            self.ended = True
            return
        if self.renderer is None:
            self.header = header
            self.renderer = RunRenderer(self.wp, header, max_width=self.panel_width,
                                        crop=self.crop, scale=self.scale)
        self.last = self.renderer.compose(frame)
        self.last_t = frame.t_s

    panel_width: int = MAX_WIDTH
    crop: Crop | None = None
    scale: int = 1


def _grey_ended(img: Image.Image) -> Image.Image:
    g = ImageOps.grayscale(img).point(lambda v: int(v * 0.55)).convert("RGB")
    d = ImageDraw.Draw(g)
    d.rectangle([4, 4, 64, 22], fill=(90, 30, 30))
    d.text((8, 6), "ended", fill=(255, 220, 220), font=_font(12))
    return g


def _clock_text(t_s: int) -> str:
    day = t_s // 86400 + 1
    hh, mm = (t_s % 86400) // 3600, (t_s % 3600) // 60
    return f"t = {t_s} s   day {day} {hh:02d}:{mm:02d}"


def render_side_by_side(streams: list[tuple[str, str | Path, str | Path | WorldPack]],
                        out_dir: str | Path, *, every: int = 1, cols: int | None = None,
                        max_width: int = MAX_WIDTH, crop: Crop | None = None,
                        scale: int = 1) -> list[Path]:
    """`streams` = [(label, stream_path, worldpack_path_or_pack), ...] -> numbered PNGs."""
    if not streams:
        raise ValueError("no streams to compare")
    n = len(streams)
    cols = cols or (n if n <= 3 else math.ceil(math.sqrt(n)))
    rows = math.ceil(n / cols)
    panel_w = max(64, (max_width - GAP * (cols - 1)) // cols)
    runs: list[_Run] = []
    for label, path, wp in streams:
        pack = wp if isinstance(wp, WorldPack) else load_worldpack(wp)
        r = _Run(label, iter_frames(path, every=every), pack)
        r.panel_width, r.crop, r.scale = panel_w, crop, scale
        runs.append(r)

    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    font = _font(13)
    frames: list[Path] = []
    dt_seen: set[int] = set()
    while True:
        for r in runs:
            r.step()
        active = [r for r in runs if not r.ended]
        if not active or any(r.last is None for r in runs):
            break
        for r in runs:
            if r.header is not None:
                dt_seen.add(r.header.dt_s)
        if len(dt_seen) > 1:
            raise ValueError(f"runs disagree on dt_s: {sorted(dt_seen)}")
        t_s = active[0].last_t
        pw = max(r.last.width for r in runs)
        ph = max(r.last.height for r in runs)
        W = cols * pw + GAP * (cols - 1)
        H = CLOCK_H + rows * (LABEL_H + ph) + GAP * (rows - 1)
        canvas = Image.new("RGB", (W, H), BG)
        d = ImageDraw.Draw(canvas)
        d.text((6, 5), _clock_text(t_s), fill=(235, 235, 235), font=font)
        for i, r in enumerate(runs):
            cx, cy = i % cols, i // cols
            x0 = cx * (pw + GAP)
            y0 = CLOCK_H + cy * (LABEL_H + ph + GAP)
            d.text((x0 + 4, y0 + 4), r.label, fill=(255, 230, 120), font=font)
            panel = _grey_ended(r.last) if r.ended else r.last
            canvas.paste(panel, (x0, y0 + LABEL_H))
        frames.append(save_png(canvas, out_dir / f"{len(frames):05d}.png"))
    first = next((r for r in runs if r.renderer is not None), None)
    if first is not None:
        write_render_meta(out_dir, first.renderer.view, every=every, cols=cols,
                          runs=[r.label for r in runs], frames=len(frames))
    return frames


def compare_to_mp4(streams: list[tuple[str, str | Path, str | Path | WorldPack]],
                   out_dir: str | Path, out_mp4: str | Path, *, every: int = 1,
                   cols: int | None = None, fps: int = 12) -> Path:
    render_side_by_side(streams, out_dir, every=every, cols=cols)
    return encode_mp4(out_dir, out_mp4, fps=fps)
