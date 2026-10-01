"""NAIP colour probe (EPIC_5_PLAN 8i R2): does the terrain render the colours the ground really has?

NAIP (USDA aerial imagery, public domain, ~0.6 m, leaf-on summers) is ground-truth colour per
landcover class. Two parts:

* `fetch`: the latest NAIP year covering a region, from Microsoft Planetary Computer's STAC
  (free COGs; anonymous SAS token), reading only the overview level nearest `res_m` (default
  10 m: tens of MB for a 20 km AOI instead of GBs), mosaicked onto the world's CRS grid ->
  store/render/<region>/naip/naip_rgb.tif (+ provenance.json). Metered into the Terrain store's
  download ledger (Brad's data cap).
* `probe`: straight-down captures (a grid of sites, low enough that vegetation streams in) are
  mapped pixel -> world from the camera facts, averaged into NAIP's cells, and compared per
  FBFM40 fuel group (grass, shrub, timber, ...) in CIELAB: median colour of render vs NAIP,
  delta-E raw and after one global brightness match (auto exposure makes absolute brightness
  moot). Advisory: a report and a render | NAIP sheet, no pass / fail until tolerances are tuned
  on real captures (8i open question 2).
"""

from __future__ import annotations

import json
import math
import urllib.request
from pathlib import Path

import numpy as np

STAC = "https://planetarycomputer.microsoft.com/api/stac/v1"
TOKEN = "https://planetarycomputer.microsoft.com/api/sas/v1/token/naip"

# FBFM40 codes -> probe groups (Scott & Burgan 2005; 91-99 non-burnable)
GROUPS = {
    "urban": [91], "snow_ice": [92], "agriculture": [93], "water": [98], "barren": [99],
    "grass": list(range(101, 110)), "grass_shrub": list(range(121, 125)),
    "shrub": list(range(141, 150)), "timber_understory": list(range(161, 166)),
    "timber_litter": list(range(181, 190)), "slash_blowdown": list(range(201, 205)),
}


def _region_extent(region_dir: Path) -> tuple[str, tuple[float, float, float, float]]:
    m = json.loads((region_dir / "manifest.json").read_text(encoding="utf-8"))
    fin = max(t["lod"] for t in m["tiles"])
    b = [t["content_bounds"] for t in m["tiles"] if t["lod"] == fin]
    return m["crs"], (min(v[0] for v in b), min(v[1] for v in b),
                      max(v[2] for v in b), max(v[3] for v in b))


def fetch(region_dir: Path, out_dir: Path, res_m: float = 10.0) -> dict:
    import rasterio
    from pyproj import Transformer
    from pystac_client import Client
    from rasterio.transform import from_origin
    from rasterio.warp import Resampling, reproject

    crs, (x0, y0, x1, y1) = _region_extent(region_dir)
    tr = Transformer.from_crs(crs, "EPSG:4326", always_xy=True)
    lons, lats = tr.transform([x0, x1, x0, x1], [y0, y0, y1, y1])
    bbox = [min(lons), min(lats), max(lons), max(lats)]

    from terrain.work.netmeter import NetMeter

    store_root = region_dir.parent
    with NetMeter(store_root, region_dir.name, "naip (ember-dev)"):
        items = list(Client.open(STAC).search(collections=["naip"], bbox=bbox, limit=200).items())
        if not items:
            raise RuntimeError(f"no NAIP over {bbox}")
        year = max(int(i.properties.get("naip:year", i.datetime.year)) for i in items)
        items = [i for i in items if int(i.properties.get("naip:year", i.datetime.year)) == year]
        token = json.load(urllib.request.urlopen(TOKEN, timeout=60))["token"]

        w = int(math.ceil((x1 - x0) / res_m))
        h = int(math.ceil((y1 - y0) / res_m))
        dst_tf = from_origin(x0, y1, res_m, res_m)
        mosaic = np.zeros((3, h, w), np.uint8)
        used = []
        for it in sorted(items, key=lambda i: i.id):
            href = it.assets["image"].href + "?" + token
            with rasterio.open(href) as base:
                facs = base.overviews(1)
                # the coarsest overview still at least as fine as res_m
                lvl = max((k for k, f in enumerate(facs) if base.res[0] * f <= res_m), default=None)
            with rasterio.open(href, **({} if lvl is None else {"overview_level": lvl})) as ds:
                tile = np.zeros((3, h, w), np.uint8)
                for b in range(3):
                    reproject(rasterio.band(ds, b + 1), tile[b], dst_transform=dst_tf, dst_crs=crs,
                              resampling=Resampling.average, dst_nodata=0)
                fill = (mosaic.sum(0) == 0) & (tile.sum(0) > 0)
                mosaic[:, fill] = tile[:, fill]
                used.append({"id": it.id, "overview_level": lvl, "res_m": ds.res[0]})
    out_dir.mkdir(parents=True, exist_ok=True)
    tif = out_dir / "naip_rgb.tif"
    with rasterio.open(tif, "w", driver="GTiff", width=w, height=h, count=3, dtype="uint8",
                       crs=crs, transform=dst_tf, nodata=0, compress="deflate") as o:
        o.write(mosaic)
    prov = {"source": "USDA NAIP via Microsoft Planetary Computer (public domain)",
            "year": year, "bbox_wgs84": bbox, "res_m": res_m, "items": used,
            "coverage": float((mosaic.sum(0) > 0).mean())}
    (out_dir / "provenance.json").write_text(json.dumps(prov, indent=2), encoding="utf-8")
    return prov


