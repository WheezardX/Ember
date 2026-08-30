"""B1 + B2 (live) — WFIGS incident + perimeter adapters (NIFC ArcGIS).

The live counterpart to the historic GeoMAC adapter ([[geomac.py]]), sharing the
cleaning stage in [[arcgis.py]]. WFIGS keeps the CURRENT (latest) perimeter per
fire, not a day-by-day progression: a single live assemble yields the fire's current
footprint, and the progression accumulates as `--refresh` (F1, Phase 3) re-polls and
appends immutable observations over the fire's life.

Verified live 2026-08-30 against the NIFC WFIGS layers (IRWIN ids are brace-wrapped
GUIDs; incidents in ``*_Current``/``*_YearToDate``, perimeters likewise + all-history).
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime

from terrain.util.logging import get_logger

from ember.incidents.arcgis import (
    Perimeter,
    arcgis_query,
    features_to_perimeters,
    parse_dt,
)

log = get_logger(__name__)

_INCIDENT_SERVICES = ("WFIGS_Incident_Locations_Current", "WFIGS_Incident_Locations_YearToDate")
_PERIMETER_SERVICES = (
    "WFIGS_Interagency_Perimeters_Current",
    "WFIGS_Interagency_Perimeters_YearToDate",
    "WFIGS_Interagency_Perimeters",
)


def _f(v) -> float | None:
    try:
        return float(v) if v is not None else None
    except (TypeError, ValueError):
        return None


def normalize_irwin(irwin: str) -> str:
    """Normalize any IRWIN spelling to the brace-wrapped uppercase GUID WFIGS stores."""
    return "{" + irwin.strip().strip("{}").upper() + "}"


@dataclass
class IncidentMeta:
    """Normalized WFIGS incident metadata (B1)."""

    irwin: str
    name: str
    discovered_at: datetime | None
    contained_at: datetime | None
    size_acres: float | None
    final_acres: float | None
    percent_contained: float | None
    cause: str | None
    state: str | None


def fetch_incident(irwin: str, *, timeout: float = 60) -> IncidentMeta:
    """B1 — resolve an IRWIN id to a populated incident record (Current, then YTD)."""
    guid = normalize_irwin(irwin)
    fields = (
        "IrwinID,IncidentName,FireDiscoveryDateTime,ContainmentDateTime,"
        "IncidentSize,FinalAcres,PercentContained,FireCause,POOState"
    )
    for svc in _INCIDENT_SERVICES:
        fc = arcgis_query(
            svc,
            {"where": f"IrwinID = '{guid}'", "outFields": fields,
             "returnGeometry": "false", "f": "json"},
            timeout=timeout,
        )
        feats = fc.get("features", [])
        if feats:
            a = feats[0]["attributes"]
            log.info("wfigs incident %s -> %r (%s)", guid, a.get("IncidentName"), svc)
            return IncidentMeta(
                irwin=guid, name=str(a.get("IncidentName") or "").strip(),
                discovered_at=parse_dt(a.get("FireDiscoveryDateTime")),
                contained_at=parse_dt(a.get("ContainmentDateTime")),
                size_acres=_f(a.get("IncidentSize")), final_acres=_f(a.get("FinalAcres")),
                percent_contained=_f(a.get("PercentContained")),
                cause=a.get("FireCause"), state=a.get("POOState"),
            )
    raise RuntimeError(f"no WFIGS incident found for IRWIN {guid}")


def fetch_current_perimeters(
    irwin: str, fallback_name: str = "", *, timeout: float = 60
) -> list[Perimeter]:
    """B2 (live) — current perimeter snapshot(s) for an IRWIN, cleaned + deduped.

    Returns ``[]`` (with a warning) when the fire is located but not yet mapped —
    the caller decides how to degrade.
    """
    guid = normalize_irwin(irwin)
    for svc in _PERIMETER_SERVICES:
        fc = arcgis_query(
            svc,
            {"where": f"poly_IRWINID = '{guid}'",
             "outFields": "poly_IncidentName,poly_PolygonDateTime,poly_DateCurrent,poly_GISAcres",
             "returnGeometry": "true", "outSR": "4326",
             "orderByFields": "poly_PolygonDateTime", "f": "geojson"},
            timeout=timeout,
        )
        series = features_to_perimeters(fc.get("features", []), fallback_name=fallback_name or guid)
        if series:
            log.info("wfigs perimeters %s: %d snapshot(s) (%s)", guid, len(series), svc)
            return series
    log.warning("wfigs: no perimeter mapped yet for IRWIN %s", guid)
    return []
