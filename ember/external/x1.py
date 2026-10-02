"""External Sources X1.6: the X1 bundle - a PyreCast run directory in, the review bundle out, timed
end to end (the "their file to flyover" KPI, first measurement).

Stages: adapter (one timeline per percentile) -> composite packs + embersim replays -> fidelity
sidecars -> client renders (the S_tq26_forecast_0820 views for each percentile and for the
observed playback; the timelapse for p50 and observed) -> the 4-up sheet and the side-by-side
timelapse. Everything lands under store/review/X1/ (git-ignored: PyreCast data is internal-only,
plan rule 1) and carries the attribution line.
"""
from __future__ import annotations

import json
import re
import shutil
import subprocess
import time
from pathlib import Path
from typing import Any

from ember.external import composite as C
from ember.external import fidelity
from ember.external.pyrecast import ATTRIBUTION, load_run

INTERNAL = "INTERNAL - not for distribution"
VIEWS = ("growth_top", "kachess_hero", "aug21_run")
HOURS = (24, 48)
# Sheet rows: (view, hours after the run, pass). "map" = the smoke-off pass (the smoke is ours,
# not the model's, and from above it hides the fronts); "main" = with smoke, the cinematic views.
ROWS = (("growth_top", 24, "map"), ("growth_top", 48, "map"),
        ("kachess_hero", 24, "main"), ("kachess_hero", 48, "main"), ("aug21_run", 48, "main"))


def _sub(text: str, key: str, value: str) -> str:
    return re.sub(rf"^{key} = .*$", f"{key} = {value}", text, count=1, flags=re.M)


def _variant(template: Path, out: Path, name: str, replay: Path, orbits: bool,
             smoke: bool = True) -> Path:
    t = template.read_text(encoding="utf-8")
    t = _sub(t, "name", f'"{name}"')
    t = _sub(t, "replay", f'"{replay.resolve().as_posix()}"')
    if not smoke:
        t = re.sub(r"^smoke_wind_ms = .*$", lambda m: m.group(0) + "\nsmoke = false", t,
                   count=1, flags=re.M)
    if not orbits:
        t = t.split("[[orbits]]")[0]
    out.write_text(t, encoding="utf-8")
    return out


def _render(repo: Path, toml: Path) -> Path:
    exe = shutil.which("ember-dev") or "ember-dev"
    p = subprocess.run([exe, "run-scenario", str(toml), "--timeout", "1500"],
                       capture_output=True, text=True, cwd=repo)
    m = re.search(r"-> (.+)$", p.stdout.strip().splitlines()[-1] if p.stdout.strip() else "")
    if p.returncode != 0 or not m:
        raise RuntimeError(f"render failed for {toml.name}: {p.stdout[-800:]} {p.stderr[-800:]}")
    return Path(m.group(1).strip())


