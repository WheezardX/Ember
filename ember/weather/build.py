"""C4 — weather timeline assembly.

Merges the HRRR gridded fields (C2, [[hrrr.py]]) and the RAWS station series (C3,
[[synoptic.py]]) into one versioned weather timeline: a manifest (C1 schema) plus two
sidecars — a compressed ``.npz`` for the grid and a ``.parquet`` for the station obs.
Gaps (missing HRRR hours, no stations in AOI) are recorded explicitly, never smoothed
over. Either half may be absent; a timeline needs at least one.
"""

from __future__ import annotations

from datetime import datetime, timedelta
from pathlib import Path

from terrain.util.logging import get_logger

from ember.weather.hrrr import build_hrrr_timeline, write_grid_sidecar
from ember.weather.schema import StationSeries, WeatherTimeline
from ember.weather.synoptic import RawsStation, fetch_raws

log = get_logger(__name__)

GRID_SIDECAR = "grid.timeline.v0.npz"
STATION_SIDECAR = "stations.timeline.v0.parquet"


def _write_stations_parquet(stations: list[RawsStation], path: Path) -> None:
    import pandas as pd

    rows = []
    for st in stations:
        for s in st.samples:
            rows.append({
                "station_id": st.stid, "time": s.time,
                "air_temp_k": s.air_temp_k, "rh2": s.rh,
                "wind_speed": s.wind_speed, "wind_dir": s.wind_dir,
                "wind10_u": s.wind_u, "wind10_v": s.wind_v,
                "fuel_moisture": s.fuel_moisture,
            })
    path.parent.mkdir(parents=True, exist_ok=True)
    pd.DataFrame(rows).to_parquet(path, index=False)  # needs pyarrow


def build_weather_timeline(
    incident_id: str, bbox: list[float], t0: datetime, num_steps: int, step_minutes: int, *,
    weather_dir: str | Path, save_dir: str | Path, gridded: bool = True, stations: bool = True,
    grid_res_m: float = 3000.0, store_prefix: str = "weather",
) -> WeatherTimeline | None:
    """Assemble a weather timeline for the window; write sidecars + manifest. None if empty.

    Sidecar paths in the manifest are store-relative (``<store_prefix>/...``); the actual
    files are written under ``weather_dir``.
    """
    weather_dir = Path(weather_dir)
    weather_dir.mkdir(parents=True, exist_ok=True)
    end = t0 + timedelta(minutes=step_minutes * (num_steps - 1))

    grid = None
    steps: list = []
    variables: list[str] = []
    gaps: list[str] = []
    grid_data = station_data = None
    station_series: list[StationSeries] = []

    if gridded:
        tl = build_hrrr_timeline(bbox, t0, num_steps, step_minutes,
                                 save_dir=Path(save_dir) / "hrrr", grid_res_m=grid_res_m)
        write_grid_sidecar(tl, weather_dir / GRID_SIDECAR)
        grid, steps, variables = tl.grid, tl.steps, list(tl.variables)
        gaps += tl.gaps
        grid_data = f"{store_prefix}/{GRID_SIDECAR}"

    if stations:
        try:
            raws = fetch_raws(bbox, t0, end)
        except Exception as ex:  # noqa: BLE001 — station side is best-effort
            log.warning("synoptic RAWS fetch skipped: %s", ex)
            raws = []
            gaps.append(f"RAWS unavailable: {str(ex)[:80]}")
        if raws:
            _write_stations_parquet(raws, weather_dir / STATION_SIDECAR)
            station_data = f"{store_prefix}/{STATION_SIDECAR}"
            station_series = [
                StationSeries(station_id=s.stid, name=s.name or None, lon=s.lon, lat=s.lat,
                              elevation_m=s.elevation_m, provider="synoptic")
                for s in raws
            ]
            if not variables:  # station-only timeline: name the vars stations carry
                variables = ["wind10_u", "wind10_v", "t2", "rh2"]
        else:
            gaps.append("no RAWS stations returned for AOI/window")

    if grid is None and not station_series:
        log.warning("weather: neither HRRR grid nor RAWS stations available — no timeline")
        return None

    manifest = WeatherTimeline(
        incident_id=incident_id, t0=t0, step_minutes=step_minutes, num_steps=num_steps,
        variables=variables or ["t2"], grid=grid, stations=station_series,
        steps=steps if grid is not None else [],
        grid_data=grid_data, station_data=station_data, gaps=gaps,
    )
    (weather_dir / "timeline.v0.json").write_text(
        manifest.model_dump_json(indent=2), encoding="utf-8")
    log.info("weather timeline: grid=%s stations=%d steps=%d gaps=%d",
             grid is not None, len(station_series), num_steps, len(gaps))
    return manifest
