"""F2 — per-incident QA report (offline: assemble a synthetic incident, render it)."""

from datetime import UTC, datetime, timedelta

import pytest


def _perims():
    from shapely.geometry import Point

    from ember.incidents.arcgis import Perimeter

    return [Perimeter(observed_at=datetime(2017, 8, 12, tzinfo=UTC) + timedelta(days=i),
                      acres=100.0 * (i + 1) ** 2, geom=Point(-121.0, 47.3).buffer(0.01 * (i + 1)),
                      name="Synth", ghash=f"h{i}") for i in range(5)]


@pytest.mark.geo
def test_build_qa_report(tmp_path, monkeypatch):
    import ember.incidents.assemble as asm
    from ember.incidents.model import IncidentStore, hist_id
    from ember.incidents.qa import build_qa_report

    monkeypatch.setattr(asm, "fetch_perimeter_series", lambda name, year: _perims())
    asm.assemble_historic("synth-2017", store_root=tmp_path, buffer_km=1.0, resolution_m=90.0,
                          bake_world=False, enrich=False)

    store = IncidentStore.create(tmp_path, hist_id("Synth", 2017))
    out = build_qa_report(store)
    assert out.exists() and out.name == "qa.html"
    txt = out.read_text(encoding="utf-8")
    # the keystone views + metadata are present, and the arrival heatmap actually rendered
    assert "Arrival time" in txt and "Observations" in txt and "Growth" in txt
    assert "Synth" in txt and "hist:synth-2017" in txt
    assert "<rect" in txt  # burned cells drawn (not the empty-raster fallback)
