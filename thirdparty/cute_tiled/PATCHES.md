# This is a MODIFIED copy of cute_tiled

Upstream: <https://github.com/RandyGaul/cute_headers>, `cute_tiled.h`. Dual
licensed zlib OR public domain (Unlicense), the chooser's option; this project
takes the Unlicense alternative (recorded in `THIRD_PARTY_LICENSES.md`), under
which nothing is owed. It is marked as altered anyway, because a reader cannot
tell which alternative was taken from the file alone.

Two sets of changes. Each site carries `/* rmp patch */`; re-apply all of them
when bumping.

## Unknown keys are skipped

| File | Line | Change | Why |
|---|---|---|---|
| `cute_tiled.h` | 1785 | `cute_tiled_rmp_skip_value_internal()`: skips one JSON value of any shape | the parsers below need a way to step over a key they do not know |
| `cute_tiled.h` | 2254, 2531, 2659, 2697, 2886, 3018 | the six `default:` branches of the layer, object, tileset, map and property parsers skip the unknown value instead of failing with `Unknown identifier found` | upstream is verified against the Tiled 1.5 schema and Tiled is on 1.12, so a map saved by any recent editor contains keys it has never heard of and the whole load failed |

`tests/fixtures/map_modern.json` is the proof rather than the claim: the
unpatched parser rejects it and this one accepts it.

## Any bytes at all: no crash, no leak, no undefined behaviour

Upstream trusts its input. Fuzzing it (`tests/fuzz/tiled.cpp`, phase 16), with
the sanitizers on it as on our own code -- `cmake/sanitize_ignore.txt` used to
switch every one of them off for `cute_tiled_*`, which is how none of this was
seen -- found a map cut short crashing the game, reads past the end of the
bytes, leaks, an overflowing `memset` and a stack that nested groups ran out.
Every input that showed one is in `tests/fuzz/regressions/tiled/`, each red
against the unpatched header, and `tests/tilemap_test.cpp` holds them as named
cases ("a map cut short anywhere...", "a map that fails half way...", "group
layers nested past 32 deep...").

| File | Line | Change | Why |
|---|---|---|---|
| `cute_tiled.h` | 3240 | `cute_tiled_free_map()` finds the internal struct with `offsetof` | the classic `&((T *)0)->member` is a member access through a null pointer: UBSan reported it on every map once the ignore list stopped hiding it |
| `cute_tiled.h` | 1378, 3162 | `cute_tiled_map_internal_alloc_internal()` parses an owned copy of the input with a NUL after it; a negative size is no input | `strtod`, `strtoll` and `strtoull` read until the number ends, and a map's bytes as a game loads them have nothing after them: a file ending in a digit was read past its end |
| `cute_tiled.h` | 3199 | `cute_tiled_load_map_from_memory()` with no input returns NULL | it was `__builtin_trap()` |
| `cute_tiled.h` | 1504-1545 | `cute_tiled_peak`, `next`, `string_next` and `try` stop at the end of the input and say 0 there | `peak` and the whitespace loop of `next` had no bound, and `next` trapped at the end: a map cut short by a failed download crashed the game. Every caller already fails on an unexpected 0 |
| `cute_tiled.h` | 1579, 1617, 1657, 2005, 2223, 2724 | skipping an object, an array, a string, up to a character, an object's `text` and the deprecated tile property braces fails at the end of the input | with `next` saying 0 there, each would otherwise have read past it or spun forever |
| `cute_tiled.h` | 1848, 1855 | `true` and `false` are taken only when the input still holds them | it stepped 4 or 5 bytes ahead unchecked |
| `cute_tiled.h` | 2076, 2105 | the two look-ahead scans of a property's value -- is it a colour, is it a float -- stop at the end of the value | the float scan ran to the next comma: the last property of a file was read past its end |
| `cute_tiled.h` | 1761 | a number past float's range reads as the infinity IEEE would give | converting such a double to float is undefined behaviour in C++ (clang's UBSan does not check it); `src/rmp/tilemap.cpp` makes every float it takes finite anyway |
| `cute_tiled.h` | 1409-1435, 1878, 1948, 2025, 2549, 3124 | every array the parser grows -- CSV tiles, vertices, properties, animation frames -- is allocated by `cute_tiled_rmp_owned()` as a page of the map, and freed with the pages; the walk that freed them one by one is gone, and growing one past what an int counts fails (`cute_tiled_rmp_grow()`) | upstream malloc'ed them and freed them only with a map that loaded: every refusal after them leaked, and a key given twice leaked its first array. LeakSanitizer said so |
| `cute_tiled.h` | 2271 | a chunk is cleared with `sizeof(cute_tiled_chunk_t)` | it was `sizeof(cute_tiled_layer_t)`, a `memset` past the chunk |
| `cute_tiled.h` | 2317-2340 | group layers (and a tile's `objectgroup`) nest at most 32 deep (`CUTE_TILED_RMP_MAX_DEPTH`) | the parser recursed once a group, and so did the pass that freed the map: a file of nested groups ran the stack out |
| `cute_tiled.h` | 1349 | `#include <math.h>` | `HUGE_VAL`, above |
