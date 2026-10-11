// ===========================================================================
// rmp::Scene — see include/rmp/scene.h for what a scene is, and
// src/rmp/scene_internal.h for the half the app calls.
//
// The stack is a vector of owning pointers and the transitions are a queue.
// That is the whole data structure, and it is deliberately that small: every
// interesting property of this file is an ORDERING property, not an
// algorithmic one, which is why tests/scene_test.cpp checks a transcript of
// events rather than a return value.
// ===========================================================================

#include <rmp/scene.h>

#include "object_internal.h"
#include "internal.h"
#include "scene_internal.h"
#include "ui_internal.h"

#include <rmp/app.h>
#include <rmp/input.h>
#include <rmp/ui.h>

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rmp {

namespace {

enum class Op { CHANGE, PUSH, REPLACE, POP };

struct Pending {
    Op op;
    std::unique_ptr<Scene> next; // null for POP
    int type = 0; // detail::type_id<T>(); 0 for POP
};

// The scene stack. The dot at every use says "file state" (not `scenes`,
// which would hide the namespace).
struct {
    // Index 0 is the bottom, back() is what the player is looking at.
    std::vector<std::unique_ptr<Scene>> items;
    // Everything change/push/replace/pop recorded this frame, in the order it
    // was asked for. More than one in a frame is unusual but well defined: they
    // are applied in that order, so `push(A); pop();` leaves the stack as it was.
    std::vector<Pending> pending;
    bool running = false;
    // Whether each scene read the keys the interface navigates with in this
    // frame's update, by index: what its passes are told in the draw.
    std::vector<bool> playing;
} scene_stack;

// Owning from the template in rmp/scene.h all the way down: nothing between
// make_unique and the stack ever holds a raw pointer.
void record(Op op, std::unique_ptr<Scene> next, int type = 0) {
    // The same scene type asked for twice in one frame is asked for once. Two
    // end conditions that fire on the same frame -- the last alien dies on the
    // frame the last hit point goes -- each push<GameOver>(), and the stack
    // ended up two overlays deep. Nobody means that; the second is dropped and
    // said once.
    if (type != 0) {
        for (const Pending &p : scene_stack.pending) {
            if (p.op == op && p.type == type) {
                RMP_REPORT_ONCE("SCENE: the same scene was asked for twice in one frame; "
                                "the second change/push/replace is ignored.");
                return;
            }
        }
    }
    scene_stack.pending.push_back(Pending{ op, std::move(next), type });
}

} // namespace

// ---------------------------------------------------------------------------
// The public navigation surface. Every one of these RECORDS and returns; see
// the comment in rmp/scene.h for why doing the work here would be a bug.
// ---------------------------------------------------------------------------

void Scene::detail_change(std::unique_ptr<Scene> next, int type) {
    record(Op::CHANGE, std::move(next), type);
}
void Scene::detail_push(std::unique_ptr<Scene> next, int type) {
    record(Op::PUSH, std::move(next), type);
}
void Scene::detail_replace(std::unique_ptr<Scene> next, int type) {
    record(Op::REPLACE, std::move(next), type);
}
void Scene::pop() { record(Op::POP, nullptr); }

int Scene::depth() { return static_cast<int>(scene_stack.items.size()); }

// current() is a reference because there is always a scene while the app runs,
// and returning a pointer would invite a null check that can never fire. The
// one case where the stack really is empty is a programming error — calling it
// before RMP_GAME has started or after shutdown — and it gets a scene that does
// nothing rather than a crash in someone's release build.
Scene &Scene::current() {
    static Scene fallback;
    if (scene_stack.items.empty()) {
        RMP_REPORT_ONCE("SCENE: current() with an empty stack — is this before "
                        "RMP_GAME started, or after the app closed?");
        return fallback;
    }
    return *scene_stack.items.back();
}

} // namespace rmp

