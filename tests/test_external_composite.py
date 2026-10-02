"""External Sources X1.3: composite arrival on a pack grid (synthetic pack + timeline)."""
import json

import numpy as np
import pytest

from ember.external.composite import composite, write_pack
from ember.external.timeline import FireTimeline, Grid
from ember.sim.worldpack import load_worldpack

T0 = "2026-08-20T00:00:00Z"


def _pack(tmp_path, obs: np.ndarray):
    d = tmp_path / "base.ewp"
    d.mkdir()
    ny, nx = obs.shape
    obs.astype("<i4").tofile(d / "arrival_s.bin")
    np.full(obs.shape, 2, np.uint8).tofile(d / "confidence.bin")
    np.full(obs.shape, 102, np.uint8).tofile(d / "fbfm40.bin")
    np.zeros(obs.shape, "<i4").tofile(d / "elevation_cm.bin")
    layers = {
        "elevation_cm": {"file": "elevation_cm.bin", "dtype": "i32", "unit": "cm", "stats": {}},
        "fbfm40": {"file": "fbfm40.bin", "dtype": "u8", "unit": "code", "stats": {}},
        "arrival_s": {"file": "arrival_s.bin", "dtype": "i32", "unit": "s", "stats": {}},
        "confidence": {"file": "confidence.bin", "dtype": "u8", "unit": "class", "stats": {}},
    }
    m = {"format": "ember-world-pack", "version": 1, "name": "base",
         "grid": {"nx": nx, "ny": ny, "cell_size_m": 30.0, "crs": "EPSG:32610",
                  "origin_x": 1006.0, "origin_y": 2000.0},      # 6 m east of the source grid
         "t0_utc": T0, "t0_unix": 0, "layers": layers, "arrival": {"algorithm": "obs"},
         "weather": None, "pack_hash": ""}
    (d / "world.json").write_text(json.dumps(m))
    return d


def _timeline(ny, nx):
    arr = np.full((ny, nx), np.nan)
    arr[0, 2] = 1800.0           # forecast new growth, 30 min after the run
    arr[0, 0] = 600.0            # forecast growth on a cell already observed before the run
    before = np.zeros((ny, nx), bool)
    before[1, 1] = True          # start perimeter, not observed burned (a seam cell)
    before[1, 0] = True          # start perimeter, observed burned
    return FireTimeline(Grid(nx, ny, 30.0, 1000.0, 2000.0, "EPSG:32610"), "2026-08-20T10:00:00Z",
                        arr, before, np.ones((ny, nx), bool), {"source": "synthetic"})


def test_composite_rules(tmp_path):
    R = 36000
    obs = np.full((2, 3), -1, np.int64)
    obs[0, 0] = 100                # observed before the run
    obs[1, 0] = 200                # observed before, inside the start perimeter
    obs[0, 1] = R + 5000           # observed AFTER the run: dropped
    c = composite(_timeline(2, 3), _pack(tmp_path, obs))
    a = c["arrival_s"]
    assert c["t_ref_s"] == R
    assert a[0, 0] == 100 and a[1, 0] == 200          # observed kept
    assert a[0, 1] == -1                               # observed future dropped
    assert a[0, 2] == R + 1800                         # forecast after the run
    assert a[1, 1] == -1                               # seam: left unburned (no source time)
    s = c["seams"]
    assert s["source_start_not_observed"] == 1
    assert s["forecast_growth_already_observed"] == 1
    assert s["observed_after_t_ref_dropped"] == 1
    assert c["shift"]["dx_m"] == pytest.approx(6.0)
    assert c["confidence"][0, 2] == 0 and c["confidence"][0, 0] == 2


def test_write_pack_loads(tmp_path):
    obs = np.full((2, 3), -1, np.int64)
    obs[0, 0] = 100
    p = write_pack(_timeline(2, 3), _pack(tmp_path, obs), tmp_path, "variant")
    wp = load_worldpack(p)
    assert wp.manifest["arrival"]["algorithm"] == "external-composite-v1"
    assert wp.manifest["layers"]["arrival_s"]["stats"]["valid"] == 2
