#pragma once
// ---------------------------------------------------------------------------
// rmp::ui — the interface layer.
//
// The whole point, in four lines:
//
//     rmp::ui::begin();
//     if (rmp::ui::button("Play")) play();
//     if (rmp::ui::button("Quit")) quit();
//     rmp::ui::end();
//
// That is a centred, responsive main menu. No coordinates, no sizes, no
// anchors, no fonts, no hitboxes, and it looks the same at 800x600 as it does
// at 4K. Everything else in this header is what you reach for when the default
// is not what you want — and you only pay for it when you use it.
//
// Three rules worth knowing, and that is genuinely all of them:
//
//   1. end() draws. So the begin()/end() pair goes between BeginDrawing() and
//      EndDrawing(), and before SmokeTest_CaptureFrame() if you keep that.
//   2. One UI frame per game frame.
//   3. Interaction uses the previous frame's geometry. A button cannot be
//      clicked on the very first frame it appears — 16 ms at 60 fps. This is
//      inherent to immediate mode; see TECHNICAL.md for why the alternative is
//      worse.
//
// The layout engine underneath is Clay, and it is deliberately invisible: not
// one of its types appears in this header, so it can be replaced without any
// of your code changing.
// ---------------------------------------------------------------------------

#include <raylib.h>

// APP_UI_FONT_SIZE, so the theme's default type size is the one you set in
// [ui] rather than a number baked into this header.
#include <raylib_multiplatform/generated/app_config.h>

#include <string_view>

#ifndef APP_UI_FONT_SIZE
#define APP_UI_FONT_SIZE 20
#endif

namespace rmp::ui {

// ---------------------------------------------------------------------------
// Vocabulary
// ---------------------------------------------------------------------------

// Where content sits inside the space it was given.
enum class align {
    top_left,    top_center,    top_right,
    center_left, center,        center_right,
    bottom_left, bottom_center, bottom_right,
};

// What a control *means*, not what colour it is. The theme decides the colour,
// so restyling the game never means revisiting every call site.
enum class variant { normal, primary, danger };

// Which colour of the theme a piece of text uses.
enum class color_role { text, muted, primary, danger };

// ---------------------------------------------------------------------------
// The theme
//
// Plain data. Copy it, change what you want, set it back:
//
//     auto t = rmp::ui::current_theme();
//     t.primary = GOLD;
//     rmp::ui::set_theme(t);
//
// Colours are raylib's Color, because you already have RED and CLITERAL and
// there is no reason to invent a second one.
//
// Every metric is in DESIGN UNITS, not pixels: they are multiplied by the
// current scale before anything is drawn. See scale() below.
// ---------------------------------------------------------------------------

struct theme {
    Color background     = CLITERAL(Color){  18,  18,  22, 255 };
    Color panel          = CLITERAL(Color){  30,  30,  38, 255 };
    Color surface        = CLITERAL(Color){  44,  44,  56, 255 };
    Color surface_hover  = CLITERAL(Color){  60,  60,  76, 255 };
    Color surface_press  = CLITERAL(Color){  26,  26,  34, 255 };
    Color border         = CLITERAL(Color){  70,  70,  88, 255 };

    Color text           = CLITERAL(Color){ 235, 235, 242, 255 };
    Color text_muted     = CLITERAL(Color){ 150, 150, 168, 255 };
    Color text_on_accent = CLITERAL(Color){ 255, 255, 255, 255 };

    Color primary        = CLITERAL(Color){  88, 120, 245, 255 };
    Color primary_hover  = CLITERAL(Color){ 110, 140, 255, 255 };
    Color primary_press  = CLITERAL(Color){  70,  98, 210, 255 };

    Color danger         = CLITERAL(Color){ 220,  72,  80, 255 };
    Color danger_hover   = CLITERAL(Color){ 236,  96, 104, 255 };
    Color danger_press   = CLITERAL(Color){ 184,  56,  64, 255 };

    Color disabled       = CLITERAL(Color){  60,  60,  70, 255 };
    Color disabled_text  = CLITERAL(Color){ 110, 110, 124, 255 };

    float font_size       = APP_UI_FONT_SIZE;         // [ui] font_size
    float font_size_small = APP_UI_FONT_SIZE * 0.8f;
    float padding_x       = 20;   // inside a button
    float padding_y       = 12;
    float gap             = 12;   // between siblings
    float panel_padding   = 20;
    float corner_radius   = 8;
    float border_width    = 0;    // 0 = the default theme draws no borders
    // No control is ever shorter than this. 44 design units is Apple's touch
    // target guidance and close to Material's 48dp — it is the difference
    // between a menu you can use with a thumb and one you cannot.
    float min_touch_size  = 44;
};

const theme &current_theme();
void set_theme(const theme &t);

// ---------------------------------------------------------------------------
// Scale
//
// The one thing that makes "responsive" mean something. Every theme metric is
// multiplied by this before use, so the same menu is comfortable on a phone
// and on a 4K monitor without your code knowing which it is.
//
// It is derived from the design resolution you already declared in [window] in
// raylib_multiplatform.toml:
//
//     scale = clamp(min(w / APP_WINDOW_WIDTH, h / APP_WINDOW_HEIGHT), 0.5, 4)
//
// min() and not max(): what does not fit is worse than what is left over.
// ---------------------------------------------------------------------------

// The factor in use right now.
float scale();

// Pin it. 0 goes back to automatic. This is how you would implement an
// "interface size" accessibility option.
void set_scale(float s);

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

struct frame_options {
    align placement = align::center;  // where the root's content sits
    float gap       = -1;             // between children; -1 = the theme's
    float padding    = -1;            // inside the root; -1 = the theme's
};

// Open the UI for this frame. The default is a centred column, which is what
// makes a main menu three functions.
void begin();
void begin(const frame_options &o);

// Lay out, resolve interaction, and draw. Call it between BeginDrawing() and
// EndDrawing().
void end();

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

struct button_options {
    variant     style   = variant::normal;
    bool        enabled = true;
    // Only needed when two buttons share a label AND the UI is conditional.
    // Identical labels in one frame are already told apart automatically.
    const char *id      = nullptr;
};

struct text_options {
    color_role color = color_role::text;
    float      size  = -1;      // -1 = the theme's font_size
    bool       wrap  = true;
};

// True on the frame the pointer is released over it. Reads exactly as it looks:
//
//     if (rmp::ui::button("Play")) play();
//
bool button(std::string_view label);
bool button(std::string_view label, const button_options &o);

// The string is copied immediately, so a temporary is safe:
//
//     rmp::ui::text(std::to_string(score));
//
void text(std::string_view s);
void text(std::string_view s, const text_options &o);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Called for you by the entry point macro, before _exit() — while the window
// is still open, because releasing a font after CloseWindow() would be
// touching a GL context that no longer exists.
//
// There is no init(): the UI starts itself on the first begin(), by which time
// there is a window. A game that never draws UI allocates nothing.
void shutdown();

} // namespace rmp::ui
