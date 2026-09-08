"""Stream probe: per-tick numbers for eyeballing a run without rendering (plan G-stream)."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import numpy as np

from ember.sim.stream import DELTA_KINDS, iter_frames

COLUMNS = ("tick", "t_s", "burned", "burning", "bbox", "spots", "spots_ignited", "overlay",
           "state_hash")


def probe_stream(path: str | Path, every: int = 1) -> list[dict[str, Any]]:
    """One dict per sampled tick: t_s, burned/burning cells, fire bbox (x0, y0, x1, y1;
    inclusive-exclusive, over phase >= 2, None if no fire), spot launches / ignitions,
    overlay counts by delta kind name, state hash."""
    rows: list[dict[str, Any]] = []
    for _header, f in iter_frames(path, every=every):
        fire = f.phase >= 2
        ys, xs = np.nonzero(fire)
        bbox = ((int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)
                if xs.size else None)
        kinds, counts = np.unique(f.overlay["kind"], return_counts=True) if f.overlay.size \
            else (np.zeros(0, int), np.zeros(0, int))
        rows.append({
            "tick": int(f.tick), "t_s": int(f.t_s),
            "burned": int((f.phase == 3).sum()), "burning": int((f.phase == 2).sum()),
            "bbox": bbox,
            "spots": int(f.spots.size), "spots_ignited": int(f.spots["ignited"].sum()),
            "overlay": {DELTA_KINDS.get(int(k), str(int(k))): int(c)
                        for k, c in zip(kinds, counts, strict=True)},
            "state_hash": int(f.state_hash),
        })
    return rows


def format_probe(rows: list[dict[str, Any]]) -> str:
    head = f"{'tick':>6} {'t_s':>8} {'burned':>7} {'burning':>7} {'bbox (x0,y0,x1,y1)':>22} " \
           f"{'spots':>5} {'ign':>4}  overlay"
    out = [head, "-" * len(head)]
    for r in rows:
        bbox = "-" if r["bbox"] is None else ",".join(str(v) for v in r["bbox"])
        ov = " ".join(f"{k}={v}" for k, v in r["overlay"].items()) or "-"
        out.append(f"{r['tick']:>6} {r['t_s']:>8} {r['burned']:>7} {r['burning']:>7} "
                   f"{bbox:>22} {r['spots']:>5} {r['spots_ignited']:>4}  {ov}")
    return "\n".join(out)
