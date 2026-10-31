// In-game menu drawn in the Draw GUI event: F8 opens it, one tab per settings category plus the stat editor.
#pragma once

#include <string>
#include <vector>

namespace cp::menu
{
    struct DamageHit { double SecondsAgo; double Amount; std::string Source; double HpAfter; };
    void DrawDps(double Dps, double Total);
    void NotifyDeath(const std::vector<DamageHit>& Hits);
    void Frame(); // input and drawing, once per GUIdraw
    bool Visible(); // the menu or the death recap is on screen
    std::string Command(const std::vector<std::string>& Words); // "menu [tab name or number | off]": open a tab from code
}
