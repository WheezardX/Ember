"""Three Queens 2026: NIROPS nightly IR -> perimeter series + heat classes -> arrival raster.

The 2026 analog of Jolly 2017's GeoMAC perimeter series. Downloads the KMZ per flight (small),
parses Heat Perimeter / Intense / Scattered / Isolated heat, writes per-flight GeoJSON and a
summary CSV, then builds an arrival raster with the same perimeter-interp-v1 algorithm.
"""
import csv, io, json, re, sys, urllib.request, zipfile
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
import xml.etree.ElementTree as ET

sys.path.insert(0, r"C:\Projects\Ember")
from shapely.geometry import MultiPolygon, Polygon, Point, mapping
from shapely.ops import transform as shp_transform, unary_union
from pyproj import Transformer
from terrain.work.netmeter import NetMeter
from ember.incidents.arrival import build_incident_grid, build_arrival_raster

INC = Path(r"C:\Projects\Ember\store\incidents\dfa477f2-305c-49ca-b82e-2af072ab55f8")
OBS = INC / "observations" / "ir_perimeters"
RAW = INC / "observations" / "ir_kmz"
OUT = INC / "derived_ir"
for d in (OBS, RAW, OUT):
    d.mkdir(parents=True, exist_ok=True)
K = "{http://www.opengis.net/kml/2.2}"
PDT = timezone(timedelta(hours=-7))
to_utm = Transformer.from_crs("EPSG:4326", "EPSG:32610", always_xy=True)


def area_ac(g):
    return shp_transform(lambda x, y, z=None: to_utm.transform(x, y), g).area / 4046.856


def coords(txt):
    # NIROPS writes "x,y,z,x,y,z,..." as one comma run (no spaces); KML proper uses spaces
    v = [float(s) for s in re.split(r"[,\s]+", txt.strip()) if s]
    step = 3 if len(v) % 3 == 0 else 2
    return [(v[i], v[i + 1]) for i in range(0, len(v) - 1, step)]


def polys(pm):
    out = []
    for poly in pm.iter(K + "Polygon"):
        outer = poly.find(f"{K}outerBoundaryIs/{K}LinearRing/{K}coordinates")
        if outer is None or len(coords(outer.text)) < 4:
            continue  # degenerate sliver in the source
        holes = [c for c in (coords(h.text) for h in poly.findall(f"{K}innerBoundaryIs/{K}LinearRing/{K}coordinates")) if len(c) >= 4]
        p = Polygon(coords(outer.text), holes).buffer(0)
        if not p.is_empty:
            out.append(p)
    return out


def points(pm):
    return [Point(coords(c.text)[0]) for pt in pm.iter(K + "Point") for c in pt.iter(K + "coordinates")]


from shapely.geometry import shape
_final = shape(json.load(open(next((INC / "observations" / "perimeters").glob("*.geojson"))))["features"][0]["geometry"])
TQ_MASK = _final.buffer(0.012)  # ~1 km around the Sep 8 WFIGS perimeter (King / Goat lie farther out)
urls = [p["url"] for f in (INC / "observations" / "ir").glob("nirops_*.json")
        for p in json.load(open(f))["products"] if p["kind"] == "kmz"]
rows, series = [], []
with NetMeter(r"C:\Projects\Terrain\store", "three_queens_2026", "nirops-ir-kmz"):
    for u in urls:
        name = u.rsplit("/", 1)[1].replace("%20", "_")
        fp = RAW / name
        if not fp.exists():
            fp.write_bytes(urllib.request.urlopen(urllib.request.Request(u, headers={"User-Agent": "ember-nirops/0"}), timeout=120).read())
        z = zipfile.ZipFile(io.BytesIO(fp.read_bytes()))
        kml = z.read([n for n in z.namelist() if n.endswith(".kml")][0])
        root = ET.fromstring(kml)
        txt = kml.decode("utf-8", "replace")
        d = re.search(r"Acquisition Date:\s*</b>\s*(\d{8})", txt)
        t = re.search(r"Acquisition Time:\s*</b>\s*(\d{3,4})", txt)
        if not d:
            print("no date", name); continue
        hhmm = (t.group(1) if t else "2200").zfill(4)
        ymd = d.group(1)
        if ymd == "20260714":
            ymd = "20260814"  # source typo: file sits in IR/20260815/, maps 3,602 ac (Aug 14 update: 3,600)
        at = datetime.strptime(ymd + hhmm, "%Y%m%d%H%M").replace(tzinfo=PDT).astimezone(timezone.utc)
        parts = {}
        for pm in root.iter(K + "Placemark"):
            n = (pm.findtext(K + "name") or "").strip()
            parts.setdefault(n, []).append(pm)
        # some nights' files carry King / Goat too: keep only parts touching Three Queens
        def tq_only(gs):
            return [g for g in gs if g.intersects(TQ_MASK)]
        geo = {n: unary_union(tq_only(sum((polys(pm) for pm in pms), []))) for n, pms in parts.items()}
        perim = geo.get("Heat Perimeter")
        if perim is None or perim.is_empty:
            print("no perimeter", name); continue
        iso = [p for p in sum((points(pm) for pm in parts.get("Isolated Heat", [])), []) if p.intersects(TQ_MASK)]
        r = {"flight": name, "acquired_utc": at.isoformat(), "acquired_pdt": at.astimezone(PDT).strftime("%Y-%m-%d %H:%M"),
             "perimeter_ac": round(area_ac(perim)), "intense_ac": round(area_ac(geo["Intense Heat"])) if not geo.get("Intense Heat", Polygon()).is_empty else 0,
             "scattered_ac": round(area_ac(geo["Scattered Heat"])) if not geo.get("Scattered Heat", Polygon()).is_empty else 0,
             "isolated_n": len(iso)}
        rows.append(r)
        fc = {"type": "FeatureCollection", "features": [
            {"type": "Feature", "properties": {"class": k, "acquired_utc": r["acquired_utc"]}, "geometry": mapping(v)}
            for k, v in geo.items() if not v.is_empty] + [
            {"type": "Feature", "properties": {"class": "Isolated Heat point", "acquired_utc": r["acquired_utc"]}, "geometry": mapping(p)} for p in iso]}
        (OBS / (at.strftime("%Y%m%dT%H%M") + ".geojson")).write_text(json.dumps(fc), encoding="utf-8")

        @dataclass
        class P:
            observed_at: datetime
            geom: object
        series.append(P(at, perim if isinstance(perim, (Polygon, MultiPolygon)) else perim.buffer(0)))

