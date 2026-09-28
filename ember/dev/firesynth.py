"""Synthetic fire replay over any Terrain region (EPIC_5_PLAN two-track note: develop fire
rendering against a synthetic state stream while real worlds bake).

A wind-driven elliptical fire from an ignition point: arrival time grows with an anisotropic
distance (fast downwind head, slow backing, flanks between) plus a little value noise so the
front is not a perfect ellipse; FBFM40 non-burnable cells (91-99) never burn; cells burn for
`residence_h` and then stay burned. Written with Epic 4's own writer (ember.sim.stream), so the
result is a genuine `.ess` + `.replay.json` pair on a 30 m grid aligned to the region's DEM.

Output: store/render/<region>/fire/synth.{ess,replay.json}
"""

from __future__ import annotations

import json
import math
from pathlib import Path

import numpy as np

CELL_M = 30.0


def synth(region_dir: Path, out_dir: Path, *, ignition_frac=(0.5, 0.5), hours=48,
          head_ms=0.05, wind_to_deg=60.0, residence_h=6, seed=7) -> Path:
    import rasterio
    from rasterio.enums import Resampling

    from ember.sim.stream import (
        DIRTY_DT,
        METRIC_FIELDS,
        OVERLAY_DT,
        REJECTED_DT,
        SPOT_DT,
        End,
        Keyframe,
        StreamHeader,
        Tick,
        rle_encode,
        write_stream,
    )

    with rasterio.open(region_dir / "fuels" / "fbfm40.cog.tif") as ds:
        west, north = ds.bounds.left, ds.bounds.top
        nx = int((ds.bounds.right - west) // CELL_M)
        ny = int((north - ds.bounds.bottom) // CELL_M)
        fb = ds.read(1, out_shape=(ny, nx), resampling=Resampling.nearest)
        crs = str(ds.crs)
    n = nx * ny
    burnable = (fb > 0) & ~((fb >= 91) & (fb <= 99))

    # Arrival time: anisotropic distance from the ignition (head runs along wind_to_deg).
    iy, ix = int(ignition_frac[1] * ny), int(ignition_frac[0] * nx)
    yy, xx = np.mgrid[0:ny, 0:nx]
    dx, dy = (xx - ix) * CELL_M, -(yy - iy) * CELL_M          # east, north (m)
    th = math.radians(wind_to_deg)
    along = dx * math.sin(th) + dy * math.cos(th)             # + downwind
    across = -dx * math.cos(th) + dy * math.sin(th)
    rng = np.random.default_rng(seed)
    coarse = rng.random((ny // 20 + 2, nx // 20 + 2))
    noise = np.kron(coarse, np.ones((20, 20)))[:ny, :nx]      # blocky -> smoothed below
    from scipy import ndimage
    noise = ndimage.gaussian_filter(noise, 8)
    speed_along = np.where(along >= 0, head_ms, head_ms * 0.15)
    t = np.sqrt((along / speed_along) ** 2 + (across / (head_ms * 0.35)) ** 2)
    t = t * (0.75 + 0.5 * noise)
    arrival = np.where(burnable & (t <= hours * 3600), t, np.inf)
    fin = np.isfinite(arrival)
    arrival_s = np.full(arrival.shape, -1, np.int32)
    arrival_s[fin] = (arrival[fin] // 60 * 60).astype(np.int32)
    arrival_s = arrival_s.ravel()

    h = StreamHeader(nx=nx, ny=ny, cell_mm=int(CELL_M * 1000), t0_unix=1_500_000_000, dt_s=3600,
                     keyframe_every=24, model_id="synthetic-ellipse", model_version="1",
                     interface_version="1.0.0")
    phase = np.where(burnable.ravel(), 1, 0).astype(np.uint8)
    inten = np.zeros(n, np.uint8)
    recs = [Keyframe(0, 0, rle_encode(phase), rle_encode(inten))]
    for tick in range(1, hours + 1):
        ts = tick * 3600
        new_phase = phase.copy()
        lit = (arrival_s >= 0) & (arrival_s <= ts)
        out = lit & (arrival_s + residence_h * 3600 <= ts)
        new_phase[lit & (phase == 1)] = 2
        new_phase[out] = 3
        new_int = np.where(new_phase == 2, 1, 0).astype(np.uint8)
        idx = np.nonzero((new_phase != phase) | (new_int != inten))[0].astype(np.uint32)
        d = np.zeros(len(idx), DIRTY_DT)
        d["idx"], d["phase"], d["intensity"] = idx, new_phase[idx], new_int[idx]
        d["arrival_s"] = arrival_s[idx]
        phase, inten = new_phase, new_int
        m = dict.fromkeys(METRIC_FIELDS, 0)
        m["burning"], m["burned"] = int((phase == 2).sum()), int((phase == 3).sum())
        recs.append(Tick(tick, ts, tick, d, np.zeros(0, SPOT_DT), np.zeros(0, OVERLAY_DT),
                         np.zeros(0, REJECTED_DT), m, {}))
        if tick % 24 == 0:
            recs.append(Keyframe(tick, ts, rle_encode(phase), rle_encode(inten)))
    recs.append(End(hours, 0))
    out_dir.mkdir(parents=True, exist_ok=True)
    ess = out_dir / "synth.ess"
    write_stream(ess, h, recs)
    replay = {"format": "ember-replay", "version": 1, "interface_version": "1.0.0",
              "stream_version": 1,
              "model": {"id": "synthetic-ellipse", "version": "1",
                        "overrides": {"head_ms": head_ms, "wind_to_deg": wind_to_deg,
                                      "residence_h": residence_h, "seed": seed}},
              "world": {"name": region_dir.name, "t0_unix": h.t0_unix,
                        "grid": {"nx": nx, "ny": ny, "cell_size_m": CELL_M, "crs": crs,
                                 "origin_x": west, "origin_y": north}},
              "result": {"ticks": hours}, "stream": ess.name}
    path = out_dir / "synth.replay.json"
    path.write_text(json.dumps(replay, indent=1), encoding="utf-8")
    return path
