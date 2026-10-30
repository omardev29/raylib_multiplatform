// ---------------------------------------------------------------------------
// rmp::ui's looks and the widgets you keep, held to the campaign's rules for
// anything a game can hand them.
//
// Every kept widget, with every field of its Style drawn from what a game can
// get wrong -- NaN, the infinities, sizes past what a pixel count holds,
// negative lengths -- and pictures whose parts and margins are any numbers at
// all, played for frames with the pointer going anywhere and the clock at any
// step. Then:
//
//   - nothing undefined happened (the build is sanitized);
//   - every box Clay lays out is finite, and every record a styled element
//     hands the renderer holds numbers: a scale, an alpha from 0 to 1, offsets,
//     a shadow that does not blur backwards, margins that are not negative;
//   - the Theme a style draws with is numbers a layout can use;
//   - the events make sense: a release never comes without a press, a click
//     never without a press or a key, and a widget's hovered() is what its
//     hover and unhover events said.
//
// And the pieces on their own: a Picture of any part of any texture, slices of
// any sheet looked up by any name, set_font() with any name.
//
// covers: rmp::SheetData::slice(std::string_view) const
// covers: rmp::SheetData::slice_count() const
// covers: rmp::ui::Button::focused() const
// covers: rmp::ui::Button::hovered() const
// covers: rmp::ui::Button::pressed() const
// covers: rmp::ui::Button::shake()
// covers: rmp::ui::Button::show()
// covers: rmp::ui::Checkbox::focused() const
// covers: rmp::ui::Checkbox::hovered() const
// covers: rmp::ui::Checkbox::shake()
// covers: rmp::ui::Checkbox::show()
// covers: rmp::ui::Dropdown::focused() const
// covers: rmp::ui::Dropdown::hovered() const
// covers: rmp::ui::Dropdown::shake()
// covers: rmp::ui::Dropdown::show()
// covers: rmp::ui::Image::hovered() const
// covers: rmp::ui::Image::pressed() const
// covers: rmp::ui::Image::shake()
// covers: rmp::ui::Image::show()
// covers: rmp::ui::Label::hovered() const
// covers: rmp::ui::Label::shake()
// covers: rmp::ui::Label::show()
// covers: rmp::ui::Panel::hovered() const
// covers: rmp::ui::Panel::pressed() const
// covers: rmp::ui::Panel::shake()
// covers: rmp::ui::Panel::show [template]
// covers: rmp::ui::Picture::Picture(const NineSlice &)
// covers: rmp::ui::Picture::Picture(const rmp::Sprite &)
// covers: rmp::ui::Picture::Picture(const rmp::Texture &)
// covers: rmp::ui::Picture::Picture(const rmp::Texture &, Rectangle)
// covers: rmp::ui::Picture::empty() const
// covers: rmp::ui::Picture::size() const
// covers: rmp::ui::Picture::sliced() const
// covers: rmp::ui::ProgressBar::hovered() const
// covers: rmp::ui::ProgressBar::shake()
// covers: rmp::ui::ProgressBar::show()
// covers: rmp::ui::Slider::focused() const
// covers: rmp::ui::Slider::hovered() const
// covers: rmp::ui::Slider::shake()
// covers: rmp::ui::Slider::show()
// covers: rmp::ui::TextInput::focused() const
// covers: rmp::ui::TextInput::hovered() const
// covers: rmp::ui::TextInput::shake()
// covers: rmp::ui::TextInput::show()
// covers: rmp::ui::font_name()
// covers: rmp::ui::image(const Picture &)
// covers: rmp::ui::image(const Picture &, const ImageOptions &)
// covers: rmp::ui::image(const rmp::Texture &)
// covers: rmp::ui::image(const rmp::Texture &, const ImageOptions &)
// covers: rmp::ui::image_button(const Picture &)
// covers: rmp::ui::image_button(const Picture &, const ImageButtonOptions &)
// covers: rmp::ui::nine_slice(const rmp::SpriteSheet &, std::string_view)
// covers: rmp::ui::on_event(rmp::Callback<const Event &>)
// covers: rmp::ui::set_font(const rmp::Font &)
// covers: rmp::ui::set_font(std::string_view)
// covers: rmp::ui::slice(const rmp::SpriteSheet &, std::string_view)
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"

#include "../../src/rmp/internal.h"
#include "../../src/rmp/ui/internal.h"

#include <rmp/assets.h>
#include <rmp/object.h>
#include <rmp/ui.h>

