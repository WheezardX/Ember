"""Reference cameras (EPIC_5_PLAN 8i R1): absolute-camera bookmarks, photo EXIF -> bookmark,
photo | render pairs. Engine-free."""

from __future__ import annotations

import json
from fractions import Fraction
from pathlib import Path

import numpy as np
import pytest
from PIL import Image
from PIL.TiffImagePlugin import IFDRational

from ember.dev import refcam
from ember.dev.scenario import LoadedScenario, RenderScenario
from ember.dev.ue import _plan_bookmark


def _spec(bookmarks, orbits=()):
    return {"render_scenario_version": 1, "scenario": {"name": "x", "world": "w"},
            "bookmarks": bookmarks, "captures": [], "orbits": list(orbits)}


def test_absolute_bookmark_validation():
    ok = RenderScenario.model_validate(_spec([
        {"name": "p", "camera_lonlat": [-121.1, 47.2], "yaw_deg": 200, "fov_deg": 69.4}]))
    assert ok.bookmarks[0].absolute
    with pytest.raises(ValueError, match="exactly one"):
        RenderScenario.model_validate(_spec([
            {"name": "p", "camera_xy": [1, 2], "target_frac": [0.5, 0.5]}]))
    with pytest.raises(ValueError, match="no distance_m"):
        RenderScenario.model_validate(_spec([{"name": "p", "camera_xy": [1, 2], "distance_m": 5}]))
    with pytest.raises(ValueError, match="not both"):
        RenderScenario.model_validate(_spec([
            {"name": "p", "camera_xy": [1, 2], "camera_agl_m": 2, "camera_alt_m": 900}]))
    with pytest.raises(ValueError, match="distance_m is required"):
        RenderScenario.model_validate(_spec([{"name": "p", "target_frac": [0.5, 0.5]}]))
    with pytest.raises(ValueError, match="absolute camera"):
        RenderScenario.model_validate(_spec(
            [{"name": "p", "camera_xy": [1, 2]}],
            [{"name": "o", "bookmark": "p"}]))


def test_plan_resolves_lonlat_to_world_crs(tmp_path: Path):
    (tmp_path / "manifest.json").write_text(json.dumps({"crs": "EPSG:32610"}), encoding="utf-8")
    spec = RenderScenario.model_validate(_spec([
        {"name": "p", "camera_lonlat": [-123.0, 47.0], "yaw_deg": 90}]))
    sc = LoadedScenario(path=tmp_path / "x.toml", spec=spec, world_path=tmp_path, replay_path=None)
    d = _plan_bookmark(sc, spec.bookmarks[0])
    # -123 is UTM zone 10's central meridian: easting 500 km exactly
    assert d["camera_xy"][0] == pytest.approx(500000.0, abs=0.01)
    # on the central meridian northing = 0.9996 x the WGS84 meridian arc to 47 deg (5,207,247 m)
    assert d["camera_xy"][1] == pytest.approx(0.9996 * 5207247.0, abs=5.0)
    assert d["camera_agl_m"] == 1.7                       # eye height by default


def test_hfov_from_35mm():
    assert refcam.hfov_from_35mm(26) == pytest.approx(69.39, abs=0.01)   # a phone main camera
    assert refcam.hfov_from_35mm(50, landscape=False) == pytest.approx(26.99, abs=0.01)


def _photo(path: Path, bearing: float | None = 123.5, w: int = 400, h: int = 300) -> None:
    img = Image.fromarray(np.full((h, w, 3), 120, np.uint8))
    exif = img.getexif()
    gps = exif.get_ifd(0x8825)
    gps[1] = "N"
    gps[2] = (IFDRational(47), IFDRational(12), IFDRational(Fraction(30)))
    gps[3] = "W"
    gps[4] = (IFDRational(121), IFDRational(5), IFDRational(0))
    if bearing is not None:
        gps[17] = IFDRational(Fraction(bearing).limit_denominator(100))
    exif.get_ifd(0x8769)[0xA405] = 26
    img.save(path, exif=exif)


def test_bookmark_from_photo(tmp_path: Path):
    p = tmp_path / "ridge.jpg"
    _photo(p)
    r = refcam.bookmark_from_photo(p)
    bm = r["bookmark"]
    assert bm["name"] == "ridge"
    assert bm["camera_lonlat"] == pytest.approx([-121.0833333, 47.2083333], abs=1e-6)
    assert bm["yaw_deg"] == 123.5
    assert bm["fov_deg"] == pytest.approx(69.4, abs=0.05)
    assert r["notes"] == []
    # the printed stanza is a valid bookmark
    import tomllib
    parsed = tomllib.loads(refcam.bookmark_toml(bm))["bookmarks"][0]
    RenderScenario.model_validate(_spec([parsed]))
    q = tmp_path / "nobearing.jpg"
    _photo(q, bearing=None)
    assert any("compass" in n for n in refcam.bookmark_from_photo(q)["notes"])


def test_pair_image_keeps_photo_aspect(tmp_path: Path):
    p = tmp_path / "p.jpg"
    _photo(p, w=400, h=300)                               # 4:3 photo
    r = tmp_path / "r.png"
    Image.fromarray(np.zeros((180, 320, 3), np.uint8)).save(r)   # 16:9 render
    out = refcam.pair_image(p, r, height=240)
    assert out.size == (3 * 320, 240)
