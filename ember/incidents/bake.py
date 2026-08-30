"""The terrain<-ember seam (Epic 3 A2, starting point).

An incident defines its own AOI (fire extent + buffer); ember bakes the world-data
layers for that AOI by calling the `terrain` engine. This is the ONLY direction the
dependency flows — ember -> terrain.
"""

from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from terrain.runner import RunResult


def bake_world_for_aoi(
    name: str,
    bbox: list[float],
    *,
    store_root: str = "store",
    profile: str = "game",
    resolution_m: float | None = None,
    sources: list[str] | None = None,
    fuels: bool = True,
    aoi_crs: str = "EPSG:4326",
) -> RunResult:
    """Bake terrain (+optionally fuels) for an incident AOI via the terrain engine.

    bbox is [min_lon, min_lat, max_lon, max_lat] in `aoi_crs`. Returns terrain's
    RunResult (dem path, config hash, etc.) for ember to reference in the bundle.

    Fire AOIs are large (tens of km), so callers typically pass ``profile="custom"``
    with a coarse ``resolution_m`` (e.g. 30 m) and a coarse ``sources`` list (e.g.
    ``["copernicus-30m"]``) rather than the print/game defaults — a 10 m 3DEP bake of
    a fire footprint is impractically large. ``resolution_m`` is only valid with
    ``profile="custom"`` (mirrors terrain's DemCfg contract).
    """
    from terrain.config.models import Settings
    from terrain.runner import run_pipeline

    dem_cfg: dict = {"profile": profile}
    if profile == "custom":
        if resolution_m is None:
            raise ValueError("resolution_m is required when profile='custom'")
        dem_cfg["resolution_m"] = resolution_m
    elif resolution_m is not None:
        raise ValueError("resolution_m may only be set with profile='custom'")
    if sources is not None:
        dem_cfg["sources"] = sources

    settings = Settings.model_validate({
        "project": {"name": name},
        "aoi": {"type": "bbox", "bbox": bbox, "crs": aoi_crs},
        "dem": dem_cfg,
        "fuels": {"enabled": fuels},
    })
    return run_pipeline(settings, store_root=store_root)
