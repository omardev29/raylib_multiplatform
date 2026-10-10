// The grid. Every item goes in a cell(), which is what lets the grid count
// them and start a new row at the right moment.
#include "inventory.h"

#include <rmp/ui.h>

#include <string>

void inventory_grid() {
    rmp::ui::panel({ .box = { .grow_x = true, .grow_y = true } }, [&] {
        rmp::ui::text("Inventory");

        // columns = 0: fit as many 88-unit cells as the width allows, and at
        // least one. The grid measures the width it had last frame; the id
        // keeps it the same grid whatever else comes and goes on screen, where
        // an unnamed one is told apart by its order among the unnamed grids.
        rmp::ui::grid({ .columns = 0, .min_cell = 88, .id = "inv" }, [&] {
            for (int i = 0; i < ITEM_COUNT; ++i) {
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
