// ===========================================================================
// tooltip(): a line about the control declared just before it.
//
// Which control that is, is take_last_control() (internal.h): every control
// notes itself when it is declared, and taking it forgets it, so a second
// tooltip after one control has nothing to be about -- a mistake, said once.
//
// It shows while the pointer has rested on the control for `delay` seconds,
// or at once while the keyboard or the gamepad focus is on it and showing:
// the player who walks a menu with a pad gets the same line as the one who
// hovers it. It floats -- a Clay element attached to the control by id, in
// front of the rest of the pass -- so it takes no room and moves nothing.
//
// It stays on the screen. Where Clay would put it is known only once it has
// been laid out, so the frame after it first shows, its size (remembered
// like any element's, bounds_of_id()) and its control's decide: the other
// side when the one asked for runs off the edge and the other does not, then
// along that side as little as it takes. place_tooltip() is that decision,
// pure, so a test can hold every side to it.
// ===========================================================================

#include "floating.h"
#include "internal.h"
#include "../internal.h"

#include <cmath>

namespace rmp::ui {

namespace {

using detail::font_px;
using detail::px;
using detail::px16;
using detail::to_clay;

// In front of everything else in its pass: a stack()'s layers count up from
// 1, and an open dropdown list is at 1000.
constexpr int16_t TOOLTIP_Z = 2000;

// Its widest, in design units; a longer line wraps.
constexpr float MAX_WIDTH = 320;

// The control the pointer is resting on, and for how long. One: there is one
// pointer. `seen` is whether the pass that describes it did, this time --
// a control that stops being described stops being rested on.
struct {
    uint32_t id = 0;
    float seconds = 0;
    int pass = -1;
    bool seen = false;
} rest;

// Clay's attach points for a side: the point of the tooltip that goes on the
// point of the control, and which way the gap pushes it.
struct Attach {
    Clay_FloatingAttachPointType tip;
    Clay_FloatingAttachPointType control;
    float dx;
    float dy;
};

Attach attach_for(Align side) {
    // TOP_CENTER, the default side; every other one is said below.
    Attach a{ CLAY_ATTACH_POINT_CENTER_BOTTOM, CLAY_ATTACH_POINT_CENTER_TOP, 0, -1 };
    switch (side) {
        case Align::TOP_LEFT:
            a = Attach{ CLAY_ATTACH_POINT_LEFT_BOTTOM, CLAY_ATTACH_POINT_LEFT_TOP, 0,
                        -1 };
            break;
        case Align::TOP_CENTER:
            break;
        case Align::TOP_RIGHT:
            a = Attach{ CLAY_ATTACH_POINT_RIGHT_BOTTOM, CLAY_ATTACH_POINT_RIGHT_TOP, 0,
                        -1 };
            break;
        case Align::CENTER_LEFT:
            a = Attach{ CLAY_ATTACH_POINT_RIGHT_CENTER, CLAY_ATTACH_POINT_LEFT_CENTER, -1,
                        0 };
            break;
        case Align::CENTER:
            a = Attach{ CLAY_ATTACH_POINT_CENTER_CENTER, CLAY_ATTACH_POINT_CENTER_CENTER,
                        0, 0 };
            break;
        case Align::CENTER_RIGHT:
            a = Attach{ CLAY_ATTACH_POINT_LEFT_CENTER, CLAY_ATTACH_POINT_RIGHT_CENTER, 1,
                        0 };
            break;
        case Align::BOTTOM_LEFT:
            a = Attach{ CLAY_ATTACH_POINT_LEFT_TOP, CLAY_ATTACH_POINT_LEFT_BOTTOM, 0, 1 };
            break;
        case Align::BOTTOM_CENTER:
            a = Attach{ CLAY_ATTACH_POINT_CENTER_TOP, CLAY_ATTACH_POINT_CENTER_BOTTOM, 0,
                        1 };
            break;
        case Align::BOTTOM_RIGHT:
            a = Attach{ CLAY_ATTACH_POINT_RIGHT_TOP, CLAY_ATTACH_POINT_RIGHT_BOTTOM, 0,
                        1 };
            break;
    }
    return a;
}

bool above_or_below(Align side) {
    switch (side) {
        case Align::TOP_LEFT:
        case Align::TOP_CENTER:
        case Align::TOP_RIGHT:
        case Align::BOTTOM_LEFT:
        case Align::BOTTOM_CENTER:
        case Align::BOTTOM_RIGHT:
            return true;
        case Align::CENTER_LEFT:
        case Align::CENTER:
        case Align::CENTER_RIGHT:
            break;
    }
    return false;
}

Align opposite(Align side) {
    Align o = side; // CENTER is its own
    switch (side) {
        case Align::TOP_LEFT:
            o = Align::BOTTOM_LEFT;
            break;
        case Align::TOP_CENTER:
            o = Align::BOTTOM_CENTER;
            break;
        case Align::TOP_RIGHT:
            o = Align::BOTTOM_RIGHT;
            break;
        case Align::CENTER_LEFT:
            o = Align::CENTER_RIGHT;
            break;
        case Align::CENTER:
            break;
        case Align::CENTER_RIGHT:
            o = Align::CENTER_LEFT;
            break;
        case Align::BOTTOM_LEFT:
            o = Align::TOP_LEFT;
            break;
        case Align::BOTTOM_CENTER:
            o = Align::TOP_CENTER;
            break;
        case Align::BOTTOM_RIGHT:
            o = Align::TOP_RIGHT;
            break;
    }
    return o;
}

// Where Clay puts a tooltip of `size` on `side` of `control`, `gap` away:
// the same arithmetic as its attach points, so that what is kept on the
// screen is what is drawn.
Rectangle tip_at(Rectangle control, Vector2 size, Align side, float gap) {
    const Attach a = attach_for(side);
    float x = control.x;
    switch (a.control) {
        case CLAY_ATTACH_POINT_CENTER_TOP:
        case CLAY_ATTACH_POINT_CENTER_CENTER:
        case CLAY_ATTACH_POINT_CENTER_BOTTOM:
            x += control.width / 2;
            break;
        case CLAY_ATTACH_POINT_RIGHT_TOP:
        case CLAY_ATTACH_POINT_RIGHT_CENTER:
        case CLAY_ATTACH_POINT_RIGHT_BOTTOM:
            x += control.width;
            break;
        case CLAY_ATTACH_POINT_LEFT_TOP:
        case CLAY_ATTACH_POINT_LEFT_CENTER:
        case CLAY_ATTACH_POINT_LEFT_BOTTOM:
            break;
    }
    float y = control.y;
    switch (a.control) {
        case CLAY_ATTACH_POINT_LEFT_CENTER:
        case CLAY_ATTACH_POINT_CENTER_CENTER:
        case CLAY_ATTACH_POINT_RIGHT_CENTER:
            y += control.height / 2;
            break;
        case CLAY_ATTACH_POINT_LEFT_BOTTOM:
        case CLAY_ATTACH_POINT_CENTER_BOTTOM:
        case CLAY_ATTACH_POINT_RIGHT_BOTTOM:
            y += control.height;
            break;
        case CLAY_ATTACH_POINT_LEFT_TOP:
        case CLAY_ATTACH_POINT_CENTER_TOP:
        case CLAY_ATTACH_POINT_RIGHT_TOP:
            break;
    }
    switch (a.tip) {
        case CLAY_ATTACH_POINT_CENTER_TOP:
        case CLAY_ATTACH_POINT_CENTER_CENTER:
        case CLAY_ATTACH_POINT_CENTER_BOTTOM:
            x -= size.x / 2;
            break;
        case CLAY_ATTACH_POINT_RIGHT_TOP:
        case CLAY_ATTACH_POINT_RIGHT_CENTER:
        case CLAY_ATTACH_POINT_RIGHT_BOTTOM:
            x -= size.x;
            break;
        case CLAY_ATTACH_POINT_LEFT_TOP:
        case CLAY_ATTACH_POINT_LEFT_CENTER:
        case CLAY_ATTACH_POINT_LEFT_BOTTOM:
            break;
    }
    switch (a.tip) {
        case CLAY_ATTACH_POINT_LEFT_CENTER:
        case CLAY_ATTACH_POINT_CENTER_CENTER:
        case CLAY_ATTACH_POINT_RIGHT_CENTER:
            y -= size.y / 2;
            break;
        case CLAY_ATTACH_POINT_LEFT_BOTTOM:
        case CLAY_ATTACH_POINT_CENTER_BOTTOM:
        case CLAY_ATTACH_POINT_RIGHT_BOTTOM:
            y -= size.y;
            break;
        case CLAY_ATTACH_POINT_LEFT_TOP:
        case CLAY_ATTACH_POINT_CENTER_TOP:
        case CLAY_ATTACH_POINT_RIGHT_TOP:
            break;
    }
    return Rectangle{ x + a.dx * gap, y + a.dy * gap, size.x, size.y };
}

// How far to move something at `start`, `size` long, to lie inside
// [0, extent): 0 when it does; against 0 when it cannot.
float onto_screen(float start, float size, float extent) {
    if (start < 0 || size > extent) return -start;
    const float over = (start + size) - extent;
    return over > 0 ? -over : 0.0f;
}

bool fits_across(const Rectangle &r, Vector2 screen, bool vertically) {
    if (vertically) return r.y >= 0 && r.y + r.height <= screen.y;
    return r.x >= 0 && r.x + r.width <= screen.x;
}

} // namespace

namespace detail {

TipPlace place_tooltip(Rectangle control, Vector2 size, Align side, Vector2 screen,
                       float gap) {
    TipPlace place{ side, Vector2{ 0, 0 } };
    const bool vertical = above_or_below(side);
    // CENTER is its own opposite: it never moves to another side, only along.
    if (!fits_across(tip_at(control, size, side, gap), screen, vertical) &&
        fits_across(tip_at(control, size, opposite(side), gap), screen, vertical)) {
        place.side = opposite(side);
    }
    const Rectangle r = tip_at(control, size, place.side, gap);
    if (vertical || place.side == Align::CENTER) {
        place.shift.x = onto_screen(r.x, r.width, screen.x);
    }
    if (!vertical || place.side == Align::CENTER) {
        place.shift.y = onto_screen(r.y, r.height, screen.y);
    }
    return place;
}

void end_tooltip_pass() {
    if (rest.pass == current_pass() && !rest.seen) rest = {};
    rest.seen = false;
}

} // namespace detail

void tooltip(std::string_view text) { tooltip(text, TooltipOptions{}); }

void tooltip(std::string_view text, const TooltipOptions &o) {
    if (!detail::frame_ready("tooltip", text)) return;
    const uint32_t control = detail::take_last_control();
    if (control == 0) {
        // Once, naming the first one, and not keyed by the text: a line that
        // changes every frame -- "Ready in 3.2 s" -- would be a new report
        // every frame, on screen and in a table that never shrinks.
        RMP_REPORT_ONCE("UI: rmp::ui::tooltip(\"%.*s\") with no control declared just "
                        "before it, so it is about nothing and shows nothing. It "
                        "follows the button(), checkbox(), slider(), dropdown() or "
                        "text_input() it is about -- one tooltip each.",
                        static_cast<int>(text.size()), text.data());
        return;
    }
    // Nothing to say about it this frame -- tooltip(item.description) for an
    // item without one: no box, and no mistake.
    if (text.empty()) return;
    Clay_ElementId control_id{};
    control_id.id = control;

    // Resting: over it in this pass, frame after frame. The first frame over
    // it is the moment it arrived, so the wait counts from there.
    const bool over = detail::pointer_over(control_id);
    if (over) {
        if (rest.id == control) {
            rest.seconds += detail::frame_time();
        } else {
            rest.id = control;
            rest.seconds = 0;
        }
        rest.pass = detail::current_pass();
        rest.seen = true;
    } else if (rest.id == control) {
        rest = {};
    }
    // Not a number, or below 0, is no wait. An infinite one is what it says:
    // the pointer never waits long enough, and the focus alone shows it.
    const float delay = std::isnan(o.delay) || o.delay < 0 ? 0.0f : o.delay;
    const bool rested = over && rest.seconds >= delay;
    // The keyboard's or the pad's focus on it, and on screen: the same line,
    // without the wait -- a player walking a menu does not hover.
    const bool focused = detail::pass_input() && navigation_enabled() &&
        detail::focus_visible() && detail::focused_id() == control;
    if (!rested && !focused) return;
    // The control has to be in this layout for Clay to attach to it; it
    // always is, having just been declared, unless Clay ran out of room.
    if (!Clay_GetElementData(control_id).found) return;

    const Theme &t = current_theme();
    const Clay_ElementId tip_id = detail::sub_id(control_id, detail::TOOLTIP_SUB);
    const float gap = px(t.gap / 2);

    // Its side and where along it, from last frame's boxes: unknown the frame
    // it first shows, so that one goes where it was asked.
    detail::TipPlace place{ o.side, Vector2{ 0, 0 } };
    Clay_BoundingBox was{};
    Clay_BoundingBox control_box{};
    if (detail::bounds_of_id(tip_id, &was) &&
        detail::bounds_of_id(control_id, &control_box)) {
        const Clay_Dimensions screen = detail::viewport();
        place = detail::place_tooltip(Rectangle{ control_box.x, control_box.y,
                                                 control_box.width, control_box.height },
                                      Vector2{ was.width, was.height }, o.side,
                                      Vector2{ screen.width, screen.height }, gap);
    }
    const Attach a = attach_for(place.side);

    Clay_ElementDeclaration d{};
    d.layout.sizing.width.type = CLAY__SIZING_TYPE_FIT;
    d.layout.sizing.width.size.minMax = Clay_SizingMinMax{ 0, px(MAX_WIDTH) };
    d.layout.sizing.height.type = CLAY__SIZING_TYPE_FIT;
    d.layout.sizing.height.size.minMax = Clay_SizingMinMax{ 0, 0 };
    const auto pad_x = px16(t.padding_x / 2);
    const auto pad_y = px16(t.padding_y / 2);
    d.layout.padding = Clay_Padding{ pad_x, pad_x, pad_y, pad_y };
    d.backgroundColor = to_clay(t.surface);
    const float r = px(t.corner_radius / 2);
    d.cornerRadius = Clay_CornerRadius{ r, r, r, r };
    // An outline in either theme: it floats over whatever is there, and has
    // to be told apart from it.
    auto w = px16(1);
    if (w < 1) w = 1;
    d.border.color = to_clay(t.border);
    d.border.width = Clay_BorderWidth{ w, w, w, w, 0 };
    d.floating.attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID;
    d.floating.parentId = control;
    d.floating.attachPoints = Clay_FloatingAttachPoints{ a.tip, a.control };
    d.floating.offset =
        Clay_Vector2{ a.dx * gap + place.shift.x, a.dy * gap + place.shift.y };
    d.floating.zIndex = TOOLTIP_Z;
    d.floating.pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH;
    // Not clipped by a scroll area its control is in: it is about something
    // in the list, and may stand outside it.
    d.floating.clipTo = CLAY_CLIP_TO_NONE;

    Clay__OpenElementWithId(tip_id);
    Clay__ConfigureOpenElement(d);
    {
        Clay_TextElementConfig tc{};
        tc.textColor = to_clay(t.text);
        tc.fontSize = font_px(t.font_size_small);
        tc.wrapMode = CLAY_TEXT_WRAP_WORDS;
        // Copied: the caller's string may be a temporary.
        Clay__OpenTextElement(detail::intern(text), tc);
    }
    Clay__CloseElement();
}

} // namespace rmp::ui
