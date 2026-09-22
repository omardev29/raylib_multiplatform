// ===========================================================================
// tests/ui_layout_test.cpp — the UI layout, checked without a window.
//
//   cmake --preset debug -DBUILD_UI_TESTS=ON
//   cmake --build build --target ui_layout_test
//   ./build/ui_layout_test
//
// Clay's layout is pure computation, so with the text measurement and the
// pointer swapped for stubs and a viewport injected, the whole thing runs with
// no GPU, no window and no display. Which means these run anywhere: a CI
// container, a headless runner, your machine over ssh.
//
// What is worth testing here is exactly what a user would check by hand and
// then never check again: is the menu centred, does it stay centred when the
// window changes, do the buttons avoid each other, and does the scale behave
// at the extremes.
// ===========================================================================

#include "../src/rmp/internal.h"
#include "../src/rmp/ui/internal.h"

#include <rmp/ui.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failures++;
}

void check_near(float got, float want, float tolerance, const char *what) {
    bool ok = std::fabs(got - want) <= tolerance;
    std::printf("%s  %s (got %.2f, want %.2f +/- %.2f)\n", ok ? "ok  " : "FAIL", what,
                got, want, tolerance);
    if (!ok) g_failures++;
}

// A stub that needs no font: every glyph is half the font Size wide, and a line
// is one font Size tall. Nothing here depends on real glyph metrics — the
// questions being asked are about arrangement, not typography.
Clay_Dimensions measure_stub(Clay_StringSlice text, Clay_TextElementConfig *config,
                             void * /*unused*/) {
    return Clay_Dimensions{ static_cast<float>(text.length) * config->fontSize * 0.5f,
                            static_cast<float>(config->fontSize) };
}

// No pointer at all, so nothing is ever hovered and no click is ever reported.
void pointer_stub(Clay_Vector2 *position, bool *down) {
    *position = Clay_Vector2{ -1.0f, -1.0f };
    *down = false;
}

// One frame of the menu from src/main.cpp.
void draw_menu() {
    rmp::ui::begin();
    rmp::ui::button("Play");
    rmp::ui::button("Options");
    rmp::ui::button("Quit");
    rmp::ui::end();
}

struct Box {
    float x, y, w, h;
};

Box box_of(const char *label) {
    Clay_BoundingBox b{};
    if (!rmp::ui::detail::bounds_of(label, 0, 0, &b)) {
        std::printf("FAIL  '%s' produced no element at all\n", label);
        g_failures++;
        return Box{ 0, 0, 0, 0 };
    }
    return Box{ b.x, b.y, b.width, b.height };
}

// The menu is centred when the space left over on each side matches.
void expect_centred(float view_w, float view_h, const char *what) {
    Box play = box_of("Play");
    Box quit = box_of("Quit");

    float left_gap = play.x;
    float right_gap = view_w - (play.x + play.w);
    float top_gap = play.y;
    float bottom_gap = view_h - (quit.y + quit.h);

    char msg[160];
    std::snprintf(msg, sizeof(msg), "%s: horizontally centred", what);
    check_near(left_gap, right_gap, 1.5f, msg);
    std::snprintf(msg, sizeof(msg), "%s: vertically centred", what);
    check_near(top_gap, bottom_gap, 1.5f, msg);
}

void run_at(float w, float h, const char *what) {
    std::printf("\n--- %s (%.0fx%.0f) ---\n", what, w, h);
    rmp::ui::detail::set_test_viewport(w, h);

    // Twice: the first frame has no previous geometry to hit-test against, so
    // running it once is not representative of a real second frame.
    draw_menu();
    draw_menu();

    expect_centred(w, h, what);

    Box play = box_of("Play");
    Box options = box_of("Options");
    Box quit = box_of("Quit");

    char msg[160];
    std::snprintf(msg, sizeof(msg), "%s: Play is above Options with a gap", what);
    check(play.y + play.h <= options.y, msg);
    std::snprintf(msg, sizeof(msg), "%s: Options is above Quit with a gap", what);
    check(options.y + options.h <= quit.y, msg);

    // The content column is FIT and the buttons GROW into it, which is what
    // stops a menu coming out as a ragged staircase.
    std::snprintf(msg, sizeof(msg), "%s: all three buttons share a width", what);
    check(std::fabs(play.w - options.w) < 0.5f && std::fabs(options.w - quit.w) < 0.5f,
          msg);

    // min_touch_size, scaled. Below this a button is not reliably hittable with
    // a thumb, which is four of the fourteen targets.
    float minimum = rmp::ui::current_theme().min_touch_size * rmp::ui::scale();
    std::snprintf(msg, sizeof(msg), "%s: buttons are at least min_touch_size tall", what);
    check(play.h >= minimum - 0.5f, msg);

    std::snprintf(msg, sizeof(msg), "%s: the menu fits on screen", what);
    check(play.x >= 0 && play.y >= 0 && play.x + play.w <= w && quit.y + quit.h <= h,
          msg);
}

// ---------------------------------------------------------------------------
// Containers
// ---------------------------------------------------------------------------

// One frame of a dialog: a panel holding a row of two buttons pushed apart by
// a spacer, with a progress bar above them.
void draw_dialog() {
    rmp::ui::begin();
    rmp::ui::panel({ .box = { .id = "dialog" } }, [] {
        rmp::ui::text("Really quit?");
        rmp::ui::progress(0.5f, { .width = 200, .id = "bar" });
        rmp::ui::row({ .gap = 8, .grow_x = true, .id = "buttons" }, [] {
            rmp::ui::button("Yes");
            rmp::ui::spacer();
            rmp::ui::button("No");
        });
    });
    rmp::ui::end();
}

bool inside(Box outer, Box inner) {
    return inner.x >= outer.x - 0.5f && inner.y >= outer.y - 0.5f &&
        inner.x + inner.w <= outer.x + outer.w + 0.5f &&
        inner.y + inner.h <= outer.y + outer.h + 0.5f;
}

void run_containers(float w, float h) {
    std::printf("\n--- containers (%.0fx%.0f) ---\n", w, h);
    rmp::ui::detail::set_test_viewport(w, h);
    draw_dialog();
    draw_dialog();

    Box yes = box_of("Yes");
    Box no = box_of("No");

    // A row lays out left to right. This is the check that catches a container
    // silently falling back to the default vertical direction, which looks
    // almost right until two buttons are on top of each other.
    check(yes.x + yes.w <= no.x, "row: Yes is left of No, not above it");
    check_near(yes.y, no.y, 1.0f, "row: both buttons share a baseline");

    // grow_x on the row plus a spacer between them: they end up at opposite
    // ends. Without the spacer they would sit next to each other in the middle.
    float left_gap = yes.x;
    float right_gap = w - (no.x + no.w);
    check(std::fabs(left_gap - right_gap) < 40.0f,
          "row: the spacer pushed them to opposite ends");

    // The panel has to contain its children, padding and all. A panel that
    // sizes itself wrongly shows up here as a child hanging out of it.
    Box panel = box_of("dialog");
    check(inside(panel, yes) && inside(panel, no), "panel: it contains its children");
    check(panel.w > 0 && panel.h > 0, "panel: it has a Size at all");

    // Padding is real: the row inside is strictly narrower than the panel.
    Box row = box_of("buttons");
    check(row.w < panel.w, "panel: padding leaves the row narrower than the panel");

    // Design units, not pixels: a 200-unit bar is 200 * scale on screen. This
    // is the assertion that catches someone "fixing" a size by writing a pixel
    // count, which looks right on one monitor and wrong on every other.
    float scale = rmp::ui::scale();
    Box bar = box_of("bar");
    check_near(bar.w, 200.0f * scale, 1.0f,
               "progress: the track is 200 design units wide");
    check(inside(panel, bar), "progress: the bar is inside the panel");
}

