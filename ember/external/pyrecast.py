"""PyreCast / ELMFIRE forecast run -> fire timeline (External Sources X1.2).

A run directory (the mirror's layout, see the mirror's PROVENANCE.md) holds per percentile:
- ``{pct}.tif``: time of arrival, float32 hours since the run, nodata 0, new growth only (the
  starting perimeter is nodata), 14-day horizon;
- ``{pct}_{var}.tar``: hourly GeoTIFF granules on the same grid, named ``{var}_{YYYYMMDD_HHMMSS}``:
  the run time itself, then every whole hour, up to 169 (7 days); some were purged before the
  archive caught them.

The timeline gets arrival (s since the run), ``burned_before`` from the run-time
``hours-since-burned`` granule (the area already burned when the forecast started), and three
layers sampled at each cell's arrival hour from the granule at the first expected granule time at
or after the arrival: flame length, spread rate and crown fire. The files do not state units or
class meanings, so the layers stay RAW codes (unit "raw") until X2 confirms them against the
ELMFIRE / PyreCast documentation. A missing granule, or a nodata value in it, leaves the layer
silent for that cell - never interpolated.

Licence: PyreCast data is internal / reference-only (EXTERNAL_SOURCES_PLAN rule 1). Timelines
built from it are written under the git-ignored store and carry the licence in their provenance.
"""
from __future__ import annotations

import re
import tarfile
from datetime import UTC, datetime, timedelta
from pathlib import Path

import numpy as np

from ember.external.timeline import FireTimeline, Grid, Layer

ATTRIBUTION = "Data source: PyreCast Wildfire Forecasting Platform (pyrecast.org)"
LICENCE = "pyrecast-internal (non-commercial; no redistribution of data or derived products)"
# var -> (layer name, nodata)
LAYER_VARS = {"flame-length": ("flame_length", 0), "spread-rate": ("spread_rate", 0),
              "crown-fire": ("crown_fire", 255)}
_GRANULE = re.compile(r"^(?P<var>[a-z-]+)_(?P<ts>\d{8}_\d{6})\.tif$")


def run_time(run_dir: Path) -> datetime:
    return datetime.strptime(run_dir.name, "%Y%m%d_%H%M%S").replace(tzinfo=UTC)


def _expected_times(run: datetime, n: int = 169) -> list[datetime]:
    """The run time, then each whole hour after it: the granule clock."""
    first_hour = (run + timedelta(hours=1)).replace(minute=0, second=0, microsecond=0)
    if run.minute == 0 and run.second == 0:
        first_hour = run + timedelta(hours=1)
    return [run] + [first_hour + timedelta(hours=k) for k in range(n - 1)]


def _granules(tar_path: Path, var: str) -> dict[datetime, str]:
    with tarfile.open(tar_path) as t:
        out = {}
        for name in t.getnames():
            m = _GRANULE.match(Path(name).name)
            if m and m["var"] == var:
                out[datetime.strptime(m["ts"], "%Y%m%d_%H%M%S").replace(tzinfo=UTC)] = name
        return out


def _read_member(tar_path: Path, member: str) -> np.ndarray:
    from rasterio.io import MemoryFile

    with tarfile.open(tar_path) as t:
        data = t.extractfile(member).read()
    with MemoryFile(data) as mf, mf.open() as ds:
        return ds.read(1)


def load_run(run_dir: str | Path, pct: int | str, *, layers: bool = True) -> FireTimeline:
    import rasterio

    run_dir = Path(run_dir)
    run = run_time(run_dir)
    with rasterio.open(run_dir / f"{pct}.tif") as ds:
        tf = ds.transform
        if abs(tf.a) != abs(tf.e) or tf.b != 0 or tf.d != 0:
            raise ValueError(f"{pct}.tif is not a square north-up grid: {tf}")
        grid = Grid(ds.width, ds.height, float(tf.a), float(tf.c), float(tf.f), str(ds.crs))
        a = ds.read(1).astype(np.float64)
        nod = ds.nodata
    burned = np.isfinite(a) & (a > 0) & ((a != nod) if nod is not None else True)
    arrival_s = np.where(burned, a * 3600.0, np.nan)

    notes: list[str] = []
    burned_before = np.zeros(a.shape, bool)
    hsb_tar = run_dir / f"{pct}_hours-since-burned.tar"
    if hsb_tar.exists():
        g = _granules(hsb_tar, "hours-since-burned")
        if run in g:
            burned_before = _read_member(hsb_tar, g[run]) > 0
            burned_before &= ~burned          # (new growth is never "before")
        else:
            notes.append("no run-time hours-since-burned granule: starting perimeter unknown")
    else:
        notes.append("no hours-since-burned archive: starting perimeter unknown")

    out_layers: dict[str, Layer] = {}
    granule_report: dict[str, dict] = {}
    if layers:
        expected = _expected_times(run)
        exp_h = np.array([(t - run).total_seconds() / 3600.0 for t in expected])
        # each burned cell -> index of the first expected granule time at or after its arrival
        idx = np.full(a.shape, -1, np.int32)
        ah = np.where(burned, a, np.inf)
        k = np.searchsorted(exp_h, ah[burned], side="left")
        kk = np.where(k < len(exp_h), k, -1)
        idx[burned] = kk
        for var, (name, nodata) in LAYER_VARS.items():
            tar = run_dir / f"{pct}_{var}.tar"
            values = np.zeros(a.shape, np.uint8)
            speaks = np.zeros(a.shape, bool)
            have = _granules(tar, var) if tar.exists() else {}
            missing = 0
            for j, t in enumerate(expected):
                sel = idx == j
                if not sel.any():
                    continue
                if t not in have:
                    missing += int(sel.sum())
                    continue
                g = _read_member(tar, have[t])
                v = g[sel]
                ok = v != nodata
                values[sel] = np.where(ok, v, 0).astype(np.uint8)
                sp = np.zeros(a.shape, bool)
                sp[sel] = ok
                speaks |= sp
            beyond = int((burned & (idx < 0)).sum())
            granule_report[name] = {"granules": len(have), "expected": len(expected),
                                    "new_growth_cells_on_missing_granules": missing,
                                    "new_growth_cells_beyond_granules": beyond}
            out_layers[name] = Layer(values, speaks, "raw", "unstated",
                                     f"PyreCast {var} code at the cell's arrival hour; "
                                     "units / classes unconfirmed (X2)")

    prov = {
        "source": "pyrecast-elmfire", "fire": run_dir.parent.name, "run_ts": run_dir.name,
        "run_utc": run.strftime("%Y-%m-%dT%H:%M:%SZ"), "member": f"p{pct}",
        "member_kind": "weather percentile", "licence": LICENCE, "attribution": ATTRIBUTION,
        "path": str(run_dir), "horizon_h": 336, "granules": granule_report, "notes": notes,
    }
    return FireTimeline(grid, prov["run_utc"], arrival_s, burned_before,
                        np.ones(a.shape, bool), prov, out_layers)
