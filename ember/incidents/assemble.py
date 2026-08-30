"""E2 — assemble a fire incident into a scenario bundle.

Wires the observation adapters (E1 historic / B1+B2 live) -> A1 (incident record +
immutable observations) -> A2 (terrain+fuels bake) -> D2 (arrival raster) -> the
bundle manifest. `ember incident --historic <id>` and `--irwin <id>` both land here,
sharing the same downstream so a live fire travels the identical path as a historic one.
"""

from __future__ import annotations

import hashlib
import json
from datetime import UTC, datetime, timedelta
from pathlib import Path

from shapely.geometry import mapping
from terrain.util.logging import get_logger

from ember.incidents.arcgis import Perimeter
from ember.incidents.arrival import ALGORITHM, build_arrival_raster, build_incident_grid
from ember.incidents.bake import bake_world_for_aoi
from ember.incidents.firms import fetch_hotspots, hotspots_geojson
from ember.incidents.geomac import fetch_perimeter_series
from ember.incidents.model import (
    BundleManifest,
    IncidentRecord,
    IncidentStore,
    ObservationEnvelope,
    SizePoint,
    hist_id,
)
from ember.incidents.nirops import discover_ir_products
from ember.incidents.secrets import get_secret
from ember.incidents.wfigs import fetch_current_perimeters, fetch_incident, normalize_irwin
from ember.weather.build import build_weather_timeline

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


def _read_geojson_geom(path: Path):
    from shapely.geometry import shape

    feats = json.loads(path.read_text(encoding="utf-8")).get("features", [])
    return shape(feats[0]["geometry"]) if feats else None


