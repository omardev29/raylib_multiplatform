// ---------------------------------------------------------------------------
// rmp::Camera: following, limits, the two conversions, and the three things
// in the framework that read it -- the default bounds, the pointer and the
// hit test. No window: the camera projects onto the design size when there is
// none, which is exactly what makes these deterministic.
//
// Not here yet, because the camera does not have it yet (phase 11): the
// exponential smoothing and the shake.
// ---------------------------------------------------------------------------

#include <doctest.h>

#include "../src/rmp/object_internal.h"
#include "../src/rmp/ui/internal.h"

#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/scene.h>

namespace {

constexpr float kW = APP_WINDOW_WIDTH;
constexpr float kH = APP_WINDOW_HEIGHT;

class World : public rmp::Scene {
public:
    ~World() override { rmp::objects::detail::release_scene(*this); }
};

rmp::input::detail::DeviceState g_devices;
void fake_sample(rmp::input::detail::DeviceState *out) { *out = g_devices; }

struct Fixture {
    Fixture() {
        rmp::objects::detail::reset_for_tests();
        rmp::objects::detail::reset_pointer_for_tests();
        rmp::input::detail::reset();
        g_devices = rmp::input::detail::DeviceState{};
        rmp::input::detail::set_sample_provider(fake_sample);
        rmp::ui::detail::begin_capture_frame();
        rmp::input::detail::begin_frame();
    }
    ~Fixture() {
        rmp::objects::detail::reset_for_tests();
        rmp::input::detail::reset();
        g_devices = rmp::input::detail::DeviceState{};
    }
};

// A whole frame of a scene, the way the scene stack drives it, camera included.
void frame(rmp::Scene &scene, float delta = 1.0f / 60) {
    rmp::input::detail::begin_frame();
    rmp::objects::detail::pointer(scene);
    rmp::objects::detail::update(scene, delta);
    rmp::objects::detail::collide(scene);
    scene.camera.detail_settle();
    rmp::objects::detail::collect();
}

bool near(Vector2 a, Vector2 b) {
    return a.x == doctest::Approx(b.x).epsilon(0.001) &&
        a.y == doctest::Approx(b.y).epsilon(0.001);
}

} // namespace

