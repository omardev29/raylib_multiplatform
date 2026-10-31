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

#include <rlgl.h> // rlPushMatrix(): a moving element and what is inside it

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <optional>

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

Clay_BoundingBox push_scissor(ScissorStack &stack, Clay_BoundingBox box) {
    if (stack.depth > 0) {
        // What both let show. Two that do not meet leave a box with no area,
        // and raylib's scissor then clips everything, which is right.
        const Clay_BoundingBox &around =
            stack.boxes[static_cast<size_t>(stack.depth - 1)];
        const float left = std::fmax(box.x, around.x);
        const float top = std::fmax(box.y, around.y);
        const float right = std::fmin(box.x + box.width, around.x + around.width);
        const float bottom = std::fmin(box.y + box.height, around.y + around.height);
        box = Clay_BoundingBox{ left, top, right > left ? right - left : 0.0f,
                                bottom > top ? bottom - top : 0.0f };
    }
    if (stack.depth < MAX_SCISSORS) {
        stack.boxes[static_cast<size_t>(stack.depth++)] = box;
    } else {
        // Paired even past the limit, the way the clips and the grids are: the
        // close of one that was not pushed must not pop one that was.
        ++stack.overflow;
    }
    return box;
}

std::optional<Clay_BoundingBox> pop_scissor(ScissorStack &stack) {
    if (stack.overflow > 0) {
        --stack.overflow;
    } else if (stack.depth > 0) {
        --stack.depth;
    }
    if (stack.depth == 0) return std::nullopt;
    return stack.boxes[static_cast<size_t>(stack.depth - 1)];
}

// A slice of no length is "". Clay hands out one of length -1: a word too wide
// for its box, then a space and a newline (wrapped, the line after the word
// is the newline's, and Clay takes the space before it off a line that has
// nothing on it). -1 here wrote the terminator one byte in front of the
// buffer, every frame that text was drawn.
const char *cstr(Clay_StringSlice slice) {
    const int len = std::clamp<int>(slice.length, 0, SCRATCH - 1);
    if (len > 0) std::memcpy(scratch.text.data(), slice.chars, static_cast<size_t>(len));
    scratch.text[static_cast<size_t>(len)] = '\0';
    return scratch.text.data();
}

// --- the arithmetic of the looks: pure, and tested without a GPU ------------

rmp::Ref<DrawRecord> draw_record(const Clay_RenderCommand &cmd) {
    if (cmd.userData == nullptr || cmd.commandType == CLAY_RENDER_COMMAND_TYPE_TEXT)
        return {};
    auto &r = *static_cast<DrawRecord *>(cmd.userData);
    return r.magic == DRAW_MAGIC ? rmp::Ref<DrawRecord>(r) : rmp::Ref<DrawRecord>{};
}

rmp::Ref<const TextRecord> text_record(const Clay_RenderCommand &cmd) {
    if (cmd.userData == nullptr || cmd.commandType != CLAY_RENDER_COMMAND_TYPE_TEXT)
        return {};
    const auto &r = *static_cast<const TextRecord *>(cmd.userData);
    return r.magic == TEXT_MAGIC ? rmp::Ref<const TextRecord>(r)
                                 : rmp::Ref<const TextRecord>{};
}

Color faded(Color c, float alpha) {
    if (alpha >= 1.0f) return c;
    const float a = static_cast<float>(c.a) * std::clamp(alpha, 0.0f, 1.0f);
    return Color{ c.r, c.g, c.b, static_cast<unsigned char>(std::lround(a)) };
}

