// ---------------------------------------------------------------------------
// examples/input/01_actions.cpp
//
// Named actions, eight-direction movement in one line, and the part nobody
// thinks about until it bites: who gets to hear a press.
//
// This file is REFERENCE ONLY (not compiled by the build). See README.md.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/input.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

class PauseScene : public rmp::Scene {
public:
    void _draw() override {
        rmp::ui::begin();
        rmp::ui::text("Paused");
        if (rmp::ui::button("Resume")) rmp::Scene::pop();
        rmp::ui::end();
    }
};

class GameScene : public rmp::Scene {
public:
    void _ready() override {
        // The name first, then as many bindings as you like. The TYPE of each
        // argument says which device it is — KeyboardKey, MouseButton and
        // GamepadButton are three different enums — so passing anything else is
        // a compile error, and there is no string to spell wrong.
        rmp::input::action("jump", KEY_SPACE, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
        rmp::input::action("fire", MOUSE_BUTTON_LEFT, GAMEPAD_BUTTON_RIGHT_TRIGGER_1);
        rmp::input::action("pause", KEY_ESCAPE, GAMEPAD_BUTTON_MIDDLE_RIGHT);

        // An analogue stick needs to know which half counts, and stick() is the
        // only place you write something that is not a plain raylib constant.
        rmp::input::action("aim_left", KEY_Q,
                           rmp::input::stick(GAMEPAD_AXIS_RIGHT_X, -1));

        // Defining these here is safe even though the player enters this scene
        // over and over: registering a name again REPLACES it, never adds to it.
    }

    void _update(float delta) override {
        // Eight directions, keyboard and d-pad and stick at once, normalised so
        // the diagonal is not 41 % faster — with no configuration at all. The
        // six factory actions (move_*, ui_accept, ui_cancel) are already there,
        // and they are ordinary actions you can redefine.
        position.x += rmp::input::vector().x * speed * delta;
        position.y += rmp::input::vector().y * speed * delta;

        if (rmp::input::just_pressed("jump")) jump();

        // AND THIS IS THE PART THAT MATTERS. "fire" is bound to the left mouse
        // button, and there is a pause menu one push away with a Resume button
        // in the middle of the screen. A finger that presses Resume must not
        // also shoot.
        //
        // It does not, and nothing here says so. A query BY NAME is routed: an
        // action on a mouse button goes quiet while the UI wants the pointer,
        // one on a key goes quiet while a text field has the keyboard, and all
        // of them go quiet in a scene with something above it that kept the
        // input. Compare with what it takes by hand:
        //
        //     if (!rmp::ui::wants_pointer() && IsMouseButtonPressed(0)) shoot();
        //
        if (rmp::input::just_pressed("fire")) shoot();

        if (rmp::input::just_pressed("pause")) rmp::Scene::push<PauseScene>();

        // The escape hatch, for when you want to know regardless of who else is
        // listening. The argument type says which of the two you asked for, and
        // you can see it at the call site.
        if (rmp::input::pressed(KEY_LEFT_SHIFT))
            speed = 400.0f;
        else
            speed = 200.0f;
    }

    void _draw() override {
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
        // The pointer is one pointer: mouse on a desktop, finger on a phone,
        // and the same code on all fourteen targets with no #ifdef.
        rmp::ui::text(rmp::input::pointer_down() ? "pointer down" : "pointer up");
        rmp::ui::end();
    }

private:
    void jump() {}
    void shoot() {}

    Vector2 position{ 400, 225 };
    float speed = 200.0f;
};

RMP_GAME(GameScene);
