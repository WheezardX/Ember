# Test fixtures copied from Terrain (the scatter reference implementation)

| File | Upstream path | Upstream commit |
|---|---|---|
| `scatter_pnw.json` | `Terrain/tests/golden/scatter_pnw.json` | scatter v2, ADR 0007 (2026-09-28) |
| `scatter_pnw_tree.json` | `Terrain/tests/golden/scatter_pnw_tree.json` | U12 per-tree ground, Terrain 03230ee+ (2026-09-29) |
| `pnw_conifer.toml` | `Terrain/terrain/packs/palettes/pnw_conifer.toml` | scatter v2, ADR 0007 (2026-09-28) |

Copied so the C++ scatter conformance test runs in CI without a Terrain checkout. If either
changes upstream, re-copy both and re-run `emberworld_tests`; a mismatch means the port (or the
spec) moved.
