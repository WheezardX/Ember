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
) -> None:
    """Assemble a fire incident into a scenario bundle."""
    if not (irwin or historic):
        raise typer.BadParameter("provide --irwin or --historic")
    if irwin and historic:
        raise typer.BadParameter("provide only one of --irwin or --historic")

    if irwin:
        from ember.incidents.assemble import assemble_live

        bundle = assemble_live(irwin, bake_world=bake)
    else:
        from ember.incidents.assemble import assemble_historic

        bundle = assemble_historic(historic, bake_world=bake)
    p = bundle.provenance["arrival"]
    typer.secho(f"incident       : {bundle.incident_id}", fg=typer.colors.GREEN)
    typer.echo(f"perimeters     : {len(bundle.observations)} (immutable observations)")
    typer.echo(f"burned         : {p['burned_km2']} km2 over {p['duration_h'] / 24:.0f} days")
    typer.secho(f"arrival raster : {bundle.derived['arrival_time']}", fg=typer.colors.GREEN)
    if bundle.world_region:
        w = bundle.provenance["world"]
        typer.secho(f"world baked     : {bundle.world_region} "
                    f"(DEM+fuels @ {w['resolution_m']:.0f} m, {w['sources']})",
                    fg=typer.colors.GREEN)
    typer.echo("bundle         : store/incidents/.../package/scenario.bundle.json")


if __name__ == "__main__":
    app()
