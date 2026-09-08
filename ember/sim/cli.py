"""`ember sim ...` — Python-side sim tooling (export packs, synthetic worlds, render runs)."""

from __future__ import annotations

import re
import time
from pathlib import Path

import typer

sim_app = typer.Typer(add_completion=False, help="Sim tooling: world packs, synthetic worlds, "
                                                 "debug renders (the C++ core is `embersim`).")


@sim_app.command()
def export(
    irwin: str = typer.Option(None, "--irwin", help="IRWIN incident id."),
    historic: str = typer.Option(None, "--historic", help="Historic fire id."),
    bundle: str = typer.Option(None, "--bundle", help="Explicit scenario.bundle.json path."),
    store_root: str = typer.Option("store", "--store", help="Store root."),
    out: str = typer.Option("store/sim", "--out", help="Output directory for the .ewp pack."),
    name: str = typer.Option(None, "--name", help="Pack name (default: world region)."),
) -> None:
    """Scenario bundle + pinned world -> world pack (`<out>/<name>.ewp/`)."""
    from ember.sim.worldpack import export_bundle

    if bundle:
        bundle_path = Path(bundle)
    else:
        from ember.cli import _resolve_incident_id
        from ember.incidents.model import IncidentStore

        incident_id = _resolve_incident_id(irwin, historic)
        bundle_path = IncidentStore.create(store_root, incident_id).bundle_json
    if not bundle_path.exists():
        raise typer.BadParameter(f"no bundle at {bundle_path} — assemble it first")
    t = time.perf_counter()
    pack = export_bundle(bundle_path, out, name=name)
    typer.secho(f"world pack     : {pack}  ({time.perf_counter() - t:.1f}s)",
                fg=typer.colors.GREEN)


@sim_app.command()
def synth(
    kind: str = typer.Argument(..., help="flat | ramp | ridge | checker | barrier"),
    out: str = typer.Option("store/sim/synth", "--out"),
    nx: int = typer.Option(200, "--nx"),
    ny: int = typer.Option(200, "--ny"),
    cell: float = typer.Option(30.0, "--cell", help="Cell size (m)."),
    fuel: int = typer.Option(102, "--fuel", help="FBFM40 code."),
    name: str = typer.Option(None, "--name"),
    slope_pct: float = typer.Option(30.0, "--slope-pct", help="ramp only"),
    height_m: float = typer.Option(300.0, "--height-m", help="ridge only"),
    fuel_b: int = typer.Option(183, "--fuel-b", help="checker only"),
    block: int = typer.Option(20, "--block", help="checker only"),
    barrier_x: int = typer.Option(None, "--barrier-x", help="barrier only"),
) -> None:
    """Write a procedural test world (plan B2) in world-pack layout."""
    from ember.sim.worldpack import export_synthetic

    kw: dict = {}
    if kind == "ramp":
        kw["slope_pct"] = slope_pct
    elif kind == "ridge":
        kw["height_m"] = height_m
    elif kind == "checker":
        kw.update(fuel_a=fuel, fuel_b=fuel_b, block=block)
    elif kind == "barrier" and barrier_x is not None:
        kw["barrier_x"] = barrier_x
    pack = export_synthetic(kind, out, nx=nx, ny=ny, cell_size_m=cell, fuel=fuel, name=name, **kw)
    typer.secho(f"synthetic world: {pack}", fg=typer.colors.GREEN)


