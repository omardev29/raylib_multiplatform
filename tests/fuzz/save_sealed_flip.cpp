// ---------------------------------------------------------------------------
// A save that somebody changed, fuzzed: real saves of this framework -- sealed
// with this game's key, and plain -- and the input says what to do to them
// (src/rmp/save.cpp, detail::decode).
//
// The input: one byte that picks the save and whether it is sealed, then
// edits of four bytes each -- what (flip a byte, insert one, take one away),
// where (two bytes) and with what -- and, when its length leaves one byte
// over, whether to write the CRC again over the changed file, as somebody who
// knows the format would.
//
// The oracle:
//
//   - a sealed save changed in any byte, by any number of edits, CRC
//     rewritten or not, never reads OK: the seal covers the header and the
//     payload (XChaCha20-Poly1305);
//   - a plain save changed by ONE edit never reads OK: CRC-32 catches every
//     change of one byte, and a byte more or less changes the length;
//   - changed only past its header line, the answer is exact: MODIFIED -- the
//     CRC, or the seal when the CRC was rewritten -- or TRUNCATED for a byte
//     fewer;
//   - a save left as it was reads OK, the Value it holds;
//   - a refusal leaves `out` as it was, and nothing is reported.
//
// The saves are made when the process starts, with fresh nonces: what a run
// checks does not depend on which, and the edits land on the same places.
//
// Inputs: tests/fuzz/corpus/save_sealed_flip/ and
// tests/fuzz/regressions/save_sealed_flip/.
//
//   bash tools/fuzz.sh replay save_sealed_flip
//   bash tools/fuzz.sh run save_sealed_flip 60
// ---------------------------------------------------------------------------

#include "harness.h"

#include "../property/value_model.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using rmp::Value;
using rmp::save::Status;
using rmp::save::detail::Bytes;
using rmp::test::fuzz::require;
namespace model = rmp::test::model;

constexpr int SENTINEL_VERSION = 77;
constexpr std::size_t EDIT = 4;

struct Saves {
    std::vector<Value> values;
    std::vector<Bytes> plain;
    std::vector<Bytes> sealed;
};
Saves saves;

Value sentinel() {
    Value v;
    v["left"] = "as it was";
    rmp::detail::ValueAccess::set_version(v, SENTINEL_VERSION);
    return v;
}

// What a game saves: every type, numbers at their edges, text that is not
// ASCII, the deepest nesting a save holds, and nothing at all.
std::vector<Value> examples() {
    std::vector<Value> out;
    Value game;
    game["level"] = 7;
    game["name"] = "Omar \xc3\xb1 \xf0\x9f\x8e\xae";
    game["alive"] = true;
    game["ratio"] = 0.1;
    game["unlocked"].push("forest");
    game["unlocked"].push(Value());
    game["player"]["x"] = -3.25;
    game["empty"] = Value::object();
    out.push_back(game);
    Value numbers = Value::list();
    for (const double n : { 0.0, -0.0, 1e300, -1e-300, 9007199254740993.0, 5e-324,
                            std::numeric_limits<double>::max() }) {
        numbers.push(n);
    }
    out.push_back(numbers);
    Value deep = 1;
    for (int i = 0; i < rmp::save::detail::MAX_DEPTH; ++i) {
        Value up = Value::list();
        up.push(std::move(deep));
        deep = std::move(up);
    }
    out.push_back(deep);
    out.emplace_back(Value::object());
    out.emplace_back("just a string");
    return out;
}

// The CRC written again over what the file now holds, when the header still
// ends in one: what someone who edited a save and knows the format does.
Bytes with_fixed_crc(Bytes file) {
    std::size_t newline = 0;
    while (newline < file.size() && file[newline] != '\n') ++newline;
    if (newline == file.size() || newline < 9 || file[newline - 9] != ' ') return file;
    const std::size_t at = newline - 8;
    std::string covered(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(at - 1));
    covered += '\n';
    covered.append(file.begin() + static_cast<std::ptrdiff_t>(newline) + 1, file.end());
    std::array<char, 9> hex{};
    std::snprintf(
        hex.data(), hex.size(), "%08x",
        static_cast<unsigned>(rmp::save::detail::crc32(
            reinterpret_cast<const unsigned char *>(covered.data()), covered.size())));
    std::memcpy(file.data() + at, hex.data(), 8);
    return file;
}

// Where the header line ends: the index of its '\n'.
std::size_t header_end(const Bytes &file) {
    std::size_t newline = 0;
    while (newline < file.size() && file[newline] != '\n') ++newline;
    return newline;
}

enum class Edit { FLIP, INSERT, ERASE, NONE };

