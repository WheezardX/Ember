"""Disposable renderer smoke: frame dims, determinism, static layer renders, HUD present."""

import numpy as np
from PIL import Image

from ember.sim.render import (
    HUD_H,
    LEGEND_H,
    STATIC_LAYERS,
    downsample_factor,
    render_all_static,
    render_run,
    stats_markdown,
)
from ember.sim.stream import (
    DIRTY_DT,
    OVERLAY_DT,
    SPOT_DT,
    End,
    Keyframe,
    StreamHeader,
    Tick,
    rle_encode,
    write_stream,
)
from ember.sim.stream import REJECTED_DT as RJ
from ember.sim.worldpack import export_synthetic, load_worldpack


def _stream(path, nx, ny):
    n = nx * ny
    h = StreamHeader(nx=nx, ny=ny, cell_mm=30000, t0_unix=0, dt_s=60, keyframe_every=100,
                     model_id="null", model_version="1.0.0")
    m = {"containment_permyriad": 5000, "burning": 3, "burned": 1, "perimeter": 3,
         "structures_lost": -1, "structures_threatened": -1, "cost_cents": 0,
         "busy_resources": 0, "wind_u_cms": 400, "wind_v_cms": 300, "m10": 80}

    def recs():
        yield Keyframe(0, 0, rle_encode(np.ones(n, np.uint8)), rle_encode(np.zeros(n, np.uint8)))
        c = (ny // 2) * nx + nx // 2
        d = np.zeros(4, DIRTY_DT)
        d["idx"] = [c, c + 1, c + nx, c - 1]
        d["phase"] = [2, 2, 2, 3]
        d["intensity"] = [1, 1, 2, 3]
        d["arrival_s"] = [0, 30, 40, 50]
        s = np.zeros(1, SPOT_DT)
        s["src"], s["dst"], s["landed"], s["ignited"] = c, c + 3 * nx + 3, 1, 1
        o = np.zeros(3, OVERLAY_DT)
        o["kind"] = [1, 2, 4]
        o["idx"] = [c + 5, c + 6, c + 7]
        yield Tick(1, 60, 1, d, s, o, np.zeros(0, RJ), m, {})
        yield Tick(2, 120, 2, np.zeros(0, DIRTY_DT), np.zeros(0, SPOT_DT),
                   np.zeros(0, OVERLAY_DT), np.zeros(0, RJ), m, {})
        yield End(2, 2)

    return write_stream(path, h, recs())


def test_render_run_deterministic(tmp_path):
    pack = export_synthetic("checker", tmp_path, nx=32, ny=24, fuel_a=102, fuel_b=183, block=8)
    stream = _stream(tmp_path / "r.ess", 32, 24)
    f1 = render_run(stream, pack, tmp_path / "f1")
    f2 = render_run(stream, load_worldpack(pack), tmp_path / "f2")
    assert len(f1) == 3
    for a, b in zip(f1, f2, strict=True):
        assert a.read_bytes() == b.read_bytes()
    img = Image.open(f1[1])
    assert img.size == (32, 24 + HUD_H) and img.mode == "RGB"
    px = np.asarray(img)
    c = (12, 16)
    assert tuple(px[c[0], c[1] - 1]) == (38, 34, 34)      # burned (c-1)
    assert tuple(px[c[0], c[1] + 5]) == (0, 230, 230)     # cyan line persists
    assert tuple(px[c[0], c[1] + 7]) == (255, 255, 255)   # transient forced ignition
    img2 = np.asarray(Image.open(f1[2]))
    assert tuple(img2[c[0], c[1] + 5]) == (0, 230, 230)   # still there next frame
    assert tuple(img2[c[0], c[1] + 7]) != (255, 255, 255)  # transient gone


def test_downsample_and_static(tmp_path):
    assert downsample_factor(1600) == 1 and downsample_factor(1601) == 2
    pack = export_synthetic("ridge", tmp_path, nx=3300, ny=20, fuel=183)
    outs = render_all_static(pack, tmp_path / "static")
    names = {p.stem for p in outs}
    assert names == {la for la in STATIC_LAYERS if la in load_worldpack(pack).layers}
    img = Image.open(tmp_path / "static" / "elevation_cm.png")
    assert img.size == (1100, 7 + LEGEND_H)  # 20 rows / factor 3 -> 7
    md = (tmp_path / "static" / "stats.md").read_text()
    assert "| elevation_cm | i32 | cm |" in md
    assert stats_markdown(load_worldpack(pack)).startswith("# World pack stats")
