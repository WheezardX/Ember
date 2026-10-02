"""External Sources X1.4: the fidelity sidecar passes a faithful replay and FAILS a tampered one
(a missing cell, a late cell, a cell the source never burned). Synthetic pack, timeline, stream."""
import json

import numpy as np

from ember.external import fidelity
from ember.external.composite import write_pack
from ember.sim.stream import (
    DIRTY_DT,
    METRIC_FIELDS,
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
from tests.test_external_composite import _pack, _timeline


def _replay(tmp_path, arrival: np.ndarray, ticks: int, name: str):
    """A playback stream of `arrival` (s, -1 = never), hourly, 6 h residence."""
    ny, nx = arrival.shape
    a = arrival.ravel()
    n = a.size
    h = StreamHeader(nx=nx, ny=ny, cell_mm=30000, t0_unix=0, dt_s=3600, keyframe_every=24,
                     model_id="test", model_version="1")
    phase = np.ones(n, np.uint8)
    inten = np.zeros(n, np.uint8)
    recs = [Keyframe(0, 0, rle_encode(phase), rle_encode(inten))]
    for tick in range(1, ticks + 1):
        ts = tick * 3600
        new = phase.copy()
        lit = (a >= 0) & (a <= ts)
        new[lit & (phase == 1)] = 2
        new[lit & (a + 6 * 3600 <= ts)] = 3
        idx = np.nonzero(new != phase)[0].astype(np.uint32)
        d = np.zeros(len(idx), DIRTY_DT)
        d["idx"], d["phase"], d["arrival_s"] = idx, new[idx], a[idx]
        phase = new
        recs.append(Tick(tick, ts, tick, d, np.zeros(0, SPOT_DT), np.zeros(0, OVERLAY_DT),
                         np.zeros(0, REJECTED_DT), dict.fromkeys(METRIC_FIELDS, 0), {}))
    recs.append(End(ticks, 0))
    ess = tmp_path / f"{name}.ess"
    write_stream(ess, h, recs)
    rp = tmp_path / f"{name}.replay.json"
    rp.write_text(json.dumps({"format": "ember-replay", "version": 1, "stream": ess.name}))
    return rp


def _setup(tmp_path):
    obs = np.full((2, 3), -1, np.int64)
    obs[0, 0] = 100
    tl = _timeline(2, 3)
    pack = write_pack(tl, _pack(tmp_path, obs), tmp_path, "variant")
    wp_arr = np.fromfile(pack / "arrival_s.bin", "<i4").reshape(2, 3).astype(np.int64)
    return tl, pack, wp_arr


def test_faithful_replay_passes(tmp_path):
    tl, pack, a = _setup(tmp_path)
    res = fidelity.evaluate(tl, pack, _replay(tmp_path, a, 14, "ok"))
    assert res["ok"], res
    assert res["share_within_one_tick"] == 1.0 and res["exact_arrival_share"] == 1.0
    fc = res["forecast"]                       # the readable summary of what the source said
    assert fc["arrival_h"]["first"] == 0.17 and fc["arrival_h"]["last"] == 0.5   # 10 / 30 min
    assert fc["growth_acres_since_run"]["24h"] == fc["growth_acres_since_run"]["end"]
    assert fc["starting_area_acres"] > 0


def test_tampered_replays_fail(tmp_path):
    tl, pack, a = _setup(tmp_path)
    R = json.loads((pack / "world.json").read_text())["arrival"]["t_ref_s"]
    missing = a.copy()
    missing[0, 2] = -1                         # the forecast's growth cell never burns
    late = a.copy()
    late[0, 2] += 3 * 3600                     # shown three ticks late
    extra = a.copy()
    extra[1, 2] = R + 7200                     # a cell the source never burned
    for name, arr, key in (("missing", missing, "missing"), ("late", late, "late"),
                           ("extra", extra, "unsupported")):
        res = fidelity.evaluate(tl, pack, _replay(tmp_path, arr, 14, name))
        assert not res["ok"], name
        assert res[key] >= 1, (name, res)
