// ---------------------------------------------------------------------------
// rmp::ui's looks, headless: styles and how they resolve, the records the
// renderer draws effects from, pictures and slices, fonts by id and letters
// baked on demand, every event a control sends, and the widgets you keep.
//
// What these cannot see -- a nine-slice's corners on screen, a shadow's
// pixels, an outline around a letter, the accented letters of a .ttf -- is
// tests/ui_render_test.cpp's, on a real GPU. What they can see is everything
// that decides what is drawn: the Theme a widget is drawn with, the render
// commands Clay hands the renderer and what rides in their userData, and the
// order the events and callbacks arrive in.
//
// And the promise the framework's own themes make: with them, nothing here
// adds a single record, so every game and every golden draws what it drew.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "../src/rmp/internal.h"
#include "../src/rmp/ui/internal.h"
#include "../src/rmp/ui_internal.h"

#include <raylib.h>
#include <rmp/assets.h>
#include <rmp/object.h>
#include <rmp/ui.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ui = rmp::ui;
namespace detail = rmp::ui::detail;

namespace {

// Every kept widget is a thing that lives where it is: its callbacks cannot
// be copied, so neither can it.
static_assert(!std::is_copy_constructible_v<ui::Button>);
static_assert(!std::is_copy_constructible_v<ui::Panel>);
static_assert(!std::is_copy_constructible_v<ui::Slider>);
static_assert(std::is_aggregate_v<ui::Button> && std::is_aggregate_v<ui::Label> &&
              std::is_aggregate_v<ui::Image> && std::is_aggregate_v<ui::Panel> &&
              std::is_aggregate_v<ui::Checkbox> && std::is_aggregate_v<ui::Slider> &&
              std::is_aggregate_v<ui::Dropdown> && std::is_aggregate_v<ui::TextInput> &&
              std::is_aggregate_v<ui::ProgressBar> && std::is_aggregate_v<ui::Style>);

// NOLINTBEGIN(custom-rmp-raw-pointer): Clay's MeasureFn signature, not ours to choose
Clay_Dimensions measure_stub(Clay_StringSlice text, Clay_TextElementConfig *config,
                             void * /*user*/) {
    // NOLINTEND(custom-rmp-raw-pointer)
    const float size = config != nullptr ? static_cast<float>(config->fontSize) : 16.0f;
    return Clay_Dimensions{ static_cast<float>(text.length) * size * 0.5f, size };
}

struct {
    Clay_Vector2 position{ 5.0f, 5.0f };
    bool down = false;
} pointer;

void pointer_stub(Clay_Vector2 &position, bool &down) {
    position = pointer.position;
    down = pointer.down;
}

struct {
    detail::NavState next{};
} keys;

void keys_stub(detail::NavState &out) {
    out = keys.next;
    keys.next = detail::NavState{};
}

struct {
    std::string typed;
} typing;

void typed_stub(detail::Typed &out) {
    out = detail::Typed{};
    for (const char c : typing.typed) {
        if (out.count < detail::MAX_TYPED)
            out.codepoints[static_cast<size_t>(out.count++)] = c;
    }
    typing.typed.clear();
}

std::vector<ui::Event> &heard() {
    static std::vector<ui::Event> events;
    return events;
}

struct HeadlessUi {
    HeadlessUi() {
        // From nothing: no event memory, no fonts, no letters left by a test before.
        ui::shutdown();
        pointer = {};
        keys.next = {};
        typing.typed.clear();
        heard().clear();
        rmp::detail::reset_reports_for_tests();
        ui::set_theme(ui::theme_dark());
        detail::set_measure_provider(measure_stub);
        detail::set_pointer_provider(pointer_stub);
        detail::set_nav_provider(keys_stub);
        detail::set_text_provider(typed_stub);
        detail::set_test_viewport(1280, 720);
        detail::set_test_frame_time(0);
        ui::focus("");
        ui::on_event([](const ui::Event &e) {
            heard().push_back(ui::Event{ e.kind, e.widget, {} });
        });
    }
    HeadlessUi(const HeadlessUi &) = delete;
    HeadlessUi &operator=(const HeadlessUi &) = delete;
    ~HeadlessUi() {
        detail::begin_frame();
        detail::end_frame();
        ui::on_event({});
        ui::focus("");
        ui::set_theme(ui::theme_dark());
        ui::set_font("");
        detail::set_measure_provider(detail::measure_with_raylib);
        detail::set_pointer_provider(detail::pointer_from_raylib);
        detail::set_nav_provider(detail::nav_from_raylib);
        detail::set_text_provider(nullptr);
        detail::set_test_viewport(0, 0);
        detail::set_test_frame_time(0);
        ui::shutdown();
        rmp::detail::reset_reports_for_tests();
    }
};

// One whole frame, with `body` between begin() and end().
template <class Body> void frame(Body &&body) {
    detail::begin_frame();
    ui::begin();
    body();
    ui::end();
    detail::end_frame();
}

std::span<const Clay_RenderCommand> commands() {
    const Clay_RenderCommandArray a = detail::last_pass_commands();
    return { a.internalArray, static_cast<std::size_t>(a.length) };
}

const detail::DrawRecord *record_of(const Clay_RenderCommand &c) {
    if (c.commandType == CLAY_RENDER_COMMAND_TYPE_TEXT || c.userData == nullptr)
        return nullptr;
    const auto *r = static_cast<const detail::DrawRecord *>(c.userData);
    return r->magic == detail::DRAW_MAGIC ? r : nullptr;
}

const detail::TextRecord *text_record_of(const Clay_RenderCommand &c) {
    if (c.commandType != CLAY_RENDER_COMMAND_TYPE_TEXT || c.userData == nullptr)
        return nullptr;
    const auto *r = static_cast<const detail::TextRecord *>(c.userData);
    return r->magic == detail::TEXT_MAGIC ? r : nullptr;
}

std::string text_of(const Clay_RenderCommand &c) {
    const auto &t = c.renderData.text.stringContents;
    return { t.chars, static_cast<std::size_t>(t.length > 0 ? t.length : 0) };
}

// The text command that draws `s`, or nullptr.
const Clay_RenderCommand *text_command(std::string_view s) {
    for (const Clay_RenderCommand &c : commands())
        if (c.commandType == CLAY_RENDER_COMMAND_TYPE_TEXT && text_of(c) == s) return &c;
    return nullptr;
}

int records() {
    int n = 0;
    for (const Clay_RenderCommand &c : commands())
        n += (record_of(c) != nullptr || text_record_of(c) != nullptr) ? 1 : 0;
    return n;
}

bool same(Color a, Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool same(Clay_Color a, Color b) {
    return std::lround(a.r) == b.r && std::lround(a.g) == b.g &&
        std::lround(a.b) == b.b && std::lround(a.a) == b.a;
}

Clay_Vector2 middle_of(std::string_view label) {
    Clay_BoundingBox box{};
    detail::bounds_of(label, 0, 0, &box);
    return Clay_Vector2{ box.x + box.width / 2, box.y + box.height / 2 };
}

int count(ui::EventKind kind) {
    return static_cast<int>(std::ranges::count_if(
        heard(), [kind](const ui::Event &e) { return e.kind == kind; }));
}

// A texture that never was: an id the renderer would draw, and a guard that
// zeroes it before the last handle lets go -- raylib unloads only a texture
// whose id is not 0, and there is no GPU here to unload one on.
class FakeTexture {
public:
    FakeTexture(unsigned id, int w, int h)
        : _handle(rmp::detail::adopt(
              rmp::detail::ResourceKind::TEXTURE,
              Texture2D{ id, w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 })) {}
    FakeTexture(const FakeTexture &) = delete;
    FakeTexture &operator=(const FakeTexture &) = delete;
    ~FakeTexture() { const_cast<Texture2D &>(_handle.raw()).id = 0; }
    [[nodiscard]] const rmp::Texture &handle() const { return _handle; }

private:
    rmp::Texture _handle;
};

// A sheet with no texture (no GPU), its frames, and slices.
rmp::SpriteSheet fake_sheet() {
    rmp::SheetData data;
    data.width = 8;
    data.height = 8;
    data.frames = { rmp::SheetFrame{ Rectangle{ 0, 0, 8, 8 }, 0.1f },
                    rmp::SheetFrame{ Rectangle{ 8, 0, 8, 8 }, 0.1f } };
    data.slices = { rmp::SheetSlice{ "coin", Rectangle{ 1, 1, 4, 4 }, false, {} },
                    rmp::SheetSlice{ "frame", Rectangle{ 8, 0, 8, 8 }, true,
                                     Rectangle{ 2, 3, 3, 2 } } };
    return rmp::SpriteSheet{ rmp::detail::adopt(rmp::detail::ResourceKind::SHEET,
                                                std::move(data)) };
}

} // namespace

TEST_SUITE("ui: looks") {
    // (each suite's cases are inside its braces)

    TEST_CASE("the framework's themes carry no style, no sound and no motion") {
        for (const ui::Theme &t : { ui::theme_dark(), ui::theme_light() }) {
            CHECK_FALSE(t.sounds.click);
            CHECK_FALSE(t.sounds.hover);
            CHECK_FALSE(t.sounds.volume);
            CHECK(t.motion.hover_scale == 1);
            CHECK(t.motion.press_scale == 1);
            CHECK(t.motion.hover_lift == 0);
            CHECK(t.motion.appear == 0);
            for (const ui::Style *s :
                 { &t.styles.button, &t.styles.label, &t.styles.image, &t.styles.panel,
                   &t.styles.checkbox, &t.styles.slider, &t.styles.dropdown,
                   &t.styles.text_input, &t.styles.progress }) {
                CHECK_FALSE(s->background);
                CHECK_FALSE(s->skin);
                CHECK_FALSE(s->shadow);
                CHECK_FALSE(s->motion);
                CHECK_FALSE(s->font.valid());
            }
        }
    }

    TEST_CASE("with the framework's themes a frame of every widget carries no record") {
        const HeadlessUi headless;
        bool on = false;
        float value = 0.5f;
        int pick = 0;
        std::string name = "Ana";
        for (int i = 0; i < 3; ++i) {
            frame([&] {
                ui::text("Title");
                ui::button("Play");
                ui::checkbox("Sound", on);
                ui::slider("Volume", value, 0, 1);
                ui::dropdown("Quality", pick, { "Low", "High" });
                ui::text_input("Name", name);
                ui::panel([] { ui::text("inside"); });
                ui::progress(0.3f);
            });
        }
        CHECK(records() == 0);
        for (const Clay_RenderCommand &c : commands()) {
            if (c.commandType == CLAY_RENDER_COMMAND_TYPE_TEXT)
                CHECK(c.renderData.text.fontId == 0);
        }
        CHECK(rmp::detail::report_count() == 0);
    }

    TEST_CASE(
        "inside a style's scope current_theme() is the theme with the style in it") {
        const HeadlessUi headless;
        const ui::Theme dark = ui::theme_dark();
        const ui::Style s{ .background = RED,
                           .border = GREEN,
                           .border_width = 3,
                           .radius = 0,
                           .padding_x = 5,
                           .padding_y = 6,
                           .gap = 7,
                           .text = BLUE,
                           .text_disabled = GRAY,
                           .font_size = 40 };
        {
            const detail::LookScope scope(ui::WidgetKind::BUTTON, &s);
            const ui::Theme &t = ui::current_theme();
            CHECK(detail::look_theme() == &t);
            CHECK(same(t.surface, RED));
            CHECK(same(t.primary, RED));
            CHECK(same(t.danger, RED));
            CHECK(same(t.panel, RED));
            // A little lighter, a little darker, when not given.
            CHECK(t.surface_hover.r > RED.r - 1);
            CHECK(t.surface_hover.g > RED.g);
            CHECK(t.surface_press.r < RED.r);
            CHECK(same(t.text, BLUE));
            CHECK(same(t.text_on_accent, BLUE));
            CHECK(same(t.disabled_text, GRAY));
            CHECK(same(t.border, GREEN));
            CHECK(t.border_width == 3);
            CHECK(t.corner_radius == 0);
            CHECK(t.padding_x == 5);
            CHECK(t.panel_padding == 5);
            CHECK(t.padding_y == 6);
            CHECK(t.gap == 7);
            CHECK(t.font_size == 40);
            CHECK(t.font_size_large == doctest::Approx(dark.font_size_large * 2));
            CHECK(t.font_size_small == doctest::Approx(dark.font_size_small * 2));
            // What a style does not set is the theme's.
            CHECK(same(t.focus, dark.focus));
            CHECK(t.control_size == dark.control_size);
            CHECK(t.transition == dark.transition);
        }
        CHECK(detail::look_theme() == nullptr);
        CHECK(same(ui::current_theme().surface, dark.surface));
        CHECK(ui::current_theme().gap == dark.gap);
    }

    TEST_CASE("hover and press colours that are given are used, and negatives are 0") {
        const HeadlessUi headless;
        const ui::Style s{ .background = RED,
                           .background_hover = GOLD,
                           .background_press = MAROON,
                           .background_disabled = DARKGRAY,
                           .border_width = -4,
                           .radius = -1,
                           .gap = -2 };
        const detail::LookScope scope(ui::WidgetKind::BUTTON, &s);
        const ui::Theme &t = ui::current_theme();
        CHECK(same(t.surface_hover, GOLD));
        CHECK(same(t.primary_hover, GOLD));
        CHECK(same(t.surface_press, MAROON));
        CHECK(same(t.disabled, DARKGRAY));
        CHECK(t.border_width == 0);
        CHECK(t.corner_radius == 0);
        CHECK(t.gap == 0);
    }

    TEST_CASE(
        "a style resolves: its own over the containers' children over Theme::styles") {
        const HeadlessUi headless;
        ui::Theme t = ui::theme_dark();
        t.styles.button.text = RED;
        t.styles.button.radius = 3;
        t.sounds.click = "tick";
        t.sounds.hover = "over";
        ui::set_theme(t);

        {
            const detail::LookScope scope(ui::WidgetKind::BUTTON, nullptr);
            CHECK(same(*detail::look().text, RED));
            CHECK(*detail::look().sounds.click == "tick");
        }
        {
            // Theme::styles is per kind: a label does not get the buttons'.
            const detail::LookScope scope(ui::WidgetKind::LABEL, nullptr);
            CHECK_FALSE(detail::look().text);
            CHECK_FALSE(detail::look().radius);
        }
        detail::push_children(ui::Style{ .text = GREEN, .sounds = { .hover = "" } });
        detail::push_children(ui::Style{ .gap = 9 });
        {
            const detail::LookScope scope(ui::WidgetKind::BUTTON, nullptr);
            CHECK(same(*detail::look().text, GREEN)); // the outer container's
            CHECK(*detail::look().gap == 9); // the inner one's
            CHECK(*detail::look().radius == 3); // the theme's, nobody else set it
            CHECK(detail::look().sounds.hover->empty()); // silenced by a container
            CHECK(*detail::look().sounds.click == "tick");
        }
        {
            const ui::Style own{ .text = BLUE };
            const detail::LookScope scope(ui::WidgetKind::BUTTON, &own);
            CHECK(same(*detail::look().text, BLUE));
        }
        detail::pop_children();
        detail::pop_children();
        detail::pop_children(); // one too many does nothing
        {
            const detail::LookScope scope(ui::WidgetKind::BUTTON, nullptr);
            CHECK(same(*detail::look().text, RED));
        }
    }

    TEST_CASE("Theme::styles.button reaches every button, the immediate ones too") {
        const HeadlessUi headless;
        ui::Theme t = ui::theme_dark();
        t.styles.button.shadow = ui::Shadow{};
        t.styles.button.text = GOLD;
        ui::set_theme(t);
        frame([] { ui::button("Play"); });
        const Clay_RenderCommand *play = text_command("Play");
        REQUIRE(play != nullptr);
        CHECK(same(play->renderData.text.textColor, GOLD));
        int shadows = 0;
        for (const Clay_RenderCommand &c : commands()) {
            if (const detail::DrawRecord *r = record_of(c))
                shadows += r->has_shadow ? 1 : 0;
        }
        CHECK(shadows == 1);
    }

    TEST_CASE(
        "a kept Panel styles itself with `style` and its descendants with `children`") {
        const HeadlessUi headless;
        ui::Panel shop{ .style = { .background = RED }, .children = { .text = GREEN } };
        frame([&] {
            shop.show([] { ui::text("inside"); });
            ui::text("outside");
        });
        const Clay_RenderCommand *inside = text_command("inside");
        const Clay_RenderCommand *outside = text_command("outside");
        REQUIRE(inside != nullptr);
        REQUIRE(outside != nullptr);
        CHECK(same(inside->renderData.text.textColor, GREEN));
        CHECK(same(outside->renderData.text.textColor, ui::theme_dark().text));
        bool red_box = false;
        for (const Clay_RenderCommand &c : commands()) {
            if (c.commandType == CLAY_RENDER_COMMAND_TYPE_RECTANGLE &&
                same(c.renderData.rectangle.backgroundColor, RED))
                red_box = true;
        }
        CHECK(red_box);
    }

    TEST_CASE("a kept Panel with background = false is a bare row or column") {
        const HeadlessUi headless;
        ui::Panel bar{ .row = true, .background = false };
        frame([&] {
            bar.show([] {
                ui::text("a");
                ui::text("b");
            });
        });
        const Clay_RenderCommand *a = text_command("a");
        const Clay_RenderCommand *b = text_command("b");
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        CHECK(a->boundingBox.y == doctest::Approx(b->boundingBox.y));
        CHECK(b->boundingBox.x > a->boundingBox.x);
        for (const Clay_RenderCommand &c : commands())
            CHECK(c.commandType != CLAY_RENDER_COMMAND_TYPE_RECTANGLE);
    }

    TEST_CASE("a shadow travels to the renderer in pixels, under its element") {
        const HeadlessUi headless;
        ui::Button go{ .text = "Go",
                       .style = { .shadow = ui::Shadow{ .offset = { 2, 4 },
                                                        .blur = 8,
                                                        .spread = 1,
                                                        .color = BLACK } } };
        frame([&] { go.show(); });
        const float k = ui::scale();
        const detail::DrawRecord *r = nullptr;
        for (const Clay_RenderCommand &c : commands())
            if (record_of(c) != nullptr) r = record_of(c);
        REQUIRE(r != nullptr);
        CHECK(r->has_shadow);
        CHECK(r->shadow_offset.x == doctest::Approx(2 * k));
        CHECK(r->shadow_offset.y == doctest::Approx(4 * k));
        CHECK(r->shadow_blur == doctest::Approx(8 * k));
        CHECK(r->shadow_spread == doctest::Approx(1 * k));
        CHECK(same(r->shadow_color, BLACK));
        CHECK(r->scale == 1);
        CHECK(r->alpha == 1);
    }

    TEST_CASE("an outline, a text shadow and a font travel with the text") {
        const HeadlessUi headless;
        // A font as rmp::assets would hand it: a file's name, nothing on a GPU.
        const rmp::Font title{ rmp::detail::adopt_named(rmp::detail::ResourceKind::FONT,
                                                        "title.ttf", 32, ::Font{}) };
        ui::Label heading{ .text = "Chapter",
                           .style = { .font = title,
                                      .text_outline =
                                          ui::TextOutline{ .width = 2, .color = WHITE },
                                      .text_shadow = ui::TextShadow{ .offset = { 1, 3 },
                                                                     .color = BLACK } } };
        frame([&] {
            heading.show();
            ui::text("plain");
        });
        const Clay_RenderCommand *chapter = text_command("Chapter");
        REQUIRE(chapter != nullptr);
        const detail::TextRecord *t = text_record_of(*chapter);
        REQUIRE(t != nullptr);
        const float k = ui::scale();
        CHECK(t->outline);
        CHECK(t->outline_px == doctest::Approx(std::max(1.0f, 2 * k)));
        CHECK(same(t->outline_color, WHITE));
        CHECK(t->shadow);
        CHECK(t->shadow_px.y == doctest::Approx(3 * k));
        CHECK(chapter->renderData.text.fontId != 0);
        CHECK(detail::font_id_of(title) == chapter->renderData.text.fontId);
        const Clay_RenderCommand *plain = text_command("plain");
        REQUIRE(plain != nullptr);
        CHECK(plain->renderData.text.fontId == 0);
        CHECK(text_record_of(*plain) == nullptr);
    }

    TEST_CASE("fonts are registered by file, once, and the global one is 0") {
        const HeadlessUi headless;
        const rmp::Font a{ rmp::detail::adopt_named(rmp::detail::ResourceKind::FONT,
                                                    "a.ttf", 10, ::Font{}) };
        const rmp::Font a2{ rmp::detail::adopt_named(rmp::detail::ResourceKind::FONT,
                                                     "a.ttf", 20, ::Font{}) };
        const rmp::Font b{ rmp::detail::adopt_named(rmp::detail::ResourceKind::FONT,
                                                    "b.ttf", 10, ::Font{}) };
        const uint16_t ia = detail::font_id_of(a);
        CHECK(ia != 0);
        CHECK(detail::font_id_of(a2) == ia); // the same file at another size
        CHECK(detail::font_id_of(b) != ia);
        CHECK(detail::font_id_of(rmp::Font{}) == 0); // no font: the global one
        ui::set_font(a);
        CHECK(ui::font_name() == "a.ttf");
        CHECK(detail::font_id_of(a) == 0); // the global font is 0, whatever it is
        ui::set_font("pixel.ttf");
        CHECK(ui::font_name() == "pixel.ttf");
        ui::set_font("");
        CHECK(ui::font_name().empty());
    }
}

// ---------------------------------------------------------------------------

namespace {

struct {
    int bakes = 0;
    std::vector<int> glyphs;
} bakes;

rmp::Font counting_bake(const char * /*name*/, int pixel_size,
                        std::span<const int> glyphs) {
    ++bakes.bakes;
    bakes.glyphs.assign(glyphs.begin(), glyphs.end());
    ::Font face{};
    face.baseSize = pixel_size;
    face.glyphCount = 1;
    return rmp::Font{ rmp::detail::adopt(rmp::detail::ResourceKind::FONT, face) };
}

bool has(int codepoint) {
    return std::ranges::find(bakes.glyphs, codepoint) != bakes.glyphs.end();
}

} // namespace

TEST_SUITE("ui: letters") {
    TEST_CASE("a .ttf is baked with Latin-1 and Latin Extended-A, and any letter a text "
              "needs") {
        const HeadlessUi headless;
        detail::set_measure_provider(detail::measure_with_raylib);
        detail::set_font_provider(counting_bake, "fake.ttf");
        bakes = {};
        frame([] { ui::text("Ñandú, ¿qué?"); });
        REQUIRE(bakes.bakes >= 1);
        CHECK(has(0xF1)); // ñ
        CHECK(has(0xBF)); // ¿
        CHECK(has(0x0141)); // Ł, Latin Extended-A
        CHECK(has(0x20AC)); // €
        CHECK_FALSE(has(0x65E5)); // 日, not yet
        const int before = bakes.bakes;
        frame([] { ui::text("Ñandú, ¿qué?"); });
        CHECK(bakes.bakes == before); // nothing new: nothing baked again

        frame([] { ui::text("日本"); });
        CHECK(bakes.bakes > before); // baked again, with the new letters
        CHECK(has(0x65E5));
        CHECK(has(0x672C));
        CHECK(has(0xF1)); // and the ones it had
        CHECK(std::ranges::is_sorted(bakes.glyphs));
        detail::set_font_provider(nullptr, nullptr);
    }

    TEST_CASE("the default list of letters is what rmp::assets bakes a .ttf with") {
        const std::span<const int> list = rmp::assets::detail::default_codepoints();
        CHECK(list.front() == 32);
        CHECK(std::ranges::is_sorted(list));
        CHECK(std::ranges::adjacent_find(list) == list.end());
        for (const int c : { 0x41, 0x7A, 0x3F, 0xF1, 0xD1, 0xE1, 0xBF, 0xA1, 0x0153,
                             0x017E, 0x2026, 0x20AC })
            CHECK(std::ranges::find(list, c) != list.end());
        // raylib's own default, ASCII alone, would have had 95.
        CHECK(list.size() > 95U + 200U);
    }

    TEST_CASE("the built-in font bakes nothing, whatever the letters") {
        const HeadlessUi headless;
        detail::set_measure_provider(detail::measure_with_raylib);
        detail::set_font_provider(counting_bake, nullptr);
        bakes = {};
        frame([] { ui::text("日本 Ñ"); });
        CHECK(bakes.bakes == 0);
        detail::set_font_provider(nullptr, nullptr);
    }
}

// ---------------------------------------------------------------------------

TEST_SUITE("ui: pictures") {
    TEST_CASE(
        "a Picture is made from a texture, a part of one, a nine-slice or a sprite") {
        const FakeTexture art(7, 64, 32);
        const ui::Picture whole(art.handle());
        CHECK_FALSE(whole.empty());
        CHECK(whole.size().x == 64);
        CHECK(whole.size().y == 32);
        CHECK_FALSE(whole.sliced());

        const ui::Picture part(art.handle(), Rectangle{ 16, 0, 16, 16 });
        CHECK(part.size().x == 16);
        CHECK(part.source.x == 16);

        const ui::Picture nine = ui::NineSlice{ .texture = art.handle(),
                                                .source = { 0, 0, 24, 24 },
                                                .left = 8,
                                                .top = 8,
                                                .right = 8,
                                                .bottom = 8,
                                                .scale = 2 };
        CHECK(nine.sliced());
        CHECK(nine.slice_scale == 2);
        CHECK(nine.size().x == 24);

        rmp::Sprite sprite;
        sprite.texture = art.handle();
        const ui::Picture of_sprite(sprite);
        CHECK(of_sprite.size().x == 64);

        CHECK(ui::Picture{}.empty());
        CHECK(ui::Picture(rmp::Texture{}).empty());
    }

    TEST_CASE("a sprite with a sheet shows the frame it is on") {
        rmp::Sprite sprite;
        sprite.sheet = fake_sheet();
        sprite.set_frame(1);
        const ui::Picture p(sprite);
        CHECK(p.source.x == 8);
        CHECK(p.size().x == 8);
        // No GPU, so no texture: nothing to draw, and said so.
        CHECK(p.empty());
    }

    TEST_CASE("slice() and nine_slice() read an Aseprite file's slices") {
        const HeadlessUi headless;
        const rmp::SpriteSheet sheet = fake_sheet();
        const ui::Picture coin = ui::slice(sheet, "coin");
        CHECK(coin.source.x == 1);
        CHECK(coin.source.width == 4);
        CHECK_FALSE(coin.sliced());

        const ui::Picture frame_picture = ui::slice(sheet, "frame");
        CHECK(frame_picture.sliced());
        CHECK(frame_picture.left == 2);
        CHECK(frame_picture.top == 3);
        CHECK(frame_picture.right == 3); // 8 - 2 - 3
        CHECK(frame_picture.bottom == 3); // 8 - 3 - 2

        const ui::NineSlice n = ui::nine_slice(sheet, "frame");
        CHECK(n.left == 2);
        CHECK(n.right == 3);
        CHECK(n.source.x == 8);

        CHECK(rmp::detail::report_count() == 0);
        CHECK(ui::slice(sheet, "nope").sheet.valid() == false);
        CHECK(rmp::detail::report_count() == 1);
        CHECK(ui::slice(sheet, "nope").sheet.valid() == false);
        CHECK(rmp::detail::report_count() == 1); // once
        const ui::NineSlice plain = ui::nine_slice(sheet, "coin");
        CHECK(plain.left == 0);
        CHECK(plain.source.width == 4);
        CHECK(rmp::detail::report_count() == 2); // a slice with no 9-slices, said
    }

    TEST_CASE("image() of a Picture draws its part, tinted, from a record that keeps it "
              "loaded") {
        const HeadlessUi headless;
        const FakeTexture art(9, 64, 64);
        frame([&] {
            ui::image(ui::Picture(art.handle(), Rectangle{ 16, 16, 32, 8 }),
                      { .tint = GOLD });
            ui::image(art.handle());
        });
        int images = 0;
        for (const Clay_RenderCommand &c : commands()) {
            if (c.commandType != CLAY_RENDER_COMMAND_TYPE_IMAGE) continue;
            const detail::DrawRecord *r = record_of(c);
            REQUIRE(r != nullptr);
            CHECK(r->has_picture);
            CHECK(r->raw.id == 9);
            CHECK(r->texture.valid());
            if (images == 0) {
                CHECK(r->source.x == 16);
                CHECK(r->source.width == 32);
                CHECK(same(r->tint, GOLD));
                CHECK(c.boundingBox.width == doctest::Approx(32 * ui::scale()));
            } else {
                CHECK(r->source.width == 64);
                CHECK(same(r->tint, WHITE));
            }
            ++images;
        }
        CHECK(images == 2);
    }

    TEST_CASE("a skin is the box: no flat fill, its picture per state, the rest tinted") {
        const HeadlessUi headless;
        const FakeTexture normal(11, 24, 24);
        const FakeTexture pressed(12, 24, 24);
        ui::Button play{ .text = "Play",
                         .style = { .skin = ui::Skin{ .normal = normal.handle(),
                                                      .press = pressed.handle() } } };
        const auto skin_record = [] {
            for (const Clay_RenderCommand &c : commands()) {
                if (c.commandType == CLAY_RENDER_COMMAND_TYPE_IMAGE)
                    if (const detail::DrawRecord *r = record_of(c)) return r;
            }
            return static_cast<const detail::DrawRecord *>(nullptr);
        };
        frame([&] { play.show(); });
        for (const Clay_RenderCommand &c : commands())
            CHECK(c.commandType != CLAY_RENDER_COMMAND_TYPE_RECTANGLE);
        const detail::DrawRecord *r = skin_record();
        REQUIRE(r != nullptr);
        CHECK(r->raw.id == 11);
        CHECK(r->brighten == 0);

        // Under the pointer: no hover picture, so the normal one, lit.
        pointer.position = middle_of("Play");
        frame([&] { play.show(); });
        r = skin_record();
        REQUIRE(r != nullptr);
        CHECK(r->raw.id == 11);
        CHECK(r->brighten > 0);

        // Held: its own press picture.
        pointer.down = true;
        frame([&] { play.show(); });
        r = skin_record();
        REQUIRE(r != nullptr);
        CHECK(r->raw.id == 12);
        pointer.down = false;
        frame([&] { play.show(); });

        // Disabled: the normal one, greyed.
        play.enabled = false;
        frame([&] { play.show(); });
        r = skin_record();
        REQUIRE(r != nullptr);
        CHECK(r->raw.id == 11);
        CHECK(r->tint.r < 255);
    }

    TEST_CASE("image_button() is pressed like a button, and only the picture is drawn") {
        const HeadlessUi headless;
        const FakeTexture gear(21, 32, 32);
        bool pressed = false;
        const auto draw = [&] {
            pressed = ui::image_button(gear.handle(), { .id = "gear" }) || pressed;
        };
        frame(draw);
        for (const Clay_RenderCommand &c : commands())
            CHECK(c.commandType != CLAY_RENDER_COMMAND_TYPE_RECTANGLE);
        pointer.position = middle_of("gear");
        frame(draw);
        pointer.down = true;
        frame(draw);
        pointer.down = false;
        frame(draw);
        CHECK(pressed);
        CHECK(count(ui::EventKind::CLICK) == 1);
        CHECK(heard().back().widget == ui::WidgetKind::BUTTON);
    }

    TEST_CASE("a ProgressBar's fill can be a picture, stretched with the bar") {
        const HeadlessUi headless;
        const FakeTexture fill(31, 16, 8);
        ui::ProgressBar hp{ .value = 0.5f, .width = 200, .fill_picture = fill.handle() };
        frame([&] { hp.show(); });
        bool found = false;
        for (const Clay_RenderCommand &c : commands()) {
            const detail::DrawRecord *r = record_of(c);
            if (c.commandType == CLAY_RENDER_COMMAND_TYPE_IMAGE && r != nullptr &&
                r->raw.id == 31) {
                found = true;
                CHECK(c.boundingBox.width ==
                      doctest::Approx(100 * ui::scale()).epsilon(0.05));
            }
        }
        CHECK(found);
    }
}

// ---------------------------------------------------------------------------

TEST_SUITE("ui: events and kept widgets") {
    TEST_CASE("a button's events: hover, press, release, click, unhover, focus, blur") {
        const HeadlessUi headless;
        const auto draw = [] { ui::button("Play"); };
        frame(draw);
        CHECK(heard().empty());
        pointer.position = middle_of("Play");
        frame(draw);
        CHECK(count(ui::EventKind::HOVER) == 1);
        pointer.down = true;
        frame(draw);
        CHECK(count(ui::EventKind::PRESS) == 1);
        pointer.down = false;
        frame(draw);
        CHECK(count(ui::EventKind::RELEASE) == 1);
        CHECK(count(ui::EventKind::CLICK) == 1);
        pointer.position = Clay_Vector2{ 1, 1 };
        frame(draw);
        CHECK(count(ui::EventKind::UNHOVER) == 1);
        // In order: what happened first, first. (The pointer arriving gives the
        // button the focus too: that is a FOCUS, between the HOVER and the PRESS.)
        const auto at = [](ui::EventKind kind) {
            return std::ranges::find_if(
                       heard(), [kind](const ui::Event &e) { return e.kind == kind; }) -
                heard().begin();
        };
        CHECK(at(ui::EventKind::HOVER) == 0);
        CHECK(at(ui::EventKind::HOVER) < at(ui::EventKind::FOCUS));
        CHECK(at(ui::EventKind::FOCUS) < at(ui::EventKind::PRESS));
        CHECK(at(ui::EventKind::PRESS) < at(ui::EventKind::RELEASE));
        CHECK(at(ui::EventKind::RELEASE) < at(ui::EventKind::CLICK));
        CHECK(at(ui::EventKind::CLICK) < at(ui::EventKind::UNHOVER));
    }

    TEST_CASE(
        "Enter on the focused button is a click, and the focus arriving is an event") {
        const HeadlessUi headless;
        const auto draw = [] { ui::button("Play"); };
        ui::focus("Play");
        frame(draw);
        frame(draw);
        CHECK(count(ui::EventKind::FOCUS) == 1);
        keys.next.activate = true;
        frame(draw);
        CHECK(count(ui::EventKind::CLICK) == 1);
        CHECK(count(ui::EventKind::PRESS) == 0); // a key is no pointer
    }

    TEST_CASE("on_event({}) turns the hook off") {
        const HeadlessUi headless;
        ui::on_event({});
        const auto draw = [] { ui::button("Play"); };
        frame(draw);
        pointer.position = middle_of("Play");
        frame(draw);
        CHECK(heard().empty());
    }

    TEST_CASE(
        "a kept Button calls back in the order things happened, and knows its state") {
        const HeadlessUi headless;
        std::vector<std::string> calls;
        ui::Button play{ .text = "Play",
                         .on_click = [&] { calls.emplace_back("click"); },
                         .on_hover = [&] { calls.emplace_back("hover"); },
                         .on_unhover = [&] { calls.emplace_back("unhover"); },
                         .on_press = [&] { calls.emplace_back("press"); },
                         .on_release = [&] { calls.emplace_back("release"); },
                         .on_focus = [&] { calls.emplace_back("focus"); },
                         .on_blur = [&] { calls.emplace_back("blur"); } };
        bool clicked = false;
        const auto draw = [&] { clicked = play.show() || clicked; };
        frame(draw);
        pointer.position = middle_of("Play");
        frame(draw);
        CHECK(play.hovered());
        pointer.down = true;
        frame(draw);
        CHECK(play.pressed());
        pointer.down = false;
        frame(draw);
        CHECK_FALSE(play.pressed());
        pointer.position = Clay_Vector2{ 1, 1 };
        frame(draw);
        CHECK_FALSE(play.hovered());
        CHECK(clicked);
        // The pointer arriving gives it the focus too, on the same frame.
        const std::vector<std::string> expected{ "hover",   "focus", "press",
                                                 "release", "click", "unhover" };
        CHECK(calls == expected);
        CHECK(play.focused());

        // The focus going to another control is a blur.
        calls.clear();
        ui::Button other{ .text = "Other" };
        const auto both = [&] {
            play.show();
            other.show();
        };
        ui::focus("Other");
        frame(both);
        frame(both);
        CHECK_FALSE(play.focused());
        CHECK(calls == std::vector<std::string>{ "blur" });
        ui::focus("Play");
        frame(both);
        frame(both);
        CHECK(play.focused());
        CHECK(calls == std::vector<std::string>{ "blur", "focus" });
    }

    TEST_CASE("a kept Button with an icon and no text is a picture button") {
        const HeadlessUi headless;
        const FakeTexture icon(41, 16, 16);
        ui::Button tool{ .icon = icon.handle(), .icon_size = 24, .id = "tool" };
        frame([&] { tool.show(); });
        bool picture = false;
        for (const Clay_RenderCommand &c : commands()) {
            const detail::DrawRecord *r = record_of(c);
            if (c.commandType == CLAY_RENDER_COMMAND_TYPE_IMAGE && r != nullptr &&
                r->raw.id == 41) {
                picture = true;
                CHECK(c.boundingBox.height ==
                      doctest::Approx(24 * ui::scale()).epsilon(0.05));
            }
            CHECK(c.commandType != CLAY_RENDER_COMMAND_TYPE_TEXT);
        }
        CHECK(picture);
    }

    TEST_CASE("a kept Button with an icon and text lays them side by side, or one over "
              "the other") {
        const HeadlessUi headless;
        const FakeTexture icon(42, 16, 16);
        for (const bool above : { false, true }) {
            ui::Button save{ .text = "Save", .icon = icon.handle(), .icon_above = above };
            frame([&] { save.show(); });
            Clay_BoundingBox picture{};
            for (const Clay_RenderCommand &c : commands())
                if (c.commandType == CLAY_RENDER_COMMAND_TYPE_IMAGE)
                    picture = c.boundingBox;
            const Clay_RenderCommand *label = text_command("Save");
            REQUIRE(label != nullptr);
            if (above) {
                CHECK(picture.y < label->boundingBox.y);
            } else {
                CHECK(picture.x < label->boundingBox.x);
            }
        }
    }

    TEST_CASE("kept value controls hold their value and say when it changed") {
        const HeadlessUi headless;
        std::vector<std::string> calls;
        ui::Checkbox sound{ .text = "Sound", .on_change = [&](bool v) {
                               calls.push_back(v ? "on" : "off");
                           } };
        const auto draw = [&] { sound.show(); };
        frame(draw);
        pointer.position = middle_of("Sound");
        frame(draw);
        pointer.down = true;
        frame(draw);
        pointer.down = false;
        frame(draw);
        CHECK(sound.checked);
        CHECK(calls == std::vector<std::string>{ "on" });
        CHECK(count(ui::EventKind::CHANGE) == 1);
    }

    TEST_CASE("a kept Slider moves with the keys and calls on_change with the value") {
        const HeadlessUi headless;
        float seen = -1;
        ui::Slider volume{ .text = "Volume",
                           .value = 0.5f,
                           .step = 0.1f,
                           .on_change = [&](float v) { seen = v; } };
        const auto draw = [&] { volume.show(); };
        ui::focus("Volume");
        frame(draw);
        frame(draw);
        keys.next.x = 1;
        frame(draw);
        CHECK(volume.value == doctest::Approx(0.6f));
        CHECK(seen == doctest::Approx(0.6f));
        CHECK(count(ui::EventKind::CHANGE) == 1);
    }

    TEST_CASE("a kept Dropdown opens, closes and picks, with an event for each") {
        const HeadlessUi headless;
        int picked = -1;
        ui::Dropdown quality{ .text = "Quality",
                              .items = { "Low", "Medium", "High" },
                              .on_change = [&](int i) { picked = i; } };
        const auto draw = [&] { quality.show(); };
        ui::focus("Quality");
        frame(draw);
        frame(draw);
        keys.next.activate = true;
        frame(draw);
        CHECK(count(ui::EventKind::OPEN) == 1);
        keys.next.y = 1;
        frame(draw);
        keys.next.activate = true;
        keys.next.submit = true;
        frame(draw);
        CHECK(count(ui::EventKind::CLOSE) == 1);
        CHECK(quality.selected == 1);
        CHECK(picked == 1);
    }

    TEST_CASE("a kept TextInput holds its text and calls on_change with it") {
        const HeadlessUi headless;
        std::string seen;
        ui::TextInput name{ .text = "Name",
                            .placeholder = "you",
                            .on_change = [&](std::string_view v) { seen = v; } };
        const auto draw = [&] { name.show(); };
        ui::focus("Name");
        frame(draw);
        frame(draw);
        typing.typed = "Ana";
        frame(draw);
        CHECK(name.value == "Ana");
        CHECK(seen == "Ana");
    }

    TEST_CASE("a kept Label and Image can be clicked, and a Panel with an on_click too") {
        const HeadlessUi headless;
        const FakeTexture art(51, 32, 32);
        int label_clicks = 0;
        int image_clicks = 0;
        int panel_clicks = 0;
        ui::Label title{ .text = "Title", .on_click = [&] { ++label_clicks; } };
        ui::Image logo{ .picture = art.handle(), .id = "logo", .on_click = [&] {
                           ++image_clicks;
                       } };
        ui::Panel card{ .id = "card", .on_click = [&] { ++panel_clicks; } };
        const auto draw = [&] {
            title.show();
            logo.show();
            card.show([] { ui::text("card text"); });
        };
        const auto click_on = [&](std::string_view label) {
            frame(draw);
            pointer.position = middle_of(label);
            frame(draw);
            pointer.down = true;
            frame(draw);
            pointer.down = false;
            frame(draw);
        };
        click_on("Title");
        CHECK(label_clicks == 1);
        click_on("logo");
        CHECK(image_clicks == 1);
        click_on("card");
        CHECK(panel_clicks == 1);
    }

    TEST_CASE(
        "shake() moves the widget side to side for its Motion's time, and is an event") {
        const HeadlessUi headless;
        ui::Button wrong{ .text = "Login" };
        detail::set_test_frame_time(1.0f / 60.0f);
        frame([&] { wrong.show(); });
        wrong.shake();
        frame([&] { wrong.show(); });
        CHECK(count(ui::EventKind::SHAKE) == 1);
        bool moved = false;
        for (int i = 0; i < 5; ++i) {
            frame([&] { wrong.show(); });
            for (const Clay_RenderCommand &c : commands())
                if (const detail::DrawRecord *r = record_of(c))
                    moved = moved || r->offset.x != 0;
        }
        CHECK(moved);
        for (int i = 0; i < 40; ++i) frame([&] { wrong.show(); });
        CHECK(records() == 0); // the shake is over: back to no record at all
    }

    TEST_CASE("appear fades a box in, sliding up, over its Motion's time") {
        const HeadlessUi headless;
        detail::set_test_frame_time(0.05f);
        ui::Panel dialog{ .id = "dialog",
                          .style = { .motion = ui::Motion{ .appear = 0.2f,
                                                           .appear_offset = 20,
                                                           .appear_scale = 0.9f } } };
        std::vector<float> alphas;
        std::vector<float> offsets;
        for (int i = 0; i < 6; ++i) {
            frame([&] { dialog.show([] { ui::text("hi"); }); });
            for (const Clay_RenderCommand &c : commands()) {
                if (const detail::DrawRecord *r = record_of(c)) {
                    alphas.push_back(r->alpha);
                    offsets.push_back(r->offset.y);
                    break;
                }
            }
        }
        REQUIRE(alphas.size() == 6);
        CHECK(alphas.front() == 0);
        CHECK(offsets.front() == doctest::Approx(20 * ui::scale()));
        CHECK(std::ranges::is_sorted(alphas));
        CHECK(alphas.back() == 1);
        CHECK(offsets.back() == 0);
    }

    TEST_CASE("hover_scale and press_scale grow and shrink the box over Motion::speed") {
        const HeadlessUi headless;
        detail::set_test_frame_time(1.0f / 60.0f);
        ui::Button go{ .text = "Go",
                       .style = { .motion = ui::Motion{ .hover_scale = 1.1f,
                                                        .press_scale = 0.9f,
                                                        .hover_lift = 4 } } };
        const auto scale_now = [] {
            for (const Clay_RenderCommand &c : commands())
                if (const detail::DrawRecord *r = record_of(c)) return r->scale;
            return 0.0f;
        };
        frame([&] { go.show(); });
        CHECK(scale_now() == 1);
        pointer.position = middle_of("Go");
        for (int i = 0; i < 30; ++i) frame([&] { go.show(); });
        CHECK(scale_now() == doctest::Approx(1.1f));
        pointer.down = true;
        for (int i = 0; i < 30; ++i) frame([&] { go.show(); });
        CHECK(scale_now() == doctest::Approx(1.1f * 0.9f));
    }

    TEST_CASE("a theme's sounds are named per event, and an empty one is silence") {
        const HeadlessUi headless;
        ui::Theme t = ui::theme_dark();
        t.sounds.click = "click";
        ui::set_theme(t);
        const ui::Style quiet{ .sounds = { .click = "" } };
        const detail::LookScope a(ui::WidgetKind::BUTTON, &quiet);
        CHECK(detail::look().sounds.click->empty());
    }
}