TEST_SUITE("camera") {
    TEST_CASE_FIXTURE(Fixture, "a camera nobody touched is the identity") {
        // The promise every one-screen game relies on: a scene that never
        // mentions the camera draws where it always did.
        World world;
        CHECK(near(world.camera.position, Vector2{ kW / 2, kH / 2 }));
        CHECK(near(world.camera.to_world(Vector2{ 0, 0 }), Vector2{ 0, 0 }));
        CHECK(near(world.camera.to_world(Vector2{ 123, 45 }), Vector2{ 123, 45 }));
        CHECK(near(world.camera.to_screen(Vector2{ 123, 45 }), Vector2{ 123, 45 }));
        const Rectangle v = world.camera.view();
        CHECK(v.x == doctest::Approx(0));
        CHECK(v.y == doctest::Approx(0));
        CHECK(v.width == doctest::Approx(kW));
        CHECK(v.height == doctest::Approx(kH));
        // And a frame does not move it.
        frame(world);
        CHECK(near(world.camera.position, Vector2{ kW / 2, kH / 2 }));
    }

    TEST_CASE_FIXTURE(Fixture, "follow centres the view on the object, after it moved") {
        World world;
        rmp::Object &player = world.spawn({ .position = { 1000, 500 } });
        player.velocity = Vector2{ 600, 0 };
        world.camera.follow = player.handle();
        frame(world, 0.5f);
        // Where the player ENDED UP this frame, not where it started.
        CHECK(near(world.camera.position, player.position));
        CHECK(player.position.x == doctest::Approx(1300));

        SUBCASE("and a target that dies simply stops being followed") {
            player.destroy();
            frame(world);
            CHECK(near(world.camera.position, Vector2{ 1300, 500 }));
            world.camera.position = Vector2{ 7, 7 };
            frame(world);
            CHECK(near(world.camera.position, Vector2{ 7, 7 }));
        }
    }

    TEST_CASE_FIXTURE(Fixture, "limits win over follow") {
        World world;
        world.camera.limits = Rectangle{ 0, 0, 1600, 900 };
        rmp::Object &player = world.spawn({ .position = { 100, 100 } });
        world.camera.follow = player.handle();
        frame(world);
        // Near the top-left corner of the level: the camera stops at the edge
        // and shows the level, not the void.
        CHECK(near(world.camera.position, Vector2{ kW / 2, kH / 2 }));
        const Rectangle v = world.camera.view();
        CHECK(v.x == doctest::Approx(0));
        CHECK(v.y == doctest::Approx(0));

        player.position = Vector2{ 1500, 850 };
        frame(world);
        CHECK(near(world.camera.position, Vector2{ 1600 - kW / 2, 900 - kH / 2 }));

        player.position = Vector2{ 800, 450 };
        frame(world);
        CHECK(
            near(world.camera.position, Vector2{ 800, 450 })); // inside: follows exactly
    }

    TEST_CASE_FIXTURE(Fixture, "a zero width or height leaves that axis unbounded") {
        // The runner: follow x wherever it goes, pin y. It used to need a
        // limits rectangle as wide as an invented level length.
        World world;
        world.camera.limits = Rectangle{ 0, 0, 0, kH };
        rmp::Object &player = world.spawn({ .position = { 50000, 50 } });
        world.camera.follow = player.handle();
        frame(world);
        CHECK(world.camera.position.x == doctest::Approx(50000));
        CHECK(world.camera.position.y == doctest::Approx(kH / 2)); // pinned by the height

        world.camera.limits = Rectangle{ 0, 0, 1600, 0 };
        player.position = Vector2{ 5, -900 };
        frame(world);
        CHECK(world.camera.position.x == doctest::Approx(kW / 2)); // clamped by the width
        CHECK(world.camera.position.y == doctest::Approx(-900)); // free
    }

    TEST_CASE_FIXTURE(Fixture,
                      "a limit narrower than the view is centred, not overshot") {
        World world;
        world.camera.limits = Rectangle{ 100, 100, 300, 200 };
        world.camera.position = Vector2{ 5000, 5000 };
        frame(world);
        CHECK(near(world.camera.position, Vector2{ 250, 200 }));
    }

    TEST_CASE_FIXTURE(Fixture,
                      "world and screen convert both ways with zoom and a move") {
        World world;
        world.camera.position = Vector2{ 1000, 600 };
        world.camera.zoom = 2;
        // The centre of the screen is the camera's position.
        CHECK(
            near(world.camera.to_world(Vector2{ kW / 2, kH / 2 }), Vector2{ 1000, 600 }));
        // Twice as big: a screen pixel is half a world unit.
        CHECK(near(world.camera.to_world(Vector2{ kW / 2 + 100, kH / 2 }),
                   Vector2{ 1050, 600 }));
        for (const Vector2 p :
             { Vector2{ 0, 0 }, Vector2{ 1234, -50 }, Vector2{ -3, 999 } }) {
            CHECK(near(world.camera.to_world(world.camera.to_screen(p)), p));
        }
        const Rectangle v = world.camera.view();
        CHECK(v.width == doctest::Approx(kW / 2));
        CHECK(v.height == doctest::Approx(kH / 2));
        CHECK(v.x == doctest::Approx(1000 - kW / 4));
    }

    TEST_CASE_FIXTURE(Fixture,
                      "an empty bounds means the camera's view, not the window") {
        // Edge::CLAMP with no bounds: the object stays inside what is VISIBLE.
        // With the camera at x = 5000 the window rectangle is somewhere nothing
        // on screen is, and clamping to it would drag the object off screen.
        World world;
        world.camera.position = Vector2{ 5000, 600 };
        rmp::Object &box =
            world.spawn({ .position = { 5000, 600 }, .shape = rmp::rect({ 20, 20 }) });
        box.edges = rmp::Edge::CLAMP;
        box.velocity = Vector2{ 100000, 0 };
        frame(world);
        const Rectangle v = world.camera.view();
        CHECK(box.position.x == doctest::Approx(v.x + v.width - 10));
        CHECK(box.position.x > 5000);
    }

    TEST_CASE_FIXTURE(
        Fixture, "the pointer is in world units, through the current scene's camera") {
        // rmp::input::pointer() reads Scene::current(); this scene is not on the
        // stack, so it goes through the camera directly and the fallback scene's
        // identity camera is what pointer() answers with.
        World world;
        world.camera.position = Vector2{ 1000, 600 };
        g_devices.pointer = Vector2{ kW / 2, kH / 2 };
        rmp::input::detail::begin_frame();
        CHECK(near(rmp::input::pointer_screen(), Vector2{ kW / 2, kH / 2 }));
        CHECK(near(world.camera.to_world(rmp::input::pointer_screen()),
                   Vector2{ 1000, 600 }));
    }

    TEST_CASE_FIXTURE(Fixture, "on_click hits what is under the pointer in the world") {
        World world;
        world.camera.position = Vector2{ 1000, 600 };
        int clicks = 0;
        rmp::Object &button =
            world.spawn({ .position = { 1000, 600 }, .shape = rmp::rect({ 40, 40 }) });
        button.on_click([&clicks](rmp::Object &) { clicks++; });

        // The object is at the centre of the VIEW, which is the centre of the
        // screen. A press there, in pixels, has to land on it.
        g_devices.pointer = Vector2{ kW / 2, kH / 2 };
        g_devices.mouse[MOUSE_BUTTON_LEFT] = true;
        frame(world);
        g_devices.mouse[MOUSE_BUTTON_LEFT] = false;
        frame(world);
        CHECK(clicks == 1);

        SUBCASE("and a press at the object's WORLD coordinates in pixels misses it") {
            g_devices.pointer = Vector2{ 1000, 600 }; // off screen at this framing
            g_devices.mouse[MOUSE_BUTTON_LEFT] = true;
            frame(world);
            g_devices.mouse[MOUSE_BUTTON_LEFT] = false;
            frame(world);
            CHECK(clicks == 1);
        }
    }
}
