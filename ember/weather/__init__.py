"""Weather timeline (Epic 3 workstream C).

A versioned, source-agnostic description of the weather a fire experienced/experiences,
authored here so Epic 3 can attach weather to scenario bundles before Epic 4 exists.
The schema (v0) is explicitly renegotiable by Epic 4.1 — see adr/0007. C2 (HRRR) and
C3 (RAWS/Synoptic) fill it in; this package defines the contract they target.
"""

from ember.weather.schema import (
    SCHEMA_VERSION,
    TIMELINE_FORMAT,
    GridSpec,
    StationSeries,
    StepProvenance,
    WeatherTimeline,
    WeatherVariable,
)

__all__ = [
    "SCHEMA_VERSION", "TIMELINE_FORMAT", "GridSpec", "StationSeries",
    "StepProvenance", "WeatherTimeline", "WeatherVariable",
]
