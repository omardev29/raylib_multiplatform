// The list. scroll() clips its contents; the wheel and a dragging finger both
// move it, and neither needs a line of code here. Each item says what it is
// with tooltip().
#include "inventory.h"

#include <rmp/ui.h>

#include <string>

void item_list() {
    rmp::ui::panel({ .box = { .width = 240, .grow_y = true } }, [&] {
        rmp::ui::text("All items");

        rmp::ui::scroll({ .gap = 4, .id = "list" }, [&] {
            for (int i = 0; i < ITEM_COUNT; ++i) {
                // Two buttons could share a label across a long list, so the
                // ids are made explicit. Identical labels in one frame are told
                // apart automatically; this is for when you want the identity
                // to survive the list being reordered.
                const std::string id = "item" + std::to_string(i);
                if (rmp::ui::button(items[i].name, { .id = id })) {
                    selected = i;
                }
                // About the button just above, and only that: shown when the
                // pointer rests on it for half a second, or at once when the
                // keyboard or a gamepad walks onto it. To the left, over the
                // grid, where it hides no other item of the list.
                rmp::ui::tooltip(items[i].description,
                                 { .side = rmp::ui::Align::CENTER_LEFT });
            }
        });

        rmp::ui::text("scroll me", { .color = rmp::ui::ColorRole::MUTED, .size = 12 });
    });
}
