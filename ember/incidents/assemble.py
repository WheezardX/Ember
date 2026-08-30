"""E2 — assemble a historic fire into a scenario bundle.

Wires E1 (fetch perimeters) -> A1 (incident record + immutable observations) ->
D2 (arrival raster) -> the bundle manifest. `ember incident --historic <id>` calls this.
"""

from __future__ import annotations

import json
from datetime import UTC, datetime
from pathlib import Path

from shapely.geometry import mapping
from terrain.util.logging import get_logger

from ember.incidents.arrival import build_arrival_raster, build_incident_grid
from ember.incidents.bake import bake_world_for_aoi
from ember.incidents.geomac import fetch_perimeter_series
from ember.incidents.model import (
    BundleManifest,
    IncidentRecord,
    IncidentStore,
    ObservationEnvelope,
    SizePoint,
    hist_id,
)

log = get_logger(__name__)


def parse_historic_id(slug: str) -> tuple[str, int]:
    """'jolly-mountain-2017' -> ('Jolly Mountain', 2017)."""
    parts = slug.rsplit("-", 1)
    if len(parts) != 2 or not parts[1].isdigit():
        raise ValueError(f"historic id must end in -YYYY, got {slug!r}")
    name = parts[0].replace("-", " ").title()
    return name, int(parts[1])


def _write_geojson(path: Path, geom) -> None:
    fc = {"type": "FeatureCollection",
          "features": [{"type": "Feature", "properties": {}, "geometry": mapping(geom)}]}
    path.write_text(json.dumps(fc), encoding="utf-8")


def assemble_historic(
    slug: str, store_root: str | Path = "store", *,
    buffer_km: float = 3.0, resolution_m: float = 30.0,
    bake_world: bool = True, world_resolution_m: float = 30.0,
) -> BundleManifest:
    """Assemble a historic fire (by slug like 'jolly-mountain-2017') into a bundle.

    When ``bake_world`` is set, the incident AOI is also handed to the terrain engine
    ([[bake.py]]) to bake a coarse DEM + LANDFIRE fuels for the fire footprint. Fire
    footprints span tens of km, so this bakes at ``world_resolution_m`` (default 30 m,
    matching LANDFIRE/Copernicus native) from Copernicus rather than a game/print 3DEP
    bake, which would be impractically large. The baked world is linked into the bundle
    (``world_region`` + ``world_manifest_hash``) for replay integrity.
    """
    name, year = parse_historic_id(slug)
    incident_id = hist_id(name, year)
    store = IncidentStore.create(store_root, incident_id)
    store.ensure_dirs()
    now = datetime.now(UTC)

    series = fetch_perimeter_series(name, year)
    if not series:
        raise RuntimeError(f"no perimeters found for {name} {year}")

    # incident record (metadata + size series)
    record = IncidentRecord(
        incident_id=incident_id, name=name, year=year,
        discovered_at=series[0].observed_at, final_acres=series[-1].acres,
        size_series=[SizePoint(at=p.observed_at, acres=p.acres) for p in series if p.observed_at],
        cross_ids={"geomac": f"{year}"},
    )
    store.incident_json.write_text(record.model_dump_json(indent=2), encoding="utf-8")

    # immutable, content-addressed observations (one geojson per perimeter)
    envelopes: list[ObservationEnvelope] = []
    pdir = store.observations_dir("perimeters")
    pdir.mkdir(parents=True, exist_ok=True)
    for p in series:
        ts = p.observed_at.strftime("%Y%m%dT%H%M") if p.observed_at else "unknown"
        fp = pdir / f"{ts}_{p.ghash[:12]}.geojson"
        _write_geojson(fp, p.geom)
        envelopes.append(ObservationEnvelope(
            kind="perimeter", source=f"geomac-{year}", observed_at=p.observed_at,
            fetched_at=now, geometry_hash=p.ghash,
            path=str(fp.relative_to(store.dir)), attributes={"acres": p.acres},
        ))

    # AOI (final footprint + buffer) — drives the terrain bake below
    aoi_geom = series[-1].geom.buffer(buffer_km / 111.0)
    _write_geojson(store.aoi_geojson, aoi_geom)

    # derived: arrival-time raster (the keystone)
    grid = build_incident_grid(series, buffer_km, resolution_m)
    arr_stats = build_arrival_raster(series, grid, store.derived(""), resolution_m=resolution_m)
    alg = arr_stats["algorithm"]

    # world bake (A2): DEM + LANDFIRE fuels for the fire AOI, via the terrain engine.
    world_region: str | None = None
    world_hash: str | None = None
    world_prov: dict | None = None
    if bake_world:
        minx, miny, maxx, maxy = aoi_geom.bounds  # lon/lat bbox from the buffered footprint
        rr = bake_world_for_aoi(
            incident_id, [minx, miny, maxx, maxy], store_root=str(store_root),
            profile="custom", resolution_m=world_resolution_m,
            sources=["copernicus-30m"], fuels=True,
        )
        world_region, world_hash = rr.region, rr.config_hash
        world_prov = {
            "region": rr.region, "resolution_m": world_resolution_m,
            "sources": rr.source_id, "config_hash": rr.config_hash,
            "dem_cog": str(rr.dem_cog) if rr.dem_cog else None, "fuels": rr.fuels,
        }

    bundle = BundleManifest(
        incident_id=incident_id, created_utc=now,
        aoi_geojson=str(store.aoi_geojson.relative_to(store.dir)),
        world_region=world_region, world_manifest_hash=world_hash,
        observations=envelopes,
        derived={
            "arrival_time": f"derived/arrival_time.{alg}.cog.tif",
            "confidence": f"derived/confidence.{alg}.cog.tif",
        },
        provenance={"arrival": arr_stats, "perimeter_source": f"geomac-{year}",
                    "buffer_km": buffer_km, "resolution_m": resolution_m,
                    "world": world_prov},
    )
    store.bundle_json.write_text(bundle.model_dump_json(indent=2), encoding="utf-8")
    log.info("assembled %s: %d perimeters, world=%s, %s",
             incident_id, len(series), world_region or "(none)", arr_stats)
    return bundle
