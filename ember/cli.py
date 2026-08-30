"""ember CLI — the wildfire product entrypoint (`ember ...`).

Separate from the `terrain` CLI to keep the product boundary visible. Epic 3 fills
in `ember incident`; for now it exposes `version` and a stub.
"""

from __future__ import annotations

import typer

from ember import __version__

app = typer.Typer(add_completion=False, help="Ember — wildfire product on the terrain engine.")


@app.command()
def version() -> None:
    """Print ember + the terrain engine version it runs on."""
    import terrain

    typer.echo(f"ember    {__version__}")
    typer.echo(f"terrain  {terrain.__version__}  (world-data engine)")


@app.command()
def incident(
    irwin: str = typer.Option(None, "--irwin", help="IRWIN incident id (live)."),
    historic: str = typer.Option(
        None, "--historic", help="Historic fire id, e.g. jolly-mountain-2017."
    ),
    bake: bool = typer.Option(
        True, "--bake/--no-bake",
        help="Bake a coarse DEM + LANDFIRE fuels for the fire AOI (network). "
             "--no-bake produces the arrival raster only.",
    ),
    enrich: bool = typer.Option(
        True, "--enrich/--no-enrich",
        help="Attach FIRMS hotspots (live only; needs FIRMS_MAP_KEY) + NIROPS IR "
             "products (best-effort). --no-enrich skips both.",
    ),
    weather: bool = typer.Option(
        False, "--weather/--no-weather",
        help="Build a weather timeline (HRRR grid + RAWS stations; RAWS needs "
             "SYNOPTIC_TOKEN). Off by default — HRRR is per-hour fetches.",
    ),
    weather_hours: int = typer.Option(
        24, "--weather-hours", help="Length of the weather window in hours (with --weather)."
    ),
    dry_run: bool = typer.Option(
        False, "--dry-run",
        help="Report what a (re)fetch would add/rebuild without writing or baking. "
             "Re-running normally is an idempotent refresh: observations are append-only "
             "and derived products rebuild only when a new perimeter arrives.",
    ),
) -> None:
    """Assemble a fire incident into a scenario bundle."""
    if not (irwin or historic):
        raise typer.BadParameter("provide --irwin or --historic")
    if irwin and historic:
        raise typer.BadParameter("provide only one of --irwin or --historic")

    if irwin:
        from ember.incidents.assemble import assemble_live

        bundle = assemble_live(irwin, bake_world=bake, enrich=enrich,
                               weather=weather, weather_hours=weather_hours, dry_run=dry_run)
    else:
        from ember.incidents.assemble import assemble_historic

        bundle = assemble_historic(historic, bake_world=bake, enrich=enrich,
                                   weather=weather, weather_hours=weather_hours, dry_run=dry_run)

    if dry_run:
        added = bundle.provenance.get("refresh", {}).get("perimeters_added")
        typer.secho(f"dry-run        : {bundle.incident_id} "
                    f"(plan logged{'' if added is None else f'; +{added} new perimeter(s)'})",
                    fg=typer.colors.YELLOW)
        return
    p = bundle.provenance["arrival"]
    typer.secho(f"incident       : {bundle.incident_id}", fg=typer.colors.GREEN)
    typer.echo(f"perimeters     : {len(bundle.observations)} (immutable observations)")
    typer.echo(f"burned         : {p['burned_km2']} km2 over {p['duration_h'] / 24:.0f} days")
    typer.secho(f"arrival raster : {bundle.derived['arrival_time']}", fg=typer.colors.GREEN)
    obs = bundle.observations
    hotspots = sum(o.attributes.get("count", 0) for o in obs if o.kind == "hotspots")
    ir = sum(o.attributes.get("product_count", 0) for o in obs if o.kind == "ir")
    if hotspots:
        typer.echo(f"firms hotspots : {hotspots}")
    if ir:
        typer.echo(f"nirops IR      : {ir} product(s)")
    if bundle.weather:
        typer.secho(f"weather        : {bundle.weather}", fg=typer.colors.GREEN)
    if bundle.world_region:
        w = bundle.provenance["world"]
        typer.secho(f"world baked     : {bundle.world_region} "
                    f"(DEM+fuels @ {w['resolution_m']:.0f} m, {w['sources']})",
                    fg=typer.colors.GREEN)
    typer.echo("bundle         : store/incidents/.../package/scenario.bundle.json")


if __name__ == "__main__":
    app()
