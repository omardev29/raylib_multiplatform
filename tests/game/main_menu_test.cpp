// ---------------------------------------------------------------------------
// Your game's own tests. Every .cpp in tests/game/ is one, and holds
// TEST_CASEs and nothing else: doctest brings the main(). `rmp test unit`
// builds them with your src/ -- every file in it but main.cpp -- and runs them
// with no window, and CI runs them on every push. Delete tests/game/ and
// neither asks for them again.
//
//     rmp test unit                    build them and run them
//     ./build/game_test -tc="*menu*"   one case, by name
//
// With no window there is no GPU: a texture comes back empty and a frame
// draws nothing, while files still load and your code still runs. What a test
// drives is what the framework takes in through a seam -- time is the delta
// you hand a hook, the dice are rmp::random::seed(), the devices are
// rmp::input::detail::DeviceState -- and what it reads is what your code
// keeps. Drawing is the boot's to check: `rmp test smoke`.
// ---------------------------------------------------------------------------

#include <doctest.h>

#include <rmp/assets.h>

#include "scenes/main_menu.h"

TEST_CASE("the main menu finds every file it loads") {
    // A file renamed in resources/ and not in the code is a hole in the
    // picture rather than a crash, so a game that boots can still be missing
    // one. rmp::assets counts what it was asked for and what it could not
    // read, and a scene's _ready() is where it asks.
    const int asked = rmp::assets::requested_loads();
    const int failed = rmp::assets::failed_loads();

    MainMenuScene menu;
    menu._ready();

    CHECK(rmp::assets::requested_loads() > asked);
    CHECK(rmp::assets::failed_loads() == failed);
}
