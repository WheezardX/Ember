"""D1 + D2 — arrival-time raster (the keystone).

Per-cell first-burn time (hours since t0) derived from the ordered perimeter series.
This is an INTERPRETATION algorithm, versioned, over immutable observations:

  - Perimeters are cumulative burned extents. We enforce monotonic burning
    (cum_i = cum_{i-1} ∪ perimeter_i) so cells never un-burn.
  - Cells first burned in interval (t_{i-1}, t_i] get arrival interpolated by their
    distance from the previous front: near the old edge -> early, far -> late.
  - Confidence: 1 = observed (first snapshot), 2 = interpolated (annulus interior).
    (3 = hotspot-inferred is added in Phase 2 with FIRMS.)

Deterministic given identical perimeters. Isochrones = contours of arrival.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

from terrain.dem.rasterize import NODATA, to_cog
from terrain.util.logging import get_logger

log = get_logger(__name__)
ALGORITHM = "perimeter-interp-v1"


@dataclass
class GridSpec:
    crs: str
    transform: object  # affine.Affine
    width: int
    height: int


def _utm_epsg(lon: float, lat: float) -> int:
    zone = int((lon + 180) / 6) + 1
    return (32600 if lat >= 0 else 32700) + zone


def build_incident_grid(perimeters, buffer_km: float, resolution_m: float) -> GridSpec:
    """AOI = final footprint + buffer, in the local UTM zone, at resolution_m."""
    from pyproj import Transformer
    from rasterio.transform import from_origin
    from shapely.ops import transform as shp_transform

    final = perimeters[-1].geom
    minx, miny, maxx, maxy = final.bounds  # lon/lat
    clon, clat = (minx + maxx) / 2, (miny + maxy) / 2
    epsg = _utm_epsg(clon, clat)
    to_utm = Transformer.from_crs("EPSG:4326", f"EPSG:{epsg}", always_xy=True)
    ub = shp_transform(lambda x, y, z=None: to_utm.transform(x, y), final).bounds
    pad = buffer_km * 1000.0
    x0, y0, x1, y1 = ub[0] - pad, ub[1] - pad, ub[2] + pad, ub[3] + pad
    width = int((x1 - x0) / resolution_m) + 1
    height = int((y1 - y0) / resolution_m) + 1
    return GridSpec(f"EPSG:{epsg}", from_origin(x0, y1, resolution_m, resolution_m), width, height)


def _rasterize(geom_lonlat, grid: GridSpec):
    from pyproj import Transformer
    from rasterio.features import rasterize
    from shapely.ops import transform as shp_transform

    to_utm = Transformer.from_crs("EPSG:4326", grid.crs, always_xy=True)
    g = shp_transform(lambda x, y, z=None: to_utm.transform(x, y), geom_lonlat)
    return rasterize([(g, 1)], out_shape=(grid.height, grid.width),
                     transform=grid.transform, fill=0, dtype="uint8").astype(bool)


def build_arrival_raster(
    perimeters, grid: GridSpec, out_dir: str | Path, *, resolution_m: float
) -> dict:
    """Write arrival_time + confidence COGs. Returns stats incl. t0."""
    import numpy as np
    import rasterio
    from scipy.ndimage import distance_transform_edt

    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    t0: datetime = perimeters[0].observed_at
    hours = [((p.observed_at - t0).total_seconds() / 3600.0) for p in perimeters]

    shape = (grid.height, grid.width)
    arrival = np.full(shape, NODATA, dtype="float32")
    conf = np.zeros(shape, dtype="uint8")
    prev_cum = np.zeros(shape, dtype=bool)

    for i, p in enumerate(perimeters):
        cum = prev_cum | _rasterize(p.geom, grid)
        newly = cum & ~prev_cum
        if newly.any():
            if i == 0:
                arrival[newly] = hours[0]
                conf[newly] = 1
            else:
                dist = distance_transform_edt(~prev_cum)  # distance from already-burned
                d = dist[newly]
                frac = d / (d.max() or 1.0)
                arrival[newly] = hours[i - 1] + frac * (hours[i] - hours[i - 1])
                conf[newly] = 2
        prev_cum = cum

    burned = arrival != NODATA
    profile = dict(driver="GTiff", height=grid.height, width=grid.width, count=1,
                   crs=grid.crs, transform=grid.transform)
    a_tmp = out_dir / f"arrival_time.{ALGORITHM}.raw.tif"
    with rasterio.open(a_tmp, "w", dtype="float32", nodata=NODATA, **profile) as ds:
        ds.write(arrival, 1)
    to_cog(a_tmp, out_dir / f"arrival_time.{ALGORITHM}.cog.tif")
    a_tmp.unlink(missing_ok=True)

    c_tmp = out_dir / "confidence.raw.tif"
    with rasterio.open(c_tmp, "w", dtype="uint8", nodata=0, **profile) as ds:
        ds.write(conf, 1)
    to_cog(c_tmp, out_dir / f"confidence.{ALGORITHM}.cog.tif")
    c_tmp.unlink(missing_ok=True)

    burned_hours = arrival[burned]
    stats = {
        "algorithm": ALGORITHM,
        "t0": t0.isoformat(),
        "snapshots": len(perimeters),
        "duration_h": round(max(hours), 1),
        "burned_cells": int(burned.sum()),
        "burned_km2": round(int(burned.sum()) * resolution_m * resolution_m / 1e6, 1),
        "observed_frac": round(float((conf == 1).sum()) / max(1, int(burned.sum())), 3),
        "arrival_h_range": [
            round(float(burned_hours.min()), 1), round(float(burned_hours.max()), 1)
        ],
    }
    log.info("arrival raster: %s", stats)
    return stats
