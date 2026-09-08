"""Crop/scale windows, scale-aware overlays, and the stream probe."""

import json

import numpy as np
from PIL import Image

from ember.sim.compare import CLOCK_H, GAP, LABEL_H, render_side_by_side
from ember.sim.probe import format_probe, probe_stream
from ember.sim.render import HUD_H, LEGEND_H, OUTLINE_RGB, View, render_run, render_static
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

NX, NY = 40, 30
_M = {"containment_permyriad": 0, "burning": 0, "burned": 0, "perimeter": 0,
      "structures_lost": -1, "structures_threatened": -1, "cost_cents": 0,
      "busy_resources": 0, "wind_u_cms": 0, "wind_v_cms": 0, "m10": 0}


def _stream(path):
    n = NX * NY
    h = StreamHeader(nx=NX, ny=NY, cell_mm=30000, t0_unix=0, dt_s=60, keyframe_every=100,
                     model_id="null", model_version="1.0.0")
    c = 15 * NX + 20  # (x=20, y=15)

    def recs():
        yield Keyframe(0, 0, rle_encode(np.ones(n, np.uint8)), rle_encode(np.zeros(n, np.uint8)))
        d = np.zeros(3, DIRTY_DT)
        d["idx"] = [c, c + 1, c - NX]           # (20,15) (21,15) (20,14)
        d["phase"] = [2, 2, 3]
        d["intensity"] = [1, 2, 1]
        d["arrival_s"] = [30, 40, 10]
        o = np.zeros(5, OVERLAY_DT)
        o["kind"] = [1, 2, 3, 4, 5]
        o["idx"] = [c + 3, c + 4, c + 5, c + 6, c + 7]   # x = 23..27, y = 15
        s = np.zeros(2, SPOT_DT)
        s["src"] = [c, c]
        s["dst"] = [c + 2 * NX, c + 3 * NX]
        s["landed"] = [1, 1]
        s["ignited"] = [1, 0]
        yield Tick(1, 60, 11, d, s, o, np.zeros(0, REJECTED_DT), dict(_M, burning=2, burned=1), {})
        yield Tick(2, 120, 22, np.zeros(0, DIRTY_DT), np.zeros(0, SPOT_DT),
                   np.zeros(0, OVERLAY_DT), np.zeros(0, REJECTED_DT),
                   dict(_M, burning=2, burned=1), {})
        yield End(2, 22)

    return write_stream(path, h, recs())


def test_view_math():
    v = View(800, 627, crop=(100, 50, 150, 80), scale=4)
    assert v.crop == (100, 50, 150, 80) and v.scale == 4 and v.f == 1
    assert (v.w, v.h) == (200, 120) and v.cell_px == 4
    assert v.px(100, 50) == (0, 0) and v.px(101, 52) == (4, 8) and v.rect(101, 52) == (4, 8, 7, 11)
    # crop clamps to the grid; scale that overshoots max_width is clamped by f
    v2 = View(800, 627, crop=(-5, 600, 900, 700), scale=3)
    assert v2.crop == (0, 600, 800, 627) and v2.f == 2 and v2.cell_px == 1
    assert View(10, 10).meta()["image"] == [10, 10]


def test_crop_scale_frames_and_overlays(tmp_path):
    pack = export_synthetic("flat", tmp_path, nx=NX, ny=NY)
    st = _stream(tmp_path / "s.ess")
    crop = (10, 5, 30, 20)
    frames = render_run(st, pack, tmp_path / "f", crop=crop, scale=4)
    meta = json.loads((tmp_path / "f" / "render.json").read_text())
    assert meta["crop"] == [10, 5, 30, 20] and meta["scale"] == 4 and meta["cell_px"] == 4
    assert meta["image"] == [80, 60] and meta["frames"] == 3
    img = Image.open(frames[1])
    assert img.size == (80, 60 + HUD_H)
    px = np.asarray(img)

    def cell(x, y, dx=1, dy=1):  # a pixel inside the cell's 4x4 block, off the spot line
        return tuple(int(v) for v in px[(y - 5) * 4 + dy, (x - 10) * 4 + dx])

    # burning cell (20,15): interior orange, 1-px dark outline at the block edge
    assert cell(20, 15) == (255, 190, 40)
    assert cell(20, 15, dx=0, dy=1) == OUTLINE_RGB          # left edge of the front
    assert cell(21, 15, dx=3, dy=1) == OUTLINE_RGB          # right edge (21,15) is burning too
    assert cell(20, 15, dx=3, dy=1) != OUTLINE_RGB           # shared edge is interior
    assert cell(20, 14) == (38, 34, 34)                      # burned
    # overlays: line cyan, retardant magenta, water blue (persistent, solid blocks)
    assert cell(23, 15) == (0, 230, 230) and cell(23, 15, 0, 0) == (0, 230, 230)
    assert cell(24, 15) == (220, 40, 200) and cell(25, 15) == (60, 110, 255)
    # burnout ignition: white with a 1-px dark outline; extinguish light blue
    assert cell(26, 15) == (255, 255, 255) and cell(26, 15, 0, 0) == OUTLINE_RGB
    assert cell(27, 15) == (160, 210, 255)
    # next frame: transient marks gone, persistent ones stay
    px2 = np.asarray(Image.open(frames[2]))
    assert tuple(px2[(15 - 5) * 4 + 2, (26 - 10) * 4 + 2]) != (255, 255, 255)
    assert tuple(px2[(15 - 5) * 4 + 2, (23 - 10) * 4 + 2]) == (0, 230, 230)
    # no outline at scale 1 (cell_px < 3)
    f1 = render_run(st, pack, tmp_path / "g", crop=crop, scale=1)
    p1 = np.asarray(Image.open(f1[1]))
    assert Image.open(f1[1]).size == (20, 15 + HUD_H)
    assert tuple(p1[15 - 5, 21 - 10]) == (255, 110, 0)  # (21,15) intensity 2, off the spot line

    # static + side-by-side honour the window too
    out = render_static(pack, "fbfm40", tmp_path / "st.png", crop=crop, scale=2)
    assert Image.open(out).size == (40, 30 + LEGEND_H)
    sb = render_side_by_side([("a", st, pack), ("b", st, pack)], tmp_path / "sb", crop=crop,
                             scale=2)
    assert Image.open(sb[0]).size == (2 * 40 + GAP, CLOCK_H + LABEL_H + 30 + HUD_H)
    assert json.loads((tmp_path / "sb" / "render.json").read_text())["runs"] == ["a", "b"]


def test_probe(tmp_path):
    st = _stream(tmp_path / "s.ess")
    rows = probe_stream(st)
    assert [r["tick"] for r in rows] == [0, 1, 2]
    assert rows[0]["bbox"] is None and rows[0]["burning"] == 0
    r1 = rows[1]
    assert (r1["burned"], r1["burning"]) == (1, 2)
    assert r1["bbox"] == (20, 14, 22, 16)
    assert r1["spots"] == 2 and r1["spots_ignited"] == 1
    assert r1["overlay"] == {"FuelRemoved": 1, "RetardantApplied": 1, "MoistureBumped": 1,
                             "IgnitionForced": 1, "ExtinguishForced": 1}
    assert r1["state_hash"] == 11 and rows[2]["overlay"] == {}
    txt = format_probe(rows)
    assert "20,14,22,16" in txt and "FuelRemoved=1" in txt
    assert txt.splitlines()[0].startswith("  tick")
    assert len(probe_stream(st, every=2)) == 2
