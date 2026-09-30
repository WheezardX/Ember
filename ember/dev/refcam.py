"""Reference cameras (EPIC_5_PLAN 8i R1): real photos as targets.

* `bookmark_from_photo` reads a photo's EXIF - GPS position, compass direction (GPSImgDirection,
  when the phone wrote it), 35 mm-equivalent focal length, orientation, time - and returns an
  absolute-camera bookmark (camera_lonlat, eye height, bearing, horizontal FOV). Pitch is not in
  EXIF: it starts level and is refined by lining up the ridgeline in the pair.
* `ref_pairs` writes, for every capture with a `reference` photo, photo | render | overlay
  (render edges over the photo, for pose refinement) into <run>/refpairs/.

GPS altitude is deliberately not used (ellipsoidal, ~20 m off the DEM's datum here); the camera
stands at eye height above the rendered ground instead.
"""

from __future__ import annotations

import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageOps

from ember.dev.scenario import LoadedScenario

# EXIF tags
_GPS_IFD = 0x8825
_EXIF_IFD = 0x8769
_FOCAL_35 = 0xA405      # FocalLengthIn35mmFilm
_FOCAL = 0x920A         # FocalLength (mm, physical)
_DATETIME = 0x9003      # DateTimeOriginal
_ORIENT = 0x0112


def _dms(v, ref) -> float:
    d, m, s = (float(x) for x in v)
    deg = d + m / 60.0 + s / 3600.0
    return -deg if ref in ("S", "W") else deg


def hfov_from_35mm(f35: float, landscape: bool = True) -> float:
    """Horizontal FOV (deg) for a 35 mm-equivalent focal length (36 x 24 mm frame)."""
    width = 36.0 if landscape else 24.0
    return math.degrees(2.0 * math.atan(width / (2.0 * f35)))


def bookmark_from_photo(photo: Path, name: str | None = None) -> dict:
    img = Image.open(photo)
    exif = img.getexif()
    gps = exif.get_ifd(_GPS_IFD)
    sub = exif.get_ifd(_EXIF_IFD)
    if 2 not in gps or 4 not in gps:
        raise ValueError(f"{photo.name}: no GPS position in EXIF")
    lat = _dms(gps[2], gps.get(1, "N"))
    lon = _dms(gps[4], gps.get(3, "E"))
    w, h = ImageOps.exif_transpose(img).size
    landscape = w >= h
    f35 = sub.get(_FOCAL_35)
    notes = []
    if f35:
        fov = hfov_from_35mm(float(f35), landscape)
    else:
        fov = 65.0
        notes.append("no 35 mm-equivalent focal length in EXIF: fov_deg is a guess")
    bearing = gps.get(17)                       # GPSImgDirection
    if bearing is None:
        notes.append("no compass direction in EXIF: set yaw_deg by hand")
    bm = {
        "name": name or photo.stem,
        "camera_lonlat": [round(lon, 7), round(lat, 7)],
        "camera_agl_m": 1.7,
        "yaw_deg": round(float(bearing), 1) if bearing is not None else 0.0,
        "pitch_deg": 0.0,
        "fov_deg": round(fov, 1),
    }
    return {"bookmark": bm, "taken": sub.get(_DATETIME), "notes": notes}


def bookmark_toml(bm: dict) -> str:
    lines = ["[[bookmarks]]"]
    for k, v in bm.items():
        if isinstance(v, str):
            lines.append(f'{k} = "{v}"')
        elif isinstance(v, list):
            lines.append(f"{k} = [{', '.join(str(x) for x in v)}]")
        else:
            lines.append(f"{k} = {v}")
    return "\n".join(lines) + "\n"


def _label(img: Image.Image, text: str) -> None:
    d = ImageDraw.Draw(img, "RGBA")
    try:
        f = ImageFont.truetype("arialbd.ttf", max(14, img.height // 30))
    except OSError:
        f = ImageFont.load_default()
    d.rectangle([0, 0, img.width, img.height // 18], fill=(0, 0, 0, 150))
    d.text((10, 4), text, fill="white", font=f)


def pair_image(photo: Path, render: Path, height: int = 720) -> Image.Image:
    """photo | render | overlay (render edges in cyan over the photo), all at `height`.
    Photo and render share the horizontal FOV, so both scale to the same width and the render
    is centre-cropped (or padded) vertically to the photo's aspect - same scale, same framing.
    (Best: give the scenario the photo's aspect ratio, so nothing is cropped.)"""
    p = ImageOps.exif_transpose(Image.open(photo)).convert("RGB")
    r = Image.open(render).convert("RGB")
    w = round(height * p.width / p.height)
    p = p.resize((w, height), Image.LANCZOS)
    rh = round(w * r.height / r.width)
    r = r.resize((w, rh), Image.LANCZOS)
    canvas = Image.new("RGB", (w, height))
    canvas.paste(r, (0, (height - rh) // 2))      # negative offset = centre crop
    r = canvas
    # Structure only (skyline, ridgelines): blur away the tree-scale texture before the edges,
    # which is what a pose is lined up by.
    soft = r.convert("L").filter(ImageFilter.GaussianBlur(radius=height / 180.0))
    edges = np.asarray(soft.filter(ImageFilter.FIND_EDGES)).astype(float)
    mask = np.clip((edges - 6.0) / 10.0, 0, 1)[..., None]
    base = np.asarray(p.convert("L").convert("RGB")).astype(float) * 0.7
    over = base * (1 - mask) + np.array([0.0, 230.0, 255.0]) * mask
    o = Image.fromarray(over.astype(np.uint8))
    for im, t in ((p, "photo"), (r, "render"), (o, "render edges over photo")):
        _label(im, t)
    out = Image.new("RGB", (3 * w, height))
    for i, im in enumerate((p, r, o)):
        out.paste(im, (i * w, 0))
    return out


def ref_pairs(sc: LoadedScenario, run_dir: Path, repo: Path) -> list[dict]:
    out_dir = run_dir / "refpairs"
    res = []
    for c in sc.spec.captures:
        if not c.reference:
            continue
        photo = (repo / c.reference) if not Path(c.reference).is_absolute() else Path(c.reference)
        render = run_dir / "captures" / f"{c.name}.png"
        entry = {"capture": c.name, "reference": c.reference, "pair": None, "error": ""}
        if not photo.exists():
            entry["error"] = f"reference photo missing: {photo}"
        elif not render.exists():
            entry["error"] = f"capture missing: {render}"
        else:
            out_dir.mkdir(exist_ok=True)
            dst = out_dir / f"{c.name}_pair.jpg"
            pair_image(photo, render).save(dst, quality=88)
            entry["pair"] = str(dst)
        res.append(entry)
    return res
