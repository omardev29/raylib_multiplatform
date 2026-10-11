// ===========================================================================
// notify(): a message that goes away by itself -- and, in a debug build, the
// framework's own warnings on screen.
//
// notify() only queues: a copy of the text, in a ring of the newest eight. It
// can be called from anywhere -- _update(), a callback, a frame with no UI
// -- and it starts nothing. What is queued is drawn by draw_notices(), which
// the app calls after every scene has drawn (../ui_internal.h); a game with
// no scene stack has it drawn at the end of the first pass of each frame
// (pass_ended(), internal.h), there being no app frame to draw it after. The
// report sink a debug build installs is show_report(): the line, in the
// danger colour.
//
// The notices are laid out by a Clay context of THEIR OWN, not as one more
// pass of the game's. A pass after the game's would be the layout Clay holds
// when the next frame starts, and Clay works out which scroll area the wheel
// and a drag belong to from the layout it holds and the pointer it was told
// last -- so for as long as a notice showed, the game's lists would not
// scroll; and its elements would overwrite the slots Clay's scroll state
// points into. In a game with no scene stack a pass after the first would
// also renumber every pass after it, whose ids -- focus, hover, a field
// being typed in, a scroll position -- would all change the moment a notice
// appeared, and again when it went. A context of their own touches none of
// that: no pass, no id, no claim on the pointer, nothing for the focus.
// They are drawn by the same renderer, in the same theme, at the same scale.
// ===========================================================================

#include "floating.h"
#include "internal.h"
#include "../internal.h"
#include "../scene_internal.h" // running(): is there a scene stack to draw after

#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace rmp::ui {

