// ---------------------------------------------------------------------------
// examples/ui/05_inventory.cpp
//
// grid and scroll: an inventory beside a scrolling list, both of them behaving
// when the window changes Size — which is the only reason either exists.
//
// The two ideas worth taking away:
//
//   * grid(0, …) works out its own column count from the space it has, and
//     works it out again when that changes. A grid with a hard-coded 6 columns
//     is a grid that is wrong on a phone in portrait.
//   * scroll() clips and scrolls with the wheel or with a finger. Same gesture,
//     same code, no branch on platform.
//
// This file is REFERENCE ONLY (not compiled by the build). See README.md.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/ui.h>

#include <string>

struct Item {
    const char *name;
    int count;
    bool equipped;
};

static Texture2D icon;
static Item items[] = {
    { "Sword", 1, true }, { "Shield", 1, false }, { "Potion", 12, false },
    { "Rope", 3, false }, { "Torch", 8, false },  { "Map", 1, false },
    { "Key", 2, false },  { "Bread", 5, false },  { "Coin", 240, false },
    { "Gem", 4, false },  { "Bow", 1, false },    { "Arrow", 60, false },
};
static constexpr int kItemCount = sizeof(items) / sizeof(items[0]);
static int selected = 0;

static void on_ready() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE); // the whole point: resize it
    InitWindow(APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT, APP_WINDOW_TITLE);
    icon = rmp::assets::load_texture("rabbit.png");
}

// -----------------------------------------------------------------------
// The grid. Every item goes in a cell(), which is what lets the grid count
// them and start a new row at the right moment.
// -----------------------------------------------------------------------
static void inventory_grid() {
    rmp::ui::panel({ .box = { .grow_x = true, .grow_y = true } }, [&] {
        rmp::ui::text("Inventory");

        // columns = 0: fit as many 88-unit cells as the width allows. Give the
        // grid an id and it can measure itself; without one it falls back to
        // four, which is a reasonable guess and never the right answer.
        rmp::ui::grid({ .columns = 0, .min_cell = 88, .id = "inv" }, [&] {
            for (int i = 0; i < kItemCount; i++) {
                rmp::ui::cell([&] {
                    rmp::ui::panel(
                        { .box = { .padding = 6 },
                          .background = (i == selected)
                              ? rmp::ui::current_theme().surface_hover
                              : rmp::ui::current_theme().surface },
                        [&] {
                            rmp::ui::image(icon, { .width = 40, .height = 40 });
                            rmp::ui::text(items[i].name, { .size = 13 });
                            if (items[i].count > 1) {
                                rmp::ui::text(
                                    "x" + std::to_string(items[i].count),
                                    { .color = rmp::ui::ColorRole::MUTED, .size = 12 });
                            }
                        });
                });
            }
        });
    });
}

// -----------------------------------------------------------------------
// The list. scroll() clips its contents; the wheel and a dragging finger both
// move it, and neither needs a line of code here.
// -----------------------------------------------------------------------
static void item_list() {
    rmp::ui::panel({ .box = { .width = 240, .grow_y = true } }, [&] {
        rmp::ui::text("All items");

        rmp::ui::scroll({ .gap = 4, .id = "list" }, [&] {
            for (int i = 0; i < kItemCount; i++) {
                // Two buttons could share a label across a long list, so the
                // ids are made explicit. Identical labels in one frame are told
                // apart automatically; this is for when you want the identity
                // to survive the list being reordered.
                std::string id = "item" + std::to_string(i);
                if (rmp::ui::button(items[i].name, { .id = id.c_str() })) {
                    selected = i;
                }
            }
        });

        rmp::ui::text("scroll me", { .color = rmp::ui::ColorRole::MUTED, .size = 12 });
    });
}

static void on_frame(float delta) {
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
    UnloadTexture(icon);
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