// ---------------------------------------------------------------------------
// Interaction
//
// A screenshot proves the pixels are in the right place and nothing else. This
// is the part that proves a click does something — driven entirely through the
// injected pointer, so it still needs no window.
// ---------------------------------------------------------------------------

Clay_Vector2 g_pointer{ -1, -1 };
bool g_down = false;

void pointer_scripted(Clay_Vector2 *position, bool *down) {
    *position = g_pointer;
    *down = g_down;
}

// The keyboard and the gamepad, through the same kind of seam: what the player
// is pushing, and whether they just pressed the button that means "do it".
// Without this a headless test can lay a menu out and click it with a fake
// mouse, but cannot press its buttons the way a controller does.
rmp::ui::detail::NavState g_nav{};

void nav_scripted(rmp::ui::detail::NavState *out) { *out = g_nav; }

void run_interaction() {
    std::printf("\n--- interaction ---\n");
    rmp::ui::detail::set_pointer_provider(pointer_scripted);
    rmp::ui::detail::set_test_viewport(1280, 720);

    bool checked = false;
    float volume = 0.5f;
    int clicks = 0;

    auto frame = [&] {
        rmp::ui::begin();
        rmp::ui::panel([&] {
            if (rmp::ui::button("Apply")) clicks++;
            rmp::ui::checkbox("Fullscreen", &checked);
            rmp::ui::slider("Volume", &volume, 0.0f, 1.0f);
        });
        rmp::ui::end();
    };

    // Frame one only establishes geometry: hit testing answers for the layout
    // of the frame before, so nothing can be clicked until something has been
    // laid out at least once. That is the rule, and this is it being true.
    g_pointer = Clay_Vector2{ -1, -1 };
    g_down = false;
    frame();

    Box btn = box_of("Apply");
    check(btn.w > 0, "the button has a Box to aim at");

    // Press and release over the button.
    g_pointer = Clay_Vector2{ btn.x + btn.w / 2, btn.y + btn.h / 2 };
    g_down = true;
    frame();
    check(clicks == 0, "a press alone does not click");
    g_down = false;
    frame();
    check(clicks == 1, "press then release over the button clicks it");

    // Press on it, drag off, release: nothing. This is the behaviour people
    // rely on without ever noticing it, and the one that quietly disappears if
    // a click is implemented as "button is down over the element".
    g_down = true;
    frame();
    g_pointer = Clay_Vector2{ 5, 5 };
    g_down = false;
    frame();
    check(clicks == 1, "dragging off before releasing does not click");

    // The checkbox writes to the caller's variable.
    Box cb = box_of("Fullscreen");
    g_pointer = Clay_Vector2{ cb.x + cb.w / 2, cb.y + cb.h / 2 };
    g_down = true;
    frame();
    g_down = false;
    frame();
    check(checked, "the checkbox toggled the caller's bool");
    g_down = true;
    frame();
    g_down = false;
    frame();
    check(!checked, "and toggled it back");

    // Dragging the slider writes a value proportional to where the pointer is.
    //
    // Aim at the RAIL, not at the row: the row is [label][rail][45%], so its
    // right-hand end is the percentage text and pressing there does nothing —
    // which is correct, and which this test got wrong first time round.
    //
    // The id is built the way the widget builds it — first occurrence of the
    // label — rather than by calling element_id() again, which would hand back
    // occurrence 1 because the counters only reset at begin().
    Clay_String vol_label{ false, 6, "Volume" };
    Clay_ElementId vol_id = Clay_GetElementIdWithIndex(vol_label, 0);
    Clay_BoundingBox rail{};
    bool have_rail =
        rmp::ui::detail::bounds_of_id(rmp::ui::detail::sub_id(vol_id, 0), &rail);
    check(have_rail && rail.width > 0, "the slider's rail has a Box to aim at");

    g_pointer = Clay_Vector2{ rail.x + rail.width * 0.95f, rail.y + rail.height / 2 };
    g_down = true;
    frame();
    frame();
    check(volume > 0.7f, "dragging the slider to the right raises the value");
    g_pointer = Clay_Vector2{ rail.x, rail.y + rail.height / 2 };
    frame();
    check(volume < 0.3f, "and dragging it back lowers it");
    g_down = false;
    frame();

    // wants_pointer() is what stops the click that pressed a button from also
    // firing the player's weapon.
    g_pointer = Clay_Vector2{ btn.x + btn.w / 2, btn.y + btn.h / 2 };
    frame();
    check(rmp::ui::wants_pointer(), "wants_pointer() is true over a control");
    g_pointer = Clay_Vector2{ 4, 4 };
    frame();
    check(!rmp::ui::wants_pointer(), "and false out in the open");

    rmp::ui::detail::set_pointer_provider(pointer_stub);
}

// ---------------------------------------------------------------------------
// Dropdown
//
// Two lists that happen to share an item name is not an edge case — "Low",
// "None" and "Off" turn up in half the settings screens ever written. If the
// ids came from the item text they would be the same element.
// ---------------------------------------------------------------------------

void run_dropdown() {
    std::printf("\n--- dropdown ---\n");
    rmp::ui::detail::set_pointer_provider(pointer_scripted);
    rmp::ui::detail::set_test_viewport(1280, 720);

    static const char *a[] = { "Off", "Low", "High" };
    static const char *b[] = { "Off", "Low", "High" };
    int quality = 0;
    int shadows = 0;

    auto frame = [&] {
        rmp::ui::begin();
        rmp::ui::panel([&] {
            rmp::ui::dropdown("Quality", &quality, a, 3);
            rmp::ui::dropdown("Shadows", &shadows, b, 3);
        });
        rmp::ui::end();
    };

    g_pointer = Clay_Vector2{ -1, -1 };
    g_down = false;
    frame();

    // Open the first one.
    Box q = box_of("Quality");
    g_pointer = Clay_Vector2{ q.x + q.w * 0.8f, q.y + q.h / 2 };
    g_down = true;
    frame();
    g_down = false;
    frame();
    frame();

    // Its items exist and the other dropdown's do not overlap them, which is
    // what the derived ids buy.
    Clay_String ql{ false, 7, "Quality" };
    Clay_String sl{ false, 7, "Shadows" };
    Clay_ElementId qid = Clay_GetElementIdWithIndex(ql, 0);
    Clay_ElementId sid = Clay_GetElementIdWithIndex(sl, 0);
    check(qid.id != sid.id, "the two dropdowns are different elements");
    check(rmp::ui::detail::sub_id(qid, 1).id != rmp::ui::detail::sub_id(sid, 1).id,
          "and so are their identically named items");

    Clay_BoundingBox item{};
    bool have_item =
        rmp::ui::detail::bounds_of_id(rmp::ui::detail::sub_id(qid, 2), &item);
    check(have_item, "the open list laid its items out");

    if (have_item) {
        // Pick "Low" from the first list; the second must not move.
        g_pointer = Clay_Vector2{ item.x + item.width / 2, item.y + item.height / 2 };
        g_down = true;
        frame();
        g_down = false;
        frame();
        check(quality == 1, "clicking an item selects it");
        check(shadows == 0, "and leaves the other dropdown alone");
    }

    rmp::ui::detail::set_pointer_provider(pointer_stub);
}

