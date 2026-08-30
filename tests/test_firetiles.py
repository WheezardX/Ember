"""D3 — fire-state tiling (offline: assemble a synthetic incident, tile it)."""

from datetime import UTC, datetime, timedelta

import pytest


def _perims():
    from shapely.geometry import Point

    from ember.incidents.arcgis import Perimeter

    return [Perimeter(observed_at=datetime(2017, 8, 12, tzinfo=UTC) + timedelta(days=i),
                      acres=100.0 * (i + 1) ** 2, geom=Point(-121.0, 47.3).buffer(0.02 * (i + 1)),
                      name="Synth", ghash=f"h{i}") for i in range(5)]


@pytest.mark.geo
def test_tile_fire_state(tmp_path, monkeypatch):
    import ember.incidents.assemble as asm
    from ember.incidents.firetiles import tile_fire_state
    from ember.incidents.model import IncidentStore, hist_id

    monkeypatch.setattr(asm, "fetch_perimeter_series", lambda name, year: _perims())
    asm.assemble_historic("synth-2017", store_root=tmp_path, buffer_km=1.0, resolution_m=90.0,
                          bake_world=False, enrich=False)

    incident_id = hist_id("Synth", 2017)
    store = IncidentStore.create(tmp_path, incident_id)
    m = tile_fire_state(store, incident_id)

    assert m["algorithm"] == "perimeter-interp-v1"
    assert m["time_index"]["unit"] == "hours_since_t0"
    arr = m["layers"]["arrival_time"]
    assert arr["tiles"] > 0 and len(arr["records"]) == arr["tiles"]
    # the manifest + at least one arrival tile file exist on disk under the incident dir
    assert (store.dir / "tiles" / "firestate.manifest.json").exists()
    r0 = arr["records"][0]
    assert (store.dir / r0["path"]).exists()
    assert r0["path"].replace("\\", "/").startswith("tiles/")
