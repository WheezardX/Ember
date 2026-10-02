"""External Sources X2.2: the channels sidecar (ADR 0010) - the committed worldcore fixture is what
the writer produces, and PyreCast's codes convert exactly (synthetic data only)."""
from pathlib import Path

import numpy as np
import pytest

from ember.external import channels
from ember.external.composite import write_pack
from ember.external.timeline import Layer
from tests.test_external_composite import _pack, _timeline

REPO = Path(__file__).resolve().parents[1]


def test_worldcore_fixture_is_the_writers_output(tmp_path):
    channels.write_fixture(tmp_path)
    data = REPO / "worldcore" / "tests" / "data"
    for name in ("fire_channels.channels.bin", "fire_channels.expected.json"):
        assert (tmp_path / name).read_bytes() == (data / name).read_bytes(), name


def test_pyrecast_codes_convert_exactly(tmp_path):
    tl = _timeline(2, 3)
    fl = np.zeros((2, 3), np.uint8)
    fl[0, 2] = 5                                   # active crown: 5, taken as m (ASSUMED, ADR 0010)
    fl[0, 1] = 5                                   # surface fire: 5 ft
    sp = np.zeros((2, 3), bool)
    sp[0, 1:] = True
    cr = np.zeros((2, 3), np.uint8)
    cr[0, 2] = 2
    tl.layers["flame_length"] = Layer(fl, sp, "raw", "unstated")
    tl.layers["spread_rate"] = Layer(np.where(sp, 30, 0).astype(np.uint8), sp, "raw", "unstated")
    tl.layers["crown_fire"] = Layer(cr, sp, "raw", "unstated")
    obs = np.full((2, 3), -1, np.int64)
    pack = write_pack(tl, _pack(tmp_path, obs), tmp_path, "variant")
    channels.from_pyrecast(tl, pack)
    got = channels.read(pack)
    fl_m, e = got["flame_length_m"]
    assert e["source"]["unit"].startswith("ft") and e["unit"] == "m"
    assert "ASSUMPTION" in e["source"]["note"]
    assert fl_m[0, 1] == pytest.approx(5 * 0.3048, abs=1e-12)  # ft, exact
    assert fl_m[0, 2] == pytest.approx(5.0, abs=0.0016)         # m, to half a 0.01-ft step
    assert np.isnan(fl_m[0, 0]) and np.isnan(fl_m[1, 1])      # silent where the source is
    assert got["spread_rate_mh"][0][0, 2] == pytest.approx(30 * 18.288, abs=1e-9)
    assert got["crown_class"][0][0, 2] == 2
    assert got["manifest"]["provenance"]["source"] == "synthetic"
