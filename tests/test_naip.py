"""NAIP colour probe (EPIC_5_PLAN 8i R2): colour maths and the pixel -> ground mapping. Offline."""

from __future__ import annotations

import math

import numpy as np
import pytest

from ember.dev import naip


def test_srgb_to_lab_reference_values():
    lab = naip.srgb_to_lab(np.array([[255, 255, 255], [0, 0, 0], [119, 119, 119], [255, 0, 0]]))
    assert lab[0] == pytest.approx([100.0, 0.0, 0.0], abs=0.05)
    assert lab[1] == pytest.approx([0.0, 0.0, 0.0], abs=0.05)
    assert lab[2][0] == pytest.approx(50.0, abs=0.3)          # sRGB 119 grey is L* ~50
    assert lab[3] == pytest.approx([53.24, 80.09, 67.20], abs=0.1)   # sRGB red (D65)


def test_linear_round_trip():
    v = np.arange(256, dtype=float)
    assert naip._to_srgb8(naip._to_linear(v)) == pytest.approx(v, abs=1e-6)


def _facts(bearing: float) -> dict:
    return {"camera": {"world_x_m": 1000.0, "world_y_m": 5000.0, "agl_m": 100.0,
                       "fov_deg": 90.0, "bearing_deg": bearing}}


def test_ground_xy_north_up():
    # 90 deg horizontal FOV at 100 m: the frame spans 200 m, 2 m per pixel over 100 px
    gx, gy = naip.ground_xy(_facts(0.0), 100, 50)
    assert gx[0, 0] == pytest.approx(1000.0 - 99.0)            # left edge pixel centre = west
    assert gy[0, 0] == pytest.approx(5000.0 + 49.0)            # top row = north
    assert gx[-1, -1] == pytest.approx(1000.0 + 99.0)
    assert gy[-1, -1] == pytest.approx(5000.0 - 49.0)


def test_ground_xy_rotated():
    # camera looking east (image up = east): the top row lies east of the centre
    gx, gy = naip.ground_xy(_facts(90.0), 100, 100)
    top_mid = (gx[0, 50] + gx[0, 49]) / 2, (gy[0, 50] + gy[0, 49]) / 2
    assert top_mid[0] == pytest.approx(1000.0 + 99.0, abs=1e-6)
    assert top_mid[1] == pytest.approx(5000.0, abs=1e-6)
    # image right = south
    assert gy[50, -1] < 5000.0 - 90.0 and abs(gx[50, -1] - 1000.0) < 2.0
    assert math.isclose(float(np.hypot(gx[0, 0] - gx[0, 1], gy[0, 0] - gy[0, 1])), 2.0)


def test_groups_are_disjoint():
    seen = set()
    for codes in naip.GROUPS.values():
        assert not (seen & set(codes))
        seen |= set(codes)
