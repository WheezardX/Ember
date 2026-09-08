"""G2 comparison tooling: side-by-side frames, response curves, memo skeleton."""

import numpy as np
from PIL import Image

from ember.sim.compare import CLOCK_H, GAP, LABEL_H, render_side_by_side
from ember.sim.curves import extract_series, plot_curves, response_table, summarize
from ember.sim.memo import checkpoint_memo_template
from ember.sim.render import HUD_H
from ember.sim.stream import (
    DIRTY_DT,
    OVERLAY_DT,
    REJECTED_DT,
    SPOT_DT,
    End,
    Keyframe,
    StreamHeader,
    Tick,
    rle_encode,
    write_stream,
)
from ember.sim.worldpack import export_synthetic

_M = {"containment_permyriad": 2500, "burning": 0, "burned": 0, "perimeter": 0,
      "structures_lost": -1, "structures_threatened": -1, "cost_cents": 100,
      "busy_resources": 0, "wind_u_cms": 0, "wind_v_cms": 0, "m10": 50}


def _growing_stream(path, nx, ny, ticks, *, cx, cy, step_px=1):
    """A square fire growing `step_px` cells per tick in +x/-x/+y/-y from (cx, cy)."""
    n = nx * ny
    h = StreamHeader(nx=nx, ny=ny, cell_mm=30000, t0_unix=0, dt_s=60, keyframe_every=100,
                     model_id="null", model_version="1.0.0")

    def recs():
        yield Keyframe(0, 0, rle_encode(np.ones(n, np.uint8)), rle_encode(np.zeros(n, np.uint8)))
        lit = set()
        for t in range(1, ticks + 1):
            r = (t - 1) * step_px
            cells = [(x, y) for y in range(cy - r, cy + r + 1) for x in range(cx - r, cx + r + 1)
                     if 0 <= x < nx and 0 <= y < ny and (x, y) not in lit]
            lit.update(cells)
            d = np.zeros(len(cells), DIRTY_DT)
            d["idx"] = [y * nx + x for x, y in cells]
            d["phase"] = 2
            d["intensity"] = 1
            d["arrival_s"] = t * 60
            m = dict(_M, burning=len(lit), containment_permyriad=100 * t)
            yield Tick(t, t * 60, t, d, np.zeros(0, SPOT_DT), np.zeros(0, OVERLAY_DT),
                       np.zeros(0, REJECTED_DT), m, {})
        yield End(ticks, ticks)

    return write_stream(path, h, recs())


def test_side_by_side_grid_and_ended(tmp_path):
    a = export_synthetic("flat", tmp_path / "wa", nx=40, ny=30, name="a")
    b = export_synthetic("ramp", tmp_path / "wb", nx=40, ny=30, name="b")
    sa = _growing_stream(tmp_path / "a.ess", 40, 30, 4, cx=20, cy=15)
    sb = _growing_stream(tmp_path / "b.ess", 40, 30, 2, cx=10, cy=10)
    frames = render_side_by_side([("calm", sa, a), ("windy", sb, b)], tmp_path / "cmp")
    # tick 0 keyframe + 4 ticks; run b ends after its tick 2 and is held greyed
    assert len(frames) == 5
    img = Image.open(frames[0])
    assert img.size == (2 * 40 + GAP, CLOCK_H + LABEL_H + 30 + HUD_H)
    last = np.asarray(Image.open(frames[-1]))
    # right panel greyed: R == G == B across its fire area; left panel has orange fire
    x0 = 40 + GAP
    y0 = CLOCK_H + LABEL_H
    right = last[y0:y0 + 30, x0:x0 + 40]
    body = right[24:30]  # below the "ended" tag box
    assert np.all(body[..., 0] == body[..., 1]) and np.all(body[..., 1] == body[..., 2])
    left = last[y0:y0 + 30, 0:40]
    assert tuple(left[15, 20]) == (255, 190, 40)  # burning, intensity 1
    # "ended" tag is a dark-red box in the right panel's corner
    assert tuple(right[10, 10]) == (90, 30, 30)
    # determinism
    frames2 = render_side_by_side([("calm", sa, a), ("windy", sb, b)], tmp_path / "cmp2")
    assert frames[-1].read_bytes() == frames2[-1].read_bytes()


def test_extract_series_extents(tmp_path):
    s = _growing_stream(tmp_path / "g.ess", 41, 31, 3, cx=20, cy=15)
    ser = extract_series(s)
    assert ser["ignition"] == (20.0, 15.0) and ser["cell_m"] == 30.0
    assert ser["t_s"] == [0, 60, 120, 180]
    assert ser["burning_cells"] == [0, 1, 9, 25] and ser["burned_cells"] == [0, 0, 0, 0]
    assert ser["burned_ha"][-1] == 25 * 0.09
    assert ser["extent_px_m"] == [0.0, 0.0, 30.0, 60.0]
    assert ser["extent_nx_m"] == [0.0, 0.0, 30.0, 60.0]
    assert ser["extent_py_m"][-1] == 60.0 and ser["extent_ny_m"][-1] == 60.0
    assert abs(ser["max_radius_m"][-1] - 60 * 2**0.5) < 1e-9
    assert ser["containment_permyriad"] == [0, 100, 200, 300]
    r = summarize(ser)
    assert r["final_burned_ha"] == 25 * 0.09 and r["max_extent_m"] == 60.0
    # ignition at t=60, end at 180 -> 2 min; radius 84.85 m -> 2545 m/h
    assert abs(r["mean_head_rate_m_per_h"] - 60 * 2**0.5 / (120 / 3600)) < 1e-6
    md = response_table({"grow": ser})
    assert md.startswith("| run |") and "| grow | 2.2 | 60 |" in md


def test_plot_curves_deterministic(tmp_path):
    s1 = extract_series(_growing_stream(tmp_path / "1.ess", 30, 30, 3, cx=15, cy=15))
    s2 = extract_series(_growing_stream(tmp_path / "2.ess", 30, 30, 3, cx=15, cy=15,
                                        step_px=2))
    p1 = plot_curves({"slow": s1, "fast": s2}, tmp_path / "c1.png", title="t")
    p2 = plot_curves({"slow": s1, "fast": s2}, tmp_path / "c2.png", title="t")
    assert p1.exists() and p1.read_bytes() == p2.read_bytes()
    assert Image.open(p1).size[0] > 400


def test_memo_template(tmp_path):
    out = checkpoint_memo_template("CP9", "Test", [("a/b.png", "thing")], "the gate",
                                   tmp_path / "memo.md", findings=["f1"])
    md = out.read_text(encoding="utf-8")
    assert md.startswith("# CP9 — Test\n") and "**Gate:** the gate" in md
    assert "| `a/b.png` | thing |" in md and "- f1" in md and "## Verdict" in md
