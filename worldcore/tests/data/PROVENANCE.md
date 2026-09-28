# Test fixtures copied from Terrain (the scatter reference implementation)

| File | Upstream path | Upstream commit |
|---|---|---|
| `scatter_pnw.json` | `Terrain/tests/golden/scatter_pnw.json` | 836677e (2026-08-30) |
| `pnw_conifer.toml` | `Terrain/terrain/packs/palettes/pnw_conifer.toml` | 836677e (2026-08-30) |

Copied so the C++ scatter conformance test runs in CI without a Terrain checkout. If either
changes upstream, re-copy both and re-run `emberworld_tests`; a mismatch means the port (or the
spec) moved.