#include <doctest.h>
#include <rapidcheck.h>
#include <raylib.h>

#include <array>
#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace gen = rmp::test::gen;
namespace ui = rmp::ui;
namespace detail = rmp::ui::detail;

// NOLINTBEGIN(custom-rmp-raw-pointer): Clay's MeasureFn signature, not ours to choose
Clay_Dimensions measure_stub(Clay_StringSlice text, Clay_TextElementConfig *config,
                             void * /*user*/) {
    // NOLINTEND(custom-rmp-raw-pointer)
    const float size = config != nullptr ? static_cast<float>(config->fontSize) : 16.0f;
    return Clay_Dimensions{ static_cast<float>(text.length) * size * 0.5f, size };
}

struct {
    Clay_Vector2 position{ 5, 5 };
    bool down = false;
} pointer;

void pointer_stub(Clay_Vector2 &position, bool &down) {
    position = pointer.position;
    down = pointer.down;
}

void no_keys(detail::NavState &out) { out = detail::NavState{}; }

// A texture with an id and no GPU: zeroed before the last handle lets go, so
// raylib, which unloads only an id that is not 0, never touches GL.
class FakeTexture {
public:
    FakeTexture(int w, int h)
        : _handle(rmp::detail::adopt(
              rmp::detail::ResourceKind::TEXTURE,
              Texture2D{ 99, w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 })) {}
    FakeTexture(const FakeTexture &) = delete;
    FakeTexture &operator=(const FakeTexture &) = delete;
    ~FakeTexture() { const_cast<Texture2D &>(_handle.raw()).id = 0; }
    [[nodiscard]] const rmp::Texture &handle() const { return _handle; }

private:
    rmp::Texture _handle;
};

struct Headless {
    Headless() {
        ui::shutdown();
        rmp::detail::reset_reports_for_tests();
        detail::set_measure_provider(measure_stub);
        detail::set_pointer_provider(pointer_stub);
        detail::set_nav_provider(no_keys);
        detail::set_test_viewport(1280, 720);
        pointer = {};
    }
    Headless(const Headless &) = delete;
    Headless &operator=(const Headless &) = delete;
    ~Headless() {
        ui::on_event({});
        ui::set_theme(ui::theme_dark());
        ui::set_font("");
        detail::set_measure_provider(detail::measure_with_raylib);
        detail::set_pointer_provider(detail::pointer_from_raylib);
        detail::set_nav_provider(detail::nav_from_raylib);
        detail::set_test_viewport(0, 0);
        detail::set_test_frame_time(0);
        ui::shutdown();
        rmp::detail::reset_reports_for_tests();
    }
};

rc::Gen<Color> colour() {
    return rc::gen::map(rc::gen::arbitrary<std::array<unsigned char, 4>>(),
                        [](const auto &c) { return Color{ c[0], c[1], c[2], c[3] }; });
}

template <class T> rc::Gen<std::optional<T>> maybe(rc::Gen<T> g) {
    return rc::gen::oneOf(
        rc::gen::just(std::optional<T>{}),
        rc::gen::map(std::move(g), [](T v) { return std::optional<T>(v); }));
}

// A Style a game could write, and could get wrong.
ui::Style hostile_style(const rmp::Texture &art) {
    ui::Style s;
    s.background = *maybe(colour());
    s.background_hover = *maybe(colour());
    s.border = *maybe(colour());
    s.border_width = *maybe(gen::hostile_float());
    s.radius = *maybe(gen::hostile_float());
    s.padding_x = *maybe(gen::hostile_float());
    s.padding_y = *maybe(gen::hostile_float());
    s.gap = *maybe(gen::hostile_float());
    s.text = *maybe(colour());
    s.font_size = *maybe(gen::hostile_float());
    if (*rc::gen::arbitrary<bool>()) {
        s.shadow = ui::Shadow{ .offset = { *gen::hostile_float(), *gen::hostile_float() },
                               .blur = *gen::hostile_float(),
                               .spread = *gen::hostile_float(),
                               .color = *colour() };
    }
    if (*rc::gen::arbitrary<bool>()) {
        s.text_outline =
            ui::TextOutline{ .width = *gen::hostile_float(), .color = *colour() };
    }
    if (*rc::gen::arbitrary<bool>()) {
        s.text_shadow =
            ui::TextShadow{ .offset = { *gen::hostile_float(), *gen::hostile_float() },
                            .color = *colour() };
    }
    if (*rc::gen::arbitrary<bool>()) {
        s.motion = ui::Motion{ .hover_scale = *gen::hostile_float(),
                               .press_scale = *gen::hostile_float(),
                               .hover_lift = *gen::hostile_float(),
                               .speed = *gen::hostile_float(),
                               .appear = *gen::hostile_float(),
                               .appear_offset = *gen::hostile_float(),
                               .appear_scale = *gen::hostile_float(),
                               .shake = *gen::hostile_float(),
                               .shake_strength = *gen::hostile_float() };
    }
    if (*rc::gen::arbitrary<bool>()) {
        ui::Picture p(art, *gen::hostile_rect());
        p.left = *gen::hostile_float();
        p.right = *gen::hostile_float();
        p.slice_scale = *gen::hostile_float();
        s.skin = ui::Skin{ .normal = p };
    }
    return s;
}

