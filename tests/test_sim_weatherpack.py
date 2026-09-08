"""Weather pack v1: quantisation, north-up row flip, gap hold (ADR 0009 B), real sidecar."""

import json
from datetime import UTC, datetime, timedelta
from pathlib import Path

import numpy as np
import pytest

from ember.sim.weatherpack import VARIABLES, export_weather, quantise
from ember.weather.schema import GridSpec, StepProvenance, WeatherTimeline


def _timeline(tmp_path: Path, *, nsteps=3, missing=(), variables=("wind10_u", "wind10_v",
                                                                    "t2", "rh2")):
    ny, nx = 3, 2
    t0 = datetime(2026, 8, 1, 12, tzinfo=UTC)
    fields = {}
    for v in variables:
        a = np.zeros((nsteps, ny, nx), np.float32)
        for s in range(nsteps):
            # row r (south->north in the sidecar) carries value 10*r + s; distinct per var
            a[s] = (np.arange(ny)[:, None] * 10 + s + {"wind10_u": 0, "wind10_v": 100,
                                                       "t2": 200, "rh2": 300, "precip": 400}[v])
            if s in missing:
                a[s] = np.nan
        fields[v] = a
    wdir = tmp_path / "weather"
    wdir.mkdir()
    np.savez_compressed(wdir / "grid.npz", times=np.array([""] * nsteps),
                        variables=np.array(list(variables)), **fields)
    steps = [StepProvenance(index=i, valid_time=t0 + timedelta(hours=i),
                            gridded_source="missing" if i in missing else "hrrr:anl")
             for i in range(nsteps)]
    tl = WeatherTimeline(
        incident_id="hist:test-2026", t0=t0, step_minutes=60, num_steps=nsteps,
        variables=list(variables),
        grid=GridSpec(crs="EPSG:32610", bbox=[1000.0, 5000.0, 7000.0, 14000.0], nx=nx, ny=ny,
                      dx_m=3000.0, dy_m=3000.0),
        steps=steps, grid_data="weather/grid.npz", gaps=["x"] if missing else [])
    p = wdir / "timeline.v0.json"
    p.write_text(tl.model_dump_json(indent=2), encoding="utf-8")
    return p


def test_quantise():
    assert quantise("wind10_u", np.array([1.234, -0.5, np.nan])).tolist() == [123, -50, 0]
    assert quantise("t2", np.array([298.15])).tolist() == [2982]
    assert quantise("rh2", np.array([33.33])).tolist() == [333]
    assert quantise("precip", np.array([2.5, 1e9])).tolist() == [250, 32767]


def test_export_flips_rows_and_fills_missing_vars(tmp_path):
    tl = _timeline(tmp_path)
    out = tmp_path / "pack"
    assert export_weather(tl, tmp_path, out, {"crs": "EPSG:32610"}) == "weather.json"
    m = json.loads((out / "weather.json").read_text())
    assert m["format"] == "ember-weather-pack" and m["variables"] == VARIABLES
    assert m["grid"] == {"nx": 2, "ny": 3, "dx_m": 3000.0, "dy_m": 3000.0,
                         "origin_x": 1000.0, "origin_y": 14000.0, "crs": "EPSG:32610"}
    assert m["step_s"] == 3600 and m["num_steps"] == 3 and m["held_steps"] == 0
    assert m["missing_variables"] == ["precip"] and m["precip_present"] is False
    data = np.frombuffer((out / "weather.bin").read_bytes(), "<i2").reshape(3, 5, 3, 2)
    # sidecar row 2 (north, value 20+s) must land in pack row 0; wind in cm/s (x100)
    assert data[0, 0, 0, 0] == 2000 and data[0, 0, 2, 0] == 0
    assert data[1, 0, 0, 0] == 2100
    assert data[0, 2, 0, 0] == (220) * 10          # t2 x10
    assert np.all(data[:, 4] == 0)                 # precip zeros


def test_gap_is_held_and_flagged(tmp_path):
    tl = _timeline(tmp_path, nsteps=4, missing=(1, 2))
    out = tmp_path / "pack"
    export_weather(tl, tmp_path, out)
    m = json.loads((out / "weather.json").read_text())
    assert m["held_steps"] == 2
    assert [s["held_from"] for s in m["steps"]] == [None, 0, 0, None]
    assert [s["source"] for s in m["steps"]] == ["hrrr:anl", "missing", "missing", "hrrr:anl"]
    data = np.frombuffer((out / "weather.bin").read_bytes(), "<i2").reshape(4, 5, 3, 2)
    assert np.array_equal(data[1], data[0]) and np.array_equal(data[2], data[0])
    assert not np.array_equal(data[3], data[0])


def test_leading_gap_refused(tmp_path):
    tl = _timeline(tmp_path, nsteps=3, missing=(0,))
    with pytest.raises(ValueError, match="starts with a missing step"):
        export_weather(tl, tmp_path, tmp_path / "pack")


def test_crs_mismatch_refused(tmp_path):
    tl = _timeline(tmp_path)
    with pytest.raises(ValueError, match="CRS"):
        export_weather(tl, tmp_path, tmp_path / "pack", {"crs": "EPSG:32611"})


_REAL = Path("store/incidents/78d35d3b-f791-4961-ae36-c6d1a4dff5a0/weather/timeline.v0.json")


@pytest.mark.skipif(not _REAL.exists(), reason="real HRRR sidecar not in store")
def test_real_hrrr_sidecar(tmp_path):
    export_weather(_REAL, _REAL.parent.parent, tmp_path)
    m = json.loads((tmp_path / "weather.json").read_text())
    assert m["num_steps"] == 6 and m["grid"]["nx"] == 15 and m["grid"]["ny"] == 19
    data = np.frombuffer((tmp_path / "weather.bin").read_bytes(), "<i2")
    assert data.size == 6 * 5 * 19 * 15
    t2 = data.reshape(6, 5, 19, 15)[:, 2]
    assert 2600 < t2.min() and t2.max() < 3200  # 0.1 K: plausible surface temps
