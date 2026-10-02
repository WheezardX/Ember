"""`ember external ...` - external fire sources (docs/plans/EXTERNAL_SOURCES_PLAN.md)."""
from __future__ import annotations

import json
from pathlib import Path

import typer

external_app = typer.Typer(add_completion=False,
                           help="External fire sources: their file in, a faithful replay out.")

TQ26 = "store/incidents/dfa477f2-305c-49ca-b82e-2af072ab55f8/external/pyrecast-elmfire"


@external_app.command("pyrecast")
def pyrecast(run_dir: Path = typer.Argument(..., help="A PyreCast run dir (…/{slug}/{run_ts})."),
             pct: str = typer.Option("10,50,90", "--pct", help="Percentiles."),
             out: Path = typer.Option(None, "--out", help="Default: <mirror>/timelines/<run_ts>/")
             ) -> None:
    """PyreCast forecast run -> fire timelines (one per percentile)."""
    from ember.external.pyrecast import load_run

    for p in (int(x) for x in pct.split(",")):
        tl = load_run(run_dir, p)
        d = (out or run_dir.parents[2] / "timelines" / run_dir.name) / f"p{p}"
        tl.write(d)
        typer.echo(f"  p{p}: {json.dumps(tl.summary())} -> {d}")


@external_app.command("x1-map-video")
def x1_map_video() -> None:
    """The forecast as a conventional 2D map beside our render (map view + cinematic) ->
    store/review/X1/ (internal only). Needs the X1 bundle."""
    from ember.dev.ue import repo_root
    from ember.external import x1

    for v in x1.map_videos(repo_root()):
        typer.echo(f"  {v}")


@external_app.command("x1-bundle")
def x1_bundle(run_ts: str = typer.Option("20260820_051100", "--run", help="Run timestamp."),
              no_render: bool = typer.Option(False, "--no-render", help="Stop before rendering.")
              ) -> None:
    """External Sources X1: the Three Queens forecast, as issued, beside what happened ->
    store/review/X1/ (internal only), timed end to end."""
    from ember.dev.ue import repo_root
    from ember.external import x1

    repo = repo_root()
    run_dir = repo / TQ26 / "forecast_archive" / "wa-three-queens" / run_ts
    s = x1.build(repo, run_dir, repo / "store" / "sim" / "hist-three-queens-2026-ir.ewp",
                 repo / "runs" / "tq26" / "tq26-ir-playback.replay.json", render=not no_render)
    typer.echo(json.dumps({k: s[k] for k in ("run", "times", "fidelity")}, indent=1))
