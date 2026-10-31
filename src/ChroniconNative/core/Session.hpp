// Driving the game from code: a tick that also runs in the main menu, loading a character without input,
// and small instance tools (instances, ivar) for finding things out in the running game.
#pragma once

#include "Core.hpp"

namespace cp::session {

void Install();  // hooks obj_menu's Step event (the main menu has no GUIdraw)

// Command channel and pending loads; throttled. FromDraw: called from the Draw GUI hook. The menu Step hook
// only acts while no GUI frame has been drawn lately, i.e. in the main menu.
void Tick(bool FromDraw);

// Starts loading a character from the main menu: a save name ("elona"), or "recent" for the newest save.
std::string RequestLoad(const std::string& Name);

bool HandlesCommand(const std::string& Verb);
std::string Command(const std::vector<std::string>& Words);

}  // namespace cp::session
