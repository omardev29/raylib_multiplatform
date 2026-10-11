// ---------------------------------------------------------------------------
// rmp::ui's text, measured as it is drawn.
//
// Clay lays a text out from measurements of its PIECES -- each word, and a
// space measured once on its own (thirdparty/clay/clay.h,
// Clay__MeasureTextCached) -- and render.cpp draws each line it ends up with
// in ONE DrawTextEx(), which puts letter_spacing() between every two letters
// of the line. So the pieces have to add up to the line: a measurement that
// counts the spacing only inside a piece loses the two around every space, and
// a menu's two-word line ran 29 pixels past the buttons under it. Held here:
//
//   - a line laid out without wrapping is as wide as DrawTextEx() draws it,
//     to float rounding, for any words, spaces, UTF-8 (broken too) and font:
//     a line that starts with a space by one letter spacing at most, and
//     never less (measure_text() in src/rmp/ui/context.cpp says why);
//   - wrapped into any width, no line is drawn wider than the width its
//     command says it has, nor than the box it was wrapped into -- unless it
//     is one word wider than the box, which Clay puts on a line of its own
//     and the box grows to (with a spacing: measure_text() says why);
//   - text with a NUL inside is drawn no wider than its box either: the
//     draw stops at the NUL and the measurement may count past it;
//   - measure_text() reads nothing outside the slice it is handed (ASan);
//   - and the oracle below is raylib's own MeasureTextEx() wherever that
//     agrees with DrawTextEx(): a font whose glyphs either advance or have no
//     offset, one line. Where they disagree -- a glyph with no advance and an
//     offset -- DrawTextEx() is what is on screen, and is the oracle.
//
// The fonts are made here, not baked: baking needs a GL context. Each is the
// printable ASCII and three more characters, at a random base size, each glyph
// of random width, advance (none, sometimes: raylib's own font advances by
// its rectangle) and offset, the way a real face varies.
//
// What any input does to text() and the rest of the interface -- NaN, the
// infinities, sizes past what a pixel count holds -- is ui_prop.cpp's.
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"

#include "../../src/rmp/scene_internal.h"
#include "../../src/rmp/ui/internal.h"

#include <rmp/app.h>
#include <rmp/ui.h>

#include <doctest.h>
#include <rapidcheck.h>
#include <raylib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Past the printable ASCII: é, € and a four-byte one (a gamepad).
constexpr std::array EXTRA_CODEPOINTS{ 0xE9, 0x20AC, 0x1F3AE };

// A font as raylib holds one, and the storage its pointers point into.
struct FakeFont {
    int base = 10;
    std::vector<GlyphInfo> glyphs;
    std::vector<Rectangle> recs;

    [[nodiscard]] ::Font font() const {
        ::Font f{};
        f.baseSize = base;
        f.glyphCount = static_cast<int>(glyphs.size());
        f.glyphPadding = 0;
        // Never drawn with: a texture id is only what DrawTextEx() and
        // measure_text() take for "a font that was loaded".
        f.texture.id = 1;
        // raylib's struct, whose fields are pointers; the vectors outlive it.
        f.recs = const_cast<Rectangle *>(
            recs.data()); // NOLINT(cppcoreguidelines-pro-type-const-cast)
        f.glyphs = const_cast<GlyphInfo *>(
            glyphs.data()); // NOLINT(cppcoreguidelines-pro-type-const-cast)
        return f;
    }
};

std::ostream &operator<<(std::ostream &os, const FakeFont &f) {
    os << "font base " << f.base << ":";
    for (std::size_t i = 0; i < f.glyphs.size(); ++i) {
        os << " " << f.glyphs[i].value << "(w" << f.recs[i].width << " a"
           << f.glyphs[i].advanceX << " o" << f.glyphs[i].offsetX << ")";
    }
    return os;
}

