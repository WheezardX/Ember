"""Response curves (plan G2): per-tick series from a state stream, plots, and a summary table.

`extract_series` walks a stream once and records the observer metrics plus fire *extent*
from the ignition centroid (first burning cells) along +x/-x/+y/-y (metres, grid axes:
+y is south) and the max radius. `plot_curves` uses matplotlib (Agg, deterministic bytes);
matplotlib is NOT in the frame-render path.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

import numpy as np

from ember.sim.stream import iter_frames

SERIES_KEYS = ("tick", "t_s", "burned_cells", "burning_cells", "burned_ha",
               "containment_permyriad", "cost_cents", "wind_u_cms", "wind_v_cms", "m10",
               "extent_px_m", "extent_nx_m", "extent_py_m", "extent_ny_m", "max_radius_m")
DEFAULT_METRICS = ("burned_ha", "max_radius_m", "containment_pct", "burning_cells")
_LABELS = {"burned_ha": "burned (ha)", "max_radius_m": "max radius from ignition (m)",
           "containment_pct": "containment (%)", "burning_cells": "burning cells",
           "burned_cells": "burned cells", "cost_cents": "cost (cents)",
           "extent_px_m": "extent +x (m)", "extent_nx_m": "extent -x (m)",
           "extent_py_m": "extent +y/south (m)", "extent_ny_m": "extent -y/north (m)",
           "m10": "dead fuel moisture (0.1 %)", "wind_u_cms": "wind u (cm/s)",
           "wind_v_cms": "wind v (cm/s)"}


def extract_series(stream_path: str | Path, *, every: int = 1) -> dict[str, Any]:
    """Per-tick series (lists) + `ignition` (cx, cy), `cell_m`, `nx`, `ny`, `dt_s`."""
    out: dict[str, Any] = {k: [] for k in SERIES_KEYS}
    ign: tuple[float, float] | None = None
    cell_m = nx = ny = dt_s = None
    for header, f in iter_frames(stream_path, every=every):
        if cell_m is None:
            cell_m, nx, ny, dt_s = header.cell_mm / 1000.0, header.nx, header.ny, header.dt_s
        fire = f.phase >= 2
        ys, xs = np.nonzero(fire)
        if ign is None:
            burning_ys, burning_xs = np.nonzero(f.phase == 2)
            if burning_xs.size:
                ign = (float(burning_xs.mean()), float(burning_ys.mean()))
            elif xs.size:
                ign = (float(xs.mean()), float(ys.mean()))
        if ign is not None and xs.size:
            dx, dy = (xs - ign[0]) * cell_m, (ys - ign[1]) * cell_m
            px, nxm = float(max(dx.max(), 0.0)), float(max(-dx.min(), 0.0))
            py, nym = float(max(dy.max(), 0.0)), float(max(-dy.min(), 0.0))
            rad = float(np.hypot(dx, dy).max())
        else:
            px = nxm = py = nym = rad = 0.0
        m = f.metrics
        burned = int((f.phase == 3).sum())
        burning = int((f.phase == 2).sum())
        row = {
            "tick": f.tick, "t_s": f.t_s, "burned_cells": burned, "burning_cells": burning,
            "burned_ha": (burned + burning) * cell_m * cell_m / 10000.0,
            "containment_permyriad": m.get("containment_permyriad", 0),
            "cost_cents": m.get("cost_cents", 0), "wind_u_cms": m.get("wind_u_cms", 0),
            "wind_v_cms": m.get("wind_v_cms", 0), "m10": m.get("m10", 0),
            "extent_px_m": px, "extent_nx_m": nxm, "extent_py_m": py, "extent_ny_m": nym,
            "max_radius_m": rad,
        }
        for k in SERIES_KEYS:
            out[k].append(row[k])
    out["ignition"] = ign
    out["cell_m"], out["nx"], out["ny"], out["dt_s"] = cell_m, nx, ny, dt_s
    return out


def _ensure_conda_dlls() -> None:
    """numpy's BLAS/LAPACK is delay-loaded from the conda env's Library/bin; when the env's
    python.exe runs without `conda activate` (no PATH setup) the first linalg call dies with
    a Windows delay-load fault (0xc06d007f). matplotlib tick layout calls `inv`, so add the
    directory explicitly. Harmless elsewhere."""
    import os
    import sys

    if sys.platform != "win32":
        return
    d = Path(sys.prefix) / "Library" / "bin"
    if d.is_dir():
        os.add_dll_directory(str(d))
        if str(d) not in os.environ.get("PATH", ""):
            os.environ["PATH"] = str(d) + os.pathsep + os.environ.get("PATH", "")


def _metric(series: dict[str, Any], metric: str) -> np.ndarray:
    if metric == "containment_pct":
        return np.asarray(series["containment_permyriad"], float) / 100.0
    return np.asarray(series[metric], float)


def plot_curves(series_by_label: dict[str, dict[str, Any]], out_png: str | Path, *,
                metrics: list[str] | tuple[str, ...] = DEFAULT_METRICS,
                title: str | None = None, dpi: int = 100) -> Path:
    """Small-multiples plot, one axis per metric, one line per label. Deterministic PNG."""
    _ensure_conda_dlls()
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    metrics = list(metrics)
    ncol = 2 if len(metrics) > 1 else 1
    nrow = (len(metrics) + ncol - 1) // ncol
    fig, axes = plt.subplots(nrow, ncol, figsize=(5.2 * ncol, 3.2 * nrow), squeeze=False)
    for ax, metric in zip(axes.ravel(), metrics, strict=False):
        for label, s in series_by_label.items():
            t_h = np.asarray(s["t_s"], float) / 3600.0
            ax.plot(t_h, _metric(s, metric), label=label, linewidth=1.4)
        ax.set_xlabel("hours since t0")
        ax.set_ylabel(_LABELS.get(metric, metric))
        ax.grid(True, alpha=0.3)
    for ax in axes.ravel()[len(metrics):]:
        ax.axis("off")
    axes.ravel()[0].legend(fontsize=8)
    if title:
        fig.suptitle(title)
    fig.tight_layout()
    out_png = Path(out_png)
    out_png.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_png, dpi=dpi, metadata={"Software": None, "Creation Time": None})
    plt.close(fig)
    return out_png


def summarize(series: dict[str, Any]) -> dict[str, float]:
    """Final burned ha, max extent (m), mean head rate (m/h) since ignition."""
    if not series["t_s"]:
        return {"final_burned_ha": 0.0, "max_extent_m": 0.0, "mean_head_rate_m_per_h": 0.0,
                "duration_h": 0.0}
    t = np.asarray(series["t_s"], float)
    rad = np.asarray(series["max_radius_m"], float)
    fire_t = t[np.asarray(series["burned_cells"]) + np.asarray(series["burning_cells"]) > 0]
    t_ign = float(fire_t.min()) if fire_t.size else float(t[0])
    dur_h = max((float(t[-1]) - t_ign) / 3600.0, 0.0)
    ext = float(max(series["extent_px_m"][-1], series["extent_nx_m"][-1],
                    series["extent_py_m"][-1], series["extent_ny_m"][-1]))
    return {"final_burned_ha": float(series["burned_ha"][-1]), "max_extent_m": ext,
            "max_radius_m": float(rad[-1]),
            "mean_head_rate_m_per_h": float(rad[-1] / dur_h) if dur_h > 0 else 0.0,
            "duration_h": dur_h}


def response_table(series_by_label: dict[str, dict[str, Any]]) -> str:
    lines = ["| run | final burned (ha) | max extent (m) | max radius (m) | "
             "mean head rate (m/h) | duration (h) |",
             "|---|---|---|---|---|---|"]
    for label, s in series_by_label.items():
        r = summarize(s)
        lines.append(f"| {label} | {r['final_burned_ha']:,.1f} | {r['max_extent_m']:,.0f} | "
                     f"{r['max_radius_m']:,.0f} | {r['mean_head_rate_m_per_h']:,.0f} | "
                     f"{r['duration_h']:.2f} |")
    return "\n".join(lines) + "\n"
