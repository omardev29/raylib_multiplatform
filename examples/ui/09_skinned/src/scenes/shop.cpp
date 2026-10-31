#include "shop.h"

#include <rmp/audio.h>

#include <string>

namespace {

constexpr int STAR_PRICE = 3;
constexpr int STARS_TO_COLLECT = 5;

} // namespace

void ShopScene::_ready() {
    _skin = rmp::assets::load_sheet("skin.aseprite");

    // The look of the whole game, in one place: every button a wooden frame
    // that stretches by its middle, a darker one while it is pressed; every
    // panel the same frame, with a soft shadow, sliding in when it appears.
    // Sounds on hover and click, and a "no" for a shake.
    rmp::ui::Theme look = rmp::ui::theme_dark();
    look.sounds = { .hover = "hover", .click = "click", .shake = "nope" };
    look.motion = { .hover_scale = 1.04f, .press_scale = 0.96f };
    look.styles.button = {
        .skin = rmp::ui::Skin{ .normal = rmp::ui::slice(_skin, "button"),
                               .press = rmp::ui::slice(_skin, "button_down") },
        .padding_x = 18,
        .padding_y = 10,
        .text = RAYWHITE,
        .text_shadow = rmp::ui::TextShadow{},
    };
    look.styles.panel = {
        .skin = rmp::ui::Skin{ .normal = rmp::ui::slice(_skin, "panel") },
        .shadow = rmp::ui::Shadow{ .offset = { 0, 6 }, .blur = 10 },
        .padding_x = 22,
        .motion = rmp::ui::Motion{ .appear = 0.25f,
                                   .appear_offset = 16,
                                   .appear_scale = 0.96f },
    };
    rmp::ui::set_theme(look);

    _title.style = { .text = GOLD,
                     .font = rmp::assets::load_font("title.ttf", 40),
                     .text_outline = rmp::ui::TextOutline{ .width = 2, .color = BLACK } };
    _spinner.sprite.sheet = rmp::assets::load_sheet("spinner.aseprite");
    _spinner.sprite.play("spin");
    _buy.icon = rmp::ui::slice(_skin, "coin");
    _sell.icon = rmp::ui::slice(_skin, "star");
    _collection.fill_picture = rmp::ui::slice(_skin, "bar");

    // Every click of every control, the immediate ones too: one hook.
    rmp::ui::on_event([this](const rmp::ui::Event &e) {
        if (e.kind == rmp::ui::EventKind::CLICK) ++_clicks;
    });
}

void ShopScene::buy_star() {
    if (_coins < STAR_PRICE) {
        _buy.shake(); // and the theme plays its "nope"
        rmp::ui::notify("Not enough coins", { .color = rmp::ui::ColorRole::DANGER });
        return;
    }
    _coins -= STAR_PRICE;
    ++_stars;
}

void ShopScene::sell_star() {
    if (_stars == 0) {
        _sell.shake();
        return;
    }
    --_stars;
    _coins += STAR_PRICE - 1;
}

void ShopScene::show_shop() {
    _shop.show([&] {
        rmp::ui::row([&] {
            _spinner.show();
            rmp::ui::column([&] {
                rmp::ui::row([&] {
                    rmp::ui::image(rmp::ui::slice(_skin, "coin"),
                                   { .width = 24, .height = 24 });
                    rmp::ui::text("Coins: " + std::to_string(_coins));
                    rmp::ui::image(rmp::ui::slice(_skin, "star"),
                                   { .width = 24, .height = 24 });
                    rmp::ui::text("Stars: " + std::to_string(_stars));
                });
                rmp::ui::text("¿Cuánto cuesta? Tres monedas, señor.");
            });
        });
        rmp::ui::row([&] {
            _buy.show();
            _sell.show();
        });
        _collection.value = static_cast<float>(_stars) / STARS_TO_COLLECT;
        _collection.show();
    });
}

void ShopScene::show_settings() {
    rmp::ui::overlay({ .dim = 0.5f }, [&] {
        rmp::ui::panel([&] {
            rmp::ui::text("Settings");
            _sound.show();
            _volume.show();
            _close.show();
        });
    });
}

void ShopScene::_draw() {
    rmp::ui::begin();
    _title.show();
    show_shop();
    rmp::ui::row([&] {
        if (rmp::ui::image_button(rmp::ui::slice(_skin, "gear"),
                                  { .width = 32, .height = 32, .id = "settings" }))
            _settings = true;
        rmp::ui::text("Clicks: " + std::to_string(_clicks),
                      { .color = rmp::ui::ColorRole::MUTED });
    });
    rmp::ui::end();

    if (_settings) show_settings();
}
