"""State stream v1 (`.ess`) reader + byte-exact writer, and replay reader (formats.md §4-5).

The C++ core writes streams; Python reads them for rendering/QA. The writer here exists for
tests and Python-side tools and must stay byte-identical to the C++ writer.
"""

from __future__ import annotations

import json
import struct
from collections.abc import Iterable, Iterator
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, BinaryIO

import numpy as np

from ember.sim import STREAM_VERSION

MAGIC = b"EMBRSTRM"
KIND_TICK, KIND_KEYFRAME, KIND_END = 1, 2, 3
DELTA_KINDS = {1: "FuelRemoved", 2: "RetardantApplied", 3: "MoistureBumped",
               4: "IgnitionForced", 5: "ExtinguishForced"}

DIRTY_DT = np.dtype([("idx", "<u4"), ("phase", "u1"), ("intensity", "u1"), ("arrival_s", "<i4")])
SPOT_DT = np.dtype([("src", "<u4"), ("dst", "<u4"), ("launch_s", "<i4"), ("land_s", "<i4"),
                    ("landed", "u1"), ("ignited", "u1")])
OVERLAY_DT = np.dtype([("kind", "u1"), ("idx", "<u4"), ("magnitude", "<u2"),
                       ("resource_idx", "<u2")])
REJECTED_DT = np.dtype([("kind", "u1"), ("count", "<u4")])
RUN_DT = np.dtype([("len", "<u4"), ("value", "u1")])
METRIC_FIELDS = ("containment_permyriad", "burning", "burned", "perimeter", "structures_lost",
                 "structures_threatened", "cost_cents", "busy_resources", "wind_u_cms",
                 "wind_v_cms", "m10")
_METRICS_FMT = "<iIIIiiqIiii"
assert len(METRIC_FIELDS) == 11
for _dt in (DIRTY_DT, SPOT_DT, OVERLAY_DT, REJECTED_DT, RUN_DT):
    assert _dt.itemsize in (5, 9, 10, 18)  # packed, no padding


@dataclass
class StreamHeader:
    nx: int
    ny: int
    cell_mm: int
    t0_unix: int
    dt_s: int
    keyframe_every: int
    flags: int = 0
    world_pack_sha256: str = "0" * 64
    model_id: str = ""
    model_version: str = ""
    interface_version: str = "1.0.0"
    resources: list[tuple[str, str]] = field(default_factory=list)
    version: int = STREAM_VERSION

    @property
    def n_cells(self) -> int:
        return self.nx * self.ny


@dataclass
class Keyframe:
    tick: int
    t_s: int
    phase_runs: np.ndarray      # RUN_DT
    intensity_runs: np.ndarray  # RUN_DT

    def expand(self, n_cells: int) -> tuple[np.ndarray, np.ndarray]:
        return _expand(self.phase_runs, n_cells), _expand(self.intensity_runs, n_cells)


@dataclass
class Tick:
    tick: int
    t_s: int
    state_hash: int
    dirty: np.ndarray     # DIRTY_DT
    spots: np.ndarray     # SPOT_DT
    overlay: np.ndarray   # OVERLAY_DT
    rejected: np.ndarray  # REJECTED_DT
    metrics: dict[str, int]
    diag: dict[str, int]


@dataclass
class End:
    ticks: int
    final_hash: int


Record = Keyframe | Tick | End


def rle_encode(values: np.ndarray) -> np.ndarray:
    """u8 array -> RUN_DT runs (index order)."""
    v = np.asarray(values, dtype=np.uint8).ravel()
    if v.size == 0:
        return np.zeros(0, RUN_DT)
    change = np.flatnonzero(v[1:] != v[:-1]) + 1
    starts = np.concatenate([[0], change])
    ends = np.concatenate([change, [v.size]])
    runs = np.zeros(starts.size, RUN_DT)
    runs["len"] = ends - starts
    runs["value"] = v[starts]
    return runs


def _expand(runs: np.ndarray, n_cells: int) -> np.ndarray:
    out = np.repeat(runs["value"].astype(np.uint8), runs["len"].astype(np.int64))
    if out.size != n_cells:
        raise ValueError(f"keyframe RLE expands to {out.size} cells, expected {n_cells}")
    return out


# ---- writing --------------------------------------------------------------------------- #
def _wstr(f: BinaryIO, s: str) -> None:
    b = s.encode("utf-8")
    f.write(struct.pack("<I", len(b)))
    f.write(b)


