"""Scene facts (A2) — the machine-checkable half of every capture.

The UE `USceneFactsSubsystem` writes one JSON per capture point, `facts/<capture>.json`,
and (when the scenario asks for a perf window) `facts/perf.json`. Schema v1 is documented
in docs/viz/scene-facts.md; this module reads it, evaluates scenario asserts against it,
and checks perf budgets (viz/budgets.toml).
"""

from __future__ import annotations

import json
import operator
import tomllib
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

FACTS_FORMAT = "ember-scene-facts"
FACTS_VERSION = 1

_OPS = {"==": operator.eq, "!=": operator.ne, ">=": operator.ge, "<=": operator.le,
        ">": operator.gt, "<": operator.lt}


def load_facts(path: Path) -> dict:
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    if d.get("format") != FACTS_FORMAT:
        raise ValueError(f"{path}: not a scene-facts file (format={d.get('format')!r})")
    if d.get("version") != FACTS_VERSION:
        raise ValueError(f"{path}: scene-facts version {d.get('version')} unsupported")
    return d


_MISSING = object()


def lookup(facts: dict, dotted: str) -> Any:
    cur: Any = facts
    for part in dotted.split("."):
        if isinstance(cur, dict) and part in cur:
            cur = cur[part]
        else:
            return _MISSING
    return cur


@dataclass
class Check:
    kind: str          # "assert" | "budget"
    name: str          # fact path or budget key
    capture: str       # capture name, or "perf"
    ok: bool
    actual: Any
    expected: str

    def to_dict(self) -> dict:
        d = asdict(self)
        if d["actual"] is _MISSING:
            d["actual"] = None
        return d


def eval_assert(facts: dict, capture: str, fact: str, op: str, value: Any) -> Check:
    actual = lookup(facts, fact)
    if actual is _MISSING:
        return Check("assert", fact, capture, False, None, f"{op} {value!r} (fact missing)")
    try:
        ok = bool(_OPS[op](actual, value))
    except TypeError:
        ok = False
    return Check("assert", fact, capture, ok, actual, f"{op} {value!r}")


# Budget keys are "<dotted fact>_max" / "<dotted fact>_min" with dots written as "__"
# in TOML-friendly form, e.g. perf__frame_ms_p95_max = 16.7  ->  perf.frame_ms_p95 <= 16.7
def load_budgets(path: Path) -> dict[str, dict[str, float]]:
    if not path.exists():
        return {}
    with open(path, "rb") as f:
        return tomllib.load(f)


def eval_budgets(budget: dict[str, float], facts: dict, capture: str) -> list[Check]:
    out = []
    for key, limit in budget.items():
        if key.endswith("_max"):
            fact, op = key[:-4].replace("__", "."), "<="
        elif key.endswith("_min"):
            fact, op = key[:-4].replace("__", "."), ">="
        else:
            continue  # descriptive keys (e.g. "description") are allowed
        c = eval_assert(facts, capture, fact, op, limit)
        c.kind = "budget"
        c.name = key
        out.append(c)
    return out
