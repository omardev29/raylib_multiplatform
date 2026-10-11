// ===========================================================================
// at(): a box over a point of the world.
//
// The public half is the template in rmp/ui.h. The camera it projects
// through is the scene's, handed over by the scene stack before each scene's
// _draw() (set_pass_camera(), ../ui_internal.h); without scenes the world is
// the screen.
//
// The box is a Clay floating element attached to the root of the layout, so
// it takes no room in the column it is described in and lands wherever the
// point is: its anchor on the projected point, moved by the offset in design
// units. Three things it decides, and why:
//
//   - It is drawn UNDER the rest of its pass. A box over the world belongs to
//     the world: a health bar that walks under the HUD's panel goes behind
//     it, as the enemy does. Clay draws floating elements by z, and the
//     pass's own tree is at 0, so an at() box takes a z below 0 -- one more
//     for each box, so they keep the order they were described in.
//   - What it paints does not take the pointer. A click on the enemy under
//     its health bar is the enemy's: world_passthrough() tells the claim loop
//     (context.cpp) which render commands came from inside an at(). Its
//     controls take the pointer as controls do, by their own hover and by
//     the claims focus.cpp records for them -- a button over an NPC's head
//     is clicked and focused where it is drawn, even when the at() is
//     described inside a scroll area: the box is not clipped by it, and
//     neither is what is in the box.
//   - A point the camera cannot show, or that is no number at all -- or an
//     offset that is none -- is kept far off the screen: Clay culls the box,
//     and nothing downstream ever converts a NaN or a 1e30 into an int.
//   - So is a point the screen does not show: the box goes with its point,
//     whole -- a point just past the edge left half a box showing -- and
//     nothing in it takes the pointer, the focus or the keys (pass_input(),
//     context.cpp, says no while one is open), nor anything in a box
//     described inside it. A button over an NPC that has walked off the
//     screen is not a button anyone can find, so it is not one to focus,
//     press or call out of reach (controls_out_of_reach(), focus.cpp, asks
//     only what can be focused). Back on the screen it is a control again,
//     held to the window like any other.
// ===========================================================================

#include "floating.h"
#include "internal.h"
#include "../internal.h"

#include <array>
#include <cmath>