namespace rmp::scenes::detail {

namespace {

// The lowest scene that still takes part in a pass, found by walking down from
// the top for as long as each scene lets the one below it through. Returns an
// index into scene_stack.items; the stack is never empty when this is called.
template <class Policy> int lowest_participant(Policy lets_through) {
    int i = static_cast<int>(scene_stack.items.size()) - 1;
    while (i > 0 && lets_through(*scene_stack.items[static_cast<size_t>(i)])) --i;
    return i;
}

// Can input reach the scene at `index`? Only if every scene above it lets it
// through. Walking upwards rather than remembering a flag means a stack three
// deep answers correctly without anything having to be kept in step.
bool reachable_by_input(int index) {
    const int top = static_cast<int>(scene_stack.items.size()) - 1;
    for (int above = index + 1; above <= top; ++above) {
        if (!scene_stack.items[static_cast<size_t>(above)]->input_below) return false;
    }
    return true;
}

void end_top() {
    scene_stack.items.back()->_end();
    // The scene's objects go with it, each getting its _end(), and they go
    // AFTER the scene's own _end() so that a scene tidying up can still walk
    // what it spawned. This is the same ordering argument as rmp/app.h's
    // teardown list, and for the same reason: an object may hold an
    // rmp::Texture, and its destructor has to find its slot still there.
    rmp::objects::detail::release_scene(*scene_stack.items.back());
    scene_stack.items.pop_back();
}

void enter(std::unique_ptr<Scene> next) {
    scene_stack.items.push_back(std::move(next));
    scene_stack.items.back()->_ready();
}

} // namespace

void start(std::unique_ptr<Scene> first) {
    // The null check BEFORE scene_stack.running, because the other order leaves the app
    // running over an empty stack: a frame loop that draws nothing, forever,
    // and a current() that hands back the fallback scene to everything that
    // asks. Refusing leaves whatever was already on the stack exactly as it
    // was, which is the only other thing this could mean.
    if (first == nullptr) {
        RMP_REPORT_ONCE("SCENE: start() was given a null scene; the app is not "
                        "started. RMP_GAME(T) builds one for you -- this is a "
                        "hand-written start() or a T that failed to construct.");
        return;
    }
    scene_stack.running = true;
    enter(std::move(first));
}

bool running() { return scene_stack.running; }

void update(float delta) {
    if (scene_stack.items.empty()) return;
    const int lowest = lowest_participant([](const Scene &s) { return s.updates_below; });
    const int top = static_cast<int>(scene_stack.items.size()) - 1;
    // A scene that is not updated reads nothing, so it is not being played.
    scene_stack.playing.assign(scene_stack.items.size(), false);
    // Nothing is drawn while the world moves: an interface described here is
    // a mistake rmp::ui reports.
    rmp::ui::detail::set_phase(rmp::ui::detail::Phase::UPDATE);
    // Bottom upwards: the world moves before whatever is layered on top of it
    // reacts to where the world ended up.
    for (int i = lowest; i <= top; ++i) {
        // The other half of Scene::input_below, and the half that matters for
        // gameplay: a scene that is still running under a HUD that took the
        // input reads every action as false, so `if (just_pressed("fire"))`
        // simply does not fire. Nothing in that scene says so.
        rmp::input::detail::set_layer_input(reachable_by_input(i));
        Scene &scene = *scene_stack.items[static_cast<size_t>(i)];
        // What was read before this scene's turn is not this scene's.
        rmp::ui::detail::take_game_keys();
        // The pointer first, so a press is acted on in the frame it happened
        // and an on_click can spawn or destroy before anything else looks.
        rmp::objects::detail::pointer(scene);
        scene._update(delta);
        // The scene first, then its objects: the scene sets up the frame and
        // the objects move inside it. Documented in
        // next_architecture/03-app-and-scenes.md and asserted in
        // tests/scene_test.cpp, because every interesting property of this is
        // an ordering property.
        rmp::objects::detail::update(scene, delta);
        // Last, because it has to see where everything ended up. Separating two
        // objects before one of them has moved is separating them from where
        // they were.
        rmp::objects::detail::collide(scene);
        // The camera last of all: it follows where things ENDED UP, after the
        // collision pass has had its say, and its limits are applied after
        // its follow. Anything reading camera.view() next frame -- the
        // default bounds, the pointer, Parallax -- sees the settled frame.
        scene.camera.detail_settle(delta);
        // Did it, or its objects, read the keys the interface navigates
        // with? Then it is being played, and its HUD waits for Tab.
        scene_stack.playing[static_cast<size_t>(i)] = rmp::ui::detail::take_game_keys();
    }
    rmp::ui::detail::set_phase(rmp::ui::detail::Phase::FREE);
    rmp::input::detail::set_layer_input(true);
}

Color clear_color() {
    if (scene_stack.items.empty()) return rmp::ui::current_theme().background;
    const int lowest = lowest_participant([](const Scene &s) { return s.draws_below; });
    const Color c = scene_stack.items[static_cast<size_t>(lowest)]->background;
    // BLANK is the "I did not choose" value rather than a real colour, because
    // a fully transparent clear is never what a scene means and every other
    // colour is.
    const bool unset = c.r == 0 && c.g == 0 && c.b == 0 && c.a == 0;
    return unset ? rmp::ui::current_theme().background : c;
}

void draw() {
    if (scene_stack.items.empty()) return;
    const int lowest = lowest_participant([](const Scene &s) { return s.draws_below; });
    const int top = static_cast<int>(scene_stack.items.size()) - 1;

    for (int i = lowest; i <= top; ++i) {
        // Input belongs to the top scene unless it says otherwise, and this is
        // where that becomes true rather than a comment: a UI pass with the
        // pointer suppressed lays out and draws exactly as it would, and no
        // widget in it can be hovered or clicked. A HUD under an open pause
        // menu is drawn, is not interactive, and neither scene wrote a line
        // about it.
        rmp::ui::detail::set_pass_input(reachable_by_input(i));
        // Over a game being played, or not: what this scene read in this
        // frame's update. A draw with no update before it (a test) read
        // nothing.
        const auto slot = static_cast<size_t>(i);
        rmp::ui::detail::set_pass_playing(slot < scene_stack.playing.size() &&
                                          scene_stack.playing[slot]);
        Scene &scene = *scene_stack.items[slot];
        // Objects first, in world space, and the scene's own _draw() after, in
        // screen space: that is what puts the HUD over the game rather than
        // under it, without either one saying so.
        // The map first, underneath everything: it is the level, and the
        // objects stand on it.
        // The world under the camera; the scene's own _draw() outside it, in
        // screen space, so a HUD or a pause menu stays put while the view
        // moves. A camera nobody touched is the identity at the design
        // resolution, which is why a one-screen game never notices this.
        // Guarded on the window because tests/scene_test.cpp drives this with
        // none: BeginMode2D pushes a matrix into rlgl's batch, and without
        // InitWindow() there is no batch to push it into -- a segfault, not a
        // no-op. In a running game the window is always there.
        const bool window = IsWindowReady();
        const Camera2D camera = scene.camera.raylib();
        // LETTERBOX: the world is cut to the game's area, so the bars keep
        // the clear colour whatever stands past the edge of the design. Not
        // the scene's _draw() below: the interface lays itself out inside the
        // area on its own, and a game's own overlay may mean the whole window.
        // Cut on the same screen the camera fitted the world into.
        const Rectangle screen = rmp::objects::detail::view_rect();
        const std::optional<WorldClip> clip = window
            ? world_clip(rmp::app::stretch(), Vector2{ screen.width, screen.height })
            : std::nullopt;
        // Under the camera nothing of the interface can be drawn: what an
        // object's _draw() describes is reported (rmp::ui::at() is the way).
        rmp::ui::detail::set_phase(rmp::ui::detail::Phase::WORLD);
        if (clip) BeginScissorMode(clip->x, clip->y, clip->width, clip->height);
        if (window) BeginMode2D(camera);
        scene.map.draw();
        rmp::objects::detail::draw(scene);
        if (window) EndMode2D();
        if (clip) EndScissorMode();
        // The scene's own _draw(), in screen space: where the interface is
        // described, and at() projects through the camera that drew the world.
        rmp::ui::detail::set_pass_camera(camera);
        rmp::ui::detail::set_phase(rmp::ui::detail::Phase::DRAW);
        scene._draw();
        rmp::ui::detail::after_scene_draw(i, top);
    }
    // Outside the stack: no camera, no game being played, the input the app's.
    rmp::ui::detail::set_phase(rmp::ui::detail::Phase::FREE);
    rmp::ui::detail::set_pass_camera(Camera2D{
        .offset = Vector2{ 0, 0 }, .target = Vector2{ 0, 0 }, .rotation = 0, .zoom = 1 });
    rmp::ui::detail::set_pass_playing(false);
    rmp::ui::detail::set_pass_input(true);
}

std::optional<WorldClip> world_clip(const rmp::app::Stretch mode, const Vector2 screen) {
    if (mode != rmp::app::Stretch::LETTERBOX) return std::nullopt;
    // A screen with no area, or not a number, is the design at 1:1 to
    // world_viewport(): there is nothing to cut. Said here because the
    // comparisons below are all false against a NaN.
    if (!(screen.x > 0) || !(screen.y > 0) || !std::isfinite(screen.x) ||
        !std::isfinite(screen.y)) {
        return std::nullopt;
    }
    // A scissor is whole pixels in ints. A screen under a pixel has none to
    // cut, and past 2^24 a float stops counting every pixel and the edges
    // below leave what an int can say: no machine makes such a window, only
    // a test hands one in, and the world is left uncut.
    constexpr auto MOST_PIXELS = static_cast<float>(1 << 24);
    if (!(screen.x >= 1) || !(screen.y >= 1) || screen.x > MOST_PIXELS ||
        screen.y > MOST_PIXELS) {
        return std::nullopt;
    }
    const Rectangle area = rmp::app::detail::world_viewport(mode, screen).area;
    // Each edge rounded on its own, so the clip is the area to the nearest
    // pixel on every side: no half pixel of the world on a bar, none cut off
    // the game. lround and not a cast: truncating moves both edges the same
    // way, and one bar ends up a pixel wider than the other.
    const auto left = static_cast<int>(std::lround(area.x));
    const auto top = static_cast<int>(std::lround(area.y));
    const auto right = static_cast<int>(std::lround(area.x + area.width));
    const auto bottom = static_cast<int>(std::lround(area.y + area.height));
    // No bar wide enough to be a pixel: the world covers the window, and the
    // draw stays exactly what it is without a stretch.
    if (left <= 0 && top <= 0 && static_cast<float>(right) >= screen.x &&
        static_cast<float>(bottom) >= screen.y) {
        return std::nullopt;
    }
    return WorldClip{ left, top, right - left, bottom - top };
}

std::optional<WindowSize> parse_test_window(const std::string_view text) {
    // Digits and nothing else, read by hand: "12abc", " 12", "+12", "1e3" and
    // "-12" are each nothing here, not the 12 or 1 or wrap-around that atoi
    // and friends make of them. Five digits at most, which is past 16384 and
    // far short of what an int holds.
    const auto side = [](const std::string_view digits) -> std::optional<int> {
        if (digits.empty() || digits.size() > 5) return std::nullopt;
        int value = 0;
        for (const char c : digits) {
            if (c < '0' || c > '9') return std::nullopt;
            value = value * 10 + (c - '0');
        }
        if (value < 16 || value > 16384) return std::nullopt;
        return value;
    };
    const size_t cross = text.find('x');
    if (cross == std::string_view::npos) return std::nullopt;
    const std::optional<int> width = side(text.substr(0, cross));
    const std::optional<int> height = side(text.substr(cross + 1));
    if (!width || !height) return std::nullopt;
    return WindowSize{ *width, *height };
}

TestWindow read_test_window(const std::string_view asked) {
    if (asked.empty()) return TestWindow{ std::nullopt, false };
    const std::optional<WindowSize> size = parse_test_window(asked);
    if (!size) {
        // A mistake in the CI step that set it, said in the log next to the
        // marker every boot gate greps for. Not RMP_REPORT_ONCE: it is not a
        // mistake in the game, and the boot failing is the verdict.
        const std::string shown(asked);
        TraceLog(LOG_ERROR,
                 "RAY_TEST_BOOT_FAIL RAY_TEST_WINDOW=\"%s\" is not WIDTHxHEIGHT, two "
                 "whole numbers 16..16384 joined by an x -- 1080x2400, say",
                 shown.c_str());
        return TestWindow{ std::nullopt, true };
    }
    TraceLog(LOG_INFO, "RAY_TEST_WINDOW %dx%d", size->width, size->height);
    return TestWindow{ size, false };
}

void apply_pending() {
    // Objects first, and unconditionally: a frame with no scene transition
    // still has bullets to bury, and an early return on scene_stack.pending would have
    // leaked every one of them. It is the sort of bug that only shows up as a
    // number going up.
    rmp::objects::detail::collect();

    if (scene_stack.pending.empty()) return;

    // Take the queue rather than iterating it: _ready() and _end() are the
    // user's code and are entitled to queue transitions of their own, and those
    // belong to the NEXT frame. Without the swap, one of them pushing here
    // would reallocate the vector being walked.
    std::vector<Pending> batch;
    batch.swap(scene_stack.pending);

    for (Pending &p : batch) {
        switch (p.op) {
            case Op::CHANGE:
                // Top down, so a scene is never ended while something it put on the
                // stack is still above it.
                while (!scene_stack.items.empty()) end_top();
                enter(std::move(p.next));
                break;

            case Op::PUSH:
                if (!scene_stack.items.empty()) scene_stack.items.back()->_suspend();
                enter(std::move(p.next));
                break;

            case Op::REPLACE:
                // Only the top changes. What is underneath was suspended when the
                // top went on and stays suspended, so it gets no _resume().
                if (!scene_stack.items.empty()) end_top();
                enter(std::move(p.next));
                break;

            case Op::POP:
                if (scene_stack.items.size() <= 1) {
                    // Refusing beats obeying. An empty stack is a black window on
                    // desktop and, on iOS, a screen with no way back — and quit()
                    // is right there and says what it means.
                    RMP_REPORT_ONCE("SCENE: pop() with %d scene(s) on the stack would "
                                    "leave nothing to draw; ignoring. Did you mean "
                                    "rmp::app::quit()?",
                                    static_cast<int>(scene_stack.items.size()));
                    break;
                }
                end_top();
                scene_stack.items.back()->_resume();
                break;
        }
    }
}

void shutdown() {
    // Anything still queued is dropped: the app is going away, and running a
    // _ready() for a scene nobody will ever see would only give it a chance to
    // load assets that are about to be released.
    scene_stack.pending.clear();
    while (!scene_stack.items.empty()) end_top();
    // And again: an _end() above is the game's code, and may have asked for a
    // scene on its way out. Kept, that scene outlived the window and the
    // resource table -- destroyed at exit, with whatever its constructor
    // loaded -- and a stack started again ran it first.
    scene_stack.pending.clear();
    scene_stack.playing.clear();
    scene_stack.running = false;
}

} // namespace rmp::scenes::detail
