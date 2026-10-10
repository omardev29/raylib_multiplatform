// ---------------------------------------------------------------------------
// rmp::Value: the tree rmp::save writes. Pure data -- no files, no JSON, no
// raylib -- so all of it is tested directly in tests/save_test.cpp.
//
// The rule every function here keeps: reading never changes anything, never
// throws and never says anything. That is why [] on a non-const Value hands
// out a Ref -- a path, walked again on each use -- instead of a Value&: a
// Value& to a key that is not there has to be created first, and then
// reading a save grows it, and reading v["list"][99] has to either create 99
// elements or complain. A Ref creates only when it is written through, and
// only a write that cannot land (through a number, at a negative index, past
// the end of a list) is reported.
// ---------------------------------------------------------------------------

#include <rmp/save.h>

#include "internal.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <string>
#include <utility>

namespace rmp {

namespace {

const Value &none() {
    static const Value NOTHING;
    return NOTHING;
}

const char *type_name(Value::Type type) {
    switch (type) {
        case Value::Type::NONE:
            return "nothing";
        case Value::Type::BOOL:
            return "a bool";
        case Value::Type::NUMBER:
            return "a number";
        case Value::Type::STRING:
            return "a string";
        case Value::Type::LIST:
            return "a list";
        case Value::Type::OBJECT:
            return "an object";
    }
    return "?";
}

} // namespace

Value Value::list() { return Value(Type::LIST); }

Value Value::object() { return Value(Type::OBJECT); }

bool Value::as_bool(bool fallback) const {
    return _type == Type::BOOL ? _boolean : fallback;
}

int Value::as_int(int fallback) const {
    if (_type != Type::NUMBER || std::isnan(_number)) return fallback;
    // Clamped BEFORE the cast: converting a double outside int's range to int
    // is undefined behaviour, and 1e10 coins in an edited save must not be.
    if (_number >= static_cast<double>(INT_MAX)) return INT_MAX;
    if (_number <= static_cast<double>(INT_MIN)) return INT_MIN;
    return static_cast<int>(_number);
}

float Value::as_float(float fallback) const {
    if (_type != Type::NUMBER || std::isnan(_number)) return fallback;
    return static_cast<float>(_number);
}

std::string_view Value::as_string(std::string_view fallback) const {
    return _type == Type::STRING ? std::string_view(_string) : fallback;
}

// An object member that holds nothing does not exist, for every function
// here: `v["x"] = rmp::Value{}` clears a key, and "nothing" and "missing" read
// the same everywhere -- size(), key(), contains(), == and the file.
int Value::size() const {
    if (_type == Type::LIST) return static_cast<int>(_items.size());
    if (_type != Type::OBJECT) return 0;
    return static_cast<int>(std::ranges::count_if(
        _items, [](const Value &item) { return item.type() != Type::NONE; }));
}

bool Value::contains(std::string_view key) const {
    return (*this)[key].type() != Type::NONE;
}

std::string_view Value::key(int index) const {
    // "" and not {}: a view the framework returns always has a NUL after it,
    // and a default one has no characters at all, not even that.
    if (_type != Type::OBJECT || index < 0) return "";
    int seen = 0;
    for (std::size_t i = 0; i < _keys.size(); i++) {
        if (_items[i].type() == Type::NONE) continue;
        if (seen++ == index) return _keys[i];
    }
    return "";
}

const Value &Value::operator[](std::string_view key) const {
    if (_type != Type::OBJECT) return none();
    for (std::size_t i = 0; i < _keys.size(); i++) {
        if (_keys[i] == key) return _items[i];
    }
    return none();
}

const Value &Value::operator[](int index) const {
    if (_type != Type::LIST || index < 0 ||
        std::cmp_greater_equal(index, _items.size())) {
        return none();
    }
    return _items[static_cast<std::size_t>(index)];
}

void Value::push(Value value) {
    if (_type == Type::NONE) _type = Type::LIST;
    if (_type != Type::LIST) {
        RMP_REPORT_ONCE(
            "SAVE: push() on %s: only a list can be pushed to; nothing was added",
            type_name(_type));
        return;
    }
    _items.push_back(std::move(value));
}

bool Value::erase(std::string_view key) {
    if (_type != Type::OBJECT) return false;
    for (std::size_t i = 0; i < _keys.size(); i++) {
        if (_keys[i] == key) {
            // A member holding nothing was not there, so erasing it is
            // "false" -- and it goes all the same.
            const bool was_there = _items[i].type() != Type::NONE;
            _keys.erase(_keys.begin() + static_cast<std::ptrdiff_t>(i));
            _items.erase(_items.begin() + static_cast<std::ptrdiff_t>(i));
            return was_there;
        }
    }
    return false;
}

bool operator==(const Value &a, const Value &b) {
    if (a.type() != b.type()) return false;
    using Type = Value::Type;
    switch (a.type()) {
        case Type::NONE:
            return true;
        case Type::BOOL:
            return a.as_bool() == b.as_bool();
        case Type::NUMBER:
            return a.number() == b.number();
        case Type::STRING:
            return a.as_string() == b.as_string();
        case Type::LIST:
            return a.items() == b.items();
        case Type::OBJECT:
            // Keys in any order: two objects that say the same thing are equal
            // however they were built -- and a member holding nothing is not
            // there.
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.keys().size(); i++) {
                if (a.items()[i].type() == Type::NONE) continue;
                if (!(a.items()[i] == b[a.keys()[i]])) return false;
            }
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Ref
// ---------------------------------------------------------------------------

Value::Ref Value::operator[](std::string_view key) {
    return Ref(*this, Ref::Step{ .key = std::string(key), .index = 0, .is_key = true });
}

Value::Ref Value::operator[](int index) {
    return Ref(*this, Ref::Step{ .key = {}, .index = index, .is_key = false });
}

Value::Ref::Ref(Value &root, Step first) : _root(root) {
    _path.push_back(std::move(first));
}

Value::Ref Value::Ref::operator[](std::string_view key) const {
    Ref deeper = *this;
    deeper.extend(Step{ .key = std::string(key), .index = 0, .is_key = true });
    return deeper;
}

Value::Ref Value::Ref::operator[](int index) const {
    Ref deeper = *this;
    deeper.extend(Step{ .key = {}, .index = index, .is_key = false });
    return deeper;
}

const Value &Value::Ref::get() const {
    const Value *at = &_root;
    for (const Step &step : _path) {
        at = step.is_key ? &(*at)[std::string_view(step.key)] : &(*at)[step.index];
    }
    return *at;
}

Value *Value::Ref::find() {
    Value *at = &_root;
    for (const Step &step : _path) {
        Value *next = nullptr;
        if (step.is_key && at->type() == Type::OBJECT) {
            for (std::size_t i = 0; i < at->keys().size(); i++) {
                if (at->keys()[i] == step.key) next = &at->items()[i];
            }
        } else if (!step.is_key && at->type() == Type::LIST && step.index >= 0 &&
                   std::cmp_less(step.index, at->items().size())) {
            next = &at->items()[static_cast<std::size_t>(step.index)];
        }
        if (next == nullptr) return nullptr;
        at = next;
    }
    return at;
}

Value *Value::Ref::materialise() {
    // Two passes. The first walks the path touching nothing and says whether
    // the write can land at all; only then does the second create what is
    // missing. In one pass, a write that failed halfway -- v["a"]["b"][5] = 1
    // on an empty Value -- had already turned "a" and "b" into an object and
    // an empty list, which then went into the file while the log said
    // nothing was written.
    const Value *at = &_root; // nullptr once the path leaves what exists
    for (const Step &step : _path) {
        const Type type = at != nullptr ? at->type() : Type::NONE;
        if (step.is_key) {
            if (type != Type::NONE && type != Type::OBJECT) {
                RMP_REPORT_ONCE_KEYED(
                    step.key.c_str(),
                    "SAVE: writing [\"%s\"] into %s: nothing was written. "
                    "Assign it first -- v[...] = rmp::Value::object() -- if "
                    "it is meant to change type",
                    step.key.c_str(), type_name(type));
                return nullptr;
            }
            const Value *next = nullptr;
            if (type == Type::OBJECT) {
                for (std::size_t i = 0; i < at->keys().size(); i++) {
                    if (at->keys()[i] == step.key) next = &at->items()[i];
                }
            }
            at = next;
        } else {
            const int count =
                type == Type::LIST ? static_cast<int>(at->items().size()) : 0;
            if ((type != Type::NONE && type != Type::LIST) || step.index < 0 ||
                step.index > count) {
                // A list grows by one, at the end: v[1000000000] = x filling a
                // billion NONEs in between is a crash, not a feature.
                RMP_REPORT_ONCE(
                    "SAVE: writing [%d] into %s of size %d: nothing was written. A "
                    "list is written at 0..size(), and size() appends one",
                    step.index, type_name(type), count);
                return nullptr;
            }
            at = step.index < count ? &at->items()[static_cast<std::size_t>(step.index)]
                                    : nullptr;
        }
    }

    Value *here = &_root;
    for (const Step &step : _path) {
        if (step.is_key) {
            if (here->type() == Type::NONE) *here = Value::object();
            Value *found = nullptr;
            for (std::size_t i = 0; i < here->keys().size(); i++) {
                if (here->keys()[i] == step.key) found = &here->items()[i];
            }
            if (found == nullptr) {
                here->keys().push_back(step.key);
                here->items().emplace_back();
                found = &here->items().back();
            }
            here = found;
        } else {
            if (here->type() == Type::NONE) *here = Value::list();
            if (std::cmp_equal(step.index, here->items().size()))
                here->items().emplace_back();
            here = &here->items()[static_cast<std::size_t>(step.index)];
        }
    }
    return here;
}

Value::Ref &Value::Ref::operator=(Value value) {
    // `value` is already a copy, taken before the path is created, so
    // v["a"] = v["a"]["b"] and v["copy"] = v are both well defined.
    if (Value *slot = materialise()) *slot = std::move(value);
    return *this;
}

Value::Ref &Value::Ref::operator=(const Ref &other) {
    if (this != &other) *this = Value(other.get());
    return *this;
}

void Value::Ref::push(Value value) {
    if (Value *slot = materialise()) slot->push(std::move(value));
}

bool Value::Ref::erase(std::string_view key) {
    Value *at = find();
    return at != nullptr && at->erase(key);
}

} // namespace rmp
