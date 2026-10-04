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

// RMP_UI_FONT_SIZE, so the theme's default type size is the one you set in
// [ui] rather than a number baked into this header.
#include <rmp/config.h>

#include <string_view>

#ifndef RMP_UI_FONT_SIZE
// [ui] font_size in design units. This 20 is only the fallback for a
// rmp/config.h that does not define it, and matches the .toml's default.
#define RMP_UI_FONT_SIZE 20
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

// Which colour of the theme a piece of text uses: TEXT is the theme's text,
// MUTED its text_muted, PRIMARY its primary and DANGER its danger.
enum class ColorRole { TEXT, MUTED, PRIMARY, DANGER };

// The three type steps. Anywhere a size is asked for you can write one of
// these or a plain number of design units — it is the same field either way:
//
//     rmp::ui::text("Chapter One", { .size = rmp::ui::Size::LARGE });
//     rmp::ui::text("Chapter One", { .size = 34 });
//
// The steps are what you want almost always: they come from the theme, so they
// move together when someone changes the type scale, and they stay in
// proportion at every screen size. A number is the escape hatch. SMALL, MEDIUM
// and LARGE are the theme's font_size_small, font_size and font_size_large.
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

// Every colour and metric the widgets are drawn with. A default-constructed
// Theme is the dark theme: theme_dark() returns exactly that.
struct Theme {
    // The colour behind the interface; no widget paints it. rmp::app clears
    // every frame with it unless the scene sets rmp::Scene::background; in a
    // plain raylib loop, pass it to ClearBackground() yourself.
    Color background = CLITERAL(Color){ 18, 18, 22, 255 };
    // The fill of panel() and of a dropdown's open list.
    Color panel = CLITERAL(Color){ 30, 30, 38, 255 };
    // A NORMAL button at rest, the field of a dropdown and of a text input, a
    // slider's rail, an unticked checkbox's box, and the track of progress().
    Color surface = CLITERAL(Color){ 44, 44, 56, 255 };
    // A NORMAL, OUTLINE or GHOST button under the pointer; also a checkbox's
    // row, a dropdown's field and the items of its open list.
    Color surface_hover = CLITERAL(Color){ 60, 60, 76, 255 };
    // What surface_hover paints, while the pointer is held down on it.
    Color surface_press = CLITERAL(Color){ 26, 26, 34, 255 };
    // The outline of an OUTLINE button, of an unticked checkbox's box and of a
    // dropdown's open list; and, when border_width > 0, of a NORMAL button and
    // of a panel that has no border of its own.
    Color border = CLITERAL(Color){ 70, 70, 88, 255 };

    // text() with ColorRole::TEXT, the label of a NORMAL, OUTLINE or GHOST
    // button, the labels of the controls, and what a dropdown or a text input
    // shows.
    Color text = CLITERAL(Color){ 235, 235, 242, 255 };
    // text() with ColorRole::MUTED, a slider's percentage, and the placeholder
    // of an empty text input.
    Color text_muted = CLITERAL(Color){ 150, 150, 168, 255 };
    // The label of a PRIMARY or DANGER button, and the dot of a ticked checkbox.
    Color text_on_accent = CLITERAL(Color){ 255, 255, 255, 255 };

    // A PRIMARY button at rest, text() with ColorRole::PRIMARY, a ticked
    // checkbox, the filled part of a slider, and the bar of progress().
    Color primary = CLITERAL(Color){ 88, 120, 245, 255 };
    // A PRIMARY button under the pointer.
    Color primary_hover = CLITERAL(Color){ 110, 140, 255, 255 };
    // A PRIMARY button while the pointer is held down on it.
    Color primary_press = CLITERAL(Color){ 70, 98, 210, 255 };

    // A DANGER button at rest, and text() with ColorRole::DANGER.
    Color danger = CLITERAL(Color){ 220, 72, 80, 255 };
    // A DANGER button under the pointer.
    Color danger_hover = CLITERAL(Color){ 236, 96, 104, 255 };
    // A DANGER button while the pointer is held down on it.
    Color danger_press = CLITERAL(Color){ 184, 56, 64, 255 };

    // The fill of a control whose `enabled` is false: a button, a checkbox's
    // box, a slider's filled part, a text input's field.
    Color disabled = CLITERAL(Color){ 60, 60, 70, 255 };
    // The label of a control whose `enabled` is false, and the value a
    // disabled dropdown or text input shows.
    Color disabled_text = CLITERAL(Color){ 110, 110, 124, 255 };

