"""External Sources X1.1: the fire timeline round-trips and summarises (synthetic data only - the
real PyreCast files are internal-only and never become fixtures)."""
import numpy as np

from ember.external.timeline import FireTimeline, Grid, Layer, read_timeline


def _timeline() -> FireTimeline:
    ny, nx = 4, 5
    arr = np.full((ny, nx), np.nan)
    arr[1, 1:4] = [3600.0, 7200.0, 36000.0]
    before = np.zeros((ny, nx), bool)
    before[2, 2] = True
    fl = Layer(np.where(np.isfinite(arr), 7, 0).astype(np.uint8), np.isfinite(arr), "raw",
               "unstated", "flame length code")
    return FireTimeline(Grid(nx, ny, 30.0, 1000.0, 2000.0, "EPSG:32610"), "2026-08-20T05:11:00Z",
                        arr, before, np.ones((ny, nx), bool),
                        {"source": "synthetic", "member": "p50", "licence": "test"},
                        {"flame_length": fl})


def test_round_trip(tmp_path):
    tl = _timeline()
    tl.write(tmp_path / "tl")
    back = read_timeline(tmp_path / "tl")
    assert back.grid == tl.grid
    assert back.t_ref_utc == tl.t_ref_utc
    np.testing.assert_array_equal(back.arrival_s, tl.arrival_s)
    np.testing.assert_array_equal(back.burned_before, tl.burned_before)
    assert back.layers["flame_length"].unit == "raw"
    np.testing.assert_array_equal(back.layers["flame_length"].values,
                                  tl.layers["flame_length"].values)


def test_summary_and_growth():
    tl = _timeline()
    s = tl.summary()
    assert s["new_growth_cells"] == 3
    assert s["burned_before_cells"] == 1
    assert s["arrival_max_h"] == 10.0
    assert s["layers"]["flame_length"]["coverage_of_new_growth"] == 1.0
    cell_ac = 900.0 / 4046.8564224
    assert tl.growth_acres_by(2.0) == round(2 * cell_ac, 1)
