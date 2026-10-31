// ChroniconPlus core, started by the version.dll loader (proxy\version_proxy.cpp) on its own thread.
// Multipliers (EXP, currency, loot), cheats (god mode, mana, cooldowns), a stat editor, an in-game menu (F8)
// and a command channel for driving the mod from code (tools/cp.py).
//
// Hook targets, identified from Chronicon.exe 1.54.1 and found by name at startup:
//   gml_Script_playerExpAdd(player, amount, ...)          -> amount is argument 1
//   gml_Script_playerAddCurrency(type, amount, applyBonus) -> amount is argument 1
//   gml_Script_actorDeathDrops()                          -> self is the dying actor
//   gml_Script_GUIdraw()                                  -> every frame in the Draw GUI event (in the world)
//   gml_Object_obj_player_Step_0                          -> self is the player, every step
//   gml_Object_SYSTEM_Draw_64                             -> SYSTEM's Draw GUI, the caller of GUIdraw
//   global.godmode                                        -> developer flag checked in actorDamage
//   gml_Script_gearRecalculate / statSet / statGetCap     -> see Stats.hpp

#include "Core.hpp"
#include "Menu.hpp"
#include "Session.hpp"
#include "Stats.hpp"

#include <csignal>
#include <exception>
#include <fstream>
#include <format>

using namespace cp;

