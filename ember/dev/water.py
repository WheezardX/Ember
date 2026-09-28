"""Derived water layer for a Terrain region (HCP1: "lakes are holes" -> water plane).

LiDAR drops returns over water and Terrain's void fill stops at 5 px, so lakes are DEM nodata
(upstream U7). Until the DEM is hydro-flattened, the renderer draws a flat water surface per
water body from this layer:

  * bodies = 8-connected components of (FBFM40 == 98 water) | (DEM nodata not connected to the
    raster border, i.e. holes inside the AOI);
  * level  = local 10th percentile of the body's own LiDAR water returns (follows rivers,
    ignores a reservoir's exposed drawdown ring), else its shoreline's 10th percentile;
  * written per tile, per LOD, on each tile's apron grid (same georef/size as Terrain's tile
    rasters), float32, nodata -9999, into Ember's store (Ember never writes into Terrain's).

Output: store/render/<region>/water/{index.json, z{lod}/x{x}/y{y}/water_level.tif}
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

import numpy as np

NODATA = -9999.0
WATER_FBFM40 = 98
MIN_OWN_RETURNS = 50       # own water returns needed to trust them as the surface
BLOCK_PX = 5               # water-line percentile per 5-cell block over its 3x3-block
MIN_BLOCK_RETURNS = 5      #   neighbourhood (~150 m at 10 m)


def render_store(repo: Path, region_name: str) -> Path:
    return repo / "store" / "render" / region_name


def _manifest_hash(region_dir: Path) -> str:
    return hashlib.sha256((region_dir / "manifest.json").read_bytes()).hexdigest()


def derive_levels(dem: np.ndarray, dem_nodata: float,
                  fbfm40: np.ndarray) -> tuple[np.ndarray, list[dict]]:
    """Canonical-grid level raster (NaN = not water) + per-body records.

    One rule for lakes, reservoirs and rivers: level(cell) = the 10th percentile of the body's
    own water returns within ~150 m (block neighbourhoods); cells with too few returns nearby
    take the nearest
    computed level; then a light mean along the body.
      * reservoir drawdown: LANDFIRE water is the full-pool extent, LiDAR hits the exposed
        "bathtub ring" above the water line -> the low percentile is the water line;
      * rivers: the window is local, so the level follows the gradient (a single flat plane
        would float downstream and sink upstream);
      * bodies with no own returns: 10th percentile of the valid shoreline ring (flat).
    """
    from scipy import ndimage

    valid = np.isfinite(dem) & (dem != dem_nodata)
    hole = ~valid
    lab_h, _ = ndimage.label(hole)
    border = np.unique(np.concatenate([lab_h[0], lab_h[-1], lab_h[:, 0], lab_h[:, -1]]))
    interior_hole = hole & ~np.isin(lab_h, border[border > 0])
    water = (fbfm40 == WATER_FBFM40) | interior_hole
    lab, _ = ndimage.label(water, structure=np.ones((3, 3), dtype=bool))
    level = np.full(dem.shape, np.nan, dtype=np.float32)
    bodies = []
    pad = 3 * BLOCK_PX
    for i, sl in enumerate(ndimage.find_objects(lab), start=1):
        if sl is None:
            continue
        r0, r1 = max(sl[0].start - pad, 0), min(sl[0].stop + pad, dem.shape[0])
        c0, c1 = max(sl[1].start - pad, 0), min(sl[1].stop + pad, dem.shape[1])
        body = lab[r0:r1, c0:c1] == i
        z = dem[r0:r1, c0:c1].astype(np.float64)
        v = valid[r0:r1, c0:c1]
        own_mask = body & v & (fbfm40[r0:r1, c0:c1] == WATER_FBFM40)
        cells = int(body.sum())
        n_own = int(own_mask.sum())
        if n_own >= MIN_OWN_RETURNS:
            vals, method = _local_low_percentile(z, own_mask, body), "local_p10_returns"
        else:
            ring = ndimage.binary_dilation(body, iterations=2) & ~body & v
            if not ring.any():
                continue  # no reference height at all: skip rather than guess
            vals = np.full(cells, np.percentile(z[ring], 10))
            method = "shoreline_p10"
        level[r0:r1, c0:c1][body] = vals.astype(np.float32)
        bodies.append({"id": i, "cells": cells, "own_returns": n_own, "method": method,
                       "level_m": round(float(np.median(vals)), 3),
                       "level_range_m": [round(float(vals.min()), 2), round(float(vals.max()), 2)],
                       "holes": int((body & ~v).sum())})
    return level, bodies


def _local_low_percentile(z: np.ndarray, own: np.ndarray, body: np.ndarray) -> np.ndarray:
    """Per-cell water level for one body: 10th percentile of OWN returns only, per BLOCK_PX
    block over its 3x3-block neighbourhood (~150 m); blocks with too few returns take the
    nearest block's value; then a light mean over the body. Returns values for body cells."""
    from scipy import ndimage

    b = BLOCK_PX
    h, w = z.shape
    bh, bw = (h + b - 1) // b, (w + b - 1) // b
    lev = np.full((bh, bw), np.nan)
    for bi in range(bh):
        for bj in range(bw):
            r0, r1 = max((bi - 1) * b, 0), min((bi + 2) * b, h)
            c0, c1 = max((bj - 1) * b, 0), min((bj + 2) * b, w)
            m = own[r0:r1, c0:c1]
            if m.sum() >= MIN_BLOCK_RETURNS:
                lev[bi, bj] = np.percentile(z[r0:r1, c0:c1][m], 10)
    ok = np.isfinite(lev)
    _, (ri, ci) = ndimage.distance_transform_edt(~ok, return_indices=True)
    lev = lev[ri, ci]
    cells = np.repeat(np.repeat(lev, b, axis=0), b, axis=1)[:h, :w]
    num = ndimage.uniform_filter(np.where(body, cells, 0.0), size=2 * b + 1)
    den = ndimage.uniform_filter(body.astype(np.float64), size=2 * b + 1)
    smooth = np.where(den > 0, num / np.maximum(den, 1e-9), cells)
    return smooth[body]


