"""C2 — HRRR gridded weather adapter (herbie).

Builds the gridded half of a weather timeline (schema in [[schema.py]]) for an incident
window: for each time step it pulls HRRR analysis (F00) surface fields — 10 m wind u/v,
2 m temperature, 2 m RH — byte-range-subset to just those variables (via herbie), then
nearest-neighbor regrids the native 3 km Lambert grid onto a coarse regular grid in the
incident's UTM CRS. Fields are written to an ``.npz`` sidecar the manifest points at.

Analysis F00 is preferred (D4: historic truth); a step whose file is missing is recorded
as an explicit gap, never silently interpolated. **precip is deferred in v0**: HRRR F00
carries zero accumulation (precip needs a forecast bucket), so it is dropped with a gap
note rather than written as misleading zeros — a documented C2 refinement.

HRRR is CONUS-only; an AOI outside CONUS yields all-gap steps.
"""

from __future__ import annotations

import warnings
from dataclasses import dataclass, field
from datetime import datetime, timedelta

from terrain.util.logging import get_logger

from ember.weather.schema import GridSpec, StepProvenance, WeatherTimeline

log = get_logger(__name__)

# canonical variable -> herbie/cfgrib short name. precip (tp) is intentionally absent (v0).
_VMAP = {"wind10_u": "u10", "wind10_v": "v10", "t2": "t2m", "rh2": "r2"}
_SEARCH = r":(?:UGRD|VGRD):10 m above ground:|:(?:TMP|RH):2 m above ground:"
DEFAULT_VARS = ("wind10_u", "wind10_v", "t2", "rh2")


@dataclass
class HrrrTimeline:
    grid: GridSpec
    times: list[datetime]
    steps: list[StepProvenance]
    fields: dict[str, object]  # var -> np.ndarray (num_steps, ny, nx), float32, NaN = gap
    variables: list[str]
    gaps: list[str] = field(default_factory=list)


def _utm_epsg(lon: float, lat: float) -> int:
    zone = int((lon + 180) / 6) + 1
    return (32600 if lat >= 0 else 32700) + zone


def target_grid(bbox: list[float], grid_res_m: float):
    """Coarse regular grid (cell-centered) in the AOI's UTM CRS covering ``bbox``.

    Returns (GridSpec, center_lon 2D, center_lat 2D) — the lon/lat of each cell center,
    used to sample the source field. Pure/offline (no network), so it is unit-testable.
    """
    import numpy as np
    from pyproj import Transformer

    minx, miny, maxx, maxy = bbox
    clon, clat = (minx + maxx) / 2, (miny + maxy) / 2
    epsg = _utm_epsg(clon, clat)
    to_utm = Transformer.from_crs("EPSG:4326", f"EPSG:{epsg}", always_xy=True)
    to_ll = Transformer.from_crs(f"EPSG:{epsg}", "EPSG:4326", always_xy=True)

    xs, ys = [], []
    for x in (minx, maxx):
        for y in (miny, maxy):
            ux, uy = to_utm.transform(x, y)
            xs.append(ux)
            ys.append(uy)
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    nx = max(1, int(np.ceil((x1 - x0) / grid_res_m)))
    ny = max(1, int(np.ceil((y1 - y0) / grid_res_m)))
    cx = x0 + (np.arange(nx) + 0.5) * grid_res_m
    cy = y0 + (np.arange(ny) + 0.5) * grid_res_m
    cxg, cyg = np.meshgrid(cx, cy)  # (ny, nx)
    clon_g, clat_g = to_ll.transform(cxg, cyg)
    grid = GridSpec(crs=f"EPSG:{epsg}", bbox=[x0, y0, x1 + grid_res_m, y1 + grid_res_m],
                    nx=nx, ny=ny, dx_m=grid_res_m, dy_m=grid_res_m)
    return grid, np.asarray(clon_g), np.asarray(clat_g)