// One edit of four bytes: 0 flips a byte, 1 inserts one, 2 takes one away.
// Says what it did, and sets `at` to where.
Edit apply(Bytes &file, std::span<const std::uint8_t> edit, std::size_t &at) {
    const unsigned kind = edit[0] % 3U;
    const std::size_t where = (static_cast<std::size_t>(edit[1]) << 8U) | edit[2];
    const std::uint8_t with = edit[3];
    if (kind == 0 && !file.empty() && with != 0) {
        at = where % file.size();
        file[at] ^= with;
        return Edit::FLIP;
    }
    if (kind == 1) {
        at = where % (file.size() + 1);
        file.insert(file.begin() + static_cast<std::ptrdiff_t>(at), with);
        return Edit::INSERT;
    }
    if (kind == 2 && !file.empty()) {
        at = where % file.size();
        file.erase(file.begin() + static_cast<std::ptrdiff_t>(at));
        return Edit::ERASE;
    }
    return Edit::NONE;
}

// What a file changed only past its header line has to read as, or nullopt
// when an edit touched the header (refused, for one reason or another): the
// CRC -- or, rewritten, the seal -- catches a changed byte and one more is
// past the length, MODIFIED; one fewer is TRUNCATED -- unless a plain file is
// refused first, where saves are sealed.
std::optional<Status>
payload_answer(const std::vector<std::pair<Edit, std::size_t>> &done, std::size_t header,
               bool sealed, bool sealed_only) {
    bool erased = false;
    for (const auto &[edit, at] : done) {
        if (edit == Edit::NONE) continue;
        if (at <= header) return std::nullopt;
        erased = erased || edit == Edit::ERASE;
        // Past the first edit, an insertion or an erasure moves every later
        // position: only flips are followed further.
        if (edit != Edit::FLIP && done.size() > 1) return std::nullopt;
    }
    if (sealed_only && !sealed) return Status::MODIFIED;
    return erased ? Status::TRUNCATED : Status::MODIFIED;
}

} // namespace

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer's name
extern "C" int LLVMFuzzerInitialize(int * /*argc*/, char *** /*argv*/) {
    rmp::test::fuzz::initialize();
    saves.values = examples();
    for (const Value &v : saves.values) {
        Bytes plain;
        Bytes sealed;
        require(rmp::save::detail::encode(v, 1, false, &plain) &&
                    rmp::save::detail::encode(v, 1, true, &sealed),
                "every example is saved");
        saves.plain.push_back(std::move(plain));
        saves.sealed.push_back(std::move(sealed));
    }
    return 0;
}

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer's name
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::span<const std::uint8_t> input = rmp::test::fuzz::input({ data, size });
    if (input.empty()) return 0;
    const std::size_t pick = input[0] / 2U % saves.values.size();
    const bool sealed = (input[0] & 1U) != 0;
    const Bytes &original = sealed ? saves.sealed[pick] : saves.plain[pick];
    const std::span<const std::uint8_t> edits = input.subspan(1);

    Bytes file = original;
    const std::size_t count = edits.size() / EDIT;
    // A plain save is held to ONE edit: CRC-32 promises nothing past that.
    std::vector<std::pair<Edit, std::size_t>> done;
    for (std::size_t i = 0; i < (sealed ? count : std::min<std::size_t>(count, 1)); ++i) {
        std::size_t at = 0;
        const Edit edit = apply(file, edits.subspan(i * EDIT, EDIT), at);
        done.emplace_back(edit, at);
    }
    const bool fix_crc = sealed && edits.size() % EDIT != 0 && (edits.back() & 1U) != 0;
    if (fix_crc) file = with_fixed_crc(file);

    for (const bool sealed_only : { false, true }) {
        Value out = sentinel();
        const Status status = rmp::save::detail::decode(file, &out, sealed_only);
        if (file == original) {
            if (sealed_only && !sealed) {
                require(status == Status::MODIFIED,
                        "an unchanged plain save is MODIFIED "
                        "where saves are sealed");
                continue;
            }
            require(status == Status::OK && out == saves.values[pick],
                    "an unchanged save reads OK, the Value saved");
            continue;
        }
        require(status != Status::OK,
                sealed ? "a sealed save changed anywhere never reads OK"
                       : "a plain save changed by one edit never reads OK");
        const std::optional<Status> answer =
            payload_answer(done, header_end(original), sealed, sealed_only);
        require(
            !answer || status == *answer,
            "a save changed past its header is MODIFIED, or TRUNCATED for a byte fewer");
        require(model::exact(out) == model::exact(sentinel()) &&
                    out.version() == SENTINEL_VERSION,
                "a refused save leaves `out` as it was");
    }
    require(rmp::detail::report_count() == 0, "decoding says nothing");
    return 0;
}
