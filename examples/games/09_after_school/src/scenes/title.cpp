// The title: the town behind the menu, the camera drifting over it, and the
// language you read it in.

#include "school.h"

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/text.h>
#include <rmp/ui.h>

#include <cmath>

namespace game {

namespace {
constexpr float DRIFT = 0.1f; // how fast the camera wanders
const Color TITLE = { 255, 214, 140, 255 };
const Color OUTLINE = { 62, 39, 35, 255 };
} // namespace

void TitleScene::_ready() {
    rmp::app::set_stretch(rmp::app::Stretch::EXPAND);
    set_look();
    background = Color{ 109, 190, 106, 255 };
    map = rmp::assets::load_map("world.ldtk", "Town");
    camera.limits = map.bounds();
    camera.zoom = ZOOM;
}

void TitleScene::_update(float delta) {
    _clock += delta;
    const Rectangle b = map.bounds();
    camera.position = { b.x + (b.width / 2) + (std::sin(_clock * DRIFT) * b.width / 4),
                        b.y + (b.height / 2) + (std::cos(_clock * DRIFT) * b.height / 5) };
}

void TitleScene::_draw() {
    rmp::ui::begin();
    rmp::ui::panel([] {
        rmp::ui::Label title{ .text = "After School",
                              .size = 46,
                              .wrap = false,
                              .style = { .text = TITLE,
                                         .text_outline =
                                             rmp::ui::TextOutline{ .width = 3, .color = OUTLINE } } };
        title.show();
        rmp::ui::text("The lantern festival starts at seven.",
                      { .color = rmp::ui::ColorRole::MUTED, .wrap = false });
        if (has_saved_story() && rmp::ui::button("Continue")) {
            load_story();
            rmp::Scene::change<TownScene>();
        }
        if (rmp::ui::button("New afternoon")) {
            new_story();
            rmp::Scene::change<TownScene>();
        }
        // The languages lang.tsv has, each in its own words.
        rmp::ui::row([] {
            for (const std::string &code : rmp::lang::available()) {
                if (rmp::ui::button(rmp::Text(rmp::lang::name(code)))) rmp::lang::set(code);
            }
        });
#if !defined(PLATFORM_IOS)
        if (rmp::ui::button("Quit")) rmp::app::quit();
#endif
    });
    rmp::ui::end();
}

} // namespace game
