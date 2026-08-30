"""B4 — NIROPS IR discovery (offline classify/parse + graceful degradation; live gated)."""

import os
from datetime import UTC, datetime

import pytest

RUN_NETWORK = os.environ.get("TERRAIN_RUN_NETWORK") == "1"


def test_classify_and_date():
    from ember.incidents.nirops import _classify, _date_from_name, _slug

    assert _classify("20260829_Goat_IR.kmz") == "kmz"
    assert _classify("20260828_2041_Goat_IR.gdb.zip") == "geodatabase"
    assert _classify("20260829_Goat_IR_ShapeFileOutputs.zip") == "shapefile"
    assert _classify("20260829_Goat_IR_11x17_Topo.pdf") == "pdf"
    assert _slug("Coleman Creek") == "colemancreek"
    assert _date_from_name("20260829_Goat_IR.kmz") == datetime(2026, 8, 29, tzinfo=UTC)
    assert _date_from_name("no-date-here") is None


def test_discover_degrades_to_empty(monkeypatch):
    from ember.incidents import nirops

    def _boom(url, *, timeout=30):
        raise OSError("server moved")

    monkeypatch.setattr(nirops, "list_links", _boom)
    assert nirops.discover_ir_products("Goat", 2026) == []  # never raises


def test_discover_happy_path(monkeypatch):
    from ember.incidents import nirops

    b = nirops.BASE
    tree = {
        f"{b}/pacific_nw/": [f"{b}/pacific_nw/2026_Incidents_Washington/"],
        f"{b}/pacific_nw/2026_Incidents_Washington/": [
            f"{b}/pacific_nw/2026_Incidents_Washington/2026_Goat/",
            f"{b}/pacific_nw/2026_Incidents_Washington/2026_Other/",
        ],
        f"{b}/pacific_nw/2026_Incidents_Washington/2026_Goat/": [
            f"{b}/pacific_nw/2026_Incidents_Washington/2026_Goat/IR/",
            f"{b}/pacific_nw/2026_Incidents_Washington/2026_Goat/Products/",
        ],
        f"{b}/pacific_nw/2026_Incidents_Washington/2026_Goat/IR/": [
            f"{b}/pacific_nw/2026_Incidents_Washington/2026_Goat/IR/20260829/",
        ],
        f"{b}/pacific_nw/2026_Incidents_Washington/2026_Goat/IR/20260829/": [
            f"{b}/pacific_nw/2026_Incidents_Washington/2026_Goat/IR/20260829/20260829_Goat_IR.kmz",
            f"{b}/pacific_nw/2026_Incidents_Washington/2026_Goat/IR/20260829/20260829_Goat_IR_ShapeFileOutputs.zip",
        ],
    }
    # only search the one GACC that's in the tree
    monkeypatch.setattr(nirops, "GACCS", ("pacific_nw",))
    monkeypatch.setattr(nirops, "list_links", lambda url, *, timeout=30: tree.get(url, []))

    products = nirops.discover_ir_products("Goat", 2026)
    kinds = {p.kind for p in products}
    assert kinds == {"kmz", "shapefile"}
    assert all(p.flight_date == datetime(2026, 8, 29, tzinfo=UTC) for p in products)


@pytest.mark.network
@pytest.mark.skipif(not RUN_NETWORK, reason="walks the live wildfire.gov IR directory")
def test_nirops_live_discovery():
    from ember.incidents.nirops import discover_ir_products

    # 2026 Goat (WA) had nightly IR products at probe time; best-effort, so tolerate 0
    products = discover_ir_products("Goat", 2026, gacc="pacific_nw")
    assert isinstance(products, list)
    if products:
        assert any(p.kind == "kmz" for p in products)