int picture_parts(const DrawRecord &r, Rectangle dest, PictureParts &out) {
    if (dest.width <= 0 || dest.height <= 0) return 0;
    const Rectangle src = r.source;
    const bool nine = r.left > 0 || r.top > 0 || r.right > 0 || r.bottom > 0;
    if (!nine) {
        out[0] = PicturePart{ src, dest };
        return 1;
    }
    // The corners at their size, shrunk together when the box is too small
    // for them; the edges stretch along, the middle both ways.
    float l = r.left * r.slice_px;
    float rt = r.right * r.slice_px;
    float t = r.top * r.slice_px;
    float b = r.bottom * r.slice_px;
    if (l + rt > dest.width) {
        const float k = dest.width / (l + rt);
        l *= k;
        rt *= k;
    }
    if (t + b > dest.height) {
        const float k = dest.height / (t + b);
        t *= k;
        b *= k;
    }
    const std::array<float, 4> sx{ src.x, src.x + r.left, src.x + src.width - r.right,
                                   src.x + src.width };
    const std::array<float, 4> sy{ src.y, src.y + r.top, src.y + src.height - r.bottom,
                                   src.y + src.height };
    const std::array<float, 4> dx{ dest.x, dest.x + l, dest.x + dest.width - rt,
                                   dest.x + dest.width };
    const std::array<float, 4> dy{ dest.y, dest.y + t, dest.y + dest.height - b,
                                   dest.y + dest.height };
    int n = 0;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t col = 0; col < 3; ++col) {
            const Rectangle part{ sx[col], sy[row], sx[col + 1] - sx[col],
                                  sy[row + 1] - sy[row] };
            const Rectangle to{ dx[col], dy[row], dx[col + 1] - dx[col],
                                dy[row + 1] - dy[row] };
            if (part.width <= 0 || part.height <= 0 || to.width <= 0 || to.height <= 0)
                continue;
            out[static_cast<std::size_t>(n++)] = PicturePart{ part, to };
        }
    }
    return n;
}

int shadow_layers(const DrawRecord &r, Clay_BoundingBox box, float alpha,
                  ShadowLayers &out) {
    const int layers = r.shadow_blur > 0 ? SHADOW_LAYERS : 1;
    const Color c = faded(r.shadow_color, alpha);
    const float share = static_cast<float>(c.a) / static_cast<float>(layers);
    int n = 0;
    // Biggest and faintest first, so the edges add up to a fade.
    for (int i = layers - 1; i >= 0; --i) {
        const float grow = r.shadow_spread +
            r.shadow_blur * static_cast<float>(i + 1) / static_cast<float>(layers) -
            (layers > 1 ? r.shadow_blur * 0.5f : 0.0f);
        const Rectangle rect{ box.x + r.shadow_offset.x - grow,
                              box.y + r.shadow_offset.y - grow, box.width + 2 * grow,
                              box.height + 2 * grow };
        if (rect.width <= 0 || rect.height <= 0) continue;
        const Color layer{ c.r, c.g, c.b,
                           static_cast<unsigned char>(std::clamp(share, 1.0f, 255.0f)) };
        out[static_cast<std::size_t>(n++)] =
            ShadowLayer{ rect, std::max(0.0f, r.radius + grow), layer };
    }
    return n;
}

int outline_offsets(float width, OutlineOffsets &out) {
    // Eight copies around a letter, and eight more between them past two
    // pixels, where gaps between the first eight would show.
    const int steps = width > 2.0f ? 16 : 8;
    for (int i = 0; i < steps; ++i) {
        const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) /
            static_cast<float>(steps);
        out[static_cast<std::size_t>(i)] =
            Vector2{ std::cos(angle) * width, std::sin(angle) * width };
    }
    return steps;
}

bool inside(Clay_BoundingBox inner, Clay_BoundingBox outer) {
    constexpr float SLACK = 0.5f;
    return inner.x >= outer.x - SLACK && inner.y >= outer.y - SLACK &&
        inner.x + inner.width <= outer.x + outer.width + SLACK &&
        inner.y + inner.height <= outer.y + outer.height + SLACK;
}

int Motions::leave(Clay_BoundingBox box) {
    int left = 0;
    while (_depth > 0 && !inside(box, _boxes[static_cast<std::size_t>(_depth - 1)].box)) {
        left += _boxes[static_cast<std::size_t>(_depth - 1)].matrix ? 1 : 0;
        --_depth;
    }
    return left;
}

float Motions::alpha() const {
    return _depth > 0 ? _boxes[static_cast<std::size_t>(_depth - 1)].alpha : 1.0f;
}

bool Motions::enter(Clay_BoundingBox box, const DrawRecord &r) {
    const bool matrix = r.scale != 1.0f || r.offset.x != 0.0f || r.offset.y != 0.0f;
    if ((!matrix && r.alpha == 1.0f) || _depth >= MAX_MOTIONS) return false;
    _boxes[static_cast<std::size_t>(_depth++)] = Moving{ box, alpha() * r.alpha, matrix };
    return matrix;
}

