"""`ember-dev evaluate` — a run directory in, a verdict out.

Run directory (written by `ember-dev run-scenario`, see docs/viz/harness.md):
    run.json  captures/<capture>.png  facts/<capture>.json  [facts/perf.json]  log/
Outputs written into the same directory:
    verdict.json  contact_sheet.png  diff/<capture>.png

The verdict is the agent's feedback signal. `pass` requires every golden diff, every
scenario assert, and every budget check to hold. Missing goldens are reported as
`new` (not a failure) so a fresh view can be captured, reviewed, and blessed.
"""

from __future__ import annotations

import json
from datetime import UTC, datetime
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from ember.dev import facts as F
from ember.dev.imagediff import compare, golden_image
from ember.dev.scenario import LoadedScenario

VERDICT_FORMAT = "ember-viz-verdict"


def goldens_dir(repo: Path, scenario: str) -> Path:
    return repo / "viz" / "goldens" / scenario


def evaluate_run(repo: Path, sc: LoadedScenario, run_dir: Path) -> dict:
    spec = sc.spec
    budgets = F.load_budgets(repo / "viz" / "budgets.toml")
    budget = budgets.get(spec.scenario.budget, {})
    gdir = goldens_dir(repo, sc.name)

    run_meta = {}
    if (run_dir / "run.json").exists():
        run_meta = json.loads((run_dir / "run.json").read_text(encoding="utf-8"))

    captures_out = []
    checks: list[F.Check] = []
    for cap in spec.captures:
        png = run_dir / "captures" / f"{cap.name}.png"
        entry: dict = {"name": cap.name, "bookmark": cap.bookmark, "image": None,
                       "golden": None, "diff": None, "status": "missing"}
        if png.exists():
            entry["image"] = str(png.relative_to(run_dir))
            golden = gdir / f"{cap.name}.png"
            if not cap.golden:
                entry["status"] = "unchecked"
            elif not golden.exists():
                entry["status"] = "new"
            else:
                entry["golden"] = str(golden.relative_to(repo))
                heat = run_dir / "diff" / f"{cap.name}.png"
                d = compare(png, golden, ssim_min=cap.ssim_min,
                            region_ssim_min=cap.region_ssim_min, heatmap_out=heat)
                entry["diff"] = d.to_dict()
                entry["heatmap"] = str(heat.relative_to(run_dir)) if heat.exists() else None
                entry["status"] = "match" if d.ok else "mismatch"
        captures_out.append(entry)

        fpath = run_dir / "facts" / f"{cap.name}.json"
        if not fpath.exists():
            checks.append(F.Check("assert", "facts-file", cap.name, False, None,
                                  f"facts/{cap.name}.json present"))
            continue
        facts = F.load_facts(fpath)
        for a in spec.asserts:
            if a.capture in (None, cap.name):
                checks.append(F.eval_assert(facts, cap.name, a.fact, a.op, a.value))

    perf_path = run_dir / "facts" / "perf.json"
    if budget:
        if perf_path.exists():
            checks.extend(F.eval_budgets(budget, F.load_facts(perf_path), "perf"))
        elif spec.scenario.perf_frames > 0:
            checks.append(F.Check("budget", "perf-file", "perf", False, None,
                                  "facts/perf.json present"))


    orbit_problems = [o for o in run_meta.get("orbits", []) if not o.get("mp4")]
    for o in orbit_problems:
        checks.append(F.Check("orbit", o["name"], "orbit", False, o.get("error"), "mp4 written"))
    failures = [c for c in captures_out if c["status"] in ("mismatch", "missing")]
    failed_checks = [c for c in checks if not c.ok]
    run_ok = run_meta.get("exit_code", 0) == 0
    verdict = {
        "format": VERDICT_FORMAT, "version": 1,
        "scenario": sc.name, "run_dir": str(run_dir),
        "evaluated_utc": datetime.now(UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "pass": run_ok and not failures and not failed_checks,
        "summary": {
            "captures": len(captures_out),
            "match": sum(c["status"] == "match" for c in captures_out),
            "new": sum(c["status"] == "new" for c in captures_out),
            "mismatch": sum(c["status"] == "mismatch" for c in captures_out),
            "missing": sum(c["status"] == "missing" for c in captures_out),
            "checks": len(checks), "checks_failed": len(failed_checks),
            "run_exit_code": run_meta.get("exit_code"),
        },
        "captures": captures_out,
        "orbits": run_meta.get("orbits", []),
        "checks": [c.to_dict() for c in checks],
        "run": run_meta,
    }
    (run_dir / "verdict.json").write_text(json.dumps(verdict, indent=2), encoding="utf-8")
    contact_sheet(run_dir, repo, verdict, run_dir / "contact_sheet.png")
    return verdict


def bless(repo: Path, sc: LoadedScenario, run_dir: Path,
          only: list[str] | None = None) -> list[str]:
    """Copy a run's captures over the goldens (after a human or agent has looked)."""
    gdir = goldens_dir(repo, sc.name)
    gdir.mkdir(parents=True, exist_ok=True)
    done = []
    for cap in sc.spec.captures:
        if not cap.golden or (only and cap.name not in only):
            continue
        src = run_dir / "captures" / f"{cap.name}.png"
        if src.exists():
            golden_image(src).save(gdir / f"{cap.name}.png", optimize=True)
            done.append(cap.name)
    return done


_STATUS_COLOR = {"match": (60, 170, 80), "new": (70, 130, 220), "unchecked": (150, 150, 150),
                 "mismatch": (220, 60, 50), "missing": (220, 60, 50)}


def _font(size: int) -> ImageFont.ImageFont:
    try:
        return ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10.1
        return ImageFont.load_default()


def contact_sheet(run_dir: Path, repo: Path, verdict: dict, out: Path, thumb_w: int = 480) -> None:
    """One row per capture: capture | golden | diff heatmap, with a status bar."""
    rows = verdict["captures"]
    if not rows:
        return
    th = thumb_w * 9 // 16
    label_h = 28
    head_h = 44
    W = thumb_w * 3 + 16
    H = head_h + len(rows) * (th + label_h + 8)
    sheet = Image.new("RGB", (W, H), (24, 24, 28))
    dr = ImageDraw.Draw(sheet)
    ok = verdict["pass"]
    dr.rectangle([0, 0, W, head_h - 6], fill=(40, 120, 60) if ok else (150, 40, 40))
    s = verdict["summary"]
    dr.text((10, 10), f"{verdict['scenario']}  -  {'PASS' if ok else 'FAIL'}   "
                      f"match {s['match']}  new {s['new']}  mismatch {s['mismatch']}  "
                      f"missing {s['missing']}  checks failed {s['checks_failed']}/{s['checks']}",
            fill=(255, 255, 255), font=_font(18))
    y = head_h
    for r in rows:
        col = _STATUS_COLOR.get(r["status"], (150, 150, 150))
        text = f"{r['name']}  [{r['status']}]"
        if r.get("diff"):
            d = r["diff"]
            text += (f"  ssim {d['ssim']:.4f}  region min {d['region_ssim_min']:.4f}"
                     f" @ {tuple(d['worst_region'])}")
        dr.rectangle([0, y, 6, y + label_h + th], fill=col)
        dr.text((12, y + 4), text, fill=(235, 235, 235), font=_font(16))
        imgs = [run_dir / r["image"] if r.get("image") else None,
                repo / r["golden"] if r.get("golden") else None,
                run_dir / r["heatmap"] if r.get("heatmap") else None]
        for i, p in enumerate(imgs):
            x = 8 + i * thumb_w
            if p is not None and p.exists():
                im = Image.open(p).convert("RGB")
                im.thumbnail((thumb_w - 4, th))
                sheet.paste(im, (x, y + label_h))
            else:
                dr.rectangle([x, y + label_h, x + thumb_w - 4, y + label_h + th - 1],
                             outline=(70, 70, 70))
        y += th + label_h + 8
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)