namespace {

using detail::px;
using detail::to_clay;

// How long the fade at the end of a notice is: its last half second, or its
// second half when it is shorter than a second.
constexpr float FADE = 0.5f;

// Its widest, in design units; a longer one wraps.
constexpr float MAX_WIDTH = 560;

struct Notice {
    std::string text;
    float seconds = 0;
    ColorRole color = ColorRole::TEXT;
    float age = 0; // seconds it has been drawn for
    Rectangle box{ 0, 0, 0, 0 }; // where it was last laid out
    Color ink{ 0, 0, 0, 0 }; // what its text was last painted with
    Color fill{ 0, 0, 0, 0 }; // and its box
};

// The queue: a ring, oldest at `first`. The dot says "file state".
//
// While the ring is laid out and drawn, Clay holds pointers into its
// strings, and a report can arrive in the middle of it -- the [ui] font
// failing the first time a notice is measured says so through
// show_report(). Pushed into a full ring, it would free the string Clay
// was reading. So what arrives then waits in `later`, and joins the ring
// once the drawing is done.
struct {
    std::array<Notice, detail::NOTICE_RING> ring;
    int first = 0;
    int count = 0;
    bool drawing = false;
    std::vector<Notice> later;
} notices;

Notice &nth(int i) {
    return notices.ring[static_cast<size_t>((notices.first + i) % detail::NOTICE_RING)];
}

void enqueue(Notice &&n) {
    if (notices.drawing) {
        // However many: they go through the ring afterwards, which keeps the
        // newest, and a report is said once, so they are few.
        notices.later.push_back(std::move(n));
        return;
    }
    if (notices.count == detail::NOTICE_RING) {
        // Full: the oldest makes room.
        notices.first = (notices.first + 1) % detail::NOTICE_RING;
        --notices.count;
    }
    nth(notices.count) = std::move(n);
    ++notices.count;
}

void push(std::string_view text, float seconds, ColorRole color) {
    Notice n{ .text = std::string(text), .seconds = seconds, .color = color };
    detail::utf8_truncate(n.text, detail::NOTICE_MAX_CHARS);
    enqueue(std::move(n));
}

float alpha_of(const Notice &n) {
    const float fade = n.seconds < 2 * FADE ? n.seconds / 2 : FADE;
    const float left = n.seconds - n.age;
    if (!(left < fade)) return 1.0f; // an infinite one never starts to fade
    return left > 0 ? left / fade : 0.0f;
}

Color faded(Color c, float alpha) {
    return Color{ c.r, c.g, c.b, static_cast<unsigned char>(std::lround(c.a * alpha)) };
}

Color role_color(const Theme &t, ColorRole role) {
    Color c = t.text;
    switch (role) {
        case ColorRole::TEXT:
            break;
        case ColorRole::MUTED:
            c = t.text_muted;
            break;
        case ColorRole::PRIMARY:
            c = t.primary;
            break;
        case ColorRole::DANGER:
            c = t.danger;
            break;
    }
    return c;
}

// --- their own Clay ----------------------------------------------------------

// Elements: the root, its column, and a box and a text for each notice --
// with room to spare. Words: Clay caches the width of every word it measures,
// and eight notices of NOTICE_MAX_CHARS are at most a thousand words.
constexpr int LAYOUT_ELEMENTS = 64;
constexpr int LAYOUT_WORDS = 2048;
// Clay picks a line's bucket out of LAYOUT_WORDS / 32 and keeps the buckets
// in an array of LAYOUT_ELEMENTS: more words than that is a write past the
// array, inside the arena, where no sanitizer sees it.
static_assert(LAYOUT_WORDS / 32 <= LAYOUT_ELEMENTS);

// The Clay context the notices are laid out in, its memory, and what went
// wrong in it. Clay_Context is Clay's type; the pointer is Clay's API and
// points into `arena`, which owns the memory.
struct {
    std::vector<unsigned char> arena;
    Clay_Context *context = nullptr;
    int errors = 0;
} layout;

// The framework's own layout failing is a bug in the framework, not a mistake
// of the game's: the log says it, once, and RMP_REPORT_ONCE does not -- it
// would come back here as a notice through the debug build's sink, and
// [dev] strict would stop a correct game for it.
// RMP_COVERAGE_OFF(sized never to fail: see "a full ring of the longest notices")
void on_layout_error(Clay_ErrorData e) {
    if (layout.errors++ == 0) {
        TraceLog(LOG_WARNING, "UI: the notices could not be laid out: %.*s",
                 e.errorText.length, e.errorText.chars);
    }
}
// RMP_COVERAGE_ON

// Their context, made the first time something is drawn. Clay_Initialize()
// makes the context it creates the current one; the one that was current --
// the game's interface -- is put back before this returns.
bool ensure_layout() {
    if (layout.context != nullptr) return true;
    Clay_Context *const game = Clay_GetCurrentContext();
    // With no context current, these set what the next one is created with,
    // and touch nobody's: the game's sets its own again before it starts.
    Clay_SetCurrentContext(nullptr);
    Clay_SetMaxElementCount(LAYOUT_ELEMENTS);
    Clay_SetMaxMeasureTextCacheWordCount(LAYOUT_WORDS);
    const uint32_t size = Clay_MinMemorySize();
    layout.arena.assign(size, 0);
    layout.context = Clay_Initialize(
        Clay_CreateArenaWithCapacityAndMemory(size, layout.arena.data()),
        detail::viewport(), Clay_ErrorHandler{ on_layout_error, nullptr });
    Clay_SetCurrentContext(game);
    if (layout.context == nullptr) return false;
    TraceLog(LOG_INFO, "UI: notices ready (%u bytes)", size);
    return true;
}

Clay_String clay_string(std::string_view s) {
    return Clay_String{ false, static_cast<int32_t>(s.size()), s.data() };
}

constexpr std::string_view NOTICE_ID = "rmp-notice";

// Lay the queue out, bottom centre, oldest on top, and draw it -- in their
// own context, which is current only in here.
void lay_out_and_draw() {
    Clay_Context *const game = Clay_GetCurrentContext();
    Clay_SetCurrentContext(layout.context);
    const Clay_Dimensions screen = detail::viewport();
    Clay_SetLayoutDimensions(screen);
    const Theme &t = current_theme();
    // What Clay remembers measuring, forgotten: it keeps a line it has not
    // seen for a while until another line lands in the same one of its 64
    // buckets, and a context that lays out a notice or two a frame visits few
    // of them -- after about 150 different notices it was full, and a new one
    // was measured as nothing and drawn as an empty box. Measuring what is on
    // screen again, at most eight lines, is cheaper than that.
    Clay_ResetMeasureTextCache();

    Clay_BeginLayout();
    Clay_ElementDeclaration root{};
    root.layout.sizing.width.type = CLAY__SIZING_TYPE_GROW;
    root.layout.sizing.height.type = CLAY__SIZING_TYPE_GROW;
    // Clear of the edge as begin()'s own column is, and of the notch.
    const auto inset =
        static_cast<uint16_t>(px(t.panel_padding) + detail::safe_area_inset());
    root.layout.padding = Clay_Padding{ inset, inset, inset, inset };
    root.layout.childGap = static_cast<uint16_t>(px(t.gap / 2));
    root.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    root.layout.childAlignment =
        Clay_ChildAlignment{ CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_BOTTOM };
    Clay__OpenElement();
    Clay__ConfigureOpenElement(root);
    for (int i = 0; i < notices.count; ++i) {
        Notice &n = nth(i);
        const float alpha = alpha_of(n);
        n.fill = faded(t.panel, alpha);
        n.ink = faded(role_color(t, n.color), alpha);
        Clay_ElementDeclaration box{};
        box.layout.sizing.width.type = CLAY__SIZING_TYPE_FIT;
        box.layout.sizing.width.size.minMax = Clay_SizingMinMax{ 0, px(MAX_WIDTH) };
        box.layout.sizing.height.type = CLAY__SIZING_TYPE_FIT;
        const auto pad_x = static_cast<uint16_t>(px(t.padding_x));
        const auto pad_y = static_cast<uint16_t>(px(t.padding_y / 2));
        box.layout.padding = Clay_Padding{ pad_x, pad_x, pad_y, pad_y };
        box.backgroundColor = to_clay(n.fill);
        const float r = px(t.corner_radius);
        box.cornerRadius = Clay_CornerRadius{ r, r, r, r };
        Clay__OpenElementWithId(
            Clay_GetElementIdWithIndex(clay_string(NOTICE_ID), static_cast<uint32_t>(i)));
        Clay__ConfigureOpenElement(box);
        Clay_TextElementConfig tc{};
        tc.textColor = to_clay(n.ink);
        tc.fontSize = static_cast<uint16_t>(px(t.font_size));
        tc.wrapMode = CLAY_TEXT_WRAP_WORDS;
        // The ring's own string: nothing changes it between here and the draw
        // below, which is as long as Clay keeps it.
        Clay__OpenTextElement(clay_string(n.text), tc);
        Clay__CloseElement();
    }
    Clay__CloseElement();
    const Clay_RenderCommandArray commands = Clay_EndLayout(detail::frame_time());

    for (int i = 0; i < notices.count; ++i) {
        const Clay_ElementData d = Clay_GetElementData(
            Clay_GetElementIdWithIndex(clay_string(NOTICE_ID), static_cast<uint32_t>(i)));
        const Clay_BoundingBox b = d.found ? d.boundingBox : Clay_BoundingBox{};
        nth(i).box = Rectangle{ b.x, b.y, b.width, b.height };
    }
    if (!detail::test_mode()) detail::draw(commands);
    Clay_SetCurrentContext(game);
}

} // namespace