namespace {

gm::ScriptFn g_OrigExpAdd = nullptr;
gm::ScriptFn g_OrigAddCurrency = nullptr;
gm::ScriptFn g_OrigDeathDrops = nullptr;
gm::ScriptFn g_OrigGuiDraw = nullptr;
gm::EventFn g_OrigPlayerStep = nullptr;
gm::EventFn g_OrigSystemGui = nullptr;
bool g_GuiDrawn = false;  // GUIdraw ran inside the current SYSTEM Draw GUI event
gm::ScriptFn g_RemoveCooldowns = nullptr;

bool g_GodFlagSet = false;

void LogCall(const char* Name, int& Remaining, int ArgumentCount, gm::RValue** Arguments) {
    if (Remaining <= 0) return;
    Remaining--;
    std::string line = std::format("[call] {}(", Name);
    for (int i = 0; i < ArgumentCount; i++) line += (i ? ", " : "") + RawDescribe(Arguments[i]);
    Log(line + ")");
}

// Calls a script with one numeric argument multiplied, leaving the caller's values untouched.
gm::RValue& CallScaled(gm::ScriptFn Original, int ArgIndex, double Multiplier, CInstance* Self, CInstance* Other,
                       gm::RValue& Result, int ArgumentCount, gm::RValue** Arguments) {
    if (Multiplier == 1.0 || ArgIndex < 0 || ArgIndex >= ArgumentCount || !RawIsNumber(Arguments[ArgIndex]))
        return Original(Self, Other, Result, ArgumentCount, Arguments);

    const double amount = RawNumber(Arguments[ArgIndex]);
    if (amount <= 0) return Original(Self, Other, Result, ArgumentCount, Arguments);

    gm::RValue scaled{};
    scaled.real = amount * Multiplier;
    scaled.kind = gm::KIND_REAL;
    std::vector<gm::RValue*> arguments(Arguments, Arguments + ArgumentCount);
    arguments[ArgIndex] = &scaled;
    return Original(Self, Other, Result, ArgumentCount, arguments.data());
}

void ApplyPlayerEffects() {
    const bool god = Enabled(S_GODMODE);
    if (god != g_GodFlagSet) {
        Builtin("variable_global_set", { Value("godmode"), Value(god) });
        g_GodFlagSet = god;
    }
    if (!god && !Enabled(S_MANA) && !Enabled(S_NOCOOLDOWN)) return;

    Value player = FindPlayer();
    if (player.IsUndefined()) return;

    if (god) {
        Value max_hp = Builtin("variable_instance_get", { player, Value("maxhp") });
        if (max_hp.IsNumber() && max_hp.Number() > 0) Builtin("variable_instance_set", { player, Value("hp"), max_hp });
    }
    if (Enabled(S_MANA)) {
        Value max_mana = Builtin("variable_instance_get", { player, Value("maxmana") });
        if (max_mana.IsNumber() && max_mana.Number() > 0) Builtin("variable_instance_set", { player, Value("mana"), max_mana });
    }
    if (Enabled(S_NOCOOLDOWN))
        if (CInstance* self = PlayerInstance()) gm::CallScript(g_RemoveCooldowns, self, { Value(PlayerId()) });
}

// ---------------------------------------------------------------- hooks

gm::RValue& HookExpAdd(CInstance* Self, CInstance* Other, gm::RValue& Result, int ArgumentCount, gm::RValue** Arguments) {
    static int remaining = Adv().LogArgCalls;
    LogCall("playerExpAdd", remaining, ArgumentCount, Arguments);
    return CallScaled(g_OrigExpAdd, Adv().ExpArgIndex, SettingValue(S_EXP), Self, Other, Result, ArgumentCount, Arguments);
}

gm::RValue& HookAddCurrency(CInstance* Self, CInstance* Other, gm::RValue& Result, int ArgumentCount, gm::RValue** Arguments) {
    static int remaining = Adv().LogArgCalls;
    LogCall("playerAddCurrency", remaining, ArgumentCount, Arguments);
    return CallScaled(g_OrigAddCurrency, Adv().CurrencyArgIndex, SettingValue(S_CURRENCY), Self, Other, Result, ArgumentCount, Arguments);
}

gm::RValue& HookDeathDrops(CInstance* Self, CInstance* Other, gm::RValue& Result, int ArgumentCount, gm::RValue** Arguments) {
    gm::RValue& result = g_OrigDeathDrops(Self, Other, Result, ArgumentCount, Arguments);
    const int repeat = static_cast<int>(SettingValue(S_LOOT));
    for (int i = 1; i < repeat; i++) g_OrigDeathDrops(Self, Other, Result, ArgumentCount, Arguments);
    static int logged = 0;
    if (repeat > 1 && logged++ < 5) Log(std::format("[loot] death drops rolled {} times", repeat));
    return result;
}

void HookPlayerStep(CInstance* Self, CInstance* Other) {
    NotePlayerStep(Self);
    g_OrigPlayerStep(Self, Other);
}

gm::RValue& HookGuiDraw(CInstance* Self, CInstance* Other, gm::RValue& Result, int ArgumentCount, gm::RValue** Arguments) {
    gm::RValue& result = g_OrigGuiDraw(Self, Other, Result, ArgumentCount, Arguments);

    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("GUIdraw hook active (first frame).");
    }

    g_GuiDrawn = true;
    session::Tick(true);
    ApplyPlayerEffects();
    features::Frame();
    autotest::Frame();
    menu::Frame();
    return result;
}

// SYSTEM's Draw GUI calls GUIdraw only while the player's HUD is up; on the death screen it skips it, which would
// hide the death recap exactly when it opens. Keep an open menu or recap drawn (and closable) there.
void HookSystemGui(CInstance* Self, CInstance* Other) {
    g_GuiDrawn = false;
    g_OrigSystemGui(Self, Other);
    if (!g_GuiDrawn && menu::Visible()) menu::Frame();
}

// Where a fault happened, as module+offset, for the log.
std::string Where(const void* Address) {
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<const char*>(Address), &module))
        GetModuleFileNameA(module, name, MAX_PATH);
    const char* file = strrchr(name, 92);  // last backslash
    return std::format("{}+0x{:X}", file ? file + 1 : name, reinterpret_cast<uintptr_t>(Address) - reinterpret_cast<uintptr_t>(module));
}

