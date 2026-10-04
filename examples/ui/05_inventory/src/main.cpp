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
// Run it: `rmp example 05_inventory`.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/ui.h>

#include "inventory.h"

static void on_ready() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE); // the whole point: resize it
    InitWindow(RMP_WINDOW_WIDTH, RMP_WINDOW_HEIGHT, RMP_WINDOW_TITLE);
    icon = rmp::assets::load_texture("rabbit.png");
}

static void on_frame(float) {
    // The UI is reading the pointer, so the game must not act on the same
    // click. Without this, picking an item also swings the sword.
    if (!rmp::ui::wants_pointer() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        TraceLog(LOG_INFO, "GAME: click in the world");
    }

    BeginDrawing();
    ClearBackground(rmp::ui::current_theme().background);

    // grow: the frame is the whole window, so the row below can fill it.
    // Without it begin() is a column that fits its content -- the right thing
    // for a menu -- and a growing row would fill that column and no more.
    rmp::ui::begin({ .grow = true });
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
    icon = {};
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
// SCROLL AND SIZE. A scroll area grows by default, so it takes the height its
// parent gives it and clips there: here, what the panel has left once its two
// lines of text are in. Its rows never push a parent past the window -- inside
// one that fits its contents, it is squeezed to the room that is left and
// clips there. Give it a height when you want a particular one.
//
// PERFORMANCE. Every item is rebuilt every frame, and that is fine: this is
// twelve, and the layout engine is measured in microseconds. A list long
// enough to matter would want to draw only the rows in view, and rmp::ui has
// no call yet that tells you where the area is or how far it has scrolled.
