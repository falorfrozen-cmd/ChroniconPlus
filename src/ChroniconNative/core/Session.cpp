#include "Session.hpp"

#include <algorithm>
#include <format>

namespace cp::session {
namespace {

gm::EventFn g_OrigMenuStep = nullptr;
gm::ScriptFn g_MenuSetState = nullptr;
CInstance* g_Menu = nullptr;  // obj_menu, captured from its Step event; only used from inside that event
bool g_Busy = false;
ULONGLONG g_LastTick = 0;
ULONGLONG g_LastDrawTick = 0;

// ---------------------------------------------------------------- pending load
//
// Main menu: each obj_menu_button carries the menu state it opens in "target"; a click runs
// menuSetState(target) with obj_menu as self (menuNavigate resets obj_menu.updatestate before reading it, so
// writing that variable from outside does nothing). "Continue as <name>" (state 54) loads that character.

struct Load {
    bool Active = false;
    std::string Name;
    ULONGLONG Started = 0;
    ULONGLONG NextStep = 0;
    int Attempts = 0;
};
Load g_Load;

constexpr ULONGLONG kLoadTimeoutMs = 120000;

std::string Lower(std::string Text) {
    std::transform(Text.begin(), Text.end(), Text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return Text;
}

std::string MostRecentSave() {
    wchar_t local[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return "";
    std::error_code error;
    fs::path newest;
    fs::file_time_type newest_time{};
    for (const auto& entry : fs::directory_iterator(fs::path(local) / "Chronicon" / "save", error)) {
        if (entry.path().extension() != ".char") continue;
        const auto time = entry.last_write_time(error);
        if (newest.empty() || time > newest_time) {
            newest = entry.path();
            newest_time = time;
        }
    }
    return newest.empty() ? "" : Utf8(newest.stem().wstring());
}

std::vector<Value> Instances(const char* Object) {
    std::vector<Value> result;
    Value object = Builtin("asset_get_index", { Value(Object) });
    if (object.IsNumber() && object.Number() < 0) return result;
    const int count = static_cast<int>(Builtin("instance_number", { object }).Number());
    for (int i = 0; i < count; i++) result.push_back(Builtin("instance_find", { object, Value(i) }));
    return result;
}

Value InstanceVar(const Value& Instance, const char* Name) {
    return Builtin("variable_instance_get", { Instance, Value(Name) });
}

void FinishLoad(const std::string& Reason) {
    Log("[load] " + Reason);
    g_Load.Active = false;
}

void LoadStep() {
    if (!g_Load.Active) return;
    const ULONGLONG now = GetTickCount64();
    if (now < g_Load.NextStep) return;
    if (PlayerInstance()) return FinishLoad("character loaded: " + g_Load.Name);
    if (now - g_Load.Started > kLoadTimeoutMs) return FinishLoad("gave up after 120 s");

    const std::vector<Value> menus = Instances("obj_menu");
    const std::vector<Value> buttons = Instances("obj_menu_button");
    if (menus.empty() || buttons.empty()) {
        g_Load.NextStep = now + 1000;  // still starting up, or loading
        return;
    }
    if (g_Load.Attempts >= 3) return FinishLoad("the menu did not load the character");

    for (const Value& button : buttons) {
        const std::string text = Lower(InstanceVar(button, "text").Text());
        Value target = InstanceVar(button, "target");
        if (!target.IsNumber() || text.find("continue") == std::string::npos) continue;
        if (text.find(Lower(g_Load.Name)) == std::string::npos)
            return FinishLoad("the Continue button is for another character: " + text +
                              " (only the last played character can be loaded this way)");
        if (!g_Menu || !g_MenuSetState) {
            g_Load.NextStep = now + 500;
            return;
        }
        g_Load.Attempts++;
        Log(std::format("[load] menu \"{}\": menuSetState({})", text, target.Describe()));
        gm::CallScript(g_MenuSetState, g_Menu, { target });
        g_Load.NextStep = now + 3000;
        return;
    }
    g_Load.NextStep = now + 1000;  // another menu page; wait for the main one
}

void HookMenuStep(CInstance* Self, CInstance* Other) {
    g_OrigMenuStep(Self, Other);
    g_Menu = Self;
    Tick(false);
    g_Menu = nullptr;
}

// ---------------------------------------------------------------- instance tools

std::string InstancesCommand(const std::vector<std::string>& Words) {
    if (Words.size() < 2) return "ERR kullanım: instances <obje> [değişken...]";
    const std::vector<Value> found = Instances(Words[1].c_str());
    std::string out = std::format("OK {}: {} örnek", Words[1], found.size());
    for (const Value& instance : found) {
        out += std::format("\n  {}", Builtin("real", { instance }).Describe());
        for (size_t w = 2; w < Words.size(); w++) out += std::format("  {}={}", Words[w], InstanceVar(instance, Words[w].c_str()).Describe());
    }
    return out;
}

std::string InstanceVarCommand(const std::vector<std::string>& Words) {
    if (Words.size() < 3) return "ERR kullanım: ivar <id> <değişken> [sayı]";
    double id = 0;
    try { id = std::stod(Words[1]); }
    catch (...) { return "ERR id sayı olmalı"; }
    Value instance(id);
    if (!Builtin("instance_exists", { instance }).Bool()) return "ERR böyle bir örnek yok";
    if (Words.size() > 3) {
        try { Builtin("variable_instance_set", { instance, Value(Words[2]), Value(std::stod(Words[3])) }); }
        catch (...) { return "ERR sayı bekleniyordu"; }
    }
    return std::format("OK {}.{} = {}", Words[1], Words[2], InstanceVar(instance, Words[2].c_str()).Describe());
}

}  // namespace

void Install() {
    Hook("gml_Object_obj_menu_Step_0", &HookMenuStep, g_OrigMenuStep);
    g_MenuSetState = gm::FindScript("gml_Script_menuSetState");
    const std::string auto_load = IniReadString("Advanced", "AutoLoad");
    if (!auto_load.empty()) Log("[load] AutoLoad: " + RequestLoad(auto_load));
}

void Tick(bool FromDraw) {
    const ULONGLONG now = GetTickCount64();
    if (FromDraw)
        g_LastDrawTick = now;
    else if (now - g_LastDrawTick < 500)
        return;
    if (g_Busy || now - g_LastTick < 100) return;
    g_Busy = true;
    g_LastTick = now;
    PollCommands();
    LoadStep();
    g_Busy = false;
}

std::string RequestLoad(const std::string& Name) {
    std::string name = Name;
    if (name.empty() || Lower(name) == "recent") name = MostRecentSave();
    if (name.empty()) return "ERR kayıt bulunamadı";
    g_Load = {};
    g_Load.Active = true;
    g_Load.Name = name;
    g_Load.Started = GetTickCount64();
    return "OK " + name + " yükleniyor";
}

bool HandlesCommand(const std::string& Verb) { return Verb == "load" || Verb == "instances" || Verb == "ivar"; }

std::string Command(const std::vector<std::string>& Words) {
    const std::string& verb = Words[0];
    if (verb == "load") {
        if (PlayerInstance()) return "ERR zaten bir karakter yüklü";
        return RequestLoad(Words.size() > 1 ? Words[1] : "recent");
    }
    if (verb == "instances") return InstancesCommand(Words);
    return InstanceVarCommand(Words);
}

}  // namespace cp::session
