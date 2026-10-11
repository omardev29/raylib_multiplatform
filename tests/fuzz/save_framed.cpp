// ---------------------------------------------------------------------------
// A save's payload, fuzzed behind a header that is right: the harness writes
// the line rmp::save's writer would -- format, version, kind, length and the
// CRC of it all -- around whatever the input holds, so every input gets past
// the checks that guard the parser and reaches it (src/rmp/save.cpp,
// detail::decode then from_json). save_decode fuzzes the header; this, what
// is under it.
//
// The input: one byte that picks the version, then the payload.
//
// The oracle:
//
//   - the frame is right, so the answer is OK or UNREADABLE and nothing else:
//     OK exactly when from_json takes the payload, the same Value, at the
//     version the header says -- a Value with only finite numbers, which
//     writes back and reads the same; UNREADABLE otherwise, `out` left as it
//     was;
//   - where the game seals its saves the same file is MODIFIED, whatever it
//     holds: it is plain;
//   - nothing is reported.
//
// Inputs: tests/fuzz/corpus/save_framed/ and tests/fuzz/regressions/save_framed/.
//
//   bash tools/fuzz.sh replay save_framed
//   bash tools/fuzz.sh run save_framed 60
// ---------------------------------------------------------------------------

#include "harness.h"

#include "../property/value_model.h"

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace {

using rmp::Value;
using rmp::save::Status;
using rmp::save::detail::Bytes;
using rmp::test::fuzz::require;
namespace model = rmp::test::model;

constexpr int SENTINEL_VERSION = 77;
constexpr std::array VERSIONS = { 1, 2, 99, 2000000000, INT_MAX };

Value sentinel() {
    Value v;
    v["left"] = "as it was";
    rmp::detail::ValueAccess::set_version(v, SENTINEL_VERSION);
    return v;
}

// The header our writer puts on a plain save of this payload, CRC and all:
// rmp-save 1 <version> plain <length> <crc32 of the line so far, '\n' and payload>.
Bytes framed(std::string_view payload, int version) {
    const std::string prefix = "rmp-save 1 " + std::to_string(version) + " plain " +
        std::to_string(payload.size());
    std::string covered = prefix + "\n";
    covered += payload;
    const std::uint32_t crc = rmp::save::detail::crc32(
        reinterpret_cast<const unsigned char *>(covered.data()), covered.size());
    std::array<char, 16> hex{};
    std::snprintf(hex.data(), hex.size(), " %08x\n", static_cast<unsigned>(crc));
    Bytes file(prefix.begin(), prefix.end());
    file.insert(file.end(), hex.data(), hex.data() + 10);
    file.insert(file.end(), payload.begin(), payload.end());
    return file;
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
    const int version = VERSIONS[input[0] % VERSIONS.size()];
    const std::string payload(input.begin() + 1, input.end());
    const Bytes file = framed(payload, version);

    Value parsed;
    const bool parses = rmp::save::detail::from_json(payload, &parsed);
    Value out = sentinel();
    const Status status = rmp::save::detail::decode(file, &out, false);
    if (parses) {
        require(status == Status::OK, "a right frame around JSON that reads is OK");
        require(model::exact(out) == model::exact(parsed),
                "it reads as from_json reads it");
        require(out.version() == version, "at the version its header says");
        // Held to what a save is, not only to what from_json says -- the two
        // share a parser, and a defect in it would agree with itself.
        require(model::all_finite(out), "a payload that reads holds only finite numbers");
        Bytes again;
        require(rmp::save::detail::encode(out, version, false, &again),
                "a payload that reads can be written back");
        Value back;
        require(rmp::save::detail::decode(again, &back, false) == Status::OK &&
                    model::exact(back) == model::exact(out),
                "written back, it reads the same");
    } else {
        require(status == Status::UNREADABLE,
                "a right frame around what is not JSON, or not a save's, is UNREADABLE");
        require(model::exact(out) == model::exact(sentinel()) &&
                    out.version() == SENTINEL_VERSION,
                "a refused payload leaves `out` as it was");
    }
    Value sealed_only = sentinel();
    require(rmp::save::detail::decode(file, &sealed_only, true) == Status::MODIFIED,
            "a plain file where saves are sealed is MODIFIED, whatever it holds");
    require(rmp::detail::report_count() == 0, "decoding says nothing");
    return 0;
}
