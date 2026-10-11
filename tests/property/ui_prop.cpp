// ---------------------------------------------------------------------------
// rmp::ui, held to the campaign's rules for anything a game can hand it.
//
// An interface is a program the game runs every frame, so the generator here
// writes programs: random trees of every container around every widget, with
// every option drawn from what a game can get wrong -- NaN, the infinities,
// sizes past what a pixel count holds, negative counts, indexes out of range,
// strings with a NUL or broken UTF-8 inside -- some of it outside begin() and
// end(), some of it under an overlay() instead, with the theme and the scale
// changed under it. Played for a few frames, and then:
//
//   - nothing undefined happened (the build is sanitized: ASan and UBSan,
//     float-cast-overflow included, which is what a size cast to a pixel
//     count is);
//   - everything it asked to be drawn is somewhere: every box finite, and
//     inside what an int holds, which is what raylib's scissor is given;
//   - no NaN went into the game's variables, and each control kept its
//     promise about them: a slider's value inside its range, a dropdown's
//     index inside its list, a text field's text inside its max_length;
//   - the theme and the scale it reads back are numbers a layout can use;
//   - and the interface still works: a plain button laid out after it is
//     where a button goes.
//
// And over trees of plain containers and controls in any viewport from 320 to
// 4096: a child inside its parent, siblings apart, and Tab visiting every
// control once before it comes back to the first.
//
// covers: rmp::ui::at [template]
// covers: rmp::ui::begin()
// covers: rmp::ui::begin(const FrameOptions &)
// covers: rmp::ui::button(std::string_view)
// covers: rmp::ui::button(std::string_view, const ButtonOptions &)
// covers: rmp::ui::cell [template]
// covers: rmp::ui::center [template]
// covers: rmp::ui::checkbox(std::string_view, bool &)
// covers: rmp::ui::checkbox(std::string_view, bool &, const CheckboxOptions &)
// covers: rmp::ui::column [template]
// covers: rmp::ui::compact()
// covers: rmp::ui::current_breakpoint()
// covers: rmp::ui::current_theme()
// covers: rmp::ui::dropdown(std::string_view, int &, std::initializer_list<std::string_view>)
// covers: rmp::ui::dropdown(std::string_view, int &, std::initializer_list<std::string_view>, const DropdownOptions &)
// covers: rmp::ui::dropdown(std::string_view, int &, std::span<const std::string_view>)
// covers: rmp::ui::dropdown(std::string_view, int &, std::span<const std::string_view>, const DropdownOptions &)
// covers: rmp::ui::end()
// covers: rmp::ui::focus(std::string_view)
// covers: rmp::ui::focused()
// covers: rmp::ui::grid [template]
// covers: rmp::ui::image(const Texture2D &)
// covers: rmp::ui::image(const Texture2D &, const ImageOptions &)
// covers: rmp::ui::layer [template]
// covers: rmp::ui::navigation_enabled()
// covers: rmp::ui::notify(std::string_view)
// covers: rmp::ui::notify(std::string_view, const NotifyOptions &)
// covers: rmp::ui::overlay [template]
// covers: rmp::ui::panel [template]
// covers: rmp::ui::progress(float)
// covers: rmp::ui::progress(float, const ProgressOptions &)
// covers: rmp::ui::row [template]
// covers: rmp::ui::scale()
// covers: rmp::ui::scroll [template]
// covers: rmp::ui::set_navigation_enabled(bool)
// covers: rmp::ui::set_scale(float)
// covers: rmp::ui::set_theme(const Theme &)
// covers: rmp::ui::shutdown()
// covers: rmp::ui::slider(std::string_view, float &, float, float)
// covers: rmp::ui::slider(std::string_view, float &, float, float, const SliderOptions &)
// covers: rmp::ui::spacer()
// covers: rmp::ui::spacer(float)
// covers: rmp::ui::stack [template]
// covers: rmp::ui::text(std::string_view)
// covers: rmp::ui::text(std::string_view, const TextOptions &)
// covers: rmp::ui::text_input(std::string_view, std::string &)
// covers: rmp::ui::text_input(std::string_view, std::string &, const TextInputOptions &)
// covers: rmp::ui::theme_dark()
// covers: rmp::ui::theme_light()
// covers: rmp::ui::tooltip(std::string_view)
// covers: rmp::ui::tooltip(std::string_view, const TooltipOptions &)
// covers: rmp::ui::wants_keyboard()
// covers: rmp::ui::wants_pointer()
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"

#include "../../src/rmp/internal.h"
#include "../../src/rmp/scene_internal.h"
#include "../../src/rmp/ui/floating.h"
#include "../../src/rmp/ui/internal.h"

#include <rmp/app.h>
#include <rmp/ui.h>

#include <doctest.h>
#include <rapidcheck.h>
#include <raylib.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <ostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace gen = rmp::test::gen;
using rmp::ui::Align;

// --- the program -------------------------------------------------------------------