// `offsets`: whether a glyph may have an offset at all. With none, raylib's
// MeasureTextEx() and DrawTextEx() agree.
rc::Gen<FakeFont> fake_font(bool offsets) {
    return rc::gen::exec([offsets] {
        FakeFont f;
        // raylib's own font is 10, and drawn at 20 it is twice its size.
        f.base = *rc::gen::weightedOneOf<int>(
            { { 1, rc::gen::just(10) }, { 2, rc::gen::inRange(4, 65) } });
        std::vector<int> codepoints;
        for (int c = 32; c < 127; ++c) codepoints.push_back(c);
        codepoints.insert(codepoints.end(), EXTRA_CODEPOINTS.begin(),
                          EXTRA_CODEPOINTS.end());
        for (const int c : codepoints) {
            GlyphInfo g{};
            g.value = c;
            const int width = *rc::gen::inRange(0, 3 * f.base + 1);
            g.advanceX = *rc::gen::weightedOneOf<int>(
                { { 1, rc::gen::just(0) }, { 3, rc::gen::inRange(1, 3 * f.base + 1) } });
            g.offsetX = offsets ? *rc::gen::weightedOneOf<int>(
                                      { { 2, rc::gen::just(0) },
                                        { 1, rc::gen::inRange(-f.base, f.base + 1) } })
                                : 0;
            f.glyphs.push_back(g);
            f.recs.push_back(
                Rectangle{ 0, 0, static_cast<float>(width), static_cast<float>(f.base) });
        }
        return f;
    });
}

// One piece of a line: a word of printable ASCII, a space or two, UTF-8 in
// the font and out of it ('?' is drawn), and bytes no UTF-8 is made of.
rc::Gen<std::string> piece() {
    return rc::gen::weightedOneOf<std::string>({
        { 6, rc::gen::container<std::string>(rc::gen::inRange<char>('!', '\x7f')) },
        { 4, rc::gen::just(std::string(" ")) },
        { 1, rc::gen::just(std::string("  ")) },
        { 2,
          rc::gen::element<std::string>("\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x8E\xAE",
                                        "\xC3\xA8", "\x80", "\xC3", "\xFF") },
    });
}

rc::Gen<std::string> line_of(rc::Gen<std::string> pieces) {
    return rc::gen::map(rc::gen::container<std::vector<std::string>>(std::move(pieces)),
                        [](const std::vector<std::string> &parts) {
                            std::string text;
                            for (const std::string &part : parts) text += part;
                            return text;
                        });
}

rc::Gen<std::string> text_line() { return line_of(piece()); }

// Lines, a newline between them.
rc::Gen<std::string> text_lines() {
    return line_of(rc::gen::weightedOneOf<std::string>(
        { { 8, piece() }, { 1, rc::gen::just(std::string("\n")) } }));
}

// How far DrawTextEx() moves its pen across `line` (raylib's rtext.c, letter
// for letter), less the spacing after the last letter: the width of what it
// draws. render.cpp hands it letter_spacing(size), which is size / 10, and
// the line as cstr() copies it -- up to a NUL.
float drawn_width(const ::Font &f, std::string_view line, float size) {
    const std::string text(line.substr(0, line.find('\0')));
    const float scale = size / static_cast<float>(f.baseSize);
    const float spacing = size / 10.0f;
    float pen = 0;
    int letters = 0;
    for (std::size_t i = 0; i < text.size();) {
        int bytes = 0;
        const int codepoint = GetCodepointNext(&text[i], &bytes);
        const auto index = static_cast<std::size_t>(GetGlyphIndex(f, codepoint));
        if (f.glyphs[index].advanceX == 0) {
            pen += f.recs[index].width * scale + spacing;
        } else {
            pen += static_cast<float>(f.glyphs[index].advanceX) * scale + spacing;
        }
        ++letters;
        i += static_cast<std::size_t>(bytes);
    }
    return letters > 0 ? pen - spacing : 0;
}

// Float rounding: the pieces are added in another order than the letters.
float tolerance(float width) { return 1e-3f + 1e-4f * std::fabs(width); }

// The font the layout measures with, for the measurement provider below.
struct {
    ::Font face{};
} typeset;

// Clay's MeasureFn: what measure_with_raylib() does, with the font made here
// in place of ui_font()'s.
// NOLINTBEGIN(custom-rmp-raw-pointer): Clay's signature, not ours to choose
Clay_Dimensions measure_typeset(Clay_StringSlice text, Clay_TextElementConfig *config,
                                void * /*user*/) {
    // NOLINTEND(custom-rmp-raw-pointer)
    return rmp::ui::detail::measure_text(typeset.face, text,
                                         static_cast<float>(config->fontSize));
}

void pointer_away(Clay_Vector2 &position, bool &down) {
    position = Clay_Vector2{ -1.0f, -1.0f };
    down = false;
}

void no_nav(rmp::ui::detail::NavState &out) { out = rmp::ui::detail::NavState{}; }