    // The outline on whatever the keyboard or gamepad is pointing at. Without
    // it a controller build is unusable, so it is a theme colour rather than
    // something each widget decides.
    Color focus = CLITERAL(Color){ 130, 170, 255, 255 };

    float font_size = RMP_UI_FONT_SIZE; // [ui] font_size
    float font_size_small = RMP_UI_FONT_SIZE * 0.8f; // Size::SMALL
    float font_size_large = RMP_UI_FONT_SIZE * 1.4f; // Size::LARGE
    float padding_x = 20; // inside a button, left and right of the label
    float padding_y = 12; // inside a button, above and below the label
    float gap = 12; // between siblings
    float panel_padding = 20; // inside a panel(), and around what begin() lays out
    float corner_radius = 8; // of buttons, panels and the controls' fields; 0 = square
    float border_width = 0; // 0 = the default Theme draws no borders
    float control_size = 22; // a checkbox's box; the height of a slider's drag area
    float track_thickness = 6; // a slider's rail
    float focus_ring = 2; // the outline on the focused control
    // How long a control takes to reach its new colour, in seconds. Nothing
    // about the layout moves — only colour — so this can never make a button
    // arrive somewhere else than where you clicked. 0 turns it off, which is
    // both the "reduce motion" setting and what a test wants.
    float transition = 0.12f;
    // The shortest a button can be. It follows the button's type size, so a
    // SMALL one may be shorter, except on Android and iOS, where no button is.
    // 44 design units is Apple's touch target guidance and close to Material's
    // 48dp — it is the difference between a menu you can use with a thumb and
    // one you cannot.
    float min_touch_size = 44;
};

// The theme every widget is drawn with: current_theme() reads it, set_theme()
// copies a new one over it. There is one stored copy, so the reference
// current_theme() returns sees every later set_theme(), and a widget drawn
// after the call already uses the new one. Both work at any time, before the
// window exists too; until the first set_theme() it is the one [ui] theme names.
const Theme &current_theme();
void set_theme(const Theme &t);

// The two that come with the framework. Which one starts is [ui] theme in
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
//     scale = clamp(min(w / RMP_WINDOW_WIDTH, h / RMP_WINDOW_HEIGHT), 0.5, 4)
//
// min() and not max(): what does not fit is worse than what is left over.
// ---------------------------------------------------------------------------

// The factor in use right now.
float scale();

// Pin it. 0 goes back to automatic. This is how you would implement an
// "interface size" accessibility option.
void set_scale(float s);

// ---------------------------------------------------------------------------
// Breakpoints
//
// Scale already handles "make everything bigger". This is for the one thing
// scale cannot do: change the SHAPE of a layout. No amount of grow, fit or
// fixed sizing turns a row into a column, and on a phone held upright a
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

// The shape of the screen, by width / height: COMPACT below 1 (taller than it
// is wide), MEDIUM from 1 up to 1.6, EXPANDED at 1.6 and wider.
enum class Breakpoint { COMPACT, MEDIUM, EXPANDED };

// Which Breakpoint the screen is right now. It reads the window's size when it
// is called, so it needs no begin() first; a window with no height yet is
// MEDIUM.
Breakpoint current_breakpoint();

// Shorthand for the case that comes up: current_breakpoint() == Breakpoint::COMPACT.
bool compact();

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

// How begin() lays out the frame: a root that fills the screen, holding one
// column that everything you declare goes into. The defaults are the centred
// menu that begin() with no arguments opens.
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

// How a button() looks, how big it is, and whether it can be pressed.
struct ButtonOptions {
    Variant style = Variant::NORMAL; // what it means; the theme picks its colours
    // One of the three steps, or a number of design units. The padding and the
    // minimum touch height follow the type size, so a large button is a large
    // button all over rather than a normal one with bigger letters.
    detail::Sizing size{};
    bool enabled = true; // false = the theme's disabled colours; no press, no focus
    // Only needed when two buttons share a label AND the UI is conditional.
    // Identical labels in one frame are already told apart automatically.
    const char *id = nullptr;
};

// How text() paints a string: its colour, its size, and whether it wraps.
struct TextOptions {
    ColorRole color = ColorRole::TEXT; // which theme colour it is painted in
    detail::Sizing size{}; // a step, a number, or nothing for the theme's
    bool wrap = true; // break between words to fit; false = never broken to fit
};

