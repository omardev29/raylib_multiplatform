// ---------------------------------------------------------------------------
// rmp::Camera: following, limits, the two conversions, and the three things
// in the framework that read it -- the default bounds, the pointer and the
// hit test. No window: the camera projects onto the design size when there is
// none, which is exactly what makes these deterministic.
//
// And phase 11's two: the exponential smoothing, whose whole point is that it
// does not depend on the frame rate, and the shake, whose whole point is that
// nothing gameplay reads can see it.
// ---------------------------------------------------------------------------

#include <doctest.h>

#include "../src/rmp/object_internal.h"
#include "../src/rmp/ui/internal.h"

#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/scene.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

constexpr float SCREEN_WIDTH = RMP_WINDOW_WIDTH;
constexpr float SCREEN_HEIGHT = RMP_WINDOW_HEIGHT;

class World : public rmp::Scene {
public:
    ~World() override { rmp::objects::detail::release_scene(*this); }
};

// The fake device the input seam reads. The dot says "file state".
struct {
    rmp::input::detail::DeviceState devices;
} fake;
void fake_sample(rmp::input::detail::DeviceState *out) { *out = fake.devices; }

struct Fixture {
    Fixture() {
        rmp::objects::detail::reset_for_tests();
        rmp::objects::detail::reset_pointer_for_tests();
        rmp::input::detail::reset();
        fake.devices = rmp::input::detail::DeviceState{};
        rmp::input::detail::set_sample_provider(fake_sample);
        rmp::ui::detail::begin_capture_frame();
        rmp::input::detail::begin_frame();
    }
    ~Fixture() {
        rmp::objects::detail::reset_for_tests();
        rmp::input::detail::reset();
        fake.devices = rmp::input::detail::DeviceState{};
    }
};