bool finite_box(const Clay_BoundingBox &b) {
    return std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.width) &&
        std::isfinite(b.height);
}

// Every record the last pass handed the renderer is numbers it can draw.
void records_are_numbers() {
    const Clay_RenderCommandArray a = detail::last_pass_commands();
    for (int32_t i = 0; i < a.length; ++i) {
        const Clay_RenderCommand &c = a.internalArray[i];
        RC_ASSERT(finite_box(c.boundingBox));
        if (c.userData == nullptr) continue;
        if (c.commandType == CLAY_RENDER_COMMAND_TYPE_TEXT) {
            const auto *t = static_cast<const detail::TextRecord *>(c.userData);
            if (t->magic != detail::TEXT_MAGIC) continue;
            RC_ASSERT(std::isfinite(t->outline_px) && t->outline_px >= 0);
            RC_ASSERT(std::isfinite(t->shadow_px.x) && std::isfinite(t->shadow_px.y));
            continue;
        }
        const auto *r = static_cast<const detail::DrawRecord *>(c.userData);
        if (r->magic != detail::DRAW_MAGIC) continue;
        RC_ASSERT(std::isfinite(r->scale) && r->scale >= 0);
        RC_ASSERT(std::isfinite(r->alpha) && r->alpha >= 0 && r->alpha <= 1);
        RC_ASSERT(std::isfinite(r->offset.x) && std::isfinite(r->offset.y));
        RC_ASSERT(std::isfinite(r->shadow_blur) && r->shadow_blur >= 0);
        RC_ASSERT(std::isfinite(r->shadow_spread));
        RC_ASSERT(std::isfinite(r->shadow_offset.x) && std::isfinite(r->shadow_offset.y));
        RC_ASSERT(r->left >= 0 && r->top >= 0 && r->right >= 0 && r->bottom >= 0);
        RC_ASSERT(std::isfinite(r->slice_px) && r->slice_px >= 0);
        RC_ASSERT(std::isfinite(r->source.width) && std::isfinite(r->source.height));
    }
}

struct Counted {
    int press = 0;
    int release = 0;
    int click = 0;
    int hover = 0;
    int unhover = 0;
};

} // namespace

