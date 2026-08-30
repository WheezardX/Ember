"""C2 — HRRR gridded weather adapter (offline grid math + gated live fetch)."""

import os
from datetime import UTC, datetime

import pytest

RUN_NETWORK = os.environ.get("TERRAIN_RUN_NETWORK") == "1"


@pytest.mark.geo
def test_target_grid_is_utm_and_covers_bbox():
    from ember.weather.hrrr import target_grid

    bbox = [-121.1, 47.2, -120.8, 47.5]  # Teanaway-ish
    grid, clon, clat = target_grid(bbox, grid_res_m=3000.0)
    assert grid.crs == "EPSG:32610"  # UTM 10N
    assert grid.nx > 0 and grid.ny > 0
    assert clon.shape == (grid.ny, grid.nx)
    # cell centers fall within (a small pad around) the requested bbox
    assert -121.2 < float(clon.min()) and float(clon.max()) < -120.7
    assert 47.1 < float(clat.min()) and float(clat.max()) < 47.6


@pytest.mark.network
@pytest.mark.slow
@pytest.mark.geo
@pytest.mark.skipif(not RUN_NETWORK, reason="downloads real HRRR subsets from AWS")
def test_hrrr_timeline_live(tmp_path):
    import numpy as np

    from ember.weather.hrrr import build_hrrr_timeline, to_manifest, write_grid_sidecar
    from ember.weather.schema import WeatherTimeline

    t0 = datetime(2026, 8, 25, 18, tzinfo=UTC)
    tl = build_hrrr_timeline(
        [-121.1, 47.2, -120.8, 47.5], t0, num_steps=2, step_minutes=60,
        save_dir=tmp_path / "hrrr", grid_res_m=3000.0,
    )
    assert set(tl.variables) == {"wind10_u", "wind10_v", "t2", "rh2"}
    assert tl.grid.crs == "EPSG:32610"
    # at least one step populated with plausible 2 m temperatures (~250-330 K)
    t2 = tl.fields["t2"]
    finite = t2[np.isfinite(t2)]
    assert finite.size > 0 and 250.0 < float(finite.mean()) < 330.0

    npz = tmp_path / "grid.timeline.v0.npz"
    write_grid_sidecar(tl, npz)
    assert npz.exists()
    back = np.load(npz, allow_pickle=True)
    assert back["t2"].shape == (2, tl.grid.ny, tl.grid.nx)

    mani = to_manifest(tl, "hist:test-2026", step_minutes=60,
                       grid_data="weather/grid.timeline.v0.npz")
    assert WeatherTimeline.model_validate_json(mani.model_dump_json()).num_steps == 2
