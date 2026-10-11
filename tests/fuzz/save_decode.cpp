// ---------------------------------------------------------------------------
// A save file, fuzzed: any bytes as the whole file rmp::save::read() finds on
// disk, header and all (src/rmp/save.cpp, detail::decode).
//
// The oracle, whatever the bytes:
//
//   - decode returns one of the five statuses, and reports nothing (a read
//     never reports);
//   - anything but OK leaves `out` exactly as it was, its version included;
//   - OK means a Value that holds only finite numbers and can be written back,
//     at a version of 1 or more; written back at that version, plain and
//     sealed, it reads the same;
//   - a file that reads OK where the game seals its saves reads OK without
//     that requirement too, the same Value; one that does not is MODIFIED
//     there -- a plain file -- or refused for the same reason it is anywhere;
//   - a file that reads OK, cut short, is TRUNCATED.
//
// Inputs: tests/fuzz/corpus/save_decode/ (plain and sealed saves of this
// framework, cut and edited ones) and tests/fuzz/regressions/save_decode/.
//
//   bash tools/fuzz.sh replay save_decode
//   bash tools/fuzz.sh run save_decode 60
// ---------------------------------------------------------------------------

#include "harness.h"

#include "../property/value_model.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace {

using rmp::Value;
using rmp::save::Status;
using rmp::save::detail::Bytes;
using rmp::test::fuzz::require;
namespace model = rmp::test::model;

constexpr int SENTINEL_VERSION = 77;

Value sentinel() {
    Value v;
    v["left"] = "as it was";
    rmp::detail::ValueAccess::set_version(v, SENTINEL_VERSION);
    return v;
}

bool is_status(Status s) {
    return s == Status::OK || s == Status::MISSING || s == Status::TRUNCATED ||
        s == Status::MODIFIED || s == Status::UNREADABLE;
}

// What a decode left in `out`, held to the promises above.
void check(Status status, const Value &out) {
    require(is_status(status), "decode answers one of the five statuses");
    require(status != Status::MISSING, "decode never says MISSING: there were bytes");
    if (status != Status::OK) {
        require(model::exact(out) == model::exact(sentinel()) &&
                    out.version() == SENTINEL_VERSION,
                "a refused file leaves `out` as it was");
        return;
    }
    require(model::all_finite(out), "a save that reads holds only finite numbers");
    require(model::savable(out), "a save that reads can be written back");
    require(out.version() >= 1, "a save that reads is of version 1 or more");
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
    const Bytes file(input.begin(), input.end());

    Value any = sentinel();
    const Status as_any = rmp::save::detail::decode(file, &any, false);
    check(as_any, any);
    Value sealed = sentinel();
    const Status as_sealed = rmp::save::detail::decode(file, &sealed, true);
    check(as_sealed, sealed);
    require(rmp::detail::report_count() == 0, "decoding says nothing");

    if (as_sealed == Status::OK) {
        require(as_any == Status::OK && model::exact(any) == model::exact(sealed) &&
                    any.version() == sealed.version(),
                "a sealed save reads the same without sealed_only");
    } else if (as_any == Status::OK) {
        require(as_sealed == Status::MODIFIED,
                "a file that reads OK and not where saves are sealed is MODIFIED there");
    } else {
        require(as_sealed == as_any || as_sealed == Status::MODIFIED,
                "sealed_only refuses for the same reason, or because the file is plain");
    }
    if (as_any != Status::OK) return 0;

    for (const bool seal : { false, true }) {
        Bytes again;
        require(rmp::save::detail::encode(any, any.version(), seal, &again),
                "a save that reads is written again");
        Value back;
        require(rmp::save::detail::decode(again, &back, seal) == Status::OK &&
                    model::exact(back) == model::exact(any) &&
                    back.version() == any.version(),
                "written again, it reads the same at the same version");
    }
    for (const std::size_t length :
         { file.size() - 1, file.size() / 2, std::size_t{ 1 } }) {
        if (length == 0 || length >= file.size()) continue;
        const Bytes cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(length));
        Value out = sentinel();
        require(rmp::save::detail::decode(cut, &out, false) == Status::TRUNCATED,
                "a save that reads, cut short, is TRUNCATED");
    }
    return 0;
}