TEST_SUITE("property: ui looks") {
    TEST_CASE("any style a game writes is drawn with numbers") {
        rmp::test::check(
            "ui: every kept widget, any style, any pointer: records of numbers", [] {
                const Headless headless;
                const FakeTexture art(64, 32);
                Counted heard;
                ui::on_event([&heard](const ui::Event &e) {
                    switch (e.kind) {
                        case ui::EventKind::PRESS:
                            ++heard.press;
                            break;
                        case ui::EventKind::RELEASE:
                            ++heard.release;
                            break;
                        case ui::EventKind::CLICK:
                            ++heard.click;
                            break;
                        case ui::EventKind::HOVER:
                            ++heard.hover;
                            break;
                        case ui::EventKind::UNHOVER:
                            ++heard.unhover;
                            break;
                        default:
                            break;
                    }
                });
                ui::Theme t = ui::theme_dark();
                t.styles.button = hostile_style(art.handle());
                t.styles.panel = hostile_style(art.handle());
                ui::set_theme(t);

                ui::Button button{ .text = *gen::hostile_string(),
                                   .icon = art.handle(),
                                   .style = hostile_style(art.handle()) };
                ui::Label label{ .text = *gen::hostile_string(),
                                 .style = hostile_style(art.handle()) };
                ui::Image image{ .picture =
                                     ui::Picture(art.handle(), *gen::hostile_rect()),
                                 .width = *gen::hostile_float(),
                                 .style = hostile_style(art.handle()) };
                ui::Panel panel{ .row = *rc::gen::arbitrary<bool>(),
                                 .style = hostile_style(art.handle()),
                                 .children = hostile_style(art.handle()) };
                ui::Checkbox box{ .text = "c", .style = hostile_style(art.handle()) };
                ui::Slider slider{ .text = "s",
                                   .value = *gen::hostile_float(),
                                   .style = hostile_style(art.handle()) };
                ui::Dropdown drop{ .text = "d",
                                   .items = { "a", "b" },
                                   .selected = *rc::gen::arbitrary<int>(),
                                   .style = hostile_style(art.handle()) };
                ui::TextInput field{ .text = "t", .style = hostile_style(art.handle()) };
                ui::ProgressBar bar{ .value = *gen::hostile_float(),
                                     .fill_picture =
                                         ui::Picture(art.handle(), *gen::hostile_rect()),
                                     .style = hostile_style(art.handle()) };
                const int frames = *rc::gen::inRange(1, 8);
                for (int f = 0; f < frames; ++f) {
                    pointer.position = Clay_Vector2{ *rc::gen::inRange(-50, 1400) * 1.0f,
                                                     *rc::gen::inRange(-50, 800) * 1.0f };
                    pointer.down = *rc::gen::arbitrary<bool>();
                    detail::set_test_frame_time(
                        *rc::gen::element(0.0f, 1.0f / 60.0f, 0.1f, 2.0f));
                    if (*rc::gen::arbitrary<bool>()) button.shake();
                    if (*rc::gen::arbitrary<bool>()) panel.shake();
                    if (*rc::gen::arbitrary<bool>()) label.shake();
                    if (*rc::gen::arbitrary<bool>()) image.shake();
                    if (*rc::gen::arbitrary<bool>()) box.shake();
                    if (*rc::gen::arbitrary<bool>()) slider.shake();
                    if (*rc::gen::arbitrary<bool>()) drop.shake();
                    if (*rc::gen::arbitrary<bool>()) field.shake();
                    if (*rc::gen::arbitrary<bool>()) bar.shake();
                    detail::begin_frame();
                    ui::begin();
                    panel.show([&] {
                        button.show();
                        label.show();
                        image.show();
                    });
                    box.show();
                    slider.show();
                    drop.show();
                    field.show();
                    bar.show();
                    ui::image_button(ui::Picture(art.handle(), *gen::hostile_rect()));
                    ui::end();
                    detail::end_frame();
                    records_are_numbers();
                    // The queries say what the events said.
                    (void)button.focused();
                    (void)button.pressed();
                    (void)image.pressed();
                    (void)panel.pressed();
                    (void)label.hovered();
                    (void)box.hovered();
                    (void)box.focused();
                    (void)slider.hovered();
                    (void)slider.focused();
                    (void)drop.hovered();
                    (void)drop.focused();
                    (void)field.hovered();
                    (void)field.focused();
                    (void)bar.hovered();
                    RC_ASSERT(std::isfinite(slider.value));
                    RC_ASSERT(drop.selected >= 0 && drop.selected < 2);
                }
                RC_ASSERT(heard.release <= heard.press);
                RC_ASSERT(heard.unhover <= heard.hover);
                RC_ASSERT(heard.hover - heard.unhover <= 12);
                RC_ASSERT(button.hovered() == (button.hovered() && heard.hover > 0));
            });
    }

    TEST_CASE("inside any style, the theme is numbers a layout can use") {
        rmp::test::check("ui: a style's theme has finite metrics from 0 to 4096", [] {
            const Headless headless;
            const FakeTexture art(8, 8);
            const ui::Style s = hostile_style(art.handle());
            const detail::LookScope scope(ui::WidgetKind::BUTTON, &s);
            const ui::Theme &t = ui::current_theme();
            for (const float v : { t.font_size, t.font_size_small, t.font_size_large,
                                   t.padding_x, t.padding_y, t.gap, t.panel_padding,
                                   t.corner_radius, t.border_width }) {
                RC_ASSERT(std::isfinite(v) && v >= 0 && v <= 4096);
            }
        });
    }

    TEST_CASE(
        "a Picture of any part of any texture has a size of numbers or is the whole") {
        rmp::test::check(
            "ui: Picture's size() and sliced() for any part and margins", [] {
                const FakeTexture art(*rc::gen::inRange(1, 4096),
                                      *rc::gen::inRange(1, 4096));
                const Rectangle part = *gen::hostile_rect();
                const ui::Picture p(art.handle(), part);
                RC_ASSERT(!p.empty());
                const Vector2 size = p.size();
                if (part.width > 0 && part.height > 0) {
                    RC_ASSERT(size.x == part.width);
                } else {
                    RC_ASSERT(size.x == static_cast<float>(art.handle().raw().width));
                }
                ui::NineSlice n{ .texture = art.handle(), .source = part };
                n.left = *gen::hostile_float();
                const ui::Picture nine(n);
                RC_ASSERT(nine.sliced() == (n.left > 0));
                rmp::Sprite sprite;
                sprite.texture = art.handle();
                sprite.source = part;
                const ui::Picture of_sprite(sprite);
                RC_ASSERT(of_sprite.texture.valid());
                RC_ASSERT(ui::Picture(rmp::Texture{}).empty());

                // Drawn, whatever the part: as image() and as an rmp::Texture.
                const Headless headless;
                detail::begin_frame();
                ui::begin();
                ui::image(p);
                ui::image(p, { .width = *gen::hostile_float() });
                ui::image(art.handle());
                ui::image(art.handle(), { .grow = *rc::gen::arbitrary<bool>() });
                ui::image_button(nine);
                ui::image_button(nine, { .height = *gen::hostile_float(), .id = "n" });
                ui::end();
                detail::end_frame();
                records_are_numbers();
            });
    }

    TEST_CASE("a slice is found by its name, or is nothing") {
        rmp::test::check("ui: slice() and nine_slice() over any sheet and any name", [] {
            const Headless headless;
            rmp::SheetData data;
            data.width = 16;
            data.height = 16;
            const int n = *rc::gen::inRange(0, 6);
            for (int i = 0; i < n; ++i) {
                const int x = *rc::gen::inRange(0, 12);
                const int w = *rc::gen::inRange(1, 16 - x);
                const bool nine = *rc::gen::arbitrary<bool>() && w >= 3;
                data.slices.push_back(rmp::SheetSlice{
                    *rc::gen::element<std::string>("a", "b", "coin", ""),
                    Rectangle{ static_cast<float>(x), 0, static_cast<float>(w), 8 }, nine,
                    nine ? Rectangle{ 1, 1, static_cast<float>(w - 2), 6 }
                         : Rectangle{} });
            }
            const rmp::SpriteSheet sheet{ rmp::detail::adopt(
                rmp::detail::ResourceKind::SHEET, std::move(data)) };
            RC_ASSERT(sheet.raw().slice_count() == n);
            const std::string name = *rc::gen::oneOf(
                rc::gen::element<std::string>("a", "b", "coin"), gen::hostile_string());
            const rmp::SheetSlice &found = sheet.raw().slice(name);
            const ui::Picture p = ui::slice(sheet, name);
            const ui::NineSlice ns = ui::nine_slice(sheet, name);
            if (found.bounds.width > 0) {
                RC_ASSERT(found.name == name);
                RC_ASSERT(p.source.x == found.bounds.x);
                RC_ASSERT(p.left >= 0 && p.right >= 0 && p.top >= 0 && p.bottom >= 0);
                RC_ASSERT(p.left + p.right <= found.bounds.width);
                RC_ASSERT(ns.source.width == found.bounds.width);
                RC_ASSERT(p.sliced() == found.nine);
            } else {
                RC_ASSERT(!p.sheet.valid());
                RC_ASSERT(!ns.sheet.valid());
            }
        });
    }

    TEST_CASE("set_font() takes any name, and font_name() says it back") {
        rmp::test::check(
            "ui: set_font() with any name; font_name() is it up to a NUL", [] {
                const Headless headless;
                const std::string name = *gen::hostile_string();
                ui::set_font(name);
                const std::string_view back = ui::font_name();
                RC_ASSERT(back == std::string_view(name.c_str()));
                const rmp::Font loaded{ rmp::detail::adopt_named(
                    rmp::detail::ResourceKind::FONT, "loaded.ttf", 12, ::Font{}) };
                ui::set_font(loaded);
                RC_ASSERT(ui::font_name() == "loaded.ttf");
                ui::set_font(rmp::Font{});
                RC_ASSERT(ui::font_name().empty());
            });
    }
}
