"""A1 — incident model + store layout (Epic 3).

Observations (perimeters, hotspots, IR) are IMMUTABLE, content-addressed truth;
derived products (arrival-time raster) are versioned interpretations of them. Every
incident keys off an IRWIN id; pre-IRWIN / historic fires get a `hist:` id.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Literal

from pydantic import BaseModel, Field

SCHEMA_VERSION = 1
_SLUG = re.compile(r"[^a-z0-9]+")


def hist_id(name: str, year: int) -> str:
    """Synthetic id for a pre-IRWIN / historic fire, e.g. 'hist:jolly-mountain-2017'."""
    slug = _SLUG.sub("-", name.strip().lower()).strip("-")
    return f"hist:{slug}-{year}"


def id_to_dirname(incident_id: str) -> str:
    """Filesystem-safe dir name for an incident id (':' -> '_')."""
    return _SLUG.sub("-", incident_id.replace(":", "_").lower()).strip("-")


class ObservationEnvelope(BaseModel):
    """Provenance wrapper around one immutable observation (a perimeter, hotspot set…)."""

    kind: Literal["perimeter", "hotspots", "ir", "sitrep"]
    source: str  # e.g. "geomac-2017", "wfigs", "firms"
    observed_at: datetime | None = None  # the observation's own timestamp (UTC)
    fetched_at: datetime
    geometry_hash: str  # content hash of the normalized geometry/payload
    path: str  # store-relative path to the stored observation
    attributes: dict = Field(default_factory=dict)


class SizePoint(BaseModel):
    at: datetime
    acres: float


class IncidentRecord(BaseModel):
    """Normalized incident metadata reconciled across sources."""

    schema_version: int = SCHEMA_VERSION
    incident_id: str  # IRWIN id or hist:<slug>-<year>
    name: str
    year: int
    agency: str | None = None
    discovered_at: datetime | None = None
    contained_at: datetime | None = None
    final_acres: float | None = None
    size_series: list[SizePoint] = Field(default_factory=list)
    cross_ids: dict[str, str] = Field(default_factory=dict)  # irwin, geomac, mtbs…


class BundleManifest(BaseModel):
    """Ties the incident record + observations + derived + Epic 1-2 world pin together."""

    schema_version: int = SCHEMA_VERSION
    incident_id: str
    created_utc: datetime
    aoi_geojson: str  # store-relative
    world_region: str | None = None  # the terrain region baked for this AOI
    world_manifest_hash: str | None = None  # pins the terrain world (replay integrity)
    observations: list[ObservationEnvelope] = Field(default_factory=list)
    derived: dict[str, str] = Field(default_factory=dict)  # name -> store-relative path
    weather: str | None = None
    provenance: dict = Field(default_factory=dict)


@dataclass(frozen=True)
class IncidentStore:
    """Paths under store/incidents/<id>/ (mirrors terrain's StoreLayout style)."""

    root: Path
    incident_id: str

    @classmethod
    def create(cls, store_root: str | Path, incident_id: str) -> IncidentStore:
        return cls(root=Path(store_root), incident_id=incident_id)

    @property
    def dir(self) -> Path:
        return self.root / "incidents" / id_to_dirname(self.incident_id)

    @property
    def incident_json(self) -> Path:
        return self.dir / "incident.json"

    @property
    def aoi_geojson(self) -> Path:
        return self.dir / "aoi.geojson"

    @property
    def bundle_json(self) -> Path:
        return self.dir / "package" / "scenario.bundle.json"

    def observations_dir(self, kind: str) -> Path:
        return self.dir / "observations" / kind

    def derived(self, name: str) -> Path:
        return self.dir / "derived" / name

    def ensure_dirs(self) -> None:
        for d in (self.dir, self.dir / "observations", self.dir / "derived", self.dir / "package"):
            d.mkdir(parents=True, exist_ok=True)
