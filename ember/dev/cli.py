"""`ember-dev` — the agent iteration loop for the UE renderer (EPIC_5_PLAN §2, A1).

    ember-dev doctor                       runner health (engine pin, toolchain, GPU, data)
    ember-dev build [--target T]           UBT build, parsed diagnostics (runs/dev/build/last.json)
    ember-dev run-scenario S               harness run -> runs/viz/S/<stamp>/
    ember-dev evaluate S [--run DIR]       diffs + facts + budgets -> verdict.json, contact sheet
    ember-dev loop S                       build + run-scenario + evaluate (the inner loop)
    ember-dev bless S [--run DIR]          accept a run's captures as the new goldens
    ember-dev bundle HCPn --run L=DIR ...  copy labelled runs into checkpoints/HCPn/ for review
    ember-dev regen-assets [--check]       run asset generators headless / check the lock
    ember-dev regress [--full]             quick tier (~3 min) after a change; --full before a commit
    ember-dev timing [--since D]           where the iteration time went (budgets: viz/budgets.toml [steps])
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


@app.command()
def play(name: str,
         bookmark: str = typer.Option(None, "--bookmark", "-b",
                                      help="Start pose (default: the first capture's)."),
         res: str = typer.Option("1920x1080", "--res", help="Window size WxH."),
         fullscreen: bool = typer.Option(False, "--fullscreen"),
         wait: bool = typer.Option(False, "--wait", help="Block until the client exits.")) -> None:
    """Launch the client on a scenario and fly around in it (free camera, no captures)."""
    sc = _sc(name)
    if not (sc.world_path / "manifest.json").exists():
        typer.secho(f"world not found: {sc.world_path}", fg=typer.colors.RED)
        raise typer.Exit(2)
    try:
        w, h = (int(v) for v in res.lower().split("x"))
    except ValueError:
        typer.secho(f"--res wants WxH, got {res!r}", fg=typer.colors.RED)
        raise typer.Exit(2) from None
    eng = ue.load_engine()
    try:
        run_dir, proc = ue.play(eng, sc, bookmark=bookmark, resolution=(w, h),
                                fullscreen=fullscreen,
                                exposure_bias=sc.spec.scenario.exposure_bias)
    except ValueError as e:
        typer.secho(str(e), fg=typer.colors.RED)
        raise typer.Exit(2) from None
    typer.echo(f"playing {sc.name} (pid {proc.pid}); log {run_dir / 'log' / 'Ember.log'}")
    typer.echo("  mouse look | WASD | E/Space up, Q/C down | Shift x4, Ctrl x0.25 | wheel speed")
    typer.echo("  G walk/fly | 1-5 sun | P play fire, , . -/+1 h, [ ] rate | L lamp | H help | Esc quit")
    if wait:
        proc.wait()


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
def regress(full: bool = typer.Option(False, "--full", help="Every scenario (before commits)."),
            only: list[str] = typer.Option(None, "--only", help="These scenarios (repeatable)."),
            ) -> None:
    """Run + evaluate a regression tier (viz/regress.toml): quick by default, --full before a
    commit. Stops at the first material compile failure (renders would be meaningless). Prints a
    time table; the tier is checked against its time budget."""
    import tomllib

    from ember.dev import timing
    from ember.dev.evaluate import evaluate_run

    repo = ue.repo_root()
    tiers = tomllib.loads((repo / "viz" / "regress.toml").read_text(encoding="utf-8"))
    tier = "full" if full else "quick"
    names = list(only) if only else tiers[tier]["scenarios"]
    eng = ue.load_engine()
    t_all = time.time()
    rows, failed = [], []
    for n in names:
        sc = _sc(n)
        t0 = time.time()
        run_dir = ue.run_scenario(eng, sc, timeout_s=900, exposure_bias=sc.spec.scenario.exposure_bias)
        t_run = time.time() - t0
        meta = json.loads((run_dir / "run.json").read_text(encoding="utf-8"))
        timing.record("run-scenario", n, t_run, meta["exit_code"] == 0, repo=repo, nested=True)
        if "failed to compile" in (meta.get("error") or ""):
            typer.secho(f"{n}: STOP - {meta['error']}", fg=typer.colors.RED, bold=True)
            failed.append(n)
            rows.append((n, t_run, 0.0, "COMPILE FAIL", ""))
            break
        t1 = time.time()
        v = evaluate_run(repo, sc, run_dir)
        t_eval = time.time() - t1
        s = v["summary"]
        checks_ok = s["checks_failed"] == 0 and s["run_exit_code"] == 0
        imgs = f"{s['match']}/{s['captures']} match" + (f", {s['mismatch']} changed" if s["mismatch"] else "")
        status = ("PASS" if v["pass"] else ("IMAGES" if checks_ok else "FAIL"))
        if status == "FAIL":
            failed.append(n)
        rows.append((n, t_run, t_eval, status, imgs))
        typer.secho(f"{n:22s} {status:7s} {imgs:28s} run {t_run:5.0f}s eval {t_eval:4.0f}s",
                    fg={"PASS": typer.colors.GREEN, "IMAGES": typer.colors.YELLOW}.get(status, typer.colors.RED))
    total = time.time() - t_all
    timing.record(f"regress:{tier if not only else 'only'}", ",".join(names)[:80], total,
                  not failed, repo=repo)
    typer.echo(f"\n{tier if not only else 'selected'}: {len(rows)} scenario(s) in {total / 60:.1f} min "
               f"(runs {sum(r[1] for r in rows) / 60:.1f}, evaluate {sum(r[2] for r in rows) / 60:.1f}); "
               f"IMAGES = checks pass, captures changed (bless if intended)")
    if failed:
        typer.secho(f"failed: {', '.join(failed)}", fg=typer.colors.RED)
        raise typer.Exit(1)


@app.command("timing")
def timing_cmd(since: str = typer.Option(None, "--since", help="ISO date/time (UTC), e.g. 2026-09-30"),
               ) -> None:
    """Where the iteration time went (runs/dev/timing.jsonl): per step kind, slowest, overruns."""
    from ember.dev import timing

    rows = timing.load(since=since)
    if not rows:
        typer.echo("no timing entries")
        return
    s = timing.summary(rows)
    typer.echo(f"{len(rows)} entries, {s['total_s'] / 60:.1f} min of ember-dev time")
    typer.echo(f"{'step':24s} {'n':>4s} {'total min':>9s} {'max s':>7s} {'over':>5s}")
    for k, e in s["by_kind"].items():
        typer.echo(f"{k:24s} {e['n']:4d} {e['seconds'] / 60:9.1f} {e['max']:7.0f} {e['over']:5d}")
    if s["overruns"]:
        typer.secho("\nover budget:", fg=typer.colors.YELLOW)
        for r in s["overruns"][-15:]:
            typer.echo(f"  {r['utc']}  {r['kind']} {r['label']}: {r['seconds']:.0f}s (budget {r['budget']:.0f}s)")


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
        if (src / "orbits").is_dir():
            shutil.copytree(src / "orbits", dst / "orbits", dirs_exist_ok=True)
        v = {}
        if (src / "verdict.json").exists():
            v = json.loads((src / "verdict.json").read_text(encoding="utf-8"))
        index.append({"label": label, "dir": dst.name, "source": str(src),
                      "pass": v.get("pass"), "summary": v.get("summary"),
                      "loop_timings": v.get("loop_timings")})
    (out.parent / "bundle.json").write_text(json.dumps({"hcp": hcp, "runs": index}, indent=2),
                                            encoding="utf-8")
    typer.secho(f"bundle {hcp}: {len(index)} runs -> {out}", fg=typer.colors.GREEN)


@app.command("hillshade-compare")
def hillshade_compare(name: str, capture: str = typer.Option("topdown", "--capture"),
                      run: str = typer.Option(None, "--run")) -> None:
    """Top-down render vs Terrain's derived hillshade, side by side (HCP1)."""
    from ember.dev.hillshade import compare

    sc = _sc(name)
    run_dir = _resolve_run(name, run)
    res = compare(run_dir, capture, sc.world_path / "derived" / "hillshade.cog.tif",
                  run_dir / f"hillshade_compare_{capture}.png")
    typer.secho(f"hillshade compare -> {res['out']}", fg=typer.colors.GREEN)
    typer.echo(f"  footprint {res['footprint_m']} m, camera {res['camera_height_m']} m up")