// True on the frame it is pressed: the pointer released over it, or, while it
// has the focus, Enter or the gamepad's accept button. Reads as it looks:
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
    float padding = -1; // inside this container; -1 = none, panel_padding in a panel
    Align items = Align::CENTER; // where children sit in the leftover space
    // Sizing is interleaved by axis — x then y — rather than grouped by kind,
    // and that is not cosmetic. Designated initialisers have to be written in
    // declaration order, so grouping them as grow_x, grow_y, width, height
    // would make { .width = 240, .grow_y = true } illegal: a fixed-width
    // sidebar that fills the height, which is about the most ordinary thing
    // anyone writes. This way round, every combination is legal.
    bool grow_x = false; // fill the parent's width instead of fitting content
    float width = 0; // > 0 = a fixed width, overriding fit/grow
    bool grow_y = false; // fill the parent's height instead of fitting content
    float height = 0; // > 0 = a fixed height, overriding fit/grow
    // A stable name for the container: the same element from frame to frame,
    // whatever else is on screen. Nothing in rmp::ui needs it -- it is for a
    // test that finds an element by name, as tests/ui_layout_test.cpp does.
    // Unnamed containers are anonymous, which is what almost all of them want.
    const char *id = nullptr;
};

// A panel is a box with a background, which is what makes it visible.
struct PanelOptions {
    // The layout, as column() takes it, except that a padding of -1 is the
    // theme's panel_padding rather than none.
    BoxOptions box{};
    Color background = CLITERAL(Color){ 0, 0, 0, 0 }; // {0,0,0,0} = the theme's panel
    float radius = -1; // -1 = the theme's
    // {0,0,0,0} = the theme's border, drawn only when the theme's border_width
    // is above 0 (it is 1 in theme_light(), 0 in theme_dark()).
    Color border = CLITERAL(Color){ 0, 0, 0, 0 };
    float border_width = -1; // of `border`, when set; -1 = 1 design unit
};

// NOTE ON FIELD ORDER, here and in every options struct below. C++20 requires
// designated initialisers in declaration order: { .width = 64, .tint = GRAY } is
// fine, { .tint = GRAY, .width = 64 } does not compile. So these are ordered the
// way they are most likely to be written — sizing, then appearance, then
// identity — rather than alphabetically or by importance.

// How image() sizes and tints a texture. Sizes are in design units.
struct ImageOptions {
    bool grow = false; // fill the space available
    float width = 0; // otherwise: 0 = the texture's own width, scaled
    float height = 0; // 0 = the texture's own height, scaled
    Color tint = WHITE; // multiplies the texture's colours; WHITE leaves them as they are
};

