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
    if (!rmp::ui::detail::bounds_of(label, 0, &b)) {
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

} // namespace

int main() {
    rmp::ui::detail::set_measure_provider(measure_stub);
    rmp::ui::detail::set_pointer_provider(pointer_stub);

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

    rmp::ui::shutdown();

    std::printf("\n%s\n", g_failures == 0 ? "PASS" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
