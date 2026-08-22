// ---------------------------------------------------------------------------
// examples/ui/06_style.cpp
//
// Phase 4: the two themes, the five variants, the three sizes, and the
// transition that ties them together.
//
// The idea worth taking away is that none of this is a colour parameter. You
// say what a button MEANS — primary, danger, ghost — and how important it is —
// small, medium, large — and the theme decides what that looks like. Which is
// why the light/dark switch at the top of this file is one line and does not
// need a single call site revisited:
//
//     rmp::ui::set_theme(light ? rmp::ui::theme_light() : rmp::ui::theme_dark());
//
// If you had passed colours to widgets instead, that line would be a rewrite.
//
// This file is REFERENCE ONLY (not compiled by the build). See README.md.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/ui.h>

#include <string>

static bool light = false;
static bool reduce = false; // the accessibility switch, see apply_style()
static float excitement = 0.35f;
static int pressedCount = 0;

// One place decides what the interface looks like, and it is not spread across
// the widgets. Everything else in this file just says what things mean.
static void apply_style() {
    rmp::ui::Theme t = light ? rmp::ui::theme_light() : rmp::ui::theme_dark();

    // "Reduce motion" is a theme field, not a global switch, because the
    // transition is a property of the look and not of the machinery. 0 means
    // every control snaps to its new colour on the frame it changes.
    if (reduce) t.transition = 0.0f;

    rmp::ui::set_theme(t);
}

static void on_ready() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT, APP_WINDOW_TITLE);
    apply_style();
}

// ---------------------------------------------------------------------------
// The five variants. Read the call sites, not the colours: nothing here names
// a colour, and that is the point of the whole enum.
// ---------------------------------------------------------------------------
static void variants() {
    rmp::ui::panel({ .box = { .grow_x = true } }, [] {
        rmp::ui::text("Variants", { .size = rmp::ui::Size::LARGE });
        rmp::ui::text(
            "What the button means, not what colour it is.",
            { .color = rmp::ui::ColorRole::MUTED, .size = rmp::ui::Size::SMALL });

        rmp::ui::row({ .grow_x = true }, [] {
            if (rmp::ui::button("Start game", { .style = rmp::ui::Variant::PRIMARY }))
                pressedCount++;
            if (rmp::ui::button("Load")) pressedCount++;
            if (rmp::ui::button("Settings", { .style = rmp::ui::Variant::OUTLINE }))
                pressedCount++;
            if (rmp::ui::button("Back", { .style = rmp::ui::Variant::GHOST }))
                pressedCount++;
            if (rmp::ui::button("Delete", { .style = rmp::ui::Variant::DANGER }))
                pressedCount++;
        });

        // Disabled is a state, not a variant: it can happen to any of them, so
        // it is a flag rather than a sixth entry in the enum.
        rmp::ui::row({ .grow_x = true }, [] {
            rmp::ui::button("Continue",
                            { .style = rmp::ui::Variant::PRIMARY, .enabled = false });
            rmp::ui::button("No save file yet", { .enabled = false });
        });
    });
}

// ---------------------------------------------------------------------------
// The three sizes. They are steps in the theme's type scale, so they move
// together when someone changes the scale and stay in proportion at every
// window Size — which a hand-picked number would not.
// ---------------------------------------------------------------------------
static void sizes() {
    rmp::ui::panel({ .box = { .grow_x = true } }, [] {
        rmp::ui::text("Sizes", { .size = rmp::ui::Size::LARGE });

        rmp::ui::row({ .grow_x = true }, [] {
            rmp::ui::button("Small", { .size = rmp::ui::Size::SMALL });
            rmp::ui::button("Medium", { .size = rmp::ui::Size::MEDIUM });
            rmp::ui::button("Large", { .size = rmp::ui::Size::LARGE });
        });

        // The padding and the minimum touch height follow the type Size, so a
        // large button is large all over rather than a normal one with bigger
        // letters in it. On a touch screen the small one still never drops
        // below the 44-unit touch target: there, a small button is still a
        // button you hit with a thumb.
        rmp::ui::text(
            "A Size step moves the type, the padding and the minimum height together.",
            { .color = rmp::ui::ColorRole::MUTED, .size = rmp::ui::Size::SMALL });

        // The same field takes an exact number when you genuinely need one.
        // Reach for it for a title, not for a button.
        rmp::ui::text("CHAPTER ONE", { .size = 44 });
    });
}

// ---------------------------------------------------------------------------
// Copy-modify-set. A Theme is data, so customising it is not an API — it is
// assignment. There is no cascade, no selector and no stylesheet to learn.
// ---------------------------------------------------------------------------
static void custom_theme_demo() {
    rmp::ui::panel({ .box = { .grow_x = true } }, [] {
        rmp::ui::text("Your own Theme", { .size = rmp::ui::Size::LARGE });
        rmp::ui::text(
            "Start from one of ours, change what you want, set it back.",
            { .color = rmp::ui::ColorRole::MUTED, .size = rmp::ui::Size::SMALL });

        rmp::ui::row({ .grow_x = true }, [] {
            if (rmp::ui::button("Gold accent")) {
                rmp::ui::Theme t = rmp::ui::current_theme();
                t.primary = GOLD;
                t.primary_hover = YELLOW;
                t.primary_press = ORANGE;
                rmp::ui::set_theme(t);
            }
            if (rmp::ui::button("Sharp corners")) {
                rmp::ui::Theme t = rmp::ui::current_theme();
                t.corner_radius = 0;
                rmp::ui::set_theme(t);
            }
            if (rmp::ui::button("Bigger everything")) {
                rmp::ui::Theme t = rmp::ui::current_theme();
                t.font_size *= 1.15f;
                t.font_size_small *= 1.15f;
                t.font_size_large *= 1.15f;
                rmp::ui::set_theme(t);
            }
            if (rmp::ui::button("Reset", { .style = rmp::ui::Variant::GHOST }))
                apply_style();
        });
    });
}

static void on_frame(float) {
    BeginDrawing();
    ClearBackground(rmp::ui::current_theme().background);

    rmp::ui::begin({ .placement = rmp::ui::Align::TOP_CENTER });

    rmp::ui::panel({ .box = { .grow_x = true } }, [] {
        rmp::ui::row({ .grow_x = true }, [] {
            rmp::ui::text("Style", { .size = rmp::ui::Size::LARGE });
            rmp::ui::spacer();
            // Both of these rebuild the theme and hand it back. Nothing else in
            // the file knows or cares which one is active.
            if (rmp::ui::checkbox("Light Theme", &light)) apply_style();
            if (rmp::ui::checkbox("Reduce motion", &reduce)) apply_style();
        });
    });

    variants();
    sizes();
    custom_theme_demo();

    // The transition is on colour only — never on position or size. A control
    // is always exactly where it was drawn, so an animation can never make you
    // miss what you were aiming at. Drag this and watch the fill keep up.
    rmp::ui::panel({ .box = { .grow_x = true } }, [] {
        rmp::ui::slider("Excitement", &excitement, 0.0f, 1.0f);
        rmp::ui::progress(excitement);
        rmp::ui::text(
            "Buttons pressed: " + std::to_string(pressedCount),
            { .color = rmp::ui::ColorRole::MUTED, .size = rmp::ui::Size::SMALL });
    });

    rmp::ui::end();
    EndDrawing();
}

static void on_exit() { CloseWindow(); }

RMP_ENTRY_POINT(on_ready, on_frame, on_exit);
