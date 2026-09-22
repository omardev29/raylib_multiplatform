# This is a MODIFIED copy of Clay 0.14

Upstream: <https://github.com/nicbarker/clay>. Licence: zlib/libpng, reproduced
unaltered in `LICENSE.md`. Marked as altered here, as the licence's second
clause asks.

| File | Line | Change | Why |
|---|---|---|---|
| `clay.h` | 34-54 | the C++ version guard accepts `__cplusplus >= 201709L` as well as `202002L` | NetBSD 10.1's system GCC 10.5 reports `201709L` under `-std=c++20`, because C++20 was not final when it shipped, and the unpatched guard refuses to build there |

Commented `PATCHED FOR THIS TEMPLATE` at the site. Re-apply when bumping;
revisit when NetBSD's base GCC moves past 11.
