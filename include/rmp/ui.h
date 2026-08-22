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

// APP_UI_FONT_SIZE, so the theme's default type Size is the one you set in
// [ui] rather than a number baked into this header.
#include <rmp/config.h>

#include <string_view>

#ifndef APP_UI_FONT_SIZE
#define APP_UI_FONT_SIZE 20
#endif

namespace rmp::ui {

// ---------------------------------------------------------------------------
// Vocabulary
// ---------------------------------------------------------------------------

// Where content sits inside the space it was given.
// Read them as a 3x3 grid: three verticals (top/center/bottom) crossed with
// three horizontals (left/center/right).
enum class Align {
    TOP_LEFT,
    TOP_CENTER,
    TOP_RIGHT,
    CENTER_LEFT,
    CENTER,
    CENTER_RIGHT,
    BOTTOM_LEFT,
    BOTTOM_CENTER,
    BOTTOM_RIGHT,
};

// What a control *means*, not what colour it is. The theme decides the colour,
// so restyling the game never means revisiting every call site.
//
//   normal    a filled surface: the default, and most buttons
//   primary   the one thing you want pressed on this screen
//   danger    destructive, and it should look like it
//   outline   an outline and a label, no fill: a secondary action
//   ghost     just the label until you point at it: toolbars, "back" links
enum class Variant { NORMAL, PRIMARY, DANGER, OUTLINE, GHOST };

// Which colour of the theme a piece of text uses.
enum class ColorRole { TEXT, MUTED, PRIMARY, DANGER };

// The three type steps. Anywhere a size is asked for you can write one of
// these or a plain number of design units — it is the same field either way:
//
//     rmp::ui::text("Chapter One", { .size = rmp::ui::Size::LARGE });
//     rmp::ui::text("Chapter One", { .size = 34 });
//
// The steps are what you want almost always: they come from the theme, so they
// move together when someone changes the type scale, and they stay in
// proportion at every screen Size. A number is the escape hatch.
enum class Size { SMALL, MEDIUM, LARGE };

namespace detail {
// What an options struct stores for a size. You never write this type's name —
// you write `rmp::ui::Size::LARGE` or `34`, and both land here. It is one field
// that takes two spellings, rather than two fields that can disagree.
struct Sizing {
    float units = -1; // > 0 = design units; -1 = the theme's
    ui::Size step = ui::Size::MEDIUM;
    bool named = false;

    constexpr Sizing() = default;
    constexpr Sizing(float u) : units(u) {}
    constexpr Sizing(ui::Size s) : step(s), named(true) {}
};
} // namespace detail

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

struct Theme {
    Color background = CLITERAL(Color){ 18, 18, 22, 255 };
    Color panel = CLITERAL(Color){ 30, 30, 38, 255 };
    Color surface = CLITERAL(Color){ 44, 44, 56, 255 };
    Color surface_hover = CLITERAL(Color){ 60, 60, 76, 255 };
    Color surface_press = CLITERAL(Color){ 26, 26, 34, 255 };
    Color border = CLITERAL(Color){ 70, 70, 88, 255 };

    Color text = CLITERAL(Color){ 235, 235, 242, 255 };
    Color text_muted = CLITERAL(Color){ 150, 150, 168, 255 };
    Color text_on_accent = CLITERAL(Color){ 255, 255, 255, 255 };

    Color primary = CLITERAL(Color){ 88, 120, 245, 255 };
    Color primary_hover = CLITERAL(Color){ 110, 140, 255, 255 };
    Color primary_press = CLITERAL(Color){ 70, 98, 210, 255 };

    Color danger = CLITERAL(Color){ 220, 72, 80, 255 };
    Color danger_hover = CLITERAL(Color){ 236, 96, 104, 255 };
    Color danger_press = CLITERAL(Color){ 184, 56, 64, 255 };

    Color disabled = CLITERAL(Color){ 60, 60, 70, 255 };
    Color disabled_text = CLITERAL(Color){ 110, 110, 124, 255 };