@app.command()
def water(region: str = typer.Argument(..., help="Terrain region, e.g. three_queens_2026"),
          ) -> None:
    """Derive the water layer (bodies + per-tile levels) for a Terrain region -> Ember store."""
    from ember.dev import water as W
    from ember.dev.scenario import terrain_store_root

    region_dir = terrain_store_root() / region
    out = W.render_store(ue.repo_root(), region) / "water"
    idx = W.build(region_dir, out)
    big = sorted(idx["bodies"], key=lambda b: -b["cells"])[:5]
    typer.secho(f"water {region}: {len(idx['bodies'])} bodies, {len(idx['tiles'])} tiles -> {out}",
                fg=typer.colors.GREEN)
    for b in big:
        typer.echo(f"  body {b['id']}: {b['cells']} cells ({b['holes']} holes), "
                   f"level {b['level_m']} m via {b['method']}")


@app.command("synth-fire")
def synth_fire(region: str = typer.Argument(..., help="Terrain region, e.g. three_queens_2026"),
               hours: int = typer.Option(48, "--hours"),
               ignition: str = typer.Option("0.5,0.5", "--ignition",
                                            help="ignition as data-extent fractions x,y")) -> None:
    """Write a synthetic wind-driven fire replay (.ess + .replay.json) over a region."""
    from ember.dev import firesynth, water
    from ember.dev.scenario import terrain_store_root

    fx, fy = (float(v) for v in ignition.split(","))
    out = water.render_store(ue.repo_root(), region) / "fire"
    path = firesynth.synth(terrain_store_root() / region, out, ignition_frac=(fx, fy), hours=hours)
    typer.secho(f"synthetic fire {region}: {path}", fg=typer.colors.GREEN)


