// ===========================================================================
// The backdrop: what overlay() blurs.
//
// GLES2 cannot copy the framebuffer (rlBlitFramebuffer is GL 3.3 and ES 3
// only), so a frame that will be blurred is drawn into a texture from the
// start: begin_backdrop(), right after BeginDrawing(), when the last frame's
// overlay asked for a blur. The overlay then blurs what is in it and puts it
// on screen (blur_backdrop()); finish_backdrop() puts it there unblurred when
// no overlay took it. The hooks are declared in ../ui_internal.h and
// internal.h.
//
// As it stands nothing is redirected and nothing is blurred: every hook
// returns at once, and overlay() draws no backdrop at all.
// ===========================================================================

#include "backdrop.h"
#include "internal.h"

namespace rmp::ui::detail {

void begin_backdrop() {}
void finish_backdrop() {}
void blur_backdrop(float /*blur*/) {}
void release_backdrop() {}
bool backdrop_redirected() { return false; }

} // namespace rmp::ui::detail
