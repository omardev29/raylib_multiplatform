// ===========================================================================
// rich_text(): a text with Yarn's markup, laid out and drawn a letter at a
// time. Clay sees one element, as wide as it is given and as tall as the lines
// the words wrap into; render.cpp hands its CUSTOM command back here, and each
// letter is drawn where the layout put it, moved by [wave] and [shake],
// painted by [color] and [rainbow], sized by [size], doubled by [b]. The
// letters past `visible` are not drawn: a typewriter counts them up.
// ===========================================================================

#include "../dialogue/yarn.h"
#include "internal.h"

#include <rmp/ui.h>

#include <raylib.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <list>
#include <numbers>
#include <string>
#include <vector>

namespace rmp::ui {

namespace detail {

namespace {

// What moves or paints a letter: bits of Glyph::effects.
constexpr std::uint8_t NONE = 0;
constexpr std::uint8_t WAVE = 1;
constexpr std::uint8_t SHAKE = 2;
constexpr std::uint8_t RAINBOW = 4;
constexpr std::uint8_t BOLD = 8;

struct Glyph {
    int codepoint = 0;
    Vector2 at{}; // pixels, from the element's top left
    float size = 0; // pixels
    Color color = WHITE;
    std::uint8_t effects = NONE;
    int index = 0; // its place among the letters
};

struct RichRecord {
    std::uint16_t font = 0;
    std::vector<Glyph> glyphs;
    int visible = -1;
    float time = 0;
};

struct {
    std::list<RichRecord> records;
    float clock = 0; // seconds of rich text, for the effects
    unsigned frame = 0;
} rich;

constexpr float WAVE_HEIGHT = 0.12f; // of the letter's size
constexpr float WAVE_SPEED = 6.0f; // radians a second
constexpr float WAVE_STEP = 0.6f; // radians from one letter to the next
constexpr float SHAKE_SIZE = 0.06f; // of the letter's size
constexpr float RAINBOW_SPEED = 120.0f; // degrees a second
constexpr float RAINBOW_STEP = 24.0f; // degrees from one letter to the next
constexpr float FIRST_FRAME_WIDTH = 640; // design units, before Clay has said

std::uint8_t hex(char c) {
    if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<std::uint8_t>(c - 'A' + 10);
    return 0;
}

std::optional<Color> parse_color(std::string_view s) {
    if (s.starts_with("#") && (s.size() == 7 || s.size() == 9)) {
        const auto byte = [&](std::size_t i) {
            return static_cast<unsigned char>(hex(s[i]) * 16 + hex(s[i + 1]));
        };
        return Color{ byte(1), byte(3), byte(5),
                      s.size() == 9 ? byte(7) : static_cast<unsigned char>(255) };
    }
    struct Named {
        std::string_view name;
        Color color;
    };
    static constexpr std::array NAMES{
        Named{ "red", Color{ 230, 41, 55, 255 } },
        Named{ "green", Color{ 0, 228, 48, 255 } },
        Named{ "blue", Color{ 0, 121, 241, 255 } },
        Named{ "yellow", Color{ 253, 249, 0, 255 } },
        Named{ "orange", Color{ 255, 161, 0, 255 } },
        Named{ "purple", Color{ 200, 122, 255, 255 } },
        Named{ "white", Color{ 255, 255, 255, 255 } },
        Named{ "black", Color{ 0, 0, 0, 255 } },
        Named{ "gray", Color{ 130, 130, 130, 255 } },
        Named{ "pink", Color{ 255, 109, 194, 255 } },
    };
    for (const Named &n : NAMES) {
        if (n.name == s) return n.color;
    }
    return std::nullopt;
}

Color theme_color(const Theme &t, ColorRole role) {
    switch (role) {
        case ColorRole::TEXT:
            return t.text;
        case ColorRole::MUTED:
            return t.text_muted;
        case ColorRole::PRIMARY:
            return t.primary;
        case ColorRole::DANGER:
            return t.danger;
    }
    return t.text;
}

// The code points of a UTF-8 text, a malformed byte taken as itself.
std::vector<int> code_points(std::string_view s) {
    std::vector<int> out;
    std::size_t i = 0;
    while (i < s.size()) {
        int bytes = 0;
        const int cp = GetCodepointNext(s.data() + i, &bytes);
        out.push_back(cp);
        i += static_cast<std::size_t>(std::max(1, bytes));
    }
    return out;
}

// How far a letter moves the pen: measured as the UI measures every text --
// through its measure provider, so a headless test's stub answers, and a
// letter the face lacks is baked into it.
float advance(std::uint16_t font_id, int codepoint, float size) {
    int bytes = 0;
    // NOLINTNEXTLINE(custom-rmp-raw-pointer): raylib's static buffer, read at once
    const char *utf8 = CodepointToUTF8(codepoint, &bytes);
    const Clay_StringSlice slice{ bytes, utf8, utf8 };
    return measure_piece(slice, font_id, size).width;
}

} // namespace

// Lay the text out in `width` pixels, word by word.
RichRecord &lay_out(const yarn::Marked &marked, float width, float base_size, Color base,
                    std::uint16_t font_id, float &height, float &widest) {
    RichRecord &r = rich.records.emplace_back();
    r.font = font_id;
    const std::vector<int> cps = code_points(marked.text);
    std::vector<Glyph> glyphs(cps.size());
    for (std::size_t i = 0; i < cps.size(); ++i) {
        glyphs[i].codepoint = cps[i];
        glyphs[i].size = base_size;
        glyphs[i].color = base;
        glyphs[i].index = static_cast<int>(i);
    }
    for (const yarn::Attribute &a : marked.attributes) {
        const auto from = static_cast<std::size_t>(std::max(0, a.position));
        const auto to = std::min(glyphs.size(),
                                 from + static_cast<std::size_t>(std::max(0, a.length)));
        std::string value;
        for (const auto &[k, v] : a.properties) {
            if (k == a.name) value = v;
        }
        for (std::size_t i = from; i < to; ++i) {
            Glyph &g = glyphs[i];
            if (a.name == "b") g.effects |= BOLD;
            if (a.name == "wave") g.effects |= WAVE;
            if (a.name == "shake") g.effects |= SHAKE;
            if (a.name == "rainbow") g.effects |= RAINBOW;
            if (a.name == "color") {
                if (auto c = parse_color(value)) g.color = *c;
            }
            if (a.name == "size") {
                const float s = std::strtof(value.c_str(), nullptr);
                if (s > 0 && std::isfinite(s)) g.size = px(s);
            }
        }
    }
    // Words: a run up to a space or a new line. A word wider than the line is
    // broken where it overflows.
    float x = 0;
    float y = 0;
    float line_height = base_size;
    widest = 0;
    std::size_t i = 0;
    while (i < glyphs.size()) {
        if (glyphs[i].codepoint == '\n') {
            x = 0;
            y += line_height * 1.15f;
            line_height = base_size;
            ++i;
            continue;
        }
        std::size_t end = i;
        float word = 0;
        while (end < glyphs.size() && glyphs[end].codepoint != ' ' &&
               glyphs[end].codepoint != '\n') {
            word += advance(font_id, glyphs[end].codepoint, glyphs[end].size);
            ++end;
        }
        if (x > 0 && x + word > width) {
            x = 0;
            y += line_height * 1.15f;
            line_height = base_size;
        }
        for (std::size_t k = i; k < end; ++k) {
            const float w = advance(font_id, glyphs[k].codepoint, glyphs[k].size);
            if (x > 0 && x + w > width && word > width) {
                x = 0;
                y += line_height * 1.15f;
            }
            glyphs[k].at = Vector2{ x, y };
            line_height = std::max(line_height, glyphs[k].size);
            x += w;
            widest = std::max(widest, x);
        }
        if (end < glyphs.size() && glyphs[end].codepoint == ' ') {
            glyphs[end].at = Vector2{ x, y };
            x += advance(font_id, ' ', glyphs[end].size);
            ++end;
        }
        i = end;
    }
    height = y + line_height;
    r.glyphs = std::move(glyphs);
    return r;
}

void rich_begin_frame() {
    rich.records.clear();
    rich.clock += frame_time();
}

// RMP_COVERAGE_OFF(on the GPU: the unit tests have no window; tests/ui_render_test.cpp draws every line)
void draw_rich(const Clay_RenderCommand &cmd, float alpha) {
    const Vector2 origin{ cmd.boundingBox.x, cmd.boundingBox.y };
    // Only a record of this frame's: a custom element is not always rich
    // text (the overlay's input catcher is one, its data a 1-byte tag), and
    // reading a magic number through someone else's pointer read past it.
    const auto mine = std::ranges::find_if(rich.records, [&](const RichRecord &record) {
        return &record == cmd.renderData.custom.customData;
    });
    if (mine == rich.records.end()) return;
    const RichRecord &r = *mine;
    for (const Glyph &g : r.glyphs) {
        if (r.visible >= 0 && g.index >= r.visible) break;
        if (g.codepoint == ' ' || g.codepoint == '\n') continue;
        Vector2 at{ origin.x + g.at.x, origin.y + g.at.y };
        const auto phase = static_cast<float>(g.index);
        if ((g.effects & WAVE) != 0) {
            at.y +=
                std::sin(r.time * WAVE_SPEED + phase * WAVE_STEP) * g.size * WAVE_HEIGHT;
        }
        if ((g.effects & SHAKE) != 0) {
            // Deterministic under a fixed step: a hash of the letter and the frame.
            const auto tick = static_cast<unsigned>(r.time * 30.0f);
            const unsigned h =
                (static_cast<unsigned>(g.index) * 2654435761u) ^ (tick * 40503u);
            at.x += (static_cast<float>(h % 1000) / 500.0f - 1.0f) * g.size * SHAKE_SIZE;
            at.y += (static_cast<float>((h / 1000) % 1000) / 500.0f - 1.0f) * g.size *
                SHAKE_SIZE;
        }
        Color c = g.color;
        if ((g.effects & RAINBOW) != 0) {
            const float hue =
                std::fmod(r.time * RAINBOW_SPEED + phase * RAINBOW_STEP, 360.0f);
            c = ColorFromHSV(hue, 0.7f, 1.0f);
        }
        c.a = static_cast<unsigned char>(std::lround(static_cast<float>(c.a) * alpha));
        const ::Font f = ui_font(g.size, r.font);
        DrawTextCodepoint(f, g.codepoint, at, g.size, c);
        if ((g.effects & BOLD) != 0)
            DrawTextCodepoint(f, g.codepoint, Vector2{ at.x + 1, at.y }, g.size, c);
    }
}
// RMP_COVERAGE_ON

void rich_with(const yarn::Marked &marked, const RichTextOptions &o,
               std::string_view label) {
    const Theme &t = current_theme();
    const Clay_ElementId id =
        element_id(label.empty() ? std::string_view("rich_text") : label, o.id);
    // As wide as it was given last frame, or as asked.
    float width = o.width > 0 ? px(o.width) : 0.0f;
    if (width <= 0) {
        const Clay_ElementData data = Clay_GetElementData(id);
        width = data.found && data.boundingBox.width > 0 ? data.boundingBox.width
                                                         : px(FIRST_FRAME_WIDTH);
    }
    const float size = px(resolve_size(o.size, t));
    float height = 0;
    float widest = 0;
    RichRecord &r = lay_out(marked, width, size, theme_color(t, o.color),
                            font_id_of(look().font), height, widest);
    r.visible = o.visible;
    r.time = rich.clock;
    Clay_ElementDeclaration d{};
    if (o.width > 0) {
        d.layout.sizing.width = Clay_SizingAxis{ .size = { .minMax = { width, width } },
                                                 .type = CLAY__SIZING_TYPE_FIXED };
    } else {
        // Growing into the room there is, and never narrower than its own
        // widest line: in a panel that fits its content, room is what the
        // content asks for, and a text that asked for none was laid out 0 wide.
        // (A max of 0 is none: Clay makes it the largest float.)
        d.layout.sizing.width =
            Clay_SizingAxis{ .size = { .minMax = { std::ceil(widest), 0 } },
                             .type = CLAY__SIZING_TYPE_GROW };
    }
    d.layout.sizing.height = Clay_SizingAxis{ .size = { .minMax = { height, height } },
                                              .type = CLAY__SIZING_TYPE_FIXED };
    d.custom.customData = &r;
    Clay__OpenElementWithId(id);
    Clay__ConfigureOpenElement(d);
    Clay__CloseElement();
}

} // namespace detail

void rich_text(const rmp::Text &text) { rich_text(text, RichTextOptions{}); }

void rich_text(const rmp::Text &text, const RichTextOptions &o) {
    const std::string shown = text.str();
    if (!detail::frame_ready("rich_text", shown)) return;
    const yarn::Marked marked = yarn::parse_markup(shown, rmp::lang::current());
    detail::rich_with(marked, o, text.id());
}

int rich_text_length(const rmp::Text &text) {
    const yarn::Marked marked = yarn::parse_markup(text.str(), rmp::lang::current());
    return static_cast<int>(detail::code_points(marked.text).size());
}

} // namespace rmp::ui
