# This is a MODIFIED copy of Clay 0.14

Upstream: <https://github.com/nicbarker/clay>. Licence: zlib/libpng, reproduced
unaltered in `LICENSE.md`. Marked as altered here, as the licence's second
clause asks.

| File | Line | Change | Why |
|---|---|---|---|
| `clay.h` | 34-54 | the C++ version guard accepts `__cplusplus >= 201709L` as well as `202002L` | NetBSD 10.1's system GCC 10.5 reports `201709L` under `-std=c++20`, because C++20 was not final when it shipped, and the unpatched guard refuses to build there |
| `clay.h` | 2430, 2479 | `Clay__SizeContainersAlongAxis`, both while loops: each stops after a pass that moved no size and removed no child | from a size of hundreds of thousands of pixels a step is below what a float can take away, and the squeeze went round forever: an image 62500 design units tall at the scale of a 3300x2206 window froze the layout (found by `tests/property/ui_prop.cpp`; `tests/ui_layout_test.cpp` holds it) |

Commented `PATCHED FOR THIS TEMPLATE` at each site. Re-apply when bumping;
revisit the guard when NetBSD's base GCC moves past 11, and the loops when
upstream stops a pass that moves nothing (`tests/ui_layout_test.cpp`, "an
image too tall for any window lays out", says whether it does).
