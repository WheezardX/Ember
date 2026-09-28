"""Side-by-side of a top-down render against Terrain's own hillshade (HCP1 "terrain reads true").

The render's ground footprint comes from the capture's scene facts (camera position, FOV) and
the world anchor; the hillshade COG is cropped to the same footprint and resampled to the same
size, so the two panels register (to within top-down perspective parallax, <2% at 88 km).
"""

from __future__ import annotations

import json
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def compare(run_dir: Path, capture: str, hillshade_cog: Path, out: Path, width: int = 1280) -> dict:
    import rasterio
    from rasterio.enums import Resampling
    from rasterio.windows import from_bounds

    facts = json.loads((run_dir / "facts" / f"{capture}.json").read_text(encoding="utf-8"))
    cam = facts["camera"]
    ax, ay, az = facts["world"]["anchor_m"]
    cx = ax + cam["location_cm"][0] / 100.0
    cy = ay - cam["location_cm"][1] / 100.0            # UE Y = south
    cz = az + cam["location_cm"][2] / 100.0
    ext = facts["world"]["data_extent_m"]
    render = Image.open(run_dir / "captures" / f"{capture}.png").convert("RGB")
    aspect = render.height / render.width

    with rasterio.open(hillshade_cog) as src:
        # Footprint at the region's z_min (the anchor). Relief is ~1.5 km under an ~88 km
        # camera, so the footprint error from ignoring it is < 2%.
        h = cz - az
        half_w = h * math.tan(math.radians(cam["fov_deg"]) / 2.0)
        half_h = half_w * aspect
        win = from_bounds(cx - half_w, cy - half_h, cx + half_w, cy + half_h, src.transform)
        hs = src.read(1, window=win, boundless=True, fill_value=0,
                      out_shape=(int(width * aspect), width), resampling=Resampling.bilinear)
    left = render.resize((width, int(width * aspect)), Image.Resampling.BOX)
    right = Image.fromarray(np.clip(hs, 0, 255).astype(np.uint8)).convert("RGB")
    sheet = Image.new("RGB", (width * 2 + 12, left.height + 36), (20, 20, 22))
    sheet.paste(left, (0, 36))
    sheet.paste(right, (width + 12, 36))
    d = ImageDraw.Draw(sheet)
    d.text((8, 10), f"UE render ({capture}): clay, sun 315/45, top-down", fill=(230, 230, 230))
    d.text((width + 20, 10), "Terrain derived/hillshade.cog.tif (gdaldem 315/45)",
           fill=(230, 230, 230))
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)
    return {"footprint_m": [round(2 * half_w), round(2 * half_h)], "camera_height_m": round(h),
            "centre_m": [round(cx, 1), round(cy, 1)], "data_extent_m": ext, "out": str(out)}
