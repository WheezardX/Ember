"""Checkpoint memo skeleton generator (plan G2). One page per checkpoint under checkpoints/CPn/."""

from __future__ import annotations

from pathlib import Path

RESULT_PLACEHOLDER = "PASS | FAIL | PARTIAL — fill in"


def checkpoint_memo_template(cp: str, title: str, artifacts: list[tuple[str | Path, str]],
                             gate: str, out_md: str | Path, *,
                             findings: list[str] | None = None,
                             upstream: list[str] | None = None) -> Path:
    """Write a memo skeleton: Result line, gate, artifact table, findings, upstream, verdict."""
    out_md = Path(out_md)
    out_md.parent.mkdir(parents=True, exist_ok=True)
    rows = "\n".join(f"| `{Path(p).as_posix()}` | {c} |" for p, c in artifacts) or "| — | — |"
    findings_md = "\n".join(f"- {f}" for f in (findings or ["(fill in)"]))
    upstream_md = "\n".join(f"- {u}" for u in (upstream or ["(none filed)"]))
    text = f"""# {cp} — {title}

**Result:** {RESULT_PLACEHOLDER}
**Gate:** {gate}

## Artifacts
| artifact | what it shows |
|---|---|
{rows}

## Findings
{findings_md}

## Upstream tickets (Epics 1–3)
{upstream_md}

## Verdict
(fill in — one paragraph: does the gate hold, and what is the caveat)
"""
    out_md.write_text(text, encoding="utf-8")
    return out_md
