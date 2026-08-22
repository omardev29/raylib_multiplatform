// ---------------------------------------------------------------------------
// What you include is what you use.
//
// There is no umbrella header. `#include <rmp/ui.h>` gives you the interface
// layer and NOTHING else: no entry point, no asset layer, no ads, no <raymath.h>
// and no <math.h> behind it. That is the point of the split, and it only stays
// true if something checks — so this file is that check as well as an example.
//
// In a real game this is what your files look like. The one that draws the HUD
// knows about the interface and nothing else, so it recompiles when you change
// the HUD and not when you change the asset pipeline:
//
//     src/hud.h      void draw_hud(int score, float health);
//     src/hud.cpp    #include <rmp/ui.h>   <- this file
//     src/main.cpp   #include <rmp/app.h>
//
// It has no main() on purpose. CI compiles every example with -fsyntax-only,
// which is exactly the right check here: what is being tested is whether the
// header is self-sufficient, not whether the program runs.
//
// The headers, and what each one is for:
//
//     rmp/app.h       the entry point, and rmp::app::quit()
//     rmp/ui.h        rmp::ui — menus, buttons, layout, Theme
//     rmp/assets.h    rmp::assets — loading from resources/
//     rmp/ads.h       rmp::ads — interstitial and rewarded
//     rmp/math.h      vectors, rectangles, colours, and the arithmetic
//     rmp/config.h    APP_WINDOW_TITLE and the rest of the .toml
//
// If you get "'rmp::input' has not been declared", you are missing the header
// that declares it. The compiler is telling you the truth, which is more than
// an umbrella header ever does.
// ---------------------------------------------------------------------------

#include <rmp/ui.h>

#include <string>

void draw_hud(int score, float health) {
    // A HUD pinned to the top of the screen, and every measurement in it comes
    // from the theme, so it scales with everything else.
    rmp::ui::begin({ .placement = rmp::ui::Align::TOP_CENTER });

    rmp::ui::panel([&] {
        rmp::ui::row({ .gap = 24, .grow_x = true }, [&] {
            rmp::ui::text("SCORE");
            // Safe with a temporary: the string is copied into the frame arena
            // on the way in, because Clay keeps the pointer rather than the text.
            rmp::ui::text(std::to_string(score));
            rmp::ui::spacer();
            rmp::ui::progress(health, { .width = 160 });
        });
    });

    rmp::ui::end();
}