// How progress() sizes and colours its bar. Sizes are in design units.
struct ProgressOptions {
    float width = 0; // 0 = fill the space available
    float height = -1; // -1 = derived from the theme's font size
    float radius = -1; // -1 = half the height, for round ends
    Color fill = CLITERAL(Color){ 0, 0, 0, 0 }; // {0,0,0,0} = the theme's primary
    Color track = CLITERAL(Color){ 0, 0, 0, 0 }; // {0,0,0,0} = the theme's surface
    const char *id = nullptr; // names the bar, as BoxOptions::id names a container
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

// The texture is copied on the way in, so a temporary is fine:
// rmp::ui::image(rmp::assets::load_texture("icon.png")) draws. What the copy
// holds is a GPU handle, so the texture itself still has to be loaded when the
// frame is drawn — but that is your asset's lifetime, not this call's. By
// default it is drawn at its own size, scaled with the rest of the UI.
void image(const Texture2D &texture);
void image(const Texture2D &texture, const ImageOptions &o);

// A bar. `fraction` is 0..1 and is clamped.
void progress(float fraction);
void progress(float fraction, const ProgressOptions &o);

// ---------------------------------------------------------------------------
// Grid and scroll
// ---------------------------------------------------------------------------

// How grid() counts its columns, spaces its cells, and how much room it takes.
// Measurements are in design units, and -1 means the default given beside it.
struct GridOptions {
    int columns = 0; // 0 = as many as fit, at least one, recomputed as the window changes
    float min_cell = 96; // only used when columns = 0
    float gap = -1; // between cells and between rows; -1 = the theme's
    float padding = -1; // inside the grid; -1 = none
    bool grow_x = true; // a grid almost always wants the width it is offered
    bool grow_y = false; // fill the parent's height instead of fitting the rows
    // A name for the grid. columns = 0 reads the width the grid had last frame
    // and finds it by this name; without one, by its order among the unnamed
    // grids of the pass, which a grid that comes and goes before it changes.
    const char *id = nullptr;
};

// How scroll() clips its contents, spaces them, and how much room it takes.
// Measurements are in design units, and -1 means the default given beside it.
struct ScrollOptions {
    bool horizontal = false; // vertical by default, which is what lists want
    bool vertical = true; // clip and scroll top to bottom
    float gap = -1; // between children; -1 = the theme's
    float padding = -1; // inside the area; -1 = none
    bool grow_x = true; // fill the parent's width
    float width = 0; // > 0 = a fixed width, overriding grow_x
    bool grow_y = true; // a scroll area with no height clips nothing
    float height = 0; // > 0 = a fixed height, overriding grow_y
    // A name for the area. Without one it is told apart by its order among the
    // unnamed scroll areas of the pass, so an area that comes and goes before
    // it changes which one it is.
    const char *id = nullptr;
};

namespace detail {
void open_grid(const GridOptions &o);
void close_grid();
void open_cell();
void close_cell();
void open_scroll(const ScrollOptions &o);
void close_scroll();

struct GridCloser {
    ~GridCloser() { close_grid(); }
};
struct CellCloser {
    ~CellCloser() { close_cell(); }
};
// A scroll area closes like any other container AND stops clipping, and the
// second half is why it needs a closer of its own: what is inside one can only
// be clicked where the area itself is.
struct ScrollCloser {
    ~ScrollCloser() { close_scroll(); }
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
    detail::ScrollCloser close;
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

// Whether a checkbox() can be toggled, and the name that tells it apart.
struct CheckboxOptions {
    bool enabled = true; // false = the theme's disabled colours; no toggle, no focus
    const char *id = nullptr; // as ButtonOptions::id: for a shared label
};

// How a slider() is sized, stepped and shown. Sizes are in design units.
struct SliderOptions {
    float width = 0; // 0 = fill the space available
    float step = 0; // 0 = continuous; otherwise snap to multiples
    bool enabled = true; // false = the theme's disabled colours; no drag, no focus
    bool show_value = true; // draw the position, as a percentage, after the bar
    const char *id = nullptr; // as ButtonOptions::id: for a shared label
};

// How a dropdown() is sized, and whether it can be opened.
struct DropdownOptions {
    float width = 0; // of the field, in design units; 0 = fill the space available
    bool enabled = true; // false = the disabled text colour; no opening, no focus
    const char *id = nullptr; // as ButtonOptions::id: for a shared label
};

// How a text_input() is sized, and what it shows while it is empty.
struct TextInputOptions {
    float width = 0; // of the field, in design units; 0 = fill the space available
    bool enabled = true; // false = the theme's disabled colours; no typing, no focus
    // Shown in the theme's text_muted while the field is empty and does not have
    // the focus. It is copied, so a temporary is fine.
    std::string_view placeholder{};
    const char *id = nullptr; // as ButtonOptions::id: for a shared label
};

// A box that flips *value when it is clicked, or activated from the keyboard or
// a gamepad while it has the focus. True on the frame it flipped. The pointer is
// yours and is not kept past the call; a null one draws nothing.
bool checkbox(std::string_view label, bool *value);
bool checkbox(std::string_view label, bool *value, const CheckboxOptions &o);

// A bar that sets *value between min and max: dragged, or moved with left and
// right while it has the focus, one `step` (or 5% of the range) per press, then
// twelve a second while held. *value is clamped to the range, and snapped to
// `step` when there is one, on every call; true on any frame that changed it.
// The pointer is yours and is not kept past the call. Nothing is drawn when it
// is null or max <= min.
bool slider(std::string_view label, float *value, float min, float max);
bool slider(std::string_view label, float *value, float min, float max,
            const SliderOptions &o);

// `items` is an array of `count` C strings; *selected is the index into it.
// A click, or Enter or the A button while it has the focus, opens the list.
// Open and focused, up and down walk its items instead of moving the focus,
// Enter or A picks the one they are on, and Escape or B closes it without
// changing anything. True on the frame *selected changed.
bool dropdown(std::string_view label, int *selected, const char *const *items, int count);
bool dropdown(std::string_view label, int *selected, const char *const *items, int count,
              const DropdownOptions &o);

// Writes into your buffer, NUL-terminated, never past capacity - 1, and only
// while the field has the keyboard. It takes it when it is clicked, when the
// focus is moved onto it (Tab, the arrows, the d-pad, focus()), or when Enter,
// Space or the A button is pressed while it has the focus -- but not from the
// focus a screen gives its first control by itself. It gives it back on Enter
// or the A button (Space is typed), on Escape or the B button, and on a click
// anywhere else; the focus stays on it. Tab, up and down move the focus on from
// a field as from any other control, and take the keyboard with them.
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

// Both answer for the last frame in which the UI drew, and keep answering until
// the next begin() — so it does not matter whether you ask in _update(), which
// runs before anything draws, or on the line after end(). Like everything in
// immediate mode that is one frame of tolerance, which nobody will notice.

// The pointer is over the interface, or the interface is using it (dragging a
// slider). "Over the interface" is over anything it paints as a surface: every
// control, every box with a background colour or a border (a panel, a progress
// bar, an open dropdown list), an image, and a scroll area, empty part
// included -- but not where a scroll area has clipped its contents away. Plain
// text with nothing behind it is NOT: a score drawn over the game does not
// stop the game being clicked through it.
bool wants_pointer();

// A text field has the keyboard, so the keys are its own: see text_input().
// Having the focus is not enough -- a field the player pressed Enter on keeps
// the focus and gives the keyboard back.
bool wants_keyboard();

// ---------------------------------------------------------------------------
// Focus, keyboard and gamepad
//
// Every control that can be interacted with is focusable, in the order it was
// declared. Tab, up and down (or a d-pad) move between them, a text field
// included; Enter or the gamepad's bottom button activates, and Escape or the
// right one backs out of a text field or an open dropdown. It costs you
// nothing: the widgets you already wrote are already navigable.
//
// This is what makes a build playable on a TV with a controller, and it is why
// the focus ring is not optional in the theme.
//
// YOU DO NOT HAVE TO START IT. A screen that declares anything focusable and
// has nothing focused focuses its first control, so a game-over scene with one
// "Play again" button on it answers Enter and the A button the moment it
// appears. With two scenes drawing, the one on top gets it — unless the one
// underneath is still reachable (Scene::input_below), which is the case where
// it can hold the focus and the arrows can walk between the two.
//
// The RING, though, only appears once the player touches the keyboard or the
// gamepad, and goes away again on a click. Someone playing with a mouse should
// not find an outline on the first button of every menu that opens; someone
// playing with a controller needs to see where they are before they press
// anything. It is the same rule browsers use, for the same reason.
// ---------------------------------------------------------------------------

// Give a named control the focus — when a menu opens, or to point a controller
// at something other than the first control. Pass "" to clear it. A focus asked
// for by name is drawn straight away: you meant it, and a text field given it
// takes the keyboard. `id` is the control's label, or its explicit .id.
//
// Outside begin()/end() -- in a scene's _ready(), say -- it waits for the next
// frame that draws controls and lands on the first one carrying that name,
// whichever scene draws it; if that frame has none, the request is dropped and
// the screen keeps its own default.
void focus(std::string_view id);

// What has the focus right now, or "" if nothing does.
std::string_view focused();

// Turn keyboard and gamepad navigation off if your game drives focus itself, and
// ask whether it is on. It starts on.
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

namespace detail {
// ---------------------------------------------------------------------------
// The frame boundary. Not for you: rmp::app calls these once per frame, around
// however many begin()/end() pairs the scenes on the stack describe.
//
// WHY IT HAS TO BE OUTSIDE begin(). Every one of these is once-per-FRAME work
// that used to live in begin() because, with one scene, a frame and a pass were
// the same thing. With a pause menu over a game they stop being:
//
//   the pointer      sampled once, so the two scenes cannot disagree about
//                    where the mouse is halfway through a frame
//   scroll           Clay_UpdateScrollContainers advances the offset, so N
//                    passes would scroll N notches per wheel click
//   the anim clock   same: transitions would run N times as fast
//   the text arena   Clay keeps the pointer and reads it at Clay_EndLayout, so
//                    the strings of every pass have to outlive all of them
//   focus            navigation has to see the focusable widgets of ALL layers
//                    before it can decide where the arrow key goes
//
// WITHOUT AN APP none of this changes for you: the first begin() of a frame
// marks the boundary itself and end() closes it, which is exactly what happened
// before any of this existed. rmp::ui on its own, in a plain raylib loop, is
// still begin() and end() and nothing else.
void begin_frame();
void end_frame();

// Whether the pass that is about to be described can be interacted with. False
// lays the UI out and draws it exactly as usual, but no widget in it can be
// hovered, focused or clicked — which is what a HUD under an open pause menu
// should do. rmp::app sets it from Scene::input_below; it resets to true at
// every frame boundary, so a stray call cannot leak into the next frame.
void set_pass_input(bool reachable);
} // namespace detail

} // namespace rmp::ui
