"""B3 — FIRMS hotspot adapter (offline parse + mocked fetch; live gated on key)."""

import os
from datetime import UTC, datetime

import pytest

RUN_NETWORK = os.environ.get("TERRAIN_RUN_NETWORK") == "1"

VIIRS_CSV = (
    "latitude,longitude,bright_ti4,scan,track,acq_date,acq_time,satellite,instrument,"
    "confidence,version,bright_ti5,frp,daynight\n"
    "47.31,-121.02,320.5,0.4,0.4,2017-09-05,930,N,VIIRS,n,2.0NRT,290.1,12.3,D\n"
    "47.32,-121.01,335.0,0.4,0.4,2017-09-05,930,N,VIIRS,h,2.0NRT,295.0,25.6,D\n"
)
MODIS_CSV = (
    "latitude,longitude,brightness,scan,track,acq_date,acq_time,satellite,instrument,"
    "confidence,version,bright_t31,frp,daynight\n"
    "47.30,-121.00,330.1,1.0,1.0,2017-09-05,2015,Aqua,MODIS,85,6.1NRT,300.0,40.0,N\n"
)


def test_parse_firms_csv_viirs_and_modis():
    from ember.incidents.firms import parse_firms_csv

    v = parse_firms_csv(VIIRS_CSV)
    assert len(v) == 2
    assert v[0].acq_at == datetime(2017, 9, 5, 9, 30, tzinfo=UTC)  # HHMM zero-padded
    assert v[0].frp == 12.3 and v[0].confidence == "n" and v[0].daynight == "D"

    m = parse_firms_csv(MODIS_CSV)
    assert len(m) == 1
    assert m[0].acq_at == datetime(2017, 9, 5, 20, 15, tzinfo=UTC)
    assert m[0].confidence == "85" and m[0].satellite == "Aqua"


def test_parse_skips_bad_rows():
    from ember.incidents.firms import parse_firms_csv

    bad = ("latitude,longitude,acq_date,acq_time,frp,confidence,satellite,daynight\n"
           ",,-,-,-,-,-,-\n47.3,-121.0,2020-01-01,1200,5.0,n,N,D\n")
    assert len(parse_firms_csv(bad)) == 1


def test_hotspots_geojson_shape():
    from ember.incidents.firms import hotspots_geojson, parse_firms_csv

    gj = hotspots_geojson(parse_firms_csv(VIIRS_CSV))
    assert gj["type"] == "FeatureCollection" and len(gj["features"]) == 2
    f = gj["features"][0]
    assert f["geometry"]["type"] == "Point"
    assert f["geometry"]["coordinates"] == [-121.02, 47.31]
    assert f["properties"]["satellite"] == "N"


def test_fetch_requires_key(monkeypatch):
    from ember.incidents import firms, secrets

    monkeypatch.delenv("FIRMS_MAP_KEY", raising=False)
    monkeypatch.setattr(secrets, "get_secret", lambda name: None)  # ignore env + .secrets.toml
    with pytest.raises(RuntimeError, match="FIRMS_MAP_KEY"):
        firms.fetch_hotspots([-122, 47, -120, 48])


def test_fetch_hotspots_mocked_dedup(monkeypatch):
    """Two sources returning overlapping detections collapse to unique ones."""
    from ember.incidents import firms

    # same VIIRS rows for every source -> dedup across sources
    monkeypatch.setattr(firms, "_http_get", lambda url, timeout: VIIRS_CSV)
    hs = firms.fetch_hotspots([-122, 47, -120, 48], days=5, map_key="TESTKEY",
                              sources=("VIIRS_SNPP_NRT", "VIIRS_NOAA20_NRT"))
    assert len(hs) == 2  # 2 unique rows despite 2 sources x 2 rows fetched
    assert all(h.acq_at.tzinfo is UTC for h in hs)


@pytest.mark.network
@pytest.mark.skipif(not (RUN_NETWORK and os.environ.get("FIRMS_MAP_KEY")),
                    reason="needs TERRAIN_RUN_NETWORK=1 and FIRMS_MAP_KEY")
def test_firms_live_smoke():
    from ember.incidents.firms import fetch_hotspots

    # trailing 7-day window over a broad western-US bbox should return detections in season
    hs = fetch_hotspots([-124, 40, -117, 49], days=7)
    assert isinstance(hs, list)  # non-crash + typed; count varies with fire activity