def _warr(f: BinaryIO, arr: np.ndarray, dt: np.dtype) -> None:
    a = np.ascontiguousarray(np.asarray(arr, dtype=dt))
    f.write(struct.pack("<I", a.size))
    f.write(a.tobytes())


def write_header(f: BinaryIO, h: StreamHeader) -> None:
    f.write(MAGIC)
    f.write(struct.pack("<I", h.version))
    f.write(struct.pack("<IIIqIII", h.nx, h.ny, h.cell_mm, h.t0_unix, h.dt_s,
                        h.keyframe_every, h.flags))
    f.write(bytes.fromhex(h.world_pack_sha256))
    _wstr(f, h.model_id)
    _wstr(f, h.model_version)
    _wstr(f, h.interface_version)
    f.write(struct.pack("<I", len(h.resources)))
    for rid, rtype in h.resources:
        _wstr(f, rid)
        _wstr(f, rtype)


def write_record(f: BinaryIO, r: Record) -> None:
    if isinstance(r, Keyframe):
        f.write(struct.pack("<BIi", KIND_KEYFRAME, r.tick, r.t_s))
        _warr(f, r.phase_runs, RUN_DT)
        _warr(f, r.intensity_runs, RUN_DT)
    elif isinstance(r, Tick):
        f.write(struct.pack("<BIiQ", KIND_TICK, r.tick, r.t_s, r.state_hash))
        _warr(f, r.dirty, DIRTY_DT)
        _warr(f, r.spots, SPOT_DT)
        _warr(f, r.overlay, OVERLAY_DT)
        _warr(f, r.rejected, REJECTED_DT)
        f.write(struct.pack(_METRICS_FMT, *(int(r.metrics.get(k, 0)) for k in METRIC_FIELDS)))
        f.write(struct.pack("<I", len(r.diag)))
        for k, v in r.diag.items():
            _wstr(f, k)
            f.write(struct.pack("<q", int(v)))
    elif isinstance(r, End):
        f.write(struct.pack("<BIQ", KIND_END, r.ticks, r.final_hash))
    else:
        raise TypeError(type(r))


def write_stream(path: str | Path, header: StreamHeader, records: Iterable[Record]) -> Path:
    path = Path(path)
    with open(path, "wb") as f:
        write_header(f, header)
        for r in records:
            write_record(f, r)
    return path


# ---- reading --------------------------------------------------------------------------- #
class _Reader:
    def __init__(self, f: BinaryIO):
        self.f = f

    def raw(self, n: int) -> bytes:
        b = self.f.read(n)
        if len(b) != n:
            raise EOFError("truncated state stream")
        return b

    def unpack(self, fmt: str) -> tuple:
        return struct.unpack(fmt, self.raw(struct.calcsize(fmt)))

    def s(self) -> str:
        (n,) = self.unpack("<I")
        return self.raw(n).decode("utf-8")

    def arr(self, dt: np.dtype) -> np.ndarray:
        (n,) = self.unpack("<I")
        return np.frombuffer(self.raw(n * dt.itemsize), dtype=dt).copy()


def read_header(r: _Reader) -> StreamHeader:
    if r.raw(8) != MAGIC:
        raise ValueError("not an ember state stream (bad magic)")
    (version,) = r.unpack("<I")
    if version > STREAM_VERSION:
        raise ValueError(f"state stream version {version} newer than supported {STREAM_VERSION}")
    nx, ny, cell_mm, t0, dt_s, kf, flags = r.unpack("<IIIqIII")
    sha = r.raw(32).hex()
    model_id, model_version, iface = r.s(), r.s(), r.s()
    (nres,) = r.unpack("<I")
    resources = [(r.s(), r.s()) for _ in range(nres)]
    return StreamHeader(nx, ny, cell_mm, t0, dt_s, kf, flags, sha, model_id, model_version,
                        iface, resources, version)


