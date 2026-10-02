"""External Sources X1.2: the PyreCast adapter on a SYNTHETIC run directory shaped like the real
files (float32 hours, nodata 0; hourly granules in tars; one granule missing). The real PyreCast
data is internal-only and never becomes a fixture (plan rule 1)."""
import io
import tarfile

import numpy as np
import pytest

rasterio = pytest.importorskip("rasterio")
from rasterio.transform import from_origin  # noqa: E402

from ember.external.pyrecast import load_run  # noqa: E402

TF = from_origin(599828.56, 5284344.5, 30.0, 30.0)


def _tif_bytes(a: np.ndarray, nodata) -> bytes:
    from rasterio.io import MemoryFile

    with MemoryFile() as mf:
        with mf.open(driver="GTiff", width=a.shape[1], height=a.shape[0], count=1, dtype=a.dtype,
                     crs="EPSG:32610", transform=TF, nodata=nodata) as ds:
            ds.write(a, 1)
        return mf.read()


def _tar(path, members: dict[str, bytes]) -> None:
    with tarfile.open(path, "w") as t:
        for name, data in members.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            t.addfile(info, io.BytesIO(data))


@pytest.fixture
def run_dir(tmp_path):
    d = tmp_path / "wa-test" / "20260820_051100"
    d.mkdir(parents=True)
    arr = np.zeros((3, 4), np.float32)
    arr[0, 0] = 0.5     # before the 06:00 granule -> sampled at 06:00
    arr[0, 1] = 1.2     # -> 07:00 (missing granule: silent)
    arr[0, 2] = 2.79    # 07:58 -> 08:00
    (d / "50.tif").write_bytes(_tif_bytes(arr, 0.0))
    hsb = np.zeros((3, 4), np.int16)
    hsb[2, :2] = 40     # the starting perimeter
    _tar(d / "50_hours-since-burned.tar",
         {"hours-since-burned_20260820_051100.tif": _tif_bytes(hsb, 0)})
    fl = {}
    for ts, v in (("20260820_051100", 1), ("20260820_060000", 9), ("20260820_080000", 21)):
        g = np.full((3, 4), v, np.uint8)
        fl[f"flame-length_{ts}.tif"] = _tif_bytes(g, 0)
    _tar(d / "50_flame-length.tar", fl)     # 07:00 missing
    return d


def test_arrival_and_start_perimeter(run_dir):
    tl = load_run(run_dir, 50)
    assert tl.t_ref_utc == "2026-08-20T05:11:00Z"
    assert tl.arrival_s[0, 0] == pytest.approx(1800.0, abs=1e-3)
    assert np.isnan(tl.arrival_s[1, 1])
    assert tl.burned.sum() == 3
    assert tl.burned_before.sum() == 2 and tl.burned_before[2, 0]
    assert tl.provenance["licence"].startswith("pyrecast-internal")


def test_layers_sampled_at_arrival_hour_never_interpolated(run_dir):
    tl = load_run(run_dir, 50)
    fl = tl.layers["flame_length"]
    assert fl.unit == "raw"
    assert fl.speaks[0, 0] and fl.values[0, 0] == 9      # 05:41 -> the 06:00 granule
    assert not fl.speaks[0, 1]                           # 06:23 -> 07:00, purged: silent
    assert fl.speaks[0, 2] and fl.values[0, 2] == 21     # 07:58 -> 08:00
    rep = tl.provenance["granules"]["flame_length"]
    assert rep["new_growth_cells_on_missing_granules"] == 1
