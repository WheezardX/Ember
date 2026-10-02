"""Fidelity sidecar v0 (External Sources X1.4): does the replay say what the source said?

Plan D3 (strict; Brad: "start strict and adapt if needed, with a bias against any additional
modelling"): every source-burned cell must APPEAR in the stream within one tick of the source's
time, and no cell may burn in the source's part of the replay without the source's support.

The replay is the composite of ember.external.composite: observed history up to t_ref (R), then
the source. Checked on the pack grid, with each pack cell mapped to the source cell under its
centre (the composite's own sampling):
- source time per cell: R + the source arrival (new growth); R for the source's starting
  perimeter (it gives no time; the "source" start policy shows it at R);
- ``appear_s``: the stream tick at which the cell first shows as burning / burned;
- in the source's domain (appearing after R, or the start step at R): no cell without source
  support (``unsupported``, must be 0); every supported cell within one tick (``late`` /
  ``early``, must be 0); every source cell inside the replay's duration present (``missing``,
  must be 0); the exact ``arrival_s`` the stream carries equals the source time (to the second);
- observed history (appearing before R) is reported, not judged here - it is the observation's.

Writes ``<stem>.fidelity.json`` beside the replay. ``ember-dev evaluate`` fails a run whose
replay has a sidecar with ``ok: false``.
"""
from __future__ import annotations

import json
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

import numpy as np

from ember.external.composite import sample_onto
from ember.external.timeline import FireTimeline
from ember.sim.stream import (
    Keyframe,
    StreamHeader,
    Tick,
    read_replay,
    read_stream,
    replay_stream_path,
)

FORMAT = "ember-fidelity"


def sidecar_path(replay: str | Path) -> Path:
    p = Path(replay)
    stem = p.name[: -len(".replay.json")] if p.name.endswith(".replay.json") else p.stem
    return p.with_name(f"{stem}.fidelity.json")


def stream_appearance(ess: Path) -> tuple[StreamHeader, np.ndarray, np.ndarray, int]:
    """(header, appear_s, arrival_s, last t_s): when each cell first shows as burning or burned
    (phase >= 2), and the arrival the stream carries for it."""
    header, recs = read_stream(ess)
    n = header.nx * header.ny
    appear = np.full(n, -1, np.int64)
    arrival = np.full(n, -1, np.int64)
    last_t = 0
    for r in recs:
        if isinstance(r, Keyframe):
            ph, _ = r.expand(n)
            new = (ph >= 2) & (appear < 0)
            appear[new] = r.t_s
        elif isinstance(r, Tick):
            last_t = r.t_s
            d = r.dirty
            if d.size:
                idx = d["idx"].astype(np.int64)
                burn = d["phase"] >= 2
                fresh = idx[burn][appear[idx[burn]] < 0]
                appear[fresh] = r.t_s
                arrival[idx] = d["arrival_s"]
    return header, appear, arrival, last_t


def forecast_summary(tl: FireTimeline) -> dict[str, Any]:
    """What the source said, so a passing sidecar also reads as a record of what was rendered
    (Brad 2026-10-02): growth since the run by hour (on the source's own grid), the arrival
    range, and the starting area. Descriptive only - no comparison with any observation."""
    a = tl.arrival_s[np.isfinite(tl.arrival_s)] / 3600.0
    cell_ac = tl.grid.cell_size_m ** 2 / 4046.8564224
    horizon = tl.provenance.get("horizon_h")
    growth = {f"{h}h": tl.growth_acres_by(h) for h in (24, 48, 72)}
    growth["end"] = round(float(a.size * cell_ac), 1)
    return {
        "member": tl.provenance.get("member"), "run_utc": tl.provenance.get("run_utc"),
        "horizon_h": horizon,
        "growth_acres_since_run": growth,
        "arrival_h": ({"first": round(float(a.min()), 2), "median": round(float(np.median(a)), 2),
                       "last": round(float(a.max()), 2)} if a.size else None),
        "starting_area_acres": round(float(tl.burned_before.sum() * cell_ac), 1),
    }


