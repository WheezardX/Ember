"""2D | 3D split MP4 (HCP3): Epic 4's 2D playback animation beside the renderer's straight-down
timelapse of the same replay, map areas at the same scale and position.

The 3D side is a `[[orbits]]` entry over a straight-down bookmark (pitch ~-90, north up) with the
same frame count and rate as the 2D video. Its crop to the replay grid comes from the camera
geometry: the image centre is the bookmark target on the ground, and a UE camera's FOV is
horizontal, so the scale is width_px / (2 d tan(fov / 2)) px per metre at the target's height.
Terrain relief shifts that scale a few percent: this is a visual comparison, the probes are the
exact one.
"""

from __future__ import annotations

import json
import math
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from ember.dev.scenario import LoadedScenario

MAP_ROWS_2D = 627     # the 2D renderer draws the grid 1 px per cell, HUD band below it


@dataclass
class Crop:
    x: int
    y: int
    w: int
    h: int


def grid_crop(sc: LoadedScenario, run_dir: Path, bookmark: str) -> Crop:
    """The replay grid's rectangle (px) in a capture from `bookmark`."""
    s = sc.spec
    b = next(b for b in s.bookmarks if b.name == bookmark)
    if b.target_frac is None:
        raise ValueError(f"bookmark {bookmark!r} needs target_frac")
    if b.pitch_deg > -85:
        raise ValueError(f"bookmark {bookmark!r} is not straight down (pitch {b.pitch_deg})")
    facts = sorted((run_dir / "facts").glob("*.json"))
    if not facts:
        raise FileNotFoundError(f"{run_dir}: no facts (need world.data_extent_m)")
    minx, miny, maxx, maxy = json.loads(facts[0].read_text(encoding="utf-8"))["world"][
        "data_extent_m"]
    tx = minx + b.target_frac[0] * (maxx - minx)
    ty = maxy - b.target_frac[1] * (maxy - miny)
    replay = json.loads(Path(sc.replay_path).read_text(encoding="utf-8"))
    g = replay["world"]["grid"]
    w_px, h_px = s.scenario.resolution
    scale = w_px / (2.0 * b.distance_m * math.tan(math.radians(b.fov_deg) / 2.0))
    x0 = w_px / 2.0 + (g["origin_x"] - tx) * scale
    y0 = h_px / 2.0 - (g["origin_y"] - ty) * scale
    w = g["nx"] * g["cell_size_m"] * scale
    h = g["ny"] * g["cell_size_m"] * scale
    c = Crop(round(x0), round(y0), round(w), round(h))
    if c.x < 0 or c.y < 0 or c.x + c.w > w_px or c.y + c.h > h_px:
        raise ValueError(f"grid {c} does not fit the {w_px}x{h_px} frame: move the camera back")
    return c


def compose(left_2d: Path, right_3d: Path, crop: Crop, out: Path, height: int = 1080) -> Path:
    """hstack: 2D video scaled to `height`; 3D grid crop scaled so its map matches the 2D map
    rows, padded with the same HUD band below."""
    ff = shutil.which("ffmpeg")
    if not ff:
        raise RuntimeError("ffmpeg not on PATH")
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                            "stream=height", "-of", "csv=p=0", str(left_2d)],
                           capture_output=True, text=True, check=True)
    h2d = int(probe.stdout.strip())
    map_h = round(height * MAP_ROWS_2D / h2d) // 2 * 2
    font = "C\\:/Windows/Fonts/arial.ttf"

    def label(text: str) -> str:
        return (f"drawtext=fontfile='{font}':text='{text}':x=12:y=12:fontsize=26:"
                f"fontcolor=white:box=1:boxcolor=black@0.55:boxborderw=8")

    graph = (f"[0:v]scale=-2:{height},{label('Epic 4 CP2 - 2D playback')}[a];"
             f"[1:v]crop={crop.w}:{crop.h}:{crop.x}:{crop.y},scale=-2:{map_h},"
             f"pad=iw:{height}:0:0:color=0x16161a,{label('Epic 5 - UE renderer, same stream')}[b];"
             f"[a][b]hstack=inputs=2:shortest=1[v]")
    out.parent.mkdir(parents=True, exist_ok=True)
    p = subprocess.run([ff, "-y", "-loglevel", "error", "-i", str(left_2d), "-i", str(right_3d),
                        "-filter_complex", graph, "-map", "[v]", "-c:v", "libx264",
                        "-pix_fmt", "yuv420p", "-crf", "18", "-movflags", "+faststart", str(out)],
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError((p.stderr or "ffmpeg failed").strip()[:600])
    return out