void notify(std::string_view text) { notify(text, NotifyOptions{}); }

void notify(std::string_view text, const NotifyOptions &o) {
    if (text.empty()) {
        RMP_REPORT_ONCE("UI: rmp::ui::notify() with no text; there is nothing to show");
        return;
    }
    // Infinity is allowed: a notice that stays until eight newer ones push it
    // out. Zero, below it, or not a number, is a notice nobody can read.
    if (!(o.seconds > 0)) {
        RMP_REPORT_ONCE("UI: rmp::ui::notify(\"%.*s\") for %g seconds; a notice needs "
                        "more than 0 to be read",
                        static_cast<int>(text.size()), text.data(),
                        static_cast<double>(o.seconds));
        return;
    }
    push(text, o.seconds, o.color);
}

namespace detail {

void draw_notices() {
    if (notices.count == 0) return;
    // The game's UI up, if it was not: its measuring is the one Clay calls,
    // in any context, and its scale is the one the notices are drawn at.
    if (!ensure_started()) return;
    update_scale();
    if (!ensure_layout()) return;
    notices.drawing = true;
    lay_out_and_draw();
    // Then a frame older. Drawn first, so a notice of 1 s is on screen for
    // exactly 1 s of frames, its first one included.
    const float dt = frame_time();
    int kept = 0;
    for (int i = 0; i < notices.count; ++i) {
        Notice &n = nth(i);
        n.age += dt;
        if (!(n.age < n.seconds)) continue; // its time is up
        if (kept != i) nth(kept) = std::move(n);
        ++kept;
    }
    notices.count = kept;
    notices.drawing = false;
    // What arrived meanwhile, newest last, not a frame older: nobody saw it.
    for (Notice &n : notices.later) enqueue(std::move(n));
    notices.later.clear();
}

void show_report(std::string_view line) {
    // Never a report of its own, whatever the line: this is the sink, and a
    // report from here would be heard here.
    if (line.empty()) return;
    push(line, NotifyOptions{}.seconds, ColorRole::DANGER);
}

void pass_ended(const bool drawn) {
    end_at_pass();
    end_tooltip_pass();
    // Closed past EndDrawing(), by the runner: nothing more goes on screen.
    if (!drawn) return;
    // With a scene stack the app draws them, after the last scene.
    if (rmp::scenes::detail::running()) return;
    // Without one: after the frame's first pass, so once a frame -- a game
    // whose screen has an overlay() over it ends two. Under that overlay, as
    // the screen is: there is no telling which end() is the last.
    if (current_pass() != 0) return;
    draw_notices();
}

int notice_count() { return notices.count; }

NoticeView notice_at(int index) {
    if (index < 0 || index >= notices.count) return NoticeView{};
    const Notice &n = nth(index);
    return NoticeView{ .text = n.text,
                       .color = n.color,
                       .alpha = alpha_of(n),
                       .box = n.box,
                       .ink = n.ink,
                       .fill = n.fill };
}

void forget_notices() {
    notices.first = 0;
    notices.count = 0;
    notices.later.clear();
}

int notice_layout_errors() { return layout.errors; }

} // namespace detail

} // namespace rmp::ui
