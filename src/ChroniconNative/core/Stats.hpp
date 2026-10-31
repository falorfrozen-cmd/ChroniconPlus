// Stat editor: bonuses on top of the game's own stat calculation.
//
// Chronicon recomputes every stat in gml_Script_gearRecalculate (base stats, gear, passives, buffs, caps).
// Bonuses are applied right after it, so they survive gear and buff changes and disappear when reset.
// Two kinds of rows:
//   - core values: player instance variables the game derives during recalculation (maxhp, dmg, ...);
//   - playerstats: the ~150 entries read with gml_Script_statGet / written with statSet (stored obfuscated,
//     so they are only touched through the game's own scripts). Names come from global.stattext.
// Entries the recalculation never sets (kill counters, deaths, play time) are counters: an edit writes them
// once instead of becoming a bonus, so nothing accumulates.
#pragma once

#include "Core.hpp"
#include <optional>

namespace cp::stats
{
    struct Bonus
    {
        double Add = 0;
        double Mul = 1;
        std::optional<double> Set; // overrides add/mul
        std::optional<double> Cap; // replaces the game's cap (statGetCap) for playerstats rows

        bool Empty() const { return Add == 0 && Mul == 1 && !Set && !Cap; }
    };

    enum class RowKind { Core, Computed, Counter, Unknown };

    struct RowView
    {
        std::string Alias;
        std::string Name;
        RowKind Kind;
        std::optional<double> Current;
        std::optional<double> GameCap;
        Bonus Bonus;
    };

    void Install(); // hooks; call once from ModuleInitialize
    bool Ready();   // the player and the stat tables are available

    size_t RowCount();
    std::optional<RowView> Row(size_t Index);
    // UI discovery without querying 208 current values/caps every frame.
    std::optional<RowView> Metadata(size_t Index);
    std::string BonusText(const RowView& View);

    // Menu edits by row index; return a short status line.
    std::string AdjustAdd(size_t Index, double Delta);
    std::string ToggleCap(size_t Index);
    std::string Reset(size_t Index);

    // Commands: stats, stat, statreset, recalc.
    // A flat addition another feature owns (kept apart from the user's own bonus); 0 removes it.
    void SetFeatureAdd(const std::string& Alias, double Add);

    bool HandlesCommand(const std::string& Verb);
    std::string Command(const std::vector<std::string>& Words);
}
