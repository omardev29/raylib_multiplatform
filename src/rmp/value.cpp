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
//
// An object keeps its members in two lists side by side, _keys and _items,
// in the order they were added -- which is what key(i) and the file give
// back -- and an INDEX beside them, kept by every change: a hash table from
// a key to its place (_slots), and the places of the members that hold
// nothing, in order (_absent). Without it, walking an object the way
// rmp/save.h says to -- size(), key(i), [key] -- went through the members
// before each one: 10 000 keys took 25 s under ASan. A member that holds
// nothing keeps its place, so a key cleared and written again is back where
// it was, and one erased and written again is last; _absent is what lets
// key(i) step over those in a binary search instead of a walk.
// ---------------------------------------------------------------------------

#include <rmp/save.h>

#include "internal.h"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// The fewest slots a table starts with, and how full it may get: at most
// half, so a probe always meets an empty slot, and meets one soon.
constexpr std::size_t MIN_SLOTS = 8;

std::size_t slots_for(std::size_t keys) {
    return keys == 0 ? 0 : std::max(MIN_SLOTS, std::bit_ceil(2 * keys));
}

std::size_t hash_of(std::string_view key) { return std::hash<std::string_view>{}(key); }

// Into the first empty slot from the key's own on: linear probing, which
// place() walks the same way.
void put(std::vector<std::size_t> &slots, std::string_view key, std::size_t at) {
    const std::size_t mask = slots.size() - 1;
    std::size_t i = hash_of(key) & mask;
    while (slots[i] != 0) i = (i + 1) & mask;
    slots[i] = at + 1;
}

} // namespace

// ---------------------------------------------------------------------------
// The index
// ---------------------------------------------------------------------------

std::size_t Value::place(std::string_view key) const {
    if (_slots.empty()) return _keys.size();
    const std::size_t mask = _slots.size() - 1;
    for (std::size_t i = hash_of(key) & mask;; i = (i + 1) & mask) {
        const std::size_t slot = _slots[i];
        if (slot == 0) return _keys.size();
        if (_keys[slot - 1] == key) return slot - 1;
    }
}

std::size_t Value::member(std::string_view key, bool holds) {
    const std::size_t found = place(key);
    if (found < _keys.size()) {
        const auto it = std::ranges::lower_bound(_absent, found);
        const bool listed = it != _absent.end() && *it == found;
        if (holds && listed) {
            _absent.erase(it);
        } else if (!holds && !listed) {
            _absent.insert(it, found);
        }
        return found;
    }
    const std::size_t at = _keys.size();
    _keys.emplace_back(key);
    _items.emplace_back();
    if (!holds) _absent.push_back(at); // the last place: the list stays in order
    if (_slots.size() < slots_for(_keys.size())) {
        rehash();
    } else {
        put(_slots, key, at);
    }
    return at;
}

void Value::rehash() {
    _slots.assign(slots_for(_keys.size()), 0);
    for (std::size_t at = 0; at < _keys.size(); ++at) put(_slots, _keys[at], at);
}

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
    // Clamped to float's range, as as_int is to int's: a double past FLT_MAX
    // -- 1e300 in an edited save, which reads fine -- cast to float is an
    // infinity, and a finite number in the file must not become a speed or a
    // position that is not. An infinity a game put in itself reads as itself.
    if (std::isinf(_number)) return static_cast<float>(_number);
    return static_cast<float>(
        std::clamp(_number, -static_cast<double>(FLT_MAX), static_cast<double>(FLT_MAX)));
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
    return static_cast<int>(_keys.size() - _absent.size());
}

bool Value::contains(std::string_view key) const {
    return (*this)[key].type() != Type::NONE;
}

std::string_view Value::key(int index) const {
    // "" and not {}: a view the framework returns always has a NUL after it,
    // and a default one has no characters at all, not even that.
    if (_type != Type::OBJECT || index < 0 || index >= size()) return "";
    // The index-th member that holds something comes after as many that
    // hold nothing as have, before them, at most `index` that do -- a count
    // that only grows along _absent, so it is found by halving.
    const auto wanted = static_cast<std::size_t>(index);
    std::size_t low = 0;
    std::size_t high = _absent.size();
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (_absent[middle] - middle <= wanted) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return _keys[wanted + low];
}

