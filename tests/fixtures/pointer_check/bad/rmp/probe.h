#pragma once
// tools/pointer_check.py's red fixture: one of every shape the gate refuses.
// tests/pointer_check_test.py holds the gate to finding exactly these, and
// nothing else -- the exempt and private ones included.

namespace rmp {

struct Thing {
    int hp = 0;
};

void take(Thing *thing); // a pointer parameter
Thing *find_thing(); // a pointer return
void named(const char *name); // a C string parameter
struct Holder {
    Thing *held = nullptr; // a pointer field
};
struct Tag {
    char name[8] = {}; // a C string with its length welded on
};
using ThingPtr = Thing *; // an alias that is one
ThingPtr hidden(); // and a function that hides one behind it
void call(void (*fn)(int)); // a function pointer
inline Thing *global_thing = nullptr; // a variable at namespace scope

// A template's public member, and its private one, which the ratchet does
// not list.
template <class T> class Box {
public:
    T *get() const { return _held; }

private:
    T *_held = nullptr;
};

// A friend is anybody's to call.
struct Friendly {
    friend void touch(Friendly *friendly) { (void)friendly; }
};

// operator-> is exempt only for rmp::Handle and rmp::Ref.
struct Arrow {
    Thing *operator->() const;
};

// Defined outside its class: judged once, where it is declared.
struct Outside {
    Thing *later() const;
};
inline Thing *Outside::later() const { return nullptr; }

namespace detail {
void unlisted(Thing *thing); // detail, and not in the ratchet
} // namespace detail

class Private {
    int *_secret = nullptr; // private, and not in the ratchet
};

} // namespace rmp
