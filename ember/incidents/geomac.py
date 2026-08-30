"""E1 + B2 — historic perimeter adapter (GeoMAC via NIFC ArcGIS) with cleaning.

Fetches a fire's day-by-day perimeter progression, cleans the (messy) geometry, and
dedups republished duplicates into an ordered, immutable observation series. This is
the primary input to the arrival-time raster (D). The cleaning stage is shared with
the live WFIGS adapter — see [[arcgis.py]].

Source: Historic_Geomac_Perimeters_<year> FeatureServer on the NIFC ArcGIS org.
Verified live 2026-08-29: Jolly Mountain 2017 -> 37 perimeters, Aug 12 -> Sep+.
"""

from __future__ import annotations

from terrain.util.logging import get_logger

from ember.incidents.arcgis import Perimeter, arcgis_query, features_to_perimeters

log = get_logger(__name__)

__all__ = ["Perimeter", "fetch_perimeter_series"]


def fetch_perimeter_series(name: str, year: int, *, timeout: float = 60) -> list[Perimeter]:
    """Fetch + clean + dedup a fire's perimeter progression, ordered by time."""
    fc = arcgis_query(
        f"Historic_Geomac_Perimeters_{year}",
        {
            "where": f"incidentname LIKE '%{name}%'",
            "outFields": "incidentname,perimeterdatetime,gisacres",
            "returnGeometry": "true", "outSR": "4326",
            "orderByFields": "perimeterdatetime", "f": "geojson",
        },
        timeout=timeout,
    )
    series = features_to_perimeters(fc.get("features", []), fallback_name=name)
    log.info("geomac %s %d: %d perimeters after cleaning", name, year, len(series))
    return series
