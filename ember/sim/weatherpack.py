"""Weather pack v1 export (docs/sim/formats.md §2; ADR 0009 amendments A-C).

Turns an Epic 3 `weather-timeline-v0` manifest + npz sidecar into the int16-quantised form
the C++ core samples: `weather.json` + `weather.bin` = i16[num_steps][5][ny][nx], always the
five variables in the fixed order, **row 0 = north** (the HRRR sidecar's rows run
south->north — `hrrr.target_grid` builds centres from miny upward — so rows are flipped here).
Gaps are held from the previous valid step and flagged; a timeline that begins with a gap is
refused.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import numpy as np

from ember.sim import WEATHER_PACK_FORMAT, WEATHER_PACK_VERSION
from ember.weather.schema import WeatherTimeline

VARIABLES = ["wind10_u", "wind10_v", "t2", "rh2", "precip"]
QUANT = {"wind10_u": "cm/s", "wind10_v": "cm/s", "t2": "0.1K", "rh2": "0.1%",
         "precip": "0.01mm"}
_SCALE = {"wind10_u": 100.0, "wind10_v": 100.0, "t2": 10.0, "rh2": 10.0, "precip": 100.0}
_I16 = (-32768, 32767)


def quantise(var: str, values: np.ndarray) -> np.ndarray:
    """SI float -> int16 per QUANT (rint, clipped to int16). NaN -> 0 (caller counts them)."""
    v = np.asarray(values, dtype=np.float64) * _SCALE[var]
    v = np.where(np.isfinite(v), v, 0.0)
    return np.clip(np.rint(v), *_I16).astype("<i2")


def export_weather(timeline_manifest_path: str | Path, store_root: str | Path,
                   out_dir: str | Path, world_grid: dict[str, Any] | None = None) -> str:
    """Write `weather.json` + `weather.bin` into `out_dir`; returns "weather.json".

    `store_root` is the directory the timeline's store-relative sidecar paths hang off (the
    incident directory). `world_grid` (pack manifest `grid`) is checked for CRS equality —
    the core assumes both grids share a CRS.
    """
    tl_path = Path(timeline_manifest_path)
    tl = WeatherTimeline.model_validate_json(tl_path.read_text(encoding="utf-8"))
    if tl.grid is None or not tl.grid_data:
        raise ValueError(f"{tl_path}: timeline has no gridded field; the sim needs one")
    if world_grid is not None and world_grid.get("crs") not in (None, "LOCAL", tl.grid.crs):
        raise ValueError(f"weather grid CRS {tl.grid.crs} != world CRS {world_grid['crs']}")
    npz_path = Path(store_root) / tl.grid_data
    z = np.load(npz_path)
    ny, nx, n = tl.grid.ny, tl.grid.nx, tl.num_steps

    out = np.zeros((n, len(VARIABLES), ny, nx), dtype="<i2")
    missing_vars: list[str] = []
    nan_zeroed = 0
    present: list[np.ndarray] = []
    for vi, var in enumerate(VARIABLES):
        if var not in z.files:
            missing_vars.append(var)
            continue
        a = np.asarray(z[var], dtype=np.float64)
        if a.shape != (n, ny, nx):
            raise ValueError(f"{var}: sidecar shape {a.shape} != ({n}, {ny}, {nx})")
        a = a[:, ::-1, :]  # south-up -> north-up
        present.append(a)
        q = quantise(var, a)
        nan_zeroed += int((~np.isfinite(a)).sum())
        out[:, vi] = q

    # Gap detection: provenance says missing, or every present variable is all-NaN.
    prov = {s.index: s.gridded_source for s in tl.steps}
    all_nan = np.zeros(n, bool)
    if present:
        stacked = np.stack(present)  # (nvar, n, ny, nx)
        all_nan = np.all(~np.isfinite(stacked), axis=(0, 2, 3))
    steps_meta: list[dict[str, Any]] = []
    last_valid: int | None = None
    held = 0
    for i in range(n):
        src = prov.get(i, "unknown")
        is_missing = src == "missing" or bool(all_nan[i])
        held_from = None
        if is_missing:
            if last_valid is None:
                raise ValueError(f"{tl_path}: timeline starts with a missing step (index {i}); "
                                 "ADR 0009 B refuses to hold from nothing")
            out[i] = out[last_valid]
            held_from = last_valid
            held += 1
        else:
            last_valid = i
        vt = tl.steps[i].valid_time.isoformat() if i < len(tl.steps) else None
        steps_meta.append({"index": i, "valid_time": vt, "source": src, "held_from": held_from})

    # Grid: origin = top-left corner. hrrr.target_grid: x0 = bbox[0]; centres from y0 up, so
    # the top edge is y0 + ny*dy (bbox[3] may overshoot by up to one cell).
    x0, y0 = float(tl.grid.bbox[0]), float(tl.grid.bbox[1])
    grid = {"nx": nx, "ny": ny, "dx_m": float(tl.grid.dx_m), "dy_m": float(tl.grid.dy_m),
            "origin_x": x0, "origin_y": y0 + ny * float(tl.grid.dy_m), "crs": tl.grid.crs}

    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "weather.bin").write_bytes(np.ascontiguousarray(out).tobytes(order="C"))
    manifest = {
        "format": WEATHER_PACK_FORMAT, "version": WEATHER_PACK_VERSION,
        "grid": grid,
        "t0_unix": int(tl.t0.timestamp()), "step_s": int(tl.step_minutes) * 60,
        "num_steps": n,
        "variables": VARIABLES, "quant": QUANT,
        "file": "weather.bin",
        "steps": steps_meta,
        "gaps": list(tl.gaps),
        "held_steps": held,
        "precip_present": "precip" not in missing_vars,
        "missing_variables": missing_vars,
        "nan_cells_zeroed": nan_zeroed,
        "source_timeline": str(tl_path.name if tl_path.parent == Path(store_root)
                               else tl_path.relative_to(store_root)
                               if tl_path.is_relative_to(store_root) else tl_path),
        "source_incident_id": tl.incident_id,
    }
    (out_dir / "weather.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return "weather.json"