const Value &Value::operator[](std::string_view key) const {
    if (_type != Type::OBJECT) return none();
    const std::size_t at = place(key);
    return at < _keys.size() ? _items[at] : none();
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
    const std::size_t at = place(key);
    if (at == _keys.size()) return false;
    // A member holding nothing was not there, so erasing it is "false" --
    // and it goes all the same.
    const bool was_there = _items[at].type() != Type::NONE;
    _keys.erase(_keys.begin() + static_cast<std::ptrdiff_t>(at));
    _items.erase(_items.begin() + static_cast<std::ptrdiff_t>(at));
    // Every member after it moved down a place, in _absent and in the table,
    // which is made again: what moving them cost already.
    auto it = std::ranges::lower_bound(_absent, at);
    if (it != _absent.end() && *it == at) it = _absent.erase(it);
    for (; it != _absent.end(); ++it) --*it;
    rehash();
    return was_there;
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
            // A true equality, so a NaN equals a NaN: with IEEE's ==, a Value
            // holding one anywhere was not equal to itself, and no copy of it
            // compared equal either (Omar, 2026-10-10). 0 and -0 stay equal.
            return a.number() == b.number() ||
                (std::isnan(a.number()) && std::isnan(b.number()));
        case Type::STRING:
            return a.as_string() == b.as_string();
        case Type::LIST:
            return a.items() == b.items();
        case Type::OBJECT:
            // Keys in any order: two objects that say the same thing are equal
            // however they were built -- and a member holding nothing is not
            // there.
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.keys().size(); ++i) {
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
            const std::size_t place = at->place(step.key);
            if (place < at->keys().size()) next = &at->items()[place];
        } else if (!step.is_key && at->type() == Type::LIST && step.index >= 0 &&
                   std::cmp_less(step.index, at->items().size())) {
            next = &at->items()[static_cast<std::size_t>(step.index)];
        }
        if (next == nullptr) return nullptr;
        at = next;
    }
    return at;
}

Value *Value::Ref::materialise(const bool holds) {
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
                    std::string_view(step.key),
                    "SAVE: writing [\"%s\"] into %s: nothing was written. "
                    "Assign it first -- v[...] = rmp::Value::object() -- if "
                    "it is meant to change type",
                    step.key.c_str(), type_name(type));
                return nullptr;
            }
            const Value *next = nullptr;
            if (type == Type::OBJECT) {
                const std::size_t place = at->place(step.key);
                if (place < at->keys().size()) next = &at->items()[place];
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

    // A member on the way holds something from here on -- the list or the
    // object the next step makes of it, if it held nothing -- and the last
    // one what the caller is about to write: the object it is in writes that
    // down as it goes.
    Value *here = &_root;
    for (std::size_t s = 0; s < _path.size(); ++s) {
        const Step &step = _path[s];
        if (step.is_key) {
            if (here->type() == Type::NONE) *here = Value::object();
            const bool last = s + 1 == _path.size();
            const std::size_t found = here->member(step.key, !last || holds);
            here = &here->items()[found];
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
    if (Value *slot = materialise(value.type() != Type::NONE)) *slot = std::move(value);
    return *this;
}

Value::Ref &Value::Ref::operator=(const Ref &other) {
    if (this != &other) *this = Value(other.get());
    return *this;
}

Value::Ref &Value::Ref::operator=(Ref &&other) noexcept(false) {
    // The copy above: a Ref on the right names a value, temporary or not.
    const Ref &named = other;
    return *this = named;
}

void Value::Ref::push(Value value) {
    // Something, whatever it was: nothing becomes a list, and anything else
    // is left as it was, holding something.
    if (Value *slot = materialise(true)) slot->push(std::move(value));
}

bool Value::Ref::erase(std::string_view key) {
    Value *at = find();
    return at != nullptr && at->erase(key);
}

} // namespace rmp
