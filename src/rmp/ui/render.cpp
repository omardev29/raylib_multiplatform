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

#include <array>
#include <cmath>
#include <cstring>
#include <numbers>

namespace rmp::ui::detail {

namespace {

// Clay's string slices are NOT null terminated — it slices the original buffer
// when wrapping text instead of cloning strings. raylib's text functions all
// want a terminator, so every slice passes through here first.
constexpr int SCRATCH = 1024;
struct {
    std::array<char, SCRATCH> text;
} scratch;

Rectangle to_rect(Clay_BoundingBox b) { return Rectangle{ b.x, b.y, b.width, b.height }; }

// raylib takes roundness as a 0..1 fraction of half the shortest side, Clay
// gives pixels. Only for four equal corners: see the RECTANGLE case.
float roundness(Corners c, Clay_BoundingBox b) {
    const float half = (b.width < b.height ? b.width : b.height) * 0.5f;
    if (half <= 0.0f) return 0.0f;
    return c.top_left / half;
}

bool same_corners(Corners c) {
    return c.top_left == c.top_right && c.top_left == c.bottom_right &&
        c.top_left == c.bottom_left;
}

bool same_sides(Clay_BorderWidth w) {
    return w.left == w.right && w.left == w.top && w.left == w.bottom;
}

// One triangle, turned to face the camera. rlgl culls back faces -- raylib
// asks for its triangles counter-clockwise on screen -- so one wound the
// other way would simply not be there. Slivers with no area are dropped.
void triangle(Vector2 a, Vector2 b, Vector2 c, Color color) {
    const float turn = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(turn) < 1e-4f) return;
    if (turn > 0.0f) {
        DrawTriangle(a, c, b, color);
    } else {
        DrawTriangle(a, b, c, color);
    }
}

// A box whose corners are not all the same: a fan from its middle to its
// outline. The outline is convex, so the triangles never overlap, and a
// translucent fill comes out the same colour everywhere.
void fill_box(Clay_BoundingBox b, Corners c, Color color) {
    Outline edge{};
    box_outline(b, c, Clay_BorderWidth{}, edge);
    const Vector2 middle{ b.x + b.width * 0.5f, b.y + b.height * 0.5f };
    for (int i = 0; i < BOX_OUTLINE; ++i) {
        triangle(middle, edge[i], edge[(i + 1) % BOX_OUTLINE], color);
    }
}

// Its border: the band between the box's outline and the same outline inset
// by each side's width, two triangles per pair of points.
void stroke_box(Clay_BoundingBox b, Corners c, Clay_BorderWidth w, Color color) {
    Outline outer{};
    Outline inner{};
    box_outline(b, c, Clay_BorderWidth{}, outer);
    box_outline(b, c, w, inner);
    for (int i = 0; i < BOX_OUTLINE; ++i) {
        const int next = (i + 1) % BOX_OUTLINE;
        triangle(outer[i], outer[next], inner[next], color);
        triangle(outer[i], inner[next], inner[i], color);
    }
}

} // namespace

Corners corners_of(Clay_CornerRadius r, Clay_BoundingBox b) {
    // Each one on its own. They used to be four copies of the top-left one,
    // so a panel rounded only along its bottom came out rounded everywhere or
    // nowhere.
    const float half = (b.width < b.height ? b.width : b.height) * 0.5f;
    auto clamp = [half](float radius) {
        if (radius < 0.0f || half <= 0.0f) return 0.0f;
        return radius > half ? half : radius;
    };
    return Corners{ clamp(r.topLeft), clamp(r.topRight), clamp(r.bottomRight),
                    clamp(r.bottomLeft) };
}

