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
#include <rmp/input.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

#include "inventory.h"

class InventoryScene : public rmp::Scene {
public:
    void _ready() override {
        // The whole point: resize the window.
        _icon = rmp::assets::load_texture("rabbit.png");
        // What a click means to the game, by name.
        rmp::input::action("use", MOUSE_BUTTON_LEFT);
    }

    void _update(float) override {
        // The UI is reading the pointer, so the game must not act on the same
        // click: picking an item must not also swing the sword. An action asked
        // for by name already knows -- it is quiet while the UI wants the
        // pointer -- so there is no `if (!rmp::ui::wants_pointer())` to
        // remember here.
        if (rmp::input::just_pressed("use")) {
            TraceLog(LOG_INFO, "GAME: click in the world");
        }
    }

    void _draw() override {
        // grow: the frame is the whole window, so the row below can fill it.
        // Without it begin() is a column that fits its content -- the right
        // thing for a menu -- and a growing row would fill that column and no
        // more.
        rmp::ui::begin({ .grow = true });
        rmp::ui::row({ .gap = 12, .grow_x = true, .grow_y = true }, [&] {
            inventory_grid(_icon);
            item_list();
        });
        rmp::ui::end();
    }

private:
    // An rmp::Texture and not a Texture2D: the handle owns the texture, and the
    // scene owns the handle, so it goes when the scene does -- before the
    // window closes. Copying it into a raw Texture2D would have destroyed the
    // only owner on the same line.
    rmp::Texture _icon;
};

RMP_GAME(InventoryScene);

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
