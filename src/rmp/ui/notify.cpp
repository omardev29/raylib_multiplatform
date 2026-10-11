// ===========================================================================
// notify(): a message that goes away by itself -- and, in a debug build, the
// framework's own warnings on screen.
//
// notify() only queues. What is queued is drawn by draw_notices(), which the
// app calls after every scene has drawn (../ui_internal.h); a game with no
// scene stack has it drawn at the end of a pass (pass_ended(), internal.h).
// The report sink a debug build installs is show_report().
//
// As it stands nothing is queued and nothing is drawn.
// ===========================================================================

#include "floating.h"
#include "internal.h"

namespace rmp::ui {

void notify(std::string_view text) { notify(text, NotifyOptions{}); }

void notify(std::string_view /*text*/, const NotifyOptions & /*o*/) {}

namespace detail {

void draw_notices() {}
void show_report(std::string_view /*line*/) {}
void pass_ended() {}

} // namespace detail

} // namespace rmp::ui
