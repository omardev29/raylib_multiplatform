// ---------------------------------------------------------------------------
// examples/ui/02_layout.cpp
//
// Containers: row, column, panel, center, stack, spacer — plus image and
// progress, the two widgets that only make sense once you have somewhere to
// put them.
//
// The rule that shapes all of it: a container takes its contents as a lambda.
// There is no closing call to forget, because the compiler closes it for you.
// Capture with [&] when the body needs your variables, which is most of the
// time.
//
// Every measurement below is in DESIGN UNITS, at the [window] resolution from
// raylib_multiplatform.toml. rmp::ui multiplies them by the current scale, so
// `width = 200` is 200 at your design Size, 400 at twice it, and never a
// hard-coded pixel count that looks right on exactly one monitor.
//
// This file is REFERENCE ONLY (not compiled by the build). See README.md.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/ui.h>

#include <string>

static Texture2D portrait;
static float health = 0.72f;
static float mana = 0.35f;

static void on_ready() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE); // resize it and watch everything follow
    InitWindow(APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT, APP_WINDOW_TITLE);
    portrait = rmp::assets::load_texture("rabbit.png");
}

// -----------------------------------------------------------------------
// row and column: the two directions
// -----------------------------------------------------------------------
static void character_card() {
    // panel = a column with a background and padding. It is what you put a
    // dialog, a card or a tooltip inside.
    rmp::ui::panel([&] {
        // row lays out left to right.
        rmp::ui::row({ .gap = 16 }, [&] {
            rmp::ui::image(portrait, { .width = 72, .height = 72 });

            // column lays out top to bottom. items decides where children sit
            // across the other axis — here, hard against the left instead of
            // centred, which is what makes a card look like a card.
            rmp::ui::column({ .gap = 6, .items = rmp::ui::Align::CENTER_LEFT }, [&] {
                rmp::ui::text("Rabbit", { .color = rmp::ui::ColorRole::PRIMARY });
                rmp::ui::progress(health, { .width = 180 });
                rmp::ui::progress(mana, { .width = 180, .height = 8, .fill = SKYBLUE });
                rmp::ui::text("Level 7",
                              { .color = rmp::ui::ColorRole::MUTED, .size = 14 });
            });
        });
    });
}

// -----------------------------------------------------------------------
// spacer: how you push things apart
// -----------------------------------------------------------------------
static void toolbar() {
    // grow_x makes the row take the full width it is offered; without it the
    // row would shrink to fit its contents and the spacer would have nothing
    // to eat.
    rmp::ui::row({ .grow_x = true }, [&] {
        rmp::ui::text("Inventory");
        rmp::ui::spacer(); // <- takes everything left over
        rmp::ui::text("24 / 40", { .color = rmp::ui::ColorRole::MUTED });
    });

    // spacer(n) is the other one: a fixed gap, when you want breathing room in
    // one specific place rather than changing the container's gap.
    rmp::ui::spacer(8);
}

// -----------------------------------------------------------------------
// stack and layer: things on top of each other
// -----------------------------------------------------------------------
static void banner() {
    rmp::ui::panel({ .box = { .width = 260, .height = 90 } }, [&] {
        // Every child of a stack has to be a layer(). That is not ceremony:
        // it is what says "these are meant to overlap" instead of "these are
        // ordinary children in a column". Later layers draw on top.
        rmp::ui::stack([&] {
            rmp::ui::layer([&] {
                rmp::ui::image(
                    portrait,
                    { .grow = true, .tint = CLITERAL(Color){ 255, 255, 255, 60 } });
            });
            rmp::ui::layer([&] {
                rmp::ui::text("PAUSED", { .color = rmp::ui::ColorRole::DANGER });
            });
        });
    });
}

// -----------------------------------------------------------------------
// center: fill what you were given, put the contents in the middle
// -----------------------------------------------------------------------
static void empty_slot() {
    rmp::ui::panel({ .box = { .width = 260, .height = 70 },
                     .border = rmp::ui::current_theme().border },
                   [&] {
                       rmp::ui::center([&] {
                           rmp::ui::text("nothing here",
                                         { .color = rmp::ui::ColorRole::MUTED });
                       });
                   });
}

static void on_frame(float delta) {
    // Bars that move, so the layout is doing something rather than sitting
    // still — and so it is obvious the numbers are read fresh every frame.
    health -= delta * 0.05f;
    if (health < 0) health = 1.0f;
    mana += delta * 0.08f;
    if (mana > 1) mana = 0.0f;

    BeginDrawing();
    ClearBackground(rmp::ui::current_theme().background);

    // begin() is a centred column. placement moves the whole thing: this one
    // hugs the top-left corner, like a HUD would.
    rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT, .gap = 14 });

    toolbar();
    character_card();
    banner();
    empty_slot();

    if (rmp::ui::button("Done")) rmp::app::quit();

    rmp::ui::end();
    EndDrawing();
}

static void on_exit() {
    UnloadTexture(portrait);
    CloseWindow();
}

RMP_ENTRY_POINT(on_ready, on_frame, on_exit);

// ---------------------------------------------------------------------------
// The Sizing model, in three lines
// ---------------------------------------------------------------------------
//
//   default        fit — as big as the contents need
//   grow_x/grow_y  fill the space the parent is offering
//   width/height   a fixed number of design units, overriding both
//
// A container fits its contents; a child with grow expands to fill whatever
// the container ended up being. That pairing is what makes the buttons in a
// menu come out the same width without anyone measuring anything.
//
// NAMING A CONTAINER. Every container takes an optional `.id`. You need it
// only when you want to ask about that element later:
//
//     rmp::ui::panel({ .box = { .id = "inventory" } }, [&]{ ... });
//
// Unnamed containers are anonymous, which is right for almost all of them.
