"""Fetch CC0 texture sets listed in an assets/sources/*.toml manifest into the Ember store.

Sources: Poly Haven (api.polyhaven.com, per-map JPGs) and ambientCG (ambientcg.com, one zip per
resolution). Each set lands in store/textures/<manifest stem>/<key>/ as
    color.jpg  normal_gl.jpg  roughness.jpg  height.jpg  ao.jpg (when the source has it)
plus source.json (where it came from, licence, the exact files). Downloads are metered in the
household ledger (Terrain NetMeter); sets already on disk are skipped.
"""
from __future__ import annotations

import io
import json
import tomllib
import urllib.request
import zipfile
from pathlib import Path

UA = {"User-Agent": "ember-textures/0"}
PH_MAPS = {"Diffuse": "color", "nor_gl": "normal_gl", "Rough": "roughness",
           "Displacement": "height", "AO": "ao"}
ACG_SUFFIX = {"_Color": "color", "_NormalGL": "normal_gl", "_Roughness": "roughness",
              "_Displacement": "height", "_AmbientOcclusion": "ao"}


def _get(url: str) -> bytes:
    return urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120).read()


def _polyhaven(asset: str, res: str, out: Path) -> dict:
    files = json.loads(_get(f"https://api.polyhaven.com/files/{asset}"))
    got = {}
    for src, dst in PH_MAPS.items():
        f = files.get(src, {}).get(res, {}).get("jpg")
        if not f:
            continue
        (out / f"{dst}.jpg").write_bytes(_get(f["url"]))
        got[dst] = f["url"]
    return got


def _ambientcg(asset: str, res: str, out: Path) -> dict:
    url = f"https://ambientcg.com/get?file={asset}_{res.upper()}-JPG.zip"
    z = zipfile.ZipFile(io.BytesIO(_get(url)))
    got = {}
    for name in z.namelist():
        for suf, dst in ACG_SUFFIX.items():
            if name.endswith(f"{suf}.jpg"):
                (out / f"{dst}.jpg").write_bytes(z.read(name))
                got[dst] = f"{url}#{name}"
    return got


def fetch(manifest: Path, store: Path, res: str = "2k") -> list[dict]:
    from terrain.work.netmeter import NetMeter

    m = tomllib.loads(manifest.read_text(encoding="utf-8"))
    root = store / "textures" / manifest.stem
    done = []
    with NetMeter(r"C:\Projects\Terrain\store", "textures", f"{manifest.stem}-{res}"):
        for s in m["set"]:
            out = root / s["key"]
            if (out / "source.json").exists():
                done.append({"key": s["key"], "skipped": True})
                continue
            out.mkdir(parents=True, exist_ok=True)
            got = (_polyhaven if s["source"] == "polyhaven" else _ambientcg)(s["id"], res, out)
            if "color" not in got or "normal_gl" not in got:
                raise RuntimeError(f"{s['source']} {s['id']}: no colour / normal at {res}")
            meta = {**s, "resolution": res, "files": got}
            (out / "source.json").write_text(json.dumps(meta, indent=1), encoding="utf-8")
            done.append({"key": s["key"], "maps": sorted(got),
                         "mb": round(sum(p.stat().st_size for p in out.glob("*.jpg")) / 1e6, 1)})
    return done
