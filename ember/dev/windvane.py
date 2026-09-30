"""Wind-vane overlay for replay timelapses (EPIC_5_PLAN 8h HCP4 bundle: "plume direction never
contradicts the HUD wind").

Burns the stream's own wind into an orbit MP4: for every frame, the sim time the harness showed
(t_from_s -> t_to_s across the frames, as AEmberHarness lays it out), the stream's tick wind at
that time (TICK metrics wind_u/v, cm/s, velocity toward +x east / +y north), drawn as a vane
turned to the camera's heading (screen up = the direction the camera looks), with the compass
reading, speed and sim time. The orbit must keep a fixed heading (degrees = 0, no flyover), so
screen directions map to compass directions by one rotation; on an oblique view the vane is the
wind's direction across the ground, foreshortening aside.
"""

from __future__ import annotations

import bisect
import math
import shutil
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from ember.dev.scenario import LoadedScenario
from ember.sim.stream import Tick, read_stream, replay_stream_path


def stream_wind(stream: Path) -> tuple[list[int], list[tuple[float, float]]]:
    """(tick times s, [(u, v) m/s]) for every tick record of a state stream."""
    _, records = read_stream(stream)
    ts, uv = [], []
    for r in records:
        if isinstance(r, Tick):
            ts.append(r.t_s)
            uv.append((r.metrics["wind_u_cms"] / 100.0, r.metrics["wind_v_cms"] / 100.0))
    return ts, uv


def wind_at(ts: list[int], uv: list[tuple[float, float]], t: float) -> tuple[float, float]:
    """The tick in force at time t (the last tick with t_s <= t), as the renderer uses it."""
    i = max(0, bisect.bisect_right(ts, t) - 1)
    return uv[i]


def _font(size: int) -> ImageFont.ImageFont:
    for name in ("arialbd.ttf", "arial.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def draw_vane(img: Image.Image, u: float, v: float, cam_yaw_deg: float, t_h: float) -> None:
    """Vane in the top-right corner: arrow = where the wind blows TO, relative to the view."""
    w, h = img.size
    s = h / 1080.0
    r = 70 * s
    cx, cy = w - 130 * s, 150 * s
    d = ImageDraw.Draw(img, "RGBA")
    d.ellipse([cx - r - 12 * s, cy - r - 12 * s, cx + r + 12 * s, cy + r + 12 * s],
              fill=(0, 0, 0, 120), outline=(255, 255, 255, 160), width=max(1, int(2 * s)))
    # compass ticks: N on the ring where north is on screen
    for k, lab in enumerate("NESW"):
        a = math.radians(k * 90 - cam_yaw_deg)
        x, y = cx + math.sin(a) * (r + 2 * s), cy - math.cos(a) * (r + 2 * s)
        d.text((x, y), lab, fill=(255, 255, 255, 200), font=_font(int(16 * s)), anchor="mm")
    speed = math.hypot(u, v)
    to_deg = math.degrees(math.atan2(u, v)) % 360.0           # compass bearing it blows toward
    from_deg = (to_deg + 180.0) % 360.0
    a = math.radians(to_deg - cam_yaw_deg)                   # screen angle, clockwise from up
    dx, dy = math.sin(a), -math.cos(a)
    L = r * 0.8
    tail = (cx - dx * L, cy - dy * L)
    tip = (cx + dx * L, cy + dy * L)
    d.line([tail, tip], fill=(120, 200, 255, 255), width=max(2, int(6 * s)))
    px, py = -dy, dx
    head = 22 * s
    d.polygon([tip, (tip[0] - dx * head + px * head * 0.6, tip[1] - dy * head + py * head * 0.6),
               (tip[0] - dx * head - px * head * 0.6, tip[1] - dy * head - py * head * 0.6)],
              fill=(120, 200, 255, 255))
    f = _font(int(22 * s))
    d.text((cx, cy + r + 34 * s), f"wind from {from_deg:03.0f}°  {speed:.1f} m/s",
           fill=(255, 255, 255, 235), font=f, anchor="mm")
    d.text((cx, cy + r + 62 * s), f"t = {t_h:.1f} h", fill=(255, 255, 255, 200), font=f,
           anchor="mm")


def overlay(sc: LoadedScenario, run_dir: Path, orbit: str, out: Path | None = None) -> Path:
    o = next((x for x in sc.spec.orbits if x.name == orbit), None)
    if o is None:
        raise ValueError(f"no orbit {orbit!r} in {sc.name}")
    if o.to_bookmark is not None or abs(o.degrees) > 1e-6:
        raise ValueError("the vane needs a fixed-heading orbit (degrees = 0, no to_bookmark)")
    if o.t_from_s is None or o.t_to_s is None:
        raise ValueError("the vane needs a timelapse orbit (t_from_s / t_to_s)")
    if sc.replay_path is None:
        raise ValueError("the scenario has no replay")
    stream = replay_stream_path(sc.replay_path)
    if stream is None:
        raise ValueError("the replay names no state stream")
    yaw = next(b.yaw_deg for b in sc.spec.bookmarks if b.name == o.bookmark)
    src = run_dir / "orbits" / f"{orbit}.mp4"
    if not src.exists():
        raise FileNotFoundError(src)
    ff = shutil.which("ffmpeg")
    probe = shutil.which("ffprobe")
    if not ff or not probe:
        raise RuntimeError("ffmpeg / ffprobe not on PATH")
    wh = subprocess.run([probe, "-v", "error", "-select_streams", "v:0", "-show_entries",
                         "stream=width,height", "-of", "csv=p=0", str(src)],
                        capture_output=True, text=True, check=True).stdout.strip().split(",")
    w, h = int(wh[0]), int(wh[1])
    ts, uv = stream_wind(stream)
    dst = out or src.with_name(f"{orbit}_vane.mp4")
    dec = subprocess.Popen([ff, "-v", "error", "-i", str(src), "-f", "rawvideo", "-pix_fmt", "rgb24",
                            "-"], stdout=subprocess.PIPE)
    enc = subprocess.Popen([ff, "-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s",
                            f"{w}x{h}", "-framerate", str(o.fps), "-i", "-", "-c:v", "libx264",
                            "-pix_fmt", "yuv420p", "-crf", "18", "-movflags", "+faststart",
                            str(dst)], stdin=subprocess.PIPE)
    n = 0
    size = w * h * 3
    while True:
        buf = dec.stdout.read(size)
        if len(buf) < size:
            break
        img = Image.fromarray(np.frombuffer(buf, np.uint8).reshape(h, w, 3).copy())
        u_ = n / max(1, o.frames - 1)
        t = o.t_from_s + (o.t_to_s - o.t_from_s) * u_
        wu, wv = wind_at(ts, uv, t)
        draw_vane(img, wu, wv, yaw, t / 3600.0)
        enc.stdin.write(img.tobytes())
        n += 1
    enc.stdin.close()
    enc.wait()
    dec.wait()
    if enc.returncode != 0:
        raise RuntimeError("ffmpeg encode failed")
    return dst