@app.command()
def split(name: str = typer.Argument(..., help="Replay scenario, e.g. S_jolly_fire"),
          left: Path = typer.Option(..., "--left", help="Epic 4 2D playback MP4"),
          orbit: str = typer.Option("map_timelapse", "--orbit"),
          bookmark: str = typer.Option("map", "--bookmark",
                                       help="the orbit's straight-down bookmark"),
          run: str = typer.Option(None, "--run", help="Run dir (default: latest)."),
          out: Path = typer.Option(None, "--out")) -> None:
    """2D | 3D split MP4: Epic 4's 2D playback beside the renderer's top-down timelapse."""
    from ember.dev import split as sp

    run_dir = _resolve_run(name, run)
    sc = _sc(name)
    crop = sp.grid_crop(sc, run_dir, bookmark)
    dst = out or run_dir / "orbits" / f"{orbit}_split.mp4"
    sp.compose(left, run_dir / "orbits" / f"{orbit}.mp4", crop, dst)
    typer.secho(f"split {dst} (3D crop {crop})", fg=typer.colors.GREEN)


@app.command("wind-vane")
def wind_vane(name: str = typer.Argument(..., help="Replay scenario with a fixed-heading timelapse"),
              orbit: str = typer.Option(..., "--orbit"),
              run: str = typer.Option(None, "--run", help="Run dir (default: latest)."),
              out: Path = typer.Option(None, "--out")) -> None:
    """Burn the stream's wind into a timelapse MP4 as a vane (plume vs wind check, HCP4)."""
    from ember.dev import windvane

    dst = windvane.overlay(_sc(name), _resolve_run(name, run), orbit, out)
    typer.secho(f"wind vane {dst}", fg=typer.colors.GREEN)


@app.command()
def naip(region: str = typer.Argument(..., help="Terrain region, e.g. three_queens_2026"),
         res: float = typer.Option(10.0, "--res", help="Mosaic resolution (m); overviews only"),
         ) -> None:
    """Fetch NAIP colour reference for a region (overviews only; metered in the ledger)."""
    from ember.dev import naip as N
    from ember.dev.scenario import terrain_store_root

    from ember.dev.water import render_store

    region_dir = terrain_store_root() / region
    out = render_store(ue.repo_root(), region) / "naip"
    prov = N.fetch(region_dir, out, res)
    typer.secho(f"naip {region}: {prov['year']}, {len(prov['items'])} items, coverage "
                f"{prov['coverage']:.1%} -> {out}", fg=typer.colors.GREEN)


@app.command("naip-probe")
def naip_probe(name: str, run: str = typer.Option(None, "--run", help="Run dir (default: latest)."),
               ) -> None:
    """Render vs NAIP colour per fuel group for a straight-down probe scenario (advisory)."""
    from ember.dev import naip as N
    from ember.dev.water import render_store

    sc = _sc(name)
    run_dir = _resolve_run(name, run)
    tif = render_store(ue.repo_root(), sc.world_path.name) / "naip" / "naip_rgb.tif"
    rep = N.probe(run_dir, tif, sc.world_path / "fuels" / "fbfm40.cog.tif",
                  [c.name for c in sc.spec.captures])
    typer.echo(f"brightness gain (auto exposure) {rep['brightness_gain']}")
    typer.echo(f"{'group':20s} {'cells':>6s}  {'dE':>5s} {'dE*':>5s}  render L a b -> NAIP L a b")
    for g, v in rep["groups"].items():
        typer.echo(f"{g:20s} {v['cells']:6d}  {v['delta_e']:5.1f} {v['delta_e_brightness_matched']:5.1f}"
                   f"  {v['render_lab']} -> {v['naip_lab']}")
    typer.secho(f"report {run_dir / 'naip_probe.json'}, sheet {run_dir / 'naip_probe.jpg'}",
                fg=typer.colors.GREEN)


