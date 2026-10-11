// ===========================================================================
// tooltip(): a line about the control declared just before it.
//
// Which control that is, is take_last_control() (internal.h): every control
// notes itself when it is declared.
//
// As it stands a tooltip shows nothing.
// ===========================================================================

#include "floating.h"
#include "internal.h"

namespace rmp::ui {

void tooltip(std::string_view text) { tooltip(text, TooltipOptions{}); }

void tooltip(std::string_view /*text*/, const TooltipOptions & /*o*/) {}

} // namespace rmp::ui
