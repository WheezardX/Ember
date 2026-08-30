"""B3 — FIRMS active-fire hotspots (NASA FIRMS area API).

Per-AOI/date-range detections across VIIRS (SNPP / NOAA-20 / NOAA-21) and MODIS,
normalized to a common Hotspot record and stored as immutable GeoJSON point
observations (parquet-per-satellite-day is a later optimization, once the array stack
lands — GeoJSON keeps B3 dependency-light and consistent with perimeter observations).

Needs a free ``FIRMS_MAP_KEY`` (self-service at firms.modaps.eosdis.nasa.gov/api/map_key/;
see [[secrets.py]]); the key is never written into provenance or bundles.
API: firms.modaps.eosdis.nasa.gov/api/area.

Rate limit: 5000 transactions per 10-minute interval per key, and a multi-day request
counts as several transactions (a 7-day pull ~= 7). One incident is a handful of
requests — trivially under the cap — but bulk backfill or a wide --refresh sweep (F1)
should narrow the window / limit sources, or request a limit increase from FIRMS.
"""

from __future__ import annotations

import csv
import io
import json
import urllib.parse
import urllib.request
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from datetime import date as date_cls
from urllib.error import HTTPError

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


# Max day-range per area request. The API nominally allows 10, but a fire-sized bbox
# 400s beyond ~a few days, so we request in chunks (each counts as its own transaction,
# matching FIRMS' documented model) and split further on any 400.
CHUNK_DAYS = 3


def _source_url(key: str, src: str, area: str, ndays: int, start: str) -> str:
    segs = [key, src, area, str(ndays), start]  # quote only path segments, not FIRMS_BASE
    return FIRMS_BASE + "/" + "/".join(urllib.parse.quote(s, safe=",-") for s in segs)


def _fetch_window(
    key: str, src: str, area: str, start: date_cls, ndays: int, timeout: float, depth: int = 0,
) -> list[Hotspot]:
    """One source over [start, start+ndays); adaptively halves the window on HTTP 400."""
    url = _source_url(key, src, area, ndays, start.isoformat())
    try:
        text = _http_get(url, timeout)
    except HTTPError as ex:
        if ex.code == 400 and ndays > 1 and depth < 6:  # window too big -> split by date
            half = ndays // 2
            return (_fetch_window(key, src, area, start, half, timeout, depth + 1)
                    + _fetch_window(key, src, area, start + timedelta(days=half),
                                    ndays - half, timeout, depth + 1))
        raise
    if text.lstrip().lower().startswith(("invalid", "<!doctype", "<html")):
        log.warning("firms %s: non-CSV response (bad key or rate limit?)", src)
        return []
    return parse_firms_csv(text)


def fetch_hotspots(
    bbox: list[float], *, days: int = 10, date: str | None = None,
    sources: tuple[str, ...] = NRT_SOURCES, map_key: str | None = None, timeout: float = 60,
) -> list[Hotspot]:
    """Fetch + dedup hotspots for a lon/lat bbox [W,S,E,N] over ``days``.

    ``date`` (YYYY-MM-DD) sets the range start (historic); omit for the trailing window
    ending today (UTC). The window is requested in <=CHUNK_DAYS chunks and split further
    on a 400, so large windows over a fire-sized bbox still succeed. Overlapping
    detections (same sat/time/rounded-location) are collapsed.
    """
    key = map_key or require_secret(
        MAP_KEY_ENV, hint="register free at firms.modaps.eosdis.nasa.gov")
    w, s, e, n = bbox
    area = f"{w},{s},{e},{n}"
    days = max(1, min(int(days), 10))  # API's overall ceiling
    start0 = (date_cls.fromisoformat(date) if date
              else datetime.now(UTC).date() - timedelta(days=days - 1))

    seen: dict[tuple, Hotspot] = {}
    for src in sources:
        got = 0
        offset = 0
        while offset < days:  # walk the window in safe chunks
            span = min(CHUNK_DAYS, days - offset)
            try:
                hs = _fetch_window(key, src, area, start0 + timedelta(days=offset), span, timeout)
            except Exception as ex:  # noqa: BLE001 — one source/chunk must not sink the rest
                log.warning("firms %s [%+d,+%d]: %s", src, offset, span, ex)
                offset += span
                continue
            for h in hs:
                k = (h.satellite, h.acq_at, round(h.lat, 3), round(h.lon, 3))
                seen.setdefault(k, h)
            got += len(hs)
            offset += span
        log.info("firms %s: %d detections", src, got)

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
