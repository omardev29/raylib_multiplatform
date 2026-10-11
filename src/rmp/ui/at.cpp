// ===========================================================================
// at(): a box over a point of the world.
//
// The public half is the template in rmp/ui.h. The camera it projects
// through is the scene's, handed over by the scene stack before each scene's
// _draw() (set_pass_camera(), ../ui_internal.h); without scenes the world is
// the screen.
//
// As it stands the box is a plain column wherever at() is called, and what it
// paints takes the pointer like anything else: the point is not projected yet.
// ===========================================================================

#include "floating.h"
#include "internal.h"

namespace rmp::ui::detail {

namespace {

// The identity: the world is the screen. The dot says "file state".
struct {
    Camera2D camera{
        .offset = Vector2{ 0, 0 }, .target = Vector2{ 0, 0 }, .rotation = 0, .zoom = 1
    };
} projection;

} // namespace

void set_pass_camera(const Camera2D &camera) { projection.camera = camera; }
Camera2D pass_camera() { return projection.camera; }

void open_at(Vector2 /*world*/, const AtOptions &o) {
    open_column(BoxOptions{ .id = o.id });
}
void close_at() { close_element(); }

bool world_passthrough(const Clay_RenderCommand & /*command*/) { return false; }

} // namespace rmp::ui::detail
