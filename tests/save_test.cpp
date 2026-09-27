// ---------------------------------------------------------------------------
// rmp::Value and rmp::save, headless, with the save folder in a temporary
// directory.
//
// The test that matters most is the first one in "missing keys": reading a key
// that is not there returns its default and does not throw. It is what
// separates a save system that survives an update from one that breaks
// people's games the day a key is added.
//
// After that, the file format is attacked the way the real world attacks it:
// cut at EVERY byte (the power went during a write), every byte flipped (a
// bad sector, a hex editor), bytes appended, the header edited, a sealed
// payload re-checksummed after tampering. Each has to land on the right
// Status, and never on OK.
// ---------------------------------------------------------------------------

#include <doctest.h>

#include "../src/rmp/internal.h"
#include "../src/rmp/save_internal.h"

#include <rmp/save.h>

#include <cctype>
#include <chrono>
#include <climits>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>
#include <random>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using rmp::Value;
using rmp::save::Status;
using Bytes = rmp::save::detail::Bytes;

// A Value can be made from the things a save holds, and NOT from an arbitrary
// pointer: as a plain Value(bool), every pointer in a game would quietly
// convert to `true`.
static_assert(std::is_constructible_v<Value, bool>);
static_assert(std::is_constructible_v<Value, int>);
static_assert(std::is_constructible_v<Value, float>);
static_assert(std::is_constructible_v<Value, double>);
static_assert(std::is_constructible_v<Value, unsigned long long>);
static_assert(std::is_constructible_v<Value, const char *>);
static_assert(std::is_constructible_v<Value, std::string>);
static_assert(std::is_constructible_v<Value, std::string_view>);
static_assert(!std::is_constructible_v<Value, const int *>);
static_assert(!std::is_constructible_v<Value, void *>);
static_assert(!std::is_convertible_v<const float *, Value>);

