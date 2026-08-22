// ---------------------------------------------------------------------------
// examples/ui/07_responsive.cpp
//
// Two mechanisms, and knowing which one to reach for is most of the skill.
//
//   scale()             makes everything BIGGER or smaller, together. It is
//                       automatic, derived from [window] and the real window,
//                       and you almost never touch it.
//   current_breakpoint() changes the SHAPE of the layout. It is manual, and it
//                       is for the one thing scale cannot do: no combination of
//                       grow, fit and fixed turns a row into a column.
//
// The rule of thumb: if you are reaching for a breakpoint to pick a size, stop
// — scale already did that for you, and hard-coding around it is how a layout
// ends up looking right on exactly one machine. Reach for it when the layout
// has to become a different layout.
//
// Resize the window and watch the sidebar move under the content when the
// window becomes taller than it is wide. That is a phone held upright.
//
// This file is REFERENCE ONLY (not compiled by the build). See README.md.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/ui.h>

#include <string>

static int selected = 0;
static bool showGrid = true;

static const char *SECTIONS[] = { "World", "Bestiary", "Journal", "Crafting", "Map" };
static constexpr int kSections = sizeof(SECTIONS) / sizeof(SECTIONS[0]);

static void on_ready() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE); // the whole point: resize it
    InitWindow(APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT, APP_WINDOW_TITLE);
}

static const char *breakpoint_name() {
    switch (rmp::ui::current_breakpoint()) {
        case rmp::ui::Breakpoint::COMPACT:
            return "compact";
        case rmp::ui::Breakpoint::MEDIUM:
            return "medium";
        case rmp::ui::Breakpoint::EXPANDED:
            return "expanded";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// The two halves. Written once, arranged twice — which is the entire trick.
// Neither of them knows anything about the window.
// ---------------------------------------------------------------------------

static void sidebar() {
    // On a wide window this is a fixed-width column down the left. On a narrow
    // one it is a full-width strip along the top, and the buttons run across
    // instead of down. Same buttons, same state, same handlers.
    const bool narrow = rmp::ui::compact();

    auto items = [] {
        for (int i = 0; i < kSections; i++) {
            const bool active = (i == selected);
            if (rmp::ui::button(SECTIONS[i],
                                { .style = active ? rmp::ui::Variant::PRIMARY
                                                  : rmp::ui::Variant::GHOST,
                                  .size = rmp::ui::Size::SMALL })) {
                selected = i;
            }
        }
    };

    if (narrow) {
        rmp::ui::panel({ .box = { .grow_x = true } }, [&] {
            // A row that wraps would be better still, and "a row that wraps" is
            // spelled grid({ .columns = 0 }) — see the grid below and
            // 05_inventory.cpp. Kept as a plain row here so the shape change is
            // the only thing this example is demonstrating.
            rmp::ui::row({ .grow_x = true }, items);
        });
    } else {
        rmp::ui::panel({ .box = { .width = 200, .grow_y = true } }, [&] {
            rmp::ui::text(
                "Sections",
                { .color = rmp::ui::ColorRole::MUTED, .size = rmp::ui::Size::SMALL });
            items();
        });
    }
}

static void content() {
    rmp::ui::panel({ .box = { .grow_x = true, .grow_y = true } }, [] {
        rmp::ui::text(SECTIONS[selected], { .size = rmp::ui::Size::LARGE });
        rmp::ui::checkbox("Show the grid", &showGrid);

        if (showGrid) {
            // Nothing responsive to write here: columns = 0 works out how many
            // fit in the width it is given and works it out again every frame.
            // This is the composition answer to "a row that wraps", and it is
            // why there is no wrap flag on row().
            rmp::ui::scroll([] {
                rmp::ui::grid({ .columns = 0, .min_cell = 110, .id = "tiles" }, [] {
                    for (int i = 0; i < 24; i++) {
                        rmp::ui::cell([&] {
                            rmp::ui::panel({ .box = { .padding = 10 } }, [&] {
                                rmp::ui::text("Item " + std::to_string(i + 1),
                                              { .size = rmp::ui::Size::SMALL });
                                rmp::ui::progress(static_cast<float>(i % 10) / 9.0f);
                            });
                        });
                    }
                });
            });
        }
    });
}

static void on_frame(float) {
    BeginDrawing();
    ClearBackground(rmp::ui::current_theme().background);

    rmp::ui::begin({ .placement = rmp::ui::Align::TOP_CENTER, .padding = 12 });

    // A status line, so the breakpoint is visible while you drag the window.
    // Nothing in a real game would draw this.
    rmp::ui::row({ .grow_x = true }, [] {
        rmp::ui::text(
            std::string("Breakpoint: ") + breakpoint_name(),
            { .color = rmp::ui::ColorRole::PRIMARY, .size = rmp::ui::Size::SMALL });
        rmp::ui::spacer();
        rmp::ui::text(
            std::to_string(GetScreenWidth()) + "x" + std::to_string(GetScreenHeight()) +
                "   scale " + std::to_string(rmp::ui::scale()).substr(0, 4),
            { .color = rmp::ui::ColorRole::MUTED, .size = rmp::ui::Size::SMALL });
    });

    // THE WHOLE EXAMPLE IS THESE SEVEN LINES. Two arrangements of the same two
    // functions, and the only thing that picks between them is the shape of the
    // window — not its size, which scale() has already handled.
    if (rmp::ui::compact()) {
        rmp::ui::column({ .grow_x = true, .grow_y = true }, [] {
            sidebar();
            content();
        });
    } else {
        rmp::ui::row({ .grow_x = true, .grow_y = true }, [] {
            sidebar();
            content();
        });
    }

    rmp::ui::end();
    EndDrawing();
}

static void on_exit() { CloseWindow(); }

RMP_ENTRY_POINT(on_ready, on_frame, on_exit);
