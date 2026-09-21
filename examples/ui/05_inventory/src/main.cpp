// ---------------------------------------------------------------------------
// examples/ui/05_inventory/src/main.cpp
//
// grid and scroll: an inventory beside a scrolling list, both of them behaving
// when the window changes size -- which is the only reason either exists.
//
// The two ideas worth taking away:
//
//   * grid(0, ...) works out its own column count from the space it has, and
//     works it out again when that changes. A grid with a hard-coded 6 columns
//     is a grid that is wrong on a phone in portrait.
//   * scroll() clips and scrolls with the wheel or with a finger. Same gesture,
//     same code, no branch on platform.
//
// One panel per file under src/panels/; the item table is in include/.
// Built and booted by CI on every push, and by `just example 05_inventory` here.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/ui.h>

#include "inventory.h"

static void on_ready() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE); // the whole point: resize it
    InitWindow(APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT, APP_WINDOW_TITLE);
    g_icon = rmp::assets::load_texture("rabbit.png");
}

static void on_frame(float delta) {
    (void)delta;
    // The UI is reading the pointer, so the game must not act on the same
    // click. Without this, picking an item also swings the sword.
    if (!rmp::ui::wants_pointer() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        TraceLog(LOG_INFO, "GAME: click in the world");
    }

    BeginDrawing();
    ClearBackground(rmp::ui::current_theme().background);

    rmp::ui::begin({ .placement = rmp::ui::Align::CENTER });
    rmp::ui::row({ .gap = 12, .grow_x = true, .grow_y = true }, [&] {
        inventory_grid();
        item_list();
    });
    rmp::ui::end();

    EndDrawing();
}

static void on_exit() {
    // Let go of the texture while the window -- and its GL context -- is still
    // there. A handle held in a global would otherwise release after main().
    g_icon = {};
    CloseWindow();
}

RMP_ENTRY_POINT(on_ready, on_frame, on_exit);

// ---------------------------------------------------------------------------
// Notes
// ---------------------------------------------------------------------------
//
// WHY cell() EXISTS. The layout engine underneath wraps text, not elements, so
// a grid has to be built as real rows. cell() is what lets the grid count its
// items and start a row at the right moment. It is the same bargain as
// stack()/layer(): one extra call, in exchange for the layout knowing what you
// meant instead of guessing.
//
// SCROLL AND SIZE. A scroll area needs a height to clip against. It grows by
// default, so it takes whatever its parent gives it — which means a scroll
// inside a container that also fits its contents clips nothing, because the
// parent grew to fit everything. Give one of them a size.
//
// PERFORMANCE. Every item is rebuilt every frame, and that is fine: this is
// twelve, and the layout engine is measured in microseconds. When a list is
// long enough to matter, the fix is to draw only the visible range, and
// scroll() gives you the box to work that out from.