namespace rmp::ui::detail {

namespace {

// The identity: the world is the screen. The dot says "file state".
struct {
    Camera2D camera{
        .offset = Vector2{ 0, 0 }, .target = Vector2{ 0, 0 }, .rotation = 0, .zoom = 1
    };
} projection;

// Where a point that cannot be shown is put, in pixels from the origin:
// further than any screen, and still a number a float holds to a fraction of
// a pixel.
constexpr float FAR_AWAY = 1.0e6f;

// The z of the first at() box of a pass. Every box after it takes the next
// one up, towards 0, which is where the pass's own tree is drawn. Past
// MAX_BOXES in one pass they share the last one: still under the pass, and
// still letting the pointer through.
constexpr int16_t FIRST_AT_Z = -32000;
constexpr int MAX_BOXES = 31999;

// The at() boxes open right now, innermost last: whether each one opened an
// element -- one called with no frame open did not, and must not close
// anything -- and the first layer z handed out inside it.
constexpr int MAX_DEPTH = 8;
struct Open {
    bool opened = false;
    int16_t first_layer = 0;
    bool hidden = false; // its point is off the screen, or the box around it is
};
struct {
    std::array<Open, MAX_DEPTH> stack{};
    int depth = 0;
    int overflow = 0; // nested past MAX_DEPTH: opened nothing, close nothing
    int hidden = 0; // how many of the open ones are hidden
} boxes;

// What the pass being described has handed out inside at(), for
// world_passthrough(): how many boxes -- their own z are FIRST_AT_Z and up --
// and the layer z of whatever floats inside one -- a stack() in a health bar
// -- as ranges. Consecutive boxes give consecutive ranges, which are merged,
// so a hundred enemies are one range. And the z the last box was given, as
// Clay was told it (last_at_z()).
constexpr int MAX_RANGES = 16;
struct LayerRange {
    int16_t first;
    int16_t last;
};
struct {
    int boxes = 0;
    std::array<LayerRange, MAX_RANGES> layers{};
    int count = 0;
    int16_t last_z = 0;
} given;

void forget_given() {
    given.boxes = 0;
    given.count = 0;
    given.last_z = 0;
}

int16_t box_z(int nth) {
    return static_cast<int16_t>(FIRST_AT_Z + (nth < MAX_BOXES ? nth : MAX_BOXES - 1));
}

void remember_layers(int16_t first, int16_t last) {
    if (given.count > 0 && given.layers[given.count - 1].last + 1 >= first) {
        given.layers[given.count - 1].last = last;
        return;
    }
    if (given.count < MAX_RANGES) {
        given.layers[given.count++] = LayerRange{ first, last };
        return;
    }
    // Out of room: the last range reaches over whatever was handed out in
    // between. That costs a layer() described between two at()s its claim
    // on the pointer -- never a health bar a claim of its own.
    given.layers[MAX_RANGES - 1].last = last;
}

Clay_FloatingAttachPointType attach_point(Align a) {
    // BOTTOM_CENTER is the default anchor; every other one is said below.
    Clay_FloatingAttachPointType p = CLAY_ATTACH_POINT_CENTER_BOTTOM;
    switch (a) {
        case Align::TOP_LEFT:
            p = CLAY_ATTACH_POINT_LEFT_TOP;
            break;
        case Align::TOP_CENTER:
            p = CLAY_ATTACH_POINT_CENTER_TOP;
            break;
        case Align::TOP_RIGHT:
            p = CLAY_ATTACH_POINT_RIGHT_TOP;
            break;
        case Align::CENTER_LEFT:
            p = CLAY_ATTACH_POINT_LEFT_CENTER;
            break;
        case Align::CENTER:
            p = CLAY_ATTACH_POINT_CENTER_CENTER;
            break;
        case Align::CENTER_RIGHT:
            p = CLAY_ATTACH_POINT_RIGHT_CENTER;
            break;
        case Align::BOTTOM_LEFT:
            p = CLAY_ATTACH_POINT_LEFT_BOTTOM;
            break;
        case Align::BOTTOM_CENTER:
            break;
        case Align::BOTTOM_RIGHT:
            p = CLAY_ATTACH_POINT_RIGHT_BOTTOM;
            break;
    }
    return p;
}

// One coordinate, kept where a float means something: a NaN goes far up and
// to the left, an infinity or a number past any screen to FAR_AWAY on its
// own side.
float kept(float v) {
    if (std::isnan(v)) return -FAR_AWAY;
    if (v > FAR_AWAY) return FAR_AWAY;
    if (v < -FAR_AWAY) return -FAR_AWAY;
    return v;
}

void push(Open o) {
    if (boxes.depth < MAX_DEPTH) {
        boxes.stack[boxes.depth++] = o;
        if (o.hidden) ++boxes.hidden;
    } else {
        ++boxes.overflow;
    }
}

// Whether the screen shows a point: on it, its edges included.
bool on_screen(Vector2 p) {
    const Clay_Dimensions view = viewport();
    return p.x >= 0 && p.y >= 0 && p.x <= view.width && p.y <= view.height;
}

} // namespace

void set_pass_camera(const Camera2D &camera) { projection.camera = camera; }
Camera2D pass_camera() { return projection.camera; }

Vector2 world_to_screen(Vector2 world, const Camera2D &camera) {
    const Vector2 p = GetWorldToScreen2D(world, camera);
    return Vector2{ kept(p.x), kept(p.y) };
}

int16_t last_at_z() { return given.last_z; }

bool inside_hidden_at() { return boxes.hidden > 0; }

void end_at_pass() { forget_given(); }

void open_at(Vector2 world, const AtOptions &o) {
    if (!frame_ready("at", o.id)) {
        push(Open{});
        return;
    }
    if (boxes.depth >= MAX_DEPTH) {
        // Its body goes into the box around it, which is still somewhere sane.
        RMP_REPORT_ONCE("UI: rmp::ui::at() nested more than %d deep; the ones past that "
                        "are laid out inside the one around them",
                        MAX_DEPTH);
        push(Open{});
        return;
    }
    // The layer z this box starts at; whatever floats inside it -- and the
    // marker close_at() takes -- comes after. The first one of a pass is 1:
    // anything remembered then is an earlier pass's, one that never ended.
    const int16_t first_layer = next_layer_z();
    if (first_layer == 1) forget_given();

    const Theme &t = current_theme();
    const Vector2 at = world_to_screen(world, projection.camera);
    const bool hidden = boxes.hidden > 0 || !on_screen(at);

    Clay_ElementDeclaration d{};
    // It fits what is in it -- and is never narrower than a touch target, so
    // a progress() bar, which fills the space it is given, has some: the
    // health bar of rmp/ui.h's own example.
    d.layout.sizing.width.type = CLAY__SIZING_TYPE_FIT;
    d.layout.sizing.width.size.minMax = Clay_SizingMinMax{ px(t.min_touch_size), 0 };
    d.layout.sizing.height.type = CLAY__SIZING_TYPE_FIT;
    d.layout.sizing.height.size.minMax = Clay_SizingMinMax{ 0, 0 };
    d.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    d.layout.childGap = px16(t.gap);
    d.layout.childAlignment =
        Clay_ChildAlignment{ CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER };
    // Attached to the root of the layout, whose corner is the screen's: its
    // anchor goes on the point, the offset in design units after it.
    d.floating.attachTo = CLAY_ATTACH_TO_ROOT;
    d.floating.attachPoints =
        Clay_FloatingAttachPoints{ attach_point(o.anchor), CLAY_ATTACH_POINT_LEFT_TOP };
    // Kept again after it: an offset can be no number too -- and one that is
    // none goes far away with the box, not px()'s 0, which would put the box
    // on the point as if nothing were wrong.
    const float scale = ui_scale();
    d.floating.offset = hidden ? Clay_Vector2{ -FAR_AWAY, -FAR_AWAY }
                               : Clay_Vector2{ kept(at.x + o.offset.x * scale),
                                               kept(at.y + o.offset.y * scale) };
    d.floating.zIndex = box_z(given.boxes);
    given.last_z = d.floating.zIndex;
    // Clay's own hit test is not ours (see pointer_over()); this says the same
    // thing to anyone driving Clay's.
    d.floating.pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH;
    d.floating.clipTo = CLAY_CLIP_TO_NONE;
    if (given.boxes < MAX_BOXES) ++given.boxes;

    if (!o.id.empty()) {
        Clay__OpenElementWithId(element_id(o.id, o.id));
    } else {
        Clay__OpenElement();
    }
    Clay__ConfigureOpenElement(d);
    // Out of the scroll area it may be described in, as the box itself is
    // (CLAY_CLIP_TO_NONE, attached to the root): 0 is "in none", so a button
    // over an NPC is hovered where it is drawn and not only where the list
    // around the at() happens to be.
    push_clip(Clay_ElementId{});
    push(Open{ .opened = true, .first_layer = first_layer, .hidden = hidden });
}

void close_at() {
    if (boxes.overflow > 0) {
        --boxes.overflow;
        return;
    }
    if (boxes.depth == 0) return;
    const Open o = boxes.stack[--boxes.depth];
    if (o.hidden) --boxes.hidden;
    if (!o.opened) return;
    // One more z marks where the inside ended; nobody else is handed it.
    remember_layers(o.first_layer, next_layer_z());
    pop_clip();
    close_element();
}

bool world_passthrough(const Clay_RenderCommand &command) {
    const int16_t z = command.zIndex;
    if (given.boxes > 0 && z >= FIRST_AT_Z && z <= box_z(given.boxes - 1)) return true;
    for (int i = 0; i < given.count; ++i) {
        if (z >= given.layers[i].first && z <= given.layers[i].last) return true;
    }
    return false;
}

} // namespace rmp::ui::detail
