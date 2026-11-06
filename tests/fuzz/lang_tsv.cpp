// ---------------------------------------------------------------------------
// The translation tables' reader, fuzzed: parse_tsv() and load_tables() of
// src/rmp/text.cpp, and the lookups behind them.
//
// The input is a table's bytes. The oracle: it reads whatever it is handed,
// never aborts, reads out of bounds or leaks; every row is cells, a cell
// written back by escape_cell() reads back the same; and a lookup in any
// language of the table, of any key it has, gives text and no crash --
// plurals, placeholders and all.
//
//   bash tools/fuzz.sh replay lang_tsv      every committed input, once
//   bash tools/fuzz.sh run lang_tsv 60      a minute of libFuzzer
// ---------------------------------------------------------------------------

#include "harness.h"

#include "../../src/rmp/text_internal.h"

#include <rmp/text.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {
using rmp::test::fuzz::require;
} // namespace

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer's name
extern "C" int LLVMFuzzerInitialize(int * /*argc*/, char *** /*argv*/) {
    rmp::test::fuzz::initialize();
    rmp::lang::detail::set_persist(false);
    rmp::lang::detail::set_system_locale_for_tests("en");
    return 0;
}

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer's name
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::span<const std::uint8_t> input = rmp::test::fuzz::input({ data, size });
    const std::string text(input.begin(), input.end());
    const auto rows = rmp::lang::detail::parse_tsv(text);
    for (const auto &row : rows) {
        require(!row.empty(), "every row has a cell");
        for (const std::string &cell : row) {
            const auto again = rmp::lang::detail::parse_tsv(rmp::lang::detail::escape_cell(cell));
            require(cell.empty() || (again.size() == 1 && again[0].size() == 1 && again[0][0] == cell) ||
                        cell.front() == '#',
                    "a cell written back reads back the same");
        }
    }
    rmp::lang::detail::load_tables({ { {}, text }, { "ru", text } });
    for (const std::string &code : rmp::lang::detail::table_languages()) {
        rmp::lang::set(code);
        for (const auto &row : rows) {
            if (row.empty()) continue;
            const rmp::Text t(row[0], { { "count", 3 }, { "name", "x" } },
                              { .context = row.size() > 1 ? std::string_view(row[1]) : "" });
            (void)t.str();
            (void)rmp::lang::translate(row[0]);
            (void)rmp::lang::name(code);
        }
    }
    rmp::lang::detail::reset_for_tests();
    rmp::lang::detail::set_persist(false);
    rmp::lang::detail::set_system_locale_for_tests("en");
    return 0;
}