@app.command("ref-camera")
def ref_camera(photo: Path = typer.Argument(..., help="A photo with EXIF GPS"),
               name: str = typer.Option(None, "--name", help="Bookmark name (default: file stem)")
               ) -> None:
    """Print an absolute-camera bookmark for a reference photo (EXIF GPS / bearing / focal)."""
    from ember.dev import refcam

    r = refcam.bookmark_from_photo(photo, name)
    if r["taken"]:
        typer.echo(f"# taken {r['taken']} (pick the nearest sun preset)")
    for n in r["notes"]:
        typer.secho(f"# NOTE {n}", fg=typer.colors.YELLOW)
    typer.echo(refcam.bookmark_toml(r["bookmark"]))


@app.command("ref-pair")
def ref_pair(name: str, run: str = typer.Option(None, "--run", help="Run dir (default: latest).")
             ) -> None:
    """photo | render | edge-overlay images for every capture with a `reference` photo."""
    from ember.dev import refcam

    run_dir = _resolve_run(name, run)
    for e in refcam.ref_pairs(_sc(name), run_dir, ue.repo_root()):
        if e["pair"]:
            typer.secho(f"{e['capture']}: {e['pair']}", fg=typer.colors.GREEN)
        else:
            typer.secho(f"{e['capture']}: {e['error']}", fg=typer.colors.RED)


@app.command("forest-report")
def forest_report(region: str = typer.Argument(..., help="Terrain region, e.g. three_queens_2026"),
                  out: str = typer.Option(None, "--out",
                                          help="Output dir (default runs/dev/forest/<region>)."),
                  as_json: bool = typer.Option(False, "--json")) -> None:
    """Species / density / height / crown-cover statistics of a region's scatter (HCP2)."""
    from ember.dev import forest
    from ember.dev.scenario import terrain_store_root

    out_dir = Path(out) if out else ue.repo_root() / "runs" / "dev" / "forest" / region
    rep = forest.write(terrain_store_root() / region, out_dir)
    if as_json:
        _emit(rep, True)
        return
    typer.secho(f"forest {region}: {rep['instances']:,} trees, {rep['instances_per_ha']}/ha, "
                f"{rep['outside_dem_mask']:,} outside the DEM mask -> {out_dir}",
                fg=typer.colors.GREEN)
    for s in rep["species"]:
        typer.echo(f"  {s['key']:<24} {s['count']:>9,}  share {s['share_of_group']} "
                   f"(expected {s['expected_share']})  mean H {s['height_m']['mean']} m")
    for d in rep["density_by_cc"]:
        typer.echo(f"  CC {d['cc_pct'][0]:>2}-{d['cc_pct'][1]:<3}% {d['trees_per_ha']:>6}/ha "
                   f"(exp {d['expected_trees_per_cell'] * 100:.0f})  cover rendered "
                   f"{d['rendered_crown_cover_pct']}% vs LANDFIRE {d['landfire_cc_pct']}%")


@app.command("regen-assets")
def regen_assets(check: bool = typer.Option(False, "--check",
                                           help="Engine-free: is the lock current?"),
                 force: bool = typer.Option(False, "--force",
                                            help="Run every generator, changed or not.")) -> None:
    """Regenerate .uassets from assets/generators (headless commandlet); unchanged generators
    are skipped unless --force."""
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
    res = assets.regen(ue.load_engine(), force=force)
    for g in res["generators"]:
        fg = typer.colors.GREEN if g["ok"] else typer.colors.RED
        if g.get("skipped"):
            typer.echo(f"{g['script']}: unchanged, skipped")
            continue
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
    # Every command is timed into runs/dev/timing.jsonl (budgets: viz/budgets.toml [steps]).
    from ember.dev import timing

    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    label = " ".join(a for a in sys.argv[2:] if not a.startswith("-"))[:80]
    known = set(typer.main.get_command(app).commands)
    # play: interactive (ends when Brad quits); timing / scenarios: instant
    # regress records its own tier entry (regress:quick / :full)
    untimed = {"timing", "scenarios", "play", "regress"} | ({cmd} if cmd not in known else set())
    t0 = time.time()
    code = 0
    import click

    try:
        rv = app(standalone_mode=False)
        code = rv if isinstance(rv, int) else 0
    except SystemExit as e:
        code = e.code if isinstance(e.code, int) else 1
    except click.exceptions.Exit as e:
        code = e.exit_code
    except click.ClickException as e:        # usage errors: print as typer would
        e.show()
        code = e.exit_code
    except click.exceptions.Abort:
        code = 1
    except Exception:
        code = 1
        if cmd not in untimed:
            timing.record(cmd, label, time.time() - t0, ok=False)
        raise
    if cmd not in untimed:
        timing.record(cmd, label, time.time() - t0, ok=code == 0)
    sys.exit(code)
