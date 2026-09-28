"""Forest statistics for a Terrain region (HCP2 "species distribution stats vs Epic 2 E4
expectations", plus the render-side crown-cover check).

Input is Terrain's own scatter output (`veg/instances.npy`, the conformance oracle the C++ port
reproduces exactly) with the fuels/canopy rasters it was scattered from. Sections:

  * totals: instances, instances/ha over the AOI, instances Terrain placed outside the DEM mask
    (z = 0; the renderer drops them - upstream U8);
  * species: share of each palette key vs its group's integer weights;
  * density: trees per 10 m cell by canopy-cover bin vs the accept rule's expectation
    (candidates_per_cell x CC);
  * heights: per-species distribution, and mean tree height vs CHM by CC bin;
  * crown cover: each instance's crown radius (Terrain scatter v2 sizes crowns: 0.5 x crown_ratio
    x height, drawn as-is by the renderer) turned into a per-cell covered fraction (Poisson
    overlap of crown discs) vs LANDFIRE CC - the calibration check for the palette's stand
    structure and candidates_per_cell.

Writes forest_report.json + forest_report.png.
"""

from __future__ import annotations

import json
import tomllib
from pathlib import Path

import numpy as np

CC_BINS = [(1, 20), (20, 40), (40, 60), (60, 80), (80, 101)]


def _palette(region_dir: Path, ref: str) -> dict:
    """Resolve the palette the scatter used (scatter.input.json `palette`), Terrain-relative."""
    for base in (region_dir, region_dir.parents[1] / "terrain", region_dir.parents[1]):
        p = base / ref
        if p.exists():
            return tomllib.loads(p.read_text(encoding="utf-8"))
    raise FileNotFoundError(f"palette {ref!r} not found near {region_dir}")


def report(region_dir: Path) -> tuple[dict, dict]:
    """Returns (report dict, arrays for plotting)."""
    import rasterio

    inp = json.loads((region_dir / "veg" / "scatter.input.json").read_text(encoding="utf-8"))
    pal = _palette(region_dir, inp["palette"])
    keys, groups = [], []
    for g in pal["groups"]:
        for s in g["species"]:
            keys.append(s["key"])
            groups.append(g["name"])
    inst = np.load(region_dir / "veg" / "instances.npy")
    ras = inp["rasters"]
    with rasterio.open(region_dir / ras["cc"].replace("\\", "/")) as ds:
        cc = ds.read(1).astype(np.float64)
        transform = ds.transform
    with rasterio.open(region_dir / ras["height"].replace("\\", "/")) as ds:
        chm = ds.read(1).astype(np.float64)
    cell = float(inp["cell_size_m"])
    cand = int(inp["candidates_per_cell"])

    rows, cols = rasterio.transform.rowcol(transform, inst["x"], inst["y"])
    r, c = np.asarray(rows), np.asarray(cols)
    ok = (r >= 0) & (r < cc.shape[0]) & (c >= 0) & (c < cc.shape[1])
    r, c = r[ok], c[ok]
    h = inst["height"][ok].astype(np.float64)
    crown = 2.0 * inst["radius"][ok].astype(np.float64)

    n = np.zeros_like(cc)
    np.add.at(n, (r, c), 1.0)
    hsum = np.zeros_like(cc)
    np.add.at(hsum, (r, c), h)
    area = np.zeros_like(cc)
    np.add.at(area, (r, c), np.pi * (crown / 2.0) ** 2)
    cover = 100.0 * (1.0 - np.exp(-area / (cell * cell)))
    valid = cc > 0

    aoi_ha = float((cc >= 0).sum()) * cell * cell / 1e4
    total = int(len(inst))
    species = []
    seen: set[str] = set()
    for i, k in enumerate(keys):
        if k in seen:     # a key in several groups (one mix per forest type): report it once
            continue
        seen.add(k)
        g = groups[i]
        members = [j for j, gg in enumerate(groups) if gg == g]
        w = [s["weight"] for gg in pal["groups"] if gg["name"] == g for s in gg["species"]]
        n_g = int(np.isin(inst["species"], members).sum())
        n_first = int((inst["species"] == i).sum())
        same = [j for j, kk in enumerate(keys) if kk == k]
        sel = np.isin(inst["species"], same)
        n_k = int(sel.sum())
        hk = inst["height"][sel]
        species.append({
            "key": k, "group": g, "count": n_k,
            "share_of_all": round(n_k / max(total, 1), 4),
            "share_of_group": round(n_first / n_g, 4) if n_g else None,   # first group listed
            "expected_share": round(w[members.index(i)] / sum(w), 4),
            "height_m": {"mean": round(float(hk.mean()), 2) if n_k else None,
                         "p10": round(float(np.percentile(hk, 10)), 2) if n_k else None,
                         "p90": round(float(np.percentile(hk, 90)), 2) if n_k else None},
        })
    bins = []
    for lo, hi in CC_BINS:
        m = valid & (cc >= lo) & (cc < hi)
        if not m.any():
            continue
        trees = float(n[m].sum())
        bins.append({
            "cc_pct": [lo, hi - 1], "cells": int(m.sum()),
            "trees_per_cell": round(float(n[m].mean()), 3),
            "expected_trees_per_cell": round(cand * float(cc[m].mean()) / 100.0, 3),
            "trees_per_ha": round(float(n[m].mean()) * 1e4 / (cell * cell), 1),
            "mean_tree_height_m": round(float(hsum[m].sum() / trees), 2) if trees else None,
            "mean_chm_m": (round(float(chm[m & (chm > 0)].mean()), 2)
                           if (m & (chm > 0)).any() else None),
            "landfire_cc_pct": round(float(cc[m].mean()), 1),
            "rendered_crown_cover_pct": round(float(cover[m].mean()), 1),
        })
    bias = float((cover[valid] - cc[valid]).mean())
    rep = {
        "format": "ember-forest-report", "version": 1, "region": region_dir.name,
        "palette": pal.get("name"),
        "instances": total,
        "aoi_ha": round(aoi_ha, 1),
        "instances_per_ha": round(total / aoi_ha, 1),
        "outside_dem_mask": int((inst["z"] == 0).sum()),
        "species": species,
        "density_by_cc": bins,
        "crown_cover": {"source": "instance crown radius (scatter v2)",
                        "rendered_minus_landfire_cc_pts": round(bias, 2)},
    }
    ukeys = list(dict.fromkeys(keys))
    slots = {k: [j for j, kk in enumerate(keys) if kk == k] for k in ukeys}
    arrays = {"keys": ukeys,
              "species_heights": [inst["height"][np.isin(inst["species"], slots[k])]
                                  for k in ukeys],
              "cc": cc[valid], "cover": cover[valid], "n": n[valid]}
    return rep, arrays


