"""`ember-dev` — the agent iteration loop for the UE renderer (EPIC_5_PLAN §2, A1).

    ember-dev doctor                       runner health (engine pin, toolchain, GPU, data)
    ember-dev build [--target T]           UBT build, parsed diagnostics (runs/dev/build/last.json)
    ember-dev run-scenario S               harness run -> runs/viz/S/<stamp>/
    ember-dev evaluate S [--run DIR]       diffs + facts + budgets -> verdict.json, contact sheet
    ember-dev loop S                       build + run-scenario + evaluate (the inner loop)
    ember-dev bless S [--run DIR]          accept a run's captures as the new goldens
    ember-dev bundle HCPn --run L=DIR ...  copy labelled runs into checkpoints/HCPn/ for review
    ember-dev regen-assets [--check]       run asset generators headless / check the lock
    ember-dev scenarios                    list render scenarios
"""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path

import typer

from ember.dev import ue
from ember.dev.scenario import find_scenario, load_scenario, scenarios_dir

app = typer.Typer(add_completion=False, help=__doc__, no_args_is_help=True)


def _sc(name: str):
    return load_scenario(find_scenario(ue.repo_root(), name))


def _emit(obj: dict, as_json: bool) -> None:
    if as_json:
        typer.echo(json.dumps(obj, indent=2))


@app.command()
def doctor(as_json: bool = typer.Option(False, "--json")) -> None:
    """Check that this machine can build, run, and capture."""
    from ember.dev.doctor import run_checks

    checks = run_checks(ue.load_engine())
    if as_json:
        typer.echo(json.dumps([c.__dict__ for c in checks], indent=2))
    else:
        colors = {"ok": typer.colors.GREEN, "warn": typer.colors.YELLOW, "fail": typer.colors.RED}
        for c in checks:
            typer.secho(f"{c.status:>4}  {c.name:<14} {c.detail}", fg=colors[c.status])
            if c.fix and c.status != "ok":
                typer.echo(f"      fix: {c.fix}")
    if any(c.status == "fail" for c in checks):
        raise typer.Exit(1)


def _do_build(target: str | None, config: str, as_json: bool) -> ue.BuildResult:
    eng = ue.load_engine()
    res = ue.build(eng, target, config)
    if as_json:
        d = res.to_dict()
        d["warnings"] = d["warnings"][:50]
        _emit(d, True)
    else:
        fg = typer.colors.GREEN if res.ok else typer.colors.RED
        typer.secho(f"build {res.target} {res.config}: {res.result}"
                    f"{' (' + res.reason + ')' if res.reason else ''} in {res.seconds}s - "
                    f"{len(res.errors)} errors, {len(res.warnings)} warnings", fg=fg)
        for d in res.errors[:30]:
            typer.echo(f"  {d.file}({d.line}): {d.kind} {d.code}: {d.message}")
        for line in res.other_errors[:15]:
            typer.echo(f"  {line}")
        if not res.ok:
            typer.echo(f"  log: {res.log}")
    return res


@app.command()
def build(target: str = typer.Option(None, "--target", help="UBT target (default: editor target)."),
          config: str = typer.Option("Development", "--config"),
          as_json: bool = typer.Option(False, "--json")) -> None:
    """Compile the UE project (incremental)."""
    if not _do_build(target, config, as_json).ok:
        raise typer.Exit(1)


@app.command("run-scenario")
def run_scenario(name: str, timeout: int = typer.Option(600, "--timeout"),
                 exposure: float = typer.Option(None, "--exposure",
                                                help="EV100 bias override."),
                 as_json: bool = typer.Option(False, "--json")) -> None:
    """Run the harness for one render scenario (headless, offscreen)."""
    sc = _sc(name)
    if not (sc.world_path / "manifest.json").exists():
        typer.secho(f"world not found: {sc.world_path}", fg=typer.colors.RED)
        raise typer.Exit(2)
    eng = ue.load_engine()
    bias = exposure if exposure is not None else sc.spec.scenario.exposure_bias
    run_dir = ue.run_scenario(eng, sc, timeout_s=timeout, exposure_bias=bias)
    meta = json.loads((run_dir / "run.json").read_text(encoding="utf-8"))
    if as_json:
        _emit(meta, True)
    else:
        fg = typer.colors.GREEN if meta["exit_code"] == 0 else typer.colors.RED
        typer.secho(f"run {sc.name}: exit {meta['exit_code']} in {meta['seconds']}s -> {run_dir}",
                    fg=fg)
        if meta["error"]:
            typer.echo(f"  error: {meta['error']}")
    if meta["exit_code"] != 0:
        raise typer.Exit(1)