enum class Kind {
    BUTTON,
    TEXT,
    CHECKBOX,
    SLIDER,
    DROPDOWN_SPAN,
    DROPDOWN_LIST,
    TEXT_INPUT,
    IMAGE,
    PROGRESS,
    SPACER,
    TOOLTIP,
    NOTIFY,
    FOCUS,
    ASK, // everything that only answers: scale(), focused(), wants_*(), ...
    SET_NAVIGATION,
    SET_SCALE,
    SET_THEME,
    // Containers: what follows has children.
    ROW,
    COLUMN,
    PANEL,
    CENTER,
    STACK,
    LAYER,
    GRID,
    SCROLL,
    AT,
};
constexpr int LEAVES = static_cast<int>(Kind::SET_THEME) + 1;
constexpr int KINDS = static_cast<int>(Kind::AT) + 1;

// One call, with everything any call of its kind could be handed. What it
// owns of the game's -- a slider's value, a checkbox's bool, a dropdown's
// index, a field's text -- lives here too, from frame to frame, as a game's
// would.
struct Op {
    Kind kind = Kind::BUTTON;
    bool options = false; // the overload with options, or the one without
    std::string label;
    std::array<float, 6> f{};
    std::array<int, 3> i{};
    std::array<bool, 3> b{};
    Align align = Align::CENTER;
    std::vector<std::string> items;
    std::vector<Op> children;
    // The game's.
    float value = 0;
    int selected = 0;
    bool checked = false;
    std::string typed;
};

std::ostream &operator<<(std::ostream &os, const Op &op) {
    os << "{kind " << static_cast<int>(op.kind) << (op.options ? " +o" : "") << " label "
       << op.label.size() << "B f";
    for (const float x : op.f) os << " " << x;
    os << " i";
    for (const int x : op.i) os << " " << x;
    os << " value " << op.value << " selected " << op.selected << " items "
       << op.items.size();
    if (!op.children.empty()) {
        os << " [";
        for (const Op &child : op.children) os << child;
        os << "]";
    }
    return os << "}";
}

// An int a game gets wrong: the ends of its range, around zero, past a list.
rc::Gen<int> hostile_int() {
    return rc::gen::weightedOneOf<int>({
        { 2,
          rc::gen::element(INT_MIN, INT_MIN + 1, -1000000, -2, -1, 0, 1, 2, 3, 7, 1000000,
                           INT_MAX - 1, INT_MAX) },
        { 1, rc::gen::arbitrary<int>() },
    });
}

// A label: a plain one mostly, so that two of them can be told apart, and
// sometimes whatever a game can hand over.
rc::Gen<std::string> label() {
    return rc::gen::weightedOneOf<std::string>({
        { 3,
          rc::gen::map(rc::gen::inRange(0, 1000),
                       [](int n) { return "w" + std::to_string(n); }) },
        { 1, gen::hostile_string() },
    });
}

// A float a game gets wrong, or one it gets right.
rc::Gen<float> some_float() {
    return rc::gen::weightedOneOf<float>({
        { 1, gen::hostile_float() },
        { 1,
          rc::gen::map(rc::gen::inRange(-50, 500),
                       [](int n) { return static_cast<float>(n); }) },
    });
}

rc::Gen<Op> op(int depth);

rc::Gen<std::vector<Op>> ops(int depth) {
    return rc::gen::exec([depth] {
        const int count = *rc::gen::inRange(0, depth == 0 ? 7 : 4);
        std::vector<Op> out;
        for (int n = 0; n < count; ++n) out.push_back(*op(depth));
        return out;
    });
}

rc::Gen<Op> op(int depth) {
    return rc::gen::exec([depth] {
        Op o;
        const int kinds = depth >= 2 ? LEAVES : KINDS;
        o.kind = static_cast<Kind>(*rc::gen::inRange(0, kinds));
        o.options = *rc::gen::arbitrary<bool>();
        o.label = *label();
        for (float &x : o.f) x = *some_float();
        for (int &x : o.i) x = *hostile_int();
        for (bool &x : o.b) x = *rc::gen::arbitrary<bool>();
        o.align = static_cast<Align>(*rc::gen::inRange(0, 9));
        o.items = *rc::gen::container<std::vector<std::string>>(*rc::gen::inRange(0, 4),
                                                                label());
        o.value = *some_float();
        o.selected = *hostile_int();
        o.checked = *rc::gen::arbitrary<bool>();
        o.typed = *gen::hostile_string();
        if (static_cast<int>(o.kind) >= LEAVES) o.children = *ops(depth + 1);
        return o;
    });
}

// A colour, any of them.
Color colour(int seed) {
    const auto byte = [seed](int shift) {
        return static_cast<unsigned char>((static_cast<unsigned>(seed) >> shift) & 0xFFu);
    };
    return Color{ byte(0), byte(8), byte(16), byte(24) };
}