// Logs faults that would end the game silently: C++ terminate / abort, and access violations or stack overflows
// (first chance, but only once per address, so the game's own handled exceptions are not flooded).
LONG CALLBACK LogFault(EXCEPTION_POINTERS* Info) {
    const DWORD code = Info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW && code != 0xE06D7363 /* C++ throw */)
        return EXCEPTION_CONTINUE_SEARCH;
    static void* last = nullptr;
    static int stacks = 0;
    void* address = Info->ExceptionRecord->ExceptionAddress;
    if (address != last || stacks < 3) {
        last = address;
        std::string line = std::format("[fault] 0x{:08X} at {}", code, Where(address));
        if (stacks++ < 3) {
            void* frames[24] = {};
            const USHORT count = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
            for (USHORT i = 0; i < count; i++) line += "\n    " + Where(frames[i]);
        }
        Log(line);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallFaultLog() {
    AddVectoredExceptionHandler(1, LogFault);
    std::set_terminate([] {
        Log("[fault] std::terminate (an exception escaped)");
        std::abort();
    });
    std::signal(SIGABRT, [](int) { Log("[fault] abort()"); });
}

fs::path ModFolder() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path() / "ChroniconPlus";
}

bool Writable(const fs::path& Folder) {
    std::error_code error;
    fs::create_directories(Folder, error);
    const fs::path probe = Folder / ".write-test";
    bool ok = static_cast<bool>(std::ofstream(probe, std::ios::trunc) << "ok");
    ok = ok && fs::exists(probe, error);
    fs::remove(probe, error);
    return ok;
}

// Settings, log and command channel live next to the DLL. Where the game folder is read-only for this user (a
// locked-down Program Files install), fall back to %LOCALAPPDATA%\ChroniconPlus instead of silently losing them.
fs::path DataFolder() {
    const fs::path folder = ModFolder();
    if (Writable(folder)) return folder;
    wchar_t local[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
        const fs::path fallback = fs::path(local) / "ChroniconPlus";
        if (Writable(fallback)) return fallback;
    }
    return folder;
}

void Start() {
    const fs::path folder = DataFolder();
    LogInit(folder / "ChroniconPlus.log");
    if (folder != ModFolder()) Log("game folder is read-only; settings and log are in " + Utf8(folder.wstring()));
    ConfigInit(folder / "ChroniconPlus.ini");
    CommandsInit(folder);
    InstallFaultLog();
    WriteCatalog(folder / "menu-catalog.json");

    std::string why;
    if (!gm::Init(&why)) {
        Log("runner not recognised, nothing installed: " + why);
        return;
    }
    Log("runner: " + gm::Summary());

    // The menu and the command channel need GUIdraw; the rest are optional so one missing script doesn't disable everything.
    if (!Hook("gml_Script_GUIdraw", &HookGuiDraw, g_OrigGuiDraw)) return;
    Hook("gml_Object_SYSTEM_Draw_64", &HookSystemGui, g_OrigSystemGui);
    Hook("gml_Object_obj_player_Step_0", &HookPlayerStep, g_OrigPlayerStep);
    Hook("gml_Script_playerExpAdd", &HookExpAdd, g_OrigExpAdd);
    Hook("gml_Script_playerAddCurrency", &HookAddCurrency, g_OrigAddCurrency);
    Hook("gml_Script_actorDeathDrops", &HookDeathDrops, g_OrigDeathDrops);
    g_RemoveCooldowns = gm::FindScript("gml_Script_playerRemoveAllCooldowns");
    stats::Install();
    features::Install();
    autotest::Install();
    session::Install();
    Log("ready");
}

}  // namespace

extern "C" __declspec(dllexport) void ChroniconPlus_Start() {
    // Runs on the loader's thread: anything thrown here must not reach std::terminate and take the game down.
    try { Start(); }
    catch (const std::exception& e) { Log(std::string("[fault] startup stopped: ") + e.what()); }
    catch (...) { Log("[fault] startup stopped"); }
}
