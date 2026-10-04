#pragma once
// ---------------------------------------------------------------------------
// rmp/save.h — rmp::Value, and saving the game.
//
//     rmp::Value v;
//     v["level"] = 7;
//     v["name"] = "Omar";
//     v["unlocked"].push("forest");
//     rmp::save::write("slot1", v);
//
//     rmp::Value v;
//     if (rmp::save::read("slot1", &v)) {
//         int level = v["level"].as_int(1); // 1 when the key is not there
//     }
//
// EVERY READ HAS A DEFAULT, AND NOTHING THROWS. A save written by yesterday's
// version is not corrupt, it is missing the keys that were added since -- and a
// missing key reading as its default is what lets an update ship without a
// migration nine times out of ten. A key of the wrong type reads as the default
// too. For the tenth time, [save] version in the .toml is stored in the file and
// v.version() hands it back:
//
//     if (v.version() < 2) v["coins"] = v["gold"].as_int(0);
//
// WHAT A FAILED READ TELLS YOU. read() is true or false in an `if`, and when it
// is false, .status says why -- because "there is no save yet", "the file was
// cut short" (the power went during a write) and "the file has been changed"
// are three different messages to show a player:
//
//     const rmp::save::Result r = rmp::save::read("slot1", &v);
//     if (r.status == rmp::save::Status::MODIFIED) show("this save was edited");
//
// WHERE IT GOES. The user's data folder, or next to the executable with [save]
// portable -- see directory(). A write goes to a temporary file first and
// replaces the old save only once it is complete, so a crash mid-write leaves
// the previous save intact. On the web it lands in the browser's IndexedDB and
// survives a reload.
//
// SEALED SAVES are tamper RESISTANCE, not security: the key is in the binary.
// They stop a player opening the file in a text editor and giving themselves a
// thousand coins, and any change to a sealed file is detected. Nothing stops
// someone determined, and nothing here pretends to.
//
// The JSON underneath is cJSON, and it appears in no header: this file is all
// a game ever sees of it.
// ---------------------------------------------------------------------------

