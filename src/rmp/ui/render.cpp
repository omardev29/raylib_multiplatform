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

Rectangle to_rect(Clay_BoundingBox b) { return Rectangle{ b.x, b.y, b.width, b.height }; }

// raylib takes roundness as a 0..1 fraction of the shortest side, Clay gives
// pixels.
float roundness(Clay_CornerRadius r, Clay_BoundingBox b) {
    float shortest = (b.width < b.height ? b.width : b.height) * 0.5f;
    if (shortest <= 0.0f) return 0.0f;
    float radius = r.topLeft; // the UI never sets per-corner radii
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
                auto w = static_cast<float>(b.width.left);
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
                auto size = static_cast<float>(t.fontSize);
                DrawTextEx(f, cstr(t.stringContents), Vector2{ rect.x, rect.y }, size,
                           size / 10.0f, from_clay(t.textColor));
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_IMAGE: {
                // imageData is a pointer the element declaration passed through
                // untouched, so the contract is simply: point it at a Texture2D
                // you own and keep alive for the frame.
                //
                // rmp::ui::image() does not exist yet, but this is implemented
                // anyway because someone dropping to Clay directly can already
                // produce IMAGE commands, and silently drawing nothing would be
                // a worse answer than either supporting it or refusing it.
                const auto &img = cmd.renderData.image;
                if (img.imageData == nullptr) break;
                const auto *tex = static_cast<const Texture2D *>(img.imageData);
                Rectangle src{ 0, 0, static_cast<float>(tex->width),
                               static_cast<float>(tex->height) };
                // The tint comes through userData as an optional Color*, not
                // through backgroundColor: a background on an image element
                // makes Clay emit a RECTANGLE as well, after the IMAGE, which
                // paints a flat square over the picture. Null means untinted.
                auto tint = WHITE;
                if (cmd.userData != nullptr)
                    tint = *static_cast<const Color *>(cmd.userData);
                DrawTexturePro(*tex, src, rect, Vector2{ 0, 0 }, 0.0f, tint);
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START: {
                BeginScissorMode(static_cast<int>(rect.x), static_cast<int>(rect.y),
                                 static_cast<int>(rect.width),
                                 static_cast<int>(rect.height));
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