int Motions::depth() const { return _depth; }

int Motions::matrices() const {
    int n = 0;
    for (int i = 0; i < _depth; ++i)
        n += _boxes[static_cast<std::size_t>(i)].matrix ? 1 : 0;
    return n;
}

namespace {

// A picture in a box, by its parts; lit by drawing it again, added on top.
void draw_picture(const DrawRecord &r, Rectangle dest, Color tint) {
    if (r.raw.id == 0) return;
    PictureParts parts{};
    const int n = picture_parts(r, dest, parts);
    for (int i = 0; i < n; ++i) {
        const PicturePart &p = parts[static_cast<std::size_t>(i)];
        DrawTexturePro(r.raw, p.source, p.dest, Vector2{ 0, 0 }, 0.0f, tint);
    }
    if (r.brighten > 0) {
        BeginBlendMode(BLEND_ADDITIVE);
        const auto a = static_cast<unsigned char>(
            std::lround(255.0f * r.brighten * tint.a / 255.0f));
        for (int i = 0; i < n; ++i) {
            const PicturePart &p = parts[static_cast<std::size_t>(i)];
            DrawTexturePro(r.raw, p.source, p.dest, Vector2{ 0, 0 }, 0.0f,
                           Color{ 255, 255, 255, a });
        }
        EndBlendMode();
    }
}

void draw_shadow(const DrawRecord &r, Clay_BoundingBox box, float alpha) {
    ShadowLayers layers{};
    const int n = shadow_layers(r, box, alpha, layers);
    for (int i = 0; i < n; ++i) {
        const ShadowLayer &l = layers[static_cast<std::size_t>(i)];
        const float half = std::min(l.rect.width, l.rect.height) * 0.5f;
        if (l.radius > 0 && half > 0) {
            DrawRectangleRounded(l.rect, std::min(1.0f, l.radius / half), 8, l.color);
        } else {
            DrawRectangleRec(l.rect, l.color);
        }
    }
}

void draw_text(const ::Font &f, Clay_StringSlice text, Vector2 at, float size,
               Color color, rmp::Ref<const TextRecord> extra, float alpha) {
    const float spacing = letter_spacing(size);
    if (extra && extra->shadow) {
        DrawTextEx(f, cstr(text),
                   Vector2{ at.x + extra->shadow_px.x, at.y + extra->shadow_px.y }, size,
                   spacing, faded(extra->shadow_color, alpha));
    }
    if (extra && extra->outline) {
        OutlineOffsets offsets{};
        const int n = outline_offsets(extra->outline_px, offsets);
        const Color c = faded(extra->outline_color, alpha);
        for (int i = 0; i < n; ++i) {
            const Vector2 o = offsets[static_cast<std::size_t>(i)];
            DrawTextEx(f, cstr(text), Vector2{ at.x + o.x, at.y + o.y }, size, spacing,
                       c);
        }
    }
    DrawTextEx(f, cstr(text), at, size, spacing, faded(color, alpha));
}

} // namespace

