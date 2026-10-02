"""The fire timeline (External Sources X1.1): the one internal shape every external source is
normalised into before it reaches the sim's arrival-playback driver.

A timeline is a grid plus:
- ``arrival_s``  float64, seconds since ``t_ref_utc`` (the source's own time zero, e.g. the run
  time of a forecast); NaN where the source does not give a time.
- ``burned_before`` bool: the source says the cell had burned at or before ``t_ref_utc`` (a
  forecast's starting perimeter) but gives no time for it.
- optional per-cell ``layers`` (flame length, spread rate, crown class ... at arrival), each with
  a "source speaks" mask, the unit, and the source's own unit.
- a ``speaks`` mask for arrival: the cells the source covers at all (its grid / horizon). Inside
  it, "no arrival and not burned_before" means the source says "did not burn by the horizon".
- provenance: source, run time, member (percentile), licence class, horizon.

On disk: ``<dir>/timeline.json`` (manifest) + one ``.npy`` per array. Nothing here knows about any
particular source.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np

FORMAT = "ember-fire-timeline"
VERSION = 1


@dataclass
class Grid:
    nx: int
    ny: int
    cell_size_m: float
    origin_x: float   # west edge (m)
    origin_y: float   # north edge (m)
    crs: str

    def to_dict(self) -> dict[str, Any]:
        return {"nx": self.nx, "ny": self.ny, "cell_size_m": self.cell_size_m,
                "origin_x": self.origin_x, "origin_y": self.origin_y, "crs": self.crs}


@dataclass
class Layer:
    values: np.ndarray            # (ny, nx)
    speaks: np.ndarray            # (ny, nx) bool: the source gave a value here
    unit: str                     # our unit, or "raw" while the source unit is unconfirmed
    source_unit: str              # what the source says (or "unstated")
    note: str = ""


@dataclass
class FireTimeline:
    grid: Grid
    t_ref_utc: str                # ISO 8601 Z: arrival_s is seconds since this
    arrival_s: np.ndarray         # (ny, nx) float64, NaN = no time from the source
    burned_before: np.ndarray     # (ny, nx) bool
    speaks: np.ndarray            # (ny, nx) bool: the source covers this cell
    provenance: dict[str, Any]
    layers: dict[str, Layer] = field(default_factory=dict)

    @property
    def burned(self) -> np.ndarray:
        """Cells the source says burn (with a time) - new growth."""
        return np.isfinite(self.arrival_s)

    def summary(self) -> dict[str, Any]:
        cell_ac = self.grid.cell_size_m ** 2 / 4046.8564224
        burned = self.burned
        out: dict[str, Any] = {
            "new_growth_cells": int(burned.sum()),
            "new_growth_acres": round(float(burned.sum() * cell_ac), 1),
            "burned_before_cells": int(self.burned_before.sum()),
            "arrival_max_h": (round(float(np.nanmax(self.arrival_s)) / 3600.0, 2)
                              if burned.any() else None),
            "layers": {},
        }
        for name, ly in self.layers.items():
            cover = int((ly.speaks & burned).sum())
            out["layers"][name] = {
                "unit": ly.unit, "source_unit": ly.source_unit,
                "coverage_of_new_growth": round(cover / max(1, int(burned.sum())), 4),
            }
        return out

    def growth_acres_by(self, hours: float) -> float:
        cell_ac = self.grid.cell_size_m ** 2 / 4046.8564224
        a = self.arrival_s
        return round(float((np.isfinite(a) & (a <= hours * 3600.0)).sum() * cell_ac), 1)

    # ---- disk ----------------------------------------------------------------------------- #
    def write(self, out_dir: str | Path) -> Path:
        d = Path(out_dir)
        d.mkdir(parents=True, exist_ok=True)
        np.save(d / "arrival_s.npy", self.arrival_s.astype(np.float64))
        np.save(d / "burned_before.npy", self.burned_before.astype(bool))
        np.save(d / "speaks.npy", self.speaks.astype(bool))
        layers = {}
        for name, ly in self.layers.items():
            np.save(d / f"layer_{name}.npy", ly.values)
            np.save(d / f"layer_{name}.speaks.npy", ly.speaks.astype(bool))
            layers[name] = {"unit": ly.unit, "source_unit": ly.source_unit, "note": ly.note,
                            "dtype": str(ly.values.dtype)}
        manifest = {"format": FORMAT, "version": VERSION, "grid": self.grid.to_dict(),
                    "t_ref_utc": self.t_ref_utc, "provenance": self.provenance,
                    "layers": layers, "summary": self.summary()}
        (d / "timeline.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        return d


def read_timeline(path: str | Path) -> FireTimeline:
    d = Path(path)
    m = json.loads((d / "timeline.json").read_text(encoding="utf-8"))
    if m.get("format") != FORMAT:
        raise ValueError(f"not a fire timeline: {d}")
    layers = {}
    for name, meta in m["layers"].items():
        layers[name] = Layer(np.load(d / f"layer_{name}.npy"),
                             np.load(d / f"layer_{name}.speaks.npy"),
                             meta["unit"], meta["source_unit"], meta.get("note", ""))
    return FireTimeline(Grid(**m["grid"]), m["t_ref_utc"], np.load(d / "arrival_s.npy"),
                        np.load(d / "burned_before.npy"), np.load(d / "speaks.npy"),
                        m["provenance"], layers)
