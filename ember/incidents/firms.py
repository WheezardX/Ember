"""B3 — FIRMS active-fire hotspots (NASA FIRMS area API).

Per-AOI/date-range detections across VIIRS (SNPP / NOAA-20 / NOAA-21) and MODIS,
normalized to a common Hotspot record and stored as immutable GeoJSON point
observations (parquet-per-satellite-day is a later optimization, once the array stack
lands — GeoJSON keeps B3 dependency-light and consistent with perimeter observations).

Needs a free ``FIRMS_MAP_KEY`` (see [[secrets.py]]); the key is never written into
provenance or bundles. API: firms.modaps.eosdis.nasa.gov/api/area.
"""

from __future__ import annotations

import csv
import io
import json
import urllib.parse
import urllib.request
from dataclasses import dataclass
from datetime import UTC, datetime

from terrain.util.logging import get_logger

from ember.incidents.secrets import require_secret

log = get_logger(__name__)

FIRMS_BASE = "https://firms.modaps.eosdis.nasa.gov/api/area/csv"
MAP_KEY_ENV = "FIRMS_MAP_KEY"

# Near-real-time sources for live fires. Historic fires use the "_SP" (standard
# processing) archive variants; callers pass their own list for pre-NRT windows.
NRT_SOURCES = ("VIIRS_SNPP_NRT", "VIIRS_NOAA20_NRT", "VIIRS_NOAA21_NRT", "MODIS_NRT")
SP_SOURCES = ("VIIRS_SNPP_SP", "MODIS_SP")


@dataclass(frozen=True)
class Hotspot:
    acq_at: datetime  # UTC
    lon: float
    lat: float
    frp: float | None  # fire radiative power (MW)
    confidence: str  # raw source value ('l'/'n'/'h' for VIIRS, 0-100 for MODIS)
    satellite: str
    daynight: str  # 'D' / 'N'


def _acq_datetime(date_s: str, time_s: str) -> datetime | None:
    """FIRMS acq_date 'YYYY-MM-DD' + acq_time 'HHMM' (or 'HMM') -> UTC datetime."""
    try:
        hhmm = str(time_s).zfill(4)
        return datetime(
            *(int(x) for x in date_s.split("-")),
            int(hhmm[:2]), int(hhmm[2:]), tzinfo=UTC,
        )
    except (ValueError, TypeError):
        return None


def _f(v) -> float | None:
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def _http_get(url: str, timeout: float) -> str:
    """Fetch a URL as text. Isolated so tests can mock the network at one point."""
    req = urllib.request.Request(url, headers={"User-Agent": "ember-firms/0"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:  # noqa: S310 (fixed https host)
        return resp.read().decode("utf-8")


def parse_firms_csv(text: str) -> list[Hotspot]:
    """Parse a FIRMS area-CSV response into Hotspots (handles VIIRS + MODIS columns)."""
    out: list[Hotspot] = []
    for row in csv.DictReader(io.StringIO(text)):
        lat, lon = _f(row.get("latitude")), _f(row.get("longitude"))
        acq = _acq_datetime(row.get("acq_date", ""), row.get("acq_time", ""))
        if lat is None or lon is None or acq is None:
            continue
        out.append(Hotspot(
            acq_at=acq, lon=lon, lat=lat, frp=_f(row.get("frp")),
            confidence=str(row.get("confidence", "")).strip(),
            satellite=str(row.get("satellite", "")).strip(),
            daynight=str(row.get("daynight", "")).strip(),
        ))
    return out


def fetch_hotspots(
    bbox: list[float], *, days: int = 10, date: str | None = None,
    sources: tuple[str, ...] = NRT_SOURCES, map_key: str | None = None, timeout: float = 60,
) -> list[Hotspot]:
    """Fetch + dedup hotspots for a lon/lat bbox [W,S,E,N] over the last ``days``.

    ``date`` (YYYY-MM-DD) sets the range start (historic); omit for the trailing window.
    Overlapping detections (same sat/time/rounded-location) are collapsed.
    """
    key = map_key or require_secret(
        MAP_KEY_ENV, hint="register free at firms.modaps.eosdis.nasa.gov")
    w, s, e, n = bbox
    area = f"{w},{s},{e},{n}"
    days = max(1, min(int(days), 10))  # API caps the range at 10 days

    seen: dict[tuple, Hotspot] = {}
    for src in sources:
        segs = [key, src, area, str(days)] + ([date] if date else [])
        # quote only the path segments — NOT the scheme/host in FIRMS_BASE
        url = FIRMS_BASE + "/" + "/".join(urllib.parse.quote(s, safe=",-") for s in segs)
        try:
            text = _http_get(url, timeout)
        except Exception as ex:  # noqa: BLE001 — one source failing must not sink the rest
            log.warning("firms %s fetch failed: %s", src, ex)
            continue
        if text.lstrip().lower().startswith(("invalid", "<!doctype", "<html")):
            log.warning("firms %s: non-CSV response (bad key or rate limit?)", src)
            continue
        hs = parse_firms_csv(text)
        for h in hs:
            k = (h.satellite, h.acq_at, round(h.lat, 3), round(h.lon, 3))
            seen.setdefault(k, h)
        log.info("firms %s: %d detections", src, len(hs))

    hotspots = sorted(seen.values(), key=lambda h: h.acq_at)
    log.info("firms: %d unique hotspots over %d src(s), %d-day window",
             len(hotspots), len(sources), days)
    return hotspots


def hotspots_geojson(hotspots: list[Hotspot]) -> dict:
    """FeatureCollection of point detections (immutable observation payload)."""
    return {
        "type": "FeatureCollection",
        "features": [
            {"type": "Feature",
             "geometry": {"type": "Point", "coordinates": [h.lon, h.lat]},
             "properties": {"acq_at": h.acq_at.isoformat(), "frp": h.frp,
                            "confidence": h.confidence, "satellite": h.satellite,
                            "daynight": h.daynight}}
            for h in hotspots
        ],
    }


def write_hotspots(hotspots: list[Hotspot], path) -> None:
    from pathlib import Path

    Path(path).write_text(json.dumps(hotspots_geojson(hotspots)), encoding="utf-8")
