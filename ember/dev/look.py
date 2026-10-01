"""The look-iteration loop (Brad 2026-10-01: "small scenarios for testing purposes, especially for
iterating on look"): build if needed, regenerate changed asset generators, render one look-lab
scenario (viz/scenarios/L_*.toml), and sheet every capture as

    this render | the previous render | the reference photo (the capture's `reference`)

into <run>/look_sheet.jpg. Prints the time per step (iteration speed is a standing priority).
"""
from __future__ import annotations

import time
from pathlib import Path

from ember.dev.scenario import LoadedScenario


def _reference(sc: LoadedScenario, repo: Path, ref: str | None) -> Path | None:
    if not ref:
        return None
    for base in (sc.path.parent, repo):     # relative to the scenario file, or to the repo
        p = (base / ref).resolve()
        if p.exists():
            return p
    return None


def previous_run(runs_dir: Path, current: Path) -> Path | None:
    runs = sorted(p for p in runs_dir.iterdir() if p.is_dir() and p.name < current.name
                  and (p / "captures").is_dir())
    return runs[-1] if runs else None


def sheet(sc: LoadedScenario, repo: Path, run_dir: Path, width: int = 640) -> Path:
    from PIL import Image, ImageDraw

    prev = previous_run(run_dir.parent, run_dir)
    caps = sc.spec.captures
    h = width * 9 // 16
    out = Image.new("RGB", (3 * width, len(caps) * (h + 20)), (14, 14, 14))
    d = ImageDraw.Draw(out)
    for i, c in enumerate(caps):
        y = i * (h + 20)
        cols = [("now", run_dir / "captures" / f"{c.name}.png"),
                (f"previous ({prev.name})" if prev else "previous: none",
                 prev / "captures" / f"{c.name}.png" if prev else None),
                ("reference", _reference(sc, repo, c.reference))]
        for j, (label, f) in enumerate(cols):
            d.text((j * width + 4, y + 4), f"{c.name}: {label}", fill=(255, 220, 120))
            if f and Path(f).exists():
                im = Image.open(f).convert("RGB")
                im.thumbnail((width, h))
                out.paste(im, (j * width + (width - im.width) // 2, y + 20 + (h - im.height) // 2))
    dst = run_dir / "look_sheet.jpg"
    out.save(dst, quality=86)
    return dst


def run(name: str, *, build: bool = True, regen: bool = True) -> tuple[Path, Path, list]:
    """-> (run dir, sheet, [(step, seconds)])."""
    from ember.dev import assets, ue
    from ember.dev.scenario import find_scenario, load_scenario

    repo = ue.repo_root()
    eng = ue.load_engine()
    sc = load_scenario(find_scenario(repo, name))
    steps = []
    if build:
        t = time.perf_counter()
        b = ue.build(eng)
        steps.append(("build", time.perf_counter() - t))
        if not b.ok:
            raise RuntimeError(f"build failed: {b.errors[:3]}")
    if regen:
        t = time.perf_counter()
        res = assets.regen(eng)
        steps.append(("regen (changed generators only)", time.perf_counter() - t))
        if not res["ok"]:
            bad = [g["script"] for g in res["generators"] if not g["ok"]]
            raise RuntimeError(f"regen failed: {bad}")
    t = time.perf_counter()
    run_dir = ue.run_scenario(eng, sc, timeout_s=600, exposure_bias=sc.spec.scenario.exposure_bias)
    steps.append(("render", time.perf_counter() - t))
    t = time.perf_counter()
    dst = sheet(sc, repo, run_dir)
    steps.append(("sheet", time.perf_counter() - t))
    return run_dir, dst, steps