// A theme with any of its metrics broken.
rc::Gen<rmp::ui::Theme> some_theme() {
    return rc::gen::exec([] {
        rmp::ui::Theme t =
            *rc::gen::arbitrary<bool>() ? rmp::ui::theme_dark() : rmp::ui::theme_light();
        const auto maybe = [](float &metric) {
            if (*rc::gen::inRange(0, 4) == 0) metric = *gen::hostile_float();
        };
        maybe(t.font_size);
        maybe(t.font_size_small);
        maybe(t.font_size_large);
        maybe(t.padding_x);
        maybe(t.padding_y);
        maybe(t.gap);
        maybe(t.panel_padding);
        maybe(t.corner_radius);
        maybe(t.border_width);
        maybe(t.control_size);
        maybe(t.track_thickness);
        maybe(t.focus_ring);
        maybe(t.transition);
        maybe(t.min_touch_size);
        return t;
    });
}

// --- playing it ------------------------------------------------------------------

std::string_view view(const std::string &s) { return s; }

void play(std::vector<Op> &program);

// One call. `o` is the game's: what a control writes, it writes there.
void play(Op &o) {
    const auto children = [&o] { play(o.children); };
    switch (o.kind) {
        case Kind::BUTTON:
            if (o.options) {
                (void)rmp::ui::button(
                    o.label,
                    { .style = static_cast<rmp::ui::Variant>(std::abs(o.i[0] % 5)),
                      .size = o.f[0],
                      .enabled = o.b[0],
                      .id = view(o.typed) });
            } else {
                (void)rmp::ui::button(o.label);
            }
            break;
        case Kind::TEXT:
            if (o.options) {
                rmp::ui::text(
                    o.label,
                    { .color = static_cast<rmp::ui::ColorRole>(std::abs(o.i[0] % 4)),
                      .size = o.f[0],
                      .wrap = o.b[0] });
            } else {
                rmp::ui::text(o.label);
            }
            break;
        case Kind::CHECKBOX:
            if (o.options) {
                (void)rmp::ui::checkbox(o.label, o.checked, { .enabled = o.b[0] });
            } else {
                (void)rmp::ui::checkbox(o.label, o.checked);
            }
            break;
        case Kind::SLIDER:
            if (o.options) {
                (void)rmp::ui::slider(o.label, o.value, o.f[0], o.f[1],
                                      { .width = o.f[2],
                                        .step = o.f[3],
                                        .enabled = o.b[0],
                                        .show_value = o.b[1] });
            } else {
                (void)rmp::ui::slider(o.label, o.value, o.f[0], o.f[1]);
            }
            break;
        case Kind::DROPDOWN_SPAN: {
            std::vector<std::string_view> items(o.items.begin(), o.items.end());
            if (o.options) {
                (void)rmp::ui::dropdown(o.label, o.selected, std::span(items),
                                        { .width = o.f[0], .enabled = o.b[0] });
            } else {
                (void)rmp::ui::dropdown(o.label, o.selected,
                                        std::span<const std::string_view>(items));
            }
            break;
        }
        case Kind::DROPDOWN_LIST:
            // A list written in place, as the header's example does: two of
            // them, whatever the game drew.
            if (o.options) {
                (void)rmp::ui::dropdown(o.label, o.selected, { "Low", view(o.label) },
                                        { .width = o.f[0], .enabled = o.b[0] });
            } else {
                (void)rmp::ui::dropdown(o.label, o.selected, { "Low", view(o.label) });
            }
            break;
        case Kind::TEXT_INPUT:
            if (o.options) {
                (void)rmp::ui::text_input(o.label, o.typed,
                                          { .width = o.f[0],
                                            .enabled = o.b[0],
                                            .placeholder = view(o.label),
                                            .max_length = o.i[0] });
            } else {
                (void)rmp::ui::text_input(o.label, o.typed);
            }
            break;
        case Kind::IMAGE: {
            // A texture that was never loaded, of any size: id 0 draws nothing.
            Texture2D texture{};
            texture.width = o.i[0];
            texture.height = o.i[1];
            if (o.options) {
                rmp::ui::image(texture,
                               { .grow = o.b[0],
                                 .width = o.f[0],
                                 .height = o.f[1],
                                 .tint = colour(o.i[2]) });
            } else {
                rmp::ui::image(texture);
            }
            break;
        }
        case Kind::PROGRESS:
            if (o.options) {
                rmp::ui::progress(o.f[0],
                                  { .width = o.f[1],
                                    .height = o.f[2],
                                    .radius = o.f[3],
                                    .fill = colour(o.i[0]),
                                    .track = colour(o.i[1]) });
            } else {
                rmp::ui::progress(o.f[0]);
            }
            break;
        case Kind::SPACER:
            if (o.options) {
                rmp::ui::spacer(o.f[0]);
            } else {
                rmp::ui::spacer();
            }
            break;
        case Kind::TOOLTIP:
            if (o.options) {
                rmp::ui::tooltip(o.label, { .side = o.align, .delay = o.f[0] });
            } else {
                rmp::ui::tooltip(o.label);
            }
            break;
        case Kind::NOTIFY:
            if (o.options) {
                rmp::ui::notify(
                    o.label,
                    { .seconds = o.f[0],
                      .color = static_cast<rmp::ui::ColorRole>(std::abs(o.i[0] % 4)) });
            } else {
                rmp::ui::notify(o.label);
            }
            break;
        case Kind::FOCUS:
            rmp::ui::focus(o.b[0] ? std::string_view{} : view(o.label));
            break;
        case Kind::ASK: {
            const float s = rmp::ui::scale();
            RC_ASSERT(std::isfinite(s));
            RC_ASSERT(s > 0.0f);
            const std::string_view focused = rmp::ui::focused();
            (void)focused.size();
            (void)rmp::ui::wants_pointer();
            (void)rmp::ui::wants_keyboard();
            (void)rmp::ui::navigation_enabled();
            const rmp::ui::Breakpoint bp = rmp::ui::current_breakpoint();
            RC_ASSERT(rmp::ui::compact() == (bp == rmp::ui::Breakpoint::COMPACT));
            break;
        }
        case Kind::SET_NAVIGATION:
            rmp::ui::set_navigation_enabled(o.b[0]);
            RC_ASSERT(rmp::ui::navigation_enabled() == o.b[0]);
            break;
        case Kind::SET_SCALE:
            rmp::ui::set_scale(o.f[0]);
            break;
        case Kind::SET_THEME: {
            rmp::ui::Theme t = o.b[0] ? rmp::ui::theme_light() : rmp::ui::theme_dark();
            t.gap = o.f[0];
            t.font_size = o.f[1];
            t.padding_x = o.f[2];
            t.corner_radius = o.f[3];
            t.control_size = o.f[4];
            t.min_touch_size = o.f[5];
            rmp::ui::set_theme(t);
            break;
        }
        case Kind::ROW:
            if (o.options) {
                rmp::ui::row({ .gap = o.f[0],
                               .padding = o.f[1],
                               .items = o.align,
                               .grow_x = o.b[0],
                               .width = o.f[2],
                               .grow_y = o.b[1],
                               .height = o.f[3] },
                             children);
            } else {
                rmp::ui::row(children);
            }
            break;
        case Kind::COLUMN:
            if (o.options) {
                rmp::ui::column({ .gap = o.f[0],
                                  .padding = o.f[1],
                                  .items = o.align,
                                  .grow_x = o.b[0],
                                  .width = o.f[2],
                                  .grow_y = o.b[1],
                                  .height = o.f[3] },
                                children);
            } else {
                rmp::ui::column(children);
            }
            break;
        case Kind::PANEL:
            if (o.options) {
                rmp::ui::panel(
                    { .box = { .gap = o.f[0], .padding = o.f[1], .width = o.f[2] },
                      .background = colour(o.i[0]),
                      .radius = o.f[3],
                      .border = colour(o.i[1]),
                      .border_width = o.f[4] },
                    children);
            } else {
                rmp::ui::panel(children);
            }
            break;
        case Kind::CENTER:
            rmp::ui::center(children);
            break;
        case Kind::STACK:
            // A stack's children are its layers.
            rmp::ui::stack([&o] {
                for (Op &child : o.children) rmp::ui::layer([&child] { play(child); });
            });
            break;
        case Kind::LAYER:
            // Outside a stack: a mistake, said once.
            rmp::ui::layer(children);
            break;
        case Kind::GRID:
            if (o.options) {
                rmp::ui::grid({ .columns = o.i[0],
                                .min_cell = o.f[0],
                                .gap = o.f[1],
                                .padding = o.f[2],
                                .grow_x = o.b[0],
                                .grow_y = o.b[1] },
                              [&o] {
                                  for (Op &child : o.children) {
                                      if (o.b[2]) {
                                          play(child); // not in a cell: a mistake
                                      } else {
                                          rmp::ui::cell([&child] { play(child); });
                                      }
                                  }
                              });
            } else {
                rmp::ui::grid([&o] {
                    for (Op &child : o.children) rmp::ui::cell([&child] { play(child); });
                });
            }
            break;
        case Kind::SCROLL:
            if (o.options) {
                rmp::ui::scroll({ .horizontal = o.b[0],
                                  .vertical = o.b[1],
                                  .gap = o.f[0],
                                  .padding = o.f[1],
                                  .grow_x = o.b[2],
                                  .width = o.f[2],
                                  .grow_y = !o.b[2],
                                  .height = o.f[3] },
                                children);
            } else {
                rmp::ui::scroll(children);
            }
            break;
        case Kind::AT:
            if (o.options) {
                rmp::ui::at(Vector2{ o.f[0], o.f[1] },
                            { .anchor = o.align, .offset = Vector2{ o.f[2], o.f[3] } },
                            children);
            } else {
                rmp::ui::at(Vector2{ o.f[0], o.f[1] }, children);
            }
            break;
    }
}

