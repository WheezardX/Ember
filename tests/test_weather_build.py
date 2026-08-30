"""C4 — weather timeline assembly (offline via mocked HRRR + RAWS)."""

from datetime import UTC, datetime, timedelta

import pytest


def _fake_hrrr(t0):
    import numpy as np

    from ember.weather.hrrr import HrrrTimeline
    from ember.weather.schema import GridSpec, StepProvenance

    grid = GridSpec(crs="EPSG:32610", bbox=[0, 0, 3000, 3000], nx=3, ny=3, dx_m=1000, dy_m=1000)
    times = [t0, t0 + timedelta(hours=1)]
    steps = [StepProvenance(index=i, valid_time=t, gridded_source="hrrr:anl")
             for i, t in enumerate(times)]
    fields = {v: np.full((2, 3, 3), fill, dtype="float32") for v, fill in
              (("wind10_u", 0.0), ("wind10_v", 0.0), ("t2", 290.0), ("rh2", 40.0))}
    return HrrrTimeline(grid=grid, times=times, steps=steps, fields=fields,
                        variables=["wind10_u", "wind10_v", "t2", "rh2"], gaps=[])


def _fake_raws(t0):
    from ember.weather.synoptic import RawsStation, StationSample

    return [RawsStation(
        stid="SWAUK", name="SWAUK", lon=-120.67, lat=47.25, elevation_m=3480.0, network="2",
        samples=[StationSample(time=t0, air_temp_k=290.0, rh=40.0, wind_speed=1.0,
                               wind_dir=90.0, wind_u=-1.0, wind_v=0.0, fuel_moisture=7.7)],
    )]


@pytest.mark.geo
def test_build_weather_grid_and_stations(tmp_path, monkeypatch):
    import ember.weather.build as b
    from ember.weather.schema import WeatherTimeline

    t0 = datetime(2026, 8, 25, 15, tzinfo=UTC)
    monkeypatch.setattr(b, "build_hrrr_timeline", lambda *a, **k: _fake_hrrr(t0))
    monkeypatch.setattr(b, "fetch_raws", lambda *a, **k: _fake_raws(t0))

    mani = b.build_weather_timeline("hist:x-2026", [-121, 47, -120, 48], t0, 2, 60,
                                    weather_dir=tmp_path / "weather", save_dir=tmp_path / "raw")
    assert mani is not None and mani.grid is not None and len(mani.stations) == 1
    assert mani.grid_data == "weather/grid.timeline.v0.npz"
    assert mani.station_data == "weather/stations.timeline.v0.parquet"
    for f in ("grid.timeline.v0.npz", "stations.timeline.v0.parquet", "timeline.v0.json"):
        assert (tmp_path / "weather" / f).exists()
    assert WeatherTimeline.model_validate_json(mani.model_dump_json()).num_steps == 2

    # the parquet round-trips with the expected columns
    import pandas as pd

    df = pd.read_parquet(tmp_path / "weather" / "stations.timeline.v0.parquet")
    assert {"station_id", "time", "air_temp_k", "wind10_u", "fuel_moisture"} <= set(df.columns)


@pytest.mark.geo
def test_build_weather_station_only(tmp_path, monkeypatch):
    import ember.weather.build as b

    t0 = datetime(2026, 8, 25, 15, tzinfo=UTC)
    monkeypatch.setattr(b, "fetch_raws", lambda *a, **k: _fake_raws(t0))
    mani = b.build_weather_timeline("hist:x-2026", [-121, 47, -120, 48], t0, 2, 60,
                                    weather_dir=tmp_path / "w", save_dir=tmp_path / "raw",
                                    gridded=False)
    assert mani is not None and mani.grid is None and len(mani.stations) == 1
    assert mani.steps == []  # no gridded steps


@pytest.mark.geo
def test_build_weather_none_when_empty(tmp_path, monkeypatch):
    import ember.weather.build as b

    t0 = datetime(2026, 8, 25, 15, tzinfo=UTC)
    monkeypatch.setattr(b, "fetch_raws", lambda *a, **k: [])
    assert b.build_weather_timeline("hist:x-2026", [-121, 47, -120, 48], t0, 2, 60,
                                    weather_dir=tmp_path / "w", save_dir=tmp_path / "raw",
                                    gridded=False) is None