// ---------------------------------------------------------------------------
// Grid
// ---------------------------------------------------------------------------

void run_grid() {
    std::printf("\n--- grid ---\n");
    rmp::ui::detail::set_test_viewport(1280, 720);

    auto frame = [&] {
        rmp::ui::begin();
        rmp::ui::grid(3, [&] {
            for (int i = 0; i < 7; i++) {
                rmp::ui::cell([&] { rmp::ui::button(TextFormat("item%d", i)); });
            }
        });
        rmp::ui::end();
    };
    frame();
    frame();

    Box a = box_of("item0");
    Box b = box_of("item1");
    Box c = box_of("item2");
    Box d = box_of("item3");

    check_near(a.y, b.y, 1.0f, "three columns: the first three share a row");
    check_near(b.y, c.y, 1.0f, "…all three of them");
    check(a.x < b.x && b.x < c.x, "and they run left to right");
    // Seven items in three columns is three rows, the last one short. The one
    // that used to corrupt everything after it.
    check(d.y > a.y, "the fourth item starts a new row");
    check_near(d.x, a.x, 1.0f, "and lines up under the first");
    check_near(a.w, b.w, 2.0f, "cells are equal width");
}

// ---------------------------------------------------------------------------
// Phase 4: style.
//
// Three questions, and they are the ones a person would check by eye once and
// then never check again: does a size step actually change the size, does the
// light Theme actually differ from the dark one, and does a breakpoint change
// where the aspect ratio says it should.
// ---------------------------------------------------------------------------

void run_sizes() {
    std::printf("\n--- sizes and variants ---\n");
    rmp::ui::detail::set_test_viewport(1280, 720);

    rmp::ui::begin();
    rmp::ui::button("S", { .size = rmp::ui::Size::SMALL });
    rmp::ui::button("M", { .size = rmp::ui::Size::MEDIUM });
    rmp::ui::button("L", { .size = rmp::ui::Size::LARGE });
    // A Variant is colour only: it must not move anything. Same label length as
    // "M" on purpose, so any difference in width is the variant's doing.
    rmp::ui::button("G", { .style = rmp::ui::Variant::GHOST });
    rmp::ui::end();

    Box small = box_of("S");
    Box medium = box_of("M");
    Box large = box_of("L");
    Box ghost = box_of("G");

    check(small.h < medium.h, "Size::SMALL is shorter than Size::MEDIUM");
    check(medium.h < large.h, "Size::LARGE is taller than Size::MEDIUM");
    check(large.h / medium.h > 1.2f, "and large is a step, not a rounding difference");
    check_near(ghost.h, medium.h, 0.5f, "a Variant changes colour and nothing else");

    // Width has to be measured one button per frame. In a menu they all come
    // out the same width on purpose — the column fits the widest and the rest
    // grow to match — so three in one frame could never show a difference.
    rmp::ui::begin();
    rmp::ui::button("X", { .size = rmp::ui::Size::SMALL });
    rmp::ui::end();
    const float narrow = box_of("X").w;
    rmp::ui::begin();
    rmp::ui::button("X", { .size = rmp::ui::Size::LARGE });
    rmp::ui::end();
    check(box_of("X").w > narrow,
          "the padding follows the type, so a large button is wider too");

    // An explicit number has to work in the same field as a step: that is the
    // whole reason the field is not a plain float. Text elements have no id of
    // their own, so each one is measured through the box around it.
    rmp::ui::begin();
    rmp::ui::panel({ .box = { .id = "exact" } },
                   [] { rmp::ui::text("Ay", { .size = 40 }); });
    rmp::ui::panel({ .box = { .id = "byTheme" } }, [] { rmp::ui::text("Ay"); });
    rmp::ui::end();
    check(box_of("exact").h > box_of("byTheme").h,
          "a raw number works in the same field as a step");
}

void run_themes() {
    std::printf("\n--- themes ---\n");

    const rmp::ui::Theme dark = rmp::ui::theme_dark();
    const rmp::ui::Theme light = rmp::ui::theme_light();

    // Not "they differ" — that a light Theme is actually light, which is the
    // thing that would be wrong if someone copied the dark values by mistake.
    check(light.background.r > dark.background.r + 100,
          "the light theme's background is actually light");
    check(light.text.r < dark.text.r - 100, "and its text is actually dark");
    check(
        light.border_width > 0 && dark.border_width == 0,
        "only the light Theme draws borders: it has no shadows to separate its surfaces");

    rmp::ui::set_theme(light);
    check(rmp::ui::current_theme().background.r == light.background.r,
          "set_theme() takes");

    // Copy-modify-set, which is the only way a theme is ever customised.
    rmp::ui::Theme t = rmp::ui::current_theme();
    t.corner_radius = 0;
    rmp::ui::set_theme(t);
    check(rmp::ui::current_theme().corner_radius == 0 &&
              rmp::ui::current_theme().background.r == light.background.r,
          "copy-modify-set changes one field and keeps the rest");

    rmp::ui::set_theme(dark);
    check(rmp::ui::current_theme().background.r == dark.background.r, "and back to dark");
}

void run_breakpoints() {
    std::printf("\n--- breakpoints ---\n");

    struct CaseRow {
        float w, h;
        rmp::ui::Breakpoint want;
        const char *what;
    };
    const CaseRow cases[] = {
        { 1080, 2400, rmp::ui::Breakpoint::COMPACT, "a phone held upright is compact" },
        { 600, 800, rmp::ui::Breakpoint::COMPACT, "so is a narrow window" },
        { 1024, 768, rmp::ui::Breakpoint::MEDIUM, "4:3 is medium" },
        { 2048, 1536, rmp::ui::Breakpoint::MEDIUM, "a tablet on its side is medium" },
        { 2400, 1080, rmp::ui::Breakpoint::EXPANDED,
          "a phone on its side has room for a row" },
        { 1920, 1080, rmp::ui::Breakpoint::EXPANDED, "16:9 is expanded" },
        { 3440, 1440, rmp::ui::Breakpoint::EXPANDED, "and so is an ultrawide" },
    };

    for (const CaseRow &c : cases) {
        rmp::ui::detail::set_test_viewport(c.w, c.h);
        check(rmp::ui::current_breakpoint() == c.want, c.what);
    }

    rmp::ui::detail::set_test_viewport(1080, 2400);
    check(rmp::ui::compact(), "compact() agrees with current_breakpoint()");
    rmp::ui::detail::set_test_viewport(1920, 1080);
    check(!rmp::ui::compact(), "and disagrees when it should");

    // The point of the whole feature: 4K and a phone are both "big numbers",
    // and only one of them should be told to stack its layout.
    rmp::ui::detail::set_test_viewport(3840, 2160);
    check(!rmp::ui::compact(),
          "4K is not compact, even though a phone has more pixels tall");
}

