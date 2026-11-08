// ---------------------------------------------------------------------------
// examples/ui/11_localization/src/main.cpp
//
// A lantern stall at a night market, in English, Spanish, Japanese and
// German -- switch while it runs. Nothing in this file translates anything:
//
//   every literal          button("Buy a lantern") is its own key in
//                          resources/lang.tsv, one column a language
//   resources/lang/de.tsv  German, in a file of its own: both shapes, merged
//   {placeholders}         "Hello, {name}!" keeps its key whatever the name
//   plurals                "{count} lantern|{count} lanterns": each language
//                          its own forms (Japanese has one)
//   a context              "Open" the stall and "Open" the door: two rows
//   a font per language    Japanese draws in jp.ttf (Jigmo, cut to what it
//                          needs); the rest in Kenney's pixel font
//
// `rmp text scan` keeps the tables in step with this file; `rmp text check`
// fails a language with a hole in it. The art is Kenney's and Jigmo's (CC0):
// tools/make_story_assets.py. Run it: `rmp example 11_localization`.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/scene.h>
#include <rmp/text.h>
#include <rmp/ui.h>

#include <string>
#include <string_view>

namespace {

constexpr int PRICE = 3; // coins a lantern
constexpr int WORTH = 5; // coins a lantern sells for
constexpr float FRAME = 12; // panel.png's border
constexpr float PLATE = 10; // button.png's
const Color NIGHT = { 22, 24, 52, 255 };
const Color INK = { 62, 39, 35, 255 };

rmp::ui::Picture frame(std::string_view file, float border) {
    return rmp::ui::NineSlice{ .texture = rmp::assets::load_texture(file),
                               .left = border,
                               .top = border,
                               .right = border,
                               .bottom = border,
                               .scale = 2 };
}

// Japanese needs its own letters: the UI's font follows the language.
void font_for(std::string_view language) {
    rmp::ui::set_font(
        rmp::assets::load_font(language == "ja" ? "jp.ttf" : "font.ttf", 16));
}

} // namespace

class MarketScene : public rmp::Scene {
public:
    void _ready() override {
        background = NIGHT;
        rmp::ui::Theme look = rmp::ui::theme_light();
        look.text = INK;
        look.sounds = { .click = "click", .volume = 0.5f };
        look.styles.panel = { .skin =
                                  rmp::ui::Skin{ .normal = frame("panel.png", FRAME) },
                              .padding_x = 28,
                              .padding_y = 22,
                              .gap = 10 };
        look.styles.button = {
            .skin = rmp::ui::Skin{ .normal = frame("button.png", PLATE),
                                   .press = frame("button_down.png", PLATE) },
            .padding_x = 22,
            .padding_y = 10,
            .text = INK
        };
        rmp::ui::set_theme(look);
        _japanese = rmp::assets::load_font("jp.ttf", 16);
        _latin = rmp::assets::load_font("font.ttf", 16);
        font_for(rmp::lang::current());
    }

    void _draw() override {
        rmp::ui::begin();
        rmp::ui::panel([this] {
            rmp::ui::text("The Lantern Market",
                          { .size = rmp::ui::Size::LARGE, .wrap = false });
            rmp::ui::text(rmp::Text("Hello, {name}!", { { "name", "Ada" } }),
                          { .wrap = false });
            rmp::ui::text(rmp::Text("Open", { .context = "stall" }), { .wrap = false });
            rmp::ui::text(
                rmp::Text("{count} coin|{count} coins", { { "count", _coins } }),
                { .wrap = false });
            rmp::ui::text(
                rmp::Text("{count} lantern|{count} lanterns", { { "count", _lanterns } }),
                { .wrap = false });
            if (rmp::ui::button("Buy a lantern")) {
                if (_coins < PRICE) {
                    rmp::ui::notify("Not enough coins");
                } else {
                    _coins -= PRICE;
                    ++_lanterns;
                }
            }
            if (rmp::ui::button("Sell everything") && _lanterns > 0) {
                rmp::ui::notify(rmp::Text(
                    "Sold {count} for {coins}",
                    { { "count", _lanterns }, { "coins", _lanterns * WORTH } }));
                _coins += _lanterns * WORTH;
                _lanterns = 0;
            }
            rmp::ui::text("Language",
                          { .color = rmp::ui::ColorRole::MUTED, .wrap = false });
            // Each language's name in its own words, in a font that has its
            // letters, whatever the language is now.
            rmp::ui::row([this] {
                for (const std::string &code : rmp::lang::available()) {
                    rmp::ui::Button pick{ .text = rmp::Text(rmp::lang::name(code)),
                                          .id = "language_" + code };
                    pick.style.font = code == "ja" ? _japanese : _latin;
                    if (pick.show()) {
                        rmp::lang::set(code);
                        font_for(code);
                    }
                }
            });
        });
        rmp::ui::end();
    }

private:
    rmp::Font _japanese; // jp.ttf: the Japanese name, whatever the language
    rmp::Font _latin; // font.ttf: the others' names, in Japanese too
    int _coins = 10;
    int _lanterns = 0;
};

RMP_GAME(MarketScene);
