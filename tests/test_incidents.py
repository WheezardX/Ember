"""Epic 3 — incident model, arrival raster, historic (Phase 1) + live WFIGS (Phase 2)."""

import os
from datetime import UTC, datetime, timedelta

import pytest

RUN_NETWORK = os.environ.get("TERRAIN_RUN_NETWORK") == "1"


def test_incident_ids_and_store(tmp_path):
    from ember.incidents.model import IncidentStore, hist_id, id_to_dirname

    assert hist_id("Jolly Mountain", 2017) == "hist:jolly-mountain-2017"
    assert hist_id("JOLLY MOUNTAIN", 2017) == "hist:jolly-mountain-2017"
    dn = id_to_dirname("hist:jolly-mountain-2017")
    store = IncidentStore.create(tmp_path, "hist:jolly-mountain-2017")
    assert store.dir == tmp_path / "incidents" / dn
    store.ensure_dirs()
    assert store.observations_dir("perimeters").parent.is_dir()


def test_record_and_bundle_roundtrip():
    from ember.incidents.model import BundleManifest, IncidentRecord, SizePoint

    rec = IncidentRecord(
        incident_id="hist:x-2020", name="X", year=2020,
        size_series=[SizePoint(at=datetime(2020, 1, 1, tzinfo=UTC), acres=10.0)],
    )
    rec2 = IncidentRecord.model_validate_json(rec.model_dump_json())
    assert rec2.name == "X" and rec2.size_series[0].acres == 10.0

    b = BundleManifest(
        incident_id="hist:x-2020", created_utc=datetime.now(UTC), aoi_geojson="aoi.geojson"
    )
    assert BundleManifest.model_validate_json(b.model_dump_json()).incident_id == "hist:x-2020"


def test_parse_historic_id():
    from ember.incidents.assemble import parse_historic_id

    assert parse_historic_id("jolly-mountain-2017") == ("Jolly Mountain", 2017)
    with pytest.raises(ValueError):
        parse_historic_id("no-year")


def test_normalize_irwin():
    from ember.incidents.wfigs import normalize_irwin

    guid = "{5689AD11-5D8E-40B2-A8E1-C72A774FD1D7}"
    assert normalize_irwin("5689ad11-5d8e-40b2-a8e1-c72a774fd1d7") == guid
    assert normalize_irwin(guid) == guid
    assert normalize_irwin("  {5689ad11-5d8e-40b2-a8e1-c72a774fd1d7}  ") == guid


@pytest.mark.geo
def test_features_to_perimeters_wfigs_field_aliases():
    """WFIGS poly_* attribute names resolve through the shared cleaning stage."""
    from shapely.geometry import Point, mapping

    from ember.incidents.arcgis import features_to_perimeters

    epoch_ms = int(datetime(2025, 7, 4, tzinfo=UTC).timestamp() * 1000)
    feat = {
        "geometry": mapping(Point(-121.0, 47.3).buffer(0.01)),
        "properties": {"poly_IncidentName": "Test Fire",
                       "poly_PolygonDateTime": epoch_ms, "poly_GISAcres": 123.0},
    }
    series = features_to_perimeters([feat], fallback_name="fallback")
    assert len(series) == 1
    p = series[0]
    assert p.name == "Test Fire" and p.acres == 123.0
    assert p.observed_at == datetime(2025, 7, 4, tzinfo=UTC)
    assert p.geom.geom_type == "MultiPolygon"  # normalized


def _growing_perimeters():
    """Synthetic cumulative circles growing over 5 days near Teanaway (lon/lat)."""
    from shapely.geometry import Point

    from ember.incidents.geomac import Perimeter

    out = []
    for i in range(5):
        geom = Point(-121.0, 47.3).buffer(0.01 * (i + 1))
        out.append(Perimeter(
            observed_at=datetime(2017, 8, 12, tzinfo=UTC) + timedelta(days=i),
            acres=100.0 * (i + 1) ** 2, geom=geom, name="Synth", ghash=f"h{i}",
        ))
    return out