def build_hrrr_timeline(
    bbox: list[float], t0: datetime, num_steps: int, step_minutes: int, *,
    variables: tuple[str, ...] = DEFAULT_VARS, save_dir, grid_res_m: float = 3000.0,
    product: str = "sfc",
) -> HrrrTimeline:
    """Assemble a gridded HRRR timeline for ``num_steps`` steps of ``step_minutes`` from t0."""
    import numpy as np
    from herbie import Herbie
    from scipy.spatial import cKDTree

    gaps: list[str] = []
    wanted = [v for v in variables if v in _VMAP]
    for v in variables:
        if v not in _VMAP:
            gaps.append(f"variable {v!r} deferred in HRRR v0 (e.g. precip needs a forecast bucket)")

    grid, clon_g, clat_g = target_grid(bbox, grid_res_m)
    ny, nx = grid.ny, grid.nx
    times = [t0 + timedelta(minutes=step_minutes * i) for i in range(num_steps)]
    fields = {v: np.full((num_steps, ny, nx), np.nan, dtype="float32") for v in wanted}
    steps: list[StepProvenance] = []
    nn_idx = None  # nearest source cell per target cell; built once (grid is static)

    for i, vt in enumerate(times):
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            try:
                H = Herbie(vt.strftime("%Y-%m-%d %H:%M"), model="hrrr", product=product,
                           fxx=0, save_dir=str(save_dir), verbose=False)
                if H.grib is None:
                    raise FileNotFoundError("no HRRR grib for this hour")
                ds = H.xarray(_SEARCH, remove_grib=False)
            except Exception as ex:  # noqa: BLE001 — a missing hour is a gap, not a failure
                gaps.append(f"{vt.isoformat()}: HRRR analysis unavailable ({str(ex)[:60]})")
                steps.append(StepProvenance(index=i, valid_time=vt, gridded_source="missing",
                                            note=str(ex)[:80]))
                continue

        dslist = ds if isinstance(ds, list) else [ds]
        vals: dict[str, object] = {}
        src_lonlat = None
        for d in dslist:
            for canon, hname in _VMAP.items():
                if canon in wanted and hname in d.data_vars:
                    vals[canon] = np.asarray(d[hname].values).ravel()
                    if src_lonlat is None:
                        lat = np.asarray(d["latitude"].values).ravel()
                        lon = np.asarray(d["longitude"].values).ravel()
                        src_lonlat = (np.where(lon > 180, lon - 360, lon), lat)
        if src_lonlat is None:
            gaps.append(f"{vt.isoformat()}: no requested vars in HRRR message set")
            steps.append(StepProvenance(index=i, valid_time=vt, gridded_source="missing"))
            continue
        if nn_idx is None:  # build the regrid mapping once from the static HRRR grid
            slon, slat = src_lonlat
            tree = cKDTree(np.column_stack([slon, slat]))
            _, nn_idx = tree.query(np.column_stack([clon_g.ravel(), clat_g.ravel()]))
        for v in wanted:
            if v in vals:
                fields[v][i] = vals[v][nn_idx].reshape(ny, nx)
        steps.append(StepProvenance(index=i, valid_time=vt, gridded_source="hrrr:anl"))

    n_ok = sum(1 for s in steps if s.gridded_source == "hrrr:anl")
    log.info("hrrr timeline: %d/%d steps, %d var(s), grid %dx%d @ %.0fm, %d gap(s)",
             n_ok, num_steps, len(wanted), nx, ny, grid_res_m, len(gaps))
    return HrrrTimeline(grid=grid, times=times, steps=steps, fields=fields,
                        variables=wanted, gaps=gaps)


def write_grid_sidecar(tl: HrrrTimeline, path) -> None:
    """Persist the gridded fields to a compressed .npz the manifest references."""
    from pathlib import Path

    import numpy as np

    Path(path).parent.mkdir(parents=True, exist_ok=True)
    arrays = {v: tl.fields[v] for v in tl.variables}
    np.savez_compressed(
        path,
        times=np.array([t.isoformat() for t in tl.times]),
        variables=np.array(tl.variables),
        **arrays,
    )


def to_manifest(tl: HrrrTimeline, incident_id: str, *, step_minutes: int,
                grid_data: str) -> WeatherTimeline:
    """Build a gridded-only WeatherTimeline manifest (stations merge in later, C4)."""
    return WeatherTimeline(
        incident_id=incident_id, t0=tl.times[0], step_minutes=step_minutes,
        num_steps=len(tl.times), variables=tl.variables, grid=tl.grid,
        steps=tl.steps, grid_data=grid_data, gaps=tl.gaps,
    )
