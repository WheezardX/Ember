"""Iteration-time ledger and step budgets (Brad, 2026-09-30: "I keep having to catch these").

Every `ember-dev` command appends one line to runs/dev/timing.jsonl (and asset regeneration one
per generator). Budgets live in viz/budgets.toml [steps]: a step over its budget prints a loud
warning naming the step, so slow steps are seen when they happen, not when someone asks.
`ember-dev timing` summarises the ledger (totals per step kind, the slowest, the overruns).

    [steps]
    "run-scenario" = 120        # seconds; keys: command names, or "gen:<script>" for generators
    "gen:*" = 60
"""

from __future__ import annotations

import json
import time
import tomllib
from contextlib import contextmanager
from datetime import UTC, datetime
from pathlib import Path

import typer

LEDGER = Path("runs") / "dev" / "timing.jsonl"


def _repo() -> Path:
    from ember.dev.ue import repo_root

    return repo_root()


def budgets(repo: Path | None = None) -> dict[str, float]:
    p = (repo or _repo()) / "viz" / "budgets.toml"
    if not p.exists():
        return {}
    steps = tomllib.loads(p.read_text(encoding="utf-8")).get("steps", {})
    return {k: float(v) for k, v in steps.items()}


def budget_for(kind: str, table: dict[str, float]) -> float | None:
    if kind in table:
        return table[kind]
    if ":" in kind and kind.split(":", 1)[0] + ":*" in table:
        return table[kind.split(":", 1)[0] + ":*"]
    return None


def record(kind: str, label: str, seconds: float, ok: bool = True, note: str = "",
           repo: Path | None = None, warn: bool = True, nested: bool = False) -> dict:
    """Append a ledger entry; print a warning if the step ran over its budget."""
    repo = repo or _repo()
    b = budget_for(kind, budgets(repo))
    over = b is not None and seconds > b
    rec = {"utc": datetime.now(UTC).isoformat(timespec="seconds"), "kind": kind, "label": label,
           "seconds": round(seconds, 1), "ok": ok, "budget": b, "over": over, "note": note,
           "nested": nested}
    path = repo / LEDGER
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(rec) + "\n")
    if over and warn:
        typer.secho(f"TIME BUDGET: {kind} {label} took {seconds:.0f}s (budget {b:.0f}s) - "
                    "fix it or say why it is worth it", fg=typer.colors.YELLOW, bold=True, err=True)
    return rec


@contextmanager
def step(kind: str, label: str = ""):
    t0 = time.time()
    ok = True
    try:
        yield
    except BaseException as e:
        # typer.Exit(0) is a normal exit
        ok = isinstance(e, typer.Exit) and getattr(e, "exit_code", 1) == 0
        raise
    finally:
        record(kind, label, time.time() - t0, ok)


def load(repo: Path | None = None, since: str | None = None) -> list[dict]:
    path = (repo or _repo()) / LEDGER
    if not path.exists():
        return []
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            continue
        if since and r["utc"] < since:
            continue
        out.append(r)
    return out


def summary(rows: list[dict]) -> dict:
    by: dict[str, dict] = {}
    for r in rows:
        k = r["kind"]
        e = by.setdefault(k, {"n": 0, "seconds": 0.0, "max": 0.0, "over": 0})
        e["n"] += 1
        e["seconds"] += r["seconds"]
        e["max"] = max(e["max"], r["seconds"])
        e["over"] += 1 if r.get("over") else 0
    # top-level time only: generator runs sit inside regen-assets, scenario runs inside regress
    total = sum(r["seconds"] for r in rows
                if not r.get("nested") and not r["kind"].startswith("gen:"))
    return {"total_s": round(total, 1),
            "by_kind": dict(sorted(by.items(), key=lambda kv: -kv[1]["seconds"])),
            "slowest": sorted(rows, key=lambda r: -r["seconds"])[:10],
            "overruns": [r for r in rows if r.get("over")]}