// ---------------------------------------------------------------------------
// Identity, capture and the frame boundary
//
// Everything below was a bug first. Each one is here because the symptom in a
// game was something else entirely — a controller that could not move a
// slider, a mouse that stayed dead after a menu closed, a button that could
// not be clicked while a list scrolled — and none of them shows in a
// screenshot.
// ---------------------------------------------------------------------------

// focus(name) has to compute the id the WIDGET will have. It used to call the
// allocating id function, which counted the label a second time and handed back
// an occurrence no widget would ever own.
void run_focus_by_name() {
    std::printf("\n--- focus by name ---\n");
    rmp::ui::detail::set_test_viewport(1280, 720);

    // A frame first, so "Play"/"Options"/"Quit" are labels the occurrence table
    // has already seen. That is the arrangement that broke, and it is also the
    // normal one: focus() is called between frames, after a menu has drawn.
    draw_menu();
    rmp::ui::focus("Options");
    draw_menu();
    check(rmp::ui::focused() == "Options",
          "focus(name) reaches the widget that carries the name");

    // And it must renumber nothing: two controls sharing a label are still two
    // elements when a focus() call sits between them.
    rmp::ui::begin();
    rmp::ui::button("Dup");
    rmp::ui::focus("Dup");
    rmp::ui::button("Dup");
    rmp::ui::end();
    Clay_BoundingBox first{};
    Clay_BoundingBox second{};
    const bool both = rmp::ui::detail::bounds_of("Dup", 0, 0, &first) &&
        rmp::ui::detail::bounds_of("Dup", 1, 0, &second);
    check(both && first.y != second.y,
          "and a focus() between two buttons with the same label leaves both where "
          "they were");
}

// A stepped slider under a d-pad. The nudge used to be step * dt * 12, which at
// 60 fps is a fifth of a step — less than the half a step the snap needs, and
// the remainder was thrown away. The value never moved, at any frame rate above
// about 42 fps.
void run_slider_nav() {
    std::printf("\n--- slider: keyboard and gamepad ---\n");
    rmp::ui::detail::set_test_viewport(1280, 720);

    float quality = 2.0f;
    auto frame = [&] {
        rmp::ui::begin();
        rmp::ui::slider("Quality", &quality, 0.0f, 4.0f, { .step = 1.0f });
        rmp::ui::end();
    };

    // An empty pass, so the occurrence counters carry nothing in from whatever
    // ran before this test.
    rmp::ui::begin();
    rmp::ui::end();
    rmp::ui::focus("Quality");

    g_nav.x = 0;
    frame();
    check_near(quality, 2.0f, 0.001f, "a stick at rest moves nothing");

    g_nav.x = 1;
    frame();
    check_near(quality, 3.0f, 0.001f, "pushing right moves it exactly one step");

    g_nav.x = -1;
    frame();
    check_near(quality, 2.0f, 0.001f, "and pushing left moves it back");

    // Held rather than pressed again: a headless frame takes no time at all, so
    // the repeat clock never comes round and the value must not move.
    frame();
    frame();
    check_near(quality, 2.0f, 0.001f, "holding it does not run away with the value");

    // Pressed again, three times, which is two steps of travel and one of
    // nothing because it is already at the end.
    for (int i = 0; i < 3; i++) {
        g_nav.x = 0;
        frame();
        g_nav.x = -1;
        frame();
    }
    check_near(quality, 0.0f, 0.001f, "and it stops at min instead of running past it");

    g_nav = rmp::ui::detail::NavState{};
}

// Pointer capture belongs to the element that took it. It used to be one global
// bool that every slider in the frame wrote to, so the second slider gave the
// pointer back while the first was still being dragged — and nothing ever
// cleared it if the dragging slider went away.
void run_two_sliders() {
    std::printf("\n--- two sliders, one pointer ---\n");
    rmp::ui::detail::set_pointer_provider(pointer_scripted);
    rmp::ui::detail::set_test_viewport(1280, 720);

    float music = 0.5f;
    float sfx = 0.5f;
    bool sliders_on_screen = true;
    auto frame = [&] {
        rmp::ui::begin();
        rmp::ui::panel([&] {
            if (sliders_on_screen) {
                rmp::ui::slider("Music", &music, 0.0f, 1.0f, { .width = 200 });
                rmp::ui::slider("SFX", &sfx, 0.0f, 1.0f, { .width = 200 });
            } else {
                rmp::ui::text("the menu closed");
            }
        });
        rmp::ui::end();
    };

    g_pointer = Clay_Vector2{ -1, -1 };
    g_down = false;
    frame();
    frame();

    Clay_BoundingBox rail{};
    const bool have_rail = rmp::ui::detail::bounds_of_id(
        rmp::ui::detail::sub_id(rmp::ui::detail::peek_element_id("Music", 0, 0), 0),
        &rail);
    check(have_rail && rail.width > 0, "the Music rail has a Box to aim at");

    // Press on Music and drag away from both of them. SFX is declared after
    // Music and is not being dragged: it must not answer for the pointer.
    g_pointer = Clay_Vector2{ rail.x + rail.width * 0.5f, rail.y + rail.height / 2 };
    g_down = true;
    frame();
    g_pointer = Clay_Vector2{ 4, 4 };
    frame();
    check(rmp::ui::wants_pointer(),
          "a slider being dragged keeps the pointer, whatever is drawn after it");
    check(sfx == 0.5f, "and the other slider is not dragged along with it");

    // Let go with the menu gone, which is Escape closing a settings scene
    // mid-drag. The capture must not outlive the drag.
    //
    // Two frames, and the second one is the point: the capture is held through
    // the frame the release happens in, because that frame is what a click is
    // made of, and it is let go at the boundary after it. One frame either way
    // is the same tolerance every other interaction here has.
    g_down = false;
    sliders_on_screen = false;
    frame();
    frame();
    check(!rmp::ui::wants_pointer(),
          "releasing gives the pointer back even if the slider is never drawn again");

    rmp::ui::detail::set_pointer_provider(pointer_stub);
}

// wants_keyboard() is read AFTER end(), or in an _update() that runs before any
// _draw. It used to be cleared as the last act of the frame, so both readers got
// false and the player walked across the level while typing a save name.
void run_keyboard_capture() {
    std::printf("\n--- wants_keyboard() ---\n");
    rmp::ui::detail::set_test_viewport(1280, 720);

    char name[32] = "Omar";
    rmp::ui::begin(); // an empty pass, so the counters carry nothing in
    rmp::ui::end();
    rmp::ui::focus("Name");

    rmp::ui::begin();
    rmp::ui::text_input("Name", name, sizeof name);
    rmp::ui::end();
    check(rmp::ui::wants_keyboard(),
          "wants_keyboard() still answers after end(), which is where a game asks");

    rmp::ui::begin();
    rmp::ui::text("no field here");
    rmp::ui::end();
    check(!rmp::ui::wants_keyboard(),
          "and the next frame's begin() clears it, not the frame before's end()");
}

