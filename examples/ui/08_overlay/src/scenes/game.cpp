#include "game.h"

#include "pause.h"

#include <rmp/app.h>
#include <rmp/object.h>
#include <rmp/ui.h>

#include <array>

namespace {

// Where each ball starts, how fast it goes, and how it looks.
struct Ball {
    Vector2 position;
    Vector2 velocity;
    float radius;
    Color color;
};

constexpr std::array BALLS{
    Ball{ { 120, 110 }, { 120, 80 }, 34, RED },
    Ball{ { 300, 330 }, { -150, 95 }, 26, ORANGE },
    Ball{ { 420, 160 }, { 100, -120 }, 40, GOLD },
    Ball{ { 560, 300 }, { -90, -70 }, 30, LIME },
    Ball{ { 680, 120 }, { 140, 110 }, 24, SKYBLUE },
    Ball{ { 220, 220 }, { -110, -130 }, 36, VIOLET },
};

} // namespace

void GameScene::_ready() {
    // Something to be in front of: balls bouncing off the edges of the
    // window. A veil over an empty screen shows nothing, and a blur less.
    for (const Ball &b : BALLS) {
        rmp::Shape shape = rmp::circle(b.radius);
        shape.color = b.color;
        spawn({ .position = b.position,
                .shape = shape,
                .velocity = b.velocity,
                .edges = rmp::Edge::BOUNCE });
    }
}

void GameScene::_draw() {
    // The HUD. This scene reads no keys, so it is no game being PLAYED, and
    // its first button has the focus, as a menu's would: Enter pauses.
    rmp::ui::begin({ .placement = rmp::ui::Align::TOP_RIGHT });
    if (rmp::ui::button("Pause")) rmp::Scene::push<PauseScene>();
#if !defined(PLATFORM_IOS)
    // Inert on iPhone: Apple rejects apps that terminate themselves.
    if (rmp::ui::button("Quit")) _asking = true;
#endif
    rmp::ui::end();

    // The dialog. After end(), never inside it: an overlay is a frame of its
    // own. The panel is the box; the overlay is the veil behind it, the
    // clicks it takes from everything under it, and the focus on its first
    // button -- Stay, so that Enter is the answer that loses nothing.
    if (_asking) {
        rmp::ui::overlay({ .dim = 0.6f }, [&] {
            rmp::ui::panel([&] {
                rmp::ui::text("Really quit?");
                rmp::ui::text("The balls will stop for good.",
                              { .color = rmp::ui::ColorRole::MUTED });
                rmp::ui::row([&] {
                    if (rmp::ui::button("Stay")) _asking = false;
                    if (rmp::ui::button("Quit", { .style = rmp::ui::Variant::DANGER }))
                        rmp::app::quit();
                });
            });
        });
    }
}
