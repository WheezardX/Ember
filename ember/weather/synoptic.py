"""C3 — RAWS station observations (Synoptic Data / MesoWest timeseries API).

Pulls the point-station half of a weather timeline: RAWS stations within the incident
AOI, with wind / temperature / RH / fuel-moisture series over the window. Feeds the
station side of the C1 schema; C4 merges these with the HRRR grid ([[hrrr.py]]).

Needs a free ``SYNOPTIC_TOKEN`` (generate a token from your API key in the Synoptic
Customer Console — see docs/CREDENTIALS.md; [[secrets.py]] loads it). The token is never
written into provenance or bundles. RAWS is Synoptic network id 2.

Verified live 2026-08-30 against api.synopticdata.com/v2.
"""

from __future__ import annotations

import json
import math
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from datetime import UTC, datetime

from terrain.util.logging import get_logger

from ember.incidents.secrets import require_secret

log = get_logger(__name__)

SYNOPTIC_BASE = "https://api.synopticdata.com/v2/stations/timeseries"
TOKEN_ENV = "SYNOPTIC_TOKEN"
RAWS_NETWORK = "2"  # Synoptic/MesoWest network id for RAWS

# Synoptic variable -> our handling. Requested via the `vars` param; returned as
# "<var>_set_1" arrays aligned with the OBSERVATIONS date_time list.
_REQUEST_VARS = "air_temp,relative_humidity,wind_speed,wind_direction,fuel_moisture"


@dataclass(frozen=True)
class StationSample:
    time: datetime
    air_temp_k: float | None
    rh: float | None
    wind_speed: float | None  # m/s
    wind_dir: float | None  # degrees (meteorological, FROM)
    wind_u: float | None  # m/s (eastward)
    wind_v: float | None  # m/s (northward)
    fuel_moisture: float | None  # %


@dataclass
class RawsStation:
    stid: str
    name: str
    lon: float
    lat: float
    elevation_m: float | None
    network: str
    samples: list[StationSample] = field(default_factory=list)


def _parse_dt(s: str) -> datetime | None:
    try:
        return datetime.fromisoformat(str(s).replace("Z", "+00:00")).astimezone(UTC)
    except (ValueError, TypeError):
        return None


def _uv(speed: float | None, direction: float | None) -> tuple[float | None, float | None]:
    """Meteorological wind (FROM direction) -> eastward u / northward v components."""
    if speed is None or direction is None:
        return None, None
    r = math.radians(direction)
    return -speed * math.sin(r), -speed * math.cos(r)


def _http_get_json(url: str, timeout: float) -> dict:
    """Isolated so tests can mock the network at one point."""
    req = urllib.request.Request(url, headers={"User-Agent": "ember-synoptic/0"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:  # noqa: S310 (fixed https host)
        return json.loads(resp.read().decode("utf-8"))


def parse_timeseries(resp: dict) -> list[RawsStation]:
    """Synoptic timeseries JSON -> RawsStation list (air_temp C->K, wind speed/dir->u/v)."""
    out: list[RawsStation] = []
    for st in resp.get("STATION", []) or []:
        obs = st.get("OBSERVATIONS", {}) or {}
        times = obs.get("date_time", []) or []
        n = len(times)
        temps = obs.get("air_temp_set_1", [None] * n)
        rhs = obs.get("relative_humidity_set_1", [None] * n)
        spds = obs.get("wind_speed_set_1", [None] * n)
        dirs = obs.get("wind_direction_set_1", [None] * n)
        fms = obs.get("fuel_moisture_set_1", [None] * n)
        samples: list[StationSample] = []
        for i, ts in enumerate(times):
            t = _parse_dt(ts)
            if t is None:
                continue
            temp_c = temps[i] if i < len(temps) else None
            spd = spds[i] if i < len(spds) else None
            drc = dirs[i] if i < len(dirs) else None
            u, v = _uv(spd, drc)
            samples.append(StationSample(
                time=t,
                air_temp_k=(temp_c + 273.15) if temp_c is not None else None,
                rh=(rhs[i] if i < len(rhs) else None),
                wind_speed=spd, wind_dir=drc, wind_u=u, wind_v=v,
                fuel_moisture=(fms[i] if i < len(fms) else None),
            ))
        out.append(RawsStation(
            stid=str(st.get("STID", "")), name=str(st.get("NAME", "")).strip(),
            lon=float(st.get("LONGITUDE")), lat=float(st.get("LATITUDE")),
            elevation_m=(float(st["ELEVATION"]) if st.get("ELEVATION") not in (None, "") else None),
            network=str(st.get("MNET_ID", "")), samples=samples,
        ))
    return out


def fetch_raws(
    bbox: list[float], start: datetime, end: datetime, *,
    networks: tuple[str, ...] = (RAWS_NETWORK,), token: str | None = None, timeout: float = 60,
) -> list[RawsStation]:
    """Fetch RAWS station series for a lon/lat bbox [W,S,E,N] over [start, end] (UTC)."""
    tok = token or require_secret(TOKEN_ENV, hint="generate a token in the Synoptic console")
    w, s, e, n = bbox
    params = {
        "token": tok, "bbox": f"{w},{s},{e},{n}", "network": ",".join(networks),
        "start": start.strftime("%Y%m%d%H%M"), "end": end.strftime("%Y%m%d%H%M"),
        "vars": _REQUEST_VARS, "obtimezone": "utc", "units": "metric",
    }
    url = f"{SYNOPTIC_BASE}?{urllib.parse.urlencode(params)}"
    resp = _http_get_json(url, timeout)
    code = resp.get("SUMMARY", {}).get("RESPONSE_CODE")
    if code != 1:
        msg = resp.get("SUMMARY", {}).get("RESPONSE_MESSAGE", "?")
        log.warning("synoptic response %s: %s", code, msg)
        if code == 2:  # 2 = no data found for the query (empty, not an error)
            return []
        raise RuntimeError(f"synoptic error {code}: {msg}")
    stations = parse_timeseries(resp)
    n_obs = sum(len(s.samples) for s in stations)
    log.info("synoptic RAWS: %d station(s), %d obs over [%s, %s]",
             len(stations), n_obs, start.date(), end.date())
    return stations
