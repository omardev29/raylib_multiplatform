#pragma once
// tools/pointer_check.py's green fixture: what the API says instead of a
// pointer, the two exempt operator->, and a detail and a private pointer the
// ratchet lists.

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace rmp {

template <class T> class Ref {
public:
    T *operator->() const { return _target; }

private:
    T *_target = nullptr;
};

template <class T> class Handle {
public:
    T *operator->() const;
};

struct Thing {
    int hp = 0;
};

void take(Thing &thing);
std::optional<Thing> find_thing();
void named(std::string_view name);
std::string name_of(const Thing &thing);
void fill(std::span<Thing> out);
Ref<Thing> maybe();

namespace detail {
void listed(Thing *thing);
} // namespace detail

class Private {
    int *_listed = nullptr;
};

} // namespace rmp