void play(std::vector<Op> &program) {
    for (Op &o : program) play(o);
}

// What a frame is opened with: begin(), with or without its options, or an
// overlay() in its place.
struct Frame {
    int how = 0; // 0 begin(), 1 begin(options), 2 overlay(options)
    Align placement = Align::CENTER;
    int veil = 0;
    std::array<float, 4> f{};
    std::array<bool, 2> b{};
};

std::ostream &operator<<(std::ostream &os, const Frame &f) {
    return os << "{frame " << f.how << " f " << f.f[0] << " " << f.f[1] << " " << f.f[2]
              << " " << f.f[3] << "}";
}

rc::Gen<Frame> some_frame() {
    return rc::gen::exec([] {
        Frame f;
        f.how = *rc::gen::inRange(0, 3);
        f.placement = static_cast<Align>(*rc::gen::inRange(0, 9));
        f.veil = *hostile_int();
        for (float &x : f.f) x = *some_float();
        for (bool &x : f.b) x = *rc::gen::arbitrary<bool>();
        return f;
    });
}

void frame(const Frame &how, std::vector<Op> &program) {
    switch (how.how) {
        case 0:
            rmp::ui::begin();
            play(program);
            rmp::ui::end();
            break;
        case 1:
            rmp::ui::begin({ .placement = how.placement,
                             .gap = how.f[0],
                             .padding = how.f[1],
                             .grow = how.b[0],
                             .focus_first = how.b[1] });
            play(program);
            rmp::ui::end();
            break;
        default:
            rmp::ui::overlay({ .placement = how.placement,
                               .blur = how.f[0],
                               .dim = how.f[1],
                               .veil = colour(how.veil) },
                             [&program] { play(program); });
            break;
    }
}

