"""Fire channels sidecar writer (ADR 0010, External Sources X2.1/X2.2): the source's physical
numbers per cell, on the world-pack grid, beside the pack - ``<pack>.channels.json`` (manifest) +
``<pack>.channels.bin`` (the grids, u16 little-endian, in manifest order).

value = raw x scale + offset; raw == nodata (65535) means the source is silent for that cell.
Scales are chosen as 0.01 of the SOURCE unit, so the source's own steps survive exactly
(PyreCast: 1 ft flame length, 1 ft/min spread rate). Conversion to SI happens here, once (D5).
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import numpy as np

from ember.external.composite import sample_onto
from ember.external.timeline import FireTimeline

FORMAT = "ember-fire-channels"
VERSION = 1
NODATA = 65535
FT = 0.3048

# PyreCast / ELMFIRE layer -> channel (ADR 0010 §4): name, SI unit, scale (SI per raw step),
# source unit, conversion note.
PYRECAST = {
    "flame_length": ("flame_length_m", "m", FT / 100.0, "ft",
                     "raw = ft x 100; m = ft x 0.3048. ELMFIRE's source writes active-crown "
                     "flame length as 2.5 x canopy height in m; PyreCast's values do not follow "
                     "that on our canopy data - treated as ft, as PyreCast labels them (ADR 0010)"),
    "spread_rate": ("spread_rate_mh", "m/h", FT * 60.0 / 100.0, "ft/min",
                    "raw = ft/min x 100; m/h = ft/min x 18.288 (head-fire spread rate)"),
    "crown_fire": ("crown_class", "class", 1.0, "class",
                   "0 surface, 1 passive (torching), 2 active crown fire (ELMFIRE crown "
                   "fraction burned < 0.1 / 0.1-0.9 / > 0.9)"),
}
SOURCE_SCALE = {"flame_length": 100.0, "spread_rate": 100.0, "crown_fire": 1.0}


def sidecar_paths(pack_dir: str | Path) -> tuple[Path, Path]:
    p = Path(pack_dir)
    stem = p.name[: -len(".ewp")] if p.name.endswith(".ewp") else p.name
    return p.with_name(f"{stem}.channels.json"), p.with_name(f"{stem}.channels.bin")


def write(channels: list[dict[str, Any]], grid: dict[str, Any], pack_dir: str | Path,
          provenance: dict[str, Any]) -> Path:
    """channels: [{name, unit, scale, offset, raw (ny, nx) u16 with NODATA where silent,
    source: {...}}] -> the sidecar. Returns the manifest path."""
    mpath, bpath = sidecar_paths(pack_dir)
    ny, nx = grid["ny"], grid["nx"]
    entries = []
    with bpath.open("wb") as f:
        for i, ch in enumerate(channels):
            raw = np.ascontiguousarray(ch["raw"], dtype="<u2")
            if raw.shape != (ny, nx):
                raise ValueError(f"channel {ch['name']}: {raw.shape} != {(ny, nx)}")
            f.write(raw.tobytes(order="C"))
            speaks = raw != NODATA
            entries.append({
                "name": ch["name"], "kind": "at_arrival", "unit": ch["unit"], "dtype": "u16",
                "scale": ch["scale"], "offset": ch.get("offset", 0.0), "nodata": NODATA,
                "index": i, "cells_speaking": int(speaks.sum()),
                "max": (round(float(raw[speaks].max()) * ch["scale"] + ch.get("offset", 0.0), 4)
                        if speaks.any() else None),
                "source": ch["source"],
            })
    manifest = {"format": FORMAT, "version": VERSION,
                "grid": {k: grid[k] for k in ("nx", "ny", "cell_size_m", "origin_x", "origin_y",
                                              "crs")},
                "bin": bpath.name, "channels": entries, "provenance": provenance}
    mpath.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return mpath


def from_pyrecast(tl: FireTimeline, pack_dir: str | Path) -> Path:
    """The PyreCast timeline's layers on the pack grid -> the sidecar beside the pack."""
    pack_dir = Path(pack_dir)
    m = json.loads((pack_dir / "world.json").read_text(encoding="utf-8"))
    grid = m["grid"]
    rr, cc, shift = sample_onto(tl, grid)
    inside = (rr >= 0) & (cc >= 0)
    r0, c0 = np.where(inside, rr, 0), np.where(inside, cc, 0)
    chans = []
    for layer, (name, unit, scale, src_unit, note) in PYRECAST.items():
        ly = tl.layers.get(layer)
        if ly is None:
            continue
        speaks = inside & ly.speaks[r0, c0]
        raw = np.full((grid["ny"], grid["nx"]), NODATA, np.uint16)
        raw[speaks] = (ly.values[r0, c0][speaks].astype(np.float64)
                       * SOURCE_SCALE[layer]).round().astype(np.uint16)
        chans.append({"name": name, "unit": unit, "scale": scale, "offset": 0.0, "raw": raw,
                      "source": {"name": f"pyrecast {layer}", "unit": src_unit,
                                 "conversion": f"x {scale * SOURCE_SCALE[layer]:.6g}"
                                 f" -> {unit}", "note": note}})
    prov = {k: tl.provenance.get(k) for k in
            ("source", "fire", "run_ts", "run_utc", "member", "licence", "attribution")}
    prov["grid_shift"] = shift
    return write(chans, grid, pack_dir, prov)