// The frame arena is for DISPLAY. Identity must not depend on it: a label
// interned short hashes differently, so an element declared after an overflow
// used to become a different element every frame — hover, focus and its
// remembered box detaching all at once.
void run_arena_overflow() {
    std::printf("\n--- the text arena, full ---\n");
    rmp::ui::detail::set_test_viewport(1280, 720);

    rmp::ui::begin();
    char filler[1024];
    std::memset(filler, 'x', sizeof filler);
    for (int i = 0; i < 9; i++) {
        rmp::ui::detail::intern(std::string_view{ filler, sizeof filler });
    }
    rmp::ui::button("PlayTheGame");
    rmp::ui::end();

    Clay_BoundingBox b{};
    check(rmp::ui::detail::bounds_of("PlayTheGame", 0, 0, &b) && b.width > 0,
          "a button declared after the arena filled still has the id its label says");
}

// More distinct labels in one pass than the occurrence table holds. Every
// unrecorded label used to come back as occurrence 0, so two "Use" buttons late
// in a list became one element: hovering one lit both, clicking either fired the
// wrong one, and nothing was logged.
void run_label_overflow() {
    std::printf("\n--- more labels than the table holds ---\n");
    rmp::ui::detail::set_test_viewport(1280, 720);
    rmp::detail::reset_reports_for_tests();

    rmp::ui::begin();
    for (int i = 0; i < 300; i++) {
        char label[16];
        std::snprintf(label, sizeof label, "lbl%03d", i);
        rmp::ui::detail::element_id(std::string_view{ label }, nullptr);
    }
    const Clay_ElementId a = rmp::ui::detail::element_id("Use", nullptr);
    const Clay_ElementId b = rmp::ui::detail::element_id("Use", nullptr);
    rmp::ui::end();

    check(a.id != b.id, "past the table, two controls sharing a label are still two");
    check(rmp::detail::report_count() >= 1, "and the framework says so, once");
}

// A grid nested past the limit used to open a Clay element without a frame to
// close it with, so its close consumed the grid ABOVE it and everything after
// was off by one.
void run_nested_grids() {
    std::printf("\n--- grids nested past the limit ---\n");
    rmp::ui::detail::set_test_viewport(1280, 720);
    rmp::ui::detail::reset_clay_errors_for_tests();

    // Four grids is the limit, so the fifth is the one with no frame of its own.
    // It sits in the second of the fourth grid's four cells, and the assertion
    // is about the two cells AFTER it: they used to be shuffled onto the wrong
    // row, because the fifth grid's cells were counted against the fourth
    // grid's column index.
    auto frame = [] {
        rmp::ui::begin();
        rmp::ui::grid(2, [] {
            rmp::ui::cell([] {
                rmp::ui::grid(2, [] {
                    rmp::ui::cell([] {
                        rmp::ui::grid(2, [] {
                            rmp::ui::cell([] {
                                rmp::ui::grid(2, [] {
                                    rmp::ui::cell([] { rmp::ui::button("a"); });
                                    rmp::ui::cell([] {
                                        rmp::ui::grid(2, [] { // the fifth
                                            rmp::ui::cell(
                                                [] { rmp::ui::button("deep"); });
                                        });
                                    });
                                    rmp::ui::cell([] { rmp::ui::button("c"); });
                                    rmp::ui::cell([] { rmp::ui::button("d"); });
                                });
                            });
                        });
                    });
                });
            });
        });
        rmp::ui::button("After");
        rmp::ui::end();
    };
    frame();
    frame();

    Box a = box_of("a");
    Box c = box_of("c");
    Box d = box_of("d");
    check_near(c.y, d.y, 1.0f, "the two cells after a too-deep grid still share a row");
    check(c.y > a.y && std::fabs(c.x - a.x) < 1.0f,
          "and that row is the next one, under the first cell");

    Clay_BoundingBox after{};
    check(rmp::ui::detail::bounds_of("After", 0, 0, &after) && after.width > 0,
          "a fifth nested grid does not take the rest of the frame with it");
    check(rmp::ui::detail::clay_error_count() == 0,
          "and the element tree stays balanced");
}

// Past [ui] max_elements Clay stops laying anything out. Its own message names
// Clay_SetMaxElementCount(), a function the user cannot call and a name they
// were promised never to see.
void run_element_ceiling() {
    std::printf("\n--- the element ceiling ---\n");
    rmp::ui::detail::set_test_viewport(1280, 720);
    rmp::detail::reset_reports_for_tests();
    rmp::ui::detail::reset_clay_errors_for_tests();

    rmp::ui::begin();
    for (int i = 0; i < 600; i++) rmp::ui::text("x");
    rmp::ui::end();

    const Clay_ErrorType first = rmp::ui::detail::first_clay_error();
    check(rmp::ui::detail::clay_error_count() > 0 &&
              (first == CLAY_ERROR_TYPE_ELEMENTS_CAPACITY_EXCEEDED ||
               first == CLAY_ERROR_TYPE_HASH_MAP_CAPACITY_EXCEEDED),
          "more elements than the ceiling is something Clay reports");
    check(rmp::detail::report_count() == 1,
          "and it reaches the user once, in our words, naming [ui] max_elements");
}

// Two scenes drawing in one frame. The pass index is a block of element
// indices, which is what stops a lower scene showing a button conditionally
// from renumbering the scene above it.
void run_two_passes() {
    std::printf("\n--- two passes in one frame ---\n");
    rmp::ui::detail::set_pointer_provider(pointer_scripted);
    rmp::ui::detail::set_test_viewport(1280, 720);

    int hud_clicks = 0;
    int menu_clicks = 0;
    // The shape rmp::app drives: ONE frame boundary around a pass per scene.
    // Pass 0 is the HUD underneath, pass 1 the pause menu over it.
    auto frame = [&](bool hud_reachable) {
        rmp::ui::detail::begin_frame();
        rmp::ui::detail::set_pass_input(hud_reachable);
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
        if (rmp::ui::button("Back")) hud_clicks++;
        rmp::ui::end();
        rmp::ui::detail::set_pass_input(true);
        rmp::ui::begin({ .placement = rmp::ui::Align::BOTTOM_RIGHT });
        if (rmp::ui::button("Back")) menu_clicks++;
        rmp::ui::end();
        rmp::ui::detail::end_frame();
    };

    g_pointer = Clay_Vector2{ -1, -1 };
    g_down = false;
    frame(false);
    frame(false);

    const Clay_ElementId low = rmp::ui::detail::peek_element_id("Back", 0, 0);
    const Clay_ElementId high = rmp::ui::detail::peek_element_id("Back", 0, 1);
    check(low.id != high.id, "the same label in two passes is two elements");

    check(rmp::ui::detail::bounds_of("Back", 0, 1, nullptr),
          "bounds_of() can name the pass an element was declared in");
    check(!rmp::ui::detail::bounds_of("Back", 0, 0, nullptr),
          "and Clay itself only remembers the last pass of the frame");

    Clay_BoundingBox low_box{};
    Clay_BoundingBox high_box{};
    const bool remembered = rmp::ui::detail::bounds_of_id(low, &low_box) &&
        rmp::ui::detail::bounds_of_id(high, &high_box);
    check(remembered && low_box.y != high_box.y,
          "the per-pass snapshot remembers both, in the two places they were drawn");

    // The pass on top is clickable. It was not, until hit testing stopped going
    // through Clay: see run_two_pass_clicks() below for the whole of that.
    g_pointer =
        Clay_Vector2{ high_box.x + high_box.width / 2, high_box.y + high_box.height / 2 };
    g_down = true;
    frame(false);
    g_down = false;
    frame(false);
    check(menu_clicks == 1, "the pass on top is clickable");

    // What IS gated, and where: the pointer state every widget reads is turned
    // off for a pass input cannot reach, so nothing in the HUD under an open
    // menu can be pressed even once hit testing works.
    g_pointer =
        Clay_Vector2{ low_box.x + low_box.width / 2, low_box.y + low_box.height / 2 };
    g_down = true;
    frame(false);
    g_down = false;
    frame(false);
    check(hud_clicks == 0, "a pass input cannot reach is not clickable");

    rmp::ui::detail::set_pointer_provider(pointer_stub);
}

