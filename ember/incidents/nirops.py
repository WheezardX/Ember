"""B4 — NIROPS infrared products (best-effort).

National Infrared Operations flies fires at night and posts daily IR products (heat
perimeter / intense / scattered) to the public wildfire file server. There is no API:
products are organized operationally under
``incident_specific_maps/<GACC>/<year>_Incidents_<State>/<year>_<IncidentCamel>/IR/<YYYYMMDD>/``
as ``*_IR.kmz`` / ``*_IR.gdb.zip`` / ``*_IR_ShapeFileOutputs.zip``.

This adapter DISCOVERS the product listing (dates + URLs + kinds) by walking the
directory index; it does not download or parse the geometry (that would need fiona/gdal
and is a later enhancement feeding hotspot-assist). Per plan decision D5 it is strictly
best-effort: any failure — moved server, unmatched incident, no IR flown — degrades to
an empty listing and NEVER blocks a package.

Directory layout verified live 2026-08-30 (e.g. 2026 Goat fire, pacific_nw).
"""

from __future__ import annotations

import re
import urllib.parse
import urllib.request
from dataclasses import dataclass
from datetime import UTC, datetime

from terrain.util.logging import get_logger

log = get_logger(__name__)

BASE = "https://ftp.wildfire.gov/public/incident_specific_maps"
GACCS = (
    "alaska", "calif_n", "calif_s", "california_statewide", "eastern", "great_basin",
    "n_rockies", "pacific_nw", "rocky_mtn", "southern", "southwest",
)


@dataclass(frozen=True)
class IRProduct:
    flight_date: datetime | None  # UTC midnight of the product's YYYYMMDD folder
    kind: str  # 'kmz' | 'geodatabase' | 'shapefile' | 'pdf' | 'other'
    filename: str
    url: str


def _slug(s: str) -> str:
    return re.sub(r"[^a-z0-9]", "", s.lower())


def list_links(url: str, *, timeout: float = 30) -> list[str]:
    """Absolute hrefs from a directory-index page (parent/sort links dropped)."""
    req = urllib.request.Request(url, headers={"User-Agent": "ember-nirops/0"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:  # noqa: S310 (fixed https host)
        html = resp.read().decode("utf-8", "replace")
    out = []
    for h in re.findall(r'href="([^"?#]+)"', html):
        if h in ("../", "/"):
            continue
        j = urllib.parse.urljoin(url, h)
        if j != url and "/public/incident_specific_maps" in j:
            out.append(j)
    return sorted(set(out))


def _classify(filename: str) -> str:
    fl = filename.lower()
    if fl.endswith(".kmz"):
        return "kmz"
    if fl.endswith(".gdb.zip"):
        return "geodatabase"
    if "shapefile" in fl and fl.endswith(".zip"):
        return "shapefile"
    if fl.endswith(".pdf"):
        return "pdf"
    return "other"


def _date_from_name(name: str) -> datetime | None:
    m = re.search(r"(20\d{2})(\d{2})(\d{2})", name)
    if not m:
        return None
    try:
        return datetime(int(m[1]), int(m[2]), int(m[3]), tzinfo=UTC)
    except ValueError:
        return None


def _find_incident_dir(
    name: str, year: int, gaccs: tuple[str, ...], *, base: str, timeout: float
) -> str | None:
    """Locate ``<year>_<IncidentCamel>/`` by slug match across GACCs (best-effort)."""
    want = _slug(name)
    for gacc in gaccs:
        gurl = f"{base}/{gacc}/"
        try:
            groups = list_links(gurl)
        except Exception as ex:  # noqa: BLE001
            log.debug("nirops: skip gacc %s (%s)", gacc, ex)
            continue
        # groups are either year/state group dirs or incident dirs directly; scan both
        for grp in groups:
            if not grp.endswith("/"):
                continue
            tail = grp.rstrip("/").rsplit("/", 1)[-1]
            # direct incident dir at gacc level?
            if str(year) in tail and want and want in _slug(tail):
                return grp
            # else descend one level into year/state grouping dirs
            if str(year) in tail or "incidents" in tail.lower():
                try:
                    for inc in list_links(grp):
                        itail = inc.rstrip("/").rsplit("/", 1)[-1]
                        if (inc.endswith("/") and str(year) in itail
                                and want and want in _slug(itail)):
                            return inc
                except Exception:  # noqa: BLE001, S110
                    continue
    return None


def discover_ir_products(
    name: str, year: int, *, gacc: str | None = None, base: str = BASE, timeout: float = 30,
) -> list[IRProduct]:
    """Best-effort: discover IR products for a fire. Returns [] on any failure."""
    try:
        gaccs = (gacc,) if gacc else GACCS
        inc_dir = _find_incident_dir(name, year, gaccs, base=base, timeout=timeout)
        if not inc_dir:
            log.info("nirops: no incident dir found for %r %d (best-effort, absent)", name, year)
            return []
        ir_dir = next(
            (u for u in list_links(inc_dir)
             if u.rstrip("/").rsplit("/", 1)[-1].lower() == "ir"), None)
        if not ir_dir:
            log.info("nirops: incident %r has no IR/ dir (absent)", name)
            return []
        products: list[IRProduct] = []
        for day in list_links(ir_dir):
            if not day.endswith("/"):
                continue
            for f in list_links(day):
                if f.endswith("/"):
                    continue
                fn = f.rsplit("/", 1)[-1]
                products.append(IRProduct(
                    flight_date=_date_from_name(fn) or _date_from_name(day),
                    kind=_classify(fn), filename=fn, url=f,
                ))
        products.sort(key=lambda p: (p.flight_date or datetime.min.replace(tzinfo=UTC), p.filename))
        log.info("nirops: %r %d -> %d IR product(s)", name, year, len(products))
        return products
    except Exception as ex:  # noqa: BLE001 — best-effort; never block a package
        log.warning("nirops discovery failed for %r %d: %s (absent)", name, year, ex)
        return []