rows.sort(key=lambda r: r["acquired_utc"])
series.sort(key=lambda p: p.observed_at)
with open(OUT / "ir_flights.csv", "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(rows[0]))
    w.writeheader(); w.writerows(rows)
for r in rows:
    print(r["acquired_pdt"], r["perimeter_ac"], r["intense_ac"], r["scattered_ac"], r["isolated_n"])

# Holdover (Jul 15 - Aug 6): no IR flights. The incident maps give the size (79 ac Jul 22,
# 80 Jul 25, 88 Jul 29, unchanged on the Aug 6 map) and VIIRS puts every detection of those weeks
# in one tight cluster at the summit. Synthetic perimeters: circles of the mapped area on that
# cluster - extent approximate, timing from the record. The last one sits just before the first
# Aug 6 detection (09:15Z): the fire woke up on Aug 6, a day before the first IR flight.
UTC = timezone.utc
HOLD_C = Point(-121.2625, 47.4152)            # mean of the 16 July VIIRS detections
to_ll = Transformer.from_crs("EPSG:32610", "EPSG:4326", always_xy=True)
def circle_ac(ac):
    x, y = to_utm.transform(HOLD_C.x, HOLD_C.y)
    c = Point(x, y).buffer((ac * 4046.856 / 3.14159265) ** 0.5, 32)
    return shp_transform(lambda a, b, z=None: to_ll.transform(a, b), c)
P = type(series[0])
hold = [P(datetime(2026, 7, 16, 4, 28, tzinfo=UTC), circle_ac(5)),     # discovered (IRWIN)
        P(datetime(2026, 7, 22, 20, 30, tzinfo=UTC), circle_ac(79)),
        P(datetime(2026, 7, 25, 17, 30, tzinfo=UTC), circle_ac(80)),
        P(datetime(2026, 7, 29, 17, 0, tzinfo=UTC), circle_ac(88)),
        P(datetime(2026, 8, 6, 9, 0, tzinfo=UTC), circle_ac(88))]
# Aug 21 run: no IR on Aug 20 / 21, and the Aug 21 morning update (3,849 ac, an Aug 20 flight
# we do not have) says the fire had not grown since Aug 19. Hold the Aug 19 22:11 PDT perimeter
# until noon Aug 21 so the interpolation puts the run in the afternoon and night, as reported
# ("strengthened night-time winds"), not spread back across the gap.
aug19 = next(p for p in series if p.observed_at == datetime(2026, 8, 20, 5, 11, tzinfo=timezone.utc))
series.append(P(datetime(2026, 8, 21, 19, 0, tzinfo=timezone.utc), aug19.geom))
series = sorted(hold + series, key=lambda p: p.observed_at)

# VIIRS / MODIS detections for the whole season (hotspot-assist: a detection inside the burned
# extent means the cell was burning by then - pulls interpolated arrival earlier)
@dataclass
class HS:
    lon: float
    lat: float
    acq_at: datetime
hs_path = INC / "observations" / "hotspots" / "firms_season_20260714_20260921.geojson"
hotspots = []
if hs_path.exists():
    for ft in json.load(open(hs_path))["features"]:
        lon, lat = ft["geometry"]["coordinates"]
        hotspots.append(HS(lon, lat, datetime.fromisoformat(ft["properties"]["acq_at"])))

# cumulative footprint (IR heat perimeters can shrink where heat died out): union forward
cum, cum_series = None, []
for p in series:
    cum = p.geom if cum is None else unary_union([cum, p.geom])
    cum_series.append(type(p)(p.observed_at, cum))
grid = build_incident_grid(cum_series, buffer_km=2.0, resolution_m=30.0)
stats = build_arrival_raster(cum_series, grid, OUT, resolution_m=30.0, hotspots=hotspots)
stats["holdover"] = "synthetic circles 5/79/80/88/88 ac at the July VIIRS cluster (Jul 16 -> Aug 6 09Z)"
stats["hotspots_used"] = len(hotspots)
print(json.dumps(stats, indent=1))
(OUT / "arrival_stats.json").write_text(json.dumps(stats, indent=1))