@pytest.mark.geo
def test_arrival_raster_deterministic(tmp_path):
    import numpy as np
    import rasterio

    from ember.incidents.arrival import build_arrival_raster, build_incident_grid

    per = _growing_perimeters()
    grid = build_incident_grid(per, buffer_km=1.0, resolution_m=90.0)

    s1 = build_arrival_raster(per, grid, tmp_path / "a", resolution_m=90.0)
    s2 = build_arrival_raster(per, grid, tmp_path / "b", resolution_m=90.0)
    assert s1["burned_cells"] == s2["burned_cells"] > 0
    a = rasterio.open(tmp_path / "a" / "arrival_time.perimeter-interp-v1.cog.tif").read(1)
    b = rasterio.open(tmp_path / "b" / "arrival_time.perimeter-interp-v1.cog.tif").read(1)
    assert np.array_equal(a, b)  # deterministic
    # arrival increases outward: min at t0, max near the final ring
    burned = a[a != -9999.0]
    assert burned.min() == 0.0 and burned.max() > 0.0


def test_bake_builds_custom_coarse_settings(monkeypatch):
    """The ember->terrain seam passes a custom-resolution, Copernicus-only bake."""
    import terrain.runner as runner

    from ember.incidents.bake import bake_world_for_aoi

    captured = {}

    def _fake_run(settings, store_root="store", **kw):  # noqa: ARG001
        captured["settings"] = settings
        return runner.RunResult(region="r", config_hash="abc")

    monkeypatch.setattr(runner, "run_pipeline", _fake_run)
    rr = bake_world_for_aoi(
        "hist:x-2020", [-121.1, 47.2, -120.9, 47.4],
        profile="custom", resolution_m=30.0, sources=["copernicus-30m"], fuels=True,
    )
    assert rr.config_hash == "abc"
    s = captured["settings"]
    assert s.dem.profile == "custom" and s.dem.effective_resolution_m == 30.0
    assert s.dem.sources == ["copernicus-30m"]
    assert s.fuels.enabled is True
    assert s.aoi.bbox == [-121.1, 47.2, -120.9, 47.4]


def test_bake_resolution_requires_custom_profile():
    from ember.incidents.bake import bake_world_for_aoi

    with pytest.raises(ValueError):  # resolution_m only valid with custom
        bake_world_for_aoi("n", [0, 0, 1, 1], profile="game", resolution_m=30.0)
    with pytest.raises(ValueError):  # custom needs an explicit resolution
        bake_world_for_aoi("n", [0, 0, 1, 1], profile="custom")


@pytest.mark.geo
def test_assemble_wires_bake_into_bundle(tmp_path, monkeypatch):
    """assemble_historic hands the AOI bbox to the bake and pins the baked world."""
    from terrain.runner import RunResult

    import ember.incidents.assemble as asm

    series = _growing_perimeters()
    monkeypatch.setattr(asm, "fetch_perimeter_series", lambda name, year: series)

    seen = {}

    def _fake_bake(name, bbox, **kw):
        seen["name"], seen["bbox"], seen["kw"] = name, bbox, kw
        return RunResult(
            region="hist-synth-2017", config_hash="deadbeef",
            source_id="copernicus-30m", dem_cog="dem.cog.tif",
            fuels={"fbfm40": "fuels/fbfm40.cog.tif"},
        )

    monkeypatch.setattr(asm, "bake_world_for_aoi", _fake_bake)

    bundle = asm.assemble_historic(
        "synth-2017", store_root=tmp_path, buffer_km=1.0, resolution_m=90.0, enrich=False,
    )

    # bbox handed to the bake == the buffered final-footprint bounds
    expected = list(series[-1].geom.buffer(1.0 / 111.0).bounds)
    assert seen["bbox"] == pytest.approx(expected)
    assert seen["kw"]["profile"] == "custom" and seen["kw"]["resolution_m"] == 30.0
    assert seen["kw"]["sources"] == ["copernicus-30m"]

    # the baked world is pinned into the bundle for replay integrity
    assert bundle.world_region == "hist-synth-2017"
    assert bundle.world_manifest_hash == "deadbeef"
    assert bundle.provenance["world"]["fuels"] == {"fbfm40": "fuels/fbfm40.cog.tif"}


@pytest.mark.geo
def test_assemble_no_bake_leaves_world_unpinned(tmp_path, monkeypatch):
    import ember.incidents.assemble as asm

    monkeypatch.setattr(asm, "fetch_perimeter_series",
                        lambda name, year: _growing_perimeters())

    def _no_call(*a, **k):
        raise AssertionError("bake must not run when bake_world=False")

    monkeypatch.setattr(asm, "bake_world_for_aoi", _no_call)
    bundle = asm.assemble_historic(
        "synth-2017", store_root=tmp_path, buffer_km=1.0, resolution_m=90.0,
        bake_world=False, enrich=False,
    )
    assert bundle.world_region is None and bundle.world_manifest_hash is None
    assert bundle.provenance["world"] is None