@sim_app.command()
def render(
    run: str = typer.Argument(..., help="A .replay.json or .ess state stream."),
    world: str = typer.Option(None, "--world", help="World pack dir (default: from replay)."),
    out: str = typer.Option(None, "--out", help="Output dir (default: beside the run)."),
    every: int = typer.Option(1, "--every", help="Render every N-th tick."),
    mp4: bool = typer.Option(False, "--mp4/--no-mp4", help="Encode frames to MP4 (ffmpeg)."),
    gif: bool = typer.Option(False, "--gif/--no-gif"),
    fps: int = typer.Option(12, "--fps"),
    crop: str = typer.Option(None, "--crop", help="Cell window x0,y0,x1,y1 (excl.)."),
    scale: int = typer.Option(1, "--scale", help="Integer upscale after cropping."),
) -> None:
    """Render a run's state stream to PNG frames (+ optional MP4/GIF). Disposable 2D viz."""
    from ember.sim.render import encode_gif, encode_mp4, render_run
    from ember.sim.stream import read_replay

    run_path = Path(run)
    if run_path.suffix == ".json":
        rp = read_replay(run_path)
        if not rp.get("stream"):
            raise typer.BadParameter("replay has no baked stream; re-run with stream = true")
        stream = (run_path.parent / rp["stream"]).resolve()
        if world is None:
            world = str((run_path.parent / rp["world"]["pack"]).resolve())
    else:
        stream = run_path
    if world is None:
        raise typer.BadParameter("--world is required for a bare .ess")
    out_dir = Path(out) if out else stream.with_suffix("") / "frames"
    t = time.perf_counter()
    frames = render_run(stream, world, out_dir, every=every, crop=_parse_crop(crop),
                        scale=scale)
    typer.secho(f"frames         : {len(frames)} -> {out_dir}  "
                f"({time.perf_counter() - t:.1f}s)", fg=typer.colors.GREEN)
    if mp4:
        p = encode_mp4(out_dir, out_dir.parent / f"{stream.stem}.mp4", fps=fps)
        typer.secho(f"mp4            : {p}", fg=typer.colors.GREEN)
    if gif:
        p = encode_gif(out_dir, out_dir.parent / f"{stream.stem}.gif", fps=max(4, fps // 2))
        typer.secho(f"gif            : {p}", fg=typer.colors.GREEN)


@sim_app.command()
def static(
    pack: str = typer.Argument(..., help="World pack dir."),
    out: str = typer.Option(..., "--out", help="Output dir for per-layer PNGs + stats.md."),
) -> None:
    """Render every world-pack layer with a legend + stats.md (CP1 'the world loads')."""
    from ember.sim.render import render_all_static

    t = time.perf_counter()
    outs = render_all_static(pack, out)
    for p in outs:
        typer.echo(f"  {p}")
    typer.secho(f"static renders : {len(outs)} layer(s) + stats.md -> {out}  "
                f"({time.perf_counter() - t:.1f}s)", fg=typer.colors.GREEN)


_DRIVE_SAFE_COLON = re.compile(r":(?![\\/])")  # a ':' followed by \ or / is a drive letter


def _parse_run(spec: str) -> tuple[str, Path, Path | None]:
    """`LABEL=stream-or-replay[:worldpack]` -> (label, stream_path, worldpack or None)."""
    if "=" not in spec:
        raise typer.BadParameter(f"--run needs LABEL=path[:worldpack], got {spec!r}")
    label, rest = spec.split("=", 1)
    parts = rest.split("::") if "::" in rest else _DRIVE_SAFE_COLON.split(rest)
    if len(parts) > 2:
        raise typer.BadParameter(f"--run {spec!r}: too many ':' (use '::' to separate)")
    run_path = Path(parts[0])
    world = Path(parts[1]) if len(parts) == 2 else None
    if run_path.suffix == ".json":
        from ember.sim.stream import read_replay

        rp = read_replay(run_path)
        if not rp.get("stream"):
            raise typer.BadParameter(f"{run_path}: replay has no baked stream")
        if world is None:
            world = (run_path.parent / rp["world"]["pack"]).resolve()
        run_path = (run_path.parent / rp["stream"]).resolve()
    if world is None:
        raise typer.BadParameter(f"--run {spec!r}: a bare .ess needs :worldpack")
    return label.strip(), run_path, world


@sim_app.command()
def compare(
    run: list[str] = typer.Option(..., "--run", help="LABEL=stream-or-replay:worldpack "
                                                     "(repeat; use '::' if paths have ':')."),
    out: str = typer.Option(..., "--out", help="Output dir for frames."),
    every: int = typer.Option(1, "--every"),
    cols: int = typer.Option(None, "--cols"),
    mp4: bool = typer.Option(False, "--mp4/--no-mp4"),
    fps: int = typer.Option(12, "--fps"),
    crop: str = typer.Option(None, "--crop", help="Cell window x0,y0,x1,y1 (excl.)."),
    scale: int = typer.Option(1, "--scale", help="Integer upscale after cropping."),
) -> None:
    """Side-by-side frames of N runs on a shared clock (+ optional MP4)."""
    from ember.sim.compare import render_side_by_side
    from ember.sim.render import encode_mp4

    streams = [_parse_run(r) for r in run]
    out_dir = Path(out)
    t = time.perf_counter()
    frames = render_side_by_side(streams, out_dir, every=every, cols=cols,
                                 crop=_parse_crop(crop), scale=scale)
    typer.secho(f"frames         : {len(frames)} -> {out_dir}  "
                f"({time.perf_counter() - t:.1f}s)", fg=typer.colors.GREEN)
    if mp4:
        p = encode_mp4(out_dir, out_dir.parent / f"{out_dir.name}.mp4", fps=fps)
        typer.secho(f"mp4            : {p}", fg=typer.colors.GREEN)


@sim_app.command()
def curves(
    run: list[str] = typer.Option(..., "--run", help="LABEL=stream-or-replay:worldpack (repeat)."),
    out: str = typer.Option(..., "--out", help="Output PNG."),
    table: str = typer.Option(None, "--table", help="Also write a markdown response table."),
    metric: list[str] = typer.Option(None, "--metric", help="Metrics to plot (repeat)."),
    every: int = typer.Option(1, "--every"),
    title: str = typer.Option(None, "--title"),
) -> None:
    """Response-curve plots (+ summary table) for one or more runs."""
    from ember.sim.curves import DEFAULT_METRICS, extract_series, plot_curves, response_table

    series = {label: extract_series(path, every=every) for label, path, _ in map(_parse_run, run)}
    p = plot_curves(series, out, metrics=metric or DEFAULT_METRICS, title=title)
    typer.secho(f"curves         : {p}", fg=typer.colors.GREEN)
    if table:
        Path(table).parent.mkdir(parents=True, exist_ok=True)
        Path(table).write_text(response_table(series), encoding="utf-8")
        typer.secho(f"table          : {table}", fg=typer.colors.GREEN)


def _parse_crop(crop: str | None) -> tuple[int, int, int, int] | None:
    if not crop:
        return None
    parts = [int(v) for v in crop.replace(";", ",").split(",")]
    if len(parts) != 4:
        raise typer.BadParameter(f"--crop needs x0,y0,x1,y1, got {crop!r}")
    return (parts[0], parts[1], parts[2], parts[3])


@sim_app.command()
def probe(
    run: str = typer.Argument(..., help="A .ess stream or .replay.json."),
    every: int = typer.Option(1, "--every"),
) -> None:
    """Per-tick numbers (burned/burning, fire bbox, spots, overlays) as a plain-text table."""
    from ember.sim.probe import format_probe, probe_stream

    stream = Path(run)
    if stream.suffix == ".json":
        from ember.sim.stream import replay_stream_path

        resolved = replay_stream_path(stream)
        if resolved is None:
            raise typer.BadParameter(f"{stream}: replay has no baked stream")
        stream = resolved
    typer.echo(format_probe(probe_stream(stream, every=every)))
