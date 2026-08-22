// ---------------------------------------------------------------------------
// examples/ui/03_clay_direct.cpp
//
// THE ESCAPE HATCH: using Clay's own API directly, alongside rmp::ui.
//
// rmp::ui is a small, opinionated surface over Clay — enough for menus, HUDs
// and dialogs without ever mentioning a coordinate. When you want something it
// does not expose yet (floating elements, clipping, scroll containers, aspect
// ratios, per-corner radii, z-index), you do not have to wait for us and you do
// not have to give up rmp::ui to get it.
//
// It is the same bargain as the rest of the template: rmp::assets does not stop
// you calling LoadTexture, and rmp::ui does not stop you calling Clay. Or
// rlgl. Or raw OpenGL, for that matter.
//
//   #include <clay.h>
//
// and write Clay between rmp::ui::begin() and rmp::ui::end(). Your elements go
// into the same tree, get laid out in the same pass and drawn by the same
// renderer.
//
// Two things to know and then you are on your own:
//
//   * begin() has already opened two elements — a root that fills the screen
//     and a column that centres its contents. Yours become children of that
//     column. If you want to escape it, use Clay's floating elements.
//   * Clay does NOT copy strings. CLAY_STRING() on a literal is fine forever;
//     anything built at runtime must stay alive until end() has returned. This
//     is the one footgun rmp::ui::text() removes for you by copying.
//
// This file is REFERENCE ONLY (not compiled by the build). See README.md.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/ui.h>

#include <clay.h> // the escape hatch: not included for you on purpose

static Texture2D logo;

static void on_ready() {
    InitWindow(APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT, APP_WINDOW_TITLE);
    logo = rmp::assets::load_texture("rabbit.png");
}

static void on_frame(float delta) {
    BeginDrawing();
    ClearBackground(rmp::ui::current_theme().background);

    rmp::ui::begin();

    // --- the normal API ---------------------------------------------------
    rmp::ui::text("Inventory");

    // --- and Clay, in the same frame --------------------------------------
    //
    // A horizontal strip of slots, which rmp::ui has no primitive for yet.
    // CLAY_AUTO_ID is the version without an explicit id; CLAY(CLAY_ID("x"),
    // ...) is the one to use when you want to ask Clay about the element later
    // with Clay_PointerOver or Clay_GetElementData.
    CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_FIT(0),
                                           .height = CLAY_SIZING_FIT(0) },
                               .padding = CLAY_PADDING_ALL(10),
                               .childGap = 10,
                               .layoutDirection = CLAY_LEFT_TO_RIGHT },
                   .backgroundColor = { 30, 30, 38, 255 },
                   .cornerRadius = CLAY_CORNER_RADIUS(10) }) {
        for (int i = 0; i < 4; i++) {
            // CLAY_IDI gives each slot in a loop its own identity without
            // building a string per element.
            CLAY(CLAY_IDI("slot", i),
                 { .layout = { .sizing = { .width = CLAY_SIZING_FIXED(64),
                                           .height = CLAY_SIZING_FIXED(64) } },
                   .backgroundColor = { 44, 44, 56, 255 },
                   .cornerRadius = CLAY_CORNER_RADIUS(6) }) {
                // Images: point imageData at a Texture2D you own and keep
                // alive for the frame. Our renderer draws it stretched to the
                // element's box, untinted unless you set backgroundColor.
                if (i == 0) {
                    CLAY_AUTO_ID(
                        { .layout = { .sizing = { .width = CLAY_SIZING_GROW(0),
                                                  .height = CLAY_SIZING_GROW(0) } },
                          .image = { .imageData = &logo } }) {}
                }
            }
        }
    }

    // Hover, straight from Clay. Note it reads the PREVIOUS frame's geometry,
    // exactly like rmp::ui::button() does — the layout for this frame does not
    // exist until end().
    if (Clay_PointerOver(CLAY_IDI("slot", 0))) {
        rmp::ui::text("slot 1", { .color = rmp::ui::ColorRole::PRIMARY });
    }

    // Back to the normal API, still the same frame.
    if (rmp::ui::button("Close")) rmp::app::quit();

    rmp::ui::end();
    EndDrawing();
}

static void on_exit() {
    UnloadTexture(logo);
    CloseWindow();
}

RMP_ENTRY_POINT(on_ready, on_frame, on_exit);

// ---------------------------------------------------------------------------
// What our renderer understands
// ---------------------------------------------------------------------------
//
//   RECTANGLE      background colour + corner radius
//   BORDER         colour + width + corner radius
//   TEXT           drawn with the UI font; fontId is ignored, there is one font
//   IMAGE          imageData as a Texture2D*, backgroundColor as the tint
//   SCISSOR_START  clipping, so Clay's clip/scroll containers work
//   SCISSOR_END
//
// CUSTOM commands are not handled. If you need them, src/rmp/
// ui/render.cpp is ~150 readable lines and adding a case is the intended way to
// extend it.
//
// AND THE LIMITS. Doing this ties your code to a specific version of Clay
// (0.14, pinned in thirdparty/FROZEN_VERSIONS.md). rmp::ui exists partly so
// that most code does not carry that dependency — Clay is pre-1.0 and its API
// has moved between minor versions. That is a fair trade for a feature you need
// today; it is a bad trade for a button.
