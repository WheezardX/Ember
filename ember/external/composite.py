"""Composite arrival on a world pack's grid (External Sources X1.3): "observed so far, then the
forecast".

For a forecast timeline issued at ``t_ref`` (= R seconds after the pack's t0):
- cells the observation burned at or before R keep their observed arrival (the pack's own
  ``arrival_s``, e.g. the NIROPS playback);
- cells the forecast burns (new growth) get R + their forecast arrival, unless already observed
  burned before R;
- everything the observation burned AFTER R is dropped - this is the forecast's future, not the
  observed one.

The forecast is sampled onto the pack grid by nearest neighbour (each pack cell takes the source
cell under its centre); same CRS and cell size is required, and the sub-cell shift is recorded.
The seams between the two are counted, not resolved (plan D3: report and ask):
- ``source_start_not_observed``: the forecast's starting perimeter has these cells as burned at
  t_ref, the observation does not - left unburned (the source gives no time for them);
- ``observed_not_in_source_start``: observed burned by t_ref, outside the forecast's starting
  perimeter and new growth - kept (they come from the observation);
- ``forecast_growth_already_observed``: forecast new growth the observation had already burned -
  the observed (earlier) time is kept.

Writes a world pack variant (a copy of the pack with a new ``arrival_s`` and ``confidence``) and
the embersim scenario that replays it with arrival-playback, as the observed scenario does.
"""
from __future__ import annotations

import json
import shutil
from datetime import datetime
from pathlib import Path
from typing import Any

import numpy as np

from ember.external.timeline import FireTimeline
from ember.sim.worldpack import DTYPES, LAYER_SPECS, _pack_hash, _write_layer


def _utc(s: str) -> datetime:
    return datetime.fromisoformat(s.replace("Z", "+00:00"))


def _pack(pack_dir: Path) -> tuple[dict[str, Any], np.ndarray, np.ndarray]:
    m = json.loads((pack_dir / "world.json").read_text(encoding="utf-8"))
    g = m["grid"]
    shape = (g["ny"], g["nx"])

    def layer(name: str) -> np.ndarray:
        spec = m["layers"][name]
        return np.frombuffer((pack_dir / spec["file"]).read_bytes(),
                             DTYPES[spec["dtype"]]).reshape(shape)

    conf = layer("confidence") if "confidence" in m["layers"] else np.zeros(shape, np.uint8)
    return m, layer("arrival_s").astype(np.int64), conf


def sample_onto(tl: FireTimeline, grid: dict[str, Any]) -> tuple[np.ndarray, np.ndarray, dict]:
    """Source cell index (row, col) under each pack cell centre; -1 outside the source."""
    if tl.grid.crs != grid["crs"] or abs(tl.grid.cell_size_m - grid["cell_size_m"]) > 1e-6:
        raise ValueError(f"grid mismatch: source {tl.grid.crs} {tl.grid.cell_size_m} m vs pack "
                         f"{grid['crs']} {grid['cell_size_m']} m (resampling is not implemented)")
    c = grid["cell_size_m"]
    cx = (grid["origin_x"] + (np.arange(grid["nx"]) + 0.5) * c - tl.grid.origin_x) / c
    cy = (tl.grid.origin_y - (grid["origin_y"] - (np.arange(grid["ny"]) + 0.5) * c)) / c
    col = np.floor(cx).astype(np.int64)
    row = np.floor(cy).astype(np.int64)
    col[(col < 0) | (col >= tl.grid.nx)] = -1
    row[(row < 0) | (row >= tl.grid.ny)] = -1
    shift = {"dx_m": round(float((cx[0] - col[0] - 0.5) * c), 3) if col[0] >= 0 else None,
             "dy_m": round(float((cy[0] - row[0] - 0.5) * c), 3) if row[0] >= 0 else None,
             "offset_cells_x": round(float((grid["origin_x"] - tl.grid.origin_x) / c), 4),
             "offset_cells_y": round(float((tl.grid.origin_y - grid["origin_y"]) / c), 4),
             "method": "nearest (source cell under the pack cell centre)"}
    rr, cc = np.meshgrid(row, col, indexing="ij")
    return rr, cc, shift