def _load_prior_bundle(store: IncidentStore) -> BundleManifest | None:
    """The prior bundle is the record of what's already observed (F1 refresh)."""
    if not store.bundle_json.exists():
        return None
    try:
        return BundleManifest.model_validate_json(store.bundle_json.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def _perimeters_from_envelopes(
    store: IncidentStore, envs: list[ObservationEnvelope]
) -> list[Perimeter]:
    """Rehydrate stored perimeter observations (geom from disk, meta from envelope)."""
    out: list[Perimeter] = []
    for e in envs:
        if e.kind != "perimeter":
            continue
        geom = _read_geojson_geom(store.dir / e.path)
        if geom is not None:
            out.append(Perimeter(observed_at=e.observed_at,
                                 acres=float(e.attributes.get("acres", 0.0)),
                                 geom=geom, name="", ghash=e.geometry_hash))
    return out


def _enrich_hotspots(
    store: IncidentStore, aoi_geom, days: int, now: datetime
) -> ObservationEnvelope | None:
    """B3 — attach FIRMS hotspots over the AOI as an immutable observation (best-effort)."""
    minx, miny, maxx, maxy = aoi_geom.bounds
    hs = fetch_hotspots([minx, miny, maxx, maxy], days=days)
    if not hs:
        return None
    hdir = store.observations_dir("hotspots")
    hdir.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(hotspots_geojson(hs))
    gh = hashlib.sha256(payload.encode("utf-8")).hexdigest()
    fp = hdir / f"firms_{now.strftime('%Y%m%dT%H%M')}_{gh[:12]}.geojson"
    fp.write_text(payload, encoding="utf-8")
    return ObservationEnvelope(
        kind="hotspots", source="firms", observed_at=max(h.acq_at for h in hs),
        fetched_at=now, geometry_hash=gh, path=str(fp.relative_to(store.dir)),
        attributes={"count": len(hs), "days": days},
    )


def _enrich_nirops(
    store: IncidentStore, name: str, year: int, now: datetime
) -> ObservationEnvelope | None:
    """B4 — attach discovered NIROPS IR product listing as an observation (best-effort)."""
    products = discover_ir_products(name, year)
    if not products:
        return None
    idir = store.observations_dir("ir")
    idir.mkdir(parents=True, exist_ok=True)
    listing = [{"flight_date": p.flight_date.isoformat() if p.flight_date else None,
                "kind": p.kind, "filename": p.filename, "url": p.url} for p in products]
    payload = json.dumps({"source": "nirops", "products": listing})
    gh = hashlib.sha256(payload.encode("utf-8")).hexdigest()
    fp = idir / f"nirops_{year}_{gh[:12]}.json"
    fp.write_text(payload, encoding="utf-8")
    latest = max((p.flight_date for p in products if p.flight_date), default=None)
    return ObservationEnvelope(
        kind="ir", source="nirops", observed_at=latest, fetched_at=now,
        geometry_hash=gh, path=str(fp.relative_to(store.dir)),
        attributes={"product_count": len(products)},
    )


def _finalize_incident(
    store: IncidentStore, incident_id: str, record: IncidentRecord,
    series: list[Perimeter], now: datetime, *, perimeter_source: str,
    store_root: str | Path, buffer_km: float, resolution_m: float,
    bake_world: bool, world_resolution_m: float,
    enrich: bool = True, hotspots_days: int | None = None,
    weather: bool = False, weather_hours: int = 24, dry_run: bool = False,
) -> BundleManifest:
    """Shared downstream for both historic and live, with F1 refresh semantics:
    perimeter observations are append-only (content-addressed; already-seen hashes are
    skipped), and the arrival raster + enrichment + world bake are rebuilt only when a
    new perimeter arrives (or the derived product is missing). A re-run of an unchanged
    incident does ~zero work. ``dry_run`` reports the plan without writing or fetching."""
    prior = _load_prior_bundle(store)
    prior_obs = list(prior.observations) if prior else []
    prior_perim_hashes = {e.geometry_hash for e in prior_obs if e.kind == "perimeter"}

    new_perims = [p for p in series if p.ghash not in prior_perim_hashes]
    arrival_cog = store.derived(f"arrival_time.{ALGORITHM}.cog.tif")
    changed = bool(new_perims) or not arrival_cog.exists()

    if dry_run:
        log.info("dry-run %s: fetched=%d new_perimeter(s)=%d arrival_rebuild=%s "
                 "bake=%s enrich=%s weather=%s",
                 incident_id, len(series), len(new_perims), changed, bake_world, enrich, weather)
        return prior or BundleManifest(incident_id=incident_id, created_utc=now,
                                       aoi_geojson=str(store.aoi_geojson.name))

    if prior is not None and not changed:
        log.info("refresh %s: no new perimeters, arrival up to date — nothing to do",
                 incident_id)
        return prior

    store.incident_json.write_text(record.model_dump_json(indent=2), encoding="utf-8")

    # perimeter observations: APPEND-ONLY. Existing ones stay on disk; write only new.
    envelopes: list[ObservationEnvelope] = list(prior_obs)
    obs_hashes = {e.geometry_hash for e in envelopes}
    pdir = store.observations_dir("perimeters")
    pdir.mkdir(parents=True, exist_ok=True)
    for p in new_perims:
        ts = p.observed_at.strftime("%Y%m%dT%H%M") if p.observed_at else "unknown"
        fp = pdir / f"{ts}_{p.ghash[:12]}.geojson"
        _write_geojson(fp, p.geom)
        envelopes.append(ObservationEnvelope(
            kind="perimeter", source=perimeter_source, observed_at=p.observed_at,
            fetched_at=now, geometry_hash=p.ghash,
            path=str(fp.relative_to(store.dir)), attributes={"acres": p.acres},
        ))
        obs_hashes.add(p.ghash)

    # full perimeter set (rehydrated prior + this fetch) drives AOI + arrival
    by_hash = {pp.ghash: pp for pp in _perimeters_from_envelopes(store, prior_obs)}
    for p in series:
        by_hash[p.ghash] = p  # this fetch's geoms are authoritative
    _epoch = datetime.min.replace(tzinfo=UTC)
    full = sorted(by_hash.values(), key=lambda pp: pp.observed_at or _epoch)

    # AOI (final footprint + buffer) — drives the terrain bake below
    aoi_geom = full[-1].geom.buffer(buffer_km / 111.0)
    _write_geojson(store.aoi_geojson, aoi_geom)

    # additional observations (B3 FIRMS hotspots, B4 NIROPS IR) — best-effort, never block.
    # Append-only: a snapshot whose content hash is already stored is not re-added.
    def _append(env: ObservationEnvelope | None) -> bool:
        if env and env.geometry_hash not in obs_hashes:
            envelopes.append(env)
            obs_hashes.add(env.geometry_hash)
            return True
        return False

    if enrich:
        if hotspots_days and get_secret("FIRMS_MAP_KEY"):
            try:
                env = _enrich_hotspots(store, aoi_geom, hotspots_days, now)
                if _append(env):
                    log.info("firms: +%d hotspots (%d-day window)",
                             env.attributes["count"], hotspots_days)
            except Exception as ex:  # noqa: BLE001 — enrichment must not sink the bundle
                log.warning("firms enrichment skipped: %s", ex)
        try:
            env = _enrich_nirops(store, record.name, record.year, now)
            if _append(env):
                log.info("nirops: +%d IR product(s)", env.attributes["product_count"])
        except Exception as ex:  # noqa: BLE001
            log.warning("nirops enrichment skipped: %s", ex)

    # derived: arrival-time raster (the keystone) — rebuilt from the FULL perimeter set
    grid = build_incident_grid(full, buffer_km, resolution_m)
    arr_stats = build_arrival_raster(full, grid, store.derived(""), resolution_m=resolution_m)
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

    # weather timeline (C2 HRRR + C3 RAWS -> C4) — opt-in (HRRR is per-hour fetches),
    # best-effort, anchored on the latest perimeter time. Never blocks the bundle.
    weather_path: str | None = None
    if weather:
        ref = full[-1].observed_at or now
        w_t0 = ref.replace(minute=0, second=0, microsecond=0) - timedelta(hours=weather_hours - 1)
        minx, miny, maxx, maxy = aoi_geom.bounds
        try:
            mani = build_weather_timeline(
                incident_id, [minx, miny, maxx, maxy], w_t0, weather_hours, 60,
                weather_dir=store.weather_dir, save_dir=store.weather_dir / "raw",
            )
            if mani is not None:
                weather_path = "weather/timeline.v0.json"
                log.info("weather: %d-h timeline (grid=%s, %d station(s))",
                         weather_hours, mani.grid is not None, len(mani.stations))
        except Exception as ex:  # noqa: BLE001 — weather is best-effort
            log.warning("weather timeline skipped: %s", ex)

    bundle = BundleManifest(
        incident_id=incident_id, created_utc=now,
        aoi_geojson=str(store.aoi_geojson.relative_to(store.dir)),
        world_region=world_region, world_manifest_hash=world_hash,
        weather=weather_path,
        observations=envelopes,
        derived={
            "arrival_time": f"derived/arrival_time.{alg}.cog.tif",
            "confidence": f"derived/confidence.{alg}.cog.tif",
        },
        provenance={"arrival": arr_stats, "perimeter_source": perimeter_source,
                    "buffer_km": buffer_km, "resolution_m": resolution_m,
                    "world": world_prov,
                    "refresh": {"perimeters_total": len(full),
                                "perimeters_added": len(new_perims)}},
    )
    store.bundle_json.write_text(bundle.model_dump_json(indent=2), encoding="utf-8")
    log.info("assembled %s: %d perimeter(s) total (+%d new), source=%s, world=%s, %s",
             incident_id, len(full), len(new_perims), perimeter_source,
             world_region or "(none)", arr_stats)
    return bundle


def assemble_historic(
    slug: str, store_root: str | Path = "store", *,
    buffer_km: float = 3.0, resolution_m: float = 30.0,
    bake_world: bool = True, world_resolution_m: float = 30.0,
    enrich: bool = True, weather: bool = False, weather_hours: int = 24,
    dry_run: bool = False,
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

    record = IncidentRecord(
        incident_id=incident_id, name=name, year=year,
        discovered_at=series[0].observed_at, final_acres=series[-1].acres,
        size_series=[SizePoint(at=p.observed_at, acres=p.acres) for p in series if p.observed_at],
        cross_ids={"geomac": f"{year}"},
    )
    # historic fires predate the FIRMS NRT horizon, so hotspots_days is left None
    # (a trailing-window fetch would attach current, unrelated detections). NIROPS is
    # still attempted — it simply returns absent for years not in the archive.
    return _finalize_incident(
        store, incident_id, record, series, now, perimeter_source=f"geomac-{year}",
        store_root=store_root, buffer_km=buffer_km, resolution_m=resolution_m,
        bake_world=bake_world, world_resolution_m=world_resolution_m,
        enrich=enrich, hotspots_days=None, weather=weather, weather_hours=weather_hours,
        dry_run=dry_run,
    )


def assemble_live(
    irwin: str, store_root: str | Path = "store", *,
    buffer_km: float = 5.0, resolution_m: float = 30.0,
    bake_world: bool = True, world_resolution_m: float = 30.0,
    enrich: bool = True, firms_days: int = 10, weather: bool = False, weather_hours: int = 24,
    dry_run: bool = False,
) -> BundleManifest:
    """Assemble a live fire by IRWIN id (B1 + B2 via WFIGS) into a bundle.

    WFIGS holds the current footprint (usually one perimeter); the progression builds
    up as ``--refresh`` (Phase 3) re-polls and appends observations. Everything after
    the fetch is identical to the historic path. Raises if the fire has no mapped
    perimeter yet (located-only incidents can't seed an arrival raster).
    """
    meta = fetch_incident(irwin)  # B1
    series = fetch_current_perimeters(irwin, fallback_name=meta.name)  # B2
    if not series:
        raise RuntimeError(
            f"WFIGS incident {meta.name!r} ({normalize_irwin(irwin)}) has no mapped perimeter "
            "yet — located-only. Retry with --refresh once a perimeter is published."
        )

    incident_id = meta.irwin  # IRWIN id is canonical for live fires
    store = IncidentStore.create(store_root, incident_id)
    store.ensure_dirs()
    now = datetime.now(UTC)
    year = (meta.discovered_at or now).year

    record = IncidentRecord(
        incident_id=incident_id, name=meta.name, year=year,
        discovered_at=meta.discovered_at, contained_at=meta.contained_at,
        final_acres=meta.final_acres or meta.size_acres or series[-1].acres,
        size_series=[SizePoint(at=p.observed_at, acres=p.acres) for p in series if p.observed_at],
        cross_ids={"irwin": meta.irwin},
    )
    return _finalize_incident(
        store, incident_id, record, series, now, perimeter_source="wfigs",
        store_root=store_root, buffer_km=buffer_km, resolution_m=resolution_m,
        bake_world=bake_world, world_resolution_m=world_resolution_m,
        enrich=enrich, hotspots_days=firms_days, weather=weather, weather_hours=weather_hours,
        dry_run=dry_run,
    )