def read(pack_dir: str | Path) -> dict[str, Any]:
    """{name: (values float64 with NaN where silent, entry)} + the manifest - the Python reader
    (tests, facts)."""
    mpath, _ = sidecar_paths(pack_dir)
    man = json.loads(mpath.read_text(encoding="utf-8"))
    g = man["grid"]
    raw_all = np.frombuffer((mpath.parent / man["bin"]).read_bytes(), "<u2")
    n = g["nx"] * g["ny"]
    out: dict[str, Any] = {"manifest": man}
    for e in man["channels"]:
        raw = raw_all[e["index"] * n:(e["index"] + 1) * n].reshape(g["ny"], g["nx"])
        val = np.where(raw == e["nodata"], np.nan, raw * e["scale"] + e["offset"])
        out[e["name"]] = (val, e)
    return out


def write_fixture(out_dir: str | Path) -> Path:
    """The tiny synthetic sidecar worldcore's reader is tested against (no PyreCast data):
    tests/data/fire_channels.channels.{json,bin} + .expected.json (decoded values)."""
    out_dir = Path(out_dir)
    ny, nx = 3, 4
    grid = {"nx": nx, "ny": ny, "cell_size_m": 30.0, "origin_x": 1000.0, "origin_y": 2000.0,
            "crs": "EPSG:32610"}
    fl = np.full((ny, nx), NODATA, np.uint16)
    fl[0, :3] = [100, 550, 4400]                 # 1, 5.5, 44 ft
    sr = np.full((ny, nx), NODATA, np.uint16)
    sr[0, 0], sr[1, 1] = 300, 17400              # 3, 174 ft/min
    cr = np.full((ny, nx), NODATA, np.uint16)
    cr[0, :3] = [0, 1, 2]
    chans = [
        {"name": "flame_length_m", "unit": "m", "scale": FT / 100.0, "raw": fl,
         "source": {"name": "synthetic", "unit": "ft"}},
        {"name": "spread_rate_mh", "unit": "m/h", "scale": FT * 60.0 / 100.0, "raw": sr,
         "source": {"name": "synthetic", "unit": "ft/min"}},
        {"name": "crown_class", "unit": "class", "scale": 1.0, "raw": cr,
         "source": {"name": "synthetic", "unit": "class"}},
    ]
    mpath = write(chans, grid, out_dir / "fire_channels.ewp", {"source": "synthetic"})
    dec = read(out_dir / "fire_channels.ewp")
    exp = {name: [None if np.isnan(v) else round(float(v), 6) for v in dec[name][0].ravel()]
           for name in ("flame_length_m", "spread_rate_mh", "crown_class")}
    (out_dir / "fire_channels.expected.json").write_text(json.dumps(exp, indent=1),
                                                         encoding="utf-8")
    return mpath
