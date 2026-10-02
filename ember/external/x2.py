"""External Sources X2 gate bundle: the A/B sheet - the same forecast drawn class-based (the X1
renderer, fire_channels = false) vs channel-driven (ADR 0010), with the source's own crown-class /
flame-length map around each view. Internal only (PyreCast data, plan rule 1)."""
from __future__ import annotations

import json
import re
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

from ember.external import channels
from ember.external.pyrecast import ATTRIBUTION
from ember.external.x1 import INTERNAL, _font, _render

CROWN_RGB = {0: (250, 220, 70), 1: (245, 140, 40), 2: (215, 40, 40)}
CROWN_NAME = {0: "surface", 1: "passive (torching)", 2: "active crown"}


def source_map(pack: Path, frac: tuple[float, float], t_s: int, size=(400, 225),
               half_m: float = 900.0) -> Image.Image:
    """The source's crown class per cell around a view target (burned by t_s), target marked."""
    m = json.loads((pack / "world.json").read_text(encoding="utf-8"))
    g = m["grid"]
    arr = np.fromfile(pack / "arrival_s.bin", "<i4").reshape(g["ny"], g["nx"])
    ch = channels.read(pack)
    cr, fl = ch["crown_class"][0], ch["flame_length_m"][0]
    hs = np.frombuffer((pack / "hillshade.bin").read_bytes(), np.uint8).reshape(g["ny"], g["nx"])
    cx, cy = int(frac[0] * g["nx"]), int(frac[1] * g["ny"])
    h = int(half_m / g["cell_size_m"])
    w = int(h * size[0] / size[1])
    y0, y1, x0, x1 = max(0, cy - h), min(g["ny"], cy + h), max(0, cx - w), min(g["nx"], cx + w)
    img = np.stack([hs[y0:y1, x0:x1].astype(np.float32) * 0.6 + 60] * 3, -1)
    burned = (arr[y0:y1, x0:x1] >= 0) & (arr[y0:y1, x0:x1] <= t_s)
    img[burned] = img[burned] * 0.3 + np.array([80, 72, 68]) * 0.7
    c = cr[y0:y1, x0:x1]
    for k, rgb in CROWN_RGB.items():
        sel = burned & (c == k)
        img[sel] = img[sel] * 0.2 + np.array(rgb) * 0.8
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).resize(size, Image.NEAREST)
    d = ImageDraw.Draw(im)
    px = (cx - x0 + 0.5) / (x1 - x0) * size[0]
    py = (cy - y0 + 0.5) / (y1 - y0) * size[1]
    d.ellipse([px - 8, py - 8, px + 8, py + 8], outline=(255, 255, 255), width=2)
    k = cr[cy, cx]
    label = (f"source: {CROWN_NAME.get(int(k), '?')}, flame {fl[cy, cx]:.1f} m"
             if np.isfinite(k) else "source: silent here")
    d.rectangle([4, size[1] - 24, 8 + d.textlength(label, font=_font(13)), size[1] - 4],
                fill=(0, 0, 0))
    d.text((8, size[1] - 22), label, font=_font(13), fill=(240, 240, 240))
    return im


def build(repo: Path) -> Path:
    out = repo / "store" / "review" / "X2"
    (out / "scenarios").mkdir(parents=True, exist_ok=True)
    tmpl = repo / "viz" / "scenarios" / "S_tq26_forecast_0820_x2.toml"
    text = tmpl.read_text(encoding="utf-8")
    replay = re.search(r'^replay = "(.+)"$', text, re.M).group(1)
    rp = (tmpl.parent / replay).resolve()
    runs = {}
    for label, on in (("classes", False), ("channels", True)):
        t = re.sub(r"^name = .*$", f'name = "X2_0820_{label}"', text, count=1, flags=re.M)
        t = re.sub(r"^replay = .*$", f'replay = "{rp.as_posix()}"', t, count=1, flags=re.M)
        flag = f"\nfire_channels = {'true' if on else 'false'}"
        t = re.sub(r"^(smoke_wind_ms = .*)$", r"\g<1>" + flag, t, count=1, flags=re.M)
        toml = out / "scenarios" / f"X2_0820_{label}.toml"
        toml.write_text(t, encoding="utf-8")
        runs[label] = _render(repo, toml)
    pack = repo / "store" / "sim" / "tq26-fc0820-p90.ewp"
    rows = re.findall(r'\[\[bookmarks\]\]\nname = "(\w+)".*?\ntarget_frac = \[([\d.]+), ([\d.]+)\]',
                      text, re.S)
    t_s = int(re.search(r"^t_s = (\d+)", text, re.M).group(1))
    W, H, top, left, foot = 400, 225, 64, 140, 40
    sheet = Image.new("RGB", (left + 3 * W, top + H * len(rows) + foot), (22, 22, 26))
    d = ImageDraw.Draw(sheet)
    d.text((10, 8), "Three Queens 2026 - PyreCast forecast 2026-08-20 05:11 UTC, p90, run + 40 h: "
           "class-based vs the source's own numbers", font=_font(17), fill=(240, 240, 240))
    for j, head in enumerate(("class-based (X1)", "channel-driven (ADR 0010)",
                              "the source's crown class / flame length")):
        d.text((left + j * W + 8, 36), head, font=_font(15), fill=(255, 220, 120))
    for i, (name, fx, fy) in enumerate(rows):
        y = top + i * H
        d.text((10, y + H // 2 - 8), name.replace("_", " "), font=_font(14), fill=(220, 220, 220))
        for j, label in enumerate(("classes", "channels")):
            png = runs[label] / "captures" / f"{name}_40h.png"
            if png.exists():
                sheet.paste(Image.open(png).convert("RGB").resize((W, H)), (left + j * W, y))
        sheet.paste(source_map(pack, (float(fx), float(fy)), t_s, (W, H)), (left + 2 * W, y))
    y = top + H * len(rows) + 8
    d.text((10, y), f"{INTERNAL}. {ATTRIBUTION}. Crown map: yellow surface, orange passive, "
           "red active, grey burned (source silent).", font=_font(13), fill=(255, 150, 150))
    dst = out / "X2_0820_ab_sheet.png"
    sheet.save(dst)
    return dst
