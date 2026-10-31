#pragma once
#include <rmp/assets.h>
#include <rmp/audio.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

// The shop: coins to spend on stars, a settings panel behind the gear.
class ShopScene : public rmp::Scene {
public:
    void _ready() override;
    void _draw() override;

private:
    void buy_star();
    void sell_star();
    void show_shop();
    void show_settings();

    rmp::SpriteSheet _skin; // skin.aseprite: the frames, the panel, the icons
    int _coins = 5;
    int _stars = 0;
    int _clicks = 0; // every click of every control, counted by on_event()
    bool _settings = false;

    // A title in a font of its own, with an outline, so it reads on anything.
    rmp::ui::Label _title{ .text = "The Star Shop", .size = rmp::ui::Size::LARGE };
    // The star, turning: an animated sprite a kept Image plays by itself.
    rmp::ui::Image _spinner{ .width = 48, .height = 48, .id = "spinner" };
    // The shop: a panel whose own look comes from the theme, and whose
    // children all write in gold.
    rmp::ui::Panel _shop{ .id = "shop", .children = { .text = GOLD } };
    rmp::ui::Button _buy{ .text = "Buy a star", .id = "buy", .on_click = [this] {
                             buy_star();
                         } };
    rmp::ui::Button _sell{ .text = "Sell a star", .id = "sell", .on_click = [this] {
                              sell_star();
                          } };
    rmp::ui::ProgressBar _collection{ .width = 300, .height = 18, .id = "collection" };
    // The settings, which keep their own values.
    rmp::ui::Checkbox _sound{ .text = "Sound", .checked = true, .on_change = [](bool on) {
                                 rmp::audio::set_volume(rmp::audio::Bus::SFX,
                                                        on ? 1.0f : 0.0f);
                             } };
    rmp::ui::Slider _volume{ .text = "Volume",
                             .value = 1.0f,
                             .step = 0.1f,
                             .width = 160,
                             .on_change = [](float v) {
                                 rmp::audio::set_volume(rmp::audio::Bus::MASTER, v);
                             } };
    rmp::ui::Button _close{ .text = "Close", .on_click = [this] { _settings = false; } };
};