void draw(Clay_RenderCommandArray commands) {
    ScissorStack scissors{};
    const auto clip_to = [](Clay_BoundingBox b) {
        BeginScissorMode(static_cast<int>(b.x), static_cast<int>(b.y),
                         static_cast<int>(b.width), static_cast<int>(b.height));
    };
    Motions moving;
    // Already sorted by z order, so drawing them in sequence is correct.
    for (int32_t i = 0; i < commands.length; ++i) {
        const Clay_RenderCommand &cmd = commands.internalArray[i];
        const Rectangle rect = to_rect(cmd.boundingBox);

        // Out of the box of what was moving: no longer inside it.
        for (int left = moving.leave(cmd.boundingBox); left > 0; --left) rlPopMatrix();

        const rmp::Ref<DrawRecord> rec = draw_record(cmd);
        if (rec && !rec->shadow_drawn) {
            // The first command of a styled element: its motion on from here,
            // with everything drawn inside its box, and its shadow under it.
            rec->shadow_drawn = true;
            if (moving.enter(cmd.boundingBox, *rec)) {
                const float cx = rect.x + rect.width * 0.5f;
                const float cy = rect.y + rect.height * 0.5f;
                rlPushMatrix();
                rlTranslatef(cx + rec->offset.x, cy + rec->offset.y, 0.0f);
                rlScalef(rec->scale, rec->scale, 1.0f);
                rlTranslatef(-cx, -cy, 0.0f);
            }
            if (rec->has_shadow) draw_shadow(*rec, cmd.boundingBox, moving.alpha());
        }
        const float alpha = moving.alpha();

        switch (cmd.commandType) {
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                const auto &r = cmd.renderData.rectangle;
                const Color fill = faded(from_clay(r.backgroundColor), alpha);
                const Corners corners = corners_of(r.cornerRadius, cmd.boundingBox);
                // Four equal corners -- everything rmp::ui draws -- go through
                // raylib's own call, exactly as they always have. Only a box
                // given different corners, from Clay directly, is drawn here.
                if (!same_corners(corners)) {
                    fill_box(cmd.boundingBox, corners, fill);
                    break;
                }
                const float rn = roundness(corners, cmd.boundingBox);
                if (rn > 0.0f) {
                    DrawRectangleRounded(rect, rn, 8, fill);
                } else {
                    DrawRectangleRec(rect, fill);
                }
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_BORDER: {
                const auto &b = cmd.renderData.border;
                const Color color = faded(from_clay(b.color), alpha);
                const Corners corners = corners_of(b.cornerRadius, cmd.boundingBox);
                // The same, and the same for the widths: raylib's outline has
                // one width, and Clay gives each side its own.
                if (!same_corners(corners) || !same_sides(b.width)) {
                    stroke_box(cmd.boundingBox, corners, b.width, color);
                    break;
                }
                const float rn = roundness(corners, cmd.boundingBox);
                auto w = static_cast<float>(b.width.left);
                if (rn > 0.0f) {
                    DrawRectangleRoundedLinesEx(rect, rn, 8, w, color);
                } else {
                    DrawRectangleLinesEx(rect, w, color);
                }
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_TEXT: {
                const auto &t = cmd.renderData.text;
                if (t.stringContents.length <= 0) break; // see cstr()
                auto size = static_cast<float>(t.fontSize);
                // Baked at exactly this size, in the font this text asked for: sharp.
                const ::Font f = ui_font(size, t.fontId);
                draw_text(f, t.stringContents, Vector2{ rect.x, rect.y }, size,
                          from_clay(t.textColor), text_record(cmd), alpha);
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_IMAGE: {
                // A picture of ours -- image(), an icon, a skin -- comes with
                // its record: the part of the texture, the nine-slice margins,
                // the tint, the handles that keep it loaded.
                if (rec && rec->has_picture) {
                    draw_picture(*rec, rect, faded(rec->tint, alpha));
                    break;
                }
                // Anything else is a game driving Clay itself, and its
                // imageData is a pointer to a Texture2D it keeps alive for the
                // frame: drawn whole, untinted.
                const auto &img = cmd.renderData.image;
                if (img.imageData == nullptr) break;
                const auto *tex = static_cast<const Texture2D *>(img.imageData);
                const Rectangle src{ 0, 0, static_cast<float>(tex->width),
                                     static_cast<float>(tex->height) };
                DrawTexturePro(*tex, src, rect, Vector2{ 0, 0 }, 0.0f,
                               faded(WHITE, alpha));
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START: {
                // Cut to every scroll area around it: raylib has one scissor.
                clip_to(push_scissor(scissors, cmd.boundingBox));
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END: {
                // The one around it comes back; after the outermost, none.
                if (const std::optional<Clay_BoundingBox> around =
                        pop_scissor(scissors)) {
                    clip_to(*around);
                } else {
                    EndScissorMode();
                }
                break;
            }

            default:
                break;
        }
    }
    for (int left = moving.matrices(); left > 0; --left) rlPopMatrix();
    // A list that ended with one still open -- Clay pairs them, so never --
    // must not leave the rest of the game clipped.
    if (scissors.depth > 0) EndScissorMode();
}

} // namespace rmp::ui::detail