def build(region_dir: Path, out_dir: Path) -> dict:
    import rasterio
    from rasterio.transform import from_origin
    from rasterio.warp import Resampling, reproject

    manifest = json.loads((region_dir / "manifest.json").read_text(encoding="utf-8"))
    with rasterio.open(region_dir / "dem" / "dem.cog.tif") as d:
        dem = d.read(1).astype(np.float32)
        dem_nodata = d.nodata if d.nodata is not None else NODATA
        transform, crs = d.transform, d.crs
    with rasterio.open(region_dir / "fuels" / "fbfm40.cog.tif") as f:
        fb = f.read(1)
    level, bodies = derive_levels(dem, dem_nodata, fb)
    src = np.where(np.isfinite(level), level, NODATA).astype(np.float32)

    tp, ov = manifest["tile_px"], manifest["overlap_px"]
    n = tp + 2 * ov
    tiles_out = []
    for t in manifest["tiles"]:
        minx, miny, maxx, maxy = t["apron_bounds"]
        res = (maxx - minx) / n
        dst = np.full((n, n), NODATA, dtype=np.float32)
        reproject(src, dst, src_transform=transform, src_crs=crs, src_nodata=NODATA,
                  dst_transform=from_origin(minx, maxy, res, res), dst_crs=crs,
                  dst_nodata=NODATA, resampling=Resampling.nearest)
        if not (dst != NODATA).any():
            continue
        rel = Path(f"z{t['lod']}") / f"x{t['x']}" / f"y{t['y']}" / "water_level.tif"
        path = out_dir / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        with rasterio.open(path, "w", driver="GTiff", height=n, width=n, count=1,
                           dtype="float32", crs=crs, transform=from_origin(minx, maxy, res, res),
                           nodata=NODATA) as w:  # uncompressed strips: worldcore's TIFF reader
            w.write(dst, 1)
        tiles_out.append({"lod": t["lod"], "x": t["x"], "y": t["y"], "path": rel.as_posix(),
                          "cells": int((dst[ov:ov + tp, ov:ov + tp] != NODATA).sum())})
    index = {"format": "ember-water", "version": 1, "region": region_dir.name,
             "manifest_sha256": _manifest_hash(region_dir),
             "rule": "bodies = water(FBFM40 98) | interior DEM holes; level = local p10 of own "
                     "water returns (~150 m blocks), else shoreline p10",
             "bodies": bodies, "tiles": tiles_out}
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "index.json").write_text(json.dumps(index, indent=1), encoding="utf-8")
    return index
