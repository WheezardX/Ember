"""Iteration-time ledger and step budgets (ember/dev/timing.py). Engine-free."""

from __future__ import annotations

from pathlib import Path

from ember.dev import timing


def _repo(tmp_path: Path) -> Path:
    (tmp_path / "viz").mkdir()
    (tmp_path / "viz" / "budgets.toml").write_text(
        '[steps]\n"run-scenario" = 120\n"gen:*" = 60\n"gen:veg_species.py" = 1500\n',
        encoding="utf-8")
    return tmp_path


def test_budget_lookup_exact_and_wildcard(tmp_path: Path):
    b = timing.budgets(_repo(tmp_path))
    assert timing.budget_for("run-scenario", b) == 120
    assert timing.budget_for("gen:m_veg.py", b) == 60          # wildcard
    assert timing.budget_for("gen:veg_species.py", b) == 1500  # exact beats wildcard
    assert timing.budget_for("build", b) is None


def test_record_flags_overruns_and_summarises(tmp_path: Path, capsys):
    repo = _repo(tmp_path)
    timing.record("run-scenario", "S_a", 30.0, repo=repo)
    timing.record("run-scenario", "S_b", 200.0, repo=repo)        # over
    timing.record("gen:veg_species.py", "", 1242.0, repo=repo)    # within its own budget
    timing.record("regen-assets", "", 1250.0, repo=repo)          # no budget: never over
    timing.record("run-scenario", "S_c", 40.0, repo=repo, nested=True)   # inside a regress
    err = capsys.readouterr().err
    assert "TIME BUDGET" in err and "S_b" in err and "S_a" not in err
    rows = timing.load(repo)
    assert [r["over"] for r in rows] == [False, True, False, False, False]
    s = timing.summary(rows)
    assert s["by_kind"]["run-scenario"]["n"] == 3 and s["by_kind"]["run-scenario"]["over"] == 1
    assert s["total_s"] == 30.0 + 200.0 + 1250.0                  # generator + nested time excluded
    assert s["slowest"][0]["kind"] == "regen-assets"


def test_load_since(tmp_path: Path):
    repo = _repo(tmp_path)
    timing.record("build", "", 5.0, repo=repo)
    assert timing.load(repo, since="2999-01-01") == []
    assert len(timing.load(repo, since="2000-01-01")) == 1