@pytest.mark.geo
def test_assemble_live_wires_wfigs(tmp_path, monkeypatch):
    """assemble_live keys off IRWIN, tags observations 'wfigs', shares the downstream."""
    from terrain.runner import RunResult

    import ember.incidents.assemble as asm
    from ember.incidents.wfigs import IncidentMeta

    guid = "{5689AD11-5D8E-40B2-A8E1-C72A774FD1D7}"
    meta = IncidentMeta(
        irwin=guid, name="Williams Creek",
        discovered_at=datetime(2025, 7, 1, tzinfo=UTC), contained_at=None,
        size_acres=120.0, final_acres=None, percent_contained=10.0,
        cause="Human", state="US-WA",
    )
    monkeypatch.setattr(asm, "fetch_incident", lambda irwin: meta)
    monkeypatch.setattr(asm, "fetch_current_perimeters",
                        lambda irwin, fallback_name="": _growing_perimeters())
    monkeypatch.setattr(asm, "bake_world_for_aoi",
                        lambda name, bbox, **kw: RunResult(region="r", config_hash="h"))

    bundle = asm.assemble_live(guid, store_root=tmp_path, buffer_km=1.0, resolution_m=90.0,
                               enrich=False)

    assert bundle.incident_id == guid  # IRWIN is canonical for live
    assert bundle.provenance["perimeter_source"] == "wfigs"
    assert all(o.source == "wfigs" for o in bundle.observations)
    assert bundle.world_region == "r" and bundle.world_manifest_hash == "h"
    # store dir is derived from the IRWIN guid
    assert (tmp_path / "incidents" / "5689ad11-5d8e-40b2-a8e1-c72a774fd1d7").is_dir()


@pytest.mark.geo
def test_assemble_live_no_perimeter_raises(tmp_path, monkeypatch):
    import ember.incidents.assemble as asm
    from ember.incidents.wfigs import IncidentMeta

    meta = IncidentMeta(irwin="{ABC}", name="Located Only", discovered_at=None,
                        contained_at=None, size_acres=None, final_acres=None,
                        percent_contained=None, cause=None, state=None)
    monkeypatch.setattr(asm, "fetch_incident", lambda irwin: meta)
    monkeypatch.setattr(asm, "fetch_current_perimeters", lambda irwin, fallback_name="": [])
    monkeypatch.setattr(asm, "bake_world_for_aoi",
                        lambda *a, **k: (_ for _ in ()).throw(AssertionError("no bake")))

    with pytest.raises(RuntimeError, match="no mapped perimeter"):
        asm.assemble_live("{ABC}", store_root=tmp_path)


def _live_meta():
    from ember.incidents.wfigs import IncidentMeta

    return IncidentMeta(
        irwin="{ABC}", name="Synth", discovered_at=datetime(2026, 7, 1, tzinfo=UTC),
        contained_at=None, size_acres=100.0, final_acres=None, percent_contained=None,
        cause=None, state=None)


@pytest.mark.geo
def test_enrich_adds_hotspots_and_ir_observations(tmp_path, monkeypatch):
    """When enrich=True on a live fire, FIRMS (B3) + NIROPS (B4) land as observations."""
    from terrain.runner import RunResult

    import ember.incidents.assemble as asm
    from ember.incidents.firms import Hotspot
    from ember.incidents.nirops import IRProduct

    monkeypatch.setattr(asm, "fetch_incident", lambda irwin: _live_meta())
    monkeypatch.setattr(asm, "fetch_current_perimeters",
                        lambda irwin, fallback_name="": _growing_perimeters())
    monkeypatch.setattr(asm, "bake_world_for_aoi",
                        lambda name, bbox, **kw: RunResult(region="r", config_hash="h"))
    monkeypatch.setattr(asm, "get_secret", lambda name: "TESTKEY")  # FIRMS key present
    monkeypatch.setattr(asm, "fetch_hotspots", lambda bbox, days=10: [
        Hotspot(acq_at=datetime(2026, 7, 2, 9, 30, tzinfo=UTC), lon=-121.0, lat=47.3,
                frp=12.3, confidence="n", satellite="N", daynight="D"),
        Hotspot(acq_at=datetime(2026, 7, 2, 20, 15, tzinfo=UTC), lon=-121.0, lat=47.3,
                frp=40.0, confidence="h", satellite="Aqua", daynight="N"),
    ])
    monkeypatch.setattr(asm, "discover_ir_products", lambda name, year: [
        IRProduct(flight_date=datetime(2026, 7, 2, tzinfo=UTC), kind="kmz",
                  filename="20260702_Synth_IR.kmz", url="https://x/20260702_Synth_IR.kmz"),
    ])

    bundle = asm.assemble_live("{ABC}", store_root=tmp_path, buffer_km=1.0,
                               resolution_m=90.0, bake_world=False, enrich=True)

    assert {"perimeter", "hotspots", "ir"} <= {o.kind for o in bundle.observations}
    hs = next(o for o in bundle.observations if o.kind == "hotspots")
    assert hs.source == "firms" and hs.attributes["count"] == 2
    iro = next(o for o in bundle.observations if o.kind == "ir")
    assert iro.source == "nirops" and iro.attributes["product_count"] == 1
    dirn = tmp_path / "incidents" / "abc"  # id_to_dirname('{ABC}')
    assert (dirn / hs.path).exists() and (dirn / iro.path).exists()


