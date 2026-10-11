// ---------------------------------------------------------------------------
// examples/scenes/02_stretch/src/main.cpp
//
// [window] stretch: what the world does in a window that is not the size it
// was designed for -- a desktop window dragged wider, a phone held upright, a
// 21:9 monitor. [window] width and height in raylib_multiplatform.toml are the
// DESIGN size, 800x450 here, and this court is exactly that big.
//
//   LETTERBOX  the design, scaled to fit and centred; the rest of the window is
//              bars of the clear colour (`background`, below). Every player
//              sees the same court, and the HUD stays on it.
//   EXPAND     scaled the same, with no bars: more of the world shows where the
//              window is longer than the design -- the stands around the court.
//   NONE       1:1 around the camera: a bigger window shows more of the world
//              at the same size, a smaller one less.
//
// Resize the window, and press M (the gamepad's Y, or the button) to go round
// the three. The camera, the pointer and the interface follow the mode, so
// nothing below knows which one is on: the court is drawn in world units, and
// the ball bounces off the court's lines whatever the window does.
//
// The framework's .toml says letterbox, so that is where this starts. On
// Android and on a Linux console, raylib scales the design to the display
// itself, so every mode is a letterbox there.
//
// Run it: `rmp example 02_stretch`.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

#include <string>
#include <string_view>

namespace {

// The court's grass, its lines, and the stands beyond it: the part of the
// world only EXPAND and NONE ever show.
constexpr Color STANDS{ 52, 64, 92, 255 };
constexpr Color GRASS{ 46, 125, 60, 255 };
constexpr Color LINE = WHITE;

std::string_view mode_name() {
    switch (rmp::app::stretch()) {
        case rmp::app::Stretch::NONE:
            return "NONE";
        case rmp::app::Stretch::LETTERBOX:
            return "LETTERBOX";
        case rmp::app::Stretch::EXPAND:
            return "EXPAND";
    }
    return "?";
}

// LETTERBOX, EXPAND, NONE, and round again.
void next_mode() {
    switch (rmp::app::stretch()) {
        case rmp::app::Stretch::LETTERBOX:
            rmp::app::set_stretch(rmp::app::Stretch::EXPAND);
            break;
        case rmp::app::Stretch::EXPAND:
            rmp::app::set_stretch(rmp::app::Stretch::NONE);
            break;
        case rmp::app::Stretch::NONE:
            rmp::app::set_stretch(rmp::app::Stretch::LETTERBOX);
            break;
    }
}

// An outline: a rectangle that is not filled.
rmp::Shape outline(Vector2 size) {
    rmp::Shape s = rmp::rect(size);
    s.color = LINE;
    s.filled = false;
    s.thickness = 4;
    return s;
}

} // namespace

class CourtScene : public rmp::Scene {
public:
    void _ready() override {
        // The bars' colour, under LETTERBOX. The scene's own, so that the
        // bars are not the court and not the stands.
        background = Color{ 8, 8, 12, 255 };

        rmp::input::action("next_mode", KEY_M, GAMEPAD_BUTTON_RIGHT_FACE_UP);

        // The world, in world units, centred on the camera -- which starts
        // centred on the design, (400, 225). Higher layers draw on top.
        rmp::Shape stands = rmp::rect({ 2400, 1600 });
        stands.color = STANDS;
        spawn({ .position = { 400, 225 }, .shape = stands, .layer = -2 });
        rmp::Shape grass = rmp::rect({ 800, 450 });
        grass.color = GRASS;
        spawn({ .position = { 400, 225 }, .shape = grass, .layer = -1 });
        // The lines on top, ball included, so that nothing ever hides them:
        // tools/examples_build.sh finds the court by its white lines.
        spawn({ .position = { 400, 225 }, .shape = outline({ 760, 410 }), .layer = 1 });
        spawn({ .position = { 400, 225 }, .shape = outline({ 4, 410 }), .layer = 1 });
        rmp::Shape circle = rmp::circle(60);
        circle.color = LINE;
        circle.filled = false;
        spawn({ .position = { 400, 225 }, .shape = circle, .layer = 1 });

        // A ball that bounces off the lines, which are where they are in
        // every mode: `bounds` is in world units, like everything here.
        rmp::Shape ball = rmp::circle(10);
        ball.color = GOLD;
        spawn({ .position = { 300, 160 },
                .shape = ball,
                .velocity = { 230, 150 },
                .edges = rmp::Edge::BOUNCE,
                .bounds = { 20, 20, 760, 410 } });
    }

    void _update(float /*delta*/) override {
        if (rmp::input::just_pressed("next_mode")) next_mode();
    }

    void _draw() override {
        // The HUD stays inside the game's area: on the court, scaled with it,
        // under LETTERBOX; at the window's corner otherwise.
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
        rmp::ui::panel([] {
            rmp::ui::text("Stretch: " + std::string(mode_name()));
            rmp::ui::text(std::to_string(GetScreenWidth()) + "x" +
                              std::to_string(GetScreenHeight()) + ", designed for " +
                              std::to_string(RMP_WINDOW_WIDTH) + "x" +
                              std::to_string(RMP_WINDOW_HEIGHT),
                          { .color = rmp::ui::ColorRole::MUTED, .size = 14 });
            if (rmp::ui::button("Next mode", { .size = rmp::ui::Size::SMALL }))
                next_mode();
        });
        rmp::ui::end();
    }
};

RMP_GAME(CourtScene);