def _records(r: _Reader) -> Iterator[Record]:
    while True:
        b = r.f.read(1)
        if not b:
            return  # tolerate a stream cut before END (crash mid-run)
        kind = b[0]
        if kind == KIND_KEYFRAME:
            tick, t_s = r.unpack("<Ii")
            yield Keyframe(tick, t_s, r.arr(RUN_DT), r.arr(RUN_DT))
        elif kind == KIND_TICK:
            tick, t_s, h = r.unpack("<IiQ")
            dirty, spots, overlay, rejected = (r.arr(DIRTY_DT), r.arr(SPOT_DT),
                                               r.arr(OVERLAY_DT), r.arr(REJECTED_DT))
            metrics = dict(zip(METRIC_FIELDS, r.unpack(_METRICS_FMT), strict=True))
            (nd,) = r.unpack("<I")
            diag = {}
            for _ in range(nd):
                k = r.s()
                (v,) = r.unpack("<q")
                diag[k] = v
            yield Tick(tick, t_s, h, dirty, spots, overlay, rejected, metrics, diag)
        elif kind == KIND_END:
            ticks, h = r.unpack("<IQ")
            yield End(ticks, h)
            return
        else:
            raise ValueError(f"unknown record kind {kind}")


def read_stream(path: str | Path) -> tuple[StreamHeader, Iterator[Record]]:
    """Header + a lazy record iterator (the file stays open until exhausted)."""
    f = open(path, "rb")  # noqa: SIM115 — lifetime is the iterator's
    r = _Reader(f)
    header = read_header(r)

    def gen() -> Iterator[Record]:
        try:
            yield from _records(r)
        finally:
            f.close()

    return header, gen()


@dataclass
class Frame:
    tick: int
    t_s: int
    phase: np.ndarray       # (ny, nx) u8
    intensity: np.ndarray   # (ny, nx) u8
    arrival_s: np.ndarray   # (ny, nx) i32
    metrics: dict[str, int]
    spots: np.ndarray
    overlay: np.ndarray
    diag: dict[str, int]
    state_hash: int


def iter_frames(path: str | Path, every: int = 1) -> Iterator[tuple[StreamHeader, Frame]]:
    """Reconstruct full state per tick (keyframes + dirty lists); yield every `every`-th tick.

    The tick-0 keyframe is yielded as a frame too (tick 0, before any tick record), so a run
    of T ticks yields the initial state plus T//every ticks.
    """
    header, records = read_stream(path)
    n = header.n_cells
    phase = np.zeros(n, np.uint8)
    intensity = np.zeros(n, np.uint8)
    arrival = np.full(n, -1, np.int32)
    empty_m = dict.fromkeys(METRIC_FIELDS, 0)
    shape = (header.ny, header.nx)
    seen_first = False
    pend_spots: list[np.ndarray] = []    # spots/overlays from skipped ticks ride along
    pend_overlay: list[np.ndarray] = []
    for rec in records:
        if isinstance(rec, Keyframe):
            phase, intensity = rec.expand(n)
            if not seen_first:
                seen_first = True
                yield header, Frame(rec.tick, rec.t_s, phase.reshape(shape).copy(),
                                    intensity.reshape(shape).copy(),
                                    arrival.reshape(shape).copy(), empty_m,
                                    np.zeros(0, SPOT_DT), np.zeros(0, OVERLAY_DT), {}, 0)
        elif isinstance(rec, Tick):
            d = rec.dirty
            if d.size:
                phase[d["idx"]] = d["phase"]
                intensity[d["idx"]] = d["intensity"]
                arrival[d["idx"]] = d["arrival_s"]
            pend_spots.append(rec.spots)
            pend_overlay.append(rec.overlay)
            if every <= 1 or rec.tick % every == 0:
                spots = np.concatenate(pend_spots) if len(pend_spots) > 1 else pend_spots[0]
                overlay = (np.concatenate(pend_overlay) if len(pend_overlay) > 1
                           else pend_overlay[0])
                pend_spots, pend_overlay = [], []
                yield header, Frame(rec.tick, rec.t_s, phase.reshape(shape).copy(),
                                    intensity.reshape(shape).copy(),
                                    arrival.reshape(shape).copy(),
                                    rec.metrics, spots, overlay, rec.diag, rec.state_hash)
        elif isinstance(rec, End):
            return


def read_replay(path: str | Path) -> dict[str, Any]:
    p = Path(path)
    d = json.loads(p.read_text(encoding="utf-8"))
    if d.get("format") != "ember-replay":
        raise ValueError(f"not an ember replay: {p}")
    return d


def replay_stream_path(replay_path: str | Path) -> Path | None:
    d = read_replay(replay_path)
    if not d.get("stream"):
        return None
    return (Path(replay_path).parent / d["stream"]).resolve()
