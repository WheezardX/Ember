"""E1 + B2 — historic perimeter adapter (GeoMAC via NIFC ArcGIS) with cleaning.

Fetches a fire's day-by-day perimeter progression, cleans the (messy) geometry, and
dedups republished duplicates into an ordered, immutable observation series. This is
the primary input to the arrival-time raster (D).

Source: Historic_Geomac_Perimeters_<year> FeatureServer on the NIFC ArcGIS org.
Verified live 2026-08-29: Jolly Mountain 2017 -> 37 perimeters, Aug 12 -> Sep+.
"""

from __future__ import annotations

import hashlib
import json
import urllib.parse
import urllib.request
from dataclasses import dataclass
from datetime import UTC, datetime

from terrain.util.logging import get_logger

log = get_logger(__name__)

ORG = "https://services3.arcgis.com/T4QMspbfLg3qTGWY/arcgis/rest/services"


@dataclass
class Perimeter:
    observed_at: datetime | None
    acres: float
    geom: object  # shapely (Multi)Polygon in EPSG:4326
    name: str
    ghash: str  # content hash of the cleaned geometry


def _parse_dt(v) -> datetime | None:
    if v is None:
        return None
    if isinstance(v, (int, float)):  # epoch ms
        return datetime.fromtimestamp(v / 1000, tz=UTC)
    try:
        return datetime.fromisoformat(str(v).replace("Z", "+00:00"))
    except ValueError:
        return None


def _clean(geom):
    """make_valid + normalize to a MultiPolygon (perimeter data has bowties/dupes)."""
    from shapely.geometry import MultiPolygon
    from shapely.validation import make_valid

    g = make_valid(geom)
    if g.geom_type == "Polygon":
        g = MultiPolygon([g])
    elif g.geom_type == "GeometryCollection":
        parts = []
        for p in g.geoms:
            if p.geom_type == "Polygon":
                parts.append(p)
            elif p.geom_type == "MultiPolygon":
                parts.extend(p.geoms)
        g = MultiPolygon(parts)
    return g if not g.is_empty else None


def fetch_perimeter_series(name: str, year: int, *, timeout: float = 60) -> list[Perimeter]:
    """Fetch + clean + dedup a fire's perimeter progression, ordered by time."""
    from shapely.geometry import shape

    params = {
        "where": f"incidentname LIKE '%{name}%'",
        "outFields": "incidentname,perimeterdatetime,gisacres",
        "returnGeometry": "true",
        "outSR": "4326",
        "orderByFields": "perimeterdatetime",
        "f": "geojson",
    }
    svc = f"{ORG}/Historic_Geomac_Perimeters_{year}/FeatureServer/0/query"
    url = f"{svc}?{urllib.parse.urlencode(params)}"
    with urllib.request.urlopen(url, timeout=timeout) as resp:  # noqa: S310 (fixed https org)
        fc = json.loads(resp.read().decode("utf-8"))

    seen: dict[str, Perimeter] = {}
    raw = 0
    for feat in fc.get("features", []):
        geo = feat.get("geometry")
        if not geo:
            continue
        cleaned = _clean(shape(geo))
        if cleaned is None:
            continue
        raw += 1
        props = feat.get("properties", {})
        ghash = hashlib.sha256(cleaned.wkb).hexdigest()
        obs = _parse_dt(props.get("perimeterdatetime"))
        p = Perimeter(
            observed_at=obs, acres=float(props.get("gisacres") or 0.0),
            geom=cleaned, name=str(props.get("incidentname") or name).strip(), ghash=ghash,
        )
        # dedup identical geometry: keep the earliest observation
        prev = seen.get(ghash)
        if prev is None or (obs and prev.observed_at and obs < prev.observed_at):
            seen[ghash] = p

    _epoch = datetime.min.replace(tzinfo=UTC)
    series = sorted(seen.values(), key=lambda p: p.observed_at or _epoch)
    log.info("geomac %s %d: %d raw perimeters -> %d unique (deduped %d)",
             name, year, raw, len(series), raw - len(series))
    return series
