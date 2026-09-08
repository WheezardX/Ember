"""State stream v1: byte-exact writer/reader round trip + frame reconstruction (formats §4)."""

import struct

import numpy as np

from ember.sim.stream import (
    DIRTY_DT,
    MAGIC,
    OVERLAY_DT,
    REJECTED_DT,
    SPOT_DT,
    End,
    Keyframe,
    StreamHeader,
    Tick,
    iter_frames,
    read_stream,
    rle_encode,
    write_stream,
)


def _header():
    return StreamHeader(nx=4, ny=3, cell_mm=30000, t0_unix=1_500_000_000, dt_s=60,
                        keyframe_every=10, flags=0, world_pack_sha256="ab" * 32,
                        model_id="ember-ca", model_version="1.0.0",
                        resources=[("hc-1", "hand_t1")])


def _records():
    n = 12
    phase0 = np.ones(n, np.uint8)
    phase0[0] = 0
    yield Keyframe(0, 0, rle_encode(phase0), rle_encode(np.zeros(n, np.uint8)))
    d = np.zeros(2, DIRTY_DT)
    d["idx"] = [5, 6]
    d["phase"] = [2, 2]
    d["intensity"] = [1, 2]
    d["arrival_s"] = [30, 55]
    s = np.zeros(1, SPOT_DT)
    s["src"], s["dst"], s["launch_s"], s["land_s"], s["landed"], s["ignited"] = 5, 9, 60, 120, 1, 0
    o = np.zeros(1, OVERLAY_DT)
    o["kind"], o["idx"], o["magnitude"], o["resource_idx"] = 1, 2, 0, 0
    r = np.zeros(1, REJECTED_DT)
    r["kind"], r["count"] = 2, 3
    metrics = {"containment_permyriad": 1234, "burning": 2, "burned": 0, "perimeter": 2,
               "structures_lost": -1, "structures_threatened": -1, "cost_cents": 250000,
               "busy_resources": 1, "wind_u_cms": 300, "wind_v_cms": -100, "m10": 65}
    yield Tick(1, 60, 0xDEADBEEF, d, s, o, r, metrics, {"active_cells": 2})
    d2 = np.zeros(1, DIRTY_DT)
    d2["idx"], d2["phase"], d2["intensity"], d2["arrival_s"] = 5, 3, 1, 30
    yield Tick(2, 120, 0xBEEF, d2, np.zeros(0, SPOT_DT), np.zeros(0, OVERLAY_DT),
               np.zeros(0, REJECTED_DT), metrics, {})
    yield End(2, 0xBEEF)


def test_rle():
    runs = rle_encode(np.array([1, 1, 1, 0, 2, 2], np.uint8))
    assert runs["len"].tolist() == [3, 1, 2] and runs["value"].tolist() == [1, 0, 2]
    assert rle_encode(np.zeros(0, np.uint8)).size == 0


def test_round_trip(tmp_path):
    p = write_stream(tmp_path / "t.ess", _header(), _records())
    raw = p.read_bytes()
    assert raw[:8] == MAGIC and struct.unpack_from("<I", raw, 8)[0] == 1
    # header layout: magic(8) version(4) nx ny cell_mm(12) t0(8) dt kf flags(12) sha(32) ...
    assert struct.unpack_from("<IIIqIII", raw, 12) == (4, 3, 30000, 1_500_000_000, 60, 10, 0)
    assert raw[44:76] == bytes.fromhex("ab" * 32)

    h, recs = read_stream(p)
    assert h == _header()
    recs = list(recs)
    assert [type(r).__name__ for r in recs] == ["Keyframe", "Tick", "Tick", "End"]
    kf, t1, t2, end = recs
    ph, it = kf.expand(12)
    assert ph.tolist() == [0] + [1] * 11 and it.sum() == 0
    assert t1.state_hash == 0xDEADBEEF and t1.dirty["idx"].tolist() == [5, 6]
    assert t1.dirty["arrival_s"].tolist() == [30, 55]
    assert t1.spots["dst"][0] == 9 and t1.spots["landed"][0] == 1
    assert t1.overlay["kind"][0] == 1 and t1.rejected["count"][0] == 3
    assert t1.metrics["cost_cents"] == 250000 and t1.metrics["wind_v_cms"] == -100
    assert t1.diag == {"active_cells": 2}
    assert end.ticks == 2 and end.final_hash == 0xBEEF


def test_iter_frames_reconstructs(tmp_path):
    p = write_stream(tmp_path / "t.ess", _header(), _records())
    frames = [f for _, f in iter_frames(p)]
    assert [f.tick for f in frames] == [0, 1, 2]
    f0, f1, f2 = frames
    assert f0.phase.shape == (3, 4) and f0.phase[0, 0] == 0 and f0.arrival_s.min() == -1
    assert f1.phase.ravel()[5] == 2 and f1.intensity.ravel()[6] == 2
    assert f1.arrival_s.ravel()[[5, 6]].tolist() == [30, 55]
    assert f2.phase.ravel()[5] == 3 and f2.phase.ravel()[6] == 2  # burned, still burning
    # every=2: skipped tick 1's spots/overlay ride along with tick 2
    frames2 = [f for _, f in iter_frames(p, every=2)]
    assert [f.tick for f in frames2] == [0, 2]
    assert frames2[1].spots.size == 1 and frames2[1].overlay.size == 1


def test_truncated_stream_tolerated(tmp_path):
    p = write_stream(tmp_path / "t.ess", _header(), _records())
    raw = p.read_bytes()
    cut = p.with_name("cut.ess")
    cut.write_bytes(raw[: len(raw) - 13])  # drop END record
    _, recs = read_stream(cut)
    assert [type(r).__name__ for r in recs] == ["Keyframe", "Tick", "Tick"]