// A whole frame of a scene, the way the scene stack drives it, camera included.
void frame(rmp::Scene &scene, float delta = 1.0f / 60) {
    rmp::input::detail::begin_frame();
    rmp::objects::detail::pointer(scene);
    rmp::objects::detail::update(scene, delta);
    rmp::objects::detail::collide(scene);
    scene.camera.detail_settle(delta);
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
        CHECK(
            near(world.camera.position, Vector2{ SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 }));
        CHECK(near(world.camera.to_world(Vector2{ 0, 0 }), Vector2{ 0, 0 }));
        CHECK(near(world.camera.to_world(Vector2{ 123, 45 }), Vector2{ 123, 45 }));
        CHECK(near(world.camera.to_screen(Vector2{ 123, 45 }), Vector2{ 123, 45 }));
        const Rectangle v = world.camera.view();
        CHECK(v.x == doctest::Approx(0));
        CHECK(v.y == doctest::Approx(0));
        CHECK(v.width == doctest::Approx(SCREEN_WIDTH));
        CHECK(v.height == doctest::Approx(SCREEN_HEIGHT));
        // And a frame does not move it.
        frame(world);
        CHECK(
            near(world.camera.position, Vector2{ SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 }));
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
        CHECK(
            near(world.camera.position, Vector2{ SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 }));
        const Rectangle v = world.camera.view();
        CHECK(v.x == doctest::Approx(0));
        CHECK(v.y == doctest::Approx(0));

        player.position = Vector2{ 1500, 850 };
        frame(world);
        CHECK(near(world.camera.position,
                   Vector2{ 1600 - SCREEN_WIDTH / 2, 900 - SCREEN_HEIGHT / 2 }));

        player.position = Vector2{ 800, 450 };
        frame(world);
        CHECK(
            near(world.camera.position, Vector2{ 800, 450 })); // inside: follows exactly
    }

    TEST_CASE_FIXTURE(Fixture, "a zero width or height leaves that axis unbounded") {
        // The runner: follow x wherever it goes, pin y. It used to need a
        // limits rectangle as wide as an invented level length.
        World world;
        world.camera.limits = Rectangle{ 0, 0, 0, SCREEN_HEIGHT };
        rmp::Object &player = world.spawn({ .position = { 50000, 50 } });
        world.camera.follow = player.handle();
        frame(world);
        CHECK(world.camera.position.x == doctest::Approx(50000));
        CHECK(world.camera.position.y ==
              doctest::Approx(SCREEN_HEIGHT / 2)); // pinned by the height

        world.camera.limits = Rectangle{ 0, 0, 1600, 0 };
        player.position = Vector2{ 5, -900 };
        frame(world);
        CHECK(world.camera.position.x ==
              doctest::Approx(SCREEN_WIDTH / 2)); // clamped by the width
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
        CHECK(near(world.camera.to_world(Vector2{ SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 }),
                   Vector2{ 1000, 600 }));
        // Twice as big: a screen pixel is half a world unit.
        CHECK(near(
            world.camera.to_world(Vector2{ SCREEN_WIDTH / 2 + 100, SCREEN_HEIGHT / 2 }),
            Vector2{ 1050, 600 }));
        for (const Vector2 p :
             { Vector2{ 0, 0 }, Vector2{ 1234, -50 }, Vector2{ -3, 999 } }) {
            CHECK(near(world.camera.to_world(world.camera.to_screen(p)), p));
        }
        const Rectangle v = world.camera.view();
        CHECK(v.width == doctest::Approx(SCREEN_WIDTH / 2));
        CHECK(v.height == doctest::Approx(SCREEN_HEIGHT / 2));
        CHECK(v.x == doctest::Approx(1000 - SCREEN_WIDTH / 4));
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
        fake.devices.pointer = Vector2{ SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 };
        rmp::input::detail::begin_frame();
        CHECK(near(rmp::input::pointer_screen(),
                   Vector2{ SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 }));
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
        fake.devices.pointer = Vector2{ SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 };
        fake.devices.mouse[MOUSE_BUTTON_LEFT] = true;
        frame(world);
        fake.devices.mouse[MOUSE_BUTTON_LEFT] = false;
        frame(world);
        CHECK(clicks == 1);

        SUBCASE("and a press at the object's WORLD coordinates in pixels misses it") {
            fake.devices.pointer = Vector2{ 1000, 600 }; // off screen at this framing
            fake.devices.mouse[MOUSE_BUTTON_LEFT] = true;
            frame(world);
            fake.devices.mouse[MOUSE_BUTTON_LEFT] = false;
            frame(world);
            CHECK(clicks == 1);
        }
    }

    TEST_CASE_FIXTURE(Fixture, "on_drag hands over the movement in world units") {
        // What a drag is for is `self.position += moved`, and that only keeps
        // the object under the finger if `moved` is in the units `position`
        // is in. In pixels, a camera at zoom 2 moved it twice as far as the
        // finger went.
        World world;
        world.camera.position = Vector2{ 1000, 600 };
        rmp::Object &piece =
            world.spawn({ .position = { 1000, 600 }, .shape = rmp::rect({ 40, 40 }) });
        piece.on_drag([](rmp::Object &self, Vector2 moved) {
            self.position.x += moved.x;
            self.position.y += moved.y;
        });

        auto drag = [&](Vector2 from, Vector2 by, int steps) {
            fake.devices.pointer = from;
            fake.devices.mouse[MOUSE_BUTTON_LEFT] = true;
            frame(world);
            for (int i = 1; i <= steps; i++) {
                const float t = static_cast<float>(i) / static_cast<float>(steps);
                fake.devices.pointer = Vector2{ from.x + by.x * t, from.y + by.y * t };
                frame(world);
            }
            fake.devices.mouse[MOUSE_BUTTON_LEFT] = false;
            frame(world);
        };
        const Vector2 centre{ SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 };

        SUBCASE("at zoom 1 a pixel is a unit") {
            drag(centre, Vector2{ 30, -20 }, 3);
            CHECK(near(piece.position, Vector2{ 1030, 580 }));
        }
        SUBCASE("at zoom 2 the object stays under the finger") {
            world.camera.zoom = 2;
            const Vector2 grabbed = world.camera.to_world(centre);
            drag(centre, Vector2{ 60, 40 }, 4);
            // 60 pixels at zoom 2 are 30 units, and the point grabbed is
            // still under the pointer.
            CHECK(near(piece.position, Vector2{ 1030, 620 }));
            const Vector2 now =
                world.camera.to_world(Vector2{ centre.x + 60, centre.y + 40 });
            CHECK(near(Vector2{ now.x - piece.position.x, now.y - piece.position.y },
                       Vector2{ grabbed.x - 1000, grabbed.y - 600 }));
        }
        SUBCASE("a rotated camera drags the way the finger goes") {
            world.camera.rotation = 90;
            const Vector2 target =
                world.camera.to_world(Vector2{ centre.x + 50, centre.y });
            drag(centre, Vector2{ 50, 0 }, 5);
            CHECK(near(piece.position, target));
        }
    }

    // -----------------------------------------------------------------------
    // Smoothing
    // -----------------------------------------------------------------------

    TEST_CASE_FIXTURE(Fixture,
                      "smoothing covers exactly 1 - exp(-rate * delta) of the distance") {
        World world;
        auto &target = world.spawn({ .position = { 100, 100 } });
        world.camera.follow = target.handle();
        world.camera.smoothing = 5;
        world.camera.detail_settle(1.0f / 60); // acquires the target: a snap
        REQUIRE(near(world.camera.position, Vector2{ 100, 100 }));

        target.position = { 300, 100 };
        world.camera.detail_settle(0.1f);
        const float expected = 100 + 200 * (1 - std::exp(-5.0f * 0.1f));
        CHECK(world.camera.position.x == doctest::Approx(expected).epsilon(1e-5));
        CHECK(world.camera.position.y == doctest::Approx(100));
    }

    TEST_CASE_FIXTURE(Fixture,
                      "two half frames land exactly where one whole frame does") {
        // THE test for "delta in the exponent". A per-frame lerp factor --
        // position += (target - position) * 0.1 -- fails this, and that is the
        // bug that makes a camera follow twice as fast at 120 Hz as at 60.
        auto settle_to = [](int steps, float total) {
            World world;
            auto &target = world.spawn({ .position = { 0, 0 } });
            world.camera.follow = target.handle();
            world.camera.smoothing = 7.5f;
            world.camera.detail_settle(0.0f); // acquire
            target.position = { 640, -320 };
            for (int i = 0; i < steps; i++) {
                world.camera.detail_settle(total / static_cast<float>(steps));
            }
            return world.camera.position;
        };
        const Vector2 one = settle_to(1, 0.2f);
        const Vector2 two = settle_to(2, 0.2f);
        const Vector2 eight = settle_to(8, 0.2f);
        CHECK(two.x == doctest::Approx(one.x).epsilon(1e-4));
        CHECK(two.y == doctest::Approx(one.y).epsilon(1e-4));
        CHECK(eight.x == doctest::Approx(one.x).epsilon(1e-4));
        CHECK(eight.y == doctest::Approx(one.y).epsilon(1e-4));
        // And it actually moved, or the equality above is about nothing.
        CHECK(one.x > 100);
        CHECK(one.x < 640);
    }

    TEST_CASE_FIXTURE(Fixture, "it converges on the target and does not overshoot") {
        World world;
        auto &target = world.spawn({ .position = { 0, 0 } });
        world.camera.follow = target.handle();
        world.camera.smoothing = 4;
        world.camera.detail_settle(0.0f);
        target.position = { 500, 200 };
        float last = 0;
        for (int i = 0; i < 600; i++) {
            world.camera.detail_settle(1.0f / 60);
            // Monotonic: exponential approach never passes the target.
            CHECK(world.camera.position.x >= last - 1e-4f);
            CHECK(world.camera.position.x <= 500 + 1e-3f);
            last = world.camera.position.x;
        }
        CHECK(near(world.camera.position, Vector2{ 500, 200 }));
    }

    TEST_CASE_FIXTURE(Fixture, "a NEW target is snapped to, not glided to") {
        // A level opening with the camera drifting in from the middle of the
        // screen is the other half of the classic smoothing bug.
        World world;
        auto &a = world.spawn({ .position = { 1000, 50 } });
        auto &b = world.spawn({ .position = { -800, 300 } });
        world.camera.smoothing = 3;

        world.camera.follow = a.handle();
        world.camera.detail_settle(1.0f / 60);
        CHECK(near(world.camera.position, Vector2{ 1000, 50 }));

        SUBCASE("and switching to another target snaps too") {
            world.camera.follow = b.handle();
            world.camera.detail_settle(1.0f / 60);
            CHECK(near(world.camera.position, Vector2{ -800, 300 }));
        }
        SUBCASE("and the same target moving is smoothed, not snapped") {
            a.position = { 1200, 50 };
            world.camera.detail_settle(1.0f / 60);
            CHECK(world.camera.position.x > 1000);
            CHECK(world.camera.position.x < 1200);
        }
        SUBCASE("a target that dies and is followed again later snaps again") {
            world.camera.follow = rmp::Handle<rmp::Object>();
            world.camera.detail_settle(1.0f / 60);
            a.position = { 0, 0 };
            world.camera.follow = a.handle();
            world.camera.detail_settle(1.0f / 60);
            CHECK(near(world.camera.position, Vector2{ 0, 0 }));
        }
    }

    TEST_CASE_FIXTURE(Fixture, "limits still win over a smoothed follow") {
        World world;
        auto &target =
            world.spawn({ .position = { SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 } });
        world.camera.follow = target.handle();
        world.camera.smoothing = 2;
        world.camera.limits = { 0, 0, SCREEN_WIDTH * 2, SCREEN_HEIGHT };
        world.camera.detail_settle(0.0f);
        target.position = { -5000, SCREEN_HEIGHT / 2 };
        for (int i = 0; i < 300; i++) {
            world.camera.detail_settle(1.0f / 60);
            CHECK(world.camera.view().x >= -1e-3f); // never the void on the left
        }
    }

    TEST_CASE_FIXTURE(Fixture,
                      "a zero, negative or NaN delta moves nothing and poisons nothing") {
        World world;
        auto &target = world.spawn({ .position = { 0, 0 } });
        world.camera.follow = target.handle();
        world.camera.smoothing = 5;
        world.camera.detail_settle(0.0f);
        target.position = { 400, 0 };
        for (float bad : { 0.0f, -1.0f, -0.016f, std::nanf("") }) {
            CAPTURE(bad);
            world.camera.detail_settle(bad);
            CHECK(world.camera.position.x == doctest::Approx(0));
            CHECK_FALSE(std::isnan(world.camera.position.x));
            CHECK_FALSE(std::isnan(world.camera.position.y));
        }
    }

    // -----------------------------------------------------------------------
    // Shake
    // -----------------------------------------------------------------------

    TEST_CASE_FIXTURE(Fixture, "a shake never touches position or view()") {
        World world;
        const Vector2 before = world.camera.position;
        const Rectangle view_before = world.camera.view();
        world.camera.shake(30, 0.5f);

        bool moved = false;
        for (int i = 0; i < 20; i++) {
            world.camera.detail_settle(1.0f / 60);
            CHECK(near(world.camera.position, before));
            CHECK(world.camera.view().x == doctest::Approx(view_before.x));
            CHECK(world.camera.view().y == doctest::Approx(view_before.y));
            const Vector2 off = world.camera.shake_offset();
            if (off.x != 0 || off.y != 0) moved = true;
            // With no limits, what is DRAWN moves by exactly the offset.
            const Camera2D drawn = world.camera.raylib();
            CHECK(drawn.target.x == doctest::Approx(before.x + off.x));
            CHECK(drawn.target.y == doctest::Approx(before.y + off.y));
        }
        CHECK(moved); // or the assertions above are about a still camera
    }

    TEST_CASE_FIXTURE(Fixture,
                      "a shake at the edge of the level never draws past the limits") {
        // "Never show outside this rectangle" is a promise about what is
        // drawn, so it holds while shaking: near an edge only the inward half
        // of the shake shows. Checked through to_world() of the screen's
        // corners, which is what the player actually sees.
        World world;
        world.camera.limits = { 0, 0, SCREEN_WIDTH * 3, SCREEN_HEIGHT * 3 };
        world.camera.position = { 0, 0 }; // pinned into the top-left corner
        world.camera.detail_settle(0.0f);
        world.camera.shake(30, 0.5f);
        bool inward = false;
        for (int i = 0; i < 30; i++) {
            world.camera.detail_settle(1.0f / 60);
            for (const Vector2 corner :
                 { Vector2{ 0, 0 }, Vector2{ SCREEN_WIDTH, SCREEN_HEIGHT } }) {
                const Vector2 seen = world.camera.to_world(corner);
                CHECK(seen.x >= -1e-3f);
                CHECK(seen.y >= -1e-3f);
                CHECK(seen.x <= SCREEN_WIDTH * 3 + 1e-3f);
                CHECK(seen.y <= SCREEN_HEIGHT * 3 + 1e-3f);
            }
            const Camera2D drawn = world.camera.raylib();
            if (drawn.target.x > world.camera.position.x + 0.5f) inward = true;
        }
        CHECK(inward); // the shake still shows, inwards

        SUBCASE("a view pinned by a limit exactly its size does not shake at all") {
            World pinned;
            pinned.camera.limits = { 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT };
            pinned.camera.shake(30, 0.5f);
            for (int i = 0; i < 10; i++) {
                pinned.camera.detail_settle(1.0f / 60);
                CHECK(pinned.camera.raylib().target.x ==
                      doctest::Approx(SCREEN_WIDTH / 2));
                CHECK(pinned.camera.raylib().target.y ==
                      doctest::Approx(SCREEN_HEIGHT / 2));
            }
        }
    }

    TEST_CASE_FIXTURE(Fixture,
                      "it stays within its strength and ends at exactly nothing") {
        World world;
        world.camera.shake(12, 0.25f);
        float peak = 0;
        // 0.25 s of 1/100 s frames is 25 steps; run past the end.
        for (int i = 0; i < 40; i++) {
            world.camera.detail_settle(0.01f);
            const Vector2 off = world.camera.shake_offset();
            const float len = std::sqrt(off.x * off.x + off.y * off.y);
            peak = len > peak ? len : peak;
            CHECK(len <= 12 * 1.0001f + 1e-4f);
        }
        CHECK(peak > 0.5f);
        // Exactly zero, not an epsilon: the camera must be exactly where it was.
        CHECK(world.camera.shake_offset().x == 0.0f);
        CHECK(world.camera.shake_offset().y == 0.0f);
        CHECK(world.camera.raylib().target.x == world.camera.position.x);
    }

    TEST_CASE_FIXTURE(Fixture,
                      "a burst of hits keeps the stronger shake instead of adding up") {
        World world;
        for (int i = 0; i < 50; i++) world.camera.shake(10, 0.5f);
        for (int i = 0; i < 30; i++) {
            world.camera.detail_settle(1.0f / 60);
            const Vector2 off = world.camera.shake_offset();
            CHECK(std::sqrt(off.x * off.x + off.y * off.y) <= 10 * 1.0001f + 1e-4f);
        }

        SUBCASE("a weak shake arriving late does not cut a strong one short") {
            World w;
            w.camera.shake(40, 1.0f);
            w.camera.detail_settle(0.1f);
            w.camera.shake(1, 0.05f);
            for (int i = 0; i < 10; i++) w.camera.detail_settle(0.01f);
            // 0.2 s into a 1 s shake of 40: still well above what 1 could do.
            float biggest = 0;
            for (int i = 0; i < 20; i++) {
                w.camera.detail_settle(0.01f);
                const Vector2 off = w.camera.shake_offset();
                const float len = std::sqrt(off.x * off.x + off.y * off.y);
                biggest = len > biggest ? len : biggest;
            }
            CHECK(biggest > 2);
        }
    }

    TEST_CASE_FIXTURE(Fixture,
                      "a hit on the tail of a big shake is measured against the tail") {
        // At t = 0.5 s a shake of 40 over 1 s is moving the screen by
        // 40 * 0.5^2 = 10. A new hit of 15 is stronger than THAT, and must be
        // felt -- compared against the start strength (or a linear fade, 20)
        // it was thrown away.
        World world;
        world.camera.shake(40, 1.0f);
        for (int i = 0; i < 50; i++) world.camera.detail_settle(0.01f);
        world.camera.shake(15, 0.3f);
        float peak = 0;
        for (int i = 0; i < 10; i++) {
            world.camera.detail_settle(0.01f);
            const Vector2 off = world.camera.shake_offset();
            peak = std::max(peak, std::sqrt(off.x * off.x + off.y * off.y));
        }
        CHECK(peak > 10.5f);
    }

    TEST_CASE_FIXTURE(Fixture, "a short strong hit does not cut a long shake short") {
        World world;
        world.camera.shake(10, 2.0f);
        world.camera.detail_settle(0.1f);
        world.camera.shake(10.5f, 0.05f); // a hair stronger, a blink long
        for (int i = 0; i < 50; i++) world.camera.detail_settle(0.01f); // 0.5 s later
        float seen = 0;
        for (int i = 0; i < 10; i++) {
            world.camera.detail_settle(0.01f);
            const Vector2 off = world.camera.shake_offset();
            seen = std::max(seen, std::sqrt(off.x * off.x + off.y * off.y));
        }
        CHECK(seen > 1.0f); // still shaking, as the 2 s shake would be
    }

    TEST_CASE_FIXTURE(Fixture,
                      "infinity poisons nothing: smoothing snaps, shakes are ignored") {
        World world;
        auto &target = world.spawn({ .position = { 0, 0 } });
        world.camera.follow = target.handle();
        world.camera.detail_settle(0.0f);
        world.camera.smoothing = std::numeric_limits<float>::infinity();
        target.position = { 300, 100 };
        world.camera.detail_settle(0.0f); // -inf * 0 would be NaN
        CHECK_FALSE(std::isnan(world.camera.position.x));
        CHECK(near(world.camera.position, Vector2{ 300, 100 }));
        world.camera.shake(std::numeric_limits<float>::infinity(), 0.5f);
        world.camera.shake(5, std::numeric_limits<float>::infinity());
        world.camera.detail_settle(1.0f / 60);
        CHECK(world.camera.shake_offset().x == 0.0f);
        const Vector2 seen = world.camera.to_world(Vector2{ 10, 10 });
        CHECK_FALSE(std::isnan(seen.x));
        CHECK_FALSE(std::isnan(seen.y));
    }

    TEST_CASE_FIXTURE(Fixture, "nonsense shakes are ignored rather than stored") {
        World world;
        for (float bad : { 0.0f, -5.0f, std::nanf("") }) {
            world.camera.shake(bad, 0.5f);
            world.camera.shake(5, bad);
        }
        world.camera.detail_settle(1.0f / 60);
        CHECK(world.camera.shake_offset().x == 0.0f);
        CHECK(world.camera.shake_offset().y == 0.0f);
    }

    TEST_CASE_FIXTURE(Fixture,
                      "the same frames give the same shake: it is reproducible") {
        auto run = [] {
            World world;
            world.camera.shake(20, 0.4f);
            std::vector<Vector2> offsets;
            for (int i = 0; i < 24; i++) {
                world.camera.detail_settle(1.0f / 60);
                offsets.push_back(world.camera.shake_offset());
            }
            return offsets;
        };
        const auto a = run();
        const auto b = run();
        REQUIRE(a.size() == b.size());
        for (std::size_t i = 0; i < a.size(); i++) {
            CHECK(a[i].x == b[i].x);
            CHECK(a[i].y == b[i].y);
        }
    }

    TEST_CASE_FIXTURE(Fixture, "a click during a shake lands on what is drawn under it") {
        // to_world converts through the SHAKEN camera, because that is what
        // drew the frame the player is aiming at.
        World world;
        world.camera.shake(25, 0.5f);
        for (int i = 0; i < 5; i++) world.camera.detail_settle(1.0f / 60);
        const Vector2 off = world.camera.shake_offset();
        REQUIRE((off.x != 0 || off.y != 0));

        const Vector2 world_point{ 300, 200 };
        const Vector2 on_screen = world.camera.to_screen(world_point);
        CHECK(near(world.camera.to_world(on_screen), world_point)); // round trip
        // And it is not where the unshaken camera would have drawn it.
        CHECK(on_screen.x == doctest::Approx(world_point.x - off.x).epsilon(0.001));
        CHECK(on_screen.y == doctest::Approx(world_point.y - off.y).epsilon(0.001));
    }
}
