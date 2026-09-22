# This is a MODIFIED copy of cute_tiled

Upstream: <https://github.com/RandyGaul/cute_headers>, `cute_tiled.h`. Dual
licensed zlib OR public domain (Unlicense), the chooser's option; this project
takes the Unlicense alternative (recorded in `THIRD_PARTY_LICENSES.md`), under
which nothing is owed. It is marked as altered anyway, because a reader cannot
tell which alternative was taken from the file alone.

| File | Line | Change | Why |
|---|---|---|---|
| `cute_tiled.h` | 1742 | `cute_tiled_rmp_skip_value_internal()`: skips one JSON value of any shape | the parsers below need a way to step over a key they do not know |
| `cute_tiled.h` | 2202, 2457, 2583, 2621, 2809, 2941 | the six `default:` branches of the layer, object, tileset, map and property parsers skip the unknown value instead of failing with `Unknown identifier found` | upstream is verified against the Tiled 1.5 schema and Tiled is on 1.12, so a map saved by any recent editor contains keys it has never heard of and the whole load failed |

Each site carries `/* rmp patch */`. `tests/fixtures/map_modern.json` is the
proof rather than the claim: the unpatched parser rejects it and this one
accepts it. Re-apply when bumping.