def _resolve_run(name: str, run: str | None) -> Path:
    if run:
        return Path(run)
    r = ue.latest_run(name)
    if r is None:
        typer.secho(f"no runs for {name}; run-scenario first", fg=typer.colors.RED)
        raise typer.Exit(2)
    return r


def _print_verdict(v: dict) -> None:
    s = v["summary"]
    fg = typer.colors.GREEN if v["pass"] else typer.colors.RED
    typer.secho(f"{v['scenario']}: {'PASS' if v['pass'] else 'FAIL'} - captures {s['captures']} "
                f"(match {s['match']}, new {s['new']}, mismatch {s['mismatch']}, missing "
                f"{s['missing']}); checks {s['checks'] - s['checks_failed']}/{s['checks']}", fg=fg)
    for c in v["captures"]:
        if c["status"] in ("mismatch", "missing"):
            why = c["diff"]["reason"] if c.get("diff") else "no capture written"
            typer.echo(f"  capture {c['name']}: {c['status']} - {why}")
    for c in v["checks"]:
        if not c["ok"]:
            typer.echo(f"  {c['kind']} {c['name']} @ {c['capture']}: actual {c['actual']!r}, "
                       f"expected {c['expected']}")
    if v["run"].get("error"):
        typer.echo(f"  run error: {v['run']['error']}")
    typer.echo(f"  verdict: {Path(v['run_dir']) / 'verdict.json'}")
    typer.echo(f"  sheet  : {Path(v['run_dir']) / 'contact_sheet.png'}")


@app.command()
def evaluate(name: str, run: str = typer.Option(None, "--run", help="Run dir (default: latest)."),
             as_json: bool = typer.Option(False, "--json")) -> None:
    """Diff captures vs goldens, check facts + budgets; write verdict.json + contact sheet."""
    from ember.dev.evaluate import evaluate_run

    sc = _sc(name)
    v = evaluate_run(ue.repo_root(), sc, _resolve_run(name, run))
    _emit(v, True) if as_json else _print_verdict(v)
    if not v["pass"]:
        raise typer.Exit(1)


@app.command()
def loop(name: str, skip_build: bool = typer.Option(False, "--skip-build"),
         timeout: int = typer.Option(600, "--timeout"),
         as_json: bool = typer.Option(False, "--json")) -> None:
    """The inner loop: build -> run-scenario -> evaluate. Exit 0 only on a passing verdict."""
    from ember.dev.evaluate import evaluate_run

    t0 = time.perf_counter()
    timings = {}
    if not skip_build:
        t = time.perf_counter()
        res = _do_build(None, "Development", False)
        timings["build_s"] = round(time.perf_counter() - t, 1)
        if not res.ok:
            raise typer.Exit(1)
    sc = _sc(name)
    t = time.perf_counter()
    run_dir = ue.run_scenario(ue.load_engine(), sc, timeout_s=timeout,
                              exposure_bias=sc.spec.scenario.exposure_bias)
    timings["run_s"] = round(time.perf_counter() - t, 1)
    t = time.perf_counter()
    v = evaluate_run(ue.repo_root(), sc, run_dir)
    timings["evaluate_s"] = round(time.perf_counter() - t, 1)
    timings["total_s"] = round(time.perf_counter() - t0, 1)
    v["loop_timings"] = timings
    (run_dir / "verdict.json").write_text(json.dumps(v, indent=2), encoding="utf-8")
    if as_json:
        _emit(v, True)
    else:
        _print_verdict(v)
        typer.echo(f"  timings: {timings}")
    if not v["pass"]:
        raise typer.Exit(1)


