// ---------------------------------------------------------------------------
// The LDtk reader (src/rmp/ldtk.cpp), fuzzed.
//
// The first byte of an input picks the level; the rest is the bytes of a
// .ldtk project, exactly as rmp::assets::load_data hands them over. Byte 0
// asks for no level by name (the first one), 0xFF for one the project does
// not have, anything else for the project's own levels in turn -- in its
// `levels` and in each of its `worlds` -- so every level of a seed is reached
// and so is the refusal of a name that is not there.
//
// The oracle is the reader's promise -- it returns a map or refuses one,
// never crashes, never allocates past its budgets (libFuzzer's -rss_limit_mb)
// and never does UB on the way -- the level it returns is the one asked for,
// and then everything tests/fuzz/map_walk.h asks of any map a reader accepted.
//
// Inputs: tests/fuzz/corpus/ldtk/ (seeded with tests/fixtures/ldtk/, the
// samples cut to one level each to fit -max_len, and what fuzzing merged in)
// and tests/fuzz/regressions/ldtk/. tests/fuzz/ldtk.dict hands libFuzzer the
// format's keys.
//
//   bash tools/fuzz.sh replay ldtk          every committed input, once
//   bash tools/fuzz.sh run ldtk 60          a minute of libFuzzer
// ---------------------------------------------------------------------------

#include "harness.h"
#include "map_walk.h"

#include "../../src/rmp/json_internal.h"
#include "../../src/rmp/tilemap_internal.h"

#include <rmp/tilemap.h>

#include <cJSON.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {

using rmp::test::fuzz::require;

// The identifiers of every level of a project, in the order the reader
// searches them; none when it is not JSON.
std::vector<std::string> level_names(std::span<const std::uint8_t> project) {
    std::vector<std::string> out;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const rmp::detail::Json root(cJSON_ParseWithLength(
        reinterpret_cast<const char *>(project.data()), project.size()));
    const auto add = [&](const cJSON *levels) {
        const cJSON *level = nullptr;
        cJSON_ArrayForEach(level, levels) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(level, "identifier");
            if (cJSON_IsString(id) && id->valuestring != nullptr)
                out.emplace_back(id->valuestring);
        }
    };
    if (!cJSON_IsObject(root.get())) return out;
    add(cJSON_GetObjectItemCaseSensitive(root.get(), "levels"));
    const cJSON *world = nullptr;
    cJSON_ArrayForEach(world, cJSON_GetObjectItemCaseSensitive(root.get(), "worlds")) {
        add(cJSON_GetObjectItemCaseSensitive(world, "levels"));
    }
    return out;
}

} // namespace

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer's name
extern "C" int LLVMFuzzerInitialize(int * /*argc*/, char *** /*argv*/) {
    rmp::test::fuzz::initialize();
    return 0;
}

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer's name
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::span<const std::uint8_t> input = rmp::test::fuzz::input({ data, size });
    if (input.empty()) return 0;
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return 0;
    const std::uint8_t pick = input[0];
    const std::span<const std::uint8_t> project = input.subspan(1);

    const std::vector<std::string> names = level_names(project);
    std::string level;
    bool missing = false;
    if (pick == 0xFF) {
        level = "no level is called this";
        missing = std::ranges::find(names, level) == names.end();
    } else if (pick != 0 && !names.empty()) {
        level = names[static_cast<std::size_t>(pick - 1) % names.size()];
    }

    rmp::Tilemap map;
    rmp::tilemap::detail::Access::adopt(
        map,
        rmp::tilemap::detail::parse_map(project.data(), static_cast<int>(project.size()),
                                        "fuzz.ldtk", level.c_str()));
    if (map.valid() && !level.empty()) {
        require(map.level() == level, "the level loaded is the one asked for");
    }
    if (missing) require(!map.valid(), "a level the project does not have is no map");
    rmp::test::fuzz::maps::walk(map);
    return 0;
}
