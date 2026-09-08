"""World pack v1: synthetic kit round-trip (formats.md §1), stable pack_hash, class mapping."""

import json

import numpy as np
import pytest

from ember.sim.worldpack import (
    INT32_MIN,
    LAYER_SPECS,
    SYNTH_KINDS,
    export_synthetic,
    fuel_class,
    fuel_class_array,
    load_worldpack,
)


@pytest.mark.parametrize("kind", SYNTH_KINDS)
def test_synthetic_round_trip(tmp_path, kind):
    pack = export_synthetic(kind, tmp_path, nx=40, ny=30, fuel=102)
    assert pack.name == f"synth-{kind}.ewp"
    m = json.loads((pack / "world.json").read_text(encoding="utf-8"))
    assert m["format"] == "ember-world-pack" and m["version"] == 1
    assert m["grid"] == {"nx": 40, "ny": 30, "cell_size_m": 30.0, "crs": "LOCAL",
                         "origin_x": 0.0, "origin_y": 0.0}
    assert m["source"]["synthetic"]["kind"] == kind
    sizes = {"i32": 4, "u8": 1, "u16": 2}
    for name, meta in m["layers"].items():
        tag, unit = LAYER_SPECS[name]
        assert meta["dtype"] == tag and meta["unit"] == unit
        assert (pack / meta["file"]).stat().st_size == 40 * 30 * sizes[tag]
        assert set(meta["stats"]) == {"min", "max", "valid", "nodata"}
    wp = load_worldpack(pack)
    assert wp.nx == 40 and wp.ny == 30 and wp.t0_unix == 0
    assert wp.layer("elevation_cm").dtype == np.dtype("<i4")
    assert wp.layer("fbfm40").shape == (30, 40)
    assert wp.weather is None


def test_synthetic_shapes_and_hash_stable(tmp_path):
    a = export_synthetic("ramp", tmp_path / "a", nx=50, ny=20, slope_pct=40)
    b = export_synthetic("ramp", tmp_path / "b", nx=50, ny=20, slope_pct=40)
    ma = json.loads((a / "world.json").read_text())
    mb = json.loads((b / "world.json").read_text())
    assert ma["pack_hash"] == mb["pack_hash"]
    wp = load_worldpack(a)
    e = np.asarray(wp.layer("elevation_cm"))
    assert e.min() > INT32_MIN
    # ramp rises along +x at 40 %: 30 m cells -> 12 m = 1200 cm per column
    assert np.all(np.diff(e, axis=1) == 1200)
    assert np.all(np.diff(e, axis=0) == 0)

    c = export_synthetic("ramp", tmp_path / "c", nx=50, ny=20, slope_pct=10)
    assert json.loads((c / "world.json").read_text())["pack_hash"] != ma["pack_hash"]


def test_barrier_checker_ridge(tmp_path):
    bar = load_worldpack(export_synthetic("barrier", tmp_path, nx=21, ny=5, barrier_x=7))
    fb = np.asarray(bar.layer("fbfm40"))
    assert np.all(fb[:, 7] == 99) and np.all(fb[:, 6] == 102)

    chk = load_worldpack(export_synthetic("checker", tmp_path, nx=8, ny=8, fuel_a=102,
                                          fuel_b=183, block=4))
    fb = np.asarray(chk.layer("fbfm40"))
    assert fb[0, 0] == 102 and fb[0, 4] == 183 and fb[4, 0] == 183 and fb[4, 4] == 102
    # canopy only under timber
    cc = np.asarray(chk.layer("cc_pct"))
    assert cc[0, 4] == 60 and cc[0, 0] == 0

    rid = load_worldpack(export_synthetic("ridge", tmp_path, nx=5, ny=11, height_m=100))
    e = np.asarray(rid.layer("elevation_cm"))[:, 0]
    assert e.argmax() == 5 and e[0] == e[-1] == 100000 and e[5] == 110000


def test_fuel_classes():
    assert [fuel_class(c) for c in (91, 99, 101, 109, 121, 124, 141, 149, 161, 165, 181,
                                    189, 201, 204, 0, 110)] == [
        "NB", "NB", "GR", "GR", "GS", "GS", "SH", "SH", "TU", "TU", "TL", "TL", "SB", "SB",
        "NB", "NB"]
    arr = fuel_class_array(np.array([0, 102, 122, 145, 162, 183, 202, 250]))
    assert arr.tolist() == [0, 1, 2, 3, 4, 5, 6, 0]


def test_unknown_kind_and_kwargs(tmp_path):
    with pytest.raises(ValueError):
        export_synthetic("volcano", tmp_path)
    with pytest.raises(TypeError):
        export_synthetic("flat", tmp_path, bogus=1)
