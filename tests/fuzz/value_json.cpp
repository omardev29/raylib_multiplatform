// ---------------------------------------------------------------------------
// rmp::save's JSON, fuzzed: any text as the payload a save holds, read, walked
// and written back (src/rmp/save.cpp, from_json and to_json).
//
// The oracle, whatever the text:
//
//   - from_json returns -- no crash, no UB, no unbounded recursion -- and when
//     it refuses, `out` is left exactly as it was, its version included;
//   - what it accepts holds only finite numbers (1e400 is refused, not read as
//     an infinity: defect 7) and can be written: everything that reads, writes;
//   - every read of every node, through the const Value and through Refs,
//     reads the same, creates nothing and reports nothing (a read never
//     reports);
//   - written back, it reads as the same Value, and writing that again gives
//     the same text: to_json is idempotent;
//   - and the same through the whole file format, plain and sealed.
//
// Inputs: tests/fuzz/corpus/value_json/ and tests/fuzz/regressions/value_json/.
// tests/fuzz/value_json.dict hands libFuzzer JSON's words.
//
//   bash tools/fuzz.sh replay value_json
//   bash tools/fuzz.sh run value_json 60
// ---------------------------------------------------------------------------

#include "harness.h"

#include "../property/value_model.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace {

using rmp::Value;
using rmp::save::Status;
using rmp::test::fuzz::require;
namespace model = rmp::test::model;

constexpr int SENTINEL_VERSION = 77;

// What `out` holds before a read, to see that a refusal leaves it alone.
Value sentinel() {
    Value v;
    v["left"] = "as it was";
    rmp::detail::ValueAccess::set_version(v, SENTINEL_VERSION);
    return v;
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
    const std::string text(input.begin(), input.end());

    Value out = sentinel();
    if (!rmp::save::detail::from_json(text, &out)) {
        require(model::exact(out) == model::exact(sentinel()) &&
                    out.version() == SENTINEL_VERSION,
                "a refused text leaves `out` as it was");
        require(rmp::detail::report_count() == 0, "a refused text says nothing");
        return 0;
    }
    require(model::all_finite(out), "a text that reads holds only finite numbers");
    require(model::savable(out), "a text that reads can be written back");

    const std::string before = model::exact(out);
    const Value &as_const = out;
    const std::string through_const = model::read_everything(as_const);
    require(model::read_everything_through_refs(out) == through_const,
            "a read through a Ref reads what the const read does");
    require(model::exact(out) == before, "reading creates nothing");
    require(rmp::detail::report_count() == 0, "reading says nothing");

    const std::string json = rmp::save::detail::to_json(out);
    require(!json.empty(), "what reads, writes");
    Value again;
    require(rmp::save::detail::from_json(json, &again), "what is written reads");
    require(model::exact(again) == before, "written and read back, the same Value");
    require(again == out, "written and read back, equal");
    require(rmp::save::detail::to_json(again) == json, "written twice, the same text");

    for (const bool sealed : { false, true }) {
        rmp::save::detail::Bytes file;
        require(rmp::save::detail::encode(out, 3, sealed, &file),
                "what reads is saved, plain and sealed");
        Value back;
        require(rmp::save::detail::decode(file, &back, sealed) == Status::OK &&
                    model::exact(back) == before && back.version() == 3,
                "saved, it reads back the same at its version");
    }
    require(rmp::detail::report_count() == 0, "a whole round trip says nothing");
    return 0;
}