// image() used to hand Clay the address of its own parameter, which binds a
// temporary happily. Clay keeps that pointer until end() draws.
void run_image_lifetime() {
    std::printf("\n--- image() and the caller's texture ---\n");
    rmp::ui::detail::set_test_viewport(1280, 720);

    const void *given = nullptr;
    rmp::ui::begin();
    {
        // The lifetime of rmp::ui::image(rmp::assets::load_texture("icon.png")):
        // gone at the semicolon, long before end() draws.
        Texture2D local{};
        local.id = 77;
        local.width = 32;
        local.height = 16;
        rmp::ui::image(local);
        given = rmp::ui::detail::last_image_data();
        check(given != nullptr && given != static_cast<const void *>(&local),
              "image() copies the texture instead of pointing at the caller's");
    }
    rmp::ui::end();

    const auto *copy = static_cast<const Texture2D *>(given);
    check(copy != nullptr && copy->id == 77 && copy->width == 32,
          "and the copy still reads right once the caller's texture is gone");
}

// ---------------------------------------------------------------------------
// Hit testing with a scene stack
//
// A pause menu over a HUD is the flagship of the scene stack and it is what
// examples/scenes/01_stack shows, so "nothing in either scene can be clicked"
// is about as bad as a UI bug gets. Two causes, and both are here:
//
//   Clay_SetPointerState() ran before Clay_BeginLayout(), so it answered pass 0
//   from last frame's pass 1 and pass 1 from this frame's pass 0 — never from
//   the pass's own geometry.
//
//   end() cleared the press capture whenever the pointer was not down, and a
//   pass input cannot reach reports nothing as down. The HUD underneath wiped
//   the press the moment the player let go, so the menu's button never fired.
// ---------------------------------------------------------------------------

void run_two_pass_clicks() {
    std::printf("\n--- two passes: hover and click ---\n");
    rmp::ui::detail::set_pointer_provider(pointer_scripted);
    rmp::ui::detail::set_test_viewport(1280, 720);

    int hud_clicks = 0;
    int menu_clicks = 0;
    bool hud_reachable = false;

    // Exactly the shape rmp::app draws a stack in: one frame boundary, the
    // scene underneath first with its own input_below, the one on top second.
    auto frame = [&] {
        rmp::ui::detail::begin_frame();
        rmp::ui::detail::set_pass_input(hud_reachable);
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
        if (rmp::ui::button("Hud")) hud_clicks++;
        rmp::ui::end();
        rmp::ui::detail::set_pass_input(true);
        rmp::ui::begin({ .placement = rmp::ui::Align::BOTTOM_RIGHT });
        if (rmp::ui::button("Menu")) menu_clicks++;
        rmp::ui::end();
        rmp::ui::detail::end_frame();
    };

    auto click_at = [&](Clay_BoundingBox box) {
        g_pointer = Clay_Vector2{ box.x + box.width / 2, box.y + box.height / 2 };
        g_down = true;
        frame();
        g_down = false;
        frame();
    };

    g_pointer = Clay_Vector2{ -1, -1 };
    g_down = false;
    frame();
    frame();

    Clay_BoundingBox hud{};
    Clay_BoundingBox menu{};
    const bool laid_out = rmp::ui::detail::bounds_of_id(
                              rmp::ui::detail::peek_element_id("Hud", 0, 0), &hud) &&
        rmp::ui::detail::bounds_of_id(rmp::ui::detail::peek_element_id("Menu", 0, 1),
                                      &menu);
    check(laid_out && hud.width > 0 && menu.width > 0, "both passes laid out");

    // Hover answers on the first frame after the geometry exists, which is the
    // whole immediate-mode contract: one frame of lag and not two.
    g_pointer = Clay_Vector2{ menu.x + menu.width / 2, menu.y + menu.height / 2 };
    frame();
    check(rmp::ui::wants_pointer(), "the menu on top is hovered on the very next frame");

    // The menu is clickable with a HUD under it that input cannot reach. This
    // is the one that was broken twice over.
    click_at(menu);
    check(menu_clicks == 1, "pressing the menu's button clicks it");
    check(hud_clicks == 0, "and the HUD under it stays out of it");

    // The HUD is not hoverable while the menu owns the input.
    g_pointer = Clay_Vector2{ hud.x + hud.width / 2, hud.y + hud.height / 2 };
    frame();
    check(!rmp::ui::wants_pointer(), "a pass input cannot reach is not hovered");
    click_at(hud);
    check(hud_clicks == 0, "nor clicked");

    // input_below = true: now the scene underneath takes its own clicks, and
    // the pointer is nowhere near the menu.
    hud_reachable = true;
    frame();
    click_at(hud);
    check(hud_clicks == 1, "with input_below the scene underneath clicks too");
    check(menu_clicks == 1, "and the click does not also reach the scene above");

    rmp::ui::detail::set_pointer_provider(pointer_stub);
}

// A frame in which no scene draws any UI. There is no begin() to clear the
// capture flags, so the frame boundary has to - otherwise a pause menu popping
// while the pointer sits over one of its buttons leaves the game's mouse dead
// for good.
void run_idle_frame() {
    std::printf("\n--- a frame with no UI at all ---\n");
    rmp::ui::detail::set_pointer_provider(pointer_scripted);
    rmp::ui::detail::set_test_viewport(1280, 720);

    auto frame = [] {
        rmp::ui::detail::begin_frame();
        rmp::ui::begin();
        rmp::ui::button("Pause");
        rmp::ui::end();
        rmp::ui::detail::end_frame();
    };
    g_pointer = Clay_Vector2{ -1, -1 };
    g_down = false;
    frame();
    frame();

    Box pause = box_of("Pause");
    g_pointer = Clay_Vector2{ pause.x + pause.w / 2, pause.y + pause.h / 2 };
    frame();
    check(rmp::ui::wants_pointer(), "the pointer is over the menu");

    // The scene pops: the boundary still runs, nothing draws.
    rmp::ui::detail::begin_frame();
    rmp::ui::detail::end_frame();
    check(!rmp::ui::wants_pointer(),
          "and one frame with nothing drawn gives it straight back");

    rmp::ui::detail::set_pointer_provider(pointer_stub);
}