    // The outline on whatever the keyboard or gamepad is pointing at. Without
    // it a controller build is unusable, so it is a theme colour rather than
    // something each widget decides.
    Color focus = CLITERAL(Color){ 130, 170, 255, 255 };

    float font_size = APP_UI_FONT_SIZE; // [ui] font_size
    float font_size_small = APP_UI_FONT_SIZE * 0.8f; // Size::SMALL
    float font_size_large = APP_UI_FONT_SIZE * 1.4f; // Size::LARGE
    float padding_x = 20; // inside a button
    float padding_y = 12;
    float gap = 12; // between siblings
    float panel_padding = 20;
    float corner_radius = 8;
    float border_width = 0; // 0 = the default Theme draws no borders
    float control_size = 22; // a checkbox box, a slider handle
    float track_thickness = 6; // a slider's rail
    float focus_ring = 2; // the outline on the focused control
    // How long a control takes to reach its new colour, in seconds. Nothing
    // about the layout moves — only colour — so this can never make a button
    // arrive somewhere else than where you clicked. 0 turns it off, which is
    // both the "reduce motion" setting and what a test wants.
    float transition = 0.12f;
    // No control is ever shorter than this. 44 design units is Apple's touch
    // target guidance and close to Material's 48dp — it is the difference
    // between a menu you can use with a thumb and one you cannot.
    float min_touch_size = 44;
};

const Theme &current_theme();
void set_theme(const Theme &t);

// The two that come with the framework. Which one starts is [ui] Theme in
// raylib_multiplatform.toml; these let you switch at runtime, which is what an
// in-game "appearance" setting does:
//
//     if (rmp::ui::checkbox("Light Theme", &light))
//         rmp::ui::set_theme(light ? rmp::ui::theme_light() : rmp::ui::theme_dark());
//
// They return a copy, so the usual copy-modify-set still applies on top.
Theme theme_dark();
Theme theme_light();

// ---------------------------------------------------------------------------
// Scale
//
// The one thing that makes "responsive" mean something. Every Theme metric is
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
// "interface Size" accessibility option.
void set_scale(float s);

// ---------------------------------------------------------------------------
// Breakpoints
//
// Scale already handles "make everything bigger". This is for the one thing
// scale cannot do: change the SHAPE of a layout. No amount of grow, fit or
// fixed Sizing turns a row into a column, and on a phone held upright a
// sidebar-and-content row has to become a column or it is unusable.
//
//     if (rmp::ui::compact()) rmp::ui::column([&]{ side(); main_area(); });
//     else                    rmp::ui::row   ([&]{ side(); main_area(); });
//
// The classification is by ASPECT RATIO, not by pixels, and that is deliberate.
// Pixel thresholds are a lie on a phone — a 1080-pixel-wide screen four inches
// across is not a desktop — and scale() has already normalised how big things
// are. What is left over, and the only thing that decides whether a row fits,
// is how wide the viewport is relative to how tall it is.
//
//   compact    taller than it is wide      phone upright, a narrow window
//   medium     up to 1.6:1                 tablet, a small desktop window
//   expanded   wider than 1.6:1            a normal desktop, a TV
//
// Reach for it only when the layout genuinely has to change shape. Reaching for
// it to pick sizes means undoing the work scale() already did for you.
// ---------------------------------------------------------------------------

enum class Breakpoint { COMPACT, MEDIUM, EXPANDED };

Breakpoint current_breakpoint();

// Shorthand for the case that comes up: current_breakpoint() == compact.
bool compact();

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

struct FrameOptions {
    Align placement = Align::CENTER; // where the root's content sits
    float gap = -1; // between children; -1 = the theme's
    float padding = -1; // inside the root; -1 = the theme's
};

// Open the UI for this frame. The default is a centred column, which is what
// makes a main menu three functions.
void begin();
void begin(const FrameOptions &o);

// Lay out, resolve interaction, and draw. Call it between BeginDrawing() and
// EndDrawing().
void end();

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

struct ButtonOptions {
    Variant style = Variant::NORMAL;
    // One of the three steps, or a number of design units. The padding and the
    // minimum touch height follow the type Size, so a large button is a large
    // button all over rather than a normal one with bigger letters.
    detail::Sizing size{};
    bool enabled = true;
    // Only needed when two buttons share a label AND the UI is conditional.
    // Identical labels in one frame are already told apart automatically.
    const char *id = nullptr;
};

struct TextOptions {
    ColorRole color = ColorRole::TEXT;
    detail::Sizing size{}; // a step, a number, or nothing for the theme's
    bool wrap = true;
};

// True on the frame the pointer is released over it. Reads exactly as it looks:
//
//     if (rmp::ui::button("Play")) play();
//
bool button(std::string_view label);
bool button(std::string_view label, const ButtonOptions &o);

// The string is copied immediately, so a temporary is safe:
//
//     rmp::ui::text(std::to_string(score));
//
void text(std::string_view s);
void text(std::string_view s, const TextOptions &o);

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
struct BoxOptions {
    float gap = -1; // between children
    float padding = -1; // inside this container
    Align items = Align::CENTER; // where children sit in the leftover space
    // Sizing is interleaved by axis — x then y — rather than grouped by kind,
    // and that is not cosmetic. Designated initialisers have to be written in
    // declaration order, so grouping them as grow_x, grow_y, width, height
    // would make { .width = 240, .grow_y = true } illegal: a fixed-width
    // sidebar that fills the height, which is about the most ordinary thing
    // anyone writes. This way round, every combination is legal.
    bool grow_x = false; // fill the parent's width instead of fitting content
    float width = 0; // > 0 = a fixed width, overriding fit/grow
    bool grow_y = false;
    float height = 0;
    // Naming a container lets you ask about it later — whether the pointer is
    // over it, or where it ended up. Unnamed containers are anonymous, which is
    // what you want for the other 95%.
    const char *id = nullptr;
};

// A panel is a box with a background, which is what makes it visible.
struct PanelOptions {
    BoxOptions box{};
    Color background = CLITERAL(Color){ 0, 0, 0, 0 }; // {0,0,0,0} = the theme's panel
    float radius = -1; // -1 = the theme's
    Color border = CLITERAL(Color){ 0, 0, 0, 0 }; // {0,0,0,0} = no border
    float border_width = -1;
};

// NOTE ON FIELD ORDER, here and in every options struct below. C++20 requires
// designated initialisers in declaration order: { .width = 8, .grow = true } is
// fine, { .grow = true, .width = 8 } does not compile. So these are ordered the
// way they are most likely to be written — Sizing, then appearance, then
// identity — rather than alphabetically or by importance.
struct ImageOptions {
    bool grow = false; // fill the space available
    float width = 0; // otherwise: 0,0 = the texture's own Size, scaled
    float height = 0;
    Color tint = WHITE;
};

struct ProgressOptions {
    float width = 0; // 0 = fill the space available
    float height = -1; // -1 = derived from the theme's font Size
    float radius = -1;
    Color fill = CLITERAL(Color){ 0, 0, 0, 0 }; // {0,0,0,0} = the theme's primary
    Color track = CLITERAL(Color){ 0, 0, 0, 0 }; // {0,0,0,0} = the theme's surface
    const char *id = nullptr;
};

namespace detail {
// Not for you: the non-template halves of the containers below, so that no
// Clay type has to appear in this header. See src/rmp/ui/.
void open_row(const BoxOptions &o);
void open_column(const BoxOptions &o);
void open_panel(const PanelOptions &o);
void open_center();
void open_stack();
void open_layer();
void close_element();

// Closes the element even if the body throws. Exceptions are usually off in a
// game, but a container left open would corrupt the whole frame, and that is
// too cheap to insure against not to.
struct Closer {
    ~Closer() { close_element(); }
};
} // namespace detail

// Left to right.
template <class Body> void row(const BoxOptions &o, Body &&body) {
    detail::open_row(o);
    detail::Closer close;
    body();
}
template <class Body> void row(Body &&body) {
    row(BoxOptions{}, static_cast<Body &&>(body));
}

// Top to bottom.
template <class Body> void column(const BoxOptions &o, Body &&body) {
    detail::open_column(o);
    detail::Closer close;
    body();
}
template <class Body> void column(Body &&body) {
    column(BoxOptions{}, static_cast<Body &&>(body));
}

// A column with a background and padding: the thing you put a dialog in.
template <class Body> void panel(const PanelOptions &o, Body &&body) {
    detail::open_panel(o);
    detail::Closer close;
    body();
}
template <class Body> void panel(Body &&body) {
    panel(PanelOptions{}, static_cast<Body &&>(body));
}

// Takes all the space it is given and puts its contents in the middle of it.
template <class Body> void center(Body &&body) {
    detail::open_center();
    detail::Closer close;
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
    detail::Closer close;
    body();
}
template <class Body> void layer(Body &&body) {
    detail::open_layer();
    detail::Closer close;
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
void spacer(float fixed); // or just a gap of a given size

// ---------------------------------------------------------------------------
// More widgets
// ---------------------------------------------------------------------------

// The texture has to stay alive until end() returns. By default it is drawn at
// its own Size, scaled with the rest of the UI.
void image(const Texture2D &texture);
void image(const Texture2D &texture, const ImageOptions &o);

// A bar. `fraction` is 0..1 and is clamped.
void progress(float fraction);
void progress(float fraction, const ProgressOptions &o);

// ---------------------------------------------------------------------------
// Grid and scroll
// ---------------------------------------------------------------------------

struct GridOptions {
    int columns = 0; // 0 = as many as fit, recomputed as the window changes
    float min_cell = 96; // only used when columns = 0
    float gap = -1;
    float padding = -1;
    bool grow_x = true; // a grid almost always wants the width it is offered
    bool grow_y = false;
    const char *id = nullptr;
};

struct ScrollOptions {
    bool horizontal = false; // vertical by default, which is what lists want
    bool vertical = true;
    float gap = -1;
    float padding = -1;
    bool grow_x = true;
    float width = 0;
    bool grow_y = true; // a scroll area with no height clips nothing
    float height = 0;
    const char *id = nullptr;
};

namespace detail {
void open_grid(const GridOptions &o);
void close_grid();
void open_cell();
void close_cell();
void open_scroll(const ScrollOptions &o);

struct GridCloser {
    ~GridCloser() { close_grid(); }
};
struct CellCloser {
    ~CellCloser() { close_cell(); }
};
} // namespace detail

// Equal columns, as many rows as it takes. Each item goes in a cell(), which is
// what lets the grid count them and start a new row at the right moment — the
// layout engine underneath wraps text, not elements, so the rows are real rows.
//
//     rmp::ui::grid(4, [&]{
//         for (auto &item : inventory)
//             rmp::ui::cell([&]{ rmp::ui::image(item.icon); });
//     });
//
// With columns = 0 it works out how many fit in the width it has and works it
// out again when the window changes, which is what an inventory should do
// rather than staying at the number someone typed on a desktop.
template <class Body> void grid(const GridOptions &o, Body &&body) {
    detail::open_grid(o);
    detail::GridCloser close;
    body();
}

// One item in a grid. Every child of a grid has to be one.
template <class Body> void cell(Body &&body) {
    detail::open_cell();
    detail::CellCloser close;
    body();
}
template <class Body> void grid(int columns, Body &&body) {
    grid(GridOptions{ .columns = columns }, static_cast<Body &&>(body));
}
template <class Body> void grid(Body &&body) {
    grid(GridOptions{}, static_cast<Body &&>(body));
}

// Clips its contents and scrolls them. The wheel works, and so does dragging
// with a finger — the same gesture on a phone.
template <class Body> void scroll(const ScrollOptions &o, Body &&body) {
    detail::open_scroll(o);
    detail::Closer close;
    body();
}
template <class Body> void scroll(Body &&body) {
    scroll(ScrollOptions{}, static_cast<Body &&>(body));
}

// ---------------------------------------------------------------------------
// Controls that own a value
//
// Each one takes a pointer to YOUR variable and writes to it. That is the whole
// state model: there is nothing of ours to keep in sync, and the value on
// screen is the value in your struct because it was read this frame.
//
// They return true on the frame the value changed, so this reads the way it
// looks:
//
//     if (rmp::ui::checkbox("Fullscreen", &settings.fullscreen)) apply();
// ---------------------------------------------------------------------------

struct CheckboxOptions {
    bool enabled = true;
    const char *id = nullptr;
};

struct SliderOptions {
    float width = 0; // 0 = fill the space available
    float step = 0; // 0 = continuous; otherwise snap to multiples
    bool enabled = true;
    bool show_value = true; // draw the number next to the label
    const char *id = nullptr;
};

struct DropdownOptions {
    float width = 0;
    bool enabled = true;
    const char *id = nullptr;
};

struct TextInputOptions {
    float width = 0;
    bool enabled = true;
    std::string_view placeholder{};
    const char *id = nullptr;
};

bool checkbox(std::string_view label, bool *value);
bool checkbox(std::string_view label, bool *value, const CheckboxOptions &o);

bool slider(std::string_view label, float *value, float min, float max);
bool slider(std::string_view label, float *value, float min, float max,
            const SliderOptions &o);

// `items` is an array of `count` C strings; *selected is the index into it.
bool dropdown(std::string_view label, int *selected, const char *const *items, int count);
bool dropdown(std::string_view label, int *selected, const char *const *items, int count,
              const DropdownOptions &o);

// Writes into your buffer, NUL-terminated, never past capacity - 1.
bool text_input(std::string_view label, char *buffer, int capacity);
bool text_input(std::string_view label, char *buffer, int capacity,
                const TextInputOptions &o);

// ---------------------------------------------------------------------------
// Input, and who gets it
//
// The UI reads the pointer and the keyboard itself. These two are how your game
// finds out that it should keep its hands off — without them, the click that
// presses Pause also fires your weapon, and typing a save name walks the player
// across the level.
//
//     if (!rmp::ui::wants_pointer() && IsMouseButtonPressed(0)) shoot();
//     if (!rmp::ui::wants_keyboard() && IsKeyDown(KEY_W))       walk();
// ---------------------------------------------------------------------------

// The pointer is over the interface, or the interface is using it (dragging a
// slider). Like everything in immediate mode this answers for the layout of the
// previous frame, which is one frame of tolerance nobody will notice.
bool wants_pointer();

// A text field has focus, so the keyboard belongs to it.
bool wants_keyboard();

// ---------------------------------------------------------------------------
// Focus, keyboard and gamepad
//
// Every control that can be interacted with is focusable, in the order it was
// declared. Tab and the arrows (or a d-pad) move between them, Enter or the
// gamepad's bottom button activates. It costs you nothing: the widgets you
// already wrote are already navigable.
//
// This is what makes a build playable on a TV with a controller, and it is why
// the focus ring is not optional in the theme.
// ---------------------------------------------------------------------------

// Give a named control the focus, e.g. when a menu opens. Pass "" to clear it.
void focus(std::string_view id);

// What has the focus right now, or "" if nothing does.
std::string_view focused();

// Turn keyboard and gamepad navigation off if your game drives focus itself.
void set_navigation_enabled(bool on);
bool navigation_enabled();

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Called for you by the entry point macro, before on_exit() — while the window
// is still open, because releasing a font after CloseWindow() would be
// touching a GL context that no longer exists.
//
// There is no init(): the UI starts itself on the first begin(), by which time
// there is a window. A game that never draws UI allocates nothing.
void shutdown();

} // namespace rmp::ui
