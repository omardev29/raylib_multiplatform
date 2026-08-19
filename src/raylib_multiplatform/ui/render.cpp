// ===========================================================================
// Render commands -> raylib.
//
// Clay does not draw: it produces a sorted list of "draw this rectangle here"
// and this file turns that into raylib calls. Which is the seam that makes the
// engine replaceable — everything above knows nothing about how a rectangle
// reaches the screen.
//
// Written by hand rather than vendoring Clay's own raylib renderer: that one is
// C, assumes a global array of fonts and carries paths we do not use. A second
// third-party file to keep in sync is not worth 120 lines.
// ===========================================================================

#include "internal.h"

#include <cstring>

namespace rmp::ui::detail {

namespace {

// Clay's string slices are NOT null terminated — it slices the original buffer
// when wrapping text instead of cloning strings. raylib's text functions all
// want a terminator, so every slice passes through here first.
constexpr int kScratch = 1024;
char g_scratch[kScratch];

Rectangle to_rect(Clay_BoundingBox b) {
    return Rectangle{ b.x, b.y, b.width, b.height };
}

// raylib takes roundness as a 0..1 fraction of the shortest side, Clay gives
// pixels.
float roundness(Clay_CornerRadius r, Clay_BoundingBox b) {
    float shortest = (b.width < b.height ? b.width : b.height) * 0.5f;
    if (shortest <= 0.0f) return 0.0f;
    float radius = r.topLeft;   // the UI never sets per-corner radii
    float value = radius / shortest;
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    return value;
}

} // namespace

const char *cstr(Clay_StringSlice slice) {
    int len = slice.length;
    if (len >= kScratch) len = kScratch - 1;
    if (len > 0) std::memcpy(g_scratch, slice.chars, static_cast<size_t>(len));
    g_scratch[len] = '\0';
    return g_scratch;
}

void draw(Clay_RenderCommandArray commands) {
    // Already sorted by z order, so drawing them in sequence is correct.
    for (int32_t i = 0; i < commands.length; i++) {
        const Clay_RenderCommand &cmd = commands.internalArray[i];
        Rectangle rect = to_rect(cmd.boundingBox);

        switch (cmd.commandType) {
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                const auto &r = cmd.renderData.rectangle;
                float rn = roundness(r.cornerRadius, cmd.boundingBox);
                if (rn > 0.0f) {
                    DrawRectangleRounded(rect, rn, 8, from_clay(r.backgroundColor));
                } else {
                    DrawRectangleRec(rect, from_clay(r.backgroundColor));
                }
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_BORDER: {
                const auto &b = cmd.renderData.border;
                float rn = roundness(b.cornerRadius, cmd.boundingBox);
                float w  = static_cast<float>(b.width.left);
                if (rn > 0.0f) {
                    DrawRectangleRoundedLinesEx(rect, rn, 8, w, from_clay(b.color));
                } else {
                    DrawRectangleLinesEx(rect, w, from_clay(b.color));
                }
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_TEXT: {
                const auto &t = cmd.renderData.text;
                Font f = ui_font();
                float size = static_cast<float>(t.fontSize);
                DrawTextEx(f, cstr(t.stringContents), Vector2{ rect.x, rect.y },
                           size, size / 10.0f, from_clay(t.textColor));
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_IMAGE: {
                // Phase 2. Clay carries the texture through as userData; until
                // rmp::ui::image() exists there is nothing to draw.
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START: {
                BeginScissorMode(static_cast<int>(rect.x), static_cast<int>(rect.y),
                                 static_cast<int>(rect.width), static_cast<int>(rect.height));
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END: {
                EndScissorMode();
                break;
            }

            default:
                break;
        }
    }
}

} // namespace rmp::ui::detail