def composite(tl: FireTimeline, pack_dir: str | Path) -> dict[str, Any]:
    pack_dir = Path(pack_dir)
    m, obs, conf = _pack(pack_dir)
    t0 = _utc(m["t0_utc"])
    R = int(round((_utc(tl.t_ref_utc) - t0).total_seconds()))
    rr, cc, shift = sample_onto(tl, m["grid"])
    inside = (rr >= 0) & (cc >= 0)
    r0, c0 = np.where(inside, rr, 0), np.where(inside, cc, 0)
    fa = np.where(inside, tl.arrival_s[r0, c0], np.nan)
    fb = inside & tl.burned_before[r0, c0]
    fc = np.isfinite(fa)

    obs_before = (obs >= 0) & (obs <= R)
    out = np.full(obs.shape, -1, np.int64)
    out[obs_before] = obs[obs_before]
    take = fc & ~obs_before
    out[take] = R + np.rint(fa[take]).astype(np.int64)
    cell_ac = m["grid"]["cell_size_m"] ** 2 / 4046.8564224
    seams = {
        "source_start_not_observed": int((fb & ~obs_before).sum()),
        "observed_not_in_source_start": int((obs_before & ~fb & ~fc).sum()),
        "forecast_growth_already_observed": int((fc & obs_before).sum()),
        "observed_after_t_ref_dropped": int(((obs > R)).sum()),
    }
    seams["source_start_not_observed_acres"] = round(
        seams["source_start_not_observed"] * cell_ac, 1)
    seams["observed_not_in_source_start_acres"] = round(
        seams["observed_not_in_source_start"] * cell_ac, 1)
    conf_out = np.where(obs_before, conf, 0).astype(np.uint8)
    return {"meta": m, "arrival_s": out, "confidence": conf_out,
            "t_ref_s": R, "shift": shift, "seams": seams,
            "forecast_cells_on_pack": int(take.sum()),
            "observed_cells_before": int(obs_before.sum()),
            "outside_source_cells": int((~inside).sum())}


def write_pack(tl: FireTimeline, pack_dir: str | Path, out_dir: str | Path, name: str) -> Path:
    """The pack variant ``<out_dir>/<name>.ewp`` with the composite arrival."""
    pack_dir = Path(pack_dir)
    c = composite(tl, pack_dir)
    m = c["meta"]
    dst = Path(out_dir) / f"{name}.ewp"
    if dst.exists():
        shutil.rmtree(dst)
    dst.mkdir(parents=True)
    layers: dict[str, Any] = {}
    for lname, spec in m["layers"].items():
        if lname in ("arrival_s", "confidence"):
            continue
        shutil.copyfile(pack_dir / spec["file"], dst / spec["file"])
        layers[lname] = spec
    a = c["arrival_s"]
    _write_layer(dst, "arrival_s", a.clip(-1, 2**31 - 1), a >= 0, layers)
    _write_layer(dst, "confidence", c["confidence"], c["confidence"] > 0, layers)
    ordered = {k: layers[k] for k in LAYER_SPECS if k in layers}
    manifest = dict(m)
    manifest["name"] = name
    manifest["layers"] = ordered
    manifest["arrival"] = {
        "algorithm": "external-composite-v1",
        "observed": {"pack": str(pack_dir), "algorithm": (m.get("arrival") or {}).get("algorithm"),
                     "until_s": c["t_ref_s"]},
        "forecast": {k: tl.provenance.get(k) for k in
                     ("source", "fire", "run_ts", "run_utc", "member", "licence", "attribution")},
        "t_ref_s": c["t_ref_s"], "grid_shift": c["shift"], "seams": c["seams"],
        "forecast_cells_on_pack": c["forecast_cells_on_pack"],
        "observed_cells_before": c["observed_cells_before"],
        "outside_source_cells": c["outside_source_cells"],
        "pack_burned_cells": int((a >= 0).sum()),
    }
    manifest["pack_hash"] = _pack_hash(dst, ordered)
    (dst / "world.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return dst


def write_sim_scenario(pack: Path, out_toml: Path, run_dir: Path, name: str, duration_s: int,
                       residence_s: int = 21600) -> Path:
    """The embersim scenario that replays a composite pack (hourly ticks, as tq26-ir-playback)."""
    rel = lambda p: Path(p).resolve().as_posix()  # noqa: E731 - absolute: the toml lives in store
    out_toml.parent.mkdir(parents=True, exist_ok=True)
    out_toml.write_text(
        "# Generated by ember.external.composite (External Sources X1.3):"
        " observed, then the forecast.\n"
        "scenario_version = 1\n\n[scenario]\n"
        f'name = "{name}"\nworld = "{rel(pack)}"\nt_start_s = 0\nduration_s = {duration_s}\n'
        "dt_s = 3600\nseed = 1\n\n[model]\nid = \"arrival-playback\"\n\n[model.overrides]\n"
        f'"playback.residence_s" = {residence_s}\n\n[weather]\nmode = "constant"\n\n'
        f'[output]\ndir = "{rel(run_dir)}"\nkeyframe_every = 24\nstream = true\n',
        encoding="utf-8")
    return out_toml