// --- the checks ------------------------------------------------------------------

// raylib's scissor takes ints; render.cpp hands it a box as it is.
constexpr float INT_ROOM = 2.0e9f;

bool usable(float v) { return std::isfinite(v) && std::fabs(v) < INT_ROOM; }

// Every box the last pass asked to be drawn is somewhere a renderer can go.
void geometry_is_usable() {
    const Clay_RenderCommandArray commands = rmp::ui::detail::last_pass_commands();
    for (int32_t n = 0; n < commands.length; ++n) {
        const Clay_RenderCommand &cmd = commands.internalArray[n];
        const Clay_BoundingBox &b = cmd.boundingBox;
        RC_LOG() << "command " << n << " type " << static_cast<int>(cmd.commandType)
                 << " at " << b.x << "," << b.y << " " << b.width << "x" << b.height
                 << "\n";
        RC_ASSERT(usable(b.x));
        RC_ASSERT(usable(b.y));
        RC_ASSERT(usable(b.width));
        RC_ASSERT(usable(b.height));
        if (cmd.commandType == CLAY_RENDER_COMMAND_TYPE_RECTANGLE) {
            const Clay_CornerRadius &r = cmd.renderData.rectangle.cornerRadius;
            RC_ASSERT(usable(r.topLeft) && usable(r.topRight) && usable(r.bottomLeft) &&
                      usable(r.bottomRight));
        }
    }
}

// What each control promises about the game's variable.
void values_are_kept(const std::vector<Op> &program) {
    for (const Op &o : program) {
        switch (o.kind) {
            case Kind::SLIDER: {
                const float min = o.f[0];
                const float max = o.f[1];
                RC_LOG() << "slider " << min << ".." << max << " value " << o.value
                         << "\n";
                // NaN never goes into a game's variable, whatever came out of it.
                if (std::isfinite(min) && std::isfinite(max) && min < max &&
                    std::isfinite(max - min)) {
                    RC_ASSERT(std::isfinite(o.value));
                    RC_ASSERT(o.value >= min);
                    RC_ASSERT(o.value <= max);
                }
                break;
            }
            case Kind::DROPDOWN_SPAN:
                if (!o.items.empty()) {
                    RC_ASSERT(o.selected >= 0);
                    RC_ASSERT(o.selected < static_cast<int>(o.items.size()));
                }
                break;
            case Kind::DROPDOWN_LIST:
                RC_ASSERT(o.selected >= 0);
                RC_ASSERT(o.selected < 2);
                break;
            case Kind::TEXT_INPUT:
                if (o.options && o.i[0] > 0) {
                    RC_ASSERT(rmp::ui::detail::utf8_length(o.typed) <= o.i[0]);
                }
                break;
            default:
                break;
        }
        values_are_kept(o.children);
    }
}

void theme_is_usable() {
    const rmp::ui::Theme &t = rmp::ui::current_theme();
    for (const float m :
         { t.font_size, t.font_size_small, t.font_size_large, t.padding_x, t.padding_y,
           t.gap, t.panel_padding, t.corner_radius, t.border_width, t.control_size,
           t.track_thickness, t.focus_ring, t.transition, t.min_touch_size }) {
        RC_ASSERT(std::isfinite(m));
        RC_ASSERT(m >= 0.0f);
    }
    const float s = rmp::ui::scale();
    RC_ASSERT(std::isfinite(s));
    RC_ASSERT(s > 0.0f);
}

// A plain button after all that is laid out where a button goes.
void still_works() {
    rmp::ui::set_theme(rmp::ui::theme_dark());
    rmp::ui::set_scale(0);
    for (int n = 0; n < 2; ++n) {
        rmp::ui::begin();
        (void)rmp::ui::button("still here");
        rmp::ui::end();
    }
    Clay_BoundingBox box{};
    RC_ASSERT(rmp::ui::detail::bounds_of("still here", 0, 0, &box));
    RC_ASSERT(usable(box.x) && usable(box.y));
    RC_ASSERT(box.width > 0.0f);
    RC_ASSERT(box.height > 0.0f);
}

