"""Shared NIFC/WFIGS ArcGIS FeatureServer access + perimeter cleaning (B2).

The geometry-cleaning stage is source-agnostic: GeoMAC (historic, [[geomac.py]]) and
WFIGS (live, [[wfigs.py]]) both feed messy perimeter data through it. Kept
dependency-light (stdlib urllib) like the rest of the incident adapters — WFIGS
layers are standard FeatureServer endpoints, so no Esri SDK is needed.
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

# NIFC ArcGIS Online org hosting GeoMAC history + the live WFIGS layers.
ORG = "https://services3.arcgis.com/T4QMspbfLg3qTGWY/arcgis/rest/services"


@dataclass
class Perimeter:
    """One cleaned fire perimeter observation, in EPSG:4326."""

    observed_at: datetime | None
    acres: float
    geom: object  # shapely (Multi)Polygon
    name: str
    ghash: str  # content hash of the cleaned geometry


def arcgis_query(service: str, params: dict, *, layer: int = 0, timeout: float = 60) -> dict:
    """Query a FeatureServer layer, returning parsed JSON/GeoJSON.

    `service` is a service name under the NIFC org (e.g. ``WFIGS_Incident_Locations_Current``)
    or a full ``.../query`` URL. Raises on ArcGIS error payloads (which come back HTTP 200).
    """
    if service.startswith("http"):
        base = service
    else:
        base = f"{ORG}/{service}/FeatureServer/{layer}/query"
    url = f"{base}?{urllib.parse.urlencode(params)}"
    req = urllib.request.Request(url, headers={"User-Agent": "ember-incidents/0"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:  # noqa: S310 (fixed https org)
        data = json.loads(resp.read().decode("utf-8"))
    if isinstance(data, dict) and "error" in data:
        raise RuntimeError(f"ArcGIS error from {service}: {data['error'].get('message')}")
    return data


def parse_dt(v) -> datetime | None:
    """ArcGIS timestamps: epoch-ms ints or ISO strings; be tolerant of nulls/junk."""
    if v is None:
        return None
    if isinstance(v, (int, float)):  # epoch ms
        return datetime.fromtimestamp(v / 1000, tz=UTC)
    try:
        return datetime.fromisoformat(str(v).replace("Z", "+00:00"))
    except ValueError:
        return None


def clean_geom(geom):
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


def geohash(geom) -> str:
    """Content hash of a cleaned geometry, for dedup + immutable-observation naming."""
    return hashlib.sha256(geom.wkb).hexdigest()


# Logical field -> candidate property keys (lowercased), tried in order. Covers both
# GeoMAC (bare names) and WFIGS (poly_* / attr_* names) so one cleaner serves both.
_NAME_KEYS = ("incidentname", "poly_incidentname", "attr_incidentname")
_DATE_KEYS = ("perimeterdatetime", "poly_polygondatetime", "poly_datecurrent", "poly_createdate")
_ACRES_KEYS = ("gisacres", "poly_gisacres", "poly_acres_autocalc", "attr_calculatedacres")


def _first(props: dict, keys: tuple[str, ...]):
    for k in keys:
        v = props.get(k)
        if v is not None:
            return v
    return None


def features_to_perimeters(features: list[dict], fallback_name: str) -> list[Perimeter]:
    """GeoJSON features -> cleaned, geometry-deduped perimeters (earliest obs kept).

    Shared by GeoMAC and WFIGS: both request ``f=geojson`` so a feature is
    ``{geometry, properties}``. Field names differ (GeoMAC ``perimeterdatetime`` vs
    WFIGS ``poly_PolygonDateTime``); ``_first`` resolves each logical field across the
    known aliases, case-insensitively.
    """
    from shapely.geometry import shape

    seen: dict[str, Perimeter] = {}
    raw = 0
    for feat in features:
        geo = feat.get("geometry")
        if not geo:
            continue
        cleaned = clean_geom(shape(geo))
        if cleaned is None:
            continue
        raw += 1
        props = {k.lower(): v for k, v in (feat.get("properties") or {}).items()}
        gh = geohash(cleaned)
        obs = parse_dt(_first(props, _DATE_KEYS))
        p = Perimeter(
            observed_at=obs, acres=float(_first(props, _ACRES_KEYS) or 0.0),
            geom=cleaned, name=str(_first(props, _NAME_KEYS) or fallback_name).strip(), ghash=gh,
        )
        prev = seen.get(gh)  # dedup identical geometry: keep the earliest observation
        if prev is None or (obs and prev.observed_at and obs < prev.observed_at):
            seen[gh] = p

    _epoch = datetime.min.replace(tzinfo=UTC)
    series = sorted(seen.values(), key=lambda p: p.observed_at or _epoch)
    log.info("perimeter cleaning: %d raw -> %d unique (deduped %d)",
             raw, len(series), raw - len(series))
    return series