namespace {

// A folder of its own under the system temp directory, removed afterwards.
struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device device;
        path = fs::temp_directory_path() /
            ("rmp_save_test_" + std::to_string(device()) + "_" +
             std::to_string(device()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
    [[nodiscard]] std::string str() const { return path.string(); }
};

struct Fixture {
    TempDir dir;
    Fixture() {
        rmp::save::detail::set_folders_for_tests("", dir.str());
        rmp::detail::reset_reports_for_tests();
    }
    ~Fixture() {
        rmp::save::detail::reset_for_tests();
        rmp::detail::reset_reports_for_tests();
    }
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
};

// The six types, nested three levels.
Value sample() {
    Value v;
    v["level"] = 7;
    v["coins"] = 1250;
    v["name"] = "Omar";
    v["tutorial_done"] = true;
    v["ratio"] = 0.1;
    v["unlocked"].push("forest");
    v["unlocked"].push("cave");
    v["player"]["position"]["x"] = 12.5f;
    v["player"]["position"]["y"] = -3.25f;
    v["player"]["inventory"].push(Value{}); // a NONE in a list keeps its place
    v["player"]["inventory"].push(3);
    v["player"]["inventory"][1] = 4;
    v["empty_list"] = Value::list();
    v["empty_object"] = Value::object();
    return v;
}

Bytes encoded(const Value &v, bool sealed, int version = APP_SAVE_VERSION) {
    Bytes out;
    REQUIRE(rmp::save::detail::encode(v, version, sealed, &out));
    return out;
}

Status decode(const Bytes &file, Value *out = nullptr) {
    Value scratch;
    return rmp::save::detail::decode(file, out != nullptr ? out : &scratch);
}

std::string text_of(const Bytes &bytes) { return { bytes.begin(), bytes.end() }; }

std::size_t header_end(const Bytes &file) {
    for (std::size_t i = 0; i < file.size(); i++) {
        if (file[i] == '\n') return i;
    }
    return file.size();
}

// Rewrites the CRC in the header so it matches whatever the payload now is:
// what someone who knows the format would do after editing a save.
Bytes with_fixed_crc(Bytes file) {
    const std::size_t nl = header_end(file);
    const std::string header(file.begin(),
                             file.begin() + static_cast<std::ptrdiff_t>(nl));
    const std::size_t space = header.rfind(' ');
    const std::string prefix = header.substr(0, space);
    std::string covered = prefix + "\n";
    covered.append(file.begin() + static_cast<std::ptrdiff_t>(nl) + 1, file.end());
    char crc[9];
    std::snprintf(
        crc, sizeof crc, "%08x",
        static_cast<unsigned>(rmp::save::detail::crc32(
            reinterpret_cast<const unsigned char *>(covered.data()), covered.size())));
    std::memcpy(file.data() + space + 1, crc, 8);
    return file;
}

std::string file_text(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

int g_strict_stops = 0;

} // namespace

TEST_SUITE("save: Value") {
    // -----------------------------------------------------------------------
    // Missing keys: THE test
    // -----------------------------------------------------------------------

    TEST_CASE("reading a key that is not there returns its default and does not throw") {
        Value v;
        v["level"] = 3;
        const Value &c = v;
        CHECK_NOTHROW((void)v["missing"].as_int(1));
        CHECK(v["missing"].as_int(1) == 1);
        CHECK(c["missing"].as_int(1) == 1);
        CHECK(v["missing"].as_bool(true) == true);
        CHECK(v["missing"].as_float(2.5f) == doctest::Approx(2.5));
        CHECK(v["missing"].as_string("Player") == "Player");
        // Nested, through keys that do not exist at any level.
        CHECK_NOTHROW((void)c["a"]["b"]["c"].as_int(5));
        CHECK(c["a"]["b"]["c"].as_int(5) == 5);
        CHECK(v["a"]["b"]["c"].as_int(5) == 5);
        // And a list index that is not there.
        CHECK(c["list"][3].as_int(9) == 9);
        CHECK(c[0].as_int(9) == 9);
        CHECK(v["level"].as_int(1) == 3);
    }

    TEST_CASE("reading through a non-const Value changes nothing, however deep") {
        // std::map's operator[] inserts. Ours hands out a path that creates
        // only when written through, so a save that is read is not grown by
        // every key the game looked for -- not even by the objects on the way.
        Value v;
        v["kept"] = 1;
        const Value before = v;
        (void)v["looked_for"].as_int();
        (void)v["deep"]["deeper"]["deepest"].as_int();
        (void)v["list"][99].as_int();
        (void)v["kept"]["inside"].as_int();
        CHECK(rmp::save::detail::to_json(v) == R"({"kept":1})");
        CHECK(v == before);
        CHECK(v.size() == 1);
        CHECK(v["kept"].type() == Value::Type::NUMBER);
    }

    TEST_CASE("a read never says anything -- so [dev] strict cannot abort a load") {
        // v["list"][99] used to be a write site like any other, and reported
        // "past the end" on a READ -- fatal under strict.
        rmp::detail::reset_reports_for_tests();
        Value v;
        v["n"] = 5;
        v["list"].push(1);
        (void)v["list"][99].as_int(0);
        (void)v["list"][-1].as_int(0);
        (void)v["n"]["x"].as_int(0);
        (void)v["n"][0].as_int(0);
        (void)v["missing"]["a"][3]["b"].as_string("x");
        CHECK(rmp::detail::report_count() == 0);
        rmp::detail::reset_reports_for_tests();
    }

    TEST_CASE("a member assigned nothing is not there: size, key, contains, ==") {
        Value v;
        v["a"] = 1;
        const Value before = v;
        v["cleared"] = Value{};
        CHECK(v.size() == 1);
        CHECK_FALSE(v.contains("cleared"));
        CHECK(v.key(0) == "a");
        CHECK(v.key(1).empty());
        CHECK(v == before);
        CHECK(before == v);
        CHECK(rmp::save::detail::to_json(v) == R"({"a":1})");
        v["cleared"] = 2; // and it can be given a value again
        CHECK(v.size() == 2);
        CHECK(v.key(1) == "cleared");
        CHECK_FALSE(v == before);
    }

    TEST_CASE("a key of the wrong type reads as the default too") {
        Value v;
        v["n"] = 7;
        v["s"] = "7";
        v["b"] = true;
        CHECK(v["s"].as_int(1) == 1); // "7" is not 7
        CHECK(v["n"].as_string("x") == "x");
        CHECK(v["n"].as_bool(false) == false); // a number is not a bool
        CHECK(v["b"].as_int(2) == 2);
        CHECK(v["n"]["inside"].as_int(4) == 4); // a number indexed by key
        const Value &c = v;
        CHECK(c["n"][0].as_int(6) == 6);
    }

    // -----------------------------------------------------------------------
    // Numbers and strings
    // -----------------------------------------------------------------------

    TEST_CASE("as_int truncates towards zero and clamps instead of overflowing") {
        CHECK(Value(7.9).as_int() == 7);
        CHECK(Value(-7.9).as_int() == -7);
        CHECK(Value(1e10).as_int() == INT_MAX);
        CHECK(Value(-1e10).as_int() == INT_MIN);
        CHECK(Value(std::numeric_limits<double>::infinity()).as_int() == INT_MAX);
        CHECK(Value(std::nan("")).as_int(42) == 42);
        CHECK(Value(std::nan("")).as_float(1.5f) == doctest::Approx(1.5));
        CHECK(Value(INT_MAX).as_int() == INT_MAX);
        CHECK(Value(INT_MIN).as_int() == INT_MIN);
    }

    TEST_CASE("each constructor makes the type it says, and a literal is a string") {
        CHECK(Value().type() == Value::Type::NONE);
        CHECK(Value(true).type() == Value::Type::BOOL);
        CHECK(Value(3).type() == Value::Type::NUMBER);
        CHECK(Value(3u).type() == Value::Type::NUMBER);
        CHECK(Value(2.5f).type() == Value::Type::NUMBER);
        CHECK(Value(std::string("s")).type() == Value::Type::STRING);
        CHECK(Value(std::string_view("s")).type() == Value::Type::STRING);
        const Value v = "yes";
        CHECK(v.type() == Value::Type::STRING);
        CHECK(v.as_string() == "yes");
        char buffer[8] = "Player";
        const Value from_buffer = buffer; // a char array in a settings struct
        CHECK(from_buffer.as_string() == "Player");
        const char *null_text = nullptr;
        CHECK(Value(null_text).as_string("fallback").empty());
        CHECK(Value::list().type() == Value::Type::LIST);
        CHECK(Value::object().type() == Value::Type::OBJECT);
    }

    // -----------------------------------------------------------------------
    // Structure, and the Ref
    // -----------------------------------------------------------------------

    TEST_CASE("writing through [] builds objects and lists out of nothing") {
        Value v;
        v["a"]["b"]["c"] = 1;
        v["list"][0] = "zero";
        v["list"][1] = "one";
        CHECK(v.type() == Value::Type::OBJECT);
        CHECK(v["a"]["b"]["c"].as_int() == 1);
        CHECK(v["list"].type() == Value::Type::LIST);
        CHECK(v["list"].size() == 2);
        CHECK(v["list"][1].as_string() == "one");
    }

    TEST_CASE("a write that cannot land is lost, said once, and harms nothing") {
        rmp::detail::reset_reports_for_tests();
        Value v;
        v["gold"] = 50;
        v["gold"]["amount"] = 99; // a number written into by key
        CHECK(v["gold"].as_int() == 50); // untouched, NOT turned into an object
        CHECK(rmp::detail::report_count() == 1);
        v["gold"]["amount"] = 98;
        CHECK(rmp::detail::report_count() == 1); // once per key
        CHECK(v["gold"]["amount"].as_int(-1) == -1); // and reading it is NONE

        Value list = Value::list();
        list.push(1);
        list[5] = 6; // past the end
        list[-1] = 7; // negative
        CHECK(list.size() == 1);
        CHECK(list[0].as_int() == 1);
        list.push(2);
        list[2] = 3; // == size() appends
        CHECK(list.size() == 3);
        list[0] = 10; // and an existing one is replaced
        CHECK(list[0].as_int() == 10);

        // And a write that fails deep down creates nothing on the way to it:
        // this used to leave {"a":{"b":[]}} behind.
        Value empty;
        empty["a"]["b"][5] = 1;
        CHECK(empty.type() == Value::Type::NONE);
        Value inv;
        inv["inv"].push(Value{});
        inv["inv"].push(3);
        const Value inv_before = inv;
        inv["inv"][0]["slot"][7] = "sword";
        CHECK(inv == inv_before);
        CHECK(inv["inv"][0].type() == Value::Type::NONE);

        Value number = 3;
        number.push(4);
        CHECK(number.as_int() == 3);
        CHECK(number.size() == 0);
        rmp::detail::reset_reports_for_tests();
    }

    TEST_CASE("a Ref copies VALUES: v[a] = v[b], v[a] = v[a][b], v[copy] = v") {
        Value v;
        v["b"]["x"] = 1;
        v["a"] = v["b"]; // a temporary Ref on the right: still a copy of b
        v["b"]["x"] = 2;
        CHECK(v["a"]["x"].as_int() == 1);
        CHECK(v["b"]["x"].as_int() == 2);

        auto kept = v["b"];
        auto other = v["a"];
        other = kept; // a named Ref on the right: the value again, not the path
        CHECK(v["a"]["x"].as_int() == 2);
        other = 7;
        CHECK(v["a"].as_int() == 7);
        CHECK(v["b"]["x"].as_int() == 2); // `other` was never rebound to b

        v["nest"]["inner"]["leaf"] = 3;
        v["nest"] = v["nest"]["inner"]; // a subtree over its own parent
        CHECK(v["nest"]["leaf"].as_int() == 3);
        CHECK_FALSE(v["nest"].contains("inner"));

        Value w;
        w["x"] = 1;
        w["copy"] = w; // the whole tree into one of its own keys
        CHECK(w["copy"]["x"].as_int() == 1);
        CHECK_FALSE(w["copy"].contains("copy"));
    }

    TEST_CASE("a Ref kept while the tree grows still finds its place") {
        // A Value& into a vector dangles the moment the vector reallocates;
        // a Ref is a path, looked up again on every use.
        Value v;
        v["first"] = 0;
        auto first = v["first"];
        for (int i = 0; i < 200; i++) v["k" + std::to_string(i)] = i;
        first = 5;
        CHECK(v["first"].as_int() == 5);
        CHECK(v["k199"].as_int() == 199);
    }

    TEST_CASE("push, erase and binding a const Value& through a Ref") {
        Value v;
        v["inventory"].push("sword"); // creates the list
        v["inventory"].push("shield");
        CHECK(v["inventory"].size() == 2);
        v["player"]["hp"] = 3;
        const Value &player = v["player"]; // the Value itself, to read
        CHECK(player["hp"].as_int() == 3);
        CHECK(v["player"].erase("hp"));
        CHECK_FALSE(v["player"].erase("hp"));
        CHECK_FALSE(v["nothing"]["here"].erase("x")); // erasing creates nothing
        CHECK_FALSE(v.contains("nothing"));
        rmp::detail::reset_reports_for_tests();
        v["hp_number"] = 1;
        v["hp_number"].push(2); // a number is not a list: said, not done
        CHECK(v["hp_number"].as_int() == 1);
        CHECK(rmp::detail::report_count() == 1);
        rmp::detail::reset_reports_for_tests();
    }

    TEST_CASE("contains, size, key(i) and erase") {
        Value v;
        v["b"] = 2;
        v["a"] = 1;
        CHECK(v.size() == 2);
        CHECK(v.contains("a"));
        CHECK_FALSE(v.contains("c"));
        CHECK(v.key(0) == "b"); // insertion order
        CHECK(v.key(1) == "a");
        CHECK(v.key(2).empty());
        CHECK(v.key(-1).empty());
        CHECK(v.erase("b"));
        CHECK_FALSE(v.erase("b"));
        CHECK(v.size() == 1);
        CHECK(v.key(0) == "a");
        CHECK(Value(3).size() == 0);
        CHECK_FALSE(Value(3).contains("a"));
        CHECK_FALSE(Value(3).erase("a"));
        v["cleared"] = Value{};
        CHECK_FALSE(v.erase("cleared")); // nothing was there
    }

    TEST_CASE("equality: same type and contents, keys in any order, version ignored") {
        Value a;
        a["x"] = 1;
        a["y"] = "two";
        Value b;
        b["y"] = "two";
        b["x"] = 1;
        CHECK(a == b);
        b["x"] = 2;
        CHECK_FALSE(a == b);
        CHECK_FALSE(Value(1) == Value("1"));
        CHECK_FALSE(Value(true) == Value(1));
        CHECK(Value() == Value());
        CHECK_FALSE(Value::list() == Value::object());
        Value list_a;
        list_a.push(1);
        list_a.push(2);
        Value list_b;
        list_b.push(2);
        list_b.push(1);
        CHECK_FALSE(list_a == list_b); // a list's order is its content
        CHECK(sample() == sample());
        CHECK(a["x"] == Value(1)); // through a Ref, both ways
        CHECK(Value(1) == a["x"]);
    }

    TEST_CASE("a Value made in code is of this build's version") {
        CHECK(Value().version() == APP_SAVE_VERSION);
        CHECK(sample().version() == APP_SAVE_VERSION);
    }
}

TEST_SUITE("save: format") {
    TEST_CASE("CRC-32 is the standard one") {
        const char *check = "123456789";
        CHECK(rmp::save::detail::crc32(reinterpret_cast<const unsigned char *>(check),
                                       9) == 0xCBF43926U);
        CHECK(rmp::save::detail::crc32(nullptr, 0) == 0U);
    }

    TEST_CASE("the six types, nested three levels, survive a round trip") {
        for (const bool sealed : { false, true }) {
            CAPTURE(sealed);
            Value back;
            REQUIRE(decode(encoded(sample(), sealed), &back) == Status::OK);
            CHECK(back == sample());
            CHECK(back["player"]["inventory"][0].type() == Value::Type::NONE);
            CHECK(back["player"]["inventory"][1].as_int() == 4);
            CHECK(back["empty_list"].type() == Value::Type::LIST);
            CHECK(back["empty_object"].type() == Value::Type::OBJECT);
        }
    }

    TEST_CASE("numbers come back exactly") {
        // cJSON's own printer keeps 15 digits whenever they read back within
        // an epsilon, and 2^53 came back as 9007199254740990.
        const std::vector<double> numbers = { 0.1,
                                              -0.1,
                                              1e300,
                                              -1e-300,
                                              9007199254740992.0,
                                              9007199254740993.0,
                                              2147483647.0,
                                              -2147483648.0,
                                              std::numbers::pi,
                                              static_cast<double>(0.1f),
                                              -0.0,
                                              5e-324 };
        Value v;
        for (const double n : numbers) v["n"].push(n);
        Value back;
        REQUIRE(rmp::save::detail::from_json(rmp::save::detail::to_json(v), &back));
        REQUIRE(back["n"].size() == static_cast<int>(numbers.size()));
        for (int i = 0; i < back["n"].size(); i++) {
            CAPTURE(i);
            CHECK(rmp::detail::ValueAccess::number(back["n"][i]) ==
                  numbers[static_cast<std::size_t>(i)]);
        }
        // A float written and read back is the same float, bit for bit.
        const Value f = 0.1f;
        Value fb;
        REQUIRE(rmp::save::detail::from_json(rmp::save::detail::to_json(f), &fb));
        CHECK(fb.as_float() == 0.1f);
        // And a whole number is written as one, and a short one short: 17
        // digits every time would say 0.10000000000000001.
        CHECK(rmp::save::detail::to_json(Value(1250)) == "1250");
        CHECK(rmp::save::detail::to_json(Value(0.1)) == "0.1");
        CHECK(rmp::save::detail::to_json(Value(0.1f)) == "0.10000000149011612");
    }

    TEST_CASE("numbers are written in C notation whatever the locale") {
        // A game that calls setlocale() for its UI must not start writing
        // 0,5 -- which is not JSON -- into its saves.
        const std::string was = std::setlocale(LC_NUMERIC, nullptr);
        const char *comma = nullptr;
        for (const char *name :
             { "de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "es_ES.UTF-8" }) {
            if (std::setlocale(LC_NUMERIC, name) != nullptr) {
                comma = name;
                break;
            }
        }
        if (comma == nullptr) {
            MESSAGE("no comma-decimal locale installed here: skipped");
            return;
        }
        Value v;
        v["half"] = 0.5;
        v["big"] = 1234567.25;
        const std::string json = rmp::save::detail::to_json(v);
        Value back;
        const bool parsed = rmp::save::detail::from_json(json, &back);
        const std::string during = std::setlocale(LC_NUMERIC, nullptr);
        std::setlocale(LC_NUMERIC, was.c_str());
        CHECK(json == R"({"half":0.5,"big":1234567.25})");
        CHECK(during.find(comma) != std::string::npos); // the game's locale, given back
        REQUIRE(parsed);
        CHECK(back == v);
    }

    TEST_CASE("NaN and infinity are written as nothing, and read as the default") {
        Value v;
        v["nan"] = std::nan("");
        v["inf"] = std::numeric_limits<double>::infinity();
        v["list"].push(-std::numeric_limits<double>::infinity());
        Value back;
        REQUIRE(decode(encoded(v, false), &back) == Status::OK);
        CHECK(back["nan"].as_float(1.0f) == doctest::Approx(1.0));
        CHECK_FALSE(back.contains("nan"));
        CHECK(back["inf"].as_int(2) == 2);
        CHECK(back["list"].size() == 1); // its place kept, as NONE
        CHECK(back["list"][0].type() == Value::Type::NONE);
    }

    TEST_CASE("strings: quotes, backslashes, control characters, UTF-8, empty") {
        const std::vector<std::string> strings = {
            "",
            "plain",
            "with \"quotes\"",
            "back\\slash",
            "line\nbreak\ttab\r",
            "\x01\x1f",
            "espa\xc3\xb1ol \xe2\x9c\x93 \xf0\x9f\x8e\xae",
            R"({"looks": ["like", "json"]})"
        };
        Value v;
        for (const std::string &s : strings) v["s"].push(s);
        v[std::string("key with \"quotes\" and \xc3\xb1")] = "value";
        Value back;
        REQUIRE(decode(encoded(v, false), &back) == Status::OK);
        for (std::size_t i = 0; i < strings.size(); i++) {
            CAPTURE(i);
            CHECK(back["s"][static_cast<int>(i)].as_string() == strings[i]);
        }
        CHECK(back["key with \"quotes\" and \xc3\xb1"].as_string() == "value");
    }

    TEST_CASE("an object keeps the order its keys were added in") {
        Value v;
        v["zebra"] = 1;
        v["apple"] = 2;
        v["mango"] = 3;
        CHECK(rmp::save::detail::to_json(v) == R"({"zebra":1,"apple":2,"mango":3})");
    }

    TEST_CASE("the JSON has to be one value and nothing after it") {
        Value v;
        CHECK(rmp::save::detail::from_json(R"({"a":1})", &v));
        CHECK(rmp::save::detail::from_json(" {\"a\":1} \n\t", &v)); // whitespace is fine
        CHECK_FALSE(rmp::save::detail::from_json(R"({"a":1}x)", &v));
        CHECK_FALSE(rmp::save::detail::from_json(R"({"a":1}{"b":2})", &v));
        CHECK_FALSE(rmp::save::detail::from_json(std::string("{}\0garbage", 10), &v));
        CHECK_FALSE(rmp::save::detail::from_json("", &v));
        CHECK_FALSE(rmp::save::detail::from_json("{", &v));
        CHECK_FALSE(rmp::save::detail::from_json(R"({"a":})", &v));
    }

    TEST_CASE("nesting: as deep as can be read back, and not one level more") {
        // The parser refuses past its nesting limit, so a Value deeper than
        // that must be refused at write -- a save that writes and never reads
        // back is the worst kind. kMaxDepth here and CJSON_NESTING_LIMIT in
        // cjson_impl.c must agree, and this is what says so.
        constexpr int kLimit = rmp::save::detail::kMaxDepth;
        const auto nested = [](int depth) {
            Value inner = 1;
            for (int i = 0; i < depth; i++) {
                Value outer;
                outer["d"] = std::move(inner);
                inner = std::move(outer);
            }
            return inner;
        };
        int deepest_ok = 0;
        for (int depth = kLimit - 3; depth <= kLimit + 3; depth++) {
            Bytes out;
            const bool wrote = rmp::save::detail::encode(nested(depth), 1, false, &out);
            if (!wrote) continue;
            CAPTURE(depth);
            Value back;
            CHECK(decode(out, &back) == Status::OK); // anything written reads back
            deepest_ok = depth;
        }
        CHECK(deepest_ok == kLimit); // every level up to the limit, and not beyond
        Bytes out;
        CHECK_FALSE(rmp::save::detail::encode(nested(kLimit + 1), 1, false, &out));
        // And the parser, on its own: JSON nested past the limit is refused
        // even when somebody else wrote it.
        const auto arrays = [](int depth) {
            return std::string(static_cast<std::size_t>(depth), '[') +
                std::string(static_cast<std::size_t>(depth), ']');
        };
        Value v;
        CHECK(rmp::save::detail::from_json(arrays(kLimit), &v));
        CHECK_FALSE(rmp::save::detail::from_json(arrays(kLimit + 1), &v));
    }

    TEST_CASE("a string or a key with a NUL in it is refused, not cut short") {
        // cJSON reads strings as C strings: "ab\0cd" came back as "ab", OK.
        Value v;
        v["s"] = std::string("ab\0cd", 5);
        Bytes out;
        CHECK_FALSE(rmp::save::detail::encode(v, 1, false, &out));
        CHECK(rmp::save::detail::to_json(v).empty());
        Value k;
        k[std::string("a\0b", 3)] = 1;
        CHECK_FALSE(rmp::save::detail::encode(k, 1, false, &out));
        Value fine;
        fine["s"] = "no nul here";
        CHECK(rmp::save::detail::encode(fine, 1, false, &out));
    }

    TEST_CASE("a plain save is readable text: a header line, then the JSON") {
        Value v;
        v["coins"] = 5;
        const std::string text = text_of(encoded(v, false, 3));
        CHECK(text.rfind("rmp-save 1 3 plain ", 0) == 0);
        CHECK(text.find("\n{\"coins\":5}") != std::string::npos);
    }

    TEST_CASE("a sealed save does not contain its keys or values in clear") {
        Value v;
        v["coins"] = 123456;
        v["name"] = "secret_name";
        const std::string text = text_of(encoded(v, true));
        CHECK(text.rfind("rmp-save 1 ", 0) == 0);
        CHECK(text.find(" sealed ") != std::string::npos);
        CHECK(text.find("coins") == std::string::npos);
        CHECK(text.find("123456") == std::string::npos);
        CHECK(text.find("secret_name") == std::string::npos);
    }

    TEST_CASE("two seals of the same Value differ: the nonce is fresh every time") {
        const Bytes a = encoded(sample(), true);
        const Bytes b = encoded(sample(), true);
        CHECK(a.size() == b.size());
        CHECK_FALSE(a == b);
        Value va;
        Value vb;
        CHECK(decode(a, &va) == Status::OK);
        CHECK(decode(b, &vb) == Status::OK);
        CHECK(va == vb);
    }

    // -----------------------------------------------------------------------
    // Versions
    // -----------------------------------------------------------------------

    TEST_CASE(
        "a file written as version 1 reads as version 1, and new keys are defaults") {
        Value old;
        old["gold"] = 40;
        Value back;
        REQUIRE(decode(encoded(old, false, 1), &back) == Status::OK);
        CHECK(back.version() == 1);
        CHECK(back["gold"].as_int() == 40);
        CHECK(back["coins"].as_int(0) == 0); // added in "version 2": its default
        // The migration the header comment shows.
        if (back.version() < 2) back["coins"] = back["gold"].as_int(0);
        CHECK(back["coins"].as_int() == 40);
        Value later;
        REQUIRE(decode(encoded(old, true, 77), &later) == Status::OK);
        CHECK(later.version() == 77);
    }

    // -----------------------------------------------------------------------
    // Damage, and telling the kinds apart
    // -----------------------------------------------------------------------

    TEST_CASE("cut at every byte: TRUNCATED, and never OK") {
        for (const bool sealed : { false, true }) {
            const Bytes full = encoded(sample(), sealed);
            for (std::size_t cut = 1; cut < full.size(); cut++) {
                const Bytes part(full.begin(),
                                 full.begin() + static_cast<std::ptrdiff_t>(cut));
                const Status s = decode(part);
                if (s != Status::TRUNCATED) {
                    CAPTURE(sealed);
                    CAPTURE(cut);
                    CHECK(s == Status::TRUNCATED);
                }
            }
            CHECK(decode(Bytes{}) ==
                  Status::UNREADABLE); // an empty file is nothing of ours
        }
    }

    TEST_CASE("every byte of the payload flipped: MODIFIED, and never OK") {
        for (const bool sealed : { false, true }) {
            const Bytes full = encoded(sample(), sealed);
            const std::size_t start = header_end(full) + 1;
            for (std::size_t i = start; i < full.size(); i++) {
                for (const unsigned char mask : { 0x01, 0x80, 0xFF }) {
                    Bytes bad = full;
                    bad[i] ^= mask;
                    const Status s = decode(bad);
                    if (s != Status::MODIFIED) {
                        CAPTURE(sealed);
                        CAPTURE(i);
                        CAPTURE(static_cast<int>(mask));
                        CHECK(s == Status::MODIFIED);
                    }
                }
            }
        }
    }

    TEST_CASE("every byte of the header flipped: never OK") {
        // The header is not all equal: a flipped digit in the version is a
        // changed file (the CRC covers it), a flipped letter of the magic is
        // not ours at all. What none of them may ever be is OK.
        for (const bool sealed : { false, true }) {
            const Bytes full = encoded(sample(), sealed);
            for (std::size_t i = 0; i < header_end(full); i++) {
                Bytes bad = full;
                bad[i] ^= 0x01;
                if (decode(bad) == Status::OK) {
                    CAPTURE(sealed);
                    CAPTURE(i);
                    FAIL("a flipped header byte read as OK");
                }
            }
        }
    }

    TEST_CASE("the version edited by hand is caught, not believed") {
        Bytes file = encoded(sample(), false, 1);
        const std::size_t at = text_of(file).find(" 1 plain ");
        REQUIRE(at != std::string::npos);
        file[at + 1] = '9';
        CHECK(decode(file) == Status::MODIFIED);
    }

    TEST_CASE("bytes appended: MODIFIED") {
        for (const bool sealed : { false, true }) {
            Bytes file = encoded(sample(), sealed);
            file.push_back('x');
            CHECK(decode(file) == Status::MODIFIED);
        }
    }

    TEST_CASE("truncated and modified are DIFFERENT answers") {
        const Bytes full = encoded(sample(), false);
        const Bytes cut(full.begin(), full.end() - 5);
        Bytes flipped = full;
        flipped[flipped.size() - 3] ^= 0x20;
        CHECK(decode(cut) == Status::TRUNCATED);
        CHECK(decode(flipped) == Status::MODIFIED);
    }

    TEST_CASE("an edited plain save with the CRC recomputed is believed -- that is what "
              "plain means") {
        // Stated as a test so nobody mistakes the checksum for protection: it
        // tells a damaged file from a whole one. Sealing is what resists edits.
        Value v;
        v["coins"] = 5;
        Bytes file = encoded(v, false);
        const std::size_t at = text_of(file).find("\"coins\":5");
        REQUIRE(at != std::string::npos);
        file[at + 8] = '9';
        CHECK(decode(file) == Status::MODIFIED);
        Value back;
        CHECK(decode(with_fixed_crc(file), &back) == Status::OK);
        CHECK(back["coins"].as_int() == 9);
    }

    TEST_CASE("an edited sealed save with the CRC recomputed is still MODIFIED") {
        // The seal is what stands behind the checksum: someone who knows the
        // format can fix the CRC, and cannot forge the tag.
        const Bytes full = encoded(sample(), true);
        const std::size_t start = header_end(full) + 1;
        for (std::size_t i = start; i < full.size(); i++) {
            Bytes bad = full;
            bad[i] ^= 0x04;
            const Status s = decode(with_fixed_crc(bad));
            if (s != Status::MODIFIED) {
                CAPTURE(i);
                CHECK(s == Status::MODIFIED);
            }
        }
    }

    TEST_CASE(
        "a sealed header edited and re-checksummed is MODIFIED: the seal covers it") {
        // The header is the seal's associated data. Take it out and every
        // other test still passed -- this one is why that cannot happen again.
        Bytes file = encoded(sample(), true, 1);
        const std::size_t at = text_of(file).find(" 1 sealed ");
        REQUIRE(at != std::string::npos);
        file[at + 1] = '7'; // the version
        CHECK(decode(with_fixed_crc(file)) == Status::MODIFIED);
    }

    TEST_CASE("where the game seals its saves, a plain file is MODIFIED") {
        // The one edit a seal exists to stop: replace the sealed file with
        // your own plain one and fix its CRC -- the format is documented.
        const auto bytes = [](const std::string &s) { return Bytes(s.begin(), s.end()); };
        const Bytes forged =
            with_fixed_crc(bytes("rmp-save 1 1 plain 17 00000000\n{\"coins\":1000000}"));
        Value v;
        CHECK(rmp::save::detail::decode(forged, &v, true) == Status::MODIFIED);
        CHECK(v.type() == Value::Type::NONE);
        CHECK(rmp::save::detail::decode(forged, &v, false) == Status::OK);
        CHECK(v["coins"].as_int() == 1000000);
        Value sealed;
        CHECK(rmp::save::detail::decode(encoded(sample(), true), &sealed, true) ==
              Status::OK);
        CHECK(rmp::save::ReadOptions{}.sealed_only == (APP_SAVE_ENCRYPT != 0));
    }

    TEST_CASE("a sealed payload too short to hold its nonce and tag is MODIFIED") {
        const auto bytes = [](const std::string &s) { return Bytes(s.begin(), s.end()); };
        for (int length = 0; length < 40; length++) {
            CAPTURE(length);
            const std::string head =
                "rmp-save 1 1 sealed " + std::to_string(length) + " 00000000\n";
            const std::string body(static_cast<std::size_t>(length), 'x');
            CHECK(decode(with_fixed_crc(bytes(head + body))) == Status::MODIFIED);
        }
    }

    TEST_CASE("sealed relabelled as plain, re-checksummed: never OK") {
        Bytes file = encoded(sample(), true);
        const std::size_t at = text_of(file).find("sealed");
        REQUIRE(at != std::string::npos);
        file.erase(file.begin() + static_cast<std::ptrdiff_t>(at),
                   file.begin() + static_cast<std::ptrdiff_t>(at) + 6);
        const char *plain = "plain";
        file.insert(file.begin() + static_cast<std::ptrdiff_t>(at), plain, plain + 5);
        CHECK(decode(with_fixed_crc(file)) != Status::OK);
    }

    TEST_CASE("not a save, or a newer format: UNREADABLE") {
        const auto bytes = [](const std::string &s) { return Bytes(s.begin(), s.end()); };
        CHECK(decode(bytes("{\"coins\": 5}")) == Status::UNREADABLE);
        CHECK(decode(bytes("PK\x03\x04 a zip file")) == Status::UNREADABLE);
        CHECK(decode(bytes("rmp-save 2 1 plain 2 00000000\n{}")) == Status::UNREADABLE);
        CHECK(decode(bytes("rmp-save 1 1 rot13 2 00000000\n{}")) == Status::UNREADABLE);
        CHECK(decode(bytes("rmp-save one 1 plain 2 00000000\n{}")) == Status::UNREADABLE);
        CHECK(decode(bytes("rmp-save 1 1 plain 2 00000000 extra\n{}")) ==
              Status::UNREADABLE);
        CHECK(decode(bytes("rmp-save " + std::string(200, '1') + "\n{}")) ==
              Status::UNREADABLE);
        // A header that checks out around JSON that does not parse.
        CHECK(decode(with_fixed_crc(bytes("rmp-save 1 1 plain 3 00000000\n{x}"))) ==
              Status::UNREADABLE);
        // Or around JSON with something after it.
        CHECK(decode(with_fixed_crc(bytes("rmp-save 1 1 plain 3 00000000\n{}x"))) ==
              Status::UNREADABLE);
    }

    TEST_CASE("the header is read strictly: only what our writer writes") {
        // sscanf read "+1", " 1" and "1x" as 1. Whatever this line says, it
        // has to be exactly what encode() would have put there.
        const auto bytes = [](const std::string &s) { return Bytes(s.begin(), s.end()); };
        for (const char *header :
             { "rmp-save +1 1 plain 2 00000000", "rmp-save 1 +1 plain 2 00000000",
               "rmp-save  1 1 plain 2 00000000", "rmp-save 1 1 plain 2x 00000000",
               "rmp-save 1 1 plain 2 0000000", "rmp-save 1 1 plain 2 000000000",
               "rmp-save 1 1 plain 2 0000000g", "rmp-save 1 1 plain -2 00000000",
               "rmp-save 1 99999999999 plain 2 00000000", "rmp-save 1 1 plain 2",
               "rmp-save 1 1 plain 2 00000000 ", "RMP-SAVE 1 1 plain 2 00000000" }) {
            CAPTURE(header);
            CHECK(decode(bytes(std::string(header) + "\n{}")) == Status::UNREADABLE);
        }
        // Not only unparseable ones: what parses but is not how our writer
        // puts it -- upper-case hex, leading zeros, a version below 1 -- is
        // not ours either, even with its CRC made to match.
        // The right CRC, in upper case -- with_fixed_crc() writes lower case.
        Bytes upper = with_fixed_crc(bytes("rmp-save 1 1 plain 2 00000000\n{}"));
        REQUIRE(decode(upper) == Status::OK);
        for (std::size_t i = header_end(upper) - 8; i < header_end(upper); i++) {
            upper[i] = static_cast<unsigned char>(std::toupper(upper[i]));
        }
        REQUIRE(text_of(upper).find("F27F7A6A") != std::string::npos); // it has letters
        CHECK(decode(upper) == Status::UNREADABLE);
        for (const char *header :
             { "rmp-save 1 01 plain 2 00000000", "rmp-save 1 1 plain 02 00000000",
               "rmp-save 01 1 plain 2 00000000", "rmp-save 1 0 plain 2 00000000",
               "rmp-save 1 -5 plain 2 00000000" }) {
            CAPTURE(header);
            CHECK(decode(with_fixed_crc(bytes(std::string(header) + "\n{}"))) ==
                  Status::UNREADABLE);
        }
    }

    TEST_CASE("decoding over a Value leaves it alone unless the answer is OK") {
        Value kept;
        kept["coins"] = 99;
        Bytes cut = encoded(sample(), false);
        cut.resize(cut.size() / 2);
        CHECK(decode(cut, &kept) == Status::TRUNCATED);
        CHECK(kept["coins"].as_int() == 99);
    }
}

TEST_SUITE("save: files") {
    TEST_CASE_FIXTURE(Fixture,
                      "write, read back, and the slot's file is where directory() says") {
        REQUIRE(rmp::save::write("slot1", sample()));
        Value back;
        const rmp::save::Result r = rmp::save::read("slot1", &back);
        CHECK(static_cast<bool>(r));
        CHECK(r.status == Status::OK);
        CHECK(back == sample());
        CHECK(fs::exists(dir.path / "slot1.save"));
        const std::string where = rmp::save::directory();
        CHECK(fs::equivalent(fs::path(where), dir.path));
        CHECK((where.back() == '/' || where.back() == '\\'));
    }

    TEST_CASE_FIXTURE(Fixture, "the default of write() is [save] encrypt") {
        REQUIRE(rmp::save::write("slot", sample()));
        const std::string text = file_text(dir.path / "slot.save");
        CHECK(text.find(APP_SAVE_ENCRYPT != 0 ? " sealed " : " plain ") !=
              std::string::npos);
        CHECK(rmp::save::WriteOptions{}.encrypted == (APP_SAVE_ENCRYPT != 0));
    }

    TEST_CASE_FIXTURE(Fixture, "the save folder is created on the first write") {
        const fs::path deeper = dir.path / "not" / "yet" / "there";
        rmp::save::detail::set_folders_for_tests("", deeper.string());
        CHECK_FALSE(fs::exists(deeper));
        REQUIRE(rmp::save::write("slot", sample()));
        CHECK(fs::exists(deeper / "slot.save"));
    }

    TEST_CASE_FIXTURE(Fixture, "no save yet is MISSING, and the defaults stay put") {
        Value v;
        v["level"] = 1;
        const rmp::save::Result r = rmp::save::read("never_written", &v);
        CHECK_FALSE(static_cast<bool>(r));
        CHECK(r.status == Status::MISSING);
        CHECK(v["level"].as_int() == 1);
        CHECK_FALSE(rmp::save::exists("never_written"));
    }

    TEST_CASE_FIXTURE(Fixture, "a damaged file on disk leaves the defaults alone too") {
        REQUIRE(rmp::save::write("slot", sample()));
        const fs::path file = dir.path / "slot.save";
        fs::resize_file(file, fs::file_size(file) - 4);
        Value v;
        v["level"] = 1;
        CHECK(rmp::save::read("slot", &v).status == Status::TRUNCATED);
        CHECK(v["level"].as_int() == 1);
    }

    TEST_CASE_FIXTURE(Fixture,
                      "a write replaces the old save and leaves no temporary behind") {
        Value first;
        first["n"] = 1;
        Value second;
        second["n"] = 2;
        REQUIRE(rmp::save::write("slot", first));
        REQUIRE(rmp::save::write("slot", second));
        Value back;
        REQUIRE(rmp::save::read("slot", &back));
        CHECK(back["n"].as_int() == 2);
        int files = 0;
        for (const auto &entry : fs::directory_iterator(dir.path)) {
            CAPTURE(entry.path().filename().string());
            CHECK(entry.path().extension() == ".save");
            files++;
        }
        CHECK(files == 1);
    }

    TEST_CASE_FIXTURE(Fixture, "sealed and plain both round-trip through the disk") {
        REQUIRE(rmp::save::write("plain", sample(), { .encrypted = false }));
        REQUIRE(rmp::save::write("sealed", sample(), { .encrypted = true }));
        Value a;
        Value b;
        CHECK(rmp::save::read("plain", &a));
        CHECK(rmp::save::read("sealed", &b));
        CHECK(a == sample());
        CHECK(b == sample());
        CHECK(file_text(dir.path / "sealed.save").find("coins") == std::string::npos);
        CHECK(file_text(dir.path / "plain.save").find("coins") != std::string::npos);
    }

    TEST_CASE_FIXTURE(Fixture, "exists and remove") {
        CHECK_FALSE(rmp::save::remove("slot"));
        REQUIRE(rmp::save::write("slot", sample()));
        CHECK(rmp::save::exists("slot"));
        CHECK(rmp::save::remove("slot"));
        CHECK_FALSE(rmp::save::exists("slot"));
        CHECK_FALSE(rmp::save::remove("slot"));
    }

    TEST_CASE_FIXTURE(Fixture, "a slot is a file name, never a path") {
        for (const char *good : { "slot1", "a", "a.b-c_D9", "autosave.2" }) {
            CAPTURE(good);
            CHECK(rmp::save::detail::valid_slot(good));
            CHECK(rmp::save::write(good, sample()));
        }
        const std::string too_long(65, 'a');
        for (const std::string &bad :
             { std::string(), std::string("../escape"), std::string("a/b"),
               std::string("a\\b"), std::string(".hidden"), std::string(".."),
               std::string("sp ace"), std::string("colon:"), std::string("nul\0x", 5),
               too_long, std::string("\xc3\xb1") }) {
            CAPTURE(bad);
            CHECK_FALSE(rmp::save::detail::valid_slot(bad));
            CHECK_FALSE(rmp::save::write(bad, sample()));
            Value v;
            CHECK(rmp::save::read(bad, &v).status == Status::UNREADABLE);
            CHECK_FALSE(rmp::save::exists(bad));
            CHECK_FALSE(rmp::save::remove(bad));
        }
        CHECK(rmp::save::detail::valid_slot(std::string(64, 'a')));
        // Nothing escaped the folder.
        CHECK_FALSE(fs::exists(dir.path.parent_path() / "escape.save"));
    }

    TEST_CASE_FIXTURE(Fixture, "a bad slot name is said once per name") {
        rmp::detail::reset_reports_for_tests();
        (void)rmp::save::write("a/b", sample());
        (void)rmp::save::write("a/b", sample());
        CHECK(rmp::detail::report_count() == 1);
        (void)rmp::save::write("c/d", sample());
        CHECK(rmp::detail::report_count() == 2);
    }

    TEST_CASE_FIXTURE(Fixture,
                      "a huge file called like a save is UNREADABLE, not an allocation") {
        // Sparse, so it costs nothing on disk: 64 MB and one byte, behind a
        // header that declares exactly that length -- read whole, it would
        // be a CRC mismatch (MODIFIED); only the cap makes it UNREADABLE.
        const std::uintmax_t size = std::uintmax_t{ 64 } * 1024 * 1024 + 1;
        std::string head;
        std::uintmax_t payload = size;
        for (int pass = 0; pass < 3; pass++) { // the length's digits are in the header
            head = "rmp-save 1 1 plain " + std::to_string(payload) + " 00000000\n";
            payload = size - head.size();
        }
        const fs::path file = dir.path / "huge.save";
        std::ofstream(file, std::ios::binary) << head;
        fs::resize_file(file, size);
        Value v;
        CHECK(rmp::save::read("huge", &v).status == Status::UNREADABLE);
    }

    TEST_CASE_FIXTURE(Fixture, "read into nullptr is refused, not a crash") {
        REQUIRE(rmp::save::write("slot", sample()));
        CHECK(rmp::save::read("slot", nullptr).status == Status::UNREADABLE);
    }

    TEST_CASE_FIXTURE(
        Fixture, "a folder that cannot be written: write says false, nothing crashes") {
        // Under a regular FILE, so creating the folder fails even for root --
        // which is what CI runs as, and where chmod proves nothing.
        const fs::path blocker = dir.path / "a_file";
        std::ofstream(blocker) << "x";
        rmp::save::detail::set_folders_for_tests("", (blocker / "saves").string());
        CHECK_FALSE(rmp::save::write("slot", sample()));
        Value v;
        CHECK(rmp::save::read("slot", &v).status == Status::MISSING);
    }
}

TEST_SUITE("save: portable") {
    TEST_CASE(
        "a save made while the portable folder was blocked is found the next session") {
        // Session 1: a portable build in a folder it cannot write -- the save
        // goes to the user's folder. Session 2: the game was moved somewhere
        // writable, as the log suggested. Reading only the chosen folder, it
        // found nothing, and the player's progress was gone without a word.
        const TempDir root;
        const TempDir user;
        const fs::path blocker = root.path / "blocked";
        std::ofstream(blocker) << "x";
        rmp::save::detail::set_folders_for_tests((blocker / "saves").string(),
                                                 user.str());
        Value progress;
        progress["level"] = 9;
        REQUIRE(rmp::save::write("slot", progress));
        REQUIRE(fs::exists(user.path / "slot.save"));

        const fs::path moved = root.path / "moved" / "saves";
        rmp::save::detail::set_folders_for_tests(moved.string(), user.str());
        CHECK(fs::equivalent(fs::path(rmp::save::directory()), moved)); // writable again
        Value back;
        REQUIRE(rmp::save::read("slot", &back));
        CHECK(back["level"].as_int() == 9);
        CHECK(rmp::save::exists("slot"));
        rmp::save::detail::reset_for_tests();
    }

    TEST_CASE(
        "with a save in both folders, the newer one is read, and remove takes both") {
        // The other half: a write-time fallback (the stick filled up) leaves
        // the newer save in the user's folder and an older one next to the
        // game. Reading the older one would roll the player back.
        const TempDir portable;
        const TempDir user;
        rmp::save::detail::set_folders_for_tests(portable.str(), user.str());
        const auto put = [](const fs::path &path, int level) {
            Value v;
            v["level"] = level;
            Bytes bytes;
            REQUIRE(rmp::save::detail::encode(v, 1, false, &bytes));
            std::ofstream(path, std::ios::binary)
                .write(reinterpret_cast<const char *>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
        };
        put(portable.path / "slot.save", 1);
        put(user.path / "slot.save", 2);
        const auto now = fs::file_time_type::clock::now();
        fs::last_write_time(portable.path / "slot.save", now - std::chrono::hours(2));
        fs::last_write_time(user.path / "slot.save", now - std::chrono::hours(1));
        Value back;
        REQUIRE(rmp::save::read("slot", &back));
        CHECK(back["level"].as_int() == 2);
        fs::last_write_time(portable.path / "slot.save", now); // and the other way round
        REQUIRE(rmp::save::read("slot", &back));
        CHECK(back["level"].as_int() == 1);

        CHECK(rmp::save::remove("slot"));
        CHECK_FALSE(fs::exists(portable.path / "slot.save"));
        CHECK_FALSE(fs::exists(user.path / "slot.save")); // or it would come back
        CHECK_FALSE(rmp::save::exists("slot"));
        rmp::save::detail::reset_for_tests();
    }

    TEST_CASE(
        "a portable folder that passes the probe and fails the write falls back, once") {
        // The write-time fallback, exercised: the slot's name is taken by a
        // non-empty folder, so the temporary is written and the rename fails.
        const TempDir portable;
        const TempDir user;
        fs::create_directories(portable.path / "slot.save" / "in_the_way");
        rmp::save::detail::set_folders_for_tests(portable.str(), user.str());
        REQUIRE(fs::equivalent(fs::path(rmp::save::directory()), portable.path));
        CHECK(rmp::save::write("slot", sample()));
        CHECK(fs::exists(user.path / "slot.save"));
        CHECK(rmp::save::detail::fallbacks() == 1);
        CHECK(fs::equivalent(fs::path(rmp::save::directory()), user.path));
        CHECK_FALSE(fs::exists(portable.path / "slot.save.tmp")); // cleaned up
        CHECK(rmp::save::write("other", sample()));
        CHECK(rmp::save::detail::fallbacks() == 1); // once
        rmp::save::detail::reset_for_tests();
    }

    TEST_CASE("a writable portable folder is used") {
        const TempDir portable;
        const TempDir user;
        rmp::save::detail::set_folders_for_tests((portable.path / "saves").string(),
                                                 user.str());
        CHECK(fs::equivalent(fs::path(rmp::save::directory()), portable.path / "saves"));
        REQUIRE(rmp::save::write("slot", sample()));
        CHECK(fs::exists(portable.path / "saves" / "slot.save"));
        CHECK_FALSE(fs::exists(user.path / "slot.save"));
        CHECK(rmp::save::detail::fallbacks() == 0);
        // The probe cleaned up after itself.
        CHECK_FALSE(fs::exists(portable.path / "saves" / ".rmp-write-test"));
        rmp::save::detail::reset_for_tests();
    }

    TEST_CASE("an unwritable portable folder falls back to the user's, says so ONCE, and "
              "directory() tells the truth") {
        // The Program Files case: a portable build somewhere it cannot write.
        const TempDir root;
        const TempDir user;
        const fs::path blocker = root.path / "Program Files";
        std::ofstream(blocker) << "a file where the folder should be";
        rmp::save::detail::set_folders_for_tests((blocker / "game" / "saves").string(),
                                                 user.str());
        rmp::detail::reset_reports_for_tests();

        CHECK(fs::equivalent(fs::path(rmp::save::directory()), user.path));
        REQUIRE(rmp::save::write("slot", sample()));
        REQUIRE(rmp::save::write("slot", sample()));
        Value back;
        REQUIRE(rmp::save::read("slot", &back));
        CHECK(back == sample());
        CHECK(fs::exists(user.path / "slot.save"));
        CHECK(rmp::save::detail::fallbacks() == 1);
        CHECK(fs::equivalent(fs::path(rmp::save::directory()), user.path));
        // A plain log, not a diagnostic: the machine is not a mistake in the
        // game, and [dev] strict must not abort over it.
        CHECK(rmp::detail::report_count() == 0);
        rmp::save::detail::reset_for_tests();
    }

    TEST_CASE(
        "[dev] strict does not stop a game over a read-only folder or a missing key") {
        const TempDir root;
        const TempDir user;
        const fs::path blocker = root.path / "ro";
        std::ofstream(blocker) << "x";
        rmp::save::detail::set_folders_for_tests((blocker / "saves").string(),
                                                 user.str());
        rmp::detail::reset_reports_for_tests();
        g_strict_stops = 0;
        const bool was = rmp::detail::strict();
        const rmp::detail::StrictHandler previous =
            rmp::detail::set_strict_handler([] { g_strict_stops++; });
        rmp::detail::set_strict(true);

        Value v;
        CHECK(rmp::save::read("slot", &v).status == Status::MISSING);
        CHECK(v["missing"]["deeper"].as_int(3) == 3);
        CHECK(v["list"][42].as_int(4) == 4);
        CHECK(rmp::save::write("slot", sample()));

        rmp::detail::set_strict(was);
        rmp::detail::set_strict_handler(previous);
        CHECK(g_strict_stops == 0);
        rmp::save::detail::reset_for_tests();
        rmp::detail::reset_reports_for_tests();
    }
}

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__EMSCRIPTEN__) && \
    !defined(__ANDROID__)
TEST_SUITE("save: where, on this platform") {
    TEST_CASE("Linux and the BSDs: $XDG_DATA_HOME when absolute, else ~/.local/share") {
        const char *old_xdg = std::getenv("XDG_DATA_HOME");
        const char *old_home = std::getenv("HOME");
        const std::string saved_xdg = old_xdg != nullptr ? old_xdg : "";
        const std::string saved_home = old_home != nullptr ? old_home : "";

        setenv("HOME", "/home/player", 1);
        setenv("XDG_DATA_HOME", "/data", 1);
        CHECK(rmp::save::detail::user_folder() == "/data/" APP_NAME "/");
        setenv("XDG_DATA_HOME", "/data/", 1);
        CHECK(rmp::save::detail::user_folder() == "/data/" APP_NAME "/");
        // The spec: a relative XDG_DATA_HOME is invalid and ignored.
        setenv("XDG_DATA_HOME", "relative/path", 1);
        CHECK(rmp::save::detail::user_folder() ==
              "/home/player/.local/share/" APP_NAME "/");
        unsetenv("XDG_DATA_HOME");
        CHECK(rmp::save::detail::user_folder() ==
              "/home/player/.local/share/" APP_NAME "/");

        if (old_xdg != nullptr) {
            setenv("XDG_DATA_HOME", saved_xdg.c_str(), 1);
        } else {
            unsetenv("XDG_DATA_HOME");
        }
        if (old_home != nullptr) {
            setenv("HOME", saved_home.c_str(), 1);
        } else {
            unsetenv("HOME");
        }
    }
}
#endif
