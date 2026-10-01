import json

import numpy as np
from pyproj import Transformer

from ember.incidents.ir_heat import build


def test_heat_grid_classes_and_order(tmp_path):
    # 10 x 10 grid of 30 m cells, top-left at a UTM 10N point near Kachess
    ox, oy = 630000.0, 5250000.0
    (tmp_path / "w.ewp").mkdir()
    replay = tmp_path / "r.replay.json"
    replay.write_text(json.dumps({"world": {"pack_relative": "w.ewp", "grid": {
        "nx": 10, "ny": 10, "cell_size_m": 30.0, "crs": "EPSG:32610",
        "origin_x": ox, "origin_y": oy}}}))
    to_ll = Transformer.from_crs("EPSG:32610", "EPSG:4326", always_xy=True)

    def box(x0, y0, x1, y1):
        ring = [to_ll.transform(x, y)
                for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1), (x0, y0))]
        return {"type": "Polygon", "coordinates": [ring]}

    obs = tmp_path / "obs"
    obs.mkdir()
    # scattered over the top half, intense over its left quarter (intense wins), one isolated point
    fc = {"type": "FeatureCollection", "features": [
        {"properties": {"class": "Scattered Heat", "acquired_utc": "2026-08-11T05:10:00+00:00"},
         "geometry": box(ox, oy - 150, ox + 300, oy)},
        {"properties": {"class": "Intense Heat", "acquired_utc": "2026-08-11T05:10:00+00:00"},
         "geometry": box(ox, oy - 150, ox + 60, oy)},
        {"properties": {"class": "Isolated Heat point",
                        "acquired_utc": "2026-08-11T05:10:00+00:00"},
         "geometry": {"type": "Point", "coordinates": list(to_ll.transform(ox + 255, oy - 255))}}]}
    (obs / "20260811T0510.geojson").write_text(json.dumps(fc))
    early = {"type": "FeatureCollection", "features": [
        {"properties": {"class": "Scattered Heat", "acquired_utc": "2026-08-09T04:32:00+00:00"},
         "geometry": box(ox, oy - 30, ox + 30, oy)}]}
    (obs / "zz_early.geojson").write_text(json.dumps(early))  # sorts last by name, first by time

    out = build(obs, replay)
    meta = json.loads((tmp_path / "w.heat.json").read_text())
    assert [f["acquired_utc"][:10] for f in meta["flights"]] == ["2026-08-09", "2026-08-11"]
    g = np.fromfile(out, np.uint8).reshape(2, 10, 10)
    assert g[0].sum() == 2 and g[0, 0, 0] == 2
    assert g[1, 0, 0] == 3 and g[1, 0, 1] == 3 and g[1, 0, 2] == 2 and g[1, 4, 9] == 2
    assert g[1, 8, 8] == 1 and g[1, 6, 0] == 0