@app.command()
def bless(name: str, run: str = typer.Option(None, "--run"),
          only: list[str] = typer.Option(None, "--only", help="Capture names (repeatable)."),
          ) -> None:
    """Accept a run's captures as goldens (commit viz/goldens/ afterwards)."""
    from ember.dev.evaluate import bless as do_bless

    sc = _sc(name)
    done = do_bless(ue.repo_root(), sc, _resolve_run(name, run), only)
    typer.secho(f"blessed {len(done)}: {', '.join(done) or '-'}", fg=typer.colors.GREEN)


@app.command()
def bundle(hcp: str, runs: list[str] = typer.Option(..., "--run",
                                                   help="label=run_dir (repeatable, in order)")
           ) -> None:
    """Assemble a checkpoint review bundle: per labelled run, the contact sheet, verdict, plan
    and run metadata go to checkpoints/<HCP>/runs/<NN>-<label>/. The memo is written by hand."""
    import shutil

    out = ue.repo_root() / "checkpoints" / hcp / "runs"
    out.mkdir(parents=True, exist_ok=True)
    index = []
    for i, spec in enumerate(runs, 1):
        label, _, d = spec.partition("=")
        src = Path(d)
        if not src.is_dir():
            typer.secho(f"not a run dir: {src}", fg=typer.colors.RED)
            raise typer.Exit(2)
        dst = out / f"{i:02d}-{label}"
        dst.mkdir(exist_ok=True)
        for name in ("contact_sheet.png", "verdict.json", "plan.json", "run.json",
                     "run_status.json"):
            if (src / name).exists():
                shutil.copyfile(src / name, dst / name)
        v = {}
        if (src / "verdict.json").exists():
            v = json.loads((src / "verdict.json").read_text(encoding="utf-8"))
        index.append({"label": label, "dir": dst.name, "source": str(src),
                      "pass": v.get("pass"), "summary": v.get("summary"),
                      "loop_timings": v.get("loop_timings")})
    (out.parent / "bundle.json").write_text(json.dumps({"hcp": hcp, "runs": index}, indent=2),
                                            encoding="utf-8")
    typer.secho(f"bundle {hcp}: {len(index)} runs -> {out}", fg=typer.colors.GREEN)


@app.command("regen-assets")
def regen_assets(check: bool = typer.Option(False, "--check",
                                           help="Engine-free: is the lock current?")) -> None:
    """Regenerate every .uasset from assets/generators (headless commandlet)."""
    from ember.dev import assets

    if check:
        problems = assets.check_lock()
        for pr in problems:
            typer.secho(f"  {pr}", fg=typer.colors.RED)
        typer.secho("assets: clean" if not problems else f"assets: {len(problems)} problem(s)",
                    fg=typer.colors.GREEN if not problems else typer.colors.RED)
        if problems:
            raise typer.Exit(1)
        return
    res = assets.regen(ue.load_engine())
    for g in res["generators"]:
        fg = typer.colors.GREEN if g["ok"] else typer.colors.RED
        typer.secho(f"{g['script']}: {'ok' if g['ok'] else 'FAILED'} in {g['seconds']}s "
                    f"-> {', '.join(g['outputs'])}", fg=fg)
        if g["error"]:
            typer.echo(f"  {g['error']}")
    if not res["ok"]:
        raise typer.Exit(1)


@app.command()
def scenarios() -> None:
    """List render scenarios."""
    for p in sorted(scenarios_dir(ue.repo_root()).glob("*.toml")):
        try:
            sc = load_scenario(p)
            typer.echo(f"{sc.name:<24} {sc.spec.scenario.description}")
        except Exception as e:  # noqa: BLE001 — listing must survive a broken file
            typer.secho(f"{p.stem:<24} INVALID: {e}", fg=typer.colors.RED)


def main() -> None:  # pragma: no cover
    sys.exit(app())
