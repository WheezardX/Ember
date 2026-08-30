"""D3 — fire-state tiling.

Runs the incident's arrival-time + confidence rasters through the SAME terrain quadtree
tiler used for DEM/fuels, so fire-state tiles align with the world tiles by construction.
The arrival raster is itself the time index (per-cell first-burn hours since t0), so a
"fire state at time t" query needs only tile-local data: the burned set at t is the
cells where ``arrival_time <= t``. Tiles land under ``<incident>/tiles/z{lod}/x/y/``.
"""

from __future__ import annotations

import json

from terrain.util.logging import get_logger

from ember.incidents.arrival import ALGORITHM
from ember.incidents.model import BundleManifest, IncidentStore, id_to_dirname

log = get_logger(__name__)


def tile_fire_state(store: IncidentStore, incident_id: str, *, tiling=None) -> dict:
    """Tile arrival_time (+confidence) into the incident quadtree; write a manifest."""
    from terrain.config.models import TilingCfg
    from terrain.store.layout import StoreLayout
    from terrain.tiling.tiler import tile_layer

    tiling = tiling or TilingCfg()
    # a StoreLayout whose region_dir IS the incident dir, so tiles/records land there
    layout = StoreLayout(root=store.root / "incidents", region=id_to_dirname(incident_id))
    arrival = store.derived(f"arrival_time.{ALGORITHM}.cog.tif")
    if not arrival.exists():
        raise FileNotFoundError(f"no arrival raster to tile at {arrival}")
    confidence = store.derived(f"confidence.{ALGORITHM}.cog.tif")

    arr_recs = tile_layer(arrival, layout, tiling, "arrival_time", categorical=False)
    conf_recs = (tile_layer(confidence, layout, tiling, "confidence", categorical=True)
                 if confidence.exists() else [])

    arr_prov = {}
    if store.bundle_json.exists():
        bundle = BundleManifest.model_validate_json(store.bundle_json.read_text(encoding="utf-8"))
        arr_prov = bundle.provenance.get("arrival", {})

    manifest = {
        "scheme": tiling.scheme, "tile_px": tiling.tile_px, "overlap_px": tiling.overlap_px,
        "base_lod": tiling.base_lod, "num_lods": tiling.num_lods, "algorithm": ALGORITHM,
        "time_index": {
            "t0": arr_prov.get("t0"), "duration_h": arr_prov.get("duration_h"),
            "unit": "hours_since_t0",
            "query": "fire state at time t = cells where arrival_time <= t",
        },
        "layers": {
            "arrival_time": {"categorical": False, "unit": "h", "tiles": len(arr_recs),
                             "records": arr_recs},
            "confidence": {"categorical": True, "unit": "class", "tiles": len(conf_recs),
                           "records": conf_recs},
        },
    }
    out = store.dir / "tiles" / "firestate.manifest.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    log.info("fire-state tiles: arrival=%d confidence=%d -> %s",
             len(arr_recs), len(conf_recs), out)
    return manifest
