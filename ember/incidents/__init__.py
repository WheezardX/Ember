"""Epic 3 — live & historic fire data pipeline.

Ingests what's burning (or burned) into the same tile-aligned world representation:
WFIGS incidents/perimeters, FIRMS hotspots, NIROPS IR, HRRR/RAWS weather, and the
derived arrival-time raster + scenario bundle. Keyed off the IRWIN id. Observations
are immutable; derived products are versioned interpretations.

Consumes `terrain` for the on-demand terrain/fuels bake of each incident's AOI.
"""