// An element with no area cannot be under the pointer. A headless frame has a
// viewport of 0x0 and every box in it is 0x0 at the origin, so a pointer resting
// at the origin is inside all of them at once — and on a real machine a
// minimised window reports the same 0x0.
void run_zero_area() {
    std::printf("\n--- an element with no area ---\n");
    rmp::ui::detail::set_pointer_provider(pointer_scripted);
    rmp::ui::detail::set_test_viewport(1280, 720);

    auto frame = [] {
        rmp::ui::begin();
        rmp::ui::panel({ .box = { .padding = 0, .id = "empty" } }, [] {});
        rmp::ui::button("Solid");
        rmp::ui::end();
    };
    g_pointer = Clay_Vector2{ -1, -1 };
    g_down = false;
    frame();
    frame();

    Box empty = box_of("empty");
    check(empty.w == 0 && empty.h == 0, "an empty panel with no padding has no area");

    // Exactly on it. The box test alone would say yes — the corner of a zero
    // box contains the corner of itself — so this is the rule and nothing else.
    g_pointer = Clay_Vector2{ empty.x, empty.y };
    frame();
    check(!rmp::ui::detail::pointer_over(rmp::ui::detail::peek_element_id("empty", 0, 0)),
          "nothing is ever over it, not even the point it sits on");
    check(!rmp::ui::wants_pointer(), "and the UI does not claim the pointer for it");

    rmp::ui::detail::set_pointer_provider(pointer_stub);
}

// An open dropdown is in front of whatever it covers. Hit testing is ours now,
// and a box test on its own would let a click go straight through the list into
// the control underneath.
void run_dropdown_occlusion() {
    std::printf("\n--- an open list covers what is under it ---\n");
    rmp::ui::detail::set_pointer_provider(pointer_scripted);
    rmp::ui::detail::set_test_viewport(1280, 720);

    static const char *items[] = { "Off", "Low", "High" };
    int quality = 0;
    int clicks = 0;
    auto frame = [&] {
        rmp::ui::begin();
        rmp::ui::panel([&] {
            rmp::ui::dropdown("Quality", &quality, items, 3);
            if (rmp::ui::button("Underneath")) clicks++;
        });
        rmp::ui::end();
    };

    g_pointer = Clay_Vector2{ -1, -1 };
    g_down = false;
    frame();
    frame();

    // Open the list.
    Box field = box_of("Quality");
    g_pointer = Clay_Vector2{ field.x + field.w * 0.8f, field.y + field.h / 2 };
    g_down = true;
    frame();
    g_down = false;
    frame();
    frame();

    Box under = box_of("Underneath");
    Clay_BoundingBox item{};
    const bool have_item = rmp::ui::detail::bounds_of_id(
        rmp::ui::detail::sub_id(rmp::ui::detail::peek_element_id("Quality", 0, 0), 2),
        &item);
    check(have_item, "the open list laid its items out");

    // The list hangs over the button below it. Aim where they overlap.
    const bool overlaps = item.y < under.y + under.h && item.y + item.height > under.y;
    check(overlaps, "and it hangs over the button underneath");

    g_pointer = Clay_Vector2{ item.x + item.width / 2, item.y + item.height / 2 };
    g_down = true;
    frame();
    g_down = false;
    frame();
    check(clicks == 0, "clicking the list does not press the button behind it");
    check(quality == 1, "it picks the item, which is what was under the pointer");
}

// A scroll container clips what is inside it. An item scrolled out of view is
// not on screen, so it must not be clickable either — the box it remembers is
// outside the container, which is precisely where the pointer must not find it.
void run_scroll_clip() {
    std::printf("\n--- a clipped list is not clickable outside its box ---\n");
    rmp::ui::detail::set_pointer_provider(pointer_scripted);
    rmp::ui::detail::set_test_viewport(1280, 720);

    int clicks = 0;
    auto frame = [&] {
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
        rmp::ui::scroll({ .height = 80, .id = "list" }, [&] {
            for (int i = 0; i < 8; i++) {
                if (rmp::ui::button(TextFormat("row%d", i))) clicks++;
            }
        });
        rmp::ui::end();
    };

    g_pointer = Clay_Vector2{ -1, -1 };
    g_down = false;
    frame();
    frame();

    Box list = box_of("list");
    Box last = box_of("row7");
    check(list.h > 0 && last.h > 0, "the list and its last row both laid out");
    check(last.y > list.y + list.h, "the last row sits below the bottom of the list");

    g_pointer = Clay_Vector2{ last.x + last.w / 2, last.y + last.h / 2 };
    g_down = true;
    frame();
    g_down = false;
    frame();
    check(clicks == 0, "a row clipped out of the list cannot be clicked");
    check(!rmp::ui::wants_pointer(), "and the UI does not claim the pointer for it");

    // The row that IS inside the box still works, or the check above would pass
    // for the wrong reason.
    Box first = box_of("row0");
    g_pointer = Clay_Vector2{ first.x + first.w / 2, first.y + first.h / 2 };
    g_down = true;
    frame();
    g_down = false;
    frame();
    check(clicks == 1, "and a row inside it still is");

    rmp::ui::detail::set_pointer_provider(pointer_stub);
}

// ---------------------------------------------------------------------------
// Focus without being asked for it, and the button that presses it
//
// A scene pushed with one "Play again" button on it has to be pressable with a
// controller the moment it appears. It was not: nothing held the focus until
// the player tapped Down or Tab first, so every one of the six example games
// called rmp::ui::focus() in its _ready() to work around it.
// ---------------------------------------------------------------------------

void run_default_focus() {
    std::printf("\n--- the focus nobody asked for ---\n");
    rmp::ui::detail::set_pointer_provider(pointer_scripted);
    rmp::ui::detail::set_test_viewport(1280, 720);
    g_pointer = Clay_Vector2{ -1, -1 };
    g_down = false;
    rmp::ui::focus(""); // nothing focused, which is how a fresh scene starts
    rmp::ui::detail::set_focus_visible(false);

    rmp::ui::begin();
    rmp::ui::button("Play again");
    rmp::ui::button("Quit");
    rmp::ui::end();
    check(rmp::ui::focused() == "Play again",
          "the first widget of a pass takes the focus on its own");

    // ...and does NOT wear a ring for it. Every pass gives itself a focus now,
    // so a player holding a mouse would find one on the first button of every
    // menu that opens -- and the recorded render hash is what would say so.
    check(!rmp::ui::detail::focus_visible(), "a focus nobody asked for is not drawn");
    g_nav.y = 1;
    rmp::ui::begin();
    rmp::ui::button("Play again");
    rmp::ui::button("Quit");
    rmp::ui::end();
    g_nav.y = 0;
    check(rmp::ui::detail::focus_visible(), "touching the keyboard draws it");

    // And going back to the mouse puts it away again.
    g_pointer = Clay_Vector2{ 4, 4 };
    g_down = true;
    rmp::ui::begin();
    rmp::ui::button("Play again");
    rmp::ui::end();
    g_down = false;
    check(!rmp::ui::detail::focus_visible(), "and clicking puts it away");
    rmp::ui::detail::set_pointer_provider(pointer_stub);

    rmp::ui::focus("");
    rmp::ui::detail::set_focus_visible(false);
    rmp::ui::begin();
    rmp::ui::button("Play again");
    rmp::ui::button("Quit");
    rmp::ui::end();

    // An explicit focus() still wins, and keeps winning: the default only
    // applies when the focus belongs to nothing on screen.
    rmp::ui::focus("Quit");
    check(rmp::ui::detail::focus_visible(),
          "a focus the game asked for by name is drawn, because it meant it");
    rmp::ui::begin();
    rmp::ui::button("Play again");
    rmp::ui::button("Quit");
    rmp::ui::end();
    check(rmp::ui::focused() == "Quit", "and an explicit focus() outranks it");
    rmp::ui::begin();
    rmp::ui::button("Play again");
    rmp::ui::button("Quit");
    rmp::ui::end();
    check(rmp::ui::focused() == "Quit", "for as long as that widget is on screen");
}