// The UI with no window: a viewport given, the pointer away, no keys.
struct HeadlessText {
    explicit HeadlessText(const ::Font &face) {
        rmp::scenes::detail::shutdown();
        rmp::app::set_stretch(rmp::app::Stretch::NONE);
        typeset.face = face;
        rmp::ui::detail::set_measure_provider(measure_typeset);
        rmp::ui::detail::set_pointer_provider(pointer_away);
        rmp::ui::detail::set_nav_provider(no_nav);
        rmp::ui::detail::set_test_viewport(VIEW, VIEW);
    }
    HeadlessText(const HeadlessText &) = delete;
    HeadlessText &operator=(const HeadlessText &) = delete;
    ~HeadlessText() {
        rmp::ui::set_scale(0);
        rmp::ui::detail::set_measure_provider(rmp::ui::detail::measure_with_raylib);
        rmp::ui::detail::set_pointer_provider(rmp::ui::detail::pointer_from_raylib);
        rmp::ui::detail::set_nav_provider(rmp::ui::detail::nav_from_raylib);
        rmp::ui::detail::set_test_viewport(0, 0);
        typeset.face = ::Font{};
    }
    // Wide and tall enough for any line here at any size: nothing is culled
    // and nothing has to shrink.
    static constexpr float VIEW = 1.0e6f;
};

// A line Clay asked to be drawn: what, at what size, and how wide it says.
struct Line {
    std::string text;
    float size = 0;
    float width = 0;
};

std::vector<Line> drawn_lines() {
    std::vector<Line> out;
    const Clay_RenderCommandArray commands = rmp::ui::detail::last_pass_commands();
    for (int32_t i = 0; i < commands.length; ++i) {
        const Clay_RenderCommand &cmd = commands.internalArray[i];
        if (cmd.commandType != CLAY_RENDER_COMMAND_TYPE_TEXT) continue;
        const Clay_TextRenderData &t = cmd.renderData.text;
        // What render.cpp skips: Clay's line of length -1 (see cstr()).
        if (t.stringContents.length <= 0) continue;
        out.push_back(
            Line{ std::string(t.stringContents.chars,
                              static_cast<std::size_t>(t.stringContents.length)),
                  static_cast<float>(t.fontSize), cmd.boundingBox.width });
    }
    return out;
}

// The size a text is drawn at, in design units: small to large, the scale on top.
rc::Gen<float> design_size() {
    return rc::gen::map(rc::gen::inRange(4, 61),
                        [](int n) { return static_cast<float>(n); });
}
rc::Gen<float> some_scale() {
    return rc::gen::map(rc::gen::inRange(50, 401),
                        [](int n) { return static_cast<float>(n) / 100; });
}

std::string shown(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte >= 0x7f || c == '"' || c == '\\') {
            std::array<char, 8> hex{};
            std::snprintf(hex.data(), hex.size(), "\\x%02X", byte);
            out += hex.data();
        } else {
            out += c;
        }
    }
    return out + "\"";
}