def evaluate(tl: FireTimeline, pack_dir: str | Path, replay: str | Path) -> dict[str, Any]:
    pack_dir = Path(pack_dir)
    m = json.loads((pack_dir / "world.json").read_text(encoding="utf-8"))
    meta = m["arrival"]
    R = int(meta["t_ref_s"])
    grid = m["grid"]
    read_replay(replay)                                     # (validates the format)
    ess = replay_stream_path(replay)
    header, appear, arrival, last_t = stream_appearance(ess)
    dt = int(header.dt_s)
    shape = (grid["ny"], grid["nx"])
    appear, arrival = appear.reshape(shape), arrival.reshape(shape)

    rr, cc, shift = sample_onto(tl, grid)
    inside = (rr >= 0) & (cc >= 0)
    r0, c0 = np.where(inside, rr, 0), np.where(inside, cc, 0)
    fa = np.where(inside, tl.arrival_s[r0, c0], np.nan)
    growth = np.isfinite(fa)
    start = inside & tl.burned_before[r0, c0]
    obs_hist = (appear >= 0) & (appear < R)               # shown before the run: the observation's

    src_t = np.full(shape, -1, np.int64)
    src_t[growth] = R + np.rint(fa[growth]).astype(np.int64)
    policy = (meta.get("start") or {}).get("policy", "observed")
    if policy == "source":
        src_t[start & ~growth] = R
    supported = src_t >= 0
    domain = (appear >= R) & ~obs_hist                     # what the replay shows from the run on
    unsupported = domain & ~supported
    sup_dom = supported & ~obs_hist                         # source cells the replay must show
    due = sup_dom & (src_t <= last_t)
    shown = due & (appear >= 0)
    missing = due & (appear < 0)
    # the playback shows a cell at the first tick at or after its time: lag in [0, dt)
    lag = np.where(shown, appear - src_t, 0)
    late = shown & (lag >= dt)
    early = shown & (lag < 0)
    exact = shown & (arrival == src_t)
    cell_ac = grid["cell_size_m"] ** 2 / 4046.8564224
    n_due = int(due.sum())
    out = {
        "format": FORMAT, "version": 0,
        "evaluated_utc": datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "replay": str(replay), "pack": str(pack_dir),
        "source": {k: tl.provenance.get(k) for k in
                   ("source", "fire", "run_ts", "run_utc", "member", "licence", "attribution")},
        "test": "D3 strict: every source cell shown within one tick of the source time; no cell "
                "burns in the source's part of the replay without source support",
        "tick_s": dt, "t_ref_s": R, "replay_end_s": last_t,
        "grid_shift": shift, "start_policy": policy,
        "source_cells_due": n_due,
        "source_cells_shown": int(shown.sum()),
        "share_within_one_tick": round(float((shown & ~late & ~early).sum()) / max(1, n_due), 6),
        "exact_arrival_share": round(float(exact.sum()) / max(1, n_due), 6),
        "missing": int(missing.sum()), "late": int(late.sum()), "early": int(early.sum()),
        "unsupported": int(unsupported.sum()),
        "source_acres_due": round(n_due * cell_ac, 1),
        "replay_acres_from_source": round(float(shown.sum()) * cell_ac, 1),
        "observed_history_cells": int(obs_hist.sum()),
        "seams": meta.get("seams"),
        "forecast": forecast_summary(tl),
    }
    out["ok"] = (out["missing"] == 0 and out["late"] == 0 and out["early"] == 0
                 and out["unsupported"] == 0 and n_due > 0)
    return out


def write(tl: FireTimeline, pack_dir: str | Path, replay: str | Path) -> Path:
    res = evaluate(tl, pack_dir, replay)
    p = sidecar_path(replay)
    p.write_text(json.dumps(res, indent=2), encoding="utf-8")
    return p