@pytest.mark.geo
def test_enrich_skips_firms_without_key(tmp_path, monkeypatch):
    from terrain.runner import RunResult

    import ember.incidents.assemble as asm

    monkeypatch.setattr(asm, "fetch_incident", lambda irwin: _live_meta())
    monkeypatch.setattr(asm, "fetch_current_perimeters",
                        lambda irwin, fallback_name="": _growing_perimeters())
    monkeypatch.setattr(asm, "bake_world_for_aoi",
                        lambda name, bbox, **kw: RunResult(region="r", config_hash="h"))
    monkeypatch.setattr(asm, "get_secret", lambda name: None)  # no FIRMS key
    monkeypatch.setattr(asm, "discover_ir_products", lambda name, year: [])  # no IR
    monkeypatch.setattr(asm, "fetch_hotspots",
                        lambda *a, **k: (_ for _ in ()).throw(AssertionError("no key -> no fetch")))

    bundle = asm.assemble_live("{ABC}", store_root=tmp_path, buffer_km=1.0,
                               resolution_m=90.0, bake_world=False, enrich=True)
    assert {o.kind for o in bundle.observations} == {"perimeter"}


@pytest.mark.network
@pytest.mark.slow
@pytest.mark.geo
@pytest.mark.skipif(not RUN_NETWORK, reason="fetches real GeoMAC perimeters")
def test_assemble_historic_jolly_mountain(tmp_path):
    import rasterio

    from ember.incidents.assemble import assemble_historic

    # bake_world=False keeps this focused on the perimeter->arrival path; the bake
    # wiring is covered offline by test_assemble_wires_bake_into_bundle.
    bundle = assemble_historic("jolly-mountain-2017", store_root=tmp_path, bake_world=False)
    assert bundle.incident_id == "hist:jolly-mountain-2017"
    assert len(bundle.observations) >= 30  # ~35 perimeters
    arr = tmp_path / "incidents" / "hist-jolly-mountain-2017" / bundle.derived["arrival_time"]
    with rasterio.open(arr) as ds:
        assert ds.crs.to_epsg() == 32610
    # burned footprint matches the documented ~36,808 ac (150 km2) within tolerance
    assert 130 < bundle.provenance["arrival"]["burned_km2"] < 170


@pytest.mark.network
@pytest.mark.geo
@pytest.mark.skipif(not RUN_NETWORK, reason="hits live WFIGS FeatureServers")
def test_wfigs_live_incident_and_perimeter():
    """Discover a current fire that has a perimeter, then resolve it via B1 + B2."""
    from ember.incidents.arcgis import arcgis_query
    from ember.incidents.wfigs import fetch_current_perimeters, fetch_incident

    # find a current perimeter, take its IRWIN (guaranteed to have both a location + poly)
    fc = arcgis_query(
        "WFIGS_Interagency_Perimeters_Current",
        {"where": "poly_GISAcres > 1000", "outFields": "poly_IRWINID,poly_IncidentName",
         "orderByFields": "poly_GISAcres DESC", "resultRecordCount": "1",
         "returnGeometry": "false", "f": "json"},
    )
    feats = fc.get("features", [])
    if not feats:
        pytest.skip("no current fire >1000 ac with a perimeter right now")
    irwin = feats[0]["attributes"]["poly_IRWINID"]

    perims = fetch_current_perimeters(irwin)
    assert perims and perims[0].geom.geom_type == "MultiPolygon"
    meta = fetch_incident(irwin)
    assert meta.name and meta.irwin.startswith("{")