void box_outline(Clay_BoundingBox b, Corners c, Clay_BorderWidth inset, Outline &out) {
    // A side wider than half the box would turn the inside out.
    auto side = [](uint16_t width, float span) {
        const auto w = static_cast<float>(width);
        return w > span * 0.5f ? span * 0.5f : w;
    };
    const float left = side(inset.left, b.width);
    const float right = side(inset.right, b.width);
    const float top = side(inset.top, b.height);
    const float bottom = side(inset.bottom, b.height);

    struct Corner {
        float x, y; // the corner of the box
        float sx, sy; // which way is out, -1 or +1 on each axis
        float radius;
        float across, down; // the widths of the two sides that meet there
        float from; // the angle the arc starts at, in degrees
    };
    // Counter-clockwise on screen, each arc from one side to the next.
    const std::array<Corner, 4> corners{ {
        { b.x, b.y, -1, -1, c.top_left, left, top, 270 },
        { b.x, b.y + b.height, -1, 1, c.bottom_left, left, bottom, 180 },
        { b.x + b.width, b.y + b.height, 1, 1, c.bottom_right, right, bottom, 90 },
        { b.x + b.width, b.y, 1, -1, c.top_right, right, top, 0 },
    } };
    constexpr float DEGREES = std::numbers::pi_v<float> / 180.0f;
    int n = 0;
    for (const Corner &k : corners) {
        // The inside of a border follows the outside's curve, pulled in by the
        // width of each side: an ellipse when the two widths differ, and the
        // square corner of the inside once a width is past the radius.
        const float rx = k.radius > k.across ? k.radius - k.across : 0.0f;
        const float ry = k.radius > k.down ? k.radius - k.down : 0.0f;
        const float cx = k.x - k.sx * (k.radius > k.across ? k.radius : k.across);
        const float cy = k.y - k.sy * (k.radius > k.down ? k.radius : k.down);
        for (int i = 0; i < CORNER_POINTS; ++i) {
            const float angle =
                (k.from - 90.0f * static_cast<float>(i) / (CORNER_POINTS - 1)) * DEGREES;
            out[n++] = Vector2{ cx + rx * std::cos(angle), cy + ry * std::sin(angle) };
        }
    }
}

const char *cstr(Clay_StringSlice slice) {
    int len = slice.length;
    if (len >= SCRATCH) len = SCRATCH - 1;
    if (len > 0) std::memcpy(scratch.text.data(), slice.chars, static_cast<size_t>(len));
    scratch.text[len] = '\0';
    return scratch.text.data();
}

void draw(Clay_RenderCommandArray commands) {
    // Already sorted by z order, so drawing them in sequence is correct.
    for (int32_t i = 0; i < commands.length; ++i) {
        const Clay_RenderCommand &cmd = commands.internalArray[i];
        const Rectangle rect = to_rect(cmd.boundingBox);

        switch (cmd.commandType) {
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                const auto &r = cmd.renderData.rectangle;
                const Corners corners = corners_of(r.cornerRadius, cmd.boundingBox);
                // Four equal corners -- everything rmp::ui draws -- go through
                // raylib's own call, exactly as they always have. Only a box
                // given different corners, from Clay directly, is drawn here.
                if (!same_corners(corners)) {
                    fill_box(cmd.boundingBox, corners, from_clay(r.backgroundColor));
                    break;
                }
                const float rn = roundness(corners, cmd.boundingBox);
                if (rn > 0.0f) {
                    DrawRectangleRounded(rect, rn, 8, from_clay(r.backgroundColor));
                } else {
                    DrawRectangleRec(rect, from_clay(r.backgroundColor));
                }
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_BORDER: {
                const auto &b = cmd.renderData.border;
                const Corners corners = corners_of(b.cornerRadius, cmd.boundingBox);
                // The same, and the same for the widths: raylib's outline has
                // one width, and Clay gives each side its own.
                if (!same_corners(corners) || !same_sides(b.width)) {
                    stroke_box(cmd.boundingBox, corners, b.width, from_clay(b.color));
                    break;
                }
                const float rn = roundness(corners, cmd.boundingBox);
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
                auto size = static_cast<float>(t.fontSize);
                const ::Font f = ui_font(size); // baked at exactly this size: sharp
                DrawTextEx(f, cstr(t.stringContents), Vector2{ rect.x, rect.y }, size,
                           size / 10.0f, from_clay(t.textColor));
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_IMAGE: {
                // imageData is a pointer the element declaration passed through
                // untouched, so the contract is simply: point it at a Texture2D
                // you own and keep alive for the frame.
                //
                // rmp::ui::image() produces these, and so can a game that
                // drops to Clay directly: both point imageData at a Texture2D.
                const auto &img = cmd.renderData.image;
                if (img.imageData == nullptr) break;
                const auto *tex = static_cast<const Texture2D *>(img.imageData);
                const Rectangle src{ 0, 0, static_cast<float>(tex->width),
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
