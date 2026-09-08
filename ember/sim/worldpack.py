"""World pack v1 export + load (docs/sim/formats.md §1; plan B1 + B2).

A world pack is the ONLY world input the C++ core reads: `world.json` + raw little-endian
row-major `.bin` layers on one grid. `export_bundle` turns an Epic 3 scenario bundle (and the
Epic 1-2 world it pins) into a pack; `export_synthetic` writes procedural test worlds in the
same layout; `load_worldpack` memory-maps a pack for Python-side tools (renderer, QA).
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

import numpy as np

from ember import __version__
from ember.sim import WORLD_PACK_FORMAT, WORLD_PACK_VERSION

INT32_MIN = -(2**31)

# Manifest dtype tag -> numpy dtype (explicit little-endian).
DTYPES: dict[str, np.dtype] = {
    "i32": np.dtype("<i4"),
    "u8": np.dtype("u1"),
    "u16": np.dtype("<u2"),
}

# Layer -> (dtype tag, unit). Order is the manifest order (and the pack_hash order).
LAYER_SPECS: dict[str, tuple[str, str]] = {
    "elevation_cm": ("i32", "cm"),
    "fbfm40": ("u8", "code"),
    "cc_pct": ("u8", "%"),
    "ch_dm": ("u16", "dm"),
    "cbh_dm": ("u16", "dm"),
    "cbd_gm3": ("u16", "g/m3"),
    "evt": ("u16", "code"),
    "hillshade": ("u8", "shade"),
    "greenness": ("u8", "0-255"),
    "structures": ("u8", "mask"),
    "arrival_s": ("i32", "s"),
    "confidence": ("u8", "class"),
}

# Expected SI units in fuels.provenance.json (Epic 2 D6 paranoia: refuse anything else).
_EXPECTED_FUEL_UNITS = {
    "fbfm40": "code", "evt": "code", "cc": "percent", "ch": "m", "cbh": "m", "cbd": "kg/m3",
}


def fuel_class(code: int) -> str:
    """FBFM40 code -> class (docs/sim/default-model-spec.md §5.1)."""
    if 101 <= code <= 109:
        return "GR"
    if 121 <= code <= 124:
        return "GS"
    if 141 <= code <= 149:
        return "SH"
    if 161 <= code <= 165:
        return "TU"
    if 181 <= code <= 189:
        return "TL"
    if 201 <= code <= 204:
        return "SB"
    return "NB"


def fuel_class_array(fbfm40: np.ndarray) -> np.ndarray:
    """Vectorised class index: 0 NB, 1 GR, 2 GS, 3 SH, 4 TU, 5 TL, 6 SB."""
    c = np.asarray(fbfm40).astype(np.int32)
    out = np.zeros(c.shape, dtype=np.uint8)
    out[(c >= 101) & (c <= 109)] = 1
    out[(c >= 121) & (c <= 124)] = 2
    out[(c >= 141) & (c <= 149)] = 3
    out[(c >= 161) & (c <= 165)] = 4
    out[(c >= 181) & (c <= 189)] = 5
    out[(c >= 201) & (c <= 204)] = 6
    return out


CLASS_NAMES = ["NB", "GR", "GS", "SH", "TU", "TL", "SB"]


# ---- hillshade (also the renderer's fallback) ---------------------------------------- #
def hillshade_from_elevation(elev_m: np.ndarray, cell_m: float, *, az_deg: float = 315.0,
                             alt_deg: float = 45.0, nodata_mask: np.ndarray | None = None,
                             ) -> np.ndarray:
    """Classic Horn hillshade -> uint8 (1..255; 0 = nodata). Precomputation only."""
    e = np.asarray(elev_m, dtype=np.float64)
    if nodata_mask is not None and nodata_mask.any():
        e = e.copy()
        e[nodata_mask] = np.nanmean(e[~nodata_mask]) if (~nodata_mask).any() else 0.0
    gy, gx = np.gradient(e, cell_m)
    slope = np.arctan(np.hypot(gx, gy))
    aspect = np.arctan2(-gx, gy)
    az = np.deg2rad(360.0 - az_deg + 90.0)
    alt = np.deg2rad(alt_deg)
    shade = np.sin(alt) * np.cos(slope) + np.cos(alt) * np.sin(slope) * np.cos(az - aspect)
    out = np.clip(np.rint(1 + 254 * np.clip(shade, 0, 1)), 1, 255).astype(np.uint8)
    if nodata_mask is not None:
        out[nodata_mask] = 0
    return out


# ---- writing ---------------------------------------------------------------------------- #
def _stats(arr: np.ndarray, valid: np.ndarray) -> dict[str, Any]:
    n_valid = int(valid.sum())
    if n_valid:
        v = arr[valid]
        mn, mx = int(v.min()), int(v.max())
    else:
        mn = mx = None
    return {"min": mn, "max": mx, "valid": n_valid, "nodata": int(arr.size - n_valid)}


def _write_layer(pack_dir: Path, name: str, arr: np.ndarray, valid: np.ndarray,
                 layers: dict[str, Any]) -> None:
    tag, unit = LAYER_SPECS[name]
    data = np.ascontiguousarray(arr.astype(DTYPES[tag], copy=False))
    fname = f"{name}.bin"
    (pack_dir / fname).write_bytes(data.tobytes(order="C"))
    layers[name] = {"file": fname, "dtype": tag, "unit": unit, "stats": _stats(data, valid)}


def _pack_hash(pack_dir: Path, layers: dict[str, Any]) -> str:
    h = hashlib.sha256()
    for name in layers:
        h.update((pack_dir / layers[name]["file"]).read_bytes())
    return h.hexdigest()


def _write_manifest(pack_dir: Path, manifest: dict[str, Any]) -> None:
    (pack_dir / "world.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")


def _resolve_world_path(p: str, repo_root: Path) -> Path:
    """Bundle `provenance.world.*` paths are repo-root-relative ('store\\...')."""
    cand = Path(p)
    if cand.is_absolute() and cand.exists():
        return cand
    if cand.exists():
        return cand.resolve()
    alt = repo_root / cand
    if alt.exists():
        return alt.resolve()
    raise FileNotFoundError(f"world layer not found: {p} (tried cwd and {repo_root})")


def export_bundle(bundle_manifest_path: str | Path, out_dir: str | Path, *,
                  name: str | None = None) -> Path:
    """Scenario bundle (+ pinned world COGs) -> `<out_dir>/<name>.ewp/`. Returns the pack dir."""
    import rasterio
    from rasterio.enums import Resampling
    from rasterio.warp import reproject

    bundle_path = Path(bundle_manifest_path).resolve()
    m = json.loads(bundle_path.read_text(encoding="utf-8"))
    incident_dir = bundle_path.parent.parent          # .../store/incidents/<id>/
    store_root = incident_dir.parent.parent            # .../store
    repo_root = store_root.parent
    world = m["provenance"]["world"]
    name = name or m.get("world_region") or incident_dir.name
    pack_dir = Path(out_dir) / f"{name}.ewp"
    pack_dir.mkdir(parents=True, exist_ok=True)

    dem_path = _resolve_world_path(world["dem_cog"], repo_root)
    fuels = {k: _resolve_world_path(v, repo_root) for k, v in world["fuels"].items()}
    # SI assertion from the fuels provenance (Epic 2 D6).
    prov_path = next(iter(fuels.values())).parent / "fuels.provenance.json"
    if prov_path.exists():
        prov = json.loads(prov_path.read_text(encoding="utf-8"))
        for layer, exp in _EXPECTED_FUEL_UNITS.items():
            got = prov.get("layers", {}).get(layer, {}).get("unit")
            if got is not None and got != exp:
                raise ValueError(f"fuels layer {layer!r} unit is {got!r}, expected {exp!r} "
                                 f"(refusing non-SI world: {prov_path})")
    else:
        raise FileNotFoundError(f"fuels.provenance.json missing beside {prov_path.parent}")

    layers: dict[str, Any] = {}
    with rasterio.open(dem_path) as ds:
        ref = (ds.width, ds.height, ds.transform, ds.crs)
        nx, ny, tf, crs = ds.width, ds.height, ds.transform, ds.crs
        cell = float(tf.a)
        if abs(tf.a) != abs(tf.e) or tf.b != 0 or tf.d != 0:
            raise ValueError(f"DEM grid is not square/north-up: {tf}")
        dem = ds.read(1)
        dem_valid = dem != ds.nodata if ds.nodata is not None else np.ones(dem.shape, bool)
        dem_valid &= np.isfinite(dem)

    def _read_aligned(path: Path) -> tuple[np.ndarray, np.ndarray]:
        with rasterio.open(path) as ds:
            got = (ds.width, ds.height, ds.transform, ds.crs)
            if got != ref:
                raise ValueError(f"{path.name} is not pixel-aligned with the DEM: {got} != {ref}")
            a = ds.read(1)
            valid = np.ones(a.shape, bool) if ds.nodata is None else (a != ds.nodata)
            if a.dtype.kind == "f":
                valid &= np.isfinite(a)
            return a, valid

    elev_cm = np.where(dem_valid, np.rint(dem.astype(np.float64) * 100.0), INT32_MIN)
    _write_layer(pack_dir, "elevation_cm", elev_cm.astype(np.int64).clip(INT32_MIN, 2**31 - 1),
                 dem_valid, layers)

    fb, fbv = _read_aligned(fuels["fbfm40"])
    if fb.max() > 255:
        raise ValueError("fbfm40 codes exceed uint8")
    _write_layer(pack_dir, "fbfm40", np.where(fbv, fb, 0), fbv, layers)

    cc, ccv = _read_aligned(fuels["cc"])
    _write_layer(pack_dir, "cc_pct", np.where(ccv, np.rint(np.clip(cc, 0, 100)), 0), ccv, layers)
    for src, dst in (("ch", "ch_dm"), ("cbh", "cbh_dm")):
        a, v = _read_aligned(fuels[src])
        _write_layer(pack_dir, dst, np.where(v, np.rint(np.clip(a, 0, 6553) * 10.0), 0), v, layers)
    cbd, cbdv = _read_aligned(fuels["cbd"])
    _write_layer(pack_dir, "cbd_gm3", np.where(cbdv, np.rint(np.clip(cbd, 0, 65) * 1000.0), 0),
                 cbdv, layers)
    evt, evtv = _read_aligned(fuels["evt"])
    _write_layer(pack_dir, "evt", np.where(evtv, evt, 0), evtv, layers)

    hs_path = dem_path.parent.parent / "derived" / "hillshade.cog.tif"
    if hs_path.exists():
        hs, hsv = _read_aligned(hs_path)
        hs = np.where(hsv, np.clip(hs, 1, 255), 0)
    else:
        hs = hillshade_from_elevation(dem, cell, nodata_mask=~dem_valid)
        hsv = hs > 0
    _write_layer(pack_dir, "hillshade", hs, hsv, layers)

    # Arrival + confidence: NOT on the world grid upstream (Epic 3 derives them on the AOI
    # grid). Nearest resample onto the world grid; provenance recorded.
    arrival_meta: dict[str, Any] | None = None
    derived = m.get("derived") or {}
    if "arrival_time" in derived:
        arr_path = incident_dir / derived["arrival_time"]
        with rasterio.open(arr_path) as src:
            src_grid = {"nx": src.width, "ny": src.height, "cell_size_m": float(src.transform.a),
                        "origin_x": float(src.transform.c), "origin_y": float(src.transform.f),
                        "crs": str(src.crs)}
            dst_h = np.full((ny, nx), -9999.0, dtype=np.float32)
            reproject(rasterio.band(src, 1), dst_h, dst_transform=tf, dst_crs=crs,
                      dst_nodata=-9999.0, src_nodata=src.nodata, resampling=Resampling.nearest)
        av = dst_h != -9999.0
        arrival_s = np.where(av, np.rint(dst_h.astype(np.float64) * 3600.0), -1).astype(np.int64)
        _write_layer(pack_dir, "arrival_s", arrival_s, av, layers)
        if "confidence" in derived:
            with rasterio.open(incident_dir / derived["confidence"]) as src:
                dst_c = np.zeros((ny, nx), dtype=np.uint8)
                reproject(rasterio.band(src, 1), dst_c, dst_transform=tf, dst_crs=crs,
                          dst_nodata=0, src_nodata=src.nodata, resampling=Resampling.nearest)
            _write_layer(pack_dir, "confidence", dst_c, dst_c > 0, layers)
        aligned = (src_grid["nx"] == nx and src_grid["ny"] == ny
                   and src_grid["origin_x"] == float(tf.c) and src_grid["origin_y"] == float(tf.f))
        arrival_meta = {
            "algorithm": m["provenance"].get("arrival", {}).get("algorithm"),
            "resampled_from_grid": src_grid, "method": "nearest",
            "source_pixel_aligned": aligned,
            "source_burned_cells": m["provenance"].get("arrival", {}).get("burned_cells"),
            "pack_burned_cells": int(av.sum()),
        }

    t0_iso = m["provenance"].get("arrival", {}).get("t0")
    t0 = datetime.fromisoformat(t0_iso) if t0_iso else datetime.fromisoformat(m["created_utc"])
    if t0.tzinfo is None:
        t0 = t0.replace(tzinfo=UTC)
    t0 = t0.astimezone(UTC)

    grid = {"nx": nx, "ny": ny, "cell_size_m": cell, "crs": str(crs),
            "origin_x": float(tf.c), "origin_y": float(tf.f)}

    weather_ref = None
    if m.get("weather"):
        from ember.sim.weatherpack import export_weather

        weather_ref = export_weather(incident_dir / m["weather"], incident_dir, pack_dir, grid)

    manifest = {
        "format": WORLD_PACK_FORMAT, "version": WORLD_PACK_VERSION, "name": name,
        "grid": grid,
        "t0_utc": t0.strftime("%Y-%m-%dT%H:%M:%SZ"), "t0_unix": int(t0.timestamp()),
        "source": {
            "bundle": str(bundle_path), "incident_id": m["incident_id"],
            "world_region": m.get("world_region"),
            "world_manifest_hash": m.get("world_manifest_hash"),
            "ember_version": __version__,
            "exported_utc": datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
        },
        "layers": layers,
        "arrival": arrival_meta,
        "weather": weather_ref,
        "pack_hash": _pack_hash(pack_dir, layers),
    }
    _write_manifest(pack_dir, manifest)
    return pack_dir


# ---- synthetic world kit (plan B2) ---------------------------------------------------- #
SYNTH_KINDS = ("flat", "ramp", "ridge", "checker", "barrier")


def export_synthetic(kind: str, out_dir: str | Path, *, nx: int = 200, ny: int = 200,
                     cell_size_m: float = 30.0, fuel: int = 102, seed: int = 0,
                     name: str | None = None, **kw: Any) -> Path:
    """Procedural test world -> `<out_dir>/<name>.ewp/` (same layout as real packs).

    kinds: flat | ramp(slope_pct=30) | ridge(height_m=300) | checker(fuel_a, fuel_b, block=20)
    | barrier(barrier_x=nx//2). Canopy scalars cc_pct/ch_dm/cbh_dm/cbd_gm3 default to
    timber-like values for TU/TL fuels and 0 otherwise. base_elev_m defaults to 1000.
    """
    if kind not in SYNTH_KINDS:
        raise ValueError(f"unknown synthetic kind {kind!r}; choose from {SYNTH_KINDS}")
    name = name or f"synth-{kind}"
    pack_dir = Path(out_dir) / f"{name}.ewp"
    pack_dir.mkdir(parents=True, exist_ok=True)
    base_m = float(kw.pop("base_elev_m", 1000.0))
    xs = np.arange(nx, dtype=np.float64)[None, :] * cell_size_m
    ys = np.arange(ny, dtype=np.float64)[:, None] * cell_size_m
    elev = np.full((ny, nx), base_m, dtype=np.float64)
    fb = np.full((ny, nx), fuel, dtype=np.int32)
    params: dict[str, Any] = {"kind": kind, "nx": nx, "ny": ny, "cell_size_m": cell_size_m,
                              "fuel": fuel, "seed": seed, "base_elev_m": base_m}
    if kind == "ramp":
        slope_pct = float(kw.pop("slope_pct", 30.0))
        elev = base_m + xs * slope_pct / 100.0 + 0 * ys
        params["slope_pct"] = slope_pct
    elif kind == "ridge":
        height_m = float(kw.pop("height_m", 300.0))
        cy = max(1.0, (ny - 1) / 2.0)
        elev = base_m + height_m * (1.0 - np.abs(ys / cell_size_m - cy) / cy) + 0 * xs
        params["height_m"] = height_m
    elif kind == "checker":
        fuel_a = int(kw.pop("fuel_a", fuel))
        fuel_b = int(kw.pop("fuel_b", 183))
        block = int(kw.pop("block", 20))
        gx, gy = np.meshgrid(np.arange(nx) // block, np.arange(ny) // block)
        fb = np.where((gx + gy) % 2 == 0, fuel_a, fuel_b).astype(np.int32)
        params.update(fuel_a=fuel_a, fuel_b=fuel_b, block=block)
    elif kind == "barrier":
        bx = int(kw.pop("barrier_x", nx // 2))
        fb[:, bx] = 99
        params["barrier_x"] = bx
    elev = np.broadcast_to(elev, (ny, nx)).copy()

    cls_arr = fuel_class_array(fb)
    canopy = (cls_arr == 4) | (cls_arr == 5)
    timber = bool(canopy.any())
    cc = int(kw.pop("cc_pct", 60 if timber else 0))
    ch = int(kw.pop("ch_dm", 200 if timber else 0))
    cbh = int(kw.pop("cbh_dm", 20 if timber else 0))
    cbd = int(kw.pop("cbd_gm3", 100 if timber else 0))
    params.update(cc_pct=cc, ch_dm=ch, cbh_dm=cbh, cbd_gm3=cbd)
    if kw:
        raise TypeError(f"unknown synthetic parameters: {sorted(kw)}")

    ones = np.ones((ny, nx), bool)
    layers: dict[str, Any] = {}
    _write_layer(pack_dir, "elevation_cm", np.rint(elev * 100.0).astype(np.int64), ones, layers)
    _write_layer(pack_dir, "fbfm40", fb, fb > 0, layers)
    _write_layer(pack_dir, "cc_pct", np.where(canopy, cc, 0), ones, layers)
    _write_layer(pack_dir, "ch_dm", np.where(canopy, ch, 0), ones, layers)
    _write_layer(pack_dir, "cbh_dm", np.where(canopy, cbh, 0), ones, layers)
    _write_layer(pack_dir, "cbd_gm3", np.where(canopy, cbd, 0), ones, layers)
    _write_layer(pack_dir, "evt", np.zeros((ny, nx), np.int32), ones, layers)
    _write_layer(pack_dir, "hillshade", hillshade_from_elevation(elev, cell_size_m), ones, layers)

    manifest = {
        "format": WORLD_PACK_FORMAT, "version": WORLD_PACK_VERSION, "name": name,
        "grid": {"nx": nx, "ny": ny, "cell_size_m": float(cell_size_m), "crs": "LOCAL",
                 "origin_x": 0.0, "origin_y": 0.0},
        "t0_utc": "1970-01-01T00:00:00Z", "t0_unix": 0,
        "source": {"synthetic": params, "ember_version": __version__},
        "layers": layers, "arrival": None, "weather": None,
        "pack_hash": _pack_hash(pack_dir, layers),
    }
    _write_manifest(pack_dir, manifest)
    return pack_dir


# ---- loading ---------------------------------------------------------------------------- #
@dataclass
class WeatherPack:
    manifest: dict[str, Any]
    data: np.ndarray  # i16[num_steps][5][ny][nx] (memmap)

    @property
    def variables(self) -> list[str]:
        return list(self.manifest["variables"])

    def step(self, i: int, var: str) -> np.ndarray:
        return self.data[i, self.variables.index(var)]


@dataclass
class WorldPack:
    path: Path
    manifest: dict[str, Any]
    layers: dict[str, np.ndarray] = field(default_factory=dict)
    weather: WeatherPack | None = None

    @property
    def nx(self) -> int:
        return int(self.manifest["grid"]["nx"])

    @property
    def ny(self) -> int:
        return int(self.manifest["grid"]["ny"])

    @property
    def cell_size_m(self) -> float:
        return float(self.manifest["grid"]["cell_size_m"])

    @property
    def t0_unix(self) -> int:
        return int(self.manifest["t0_unix"])

    def layer(self, name: str) -> np.ndarray:
        return self.layers[name]

    def has(self, name: str) -> bool:
        return name in self.layers


def load_worldpack(path: str | Path) -> WorldPack:
    """Memory-map a pack directory (or its world.json)."""
    p = Path(path)
    pack_dir = p.parent if p.is_file() else p
    manifest = json.loads((pack_dir / "world.json").read_text(encoding="utf-8"))
    if manifest.get("format") != WORLD_PACK_FORMAT:
        raise ValueError(f"not a world pack: {pack_dir}")
    if int(manifest.get("version", 0)) > WORLD_PACK_VERSION:
        raise ValueError(f"world pack version {manifest['version']} newer than supported "
                         f"{WORLD_PACK_VERSION}")
    ny, nx = int(manifest["grid"]["ny"]), int(manifest["grid"]["nx"])
    layers: dict[str, np.ndarray] = {}
    for name, meta in manifest["layers"].items():
        mm = np.memmap(pack_dir / meta["file"], dtype=DTYPES[meta["dtype"]], mode="r")
        if mm.size != nx * ny:
            raise ValueError(f"layer {name}: {mm.size} values, expected {nx * ny}")
        layers[name] = mm.reshape(ny, nx)
    weather = None
    if manifest.get("weather"):
        wm = json.loads((pack_dir / manifest["weather"]).read_text(encoding="utf-8"))
        g = wm["grid"]
        shape = (int(wm["num_steps"]), len(wm["variables"]), int(g["ny"]), int(g["nx"]))
        data = np.memmap(pack_dir / wm["file"], dtype="<i2", mode="r").reshape(shape)
        weather = WeatherPack(wm, data)
    return WorldPack(pack_dir, manifest, layers, weather)