# ---------------------------------------------------------------- colour
def srgb_to_lab(rgb8: np.ndarray) -> np.ndarray:
    """(..., 3) sRGB uint8 / float 0..255 -> CIELAB (D65)."""
    c = np.asarray(rgb8, float) / 255.0
    lin = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    m = np.array([[0.4124564, 0.3575761, 0.1804375],
                  [0.2126729, 0.7151522, 0.0721750],
                  [0.0193339, 0.1191920, 0.9503041]])
    xyz = lin @ m.T / np.array([0.95047, 1.0, 1.08883])
    f = np.where(xyz > (6 / 29) ** 3, np.cbrt(xyz), xyz / (3 * (6 / 29) ** 2) + 4 / 29)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]),
                     200 * (f[..., 1] - f[..., 2])], -1)


def _to_linear(rgb8: np.ndarray) -> np.ndarray:
    c = np.asarray(rgb8, float) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def _to_srgb8(lin: np.ndarray) -> np.ndarray:
    c = np.where(lin <= 0.0031308, lin * 12.92, 1.055 * np.clip(lin, 0, None) ** (1 / 2.4) - 0.055)
    return np.clip(c * 255.0, 0, 255)


def ground_xy(facts: dict, w: int, h: int) -> tuple[np.ndarray, np.ndarray]:
    """World x / y of every pixel centre of a straight-down capture (pinhole, flat ground at
    the camera's height above ground; image up = the camera's bearing)."""
    cam = facts["camera"]
    s = 2.0 * cam["agl_m"] * math.tan(math.radians(cam["fov_deg"]) / 2.0) / w   # m per pixel
    b = math.radians(cam["bearing_deg"])
    up = np.array([math.sin(b), math.cos(b)])
    right = np.array([math.cos(b), -math.sin(b)])
    u = (np.arange(w) + 0.5 - w / 2.0) * s
    v = -(np.arange(h) + 0.5 - h / 2.0) * s
    uu, vv = np.meshgrid(u, v)
    return (cam["world_x_m"] + uu * right[0] + vv * up[0],
            cam["world_y_m"] + uu * right[1] + vv * up[1])


