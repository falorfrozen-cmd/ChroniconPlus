// ChroniconPlus core: logging, the settings catalog, GML helpers on top of runner.hpp, hooks and the command channel.
#pragma once

#include "runner.hpp"

#include <windows.h>

#include <filesystem>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cp {

namespace fs = std::filesystem;
using gm::CInstance;
using gm::Value;

// ---------------------------------------------------------------- logging

void LogInit(const fs::path& Path);
void Log(const std::string& Line);

// ---------------------------------------------------------------- settings catalog

// Multiplier: steps through Steps (x1 = off). Integer: +-1. Toggle: on/off. Action: runs once when chosen.
enum class SettingKind { Multiplier, Integer, Toggle, Action };

// One user-facing setting: its command key, menu tab, label and one-line description, INI location, bounds and
// neutral default. A setting at its default value must cost nothing (hooks check it before doing any work).
struct Setting {
    const char* Key;
    const char* Category;
    const char* Label;
    const char* Description;
    const char* Section;
    const char* IniKey;
    SettingKind Kind;
    double Minimum, Maximum, Default;
    std::span<const double> Steps;  // menu steps for Multiplier settings
    double Value;
    std::string (*Run)() = nullptr;  // Action settings
};

// In menu order; Core.cpp's table follows the same order.
enum SettingId {
    // Rewards
    S_EXP, S_CURRENCY, S_LOOT, S_RARITY_ROLLS, S_PERFECT_ROLLS, S_ENDGAME,
    // Combat
    S_DMG_DEALT, S_DMG_TAKEN, S_COOLDOWN_MULT, S_COST_MULT, S_GAME_SPEED,
    // Cheats
    S_GODMODE, S_MANA, S_NOCOOLDOWN, S_FREE_SHOP,
    // World
    S_DENSITY, S_CHAMPIONS, S_MAP_REVEAL, S_ZOOM, S_NO_TRAPS, S_LOOT_RADIUS,
    // Interface
    S_DPS_METER, S_DEATH_RECAP,
    // Progression
    S_TALENTS, S_UNLOCK_DYES, S_UNLOCK_GAMBLER, S_UNLOCK_WAYPOINTS,
    S_COUNT
};
// Writes every tab and setting (labels, descriptions, kinds, bounds, steps, current values) as JSON.
void WriteCatalog(const fs::path& Path);
void SetAction(SettingId Id, std::string (*Run)());

struct Advanced {
    int MenuKey = 119;  // F8
    int ExpArgIndex = 1;
    int CurrencyArgIndex = 1;
    int LogArgCalls = 0;  // log the first N calls of each hooked script
};

std::span<Setting> Settings();
Setting* FindSetting(std::string_view Key);
double SettingValue(SettingId Id);
bool Enabled(SettingId Id);
// Clamps, rounds for its kind, stores and saves. Returns the stored value.
double SetValue(Setting& Item, double NewValue);
std::string ValueText(const Setting& Item);
Advanced& Adv();

void ConfigInit(const fs::path& IniPath);
double IniRead(const char* Section, const char* Key, double Default);
std::string IniReadString(const char* Section, const char* Key);
// UTF-8 <-> UTF-16 for Windows paths: never go through the ANSI code page, which can't hold every folder name.
std::wstring Wide(std::string_view Text);
std::string Utf8(std::wstring_view Text);
void IniWrite(const char* Section, const char* Key, const std::string& Text);
void IniDeleteSection(const char* Section);
std::vector<std::string> IniKeys(const char* Section);

// ---------------------------------------------------------------- GML helpers

Value Builtin(const char* Name, std::initializer_list<Value> Args = {});
std::string Num(double Number);

// Raw runner values as hooks receive them.
bool RawIsNumber(const gm::RValue* Raw);
double RawNumber(const gm::RValue* Raw);
std::string RawDescribe(const gm::RValue* Raw);
void RawSetNumber(gm::RValue& Raw, double Number);  // releases what Raw held first

// GameMaker data structures arrive as refs (or plain ids). ds_exists tells the type apart.
enum class DsKind { None, Map, List, Grid };
DsKind DsKindOf(const Value& Item);
const char* DsKindName(DsKind Kind);
// Element count of an array or ds_list / ds_map (grids: width x height), 0 otherwise.
size_t ContainerSize(const Value& Item, DsKind Kind);
// Element Index of an array / ds_list, or of a 1-wide / 1-high ds_grid; undefined otherwise.
Value ContainerGet(const Value& Item, DsKind Kind, size_t Index);

// The first obj_player instance as a GML value (undefined when there is none).
Value FindPlayer();
// The player's CInstance*, captured from its Step event (see core.cpp); nullptr when no player is active.
CInstance* PlayerInstance();
void NotePlayerStep(CInstance* Self);
// The loaded player's ID, or -1. Player scripts (statGet, queueGearRecalc, playerRemoveAllCooldowns, ...)
// take it as an argument; calling them without it is a fatal GML error.
int PlayerId();

// ---------------------------------------------------------------- hooks

// Hooks a gml_Script_* or gml_Object_* function by name; Original receives the trampoline. Logs the outcome.
bool HookFunction(const char* Name, void* Detour, void** Original);
template <typename Fn>
bool Hook(const char* Name, Fn Detour, Fn& Original) {
    return HookFunction(Name, reinterpret_cast<void*>(Detour), reinterpret_cast<void**>(&Original));
}

// ---------------------------------------------------------------- command channel

// Text commands from <mod folder>\ipc\cmd.txt, answered in reply.txt (see tools/cp.py).
// Runs on the game thread, polled from the per-frame hooks.
void CommandsInit(const fs::path& Folder);
void PollCommands();
std::string RunCommand(const std::string& Line);

// Gameplay features (Features.cpp): hooks installed once, Frame() from the Draw GUI hook.
namespace features {
void Install();
void Frame();
bool HandlesCommand(const std::string& Verb);  // dps, recap
std::string Command(const std::vector<std::string>& Words);
}  // namespace features

// Playing from code for tests (Autotest.cpp): fight, keepalive.
namespace autotest {
void Install();
void Frame();
bool HandlesCommand(const std::string& Verb);
std::string Command(const std::vector<std::string>& Words);
}  // namespace autotest

// Live tracing of any script (Trace.cpp): trace <script> [count] / untrace <script> / trace.
namespace trace {
bool HandlesCommand(const std::string& Verb);
std::string Command(const std::vector<std::string>& Words);
}  // namespace trace

}  // namespace cp