def plot(rep: dict, arrays: dict, out_png: Path) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(1, 3, figsize=(16, 4.6), dpi=110)
    b = rep["density_by_cc"]
    x = [f"{d['cc_pct'][0]}-{d['cc_pct'][1]}" for d in b]
    ax[0].bar(x, [d["trees_per_ha"] for d in b], color="#4a6b3a", label="Terrain scatter")
    ax[0].plot(x, [d["expected_trees_per_cell"] * 100 for d in b], "k_", ms=28, mew=2,
               label="expected (candidates x CC per cell)")
    ax[0].set(title="Trees per ha by LANDFIRE canopy cover", xlabel="CC %", ylabel="trees / ha")
    ax[0].legend(fontsize=8)
    top = sorted(range(len(arrays["keys"])), key=lambda i: -len(arrays["species_heights"][i]))
    for i in top[:8]:                                  # the 8 most common species
        k, hs = arrays["keys"][i], arrays["species_heights"][i]
        if len(hs) > 100:
            ax[1].hist(hs, bins=60, range=(0, 75), histtype="step", lw=1.5,
                       label=f"{k} ({len(hs):,})")
    ax[1].set(title="Tree heights (crown class x canopy-top height)", xlabel="m",
              ylabel="instances")
    ax[1].legend(fontsize=7)
    ax[2].plot(x, [d["landfire_cc_pct"] for d in b], "o-", color="#333", label="LANDFIRE CC")
    ax[2].plot(x, [d["rendered_crown_cover_pct"] for d in b], "s-", color="#4a6b3a",
               label="rendered crowns")
    ax[2].set(title="Canopy cover: data vs rendered crowns", xlabel="CC bin %",
              ylabel="% of ground covered", ylim=(0, 100))
    ax[2].legend(fontsize=8)
    fig.suptitle(f"{rep['region']}: {rep['instances']:,} trees, {rep['instances_per_ha']} / ha "
                 f"over {rep['aoi_ha']:,.0f} ha", fontsize=11)
    fig.tight_layout()
    out_png.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_png)
    plt.close(fig)


def write(region_dir: Path, out_dir: Path) -> dict:
    rep, arrays = report(region_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "forest_report.json").write_text(json.dumps(rep, indent=1), encoding="utf-8")
    plot(rep, arrays, out_dir / "forest_report.png")
    return rep