void run_default_focus_two_passes() {
    std::printf("\n--- the focus when two scenes draw ---\n");
    rmp::ui::detail::set_test_viewport(1280, 720);

    bool hud_reachable = false;
    auto frame = [&] {
        rmp::ui::detail::begin_frame();
        rmp::ui::detail::set_pass_input(hud_reachable);
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
        rmp::ui::button("Hud");
        rmp::ui::end();
        rmp::ui::detail::set_pass_input(true);
        rmp::ui::begin({ .placement = rmp::ui::Align::BOTTOM_RIGHT });
        rmp::ui::button("Resume");
        rmp::ui::button("Give up");
        rmp::ui::end();
        rmp::ui::detail::end_frame();
    };

    // A pause menu over a HUD it suppresses: the menu's first button, not the
    // HUD's, even though the HUD is declared first.
    rmp::ui::focus("");
    frame();
    check(rmp::ui::focused() == "Resume",
          "the pass on top takes the focus, not the one underneath it");

    // With input_below the HUD is reachable again, so it is focusable and it is
    // first — and the menu must not steal the focus back every frame, or the
    // player could never walk down into the HUD at all.
    hud_reachable = true;
    rmp::ui::focus("");
    frame();
    check(rmp::ui::focused() == "Hud",
          "with input_below the scene underneath can hold the focus");
    frame();
    check(rmp::ui::focused() == "Hud", "and the pass above does not take it back");
}

void run_activate() {
    std::printf("\n--- pressing a button with the keyboard ---\n");
    rmp::ui::detail::set_test_viewport(1280, 720);
    rmp::ui::focus("");

    int played = 0;
    int quit = 0;
    auto frame = [&] {
        rmp::ui::begin();
        if (rmp::ui::button("Play again")) played++;
        if (rmp::ui::button("Quit")) quit++;
        rmp::ui::end();
    };

    frame(); // the first one is what takes the focus
    check(rmp::ui::focused() == "Play again", "the first button has the focus");

    g_nav.activate = true;
    frame();
    g_nav.activate = false;
    check(played == 1, "Enter presses the focused button");
    check(quit == 0, "and only that one");

    // Down, then Enter: the other one.
    g_nav.y = 1;
    frame();
    g_nav.y = 0;
    check(rmp::ui::focused() == "Quit", "Down moves the focus");
    g_nav.activate = true;
    frame();
    g_nav.activate = false;
    check(quit == 1, "and Enter presses what it landed on");
    check(played == 1, "without pressing the one it left");

    // A pass input cannot reach cannot be activated either, however loudly the
    // player presses.
    rmp::ui::detail::begin_frame();
    rmp::ui::detail::set_pass_input(false);
    g_nav.activate = true;
    rmp::ui::begin();
    if (rmp::ui::button("Play again")) played++;
    rmp::ui::end();
    rmp::ui::detail::end_frame();
    g_nav.activate = false;
    check(played == 1, "a pass input cannot reach does not answer the keyboard");

    g_nav = rmp::ui::detail::NavState{};
}

} // namespace

int main() {
    rmp::ui::detail::set_measure_provider(measure_stub);
    rmp::ui::detail::set_pointer_provider(pointer_stub);
    // Nothing held down and nothing pressed, until a test says otherwise.
    rmp::ui::detail::set_nav_provider(nav_scripted);

    // The design resolution these are all measured against is APP_WINDOW_*,
    // straight from [window] in raylib_multiplatform.toml.
    std::printf("design resolution: %dx%d\n", APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT);

    run_at(800, 600, "small window");
    run_at(1280, 720, "720p");
    run_at(1920, 1080, "1080p");
    run_at(2560, 1440, "1440p");

    run_containers(1280, 720);
    run_containers(800, 600);
    run_grid();
    run_interaction();
    run_dropdown();
    run_focus_by_name();
    run_slider_nav();
    run_two_sliders();
    run_keyboard_capture();
    run_arena_overflow();
    run_label_overflow();
    run_nested_grids();
    run_two_passes();
    run_two_pass_clicks();
    run_default_focus();
    run_default_focus_two_passes();
    run_activate();
    run_idle_frame();
    run_zero_area();
    run_dropdown_occlusion();
    run_scroll_clip();
    run_image_lifetime();
    run_sizes();
    run_themes();
    run_breakpoints();

    std::printf("\n--- scale limits ---\n");

    // Automatic scale, both ends clamped. A tiny window must not make the text
    // unreadable, and a huge one must not turn a button into a billboard.
    rmp::ui::detail::set_test_viewport(160, 120);
    draw_menu();
    check_near(rmp::ui::scale(), 0.5f, 0.001f, "a tiny viewport clamps the scale at 0.5");

    rmp::ui::detail::set_test_viewport(7680, 4320);
    draw_menu();
    check_near(rmp::ui::scale(), 4.0f, 0.001f, "a huge viewport clamps the scale at 4.0");

    // Very wide and short: the tighter axis has to win, or the menu runs off
    // the bottom of the screen. This is why the scale uses min() and not max().
    rmp::ui::detail::set_test_viewport(3840, 480);
    draw_menu();
    float by_height = 480.0f / static_cast<float>(APP_WINDOW_HEIGHT);
    if (by_height < 0.5f) by_height = 0.5f;
    check_near(rmp::ui::scale(), by_height, 0.001f,
               "on a very wide window the height decides");

    // A pinned scale ignores the viewport entirely — this is how an "interface
    // Size" accessibility option would work.
    rmp::ui::set_scale(2.0f);
    rmp::ui::detail::set_test_viewport(1280, 720);
    draw_menu();
    check_near(rmp::ui::scale(), 2.0f, 0.001f, "set_scale() pins it");
    rmp::ui::set_scale(0.0f);
    draw_menu();
    check(std::fabs(rmp::ui::scale() - 2.0f) > 0.001f,
          "set_scale(0) goes back to automatic");

    // LAST, and it has to be: Clay's element hashmap never shrinks once it has
    // filled, so a frame that crosses the ceiling leaves the context unable to
    // lay anything out for the rest of the process. That is the reason the
    // warning it produces says "until the game is restarted", and it is why no
    // test may run after this one.
    run_element_ceiling();

    rmp::ui::shutdown();

    std::printf("\n%s\n", g_failures == 0 ? "PASS" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
