"""Observed heat for a playback: the NIROPS heat classes of every IR flight on the replay's grid.

A perimeter playback knows only when each cell burned. The nightly IR says where it was still
hot: intense heat (the active front), scattered heat (interior burning, often most of the scar for
weeks), isolated heat points. The client (EmberFireActor) reads this to drive interior smoke
columns, smoulder and ground glow from the record instead of a fixed decay.

    python -m ember.incidents.ir_heat --obs <incident>/observations/ir_perimeters --replay <run>.replay.json

Writes beside the world pack (`<pack>.heat.json` + `.heat.bin`): one uint8 grid per flight, row 0
north, 0 none / 1 isolated / 2 scattered / 3 intense. ~0.36 MB per flight on a 620 x 576 grid.
"""
import argparse, json
from datetime import datetime
from pathlib import Path

import numpy as np
from pyproj import Transformer
from rasterio.features import rasterize
from rasterio.transform import from_origin
from shapely.geometry import shape
from shapely.ops import transform as shp_transform

CLASSES = {"Isolated Heat point": 1, "Scattered Heat": 2, "Intense Heat": 3}


def build(obs: Path, replay: Path) -> Path:
    r = json.loads(replay.read_text(encoding="utf-8"))
    g = r["world"]["grid"]
    nx, ny, cell = g["nx"], g["ny"], g["cell_size_m"]
    tf = from_origin(g["origin_x"], g["origin_y"], cell, cell)
    to_grid = Transformer.from_crs("EPSG:4326", g["crs"], always_xy=True)
    pack = (replay.parent / r["world"]["pack_relative"]).resolve()
    flights, grids = [], []
    for f in sorted(obs.glob("*.geojson")):
        fc = json.loads(f.read_text(encoding="utf-8"))
        a = np.zeros((ny, nx), np.uint8)
        counts = {}
        # lowest class first so intense wins where they overlap
        for name, cls in sorted(CLASSES.items(), key=lambda kv: kv[1]):
            geoms = [shp_transform(lambda x, y, z=None: to_grid.transform(x, y), shape(ft["geometry"]))
                     for ft in fc["features"] if ft["properties"]["class"] == name]
            if not geoms:
                continue
            m = rasterize(((gm, 1) for gm in geoms), out_shape=(ny, nx), transform=tf, fill=0,
                          all_touched=(cls == 1), dtype="uint8")
            a[m > 0] = cls
            counts[name] = int((m > 0).sum())
        at = fc["features"][0]["properties"]["acquired_utc"]
        flights.append({"acquired_utc": at, "unix": int(datetime.fromisoformat(at).timestamp()),
                        "cells": counts})
        grids.append(a)
    order = np.argsort([fl["unix"] for fl in flights])
    flights = [flights[i] for i in order]
    out = pack.with_suffix(".heat.bin")
    np.stack([grids[i] for i in order]).tofile(out)
    meta = {"format": "ember-observed-heat", "version": 1, "nx": nx, "ny": ny, "cell_size_m": cell,
            "origin_x": g["origin_x"], "origin_y": g["origin_y"], "crs": g["crs"],
            "classes": {"1": "isolated", "2": "scattered", "3": "intense"},
            "source": str(obs), "bin": out.name, "flights": flights}
    pack.with_suffix(".heat.json").write_text(json.dumps(meta, indent=1), encoding="utf-8")
    return out


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--obs", type=Path, required=True)
    ap.add_argument("--replay", type=Path, required=True)
    a = ap.parse_args()
    out = build(a.obs, a.replay)
    print(out, out.stat().st_size)
