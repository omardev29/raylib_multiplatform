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
// Containers
//
// Everything above arranges itself in a centred column, which is the right
// answer for a menu and the wrong one for anything with structure. These are
// how you get structure, and they compose:
//
//     rmp::ui::begin();
//     rmp::ui::panel([]{
//         rmp::ui::text("Really quit?");
//         rmp::ui::row([]{
//             if (rmp::ui::button("Yes")) quit();
//             rmp::ui::button("No");
//         });
//     });
//     rmp::ui::end();
//
// The contents are a lambda, and that is the whole reason there is no
// matching end() to forget: the compiler closes the container for you. Capture
// what you need with [&].
// ---------------------------------------------------------------------------

// Shared by every container. All measurements are in design units and are
// scaled; -1 means "whatever the theme says".
struct box_options {
    float gap     = -1;      // between children
    float padding = -1;      // inside this container
    align items   = align::center;   // where children sit in the leftover space
    bool  grow_x  = false;   // fill the parent's width instead of fitting content
    bool  grow_y  = false;
    float width   = 0;       // > 0 = a fixed width, overriding fit/grow
    float height  = 0;
    // Naming a container lets you ask about it later — whether the pointer is
    // over it, or where it ended up. Unnamed containers are anonymous, which is
    // what you want for the other 95%.
    const char *id = nullptr;
};

// A panel is a box with a background, which is what makes it visible.
struct panel_options {
    box_options box{};
    Color background = CLITERAL(Color){0, 0, 0, 0};  // {0,0,0,0} = the theme's panel
    float radius     = -1;                            // -1 = the theme's
    Color border     = CLITERAL(Color){0, 0, 0, 0};  // {0,0,0,0} = no border
    float border_width = -1;
};

// NOTE ON FIELD ORDER, here and in every options struct below. C++20 requires
// designated initialisers in declaration order: { .width = 8, .grow = true } is
// fine, { .grow = true, .width = 8 } does not compile. So these are ordered the
// way they are most likely to be written — sizing, then appearance, then
// identity — rather than alphabetically or by importance.
struct image_options {
    bool  grow   = false;   // fill the space available
    float width  = 0;       // otherwise: 0,0 = the texture's own size, scaled
    float height = 0;
    Color tint   = WHITE;
};

struct progress_options {
    float width  = 0;      // 0 = fill the space available
    float height = -1;     // -1 = derived from the theme's font size
    float radius = -1;
    Color fill   = CLITERAL(Color){0, 0, 0, 0};   // {0,0,0,0} = the theme's primary
    Color track  = CLITERAL(Color){0, 0, 0, 0};   // {0,0,0,0} = the theme's surface
    const char *id = nullptr;
};

namespace detail {
// Not for you: the non-template halves of the containers below, so that no
// Clay type has to appear in this header. See src/raylib_multiplatform/ui/.
void open_row(const box_options &o);
void open_column(const box_options &o);
void open_panel(const panel_options &o);
void open_center();
void open_stack();
void open_layer();
void close_element();

// Closes the element even if the body throws. Exceptions are usually off in a
// game, but a container left open would corrupt the whole frame, and that is
// too cheap to insure against not to.
struct closer {
    ~closer() { close_element(); }
};
} // namespace detail

// Left to right.
template <class Body> void row(const box_options &o, Body &&body) {
    detail::open_row(o);
    detail::closer close;
    body();
}
template <class Body> void row(Body &&body) { row(box_options{}, static_cast<Body &&>(body)); }

// Top to bottom.
template <class Body> void column(const box_options &o, Body &&body) {
    detail::open_column(o);
    detail::closer close;
    body();
}
template <class Body> void column(Body &&body) { column(box_options{}, static_cast<Body &&>(body)); }

// A column with a background and padding: the thing you put a dialog in.
template <class Body> void panel(const panel_options &o, Body &&body) {
    detail::open_panel(o);
    detail::closer close;
    body();
}
template <class Body> void panel(Body &&body) { panel(panel_options{}, static_cast<Body &&>(body)); }

// Takes all the space it is given and puts its contents in the middle of it.
template <class Body> void center(Body &&body) {
    detail::open_center();
    detail::closer close;
    body();
}

// Layers, drawn back to front. Each child has to be a layer():
//
//     rmp::ui::stack([&]{
//         rmp::ui::layer([&]{ rmp::ui::image(background); });
//         rmp::ui::layer([&]{ rmp::ui::text("PAUSED");   });
//     });
//
// The explicit layer() is not ceremony — it is what tells the layout which
// things are supposed to overlap and which are ordinary children.
template <class Body> void stack(Body &&body) {
    detail::open_stack();
    detail::closer close;
    body();
}
template <class Body> void layer(Body &&body) {
    detail::open_layer();
    detail::closer close;
    body();
}

// Eats the space left over, which is how you push things apart:
//
//     rmp::ui::row({ .grow_x = true }, []{
//         rmp::ui::text("Health");
//         rmp::ui::spacer();          // <- shoves the score to the right
//         rmp::ui::text("999");
//     });
void spacer();
void spacer(float fixed);   // or just a gap of a given size

// ---------------------------------------------------------------------------
// More widgets
// ---------------------------------------------------------------------------

// The texture has to stay alive until end() returns. By default it is drawn at
// its own size, scaled with the rest of the UI.
void image(const Texture2D &texture);
void image(const Texture2D &texture, const image_options &o);

// A bar. `fraction` is 0..1 and is clamped.
void progress(float fraction);
void progress(float fraction, const progress_options &o);

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
