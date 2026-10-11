// ---------------------------------------------------------------------------
// rmp::save, held to what rmp/save.h and save_internal.h say of it, over every
// tree tests/property/gen.h draws:
//
//   - what is written reads back -- the same Value, the same JSON text, at
//     the version it was written with, plain and sealed -- and encode refuses
//     exactly what could not: nested past 64 containers, a NUL in a string or
//     a key, a number that is not finite (Omar, 2026-10-10: no NaN and no
//     infinity enters a save; JSON has neither);
//   - a JSON number too large for a double is no save at all: UNREADABLE, never
//     an infinity read in and written back as nothing;
//   - one byte changed, added or taken away never reads OK, and neither does a
//     sealed save changed anywhere, its CRC recomputed or not -- in the payload
//     it is MODIFIED, or TRUNCATED for a byte fewer; a save cut short anywhere
//     is TRUNCATED;
//   - a refusal leaves `out` exactly as it was, its version included;
//   - read(), write(), exists() and remove() agree on any slot name, and the
//     bytes on disk read as decode() reads them, saying nothing.
//
// Each was seen red: the commits that added them name the defect or the hand
// mutation. A counterexample becomes a named case in tests/save_test.cpp.
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"
#include "value_model.h"

#include "../../src/rmp/internal.h"

#include <doctest.h>
#include <raylib.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// covers: rmp::save::write(std::string_view, const Value &, const WriteOptions &)
// covers: rmp::save::read(std::string_view, Value &, const ReadOptions &)
// covers: rmp::save::exists(std::string_view)
// covers: rmp::save::remove(std::string_view)
// covers: rmp::save::directory()
// covers: rmp::save::Result::operator bool() const

