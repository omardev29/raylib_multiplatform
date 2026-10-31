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

#include "../src/rmp/audio_internal.h"
#include "../src/rmp/internal.h"
#include "../src/rmp/ui/internal.h"
#include "../src/rmp/ui_internal.h"

#include <raylib.h>
#include <rmp/assets.h>
#include <rmp/object.h>
#include <rmp/ui.h>

#include <algorithm>
#include <array>
#include <limits>
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
            out.codepoints[static_cast<size_t>(out.count++)] =
                static_cast<unsigned char>(c);
    }
    typing.typed.clear();
}

// How many times rmp::audio asked for a sound device.
struct {
    int count = 0;
} audio_opens;

// What the test's sound player was asked to play.
struct {
    std::vector<std::string> names;
    std::vector<float> volumes;
} played;

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
        detail::set_sound_player({});
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

rmp::Ref<const detail::DrawRecord> record_of(const Clay_RenderCommand &c) {
    const rmp::Ref<detail::DrawRecord> r = detail::draw_record(c);
    return r ? rmp::Ref<const detail::DrawRecord>(*r)
             : rmp::Ref<const detail::DrawRecord>{};
}

rmp::Ref<const detail::TextRecord> text_record_of(const Clay_RenderCommand &c) {
    return detail::text_record(c);
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

// How many records the last pass carried: an element's commands (its fill, its
// border) share one, so each is counted once.
int records() {
    std::vector<const void *> seen;
    const auto count = [&seen](const auto &record) {
        if (record && std::ranges::find(seen, &*record) == seen.end())
            seen.push_back(&*record);
    };
    for (const Clay_RenderCommand &c : commands()) {
        count(record_of(c));
        count(text_record_of(c));
    }
    return static_cast<int>(seen.size());
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
    data.tags = { rmp::SheetTag{ "spin", 0, 1, false, false } };
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
            for (const ui::Style &s :
                 { t.styles.button, t.styles.label, t.styles.image, t.styles.panel,
                   t.styles.checkbox, t.styles.slider, t.styles.dropdown,
                   t.styles.text_input, t.styles.progress }) {
                CHECK_FALSE(s.background);
                CHECK_FALSE(s.skin);
                CHECK_FALSE(s.shadow);
                CHECK_FALSE(s.motion);
                CHECK_FALSE(s.font.valid());
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
            const detail::LookScope scope(ui::WidgetKind::BUTTON,
                                          rmp::Ref<const ui::Style>(s));
            const ui::Theme &t = ui::current_theme();
            CHECK(&*detail::look_theme() == &t);
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
        const detail::LookScope scope(ui::WidgetKind::BUTTON,
                                      rmp::Ref<const ui::Style>(s));
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
            const detail::LookScope scope(ui::WidgetKind::BUTTON, {});
            CHECK(same(detail::look().text.value_or(BLANK), RED));
            CHECK(detail::look().sounds.click.value_or("") == "tick");
        }
        {
            // Theme::styles is per kind: a label does not get the buttons'.
            const detail::LookScope scope(ui::WidgetKind::LABEL, {});
            CHECK_FALSE(detail::look().text);
            CHECK_FALSE(detail::look().radius);
        }
        detail::push_children(ui::Style{ .text = GREEN, .sounds = { .hover = "" } });
        detail::push_children(ui::Style{ .gap = 9 });
        {
            const detail::LookScope scope(ui::WidgetKind::BUTTON, {});
            CHECK(same(detail::look().text.value_or(BLANK),
                       GREEN)); // the outer container's
            CHECK(detail::look().gap.value_or(-1) == 9); // the inner one's
            CHECK(detail::look().radius.value_or(-1) == 3); // the theme's, nobody else's
            // silenced by a container
            CHECK(detail::look().sounds.hover.value_or("x").empty());
            CHECK(detail::look().sounds.click.value_or("") == "tick");
        }
        {
            const ui::Style own{ .text = BLUE };
            const detail::LookScope scope(ui::WidgetKind::BUTTON,
                                          rmp::Ref<const ui::Style>(own));
            CHECK(same(detail::look().text.value_or(BLANK), BLUE));
        }
        detail::pop_children();
        detail::pop_children();
        detail::pop_children(); // one too many does nothing
        {
            const detail::LookScope scope(ui::WidgetKind::BUTTON, {});
            CHECK(same(detail::look().text.value_or(BLANK), RED));
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
            if (const auto r = record_of(c)) shadows += r->has_shadow ? 1 : 0;
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
        rmp::Ref<const detail::DrawRecord> r;
        for (const Clay_RenderCommand &c : commands())
            if (record_of(c)) r = record_of(c);
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
        const auto t = text_record_of(*chapter);
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

rmp::Font counting_bake(const std::string & /*name*/, int pixel_size,
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
            const auto r = record_of(c);
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
                    if (const auto r = record_of(c)) return r;
            }
            return rmp::Ref<const detail::DrawRecord>{};
        };
        frame([&] { play.show(); });
        for (const Clay_RenderCommand &c : commands())
            CHECK(c.commandType != CLAY_RENDER_COMMAND_TYPE_RECTANGLE);
        auto r = skin_record();
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

    TEST_CASE("image_button() with no id has no name, and is still a control") {
        // Its focus name is an empty view, with no pointer behind it: the focus
        // list copies it without reading from null (UBSan said so once).
        const HeadlessUi headless;
        const FakeTexture gear(22, 32, 32);
        int clicks = 0;
        const auto draw = [&] { clicks += ui::image_button(gear.handle()) ? 1 : 0; };
        frame(draw);
        int pictures = 0;
        for (const Clay_RenderCommand &c : commands())
            pictures += c.commandType == CLAY_RENDER_COMMAND_TYPE_IMAGE ? 1 : 0;
        CHECK(pictures == 1);
        keys.next.activate = true; // the first control of a screen has the focus
        frame(draw);
        frame(draw);
        CHECK(clicks == 1);
        CHECK(heard().back().name.empty());
    }

    TEST_CASE("a ProgressBar's fill can be a picture, stretched with the bar") {
        const HeadlessUi headless;
        const FakeTexture fill(31, 16, 8);
        ui::ProgressBar hp{ .value = 0.5f, .width = 200, .fill_picture = fill.handle() };
        frame([&] { hp.show(); });
        bool found = false;
        for (const Clay_RenderCommand &c : commands()) {
            const auto r = record_of(c);
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
            const auto r = record_of(c);
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
                               calls.emplace_back(v ? "on" : "off");
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
                if (const auto r = record_of(c)) moved = moved || r->offset.x != 0;
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
                if (const auto r = record_of(c)) {
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
                if (const auto r = record_of(c)) return r->scale;
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

    TEST_CASE("a SoundName is a string, and the first one written brings the player") {
        const HeadlessUi headless;
        CHECK_FALSE(detail::has_sound_player());
        const ui::SoundName none;
        CHECK(none.empty());
        CHECK_FALSE(detail::has_sound_player()); // no name, nothing to play
        const ui::SoundName click = "click";
        CHECK(detail::has_sound_player()); // rmp::audio::play(), from sound.cpp
        CHECK(click == "click");
        CHECK(click.view() == "click");
        CHECK_FALSE(click.empty());
        const std::string kept = "chime";
        const ui::SoundName from_string = kept;
        const ui::SoundName from_view = std::string_view("bell");
        CHECK(from_string == "chime");
        CHECK(from_view == "bell");
        // NOLINTNEXTLINE(modernize-avoid-c-arrays): what the constructor takes
        constexpr char UNTERMINATED[3] = { 'a', 'b', 'c' };
        CHECK(ui::SoundName(UNTERMINATED) == "abc"); // an array with no NUL: all of it
        const ui::SoundName silence = "";
        CHECK(silence.empty());
    }

    TEST_CASE("the player a SoundName brings is rmp::audio::play()") {
        const HeadlessUi headless;
        rmp::audio::detail::reset_for_tests();
        audio_opens.count = 0;
        rmp::audio::detail::set_device_opener([] {
            ++audio_opens.count;
            return false; // no device: play() asks for one, then plays nothing
        });
        ui::Theme t = ui::theme_dark();
        t.sounds.click = "click"; // the first name: sound.cpp's player goes in
        ui::set_theme(t);
        REQUIRE(detail::has_sound_player());
        const auto draw = [] { ui::button("Play"); };
        frame(draw);
        keys.next.activate = true; // the first button has the focus
        frame(draw);
        frame(draw);
        CHECK(count(ui::EventKind::CLICK) == 1);
        CHECK(audio_opens.count == 1); // rmp::audio::play("click") asked for the device
        rmp::audio::detail::reset_for_tests();
    }

    TEST_CASE("an event plays its sound through the player, at the look's volume") {
        const HeadlessUi headless;
        played.names.clear();
        played.volumes.clear();
        detail::set_sound_player([](std::string_view name, float volume) {
            played.names.emplace_back(name);
            played.volumes.push_back(volume);
        });
        ui::Theme t = ui::theme_dark();
        t.sounds.click = "click"; // the test's player stays: one was there already
        t.sounds.hover = "over";
        t.sounds.volume = 0.5f;
        ui::set_theme(t);
        ui::Button quiet{ .text = "Quiet", .style = { .sounds = { .click = "" } } };
        const auto draw = [&] {
            ui::button("Play");
            quiet.show();
        };
        frame(draw);
        pointer.position = middle_of("Play");
        frame(draw);
        pointer.down = true;
        frame(draw);
        pointer.down = false;
        frame(draw);
        CHECK(std::ranges::count(played.names, "over") == 1);
        CHECK(std::ranges::count(played.names, "click") == 1);
        for (const float v : played.volumes) CHECK(v == doctest::Approx(0.5f));
        // The quiet one: hovered, it sounds; clicked, it is silent.
        played.names.clear();
        pointer.position = middle_of("Quiet");
        frame(draw);
        pointer.down = true;
        frame(draw);
        pointer.down = false;
        frame(draw);
        CHECK(std::ranges::count(played.names, "over") == 1);
        CHECK(std::ranges::count(played.names, "click") == 0);
        // A volume that is no number is said once and plays at 1.
        t.sounds.volume = std::numeric_limits<float>::quiet_NaN();
        ui::set_theme(t);
        played.volumes.clear();
        pointer.position = middle_of("Play");
        frame(draw);
        REQUIRE_FALSE(played.volumes.empty());
        CHECK(played.volumes.back() == 1.0f);
    }

    TEST_CASE("a theme's sounds are named per event, and an empty one is silence") {
        const HeadlessUi headless;
        ui::Theme t = ui::theme_dark();
        t.sounds.click = "click";
        ui::set_theme(t);
        const ui::Style quiet{ .sounds = { .click = "" } };
        const detail::LookScope a(ui::WidgetKind::BUTTON,
                                  rmp::Ref<const ui::Style>(quiet));
        CHECK(detail::look().sounds.click.value_or("x").empty());
    }
}

// ---------------------------------------------------------------------------

TEST_SUITE("ui: the arithmetic of the looks") {
    TEST_CASE("a picture is one part stretched, or nine with their corners kept") {
        detail::DrawRecord r;
        r.source = Rectangle{ 0, 0, 24, 24 };
        detail::PictureParts parts{};
        CHECK(detail::picture_parts(r, Rectangle{ 10, 10, 100, 50 }, parts) == 1);
        CHECK(parts[0].dest.width == 100);
        CHECK(detail::picture_parts(r, Rectangle{ 0, 0, 0, 50 }, parts) == 0);

        r.left = r.top = r.right = r.bottom = 8;
        r.slice_px = 2; // a source pixel is two on screen
        REQUIRE(detail::picture_parts(r, Rectangle{ 0, 0, 100, 60 }, parts) == 9);
        CHECK(parts[0].source.width == 8); // the top-left corner, from the source
        CHECK(parts[0].dest.width == 16); // drawn at twice its size
        CHECK(parts[1].dest.width == 100 - 32); // the top edge, stretched along
        CHECK(parts[4].source.x == 8); // the middle
        CHECK(parts[8].dest.x == 100 - 16); // the bottom-right corner, at the far edge

        // Too small a box for the corners: they shrink together, and the edges
        // and the middle, of no size, are left out.
        const int n = detail::picture_parts(r, Rectangle{ 0, 0, 20, 20 }, parts);
        CHECK(n == 4);
        CHECK(parts[0].dest.width == doctest::Approx(10));
    }

    TEST_CASE("a shadow is six rounded layers, biggest first, or one hard copy") {
        detail::DrawRecord r;
        r.shadow_color = Color{ 0, 0, 0, 120 };
        r.shadow_offset = Vector2{ 0, 4 };
        r.radius = 4;
        const Clay_BoundingBox box{ 10, 10, 100, 40 };
        detail::ShadowLayers layers{};
        CHECK(detail::shadow_layers(r, box, 1.0f, layers) == 1); // no blur: one copy
        CHECK(layers[0].rect.y == 14);
        CHECK(layers[0].color.a == 120);
        r.shadow_blur = 12;
        REQUIRE(detail::shadow_layers(r, box, 1.0f, layers) == detail::SHADOW_LAYERS);
        CHECK(layers[0].rect.width > layers[5].rect.width);
        CHECK(layers[0].radius > layers[5].radius);
        CHECK(layers[0].color.a == 20); // 120 shared out
        detail::shadow_layers(r, box, 0.5f, layers);
        CHECK(layers[0].color.a == 10); // faded with what it is under
        r.shadow_blur = 0;
        r.shadow_spread = -100; // a shadow smaller than nothing is none
        CHECK(detail::shadow_layers(r, box, 1.0f, layers) == 0);
    }

    TEST_CASE("an outline is eight copies around a letter, sixteen past two pixels") {
        detail::OutlineOffsets o{};
        CHECK(detail::outline_offsets(1.0f, o) == 8);
        CHECK(o[0].x == doctest::Approx(1.0f));
        CHECK(o[2].y == doctest::Approx(1.0f));
        CHECK(detail::outline_offsets(3.0f, o) == 16);
        for (int i = 0; i < 16; ++i)
            CHECK(std::hypot(o[static_cast<size_t>(i)].x, o[static_cast<size_t>(i)].y) ==
                  doctest::Approx(3.0f));
    }

    TEST_CASE("faded, inside, and the records told from anything else") {
        CHECK(same(detail::faded(RED, 1.0f), RED));
        CHECK(detail::faded(RED, 0.5f).a == 128);
        CHECK(detail::faded(RED, -1.0f).a == 0);
        CHECK(detail::inside(Clay_BoundingBox{ 1, 1, 5, 5 },
                             Clay_BoundingBox{ 0, 0, 10, 10 }));
        CHECK(detail::inside(Clay_BoundingBox{ -0.4f, 0, 10.4f, 10 },
                             Clay_BoundingBox{ 0, 0, 10, 10 }));
        CHECK_FALSE(detail::inside(Clay_BoundingBox{ 8, 8, 5, 5 },
                                   Clay_BoundingBox{ 0, 0, 10, 10 }));
        detail::DrawRecord draw;
        detail::TextRecord text;
        std::array<uint32_t, 4> other{ 1, 2, 3, 4 };
        Clay_RenderCommand box{};
        box.commandType = CLAY_RENDER_COMMAND_TYPE_RECTANGLE;
        Clay_RenderCommand words{};
        words.commandType = CLAY_RENDER_COMMAND_TYPE_TEXT;
        box.userData = &draw;
        REQUIRE(detail::draw_record(box));
        CHECK(&*detail::draw_record(box) == &draw);
        CHECK_FALSE(detail::text_record(box)); // a record only a text carries
        box.userData = other.data();
        CHECK_FALSE(detail::draw_record(box));
        box.userData = nullptr;
        CHECK_FALSE(detail::draw_record(box));
        words.userData = &text;
        REQUIRE(detail::text_record(words));
        CHECK(&*detail::text_record(words) == &text);
        CHECK_FALSE(detail::draw_record(words)); // a text's record is no DrawRecord
        words.userData = other.data();
        CHECK_FALSE(detail::text_record(words));
        words.userData = nullptr;
        CHECK_FALSE(detail::text_record(words));
    }

    TEST_CASE("what moves pushes a matrix while its box holds what is drawn") {
        detail::Motions m;
        const detail::DrawRecord still;
        CHECK_FALSE(m.enter(Clay_BoundingBox{ 0, 0, 100, 100 }, still)); // nothing moves
        CHECK(m.depth() == 0);
        detail::DrawRecord grows;
        grows.scale = 1.1f;
        grows.alpha = 0.5f;
        CHECK(m.enter(Clay_BoundingBox{ 0, 0, 100, 100 }, grows));
        CHECK(m.alpha() == 0.5f);
        detail::DrawRecord fades;
        fades.alpha = 0.5f;
        CHECK_FALSE(
            m.enter(Clay_BoundingBox{ 10, 10, 20, 20 }, fades)); // no matrix: only a fade
        CHECK(m.depth() == 2);
        CHECK(m.alpha() == 0.25f);
        CHECK(m.matrices() == 1);
        CHECK(m.leave(Clay_BoundingBox{ 12, 12, 5, 5 }) == 0); // still inside both
        CHECK(m.leave(Clay_BoundingBox{ 50, 50, 5, 5 }) == 0); // out of the fade only
        CHECK(m.depth() == 1);
        CHECK(m.leave(Clay_BoundingBox{ 500, 500, 5, 5 }) == 1); // out of the growing one
        CHECK(m.depth() == 0);
        CHECK(m.alpha() == 1.0f);
        for (int i = 0; i < detail::MAX_MOTIONS + 3; ++i)
            m.enter(Clay_BoundingBox{ 0, 0, 10, 10 }, grows);
        CHECK(m.depth() == detail::MAX_MOTIONS);
    }
}

// ---------------------------------------------------------------------------

TEST_SUITE("ui: every kept widget's state and shake") {
    TEST_CASE("each kept widget answers what it is doing, and shakes") {
        const HeadlessUi headless;
        const FakeTexture art(61, 16, 16);
        ui::Label label{ .text = "label" };
        ui::Image image{ .picture = art.handle(), .id = "image" };
        ui::Panel panel{ .id = "panel" };
        ui::Checkbox box{ .text = "box" };
        ui::Slider slider{ .text = "slider" };
        ui::Dropdown drop{ .text = "drop", .items = { "a" } };
        ui::TextInput field{ .text = "field" };
        ui::ProgressBar bar{ .value = 0.5f, .width = 100, .id = "bar" };
        label.shake();
        image.shake();
        panel.shake();
        box.shake();
        slider.shake();
        drop.shake();
        field.shake();
        bar.shake();
        const auto draw = [&] {
            label.show();
            image.show();
            panel.show([] {});
            box.show();
            slider.show();
            drop.show();
            field.show();
            bar.show();
        };
        frame(draw);
        frame(draw);
        CHECK(count(ui::EventKind::SHAKE) == 8);
        CHECK_FALSE(label.hovered());
        CHECK_FALSE(image.hovered());
        CHECK_FALSE(image.pressed());
        CHECK_FALSE(panel.hovered());
        CHECK_FALSE(panel.pressed());
        CHECK_FALSE(box.hovered());
        CHECK_FALSE(slider.hovered());
        CHECK_FALSE(drop.hovered());
        CHECK_FALSE(field.hovered());
        CHECK_FALSE(bar.hovered());
        pointer.position = middle_of("bar");
        frame(draw);
        CHECK(bar.hovered());
        // The focus: the first control of the screen takes it.
        const int focused = (box.focused() ? 1 : 0) + (slider.focused() ? 1 : 0) +
            (drop.focused() ? 1 : 0) + (field.focused() ? 1 : 0);
        CHECK(focused <= 1);
    }

    TEST_CASE("a kept Image plays its sprite's animation") {
        const HeadlessUi headless;
        ui::Image spinner{ .id = "spinner" };
        spinner.sprite.sheet = fake_sheet();
        spinner.sprite.play("spin");
        // Frames of 0.1 s: one step of 0.11 s is on the second frame, a second
        // step loops back to the first.
        detail::set_test_frame_time(0.11f);
        frame([&] { spinner.show(); });
        CHECK(spinner.sprite.frame_index() == 1);
        frame([&] { spinner.show(); });
        CHECK(spinner.sprite.frame_index() == 0);
    }
}

// ---------------------------------------------------------------------------

TEST_SUITE("ui: the looks of the immediate widgets") {
    TEST_CASE("image() of a Picture or an rmp::Texture, with and without options") {
        const HeadlessUi headless;
        const FakeTexture art(71, 20, 10);
        frame([&] {
            ui::image(ui::Picture(art.handle()));
            ui::image(art.handle(), { .width = 40 });
        });
        int images = 0;
        for (const Clay_RenderCommand &c : commands())
            images += c.commandType == CLAY_RENDER_COMMAND_TYPE_IMAGE ? 1 : 0;
        CHECK(images == 2);
    }

    TEST_CASE("an image with a style: tinted, framed by a skin, shadowed") {
        const HeadlessUi headless;
        const FakeTexture art(72, 16, 16);
        const FakeTexture frame_art(73, 24, 24);
        ui::Image framed{ .picture = art.handle(),
                          .id = "framed",
                          .style = { .skin = ui::Skin{ .normal = frame_art.handle() },
                                     .tint = Color{ 128, 128, 128, 255 },
                                     .padding_x = 4,
                                     .padding_y = 4 } };
        frame([&] { framed.show(); });
        std::vector<unsigned> ids;
        for (const Clay_RenderCommand &c : commands())
            if (const auto r = record_of(c)) ids.push_back(r->raw.id);
        CHECK(std::ranges::find(ids, 73u) != ids.end()); // the frame
        CHECK(std::ranges::find(ids, 72u) != ids.end()); // the picture inside it
        for (const Clay_RenderCommand &c : commands()) {
            const auto r = record_of(c);
            if (r != nullptr && r->raw.id == 72) CHECK(r->tint.r == 128);
        }
    }

    TEST_CASE("a panel's border: its own, its style's, the light theme's") {
        const HeadlessUi headless;
        const auto borders = [] {
            int n = 0;
            for (const Clay_RenderCommand &c : commands())
                n += c.commandType == CLAY_RENDER_COMMAND_TYPE_BORDER ? 1 : 0;
            return n;
        };
        frame([] { ui::panel({ .border = RED, .border_width = 2 }, [] {}); });
        CHECK(borders() == 1);
        ui::Panel styled{ .style = { .border = BLUE, .border_width = 3 } };
        frame([&] { styled.show([] {}); });
        CHECK(borders() == 1);
        ui::set_theme(ui::theme_light());
        frame([] { ui::panel([] {}); });
        CHECK(borders() == 1);
        ui::Theme t = ui::theme_dark();
        t.styles.panel.shadow = ui::Shadow{};
        ui::set_theme(t);
        frame([] { ui::panel([] {}); });
        CHECK(records() == 1); // Theme::styles.panel: a shadow, through a record
    }

    TEST_CASE(
        "a progress bar with a style: its border, its shadow; and without one, none") {
        const HeadlessUi headless;
        frame([] { ui::progress(0.5f, { .id = "plain" }); });
        CHECK(records() == 0);
        ui::Theme t = ui::theme_dark();
        t.styles.progress = { .border = WHITE,
                              .border_width = 1,
                              .shadow = ui::Shadow{} };
        ui::set_theme(t);
        frame([] { ui::progress(0.5f); });
        CHECK(records() == 1);
        int borders = 0;
        for (const Clay_RenderCommand &c : commands())
            borders += c.commandType == CLAY_RENDER_COMMAND_TYPE_BORDER ? 1 : 0;
        CHECK(borders == 1);
    }

    TEST_CASE("only a hover or a press colour, without a background, is used alone") {
        const HeadlessUi headless;
        const ui::Style s{ .background_hover = GOLD, .background_press = MAROON };
        const detail::LookScope scope(ui::WidgetKind::BUTTON,
                                      rmp::Ref<const ui::Style>(s));
        CHECK(same(ui::current_theme().surface_hover, GOLD));
        CHECK(same(ui::current_theme().surface_press, MAROON));
        CHECK(same(ui::current_theme().surface, ui::theme_dark().surface));
        CHECK(detail::look_kind() == ui::WidgetKind::BUTTON);
    }

    TEST_CASE("outside any widget the look is empty") {
        const HeadlessUi headless;
        CHECK_FALSE(detail::look().background);
        CHECK_FALSE(detail::look().motion);
        CHECK(detail::look_kind() == ui::WidgetKind::BUTTON);
        CHECK(detail::look_theme() == nullptr);
    }

    TEST_CASE("a style of numbers that are none is said once, and drawn as none") {
        const HeadlessUi headless;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const ui::Style s{ .font_size = nan, .motion = ui::Motion{ .hover_scale = nan } };
        {
            const detail::LookScope scope(ui::WidgetKind::BUTTON,
                                          rmp::Ref<const ui::Style>(s));
            CHECK(ui::current_theme().font_size == ui::theme_dark().font_size);
        }
        CHECK(rmp::detail::report_count() == 1);
        ui::Button b{ .text = "b", .style = s };
        frame([&] { b.show(); });
        CHECK(rmp::detail::report_count() == 2); // the motion's, once
    }

    TEST_CASE("a skin with no press picture is the normal one, darker, while held") {
        const HeadlessUi headless;
        const FakeTexture art(74, 24, 24);
        ui::Button b{ .text = "Hold",
                      .style = { .skin = ui::Skin{ .normal = art.handle() } } };
        const auto draw = [&] { b.show(); };
        frame(draw);
        pointer.position = middle_of("Hold");
        frame(draw);
        pointer.down = true;
        frame(draw);
        for (const Clay_RenderCommand &c : commands()) {
            const auto r = record_of(c);
            if (r != nullptr && r->has_picture) {
                CHECK(r->raw.id == 74);
                CHECK(r->tint.r < 255);
            }
        }
    }

    TEST_CASE("a picture's part that is no rectangle is the whole texture, said once") {
        const HeadlessUi headless;
        const FakeTexture art(75, 30, 20);
        const float inf = std::numeric_limits<float>::infinity();
        frame([&] { ui::image(ui::Picture(art.handle(), Rectangle{ inf, 0, 5, 5 })); });
        for (const Clay_RenderCommand &c : commands()) {
            if (const auto r = record_of(c)) CHECK(r->source.width == 30);
        }
        CHECK(rmp::detail::report_count() == 1);
    }
}

// ---------------------------------------------------------------------------

// What the font provider of "ui: fonts by id" was asked for.
struct {
    std::vector<std::string> asked;
} font_log;

TEST_SUITE("ui: fonts by id") {
    TEST_CASE(
        "a style's font is baked from its own file, and one that fails is said once") {
        const HeadlessUi headless;
        detail::set_measure_provider(detail::measure_with_raylib);
        font_log.asked.clear();
        const std::vector<std::string> &asked = font_log.asked;
        detail::set_font_provider(
            [](const std::string &name, int size, std::span<const int>) {
                font_log.asked.push_back(name);
                if (name == "broken.ttf") return rmp::Font{};
                ::Font face{};
                face.baseSize = size;
                face.glyphCount = 1;
                return rmp::Font{ rmp::detail::adopt(rmp::detail::ResourceKind::FONT,
                                                     face) };
            },
            "global.ttf");
        const rmp::Font title{ rmp::detail::adopt_named(rmp::detail::ResourceKind::FONT,
                                                        "title.ttf", 30, ::Font{}) };
        const rmp::Font broken{ rmp::detail::adopt_named(rmp::detail::ResourceKind::FONT,
                                                         "broken.ttf", 30, ::Font{}) };
        ui::Label a{ .text = "a", .style = { .font = title } };
        ui::Label b{ .text = "b", .style = { .font = broken } };
        frame([&] {
            a.show();
            b.show();
            ui::text("c");
        });
        CHECK(std::ranges::find(asked, "title.ttf") != asked.end());
        CHECK(std::ranges::find(asked, "global.ttf") != asked.end());
        CHECK(std::ranges::count(asked, "broken.ttf") == 1);
        frame([&] { b.show(); });
        CHECK(std::ranges::count(asked, "broken.ttf") == 1); // not asked again
        CHECK(rmp::detail::report_count() == 1);
        detail::set_font_provider(nullptr, nullptr);
    }

    TEST_CASE("past the most letters a font is baked with, the rest are said once") {
        const HeadlessUi headless;
        detail::set_measure_provider(detail::measure_with_raylib);
        detail::set_font_provider(
            [](const std::string &, int size, std::span<const int>) {
                ::Font face{};
                face.baseSize = size;
                face.glyphCount = 1;
                return rmp::Font{ rmp::detail::adopt(rmp::detail::ResourceKind::FONT,
                                                     face) };
            },
            "many.ttf");
        std::string text;
        for (int c = 0x4E00; c < 0x4E00 + 8400; ++c) {
            // UTF-8 of a CJK ideograph: three bytes.
            text += static_cast<char>(0xE0 | (c >> 12));
            text += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            text += static_cast<char>(0x80 | (c & 0x3F));
        }
        frame([&] { ui::text(text); });
        CHECK(rmp::detail::report_count() >= 1);
        detail::set_font_provider(nullptr, nullptr);
    }

    TEST_CASE("load_font_glyphs() refuses what load_font() refuses") {
        const int before = rmp::assets::failed_loads();
        const std::array<int, 3> letters{ 'a', 'b', 0xF1 };
        CHECK_FALSE(rmp::assets::detail::load_font_glyphs("", 20, letters).valid());
        CHECK_FALSE(rmp::assets::detail::load_font_glyphs("ui.ttf", 0, letters).valid());
        CHECK_FALSE(
            rmp::assets::detail::load_font_glyphs("ui.ttf", 5000, letters).valid());
        CHECK_FALSE(rmp::assets::detail::load_font_glyphs("missing-font.ttf", 20, letters)
                        .valid());
        CHECK(rmp::assets::failed_loads() > before);
        rmp::detail::reset_reports_for_tests();
    }
}
