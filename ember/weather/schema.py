"""C1 — weather timeline schema v0 (the mini-ADR's machine form; see adr/0007).

A weather timeline is what future Epic 4.1 (fire behavior) consumes: a regular time
axis over the incident window, plus two aligned representations of the atmosphere —

  * a low-res GRIDDED field over the incident AOI (from HRRR, C2), SI units, and
  * point STATION series (from RAWS/Synoptic, C3), for ground truth / bias checks.

This module defines only the manifest/metadata contract and validation. The bulk
numeric payloads (grid arrays, station tables) live in sidecar files the manifest
points at (parquet/npz), written by C2–C4 once the array stack is provisioned; the
manifest is self-describing so a consumer knows shapes, units, and provenance without
opening them. Everything is versioned and deliberately minimal — v0 is a starting
point Epic 4.1 is expected to renegotiate.
"""

from __future__ import annotations

from datetime import datetime
from typing import Literal

from pydantic import BaseModel, Field, model_validator

SCHEMA_VERSION = 0
TIMELINE_FORMAT = "weather-timeline-v0"

# SI-ish canonical units. Wind as u/v components (m/s) avoids direction-averaging
# pitfalls; temperature in kelvin (SI); RH percent; precip mm accumulated per step.
WeatherVariable = Literal["wind10_u", "wind10_v", "t2", "rh2", "precip"]

_UNITS: dict[str, str] = {
    "wind10_u": "m/s", "wind10_v": "m/s", "t2": "K", "rh2": "%", "precip": "mm/step",
}


class GridSpec(BaseModel):
    """Low-res gridded field footprint over the incident AOI.

    Aligned to the incident AOI in a metric CRS (Epic 1 rules). Fields are cell-centered
    (no Arakawa staggering at v0 — documented so C2 resamples to centers on ingest).
    """

    crs: str  # e.g. "EPSG:32610"
    bbox: list[float] = Field(min_length=4, max_length=4)  # [minx, miny, maxx, maxy] in crs
    nx: int = Field(gt=0)
    ny: int = Field(gt=0)
    dx_m: float = Field(gt=0)
    dy_m: float = Field(gt=0)
    staggering: Literal["cell-center"] = "cell-center"


class StationSeries(BaseModel):
    """One point station's metadata (the observation table lives in the sidecar file)."""

    station_id: str
    name: str | None = None
    lon: float
    lat: float
    elevation_m: float | None = None
    provider: str = "synoptic"


class StepProvenance(BaseModel):
    """Which source produced the gridded field for one time step (D4: record which)."""

    index: int = Field(ge=0)
    valid_time: datetime
    # e.g. "hrrr:anl" (analysis F00, historic truth) or "hrrr:f01" (1-h forecast, live)
    gridded_source: str
    note: str | None = None


class WeatherTimeline(BaseModel):
    """The manifest tying the time axis, grid, stations, provenance, and sidecars together."""

    schema_version: int = SCHEMA_VERSION
    format: Literal["weather-timeline-v0"] = TIMELINE_FORMAT
    incident_id: str
    t0: datetime  # first step, UTC
    step_minutes: int = Field(gt=0)
    num_steps: int = Field(gt=0)
    variables: list[WeatherVariable]
    units: dict[str, str] = Field(default_factory=dict)  # var -> unit, filled on validate
    grid: GridSpec | None = None  # absent if no gridded source available
    stations: list[StationSeries] = Field(default_factory=list)
    steps: list[StepProvenance] = Field(default_factory=list)
    # store-relative sidecar paths holding the numeric payload (written by C2–C4)
    grid_data: str | None = None      # e.g. "weather/grid.timeline.v0.npz"
    station_data: str | None = None   # e.g. "weather/stations.timeline.v0.parquet"
    gaps: list[str] = Field(default_factory=list)  # human-readable gap notes (never silent)
    notes: str | None = None

    @model_validator(mode="after")
    def _fill_and_check(self) -> WeatherTimeline:
        self.units = {v: _UNITS[v] for v in self.variables}
        if self.steps and len(self.steps) != self.num_steps:
            raise ValueError(
                f"steps provenance ({len(self.steps)}) must match num_steps ({self.num_steps})"
            )
        if self.grid is None and not self.stations:
            raise ValueError("a timeline needs at least a grid or one station")
        return self
