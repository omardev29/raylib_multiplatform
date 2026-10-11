// ---------------------------------------------------------------------------
// The Tiled reader (src/rmp/tilemap.cpp over thirdparty/cute_tiled), fuzzed.
//
// The input is the bytes of a .json map, exactly as rmp::assets::load_data
// hands them over: no NUL after them. The oracle is the reader's promise --
// it returns a map or refuses one, never crashes, never reads past the bytes
// and never does UB on the way (cute_tiled is instrumented like our code:
// cmake/sanitize_ignore.txt leaves it nothing) -- and then everything
// tests/fuzz/map_walk.h asks of any map a reader accepted.
//
// Inputs: tests/fuzz/corpus/tiled/ (seeded with tests/fixtures/map_*.json and
// what fuzzing merged in) and tests/fuzz/regressions/tiled/.
// tests/fuzz/tiled.dict hands libFuzzer the keys cute_tiled knows.
//
//   bash tools/fuzz.sh replay tiled         every committed input, once
//   bash tools/fuzz.sh run tiled 60         a minute of libFuzzer
// ---------------------------------------------------------------------------

#include "harness.h"
#include "map_walk.h"

#include "../../src/rmp/tilemap_internal.h"

#include <rmp/tilemap.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer's name
extern "C" int LLVMFuzzerInitialize(int * /*argc*/, char *** /*argv*/) {
    rmp::test::fuzz::initialize();
    return 0;
}

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer's name
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::span<const std::uint8_t> input = rmp::test::fuzz::input({ data, size });
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return 0;
    rmp::Tilemap map;
    rmp::tilemap::detail::Access::adopt(
        map,
        rmp::tilemap::detail::parse_map(input.data(), static_cast<int>(input.size()),
                                        "fuzz.json"));
    rmp::test::fuzz::maps::walk(map);
    return 0;
}