def probe(run_dir: Path, naip_tif: Path, fbfm40_tif: Path, captures: list[str]) -> dict:
    import rasterio
    from PIL import Image

    with rasterio.open(naip_tif) as n:
        naip = n.read()                             # 3, H, W
        ntf = n.transform
    with rasterio.open(fbfm40_tif) as f:
        fb = f.read(1)
        ftf = f.transform
    acc: dict[str, dict[str, list]] = {}
    sheets = []
    for name in captures:
        img = np.asarray(Image.open(run_dir / "captures" / f"{name}.png").convert("RGB"))
        facts = json.loads((run_dir / "facts" / f"{name}.json").read_text(encoding="utf-8"))
        h, w, _ = img.shape
        gx, gy = ground_xy(facts, w, h)
        col = np.floor((gx - ntf.c) / ntf.a).astype(int)
        row = np.floor((gy - ntf.f) / ntf.e).astype(int)
        ok = (col >= 0) & (row >= 0) & (col < naip.shape[2]) & (row < naip.shape[1])
        # render averaged into NAIP cells, in linear light
        key = row[ok] * naip.shape[2] + col[ok]
        lin = _to_linear(img[ok])
        uk, inv = np.unique(key, return_inverse=True)
        sums = np.zeros((len(uk), 3))
        np.add.at(sums, inv, lin)
        cnt = np.bincount(inv, minlength=len(uk))[:, None]
        rend = _to_srgb8(sums / cnt)
        rr, cc = uk // naip.shape[2], uk % naip.shape[2]
        nai = naip[:, rr, cc].T.astype(float)
        good = (cnt[:, 0] >= 20) & (nai.sum(1) > 0)
        # fuel class at the cell centre
        wx = ntf.c + (cc + 0.5) * ntf.a
        wy = ntf.f + (rr + 0.5) * ntf.e
        fc = np.floor((wx - ftf.c) / ftf.a).astype(int)
        fr = np.floor((wy - ftf.f) / ftf.e).astype(int)
        inside = (fc >= 0) & (fr >= 0) & (fc < fb.shape[1]) & (fr < fb.shape[0])
        codes = np.where(inside,
                         fb[np.clip(fr, 0, fb.shape[0] - 1), np.clip(fc, 0, fb.shape[1] - 1)], -1)
        for g in GROUPS:
            m = good & np.isin(codes, GROUPS[g])
            if m.any():
                a = acc.setdefault(g, {"render": [], "naip": []})
                a["render"].append(rend[m])
                a["naip"].append(nai[m])
        # sheet: render | NAIP over the same footprint
        nimg = np.zeros_like(img)
        nimg[ok] = naip[:, row[ok], col[ok]].T
        sheets.append((name, img, nimg))
    # a global brightness match (auto exposure): scale render L so the all-class medians agree
    all_r = np.concatenate([np.concatenate(a["render"]) for a in acc.values()])
    all_n = np.concatenate([np.concatenate(a["naip"]) for a in acc.values()])
    gain = np.median(srgb_to_lab(all_n)[:, 0]) / max(np.median(srgb_to_lab(all_r)[:, 0]), 1e-3)
    groups = {}
    for g, a in acc.items():
        r = srgb_to_lab(np.concatenate(a["render"]))
        n = srgb_to_lab(np.concatenate(a["naip"]))
        rm, nm = np.median(r, 0), np.median(n, 0)
        rn = rm * np.array([gain, 1.0, 1.0])
        groups[g] = {
            "cells": int(len(r)),
            "render_lab": [round(float(v), 1) for v in rm],
            "naip_lab": [round(float(v), 1) for v in nm],
            "delta_e": round(float(np.linalg.norm(rm - nm)), 1),
            "delta_e_brightness_matched": round(float(np.linalg.norm(rn - nm)), 1),
            "chroma_render": round(float(np.hypot(rm[1], rm[2])), 1),
            "chroma_naip": round(float(np.hypot(nm[1], nm[2])), 1),
            "hue_render_deg": round(float(np.degrees(np.arctan2(rm[2], rm[1]))), 0),
            "hue_naip_deg": round(float(np.degrees(np.arctan2(nm[2], nm[1]))), 0),
        }
    report = {"format": "ember-naip-probe", "version": 1, "run": str(run_dir),
              "naip": str(naip_tif), "brightness_gain": round(float(gain), 3),
              "groups": dict(sorted(groups.items(), key=lambda kv: -kv[1]["cells"]))}
    (run_dir / "naip_probe.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    _sheet(run_dir / "naip_probe.jpg", sheets)
    return report


def _sheet(path: Path, sheets: list) -> None:
    from PIL import Image, ImageDraw

    tw = 480
    rows = []
    for name, a, b in sheets:
        th = round(tw * a.shape[0] / a.shape[1])
        row = Image.new("RGB", (2 * tw, th))
        row.paste(Image.fromarray(a).resize((tw, th)), (0, 0))
        row.paste(Image.fromarray(b.astype(np.uint8)).resize((tw, th)), (tw, 0))
        d = ImageDraw.Draw(row)
        d.text((6, 4), f"{name}: render", fill="white")
        d.text((tw + 6, 4), "NAIP", fill="white")
        rows.append(row)
    cols = 2
    rh = rows[0].height
    out = Image.new("RGB", (cols * 2 * tw, rh * math.ceil(len(rows) / cols)))
    for i, r in enumerate(rows):
        out.paste(r, ((i % cols) * 2 * tw, (i // cols) * rh))
    out.save(path, quality=85)
