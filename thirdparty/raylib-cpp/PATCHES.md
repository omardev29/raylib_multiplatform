# This is a PARTIAL (MODIFIED) copy of raylib-cpp 6.0.3

Upstream: <https://github.com/RobLoach/raylib-cpp>. Licence: zlib/libpng,
reproduced unaltered in `LICENSE`. No file's content was changed; the
modification is what was left out, and the zlib licence's second clause is
read conservatively enough to mark that too.

| File | Line | Change | Why |
|---|---|---|---|
| `Vector2.hpp`, `Vector3.hpp`, `Vector4.hpp`, `Matrix.hpp`, `Rectangle.hpp`, `Color.hpp` | -- | vendored verbatim | the math subset that backs `rmp/math.h` |
| `raylib.hpp`, `raymath.hpp`, `raylib-cpp-utils.hpp` | -- | vendored verbatim | the three headers the subset needs |
| every resource wrapper (`Texture`, `Font`, `Sound`, `Model`, ...) | -- | left out | they throw `RaylibException` on a failed load and this framework does not throw |
| `Quaternion.hpp` | -- | left out | it redeclares `raylib::Vector4` and collides with `Vector4.hpp` |
