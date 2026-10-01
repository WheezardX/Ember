"""Progression archive (Brad, 2026-09-30: "showing a timeline progression of our work can be very
useful to pitching to investors ... err on the side of lots of progression data").

checkpoints/progression/
    <scenario>/<capture>/<YYYY-MM-DD>_<HHMM>_<sha7>.jpg   one image per blessed / archived version
    index.json      every entry: view, date, commit, message, source
    timeline.html   per view, oldest -> newest (built by `timeline`)

Sources:
  * `backfill`: every version of every golden in git history (each bless overwrote the golden,
    git kept the old one) - the timeline back to day one.
  * `archive`: the latest run of a scenario (called by `ember-dev regress --full`, i.e. before
    every commit) - views that changed since their last archived version.
Images are 1600 px wide JPEGs (~300 KB).
"""

from __future__ import annotations

import io
import json
import subprocess
from datetime import datetime
from pathlib import Path

from PIL import Image

ROOT = Path("checkpoints") / "progression"
WIDTH = 1600


def _git(repo: Path, *args: str, binary: bool = False):
    p = subprocess.run(["git", *args], cwd=repo, capture_output=True, check=True)
    return p.stdout if binary else p.stdout.decode("utf-8", "replace")


def _save(img_bytes: bytes, dst: Path) -> None:
    im = Image.open(io.BytesIO(img_bytes)).convert("RGB")
    if im.width > WIDTH:
        im = im.resize((WIDTH, round(im.height * WIDTH / im.width)), Image.LANCZOS)
    dst.parent.mkdir(parents=True, exist_ok=True)
    im.save(dst, "JPEG", quality=85)


def _index(repo: Path) -> dict:
    p = repo / ROOT / "index.json"
    return json.loads(p.read_text(encoding="utf-8")) if p.exists() else {"entries": []}


def _write_index(repo: Path, idx: dict) -> None:
    idx["entries"].sort(key=lambda e: (e["view"], e["date"]))
    p = repo / ROOT / "index.json"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(idx, indent=1) + "\n", encoding="utf-8")


def backfill(repo: Path) -> int:
    """Every version of every golden in git history -> the archive. Idempotent."""
    idx = _index(repo)
    have = {(e["view"], e["commit"]) for e in idx["entries"]}
    log = _git(repo, "log", "--format=%H%x09%aI%x09%s", "--name-only", "--diff-filter=AM",
               "--", "viz/goldens")
    added = 0
    sha = date = msg = None
    for line in log.splitlines():
        if "\t" in line:
            sha, date, msg = line.split("\t", 2)
            continue
        if not line.strip() or not line.endswith(".png"):
            continue
        parts = Path(line).parts                      # viz/goldens/<scenario>/<capture>.png
        if len(parts) < 4:
            continue
        view = f"{parts[2]}/{Path(parts[3]).stem}"
        if (view, sha[:7]) in have:
            continue
        data = _git(repo, "show", f"{sha}:{line}", binary=True)
        d = datetime.fromisoformat(date)
        rel = ROOT / parts[2] / Path(parts[3]).stem / f"{d:%Y-%m-%d_%H%M}_{sha[:7]}.jpg"
        _save(data, repo / rel)
        idx["entries"].append({"view": view, "date": d.isoformat(timespec="minutes"),
                               "commit": sha[:7], "message": msg, "source": "golden",
                               "image": str(rel).replace("\\", "/")})
        have.add((view, sha[:7]))
        added += 1
    _write_index(repo, idx)
    return added


def archive_run(repo: Path, scenario: str, run_dir: Path, note: str = "") -> int:
    """Archive a run's captures that differ from the view's last archived image."""
    import numpy as np

    idx = _index(repo)
    head = _git(repo, "rev-parse", "--short=7", "HEAD").strip()
    now = datetime.now().astimezone()
    added = 0
    for cap in sorted((run_dir / "captures").glob("*.png")):
        view = f"{scenario}/{cap.stem}"
        prev = [e for e in idx["entries"] if e["view"] == view]
        new = Image.open(cap).convert("RGB")
        if prev:
            last = Image.open(repo / prev[-1]["image"]).convert("L").resize((320, 180))
            cur = new.convert("L").resize((320, 180))
            if np.abs(np.asarray(last, float) - np.asarray(cur, float)).mean() < 1.5:
                continue                              # unchanged (within run-to-run noise)
        rel = ROOT / scenario / cap.stem / f"{now:%Y-%m-%d_%H%M}_{head}.jpg"
        buf = io.BytesIO()
        new.save(buf, "PNG")
        _save(buf.getvalue(), repo / rel)
        idx["entries"].append({"view": view, "date": now.isoformat(timespec="minutes"),
                               "commit": head, "message": note, "source": "run",
                               "image": str(rel).replace("\\", "/")})
        added += 1
    _write_index(repo, idx)
    return added


def timeline_html(repo: Path) -> Path:
    """A self-contained page: per view, its versions oldest -> newest with date + commit message."""
    import html

    idx = _index(repo)
    views: dict[str, list] = {}
    for e in idx["entries"]:
        views.setdefault(e["view"], []).append(e)
    rows = []
    for view in sorted(views, key=lambda v: -len(views[v])):
        es = sorted(views[view], key=lambda e: e["date"])
        cells = "".join(
            f'<figure><img loading="lazy" '
            f'src="{html.escape(Path(e["image"]).relative_to(ROOT).as_posix())}">'
            f'<figcaption><b>{e["date"][:10]}</b> {html.escape(e["commit"])}'
            f'<br>{html.escape(e["message"][:90])}'
            f'</figcaption></figure>' for e in es)
        rows.append(f'<section><h2>{html.escape(view)} <span>{len(es)} versions</span></h2>'
                    f'<div class="strip">{cells}</div></section>')
    page = ("<!doctype html><meta charset=utf-8><title>Ember progression</title><style>"
            "body{font:14px system-ui;background:#16181b;color:#e6e2da;margin:0;padding:20px}"
            "h1{font-weight:600}h2{font-size:16px;margin:24px 0 8px}"
            "h2 span{color:#999;font-weight:400}"
            ".strip{display:flex;gap:10px;overflow-x:auto;padding-bottom:8px}"
            "figure{margin:0;flex:0 0 360px}img{width:360px;border-radius:3px;display:block}"
            "figcaption{font-size:12px;color:#aaa;margin-top:4px}</style>"
            f"<h1>Ember progression</h1><p>{len(idx['entries'])} images, {len(views)} views. "
            "Each row: one view, oldest left.</p>" + "".join(rows))
    out = repo / ROOT / "timeline.html"
    out.write_text(page, encoding="utf-8")
    return out