TEST_SUITE("property: ui text") {
    TEST_CASE("a line is laid out as wide as it is drawn") {
        rmp::test::check(
            "ui: a line laid out without wrapping is as wide as DrawTextEx draws it", [] {
                const FakeFont font = *fake_font(true);
                const std::string text = *text_lines();
                const float size = *design_size();
                const float scale = *some_scale();
                const HeadlessText ui(font.font());
                rmp::ui::set_scale(scale);
                for (int frame = 0; frame < 2; ++frame) {
                    rmp::ui::begin();
                    rmp::ui::text(text, { .size = size, .wrap = false });
                    rmp::ui::end();
                }
                for (const Line &line : drawn_lines()) {
                    const float drawn = drawn_width(font.font(), line.text, line.size);
                    RC_LOG() << "line " << shown(line.text) << " at " << line.size
                             << ": laid out " << line.width << ", drawn " << drawn
                             << "\n";
                    RC_ASSERT(line.width >= drawn - tolerance(drawn));
                    // One letter spacing more at most, and only for a line that
                    // starts with a space.
                    const float over =
                        line.text.starts_with(' ') ? line.size / 10.0f : 0.0f;
                    RC_ASSERT(line.width <= drawn + over + tolerance(drawn));
                }
            });
    }

    TEST_CASE("wrapped into any width, no line is drawn past its box") {
        rmp::test::check(
            "ui: no wrapped line is drawn wider than its command or its box", [] {
                const FakeFont font = *fake_font(true);
                const std::string text = *text_lines();
                const float size = *design_size();
                const float box = static_cast<float>(*rc::gen::inRange(20, 2000));
                const HeadlessText ui(font.font());
                rmp::ui::set_scale(1);
                for (int frame = 0; frame < 2; ++frame) {
                    rmp::ui::begin();
                    rmp::ui::column({ .width = box, .id = "probe" }, [&] {
                        rmp::ui::text(text, { .size = size, .wrap = true });
                    });
                    rmp::ui::end();
                }
                Clay_BoundingBox probe{};
                RC_ASSERT(rmp::ui::detail::bounds_of("probe", 0, 0, &probe));
                // A word wider than the box cannot be broken: the text is as wide as
                // that word, and a spacing more (measure_text() says why), and its
                // other lines are wrapped to that.
                float room = probe.width;
                for (const Line &line : drawn_lines()) {
                    std::size_t from = 0;
                    while (from <= line.text.size()) {
                        const std::size_t to =
                            std::min(line.text.find(' ', from), line.text.size());
                        const float word = drawn_width(
                            font.font(), line.text.substr(from, to - from), line.size);
                        room = std::max(room, word + line.size / 10.0f);
                        from = to + 1;
                    }
                }
                for (const Line &line : drawn_lines()) {
                    const float drawn = drawn_width(font.font(), line.text, line.size);
                    RC_LOG() << "line " << shown(line.text) << " at " << line.size
                             << " in " << probe.width << " (room " << room
                             << "): laid out " << line.width << ", drawn " << drawn
                             << "\n";
                    RC_ASSERT(drawn <= line.width + tolerance(drawn));
                    // A word on a line of its own -- the spaces after it go with it --
                    // is drawn whole, whatever the room.
                    const std::size_t last = line.text.find_last_not_of(' ');
                    const bool lone = last == std::string::npos ||
                        line.text.find(' ') == std::string::npos ||
                        line.text.find(' ') > last;
                    if (!lone) RC_ASSERT(drawn <= room + tolerance(drawn));
                }
            });
    }

    TEST_CASE("a NUL inside a text") {
        rmp::test::check("ui: text with a NUL inside is drawn no wider than its box", [] {
            const FakeFont font = *fake_font(true);
            std::string text = *text_line();
            text.insert(*rc::gen::inRange<std::size_t>(0, text.size() + 1), 1, '\0');
            const float size = *design_size();
            const HeadlessText ui(font.font());
            for (int frame = 0; frame < 2; ++frame) {
                rmp::ui::begin();
                rmp::ui::text(text, { .size = size, .wrap = false });
                rmp::ui::end();
            }
            for (const Line &line : drawn_lines()) {
                const float drawn = drawn_width(font.font(), line.text, line.size);
                RC_LOG() << "line " << shown(line.text) << ": laid out " << line.width
                         << ", drawn " << drawn << "\n";
                RC_ASSERT(drawn <= line.width + tolerance(drawn));
            }
        });
    }

    TEST_CASE("measure_text reads only its slice") {
        rmp::test::check(
            "ui: measure_text reads nothing outside the slice it is given", [] {
                const FakeFont font = *fake_font(true);
                const std::string text = *text_lines();
                RC_PRE(!text.empty());
                // Exactly the string's bytes, with no terminator after them: a read
                // past the end is a heap overflow ASan stops at.
                std::vector<char> bytes(text.begin(), text.end());
                const auto start = *rc::gen::inRange<std::size_t>(0, bytes.size());
                const auto length =
                    *rc::gen::inRange<std::size_t>(0, bytes.size() - start + 1);
                const Clay_StringSlice slice{ static_cast<int32_t>(length),
                                              bytes.data() + start, bytes.data() };
                const float size = *design_size();
                const Clay_Dimensions d =
                    rmp::ui::detail::measure_text(font.font(), slice, size);
                RC_ASSERT(std::isfinite(d.width));
                RC_ASSERT(d.width >= 0.0f);
            });
    }

    TEST_CASE("the oracle is raylib's own measurement where it can be") {
        rmp::test::check(
            "ui: DrawTextEx's width here is raylib's MeasureTextEx where they agree", [] {
                const FakeFont font = *fake_font(false);
                std::string text = *text_line();
                text = text.substr(0, text.find('\0'));
                const float size = *design_size();
                const float drawn = drawn_width(font.font(), text, size);
                const Vector2 raylib =
                    MeasureTextEx(font.font(), text.c_str(), size, size / 10.0f);
                RC_LOG() << shown(text) << ": " << drawn << " and " << raylib.x << "\n";
                RC_ASSERT(std::fabs(drawn - raylib.x) <= tolerance(drawn));
            });
    }

} // TEST_SUITE

} // namespace
