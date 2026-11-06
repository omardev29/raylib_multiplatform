// ===========================================================================
// rmp::behavior::Talker -- see rmp/behavior.h. A conversation an object
// starts when the player comes close and asks; the showing is the game's.
// ===========================================================================

#include <rmp/behavior.h>

#include <rmp/assets.h>
#include <rmp/dialogue.h>
#include <rmp/input.h>
#include <rmp/tilemap.h>

#include <cmath>
#include <string>

namespace rmp::behavior {

Talker Talker::from(const MapObject &object) {
    Talker made;
    made.dialogue = std::string(object.property_string("dialogue", made.dialogue));
    made.node = std::string(object.property_string("node", made.node));
    made.range = object.property_float("range", made.range);
    made.once = object.property_bool("once", made.once);
    return made;
}

void Talker::_update(Object &self, float delta) {
    (void)delta;
    if (ours.talking) {
        if (ours.talk.running()) return;
        ours.talking = false;
        on_done();
    }
    const auto who = player.get();
    if (!who) {
        ours.near = false;
        return;
    }
    const float dx = who->position.x - self.position.x;
    const float dy = who->position.y - self.position.y;
    ours.near = std::isfinite(range) && dx * dx + dy * dy <= range * range;
    const bool arrived = ours.near && !ours.was_near;
    ours.was_near = ours.near;
    if (!ours.near || (once && ours.talked) || dialogue.empty()) return;
    const bool asked = action.empty() ? arrived : rmp::input::just_pressed(action);
    if (!asked) return;
    if (!ours.loaded) {
        ours.talk = rmp::assets::load_dialogue(dialogue);
        ours.loaded = true;
    }
    if (!ours.talk.start(node)) return;
    ours.talking = true;
    ours.talked = true;
    on_talk(ours.talk);
}

} // namespace rmp::behavior