#include <rmp/config.h>

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace rmp {

namespace detail {
struct ValueAccess; // how rmp::save sets version(); nothing a game touches
} // namespace detail

// A tree: nothing, a bool, a number, a string, a list or an object -- exactly
// what JSON holds, so a real structure fits and not only flat pairs.
class Value {
public:
    enum class Type { NONE, BOOL, NUMBER, STRING, LIST, OBJECT };
    class Ref;

    Value() = default; // NONE
    // A template so that only a real bool gets here: as a plain Value(bool),
    // every pointer in the program would quietly convert to `true`.
    template <class B>
        requires std::is_same_v<B, bool>
    Value(B b) : type_(Type::BOOL), boolean_(b) {}
    // Every number type, as a double: an int survives exactly up to 2^53.
    template <class N>
        requires(std::is_arithmetic_v<N> && !std::is_same_v<N, bool>)
    Value(N n) : type_(Type::NUMBER), number_(static_cast<double>(n)) {}
    Value(std::string_view s) : type_(Type::STRING), string_(s) {}
    Value(std::string s) : type_(Type::STRING), string_(std::move(s)) {}
    // Without this a string literal would become a bool, which is what C++
    // does with a pointer when it is given the choice.
    Value(const char *s) : type_(Type::STRING), string_(s != nullptr ? s : "") {}

    // An empty list or object, for when "nothing yet" has to be one of them:
    // v["inventory"] = rmp::Value::list();
    [[nodiscard]] static Value list();
    [[nodiscard]] static Value object();

    // ---- reading: never throws, never changes anything ---------------------
    // The fallback when this is not of that type -- a missing key is NONE.
    // Types are not converted: a number is not a bool and "7" is not 7.
    // as_int truncates towards zero and clamps to int's range.
    [[nodiscard]] bool as_bool(bool fallback = false) const;
    [[nodiscard]] int as_int(int fallback = 0) const;
    [[nodiscard]] float as_float(float fallback = 0) const;
    // Valid while this Value is alive and unchanged -- and adding to the
    // object or list that holds it moves it. The fallback's lifetime is yours.
    [[nodiscard]] std::string_view as_string(std::string_view fallback = "") const;

    [[nodiscard]] Type type() const { return type_; }
    // Elements of a list, keys of an object; 0 for anything else. A member
    // assigned NONE counts as absent, here and in key(), contains(), == and
    // the file: nothing and missing read the same.
    [[nodiscard]] int size() const;
    [[nodiscard]] bool contains(std::string_view key) const;
    // The i-th key of an object, in the order the keys were added; "" out of
    // range. With operator[](key) it is how to walk an object you did not write.
    [[nodiscard]] std::string_view key(int index) const;

    // ---- structure ---------------------------------------------------------
    // [] on a const Value reads: a missing key, an index out of range, or a
    // Value of another type is NONE.
    //
    // [] on a non-const Value gives a Ref -- a path into this Value that READS
    // exactly like the const one, creating nothing, and WRITES by creating
    // what is missing on the way: v["a"]["b"].as_int(1) leaves v as it was,
    // v["a"]["b"] = 1 builds both objects. So reading a save never grows it,
    // and a read never says anything. A write that cannot land -- through a
    // number read from an old save, at a negative index, past the end of a
    // list (size() appends one) -- goes nowhere and is said once: turning a
    // number into an object behind your back would lose it.
    const Value &operator[](std::string_view key) const;
    const Value &operator[](int index) const;
    Ref operator[](std::string_view key);
    Ref operator[](int index);

    // Append. NONE becomes a list first; anything else that is not a list
    // is left alone and said once.
    void push(Value value);
    // Remove a key from an object. False when it was not there.
    bool erase(std::string_view key);

    // The [save] version this came from, when rmp::save::read() filled it; a
    // Value made in code is of this build's version. Not compared by ==.
    [[nodiscard]] int version() const { return version_; }

    // Same type and same contents, keys in any order.
    friend bool operator==(const Value &a, const Value &b);

private:
    friend struct detail::ValueAccess;

    Type type_ = Type::NONE;
    bool boolean_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<std::string> keys_; // OBJECT: keys_[i] names items_[i]
    std::vector<Value> items_; // LIST and OBJECT
    int version_ = RMP_SAVE_VERSION;
};

// What [] on a non-const Value gives: the root it started from and the keys
// and indices it went through, looked up again on every use -- so a Ref
// kept while other keys are added never points at moved memory. You do not
// name this type; you index, read and assign through it:
//
//     v["player"]["x"] = 3;                 // creates "player"
//     int x = v["player"]["x"].as_int(0);   // creates nothing
//     const rmp::Value &p = v["player"];    // the Value itself, to read
//
// To fill a whole subtree, build it and assign it: v["player"] = player;
class Value::Ref {
public:
    // Reading: the Value this names, or NONE where the path leads nowhere.
    [[nodiscard]] const Value &get() const;
    // NOLINTNEXTLINE(google-explicit-constructor): a Ref reads as its Value
    operator const Value &() const { return get(); }
    [[nodiscard]] bool as_bool(bool fallback = false) const {
        return get().as_bool(fallback);
    }
    [[nodiscard]] int as_int(int fallback = 0) const { return get().as_int(fallback); }
    [[nodiscard]] float as_float(float fallback = 0) const {
        return get().as_float(fallback);
    }
    [[nodiscard]] std::string_view as_string(std::string_view fallback = "") const {
        return get().as_string(fallback);
    }
    [[nodiscard]] Type type() const { return get().type(); }
    [[nodiscard]] int size() const { return get().size(); }
    [[nodiscard]] bool contains(std::string_view key) const {
        return get().contains(key);
    }
    [[nodiscard]] std::string_view key(int index) const { return get().key(index); }

    // Deeper, still creating nothing.
    [[nodiscard]] Ref operator[](std::string_view key) const;
    [[nodiscard]] Ref operator[](int index) const;

    // Writing: the path is created, then assigned. A Ref assigned from a Ref
    // copies the VALUE, never the path -- v["a"] = v["b"] copies b into a.
    Ref &operator=(Value value);
    Ref &operator=(const Ref &other);
    // No move assignment on purpose: a temporary Ref on the right -- which is
    // what v["a"] = v["b"] hands over -- goes through the copy above, which
    // copies the value. A defaulted one would move the PATH and rebind.
    Ref(const Ref &) = default;
    Ref(Ref &&) = default;
    ~Ref() = default;
    void push(Value value);
    bool erase(std::string_view key);

private:
    friend class Value;
    struct Step {
        std::string key; // when is_key
        int index = 0; // when not
        bool is_key = true;
    };
    Ref(Value *root, Step first);
    // The Value to write into, created on the way; nullptr (said once) when
    // the path cannot be made. find() is the same walk creating nothing.
    Value *materialise();
    Value *find();

    Value *root_;
    std::vector<Step> path_;
};

namespace save {

enum class Status {
    OK,
    MISSING, // nothing saved in that slot yet: the first run
    TRUNCATED, // cut short, as a write the power went out on would be
    MODIFIED, // the checksum or the seal does not match: edited, or damaged
    UNREADABLE, // not a save at all, or one from a newer framework
};

// A read's outcome. True in an `if` only when it is OK, so the three-line
// load stays three lines; .status is there when the reason matters.
struct Result {
    Status status = Status::MISSING;
    explicit operator bool() const { return status == Status::OK; }
};

// Declaration order is the order they are written in, as C++20 requires.
struct WriteOptions {
    bool encrypted = RMP_SAVE_ENCRYPT != 0; // [save] encrypt decides the default
};

struct ReadOptions {
    // With [save] encrypt on, a PLAIN file reads as MODIFIED: anyone can write
    // one -- the format is documented and a plain save shows it -- and
    // accepting it would let a player replace a sealed save with their own.
    // A game that turns sealing on in an update reads its players' old plain
    // saves with { .sealed_only = false } for as long as they may have one.
    bool sealed_only = RMP_SAVE_ENCRYPT != 0;
};

// A slot is a file name without the extension: letters, digits, '_', '-' and
// '.', not starting with '.', at most 64 of them. Anything else is refused
// and said once -- a slot name is not a place to put a path.
//
// False when it could not be written; the previous save, if any, is intact.
// Also false, and said once, for a Value that could not be read back: nested
// deeper than 64 levels, or with a NUL character in a string or a key.
bool write(std::string_view slot, const Value &value, const WriteOptions &options = {});

// Fills *out and says so. On anything but OK, *out is left exactly as it was,
// so a game can fill it with defaults first and read over them.
Result read(std::string_view slot, Value *out, const ReadOptions &options = {});

[[nodiscard]] bool exists(std::string_view slot);
bool remove(std::string_view slot); // false when there was nothing to remove

// Where saves are written: the folder [save] portable asked for if it can be
// written, otherwise the user's data folder -- never where the .toml asked
// for if that turned out to be read-only. With a trailing separator. With
// [save] portable, read() looks in both folders and takes the newer save, so
// a session that had to fall back and the next one that did not agree.
[[nodiscard]] std::string directory();

} // namespace save

} // namespace rmp