def _font(size: int):
    from PIL import ImageFont

    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def sheet(cols: list[tuple[str, dict[str, Path], dict]], run_label: str, out: Path) -> Path:
    """Rows = ROWS (view x hour x pass), columns = members (+ observed). Labels only; no scores."""
    from PIL import Image, ImageDraw

    W, H, top, left, foot = 400, 225, 64, 150, 46
    rows = ROWS
    img = Image.new("RGB", (left + W * len(cols), top + H * len(rows) + foot), (22, 22, 26))
    d = ImageDraw.Draw(img)
    d.text((10, 8), f"Three Queens 2026 - forecast issued {run_label}, as issued, "
           "beside what happened", font=_font(18), fill=(240, 240, 240))
    for j, (label, runs, info) in enumerate(cols):
        x = left + j * W
        d.text((x + 8, 34), label, font=_font(16), fill=(255, 220, 120))
        for i, (v, h, kind) in enumerate(rows):
            y = top + i * H
            png = runs[kind] / "captures" / f"{v}_{h}h.png"
            if png.exists():
                img.paste(Image.open(png).convert("RGB").resize((W, H)), (x, y))
            ac = info.get(f"acres_{h}h")
            if ac is not None:
                d.rectangle([x + 4, y + H - 24, x + 190, y + H - 4], fill=(0, 0, 0))
                d.text((x + 8, y + H - 22), f"growth since run: {ac:,.0f} ac", font=_font(13),
                       fill=(230, 230, 230))
    for i, (v, h, kind) in enumerate(rows):
        y = top + i * H
        d.text((10, y + H // 2 - 16), v.replace("_", " "), font=_font(14), fill=(220, 220, 220))
        d.text((10, y + H // 2 + 2), f"run + {h} h", font=_font(14), fill=(170, 170, 170))
        if kind == "map":
            d.text((10, y + H // 2 + 20), "smoke off", font=_font(12), fill=(150, 190, 255))
    y = top + H * len(rows) + 8
    d.text((10, y), f"{INTERNAL}. {ATTRIBUTION}.", font=_font(14), fill=(255, 150, 150))
    d.text((10, y + 18), "Forecast columns: observed (NIROPS) until the run, then the forecast as "
           "issued (its starting perimeter shown at the run time). Observed column: the NIROPS "
           "playback.", font=_font(12), fill=(170, 170, 170))
    img.save(out)
    return out


def side_by_side(left: Path, right: Path, labels: tuple[str, str], out: Path) -> Path:
    from PIL import Image, ImageDraw

    banner = out.with_suffix(".banner.png")
    b = Image.new("RGBA", (3200, 70), (0, 0, 0, 170))
    d = ImageDraw.Draw(b)
    d.text((20, 8), labels[0], font=_font(28), fill=(255, 220, 120))
    d.text((1620, 8), labels[1], font=_font(28), fill=(255, 220, 120))
    d.text((20, 42), f"{INTERNAL}. {ATTRIBUTION}.", font=_font(18), fill=(255, 160, 160))
    b.save(banner)
    ff = shutil.which("ffmpeg")
    if not ff:
        raise RuntimeError("ffmpeg not found")
    subprocess.run([ff, "-y", "-v", "error", "-i", str(left), "-i", str(right), "-i", str(banner),
                    "-filter_complex", "[0:v][1:v]hstack=inputs=2[s];[s][2:v]overlay=0:0",
                    "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", str(out)], check=True)
    banner.unlink(missing_ok=True)
    return out


def _finished_render(repo: Path, name: str, orbits: bool) -> Path | None:
    """The newest complete render of a variant (every capture, + the timelapse when wanted):
    lets the bundle resume after an interrupted run instead of re-rendering everything."""
    root = repo / "runs" / "viz" / name
    for d in sorted(root.glob("*"), reverse=True) if root.exists() else []:
        caps = [d / "captures" / f"{v}_{h}h.png" for v in VIEWS for h in HOURS]
        if all(c.exists() for c in caps) and (
                not orbits or (d / "orbits" / "timelapse_72h.mp4").exists()):
            return d
    return None


def build(repo: Path, run_dir: Path, pack: Path, observed_replay: Path,
          pcts: tuple[int, ...] = (10, 50, 90), render: bool = True,
          reuse: bool = True) -> dict[str, Any]:
    out = repo / "store" / "review" / "X1"
    (out / "scenarios").mkdir(parents=True, exist_ok=True)
    tag = run_dir.name[4:8]                      # 20260820_051100 -> 0820
    times: dict[str, float] = {}
    t_all = time.perf_counter()
    t = time.perf_counter()
    tls = {p: load_run(run_dir, p) for p in pcts}
    times["adapter_s"] = time.perf_counter() - t

    t = time.perf_counter()
    replays: dict[int, Path] = {}
    exe = repo / "sim" / "build" / "Release" / "embersim.exe"
    for p, tl in tls.items():
        tl.write(run_dir.parents[2] / "timelines" / run_dir.name / f"p{p}")
        name = f"tq26-fc{tag}-p{p}"
        pk = C.write_pack(tl, pack, repo / "store" / "sim", name)
        meta = json.loads((pk / "world.json").read_text(encoding="utf-8"))["arrival"]
        sim_toml = repo / "store" / "external" / "sim" / f"{name}.scenario.toml"
        toml = C.write_sim_scenario(pk, sim_toml, repo / "runs" / name, name,
                                    meta["t_ref_s"] + 336 * 3600)
        subprocess.run([str(exe), "run", str(toml), "--quiet"], check=True, capture_output=True)
        replays[p] = repo / "runs" / name / f"{name}.replay.json"
    times["composite_and_replay_s"] = time.perf_counter() - t

    t = time.perf_counter()
    fid = {}
    for p, tl in tls.items():
        fp = fidelity.write(tl, repo / "store" / "sim" / f"tq26-fc{tag}-p{p}.ewp", replays[p])
        shutil.copyfile(fp, out / fp.name)
        fid[p] = json.loads(fp.read_text(encoding="utf-8"))
    times["fidelity_s"] = time.perf_counter() - t

    renders: dict[str, dict[str, Path]] = {}
    if render:
        t = time.perf_counter()
        template = repo / "viz" / "scenarios" / "S_tq26_forecast_0820.toml"
        jobs = [(f"p{p}", replays[p], p == 50) for p in pcts]
        jobs.append(("observed", observed_replay, True))
        for label, rp, timelapse in jobs:
            renders[label] = {}
            # main: with smoke, no timelapse; map: smoke off, + the timelapse for p50 / observed
            for kind, smoke, orbits in (("main", True, False), ("map", False, timelapse)):
                name = f"X1_{tag}_{label}" + ("" if kind == "main" else "_map")
                toml = _variant(template, out / "scenarios" / f"{name}.toml", name, rp, orbits,
                                smoke)
                done = _finished_render(repo, name, orbits) if reuse else None
                renders[label][kind] = done or _render(repo, toml)
        times["render_s"] = time.perf_counter() - t

        t = time.perf_counter()
        R = fid[pcts[0]]["t_ref_s"]
        obs = _observed_growth(observed_replay, pack, R)
        cols = [(f"forecast p{p}", renders[f"p{p}"],
                 {f"acres_{h}h": tls[p].growth_acres_by(h) for h in HOURS}) for p in pcts]
        cols.append(("observed (NIROPS)", renders["observed"], obs))
        sheet(cols, tls[pcts[0]].provenance["run_utc"], out / f"X1_{tag}_sheet.png")
        side_by_side(renders["p50"]["map"] / "orbits" / "timelapse_72h.mp4",
                     renders["observed"]["map"] / "orbits" / "timelapse_72h.mp4",
                     ("forecast p50 (as issued)", "observed (NIROPS)"),
                     out / f"X1_{tag}_timelapse_p50_vs_observed.mp4")
        times["compose_s"] = time.perf_counter() - t
    times["total_s"] = time.perf_counter() - t_all
    keys = ("ok", "source_cells_due", "share_within_one_tick", "missing", "late", "early",
            "unsupported")
    summary = {"run": tls[pcts[0]].provenance["run_utc"],
               "times": {k: round(v, 1) for k, v in times.items()},
               "fidelity": {f"p{p}": {k: f[k] for k in keys} for p, f in fid.items()},
               "seams": fid[pcts[0]]["seams"],
               "renders": {k: {kk: str(vv) for kk, vv in v.items()} for k, v in renders.items()},
               "timeline": {f"p{p}": tl.summary() for p, tl in tls.items()}}
    (out / f"X1_{tag}_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    return summary


# The cinematic camera for the map-vs-render demo (Brad 2026-10-02: show our output beside
# theirs): a low oblique on the p50 growth (the east lobe) from the SSE, smoke on.
CINE = """
[[bookmarks]]
name = "front_cine"
target_frac = [0.70, 0.52]
distance_m = 3000
yaw_deg = 330
pitch_deg = -15
fov_deg = 60
sun = "afternoon"

[[captures]]
name = "front_cine_36h"
bookmark = "front_cine"
t_s = 3156180
golden = false

[[orbits]]
name = "timelapse_72h"
bookmark = "front_cine"
degrees = 0
frames = 360
fps = 30
t_from_s = 3026580
t_to_s = 3285780
"""


def map_videos(repo: Path, run_ts: str = "20260820_051100", pct: int = 50) -> list[Path]:
    """The forecast drawn as a conventional 2D map (left), frame-synced with our render (right):
    once with the smoke-off map view, once with the cinematic camera. Needs the X1 bundle."""
    from ember.external.mapvideo import ForecastMap, side_by_side
    from ember.external.timeline import read_timeline

    tag = run_ts[4:8]
    out = repo / "store" / "review" / "X1"
    ext = repo / "store" / "incidents" / "dfa477f2-305c-49ca-b82e-2af072ab55f8" / "external" \
        / "pyrecast-elmfire"
    tl = read_timeline(ext / "timelines" / run_ts / f"p{pct}")
    fm = ForecastMap(tl, repo / "store" / "sim" / "hist-three-queens-2026-ir.ewp")
    mapped = _finished_render(repo, f"X1_{tag}_p{pct}_map", True)
    if mapped is None:
        raise RuntimeError("run `ember external x1-bundle` first (the p50 map render)")
    name = f"X1_{tag}_p{pct}_cine"
    main = out / "scenarios" / f"X1_{tag}_p{pct}.toml"
    toml = out / "scenarios" / f"{name}.toml"
    text = main.read_text(encoding="utf-8").split("[[bookmarks]]")[0]
    toml.write_text(_sub(text, "name", f'"{name}"') + CINE, encoding="utf-8")
    root = repo / "runs" / "viz" / name
    done = [d for d in sorted(root.glob("*"), reverse=True)
            if (d / "orbits" / "timelapse_72h.mp4").exists()] if root.exists() else []
    cine = done[0] if done else _render(repo, toml)
    vids = []
    for label, run in (("mapview", mapped), ("cinematic", cine)):
        vids.append(side_by_side(fm, run / "orbits" / "timelapse_72h.mp4", 0.0, 72.0, 360, 30,
                                 f"p{pct}", out / f"X1_{tag}_map_vs_ember_{label}_p{pct}.mp4"))
    return vids


def _observed_growth(observed_replay: Path, pack: Path, R: int) -> dict[str, float]:
    """Observed acres burned between the run and run + h (the NIROPS arrival on the pack)."""
    m, obs, _ = C._pack(pack)
    cell_ac = m["grid"]["cell_size_m"] ** 2 / 4046.8564224
    return {f"acres_{h}h": round(float(((obs > R) & (obs <= R + h * 3600)).sum()) * cell_ac, 1)
            for h in HOURS}

