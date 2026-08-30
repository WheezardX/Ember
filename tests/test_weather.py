"""C1 — weather timeline schema v0 (adr/0007)."""

from datetime import UTC, datetime

import pytest


def _grid():
    from ember.weather import GridSpec

    return GridSpec(crs="EPSG:32610", bbox=[600000, 5200000, 620000, 5220000],
                    nx=20, ny=20, dx_m=1000.0, dy_m=1000.0)


def test_timeline_validates_and_fills_units():
    from ember.weather import StepProvenance, WeatherTimeline

    t0 = datetime(2017, 8, 12, tzinfo=UTC)
    tl = WeatherTimeline(
        incident_id="hist:jolly-mountain-2017", t0=t0, step_minutes=60, num_steps=2,
        variables=["wind10_u", "wind10_v", "t2"], grid=_grid(),
        steps=[
            StepProvenance(index=0, valid_time=t0, gridded_source="hrrr:anl"),
            StepProvenance(index=1, valid_time=t0, gridded_source="hrrr:anl"),
        ],
    )
    assert tl.format == "weather-timeline-v0" and tl.schema_version == 0
    assert tl.units == {"wind10_u": "m/s", "wind10_v": "m/s", "t2": "K"}
    # round-trips
    from ember.weather import WeatherTimeline as WT

    assert WT.model_validate_json(tl.model_dump_json()).num_steps == 2


def test_timeline_requires_grid_or_station():
    from ember.weather import WeatherTimeline

    with pytest.raises(ValueError, match="grid or one station"):
        WeatherTimeline(incident_id="x", t0=datetime(2020, 1, 1, tzinfo=UTC),
                        step_minutes=60, num_steps=1, variables=["t2"])


def test_step_count_must_match():
    from ember.weather import StepProvenance, WeatherTimeline

    t0 = datetime(2020, 1, 1, tzinfo=UTC)
    with pytest.raises(ValueError, match="must match num_steps"):
        WeatherTimeline(
            incident_id="x", t0=t0, step_minutes=60, num_steps=3, variables=["t2"],
            grid=_grid(),
            steps=[StepProvenance(index=0, valid_time=t0, gridded_source="hrrr:anl")],
        )


def test_station_only_timeline_ok():
    from ember.weather import StationSeries, WeatherTimeline

    tl = WeatherTimeline(
        incident_id="x", t0=datetime(2020, 1, 1, tzinfo=UTC), step_minutes=60,
        num_steps=1, variables=["t2", "rh2"],
        stations=[StationSeries(station_id="TR266", lon=-120.9, lat=47.2, provider="synoptic")],
    )
    assert tl.grid is None and len(tl.stations) == 1
