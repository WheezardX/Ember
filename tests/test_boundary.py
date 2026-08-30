"""The ember -> terrain dependency (adr/0005, adr/0006).

Ember is built ON the terrain world-data engine and depends on it as an installed
package. This asserts the engine is actually available and the bake seam wires up —
the mirror of Terrain's own test that terrain never imports ember.
"""


def test_ember_runs_on_terrain():
    import terrain

    import ember

    assert ember.__version__ and terrain.__version__


def test_bake_seam_imports_terrain():
    """`ember/incidents/bake.py` is the one seam that calls into the engine."""
    from ember.incidents.bake import bake_world_for_aoi

    assert callable(bake_world_for_aoi)
    # the engine pieces the seam reaches for must exist
    from terrain.config.models import Settings  # noqa: F401
    from terrain.runner import RunResult, run_pipeline  # noqa: F401
