"""C3 — Synoptic RAWS adapter (offline parse/units + mocked fetch; live gated on token)."""

import os
from datetime import UTC, datetime, timedelta

import pytest

RUN_NETWORK = os.environ.get("TERRAIN_RUN_NETWORK") == "1"


def _resp():
    return {
        "SUMMARY": {"RESPONSE_CODE": 1, "RESPONSE_MESSAGE": "OK"},
        "STATION": [{
            "STID": "SWAUK", "NAME": "SWAUK", "LATITUDE": 47.25, "LONGITUDE": -120.67,
            "ELEVATION": 3480.0, "MNET_ID": "2",
            "OBSERVATIONS": {
                "date_time": ["2026-08-25T15:00:00Z", "2026-08-25T16:00:00Z"],
                "air_temp_set_1": [20.0, 22.0],
                "relative_humidity_set_1": [35.0, 30.0],
                "wind_speed_set_1": [3.0, None],
                "wind_direction_set_1": [90.0, None],
                "fuel_moisture_set_1": [7.7, 7.5],
            },
        }],
    }


def test_parse_timeseries_units_and_uv():
    from ember.weather.synoptic import parse_timeseries

    st = parse_timeseries(_resp())[0]
    assert st.stid == "SWAUK" and st.network == "2" and len(st.samples) == 2
    s0 = st.samples[0]
    assert abs(s0.air_temp_k - 293.15) < 1e-6  # 20 C -> K
    assert s0.rh == 35.0 and s0.fuel_moisture == 7.7
    # wind FROM 90° (east) at 3 m/s -> u=-3, v~0
    assert abs(s0.wind_u + 3.0) < 1e-6 and abs(s0.wind_v) < 1e-6
    assert st.samples[1].wind_u is None  # missing speed/dir -> no components


def test_fetch_requires_token(monkeypatch):
    from ember.incidents import secrets
    from ember.weather import synoptic

    monkeypatch.setattr(secrets, "get_secret", lambda name: None)
    with pytest.raises(RuntimeError, match="SYNOPTIC_TOKEN"):
        synoptic.fetch_raws([-121, 47, -120, 48], datetime(2026, 8, 25, tzinfo=UTC),
                            datetime(2026, 8, 26, tzinfo=UTC))


def test_fetch_raws_mocked(monkeypatch):
    from ember.weather import synoptic

    monkeypatch.setattr(synoptic, "_http_get_json", lambda url, timeout: _resp())
    sts = synoptic.fetch_raws([-121, 47, -120, 48], datetime(2026, 8, 25, tzinfo=UTC),
                              datetime(2026, 8, 26, tzinfo=UTC), token="X")
    assert len(sts) == 1 and sts[0].stid == "SWAUK"


def test_fetch_raws_code2_is_empty(monkeypatch):
    from ember.weather import synoptic

    monkeypatch.setattr(
        synoptic, "_http_get_json",
        lambda url, timeout: {"SUMMARY": {"RESPONSE_CODE": 2, "RESPONSE_MESSAGE": "none"}})
    out = synoptic.fetch_raws([-121, 47, -120, 48], datetime(2026, 8, 25, tzinfo=UTC),
                              datetime(2026, 8, 26, tzinfo=UTC), token="X")
    assert out == []


@pytest.mark.network
@pytest.mark.geo
@pytest.mark.skipif(not (RUN_NETWORK and os.environ.get("SYNOPTIC_TOKEN")),
                    reason="needs TERRAIN_RUN_NETWORK=1 and SYNOPTIC_TOKEN")
def test_synoptic_live():
    from ember.weather.synoptic import fetch_raws

    end = datetime(2026, 8, 25, 18, tzinfo=UTC)
    sts = fetch_raws([-121.3, 47.0, -120.5, 47.7], end - timedelta(hours=6), end)
    assert sts and all(s.network == "2" for s in sts)  # RAWS network
    assert any(any(x.air_temp_k for x in s.samples) for s in sts)