// --- the headless interface ----------------------------------------------------------

// NOLINTBEGIN(custom-rmp-raw-pointer): Clay's signature, not ours to choose
Clay_Dimensions measure_stub(Clay_StringSlice text, Clay_TextElementConfig *config,
                             void * /*user*/) {
    // NOLINTEND(custom-rmp-raw-pointer)
    return Clay_Dimensions{ static_cast<float>(text.length) * config->fontSize * 0.5f,
                            static_cast<float>(config->fontSize) };
}

struct {
    Clay_Vector2 at{ -1.0f, -1.0f };
    bool down = false;
    rmp::ui::detail::NavState nav{};
} hands;

void pointer(Clay_Vector2 &position, bool &down) {
    position = hands.at;
    down = hands.down;
}
void navigate(rmp::ui::detail::NavState &out) { out = hands.nav; }

Camera2D identity() {
    return Camera2D{ .offset = { 0, 0 }, .target = { 0, 0 }, .rotation = 0, .zoom = 1 };
}

// A fresh interface for each case, and the real providers back after it.
struct HeadlessUi {
    HeadlessUi(float width, float height) {
        rmp::scenes::detail::shutdown();
        rmp::app::set_stretch(rmp::app::Stretch::NONE);
        rmp::detail::reset_reports_for_tests();
        hands.at = Clay_Vector2{ -1.0f, -1.0f };
        hands.down = false;
        hands.nav = rmp::ui::detail::NavState{};
        rmp::ui::detail::set_measure_provider(measure_stub);
        rmp::ui::detail::set_pointer_provider(pointer);
        rmp::ui::detail::set_nav_provider(navigate);
        rmp::ui::detail::set_test_viewport(width, height);
        rmp::ui::detail::set_test_frame_time(1.0f / 60);
        rmp::ui::detail::set_pass_camera(identity());
        rmp::ui::detail::forget_notices();
    }
    HeadlessUi(const HeadlessUi &) = delete;
    HeadlessUi &operator=(const HeadlessUi &) = delete;
    ~HeadlessUi() {
        rmp::ui::shutdown();
        rmp::ui::detail::forget_notices();
        rmp::ui::set_theme(rmp::ui::theme_dark());
        rmp::ui::set_scale(0);
        rmp::ui::set_navigation_enabled(true);
        rmp::ui::detail::set_test_frame_time(0);
        rmp::ui::detail::set_measure_provider(rmp::ui::detail::measure_with_raylib);
        rmp::ui::detail::set_pointer_provider(rmp::ui::detail::pointer_from_raylib);
        rmp::ui::detail::set_nav_provider(rmp::ui::detail::nav_from_raylib);
        rmp::ui::detail::set_test_viewport(0, 0);
        rmp::detail::reset_reports_for_tests();
    }
};

// --- plain trees --------------------------------------------------------------------

// A container of plain things: rows, columns and panels around buttons and
// lines of text, every one with a name of its own to be found by.
struct Node {
    int kind = 0; // 0 row, 1 column, 2 panel, 3 button, 4 text
    std::string name;
    std::vector<Node> children;
};

std::ostream &operator<<(std::ostream &os, const Node &n) {
    os << "{" << n.kind << " " << n.name;
    for (const Node &c : n.children) os << " " << c;
    return os << "}";
}

rc::Gen<Node> node(int depth) {
    return rc::gen::exec([depth] {
        Node n;
        n.kind = depth >= 3 ? *rc::gen::inRange(3, 5) : *rc::gen::inRange(0, 5);
        if (n.kind < 3) {
            const int children = *rc::gen::inRange(0, 4);
            for (int i = 0; i < children; ++i) n.children.push_back(*node(depth + 1));
        }
        return n;
    });
}

// Every node its own name, in the order they are written.
void name(Node &n, int &count) {
    n.name = "n" + std::to_string(count++);
    for (Node &c : n.children) name(c, count);
}

void lay_out(const Node &n) {
    const auto children = [&n] {
        for (const Node &c : n.children) lay_out(c);
    };
    switch (n.kind) {
        case 0:
            rmp::ui::row({ .id = n.name }, children);
            break;
        case 1:
            rmp::ui::column({ .id = n.name }, children);
            break;
        case 2:
            rmp::ui::panel({ .box = { .id = n.name } }, children);
            break;
        case 3:
            (void)rmp::ui::button(n.name);
            break;
        default:
            // A line of text has no name to be found by: what it is in is.
            rmp::ui::text(n.name);
            break;
    }
}

Clay_BoundingBox box_of(const Node &n) {
    Clay_BoundingBox b{};
    RC_ASSERT(rmp::ui::detail::bounds_of(n.name, 0, 0, &b));
    return b;
}

constexpr float SLACK = 0.5f; // a pixel's rounding either way

