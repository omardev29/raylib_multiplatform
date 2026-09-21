// The grid. Every item goes in a cell(), which is what lets the grid count
// them and start a new row at the right moment.
#include "inventory.h"

#include <rmp/ui.h>

#include <string>

void inventory_grid() {
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
                          .background = (i == g_selected)
                              ? rmp::ui::current_theme().surface_hover
                              : rmp::ui::current_theme().surface },
                        [&] {
                            rmp::ui::image(g_icon, { .width = 40, .height = 40 });
                            rmp::ui::text(g_items[i].name, { .size = 13 });
                            if (g_items[i].count > 1) {
                                rmp::ui::text(
                                    "x" + std::to_string(g_items[i].count),
                                    { .color = rmp::ui::ColorRole::MUTED, .size = 12 });
                            }
                        });
                });
            }
        });
    });
}