namespace {

using rmp::Value;
using rmp::save::Status;
using Bytes = rmp::save::detail::Bytes;
namespace model = rmp::test::model;
namespace fs = std::filesystem;

struct QuietLog {
    QuietLog() { SetTraceLogLevel(LOG_ERROR); }
    ~QuietLog() { SetTraceLogLevel(LOG_INFO); }
    QuietLog(const QuietLog &) = delete;
    QuietLog &operator=(const QuietLog &) = delete;
};

// A folder of its own under the system's temporary one, the save folder for
// the length of one property; gone afterwards, and the real folders back.
struct SaveFolder {
    fs::path path;
    SaveFolder() {
        std::random_device device;
        path = fs::temp_directory_path() /
            ("rmp_save_prop_" + std::to_string(device()) + "_" +
             std::to_string(device()));
        fs::create_directories(path);
        rmp::save::detail::set_folders_for_tests("", path.string());
    }
    ~SaveFolder() {
        rmp::save::detail::reset_for_tests();
        std::error_code error;
        fs::remove_all(path, error);
    }
    SaveFolder(const SaveFolder &) = delete;
    SaveFolder &operator=(const SaveFolder &) = delete;
};

Bytes encoded(const Value &v, int version, bool sealed) {
    Bytes out;
    RC_ASSERT(rmp::save::detail::encode(v, version, sealed, &out));
    return out;
}

// detail::decode, `out` by reference: save_internal.h still takes a pointer
// (tools/pointer_ratchet.txt), and the properties do not.
Status decoded(const Bytes &file, Value &out, bool sealed_only) {
    return rmp::save::detail::decode(file, &out, sealed_only);
}

// The header's CRC rewritten to match what the file now holds: what someone
// who knows the format does after editing a save.
Bytes with_fixed_crc(Bytes file) {
    std::size_t newline = 0;
    while (newline < file.size() && file[newline] != '\n') ++newline;
    const std::string header(file.begin(),
                             file.begin() + static_cast<std::ptrdiff_t>(newline));
    const std::size_t space = header.rfind(' ');
    if (newline == file.size() || space == std::string::npos ||
        header.size() - space != 9)
        return file;
    std::string covered = header.substr(0, space) + "\n";
    covered.append(file.begin() + static_cast<std::ptrdiff_t>(newline) + 1, file.end());
    std::array<char, 9> crc{};
    std::snprintf(
        crc.data(), crc.size(), "%08x",
        static_cast<unsigned>(rmp::save::detail::crc32(
            reinterpret_cast<const unsigned char *>(covered.data()), covered.size())));
    std::memcpy(file.data() + space + 1, crc.data(), 8);
    return file;
}

// Where the header line ends: the index of its '\n'.
std::size_t header_end(const Bytes &file) {
    std::size_t newline = 0;
    while (newline < file.size() && file[newline] != '\n') ++newline;
    return newline;
}

// `v` rebuilt with, now and then, one more member in an object: one holding
// nothing, under a key with a NUL in it. A member holding nothing is not
// there for any function of Value, so it is not written and must not stop
// the save -- and gen.h draws one in about one tree in three hundred, past a
// run's 200 cases: unsavable() reading the key of a member it then skips
// survived them (review of P1, hand mutation).
Value with_hidden_members(const Value &v) {
    if (v.type() == Value::Type::LIST) {
        Value out = Value::list();
        for (int i = 0; i < v.size(); ++i) out.push(with_hidden_members(v[i]));
        return out;
    }
    if (v.type() != Value::Type::OBJECT) return v;
    Value out = Value::object();
    for (int i = 0; i < v.size(); ++i) out[v.key(i)] = with_hidden_members(v[v.key(i)]);
    const std::string key =
        std::string("hidden") + '\0' + *rc::gen::string<std::string>();
    if (*rc::gen::arbitrary<bool>() && !out.contains(key)) out[key] = Value();
    return out;
}

// A savable Value: drawn until it is one -- most are.
Value savable_value() {
    return *rc::gen::suchThat(rc::gen::arbitrary<Value>(),
                              [](const Value &v) { return model::savable(v); });
}

int draw_version() {
    return *rc::gen::weightedOneOf<int>(
        { { 3, rc::gen::inRange(1, 1000) },
          { 1, rc::gen::element(1, INT_MAX - 1, INT_MAX) } });
}

// The slot names the header promises to take, from rmp/save.h.
bool valid_slot(std::string_view slot) {
    if (slot.empty() || slot.size() > 64 || slot.front() == '.') return false;
    return std::ranges::all_of(slot, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}

// A slot name: often one write() takes, at its edges too -- 64 characters,
// dots inside -- and often one it refuses: a path, a dot first, 65
// characters, empty, any string at all.
std::string draw_slot() {
    // The generator outlives this call -- RapidCheck replays it to shrink --
    // so it holds nothing of the call's: ALLOWED is a constant.
    static constexpr std::string_view ALLOWED =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.";
    return *rc::gen::weightedOneOf<std::string>({
        { 3, rc::gen::exec([] {
              const int length = *rc::gen::inRange(1, 66);
              std::string slot;
              for (int i = 0; i < length; ++i)
                  slot += ALLOWED[*rc::gen::inRange<std::size_t>(0, ALLOWED.size())];
              return slot;
          }) },
        { 1,
          rc::gen::element<std::string>("", ".", ".hidden", "a/b", "..", "../up", "a\\b",
                                        "C:", "slot1", std::string(64, 'x'),
                                        std::string(65, 'x')) },
        { 1, rmp::test::gen::hostile_string() },
    });
}

// A JSON number no double can hold: 1 to 999, times ten to the 309th or more.
std::string huge_number() {
    const int mantissa = *rc::gen::inRange(1, 1000);
    const int exponent = *rc::gen::inRange(309, 100000);
    const bool negative = *rc::gen::arbitrary<bool>();
    const bool fraction = *rc::gen::arbitrary<bool>();
    return (negative ? "-" : "") + std::to_string(mantissa) + (fraction ? ".5" : "") +
        (*rc::gen::arbitrary<bool>() ? "e" : "E+") + std::to_string(exponent);
}

// A plain save around any payload: what a person who knows the format writes.
Bytes framed_plain(std::string_view json, int version) {
    const std::string prefix =
        "rmp-save 1 " + std::to_string(version) + " plain " + std::to_string(json.size());
    Bytes file(prefix.begin(), prefix.end());
    const std::string tail = " 00000000\n";
    file.insert(file.end(), tail.begin(), tail.end());
    file.insert(file.end(), json.begin(), json.end());
    return with_fixed_crc(file);
}

} // namespace

TEST_SUITE("property: save") {
    TEST_CASE("what is written reads back, and only that is written") {
        rmp::test::check(
            "encode refuses exactly what would not read back: past 64 containers, a NUL, "
            "a number that is not finite",
            [](const Value &drawn, bool sealed) {
                const Value v =
                    *rc::gen::arbitrary<bool>() ? with_hidden_members(drawn) : drawn;
                Bytes out = { 1, 2, 3 };
                const bool wrote = rmp::save::detail::encode(v, 1, sealed, &out);
                RC_ASSERT(wrote == model::savable(v));
                RC_ASSERT(rmp::save::detail::to_json(v).empty() == !model::savable(v));
                if (!wrote) RC_ASSERT(out == Bytes({ 1, 2, 3 }));
            });
        rmp::test::check(
            "a save reads back as the Value written, bit for bit, at its version",
            [](bool sealed) {
                const Value v = savable_value();
                const int version = draw_version();
                const Bytes file = encoded(v, version, sealed);
                for (const bool sealed_only : { false, true }) {
                    Value back;
                    const Status status = decoded(file, back, sealed_only);
                    if (sealed_only && !sealed) {
                        RC_ASSERT(status == Status::MODIFIED);
                        continue;
                    }
                    RC_ASSERT(status == Status::OK);
                    RC_ASSERT(back == v);
                    RC_ASSERT(model::exact(back) == model::exact(v));
                    RC_ASSERT(back.version() == version);
                }
            });
        rmp::test::check(
            "a Value as JSON reads back as itself, and writes the same text again", [] {
                const Value v = savable_value();
                const std::string json = rmp::save::detail::to_json(v);
                RC_ASSERT(!json.empty());
                Value back;
                RC_ASSERT(rmp::save::detail::from_json(json, &back));
                RC_ASSERT(model::exact(back) == model::exact(v));
                RC_ASSERT(rmp::save::detail::to_json(back) == json);
            });
        rmp::test::check("a JSON number too large for a double reads as no save at all, "
                         "never as infinity",
                         [] {
                             const std::string number = huge_number();
                             const std::string json =
                                 *rc::gen::element(number, "[" + number + "]",
                                                   R"({"coins":)" + number + "}",
                                                   R"({"a":[1,{"b":)" + number + "}]}");
                             Value out = 7;
                             RC_ASSERT(!rmp::save::detail::from_json(json, &out));
                             RC_ASSERT(out == Value(7));
                             RC_ASSERT(decoded(framed_plain(json, 1), out, false) ==
                                       Status::UNREADABLE);
                             RC_ASSERT(out == Value(7));
                         });
    }

    TEST_CASE("a changed save never reads OK, and a cut one is TRUNCATED") {
        rmp::test::check(
            "one byte changed, added or taken away never reads OK; in the payload, "
            "MODIFIED "
            "or TRUNCATED",
            [](bool sealed) {
                const Bytes file = encoded(savable_value(), 1, sealed);
                const std::size_t header = header_end(file);
                Bytes changed = file;
                const int kind = *rc::gen::inRange(0, 3);
                std::size_t at = 0;
                if (kind == 0) {
                    at = *rc::gen::inRange<std::size_t>(0, file.size());
                    changed[at] ^= *rc::gen::inRange<unsigned char>(1, 255);
                } else if (kind == 1) {
                    at = *rc::gen::inRange<std::size_t>(0, file.size() + 1);
                    changed.insert(changed.begin() + static_cast<std::ptrdiff_t>(at),
                                   *rc::gen::arbitrary<unsigned char>());
                } else {
                    at = *rc::gen::inRange<std::size_t>(0, file.size());
                    changed.erase(changed.begin() + static_cast<std::ptrdiff_t>(at));
                }
                for (const bool sealed_only : { false, true }) {
                    Value out;
                    const Status status = decoded(changed, out, sealed_only);
                    RC_ASSERT(status != Status::OK);
                    if (at <= header)
                        continue; // the header: refused, for one reason or another
                    // The payload: the CRC (or the seal) for a changed byte or one
                    // more, the length for one fewer -- unless a plain file is
                    // refused first, where saves are sealed.
                    const bool refused_first = sealed_only && !sealed;
                    RC_ASSERT(status ==
                              (kind == 2 && !refused_first ? Status::TRUNCATED
                                                           : Status::MODIFIED));
                }
            });
        rmp::test::check(
            "a sealed save changed in any number of bytes never reads OK; in the "
            "payload, "
            "CRC rewritten or not, it is MODIFIED",
            [] {
                const Bytes file = encoded(savable_value(), 1, true);
                const std::size_t header = header_end(file);
                Bytes changed = file;
                const int edits = *rc::gen::inRange(1, 9);
                const bool payload_only = *rc::gen::arbitrary<bool>();
                for (int i = 0; i < edits; ++i) {
                    const auto at = payload_only
                        ? *rc::gen::inRange<std::size_t>(header + 1, changed.size())
                        : *rc::gen::inRange<std::size_t>(0, changed.size());
                    changed[at] ^= *rc::gen::inRange<unsigned char>(1, 255);
                }
                const bool fix_crc = *rc::gen::arbitrary<bool>();
                if (fix_crc) changed = with_fixed_crc(changed);
                RC_PRE(changed != file);
                for (const bool sealed_only : { false, true }) {
                    Value out;
                    const Status status = decoded(changed, out, sealed_only);
                    RC_ASSERT(status != Status::OK);
                    // The CRC catches it, or -- rewritten -- the seal does.
                    if (payload_only) RC_ASSERT(status == Status::MODIFIED);
                }
            });
        rmp::test::check("a save cut short anywhere reads TRUNCATED", [](bool sealed) {
            const Bytes file = encoded(savable_value(), draw_version(), sealed);
            const auto length = *rc::gen::inRange<std::size_t>(1, file.size());
            const Bytes cut(file.begin(),
                            file.begin() + static_cast<std::ptrdiff_t>(length));
            Value out;
            RC_ASSERT(decoded(cut, out, sealed) == Status::TRUNCATED);
        });
        rmp::test::check(
            "a plain save where the game seals its saves reads MODIFIED", [] {
                const Bytes file = encoded(savable_value(), draw_version(), false);
                Value out;
                RC_ASSERT(decoded(file, out, true) == Status::MODIFIED);
            });
    }

    TEST_CASE("a refusal leaves `out` as it was") {
        rmp::test::check(
            "a decode that is not OK leaves `out` exactly as it was, its version "
            "included",
            [](const Value &before) {
                const int kind = *rc::gen::inRange(0, 4);
                Bytes file;
                if (kind == 0) {
                    file =
                        *rc::gen::container<Bytes>(rc::gen::arbitrary<unsigned char>());
                } else if (kind == 1) {
                    const std::string junk = *rmp::test::gen::hostile_string();
                    file = framed_plain(junk, draw_version());
                } else {
                    file = encoded(savable_value(), draw_version(), kind == 3);
                    const auto at = *rc::gen::inRange<std::size_t>(0, file.size());
                    file[at] ^= *rc::gen::inRange<unsigned char>(1, 255);
                    if (*rc::gen::arbitrary<bool>()) file = with_fixed_crc(file);
                }
                const int version = *rc::gen::inRange(1, INT_MAX);
                Value out = before;
                rmp::detail::ValueAccess::set_version(out, version);
                rmp::detail::reset_reports_for_tests();
                const Status status = decoded(file, out, *rc::gen::arbitrary<bool>());
                RC_ASSERT(rmp::detail::report_count() == 0);
                if (status != Status::OK) {
                    RC_ASSERT(model::exact(out) == model::exact(before));
                    RC_ASSERT(out.version() == version);
                } else {
                    RC_ASSERT(model::savable(out));
                }
            });
    }

    TEST_CASE("the slots on disk") {
        const QuietLog quiet;
        const SaveFolder folder;
        rmp::test::check(
            "write, read, exists and remove agree on any slot name, said once when "
            "refused",
            [&folder](const Value &v, bool sealed, bool sealed_only) {
                const std::string slot = draw_slot();
                const bool valid = valid_slot(slot);
                const bool can_save = model::savable(v);
                rmp::detail::reset_reports_for_tests();

                const std::string where = rmp::save::directory();
                RC_ASSERT(where == (folder.path / "").string());

                const bool wrote = rmp::save::write(slot, v, { .encrypted = sealed });
                RC_ASSERT(wrote == (valid && can_save));
                RC_ASSERT(rmp::detail::report_count() == (wrote ? 0 : 1));
                RC_ASSERT(rmp::save::exists(slot) == wrote);

                const Value defaults = Value("defaults");
                Value out = defaults;
                const rmp::save::Result result =
                    rmp::save::read(slot, out, { .sealed_only = sealed_only });
                RC_ASSERT(static_cast<bool>(result) == (result.status == Status::OK));
                if (!valid) {
                    RC_ASSERT(result.status == Status::UNREADABLE);
                } else if (!wrote) {
                    RC_ASSERT(result.status == Status::MISSING);
                } else if (sealed_only && !sealed) {
                    RC_ASSERT(result.status == Status::MODIFIED);
                } else {
                    RC_ASSERT(result.status == Status::OK);
                    RC_ASSERT(model::exact(out) == model::exact(v));
                    RC_ASSERT(out.version() == RMP_SAVE_VERSION);
                }
                if (result.status != Status::OK)
                    RC_ASSERT(model::exact(out) == "s8:defaults");

                RC_ASSERT(rmp::save::remove(slot) == wrote);
                RC_ASSERT(!rmp::save::exists(slot));
                RC_ASSERT(!rmp::save::remove(slot));
                Value after = defaults;
                const rmp::save::Result gone = rmp::save::read(slot, after);
                RC_ASSERT(!gone);
                RC_ASSERT(gone.status == (valid ? Status::MISSING : Status::UNREADABLE));
                RC_ASSERT(after == defaults);
                // Every refusal of the slot name is the same mistake, said once.
                RC_ASSERT(rmp::detail::report_count() == (wrote ? 0 : 1));
            });
        rmp::test::check(
            "whatever a save file holds, read() answers what decode() does, and says "
            "nothing",
            [&folder](bool sealed_only) {
                const int kind = *rc::gen::inRange(0, 3);
                Bytes bytes;
                if (kind == 0) {
                    bytes =
                        *rc::gen::container<Bytes>(rc::gen::arbitrary<unsigned char>());
                } else {
                    bytes = encoded(savable_value(), draw_version(), kind == 2);
                    if (*rc::gen::arbitrary<bool>()) {
                        const auto at = *rc::gen::inRange<std::size_t>(0, bytes.size());
                        bytes[at] ^= *rc::gen::inRange<unsigned char>(1, 255);
                    }
                }
                {
                    std::ofstream file(folder.path / "disk.save", std::ios::binary);
                    file.write(reinterpret_cast<const char *>(bytes.data()),
                               static_cast<std::streamsize>(bytes.size()));
                }
                Value expected = Value::list();
                const Status status = decoded(bytes, expected, sealed_only);
                rmp::detail::reset_reports_for_tests();
                Value out = Value::list();
                const rmp::save::Result result =
                    rmp::save::read("disk", out, { .sealed_only = sealed_only });
                RC_ASSERT(result.status == status);
                RC_ASSERT(model::exact(out) == model::exact(expected));
                RC_ASSERT(out.version() == expected.version());
                RC_ASSERT(rmp::detail::report_count() == 0);
                RC_ASSERT(rmp::save::remove("disk"));
            });
    }
}
