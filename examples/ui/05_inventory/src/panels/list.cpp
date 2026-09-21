// The list. scroll() clips its contents; the wheel and a dragging finger both
// move it, and neither needs a line of code here.
#include "inventory.h"

#include <rmp/ui.h>

#include <string>

void item_list() {
    rmp::ui::panel({ .box = { .width = 240, .grow_y = true } }, [&] {
        rmp::ui::text("All items");

        rmp::ui::scroll({ .gap = 4, .id = "list" }, [&] {
            for (int i = 0; i < kItemCount; i++) {
                // Two buttons could share a label across a long list, so the
                // ids are made explicit. Identical labels in one frame are told
                // apart automatically; this is for when you want the identity
                // to survive the list being reordered.
                std::string id = "item" + std::to_string(i);
                if (rmp::ui::button(g_items[i].name, { .id = id.c_str() })) {
                    g_selected = i;
                }
            }
        });

        rmp::ui::text("scroll me", { .color = rmp::ui::ColorRole::MUTED, .size = 12 });
    });
}