bool inside(const Clay_BoundingBox &outer, const Clay_BoundingBox &inner) {
    return inner.x >= outer.x - SLACK && inner.y >= outer.y - SLACK &&
        inner.x + inner.width <= outer.x + outer.width + SLACK &&
        inner.y + inner.height <= outer.y + outer.height + SLACK;
}

// Each child inside its container, and the children of a row side by side,
// of a column one under the other, apart.
void arranged(const Node &n) {
    if (n.kind >= 3) return;
    const Clay_BoundingBox parent = box_of(n);
    std::vector<Clay_BoundingBox> boxes;
    for (const Node &c : n.children) {
        if (c.kind == 4) continue;
        const Clay_BoundingBox child = box_of(c);
        RC_LOG() << c.name << " in " << n.name << ": " << child.x << "," << child.y << " "
                 << child.width << "x" << child.height << " in " << parent.x << ","
                 << parent.y << " " << parent.width << "x" << parent.height << "\n";
        RC_ASSERT(inside(parent, child));
        boxes.push_back(child);
        arranged(c);
    }
    for (std::size_t i = 1; i < boxes.size(); ++i) {
        if (n.kind == 0) {
            RC_ASSERT(boxes[i].x >= boxes[i - 1].x + boxes[i - 1].width - SLACK);
        } else {
            RC_ASSERT(boxes[i].y >= boxes[i - 1].y + boxes[i - 1].height - SLACK);
        }
    }
}

TEST_SUITE("property: ui") {
    TEST_CASE("any program, any input") {
        rmp::test::check(
            "ui: any calls with any input lay out somewhere, keep the game's values and "
            "go on",
            [] {
                const Frame how = *some_frame();
                std::vector<Op> before = *ops(1); // outside any frame
                std::vector<Op> program = *ops(0);
                const float width = static_cast<float>(*rc::gen::inRange(1, 4097));
                const float height = static_cast<float>(*rc::gen::inRange(1, 4097));
                const HeadlessUi ui(width, height);
                if (*rc::gen::arbitrary<bool>()) rmp::ui::set_theme(*some_theme());
                if (*rc::gen::arbitrary<bool>()) rmp::ui::set_scale(*some_float());
                theme_is_usable();
                for (int n = 0; n < 3; ++n) {
                    // The pointer somewhere, pressed or not; Tab and the arrows.
                    hands.at =
                        Clay_Vector2{ static_cast<float>(*rc::gen::inRange(-10, 4100)),
                                      static_cast<float>(*rc::gen::inRange(-10, 4100)) };
                    hands.down = *rc::gen::arbitrary<bool>();
                    hands.nav.y = *rc::gen::inRange(-1, 2);
                    hands.nav.x = *rc::gen::inRange(-1, 2);
                    hands.nav.tab = *rc::gen::arbitrary<bool>();
                    hands.nav.activate = *rc::gen::arbitrary<bool>();
                    play(before);
                    frame(how, program);
                    geometry_is_usable();
                    values_are_kept(program);
                    theme_is_usable();
                    if (*rc::gen::inRange(0, 6) == 0) rmp::ui::shutdown();
                }
                hands.nav = rmp::ui::detail::NavState{};
                hands.down = false;
                still_works();
            });
    }

    TEST_CASE("plain trees") {
        rmp::test::check(
            "ui: in any viewport, a child is inside its container and apart from "
            "its siblings",
            [] {
                Node root;
                root.kind = *rc::gen::inRange(0, 3);
                const int children = *rc::gen::inRange(1, 5);
                for (int i = 0; i < children; ++i) root.children.push_back(*node(1));
                int count = 0;
                name(root, count);
                const auto width = static_cast<float>(*rc::gen::inRange(320, 4097));
                const auto height = static_cast<float>(*rc::gen::inRange(320, 4097));
                const HeadlessUi ui(width, height);
                for (int n = 0; n < 2; ++n) {
                    rmp::ui::begin();
                    lay_out(root);
                    rmp::ui::end();
                }
                // What does not fit the window is squeezed, and a
                // squeezed box need not hold its contents.
                const Clay_BoundingBox all = box_of(root);
                RC_PRE(inside(Clay_BoundingBox{ 0, 0, width, height }, all));
                arranged(root);
            });
    }

    // --- Tab ----------------------------------------------------------------------------

    TEST_CASE("Tab") {
        rmp::test::check(
            "ui: Tab visits every control that can take the focus once, in order, "
            "and comes back to the first",
            [] {
                // Buttons and checkboxes, some of them disabled, some
                // in rows and panels.
                const int count = *rc::gen::inRange(1, 10);
                std::vector<int> kinds; // 0 button, 1 checkbox
                std::vector<bool> enabled;
                std::vector<int> wrap; // 0 none, 1 row, 2 panel
                for (int i = 0; i < count; ++i) {
                    kinds.push_back(*rc::gen::inRange(0, 2));
                    enabled.push_back(*rc::gen::weightedOneOf<bool>(
                        { { 3, rc::gen::just(true) }, { 1, rc::gen::just(false) } }));
                    wrap.push_back(*rc::gen::inRange(0, 3));
                }
                std::vector<std::string> order;
                for (int i = 0; i < count; ++i) {
                    if (enabled[static_cast<std::size_t>(i)])
                        order.push_back("c" + std::to_string(i));
                }
                RC_PRE(!order.empty());
                const HeadlessUi ui(1280, 4096);
                std::vector<char> ticks(static_cast<std::size_t>(count), 0);
                const auto frame = [&] {
                    rmp::ui::begin({ .focus_first = true });
                    for (int i = 0; i < count; ++i) {
                        const auto at = static_cast<std::size_t>(i);
                        const std::string name = "c" + std::to_string(i);
                        const auto control = [&] {
                            if (kinds[at] == 0) {
                                (void)rmp::ui::button(name, { .enabled = enabled[at] });
                            } else {
                                bool on = ticks[at] != 0;
                                (void)rmp::ui::checkbox(name, on,
                                                        { .enabled = enabled[at] });
                                ticks[at] = on ? 1 : 0;
                            }
                        };
                        if (wrap[at] == 1) {
                            rmp::ui::row(control);
                        } else if (wrap[at] == 2) {
                            rmp::ui::panel(control);
                        } else {
                            control();
                        }
                    }
                    rmp::ui::end();
                };
                frame();
                frame();
                RC_ASSERT(std::string(rmp::ui::focused()) == order.front());
                for (std::size_t press = 1; press <= order.size(); ++press) {
                    hands.nav = rmp::ui::detail::NavState{ .y = 1, .tab = true };
                    frame();
                    hands.nav = rmp::ui::detail::NavState{};
                    frame();
                    RC_LOG() << "Tab " << press << ": " << rmp::ui::focused() << "\n";
                    RC_ASSERT(std::string(rmp::ui::focused()) ==
                              order[press % order.size()]);
                }
            });
    }

    // --- what a text field edits with ---------------------------------------------

    TEST_CASE("UTF-8, as text_input() edits it") {
        const auto continues = [](char c) {
            return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U;
        };
        rmp::test::check(
            "ui: a text cut to max_length is a prefix, cut where a character "
            "starts, as long as it may be",
            [&] {
                const std::string before = *gen::hostile_string();
                const int most = *hostile_int();
                std::string text = before;
                const bool cut = rmp::ui::detail::utf8_truncate(text, most);
                RC_ASSERT(before.starts_with(text));
                const int length = rmp::ui::detail::utf8_length(before);
                if (most <= 0) {
                    RC_ASSERT(!cut);
                    RC_ASSERT(text == before);
                    return;
                }
                RC_ASSERT(cut == (length > most));
                RC_ASSERT(rmp::ui::detail::utf8_length(text) == std::min(length, most));
                if (cut) RC_ASSERT(!continues(before[text.size()]));
            });
        rmp::test::check("ui: Backspace takes off one character, never half of one", [&] {
            const std::string before = *gen::hostile_string();
            std::string text = before;
            RC_ASSERT(rmp::ui::detail::utf8_pop(text) == !before.empty());
            RC_ASSERT(before.starts_with(text));
            RC_ASSERT(rmp::ui::detail::utf8_length(text) ==
                      std::max(0, rmp::ui::detail::utf8_length(before) - 1));
            // What went started a character, or was nothing but the rest of one.
            const std::string gone = before.substr(text.size());
            RC_ASSERT(gone.empty() || !continues(gone[0]) || text.empty());
        });
        rmp::test::check(
            "ui: what is typed goes on the end, one character, if it is one and "
            "there is room",
            [&] {
                const std::string before = *gen::hostile_string();
                const int codepoint = *rc::gen::weightedOneOf<int>({
                    { 2, rc::gen::inRange(0x20, 0x7F) },
                    { 2, rc::gen::inRange(0xA0, 0x110000) },
                    { 1, rc::gen::inRange(0xD800, 0xE000) },
                    { 1, hostile_int() },
                });
                const int most = *rc::gen::inRange(-2, 8);
                std::string text = before;
                const bool typed = rmp::ui::detail::utf8_append(text, codepoint, most);
                const int length = rmp::ui::detail::utf8_length(before);
                const bool typeable = codepoint >= 0x20 &&
                    !(codepoint >= 0x7F && codepoint < 0xA0) &&
                    !(codepoint >= 0xD800 && codepoint <= 0xDFFF) &&
                    codepoint <= 0x10FFFF;
                RC_ASSERT(typed == (typeable && (most <= 0 || length < most)));
                if (!typed) {
                    RC_ASSERT(text == before);
                    return;
                }
                RC_ASSERT(text.starts_with(before));
                RC_ASSERT(rmp::ui::detail::utf8_length(text) == length + 1);
                // raylib reads it back as what was typed.
                const std::string added = text.substr(before.size());
                int bytes = 0;
                RC_ASSERT(GetCodepointNext(added.c_str(), &bytes) == codepoint);
                RC_ASSERT(bytes == static_cast<int>(added.size()));
            });
    }

} // TEST_SUITE

} // namespace
