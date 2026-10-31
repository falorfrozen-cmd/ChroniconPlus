#include "Core.hpp"

#include "hook.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <fstream>
#include <mutex>

namespace cp {
namespace {

fs::path g_LogPath;
fs::path g_IniPath;
std::mutex g_LogMutex;
Advanced g_Advanced;

// Menu steps, in the order Left/Right walks them.
constexpr double kUp[] = { 1, 1.5, 2, 3, 5, 10, 25, 50, 100, 1000 };
constexpr double kDealt[] = { 1, 1.5, 2, 3, 5, 10, 25, 50, 100 };
constexpr double kTaken[] = { 0, 0.1, 0.25, 0.5, 0.75, 1, 1.5, 2, 3, 5 };
constexpr double kDown[] = { 0, 0.1, 0.25, 0.5, 0.75, 1 };
constexpr double kSpeed[] = { 0.25, 0.5, 0.75, 1, 1.25, 1.5, 2, 3 };
constexpr double kZoom[] = { 0.5, 0.75, 1, 1.25, 1.5, 2 };
constexpr double kChampions[] = { 1, 2, 3, 5, 10, 25, 100 };
constexpr double kRadius[] = { 0, 25, 50, 100, 200, 500, 1000 };
constexpr std::span<const double> kNone;

constexpr const char* kRewards = "Ödüller";
constexpr const char* kCombat = "Savaş";
constexpr const char* kCheats = "Hileler";
constexpr const char* kWorld = "Dünya";
constexpr const char* kInterface = "Arayüz";
constexpr const char* kProgress = "İlerleme";

// Same order as SettingId.
std::array<Setting, S_COUNT> g_Settings = { {
    { "exp",        kRewards,   "EXP çarpanı",            "Düşmanlardan ve görevlerden gelen EXP bu sayıyla çarpılır.",          "Multipliers", "Exp",          SettingKind::Multiplier, 0, 100000, 1, kUp, 1 },
    { "currency",   kRewards,   "Kristal çarpanı",        "Kazanılan kristal bu sayıyla çarpılır.",                               "Multipliers", "Currency",     SettingKind::Multiplier, 0, 100000, 1, kUp, 1 },
    { "loot",       kRewards,   "Loot tekrarı",           "Ölen düşmanın drop zarı bu kadar kez atılır.",                         "Multipliers", "LootRepeat",   SettingKind::Integer,    1, 10,     1, kNone, 1 },
    { "rarity",     kRewards,   "Kalite zarı",            "Her eşya bu kadar kez üretilir, en yüksek kaliteli olanı düşer.",      "Loot",        "RarityRolls",  SettingKind::Integer,    1, 10,     1, kNone, 1 },
    { "perfect",    kRewards,   "Mükemmel affix",         "Affix değerleri her zaman aralığın üst sınırında gelir.",              "Loot",        "PerfectRolls", SettingKind::Toggle,     0, 1,      0, kNone, 0 },
    { "endgame",    kRewards,   "Endgame ödülleri",       "Endgame XP, kristal ve öz ödülleri bu sayıyla çarpılır.",              "Multipliers", "Endgame",      SettingKind::Multiplier, 0, 1000,   1, kUp, 1 },

    { "dmgdealt",   kCombat,    "Verilen hasar",          "Oyuncunun düşmanlara verdiği hasar çarpanı.",                          "Combat",      "DamageDealt",  SettingKind::Multiplier, 0, 1000,   1, kDealt, 1 },
    { "dmgtaken",   kCombat,    "Alınan hasar",           "Oyuncunun aldığı hasar çarpanı (0 = hasar yok).",                      "Combat",      "DamageTaken",  SettingKind::Multiplier, 0, 100,    1, kTaken, 1 },
    { "cooldown",   kCombat,    "Cooldown süresi",        "Yetenek bekleme süreleri (0 = beklemesiz).",                           "Combat",      "Cooldown",     SettingKind::Multiplier, 0, 1,      1, kDown, 1 },
    { "manacost",   kCombat,    "Mana maliyeti",          "Yeteneklerin mana maliyeti (0 = bedava).",                             "Combat",      "ManaCost",     SettingKind::Multiplier, 0, 1,      1, kDown, 1 },
    { "speed",      kCombat,    "Oyun hızı",              "Bütün oyunun hızı: 1'in altı yavaş çekim, üstü hızlı.",                "Combat",      "GameSpeed",    SettingKind::Multiplier, 0.1, 5,    1, kSpeed, 1 },

    { "godmode",    kCheats,    "Ölümsüzlük",             "Hasar alınmaz, can her karede dolar.",                                 "Cheats",      "GodMode",      SettingKind::Toggle,     0, 1,      0, kNone, 0 },
    { "mana",       kCheats,    "Sınırsız mana",          "Mana her karede dolar.",                                               "Cheats",      "InfiniteMana", SettingKind::Toggle,     0, 1,      0, kNone, 0 },
    { "nocd",       kCheats,    "Cooldown yok",           "Bütün yetenek beklemeleri her karede sıfırlanır.",                     "Cheats",      "NoCooldowns",  SettingKind::Toggle,     0, 1,      0, kNone, 0 },
    { "freeshop",   kCheats,    "Para düşülmesin",        "Satın almalarda ve ödemelerde kristal düşülmez.",                      "Loot",        "FreeShop",     SettingKind::Toggle,     0, 1,      0, kNone, 0 },

    { "density",    kWorld,     "Canavar yoğunluğu",      "Her canavar grubu bu kadar kat kalabalık doğar (yeni bölgelerde).",    "World",       "Density",      SettingKind::Integer,    1, 5,      1, kNone, 1 },
    { "champions",  kWorld,     "Champion / elite",       "Champion ve elite çıkma oranı çarpılır (yeni bölgelerde).",           "World",       "Champions",    SettingKind::Multiplier, 0, 1000,   1, kChampions, 1 },
    { "reveal",     kWorld,     "Haritayı aç",            "Girilen her bölgenin haritası tamamen açılır.",                        "World",       "MapReveal",    SettingKind::Toggle,     0, 1,      0, kNone, 0 },
    { "zoom",       kWorld,     "Kamera uzaklığı",        "Kameranın görüş alanı; büyük değer daha uzaktan gösterir.",            "World",       "Zoom",         SettingKind::Multiplier, 0.25, 4,   1, kZoom, 1 },
    { "notraps",    kWorld,     "Sandık tuzağı yok",      "Sandıklar tuzaksız gelir.",                                            "Loot",        "NoTraps",      SettingKind::Toggle,     0, 1,      0, kNone, 0 },
    { "radius",     kWorld,     "Toplama yarıçapı +",     "Eşya toplama yarıçapına eklenir.",                                     "Loot",        "LootRadius",   SettingKind::Integer,    0, 5000,   0, kRadius, 0 },

    { "dps",        kInterface, "Hasar ölçer (DPS)",      "Sağ üstte son 5 saniyenin saniyelik hasarını ve toplamı gösterir.",    "Combat",      "DpsMeter",     SettingKind::Toggle,     0, 1,      0, kNone, 0 },
    { "deathrecap", kInterface, "Ölüm özeti",             "Son 10 vuruşu kaydeder; öldüğünde seni neyin öldürdüğünü gösterir.",   "Combat",      "DeathRecap",   SettingKind::Toggle,     0, 1,      0, kNone, 0 },

    { "talents",    kProgress,  "Yetenek puanı çarpanı",  "Seviye atlayınca kazanılan yetenek puanı bu sayıyla çarpılır.",        "Progression", "Talents",      SettingKind::Integer,    1, 10,     1, kNone, 1 },
    { "dyes",       kProgress,  "Tüm boyaları aç",        "Bütün sınıfların 40 boyasını gardıropta açar.",                        "",            "",             SettingKind::Action,     0, 0,      0, kNone, 0 },
    { "gambler",    kProgress,  "Gambler'ı aç",           "Gambler satıcısını açar.",                                             "",            "",             SettingKind::Action,     0, 0,      0, kNone, 0 },
    { "waypoints",  kProgress,  "Tüm waypoint'leri aç",   "Bütün bölgelerin waypoint'ini açar; hikâye ilerlemesini atlatır.",     "",            "",             SettingKind::Action,     0, 0,      0, kNone, 0 },
} };

double Normalize(const Setting& Item, double Number) {
    if (!std::isfinite(Number)) Number = Item.Default;
    if (Item.Kind != SettingKind::Multiplier) Number = std::round(Number);
    return std::clamp(Number, Item.Minimum, Item.Maximum);
}

CInstance* g_Player = nullptr;
ULONGLONG g_PlayerSeen = 0;

}  // namespace

// ---------------------------------------------------------------- logging

void LogInit(const fs::path& Path) {
    g_LogPath = Path;
    std::ofstream(g_LogPath, std::ios::trunc) << "ChroniconPlus loaded\n";
}

void Log(const std::string& Line) {
    std::scoped_lock lock(g_LogMutex);
    std::ofstream out(g_LogPath, std::ios::app);
    out << Line << '\n';
}

// ---------------------------------------------------------------- settings

double IniRead(const char* Section, const char* Key, double Default) {
    const std::string text = IniReadString(Section, Key);
    if (text.empty()) return Default;
    try { return std::stod(text); }
    catch (...) { return Default; }
}

std::wstring Wide(std::string_view Text) {
    if (Text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), nullptr, 0);
    std::wstring out(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), out.data(), length);
    return out;
}

std::string Utf8(std::wstring_view Text) {
    if (Text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

// The wide profile API: the game folder may be anywhere, under any name.
std::string IniReadString(const char* Section, const char* Key) {
    wchar_t buffer[512] = {};
    GetPrivateProfileStringW(Wide(Section).c_str(), Wide(Key).c_str(), L"", buffer, 512, g_IniPath.c_str());
    return Utf8(buffer);
}

void IniWrite(const char* Section, const char* Key, const std::string& Text) {
    WritePrivateProfileStringW(Wide(Section).c_str(), Wide(Key).c_str(), Wide(Text).c_str(), g_IniPath.c_str());
}

void IniDeleteSection(const char* Section) {
    WritePrivateProfileStringW(Wide(Section).c_str(), nullptr, nullptr, g_IniPath.c_str());
}

std::vector<std::string> IniKeys(const char* Section) {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetPrivateProfileStringW(Wide(Section).c_str(), nullptr, L"", buffer.data(), static_cast<DWORD>(buffer.size()), g_IniPath.c_str());
    std::vector<std::string> keys;
    for (DWORD i = 0; i < length;) {
        std::wstring key(buffer.data() + i);
        i += static_cast<DWORD>(key.size()) + 1;
        if (!key.empty()) keys.push_back(Utf8(key));
    }
    return keys;
}

void ConfigInit(const fs::path& IniPath) {
    g_IniPath = IniPath;
    for (Setting& item : g_Settings) {
        if (item.Kind == SettingKind::Action) continue;
        item.Value = Normalize(item, IniRead(item.Section, item.IniKey, item.Default));
        IniWrite(item.Section, item.IniKey, Num(item.Value));
    }

    g_Advanced.MenuKey = static_cast<int>(IniRead("Advanced", "MenuKey", 119));
    g_Advanced.ExpArgIndex = static_cast<int>(IniRead("Advanced", "ExpArgIndex", 1));
    g_Advanced.CurrencyArgIndex = static_cast<int>(IniRead("Advanced", "CurrencyArgIndex", 1));
    g_Advanced.LogArgCalls = static_cast<int>(IniRead("Advanced", "LogArgCalls", 0));
    IniWrite("Advanced", "MenuKey", std::to_string(g_Advanced.MenuKey));
    IniWrite("Advanced", "ExpArgIndex", std::to_string(g_Advanced.ExpArgIndex));
    IniWrite("Advanced", "CurrencyArgIndex", std::to_string(g_Advanced.CurrencyArgIndex));
    IniWrite("Advanced", "LogArgCalls", std::to_string(g_Advanced.LogArgCalls));
    IniWrite("Advanced", "AutoLoad", IniReadString("Advanced", "AutoLoad"));  // save name or "recent"; empty = off
}

std::span<Setting> Settings() { return g_Settings; }
Advanced& Adv() { return g_Advanced; }
double SettingValue(SettingId Id) { return g_Settings[Id].Value; }
bool Enabled(SettingId Id) { return g_Settings[Id].Value != g_Settings[Id].Default; }

Setting* FindSetting(std::string_view Key) {
    for (Setting& item : g_Settings)
        if (Key == item.Key) return &item;
    return nullptr;
}

double SetValue(Setting& Item, double NewValue) {
    if (Item.Kind == SettingKind::Action) return 0;
    Item.Value = Normalize(Item, NewValue);
    IniWrite(Item.Section, Item.IniKey, Num(Item.Value));
    return Item.Value;
}

std::string ValueText(const Setting& Item) {
    switch (Item.Kind) {
    case SettingKind::Toggle: return Item.Value ? "AÇIK" : "KAPALI";
    case SettingKind::Action: return "[Enter / Sağ]";
    case SettingKind::Integer: return Item.Value == Item.Default ? Num(Item.Value) + " (kapalı)" : Num(Item.Value);
    default: return Item.Value == Item.Default ? std::format("x{} (kapalı)", Num(Item.Value)) : "x" + Num(Item.Value);
    }
}

void SetAction(SettingId Id, std::string (*Run)()) { g_Settings[Id].Run = Run; }

namespace {
std::string Json(const std::string& Text) {
    std::string out = "\"";
    for (const char c : Text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

const char* KindKey(SettingKind Kind) {
    switch (Kind) {
    case SettingKind::Multiplier: return "multiplier";
    case SettingKind::Integer: return "integer";
    case SettingKind::Toggle: return "toggle";
    default: return "action";
    }
}
}  // namespace

void WriteCatalog(const fs::path& Path) {
    std::vector<std::string> tabs;
    for (const Setting& item : g_Settings)
        if (std::find(tabs.begin(), tabs.end(), item.Category) == tabs.end()) tabs.push_back(item.Category);

    std::string out = "{\n  \"menuKey\": \"F8\",\n  \"tabs\": [\n";
    for (size_t t = 0; t < tabs.size(); t++) {
        out += "    { \"name\": " + Json(tabs[t]) + ", \"settings\": [\n";
        bool first = true;
        for (const Setting& item : g_Settings) {
            if (tabs[t] != item.Category) continue;
            std::string steps;
            for (const double step : item.Steps) steps += (steps.empty() ? "" : ", ") + Num(step);
            out += std::string(first ? "" : ",\n") + "      { \"key\": " + Json(item.Key) + ", \"label\": " + Json(item.Label) +
                   ", \"description\": " + Json(item.Description) + ", \"kind\": \"" + KindKey(item.Kind) + "\"";
            if (item.Kind != SettingKind::Action)
                out += ", \"min\": " + Num(item.Minimum) + ", \"max\": " + Num(item.Maximum) + ", \"default\": " + Num(item.Default) +
                       ", \"steps\": [" + steps + "], \"value\": " + Num(item.Value) + ", \"valueText\": " + Json(ValueText(item));
            out += " }";
            first = false;
        }
        out += "\n    ] },\n";
    }
    out += "    { \"name\": \"Statlar\", \"statEditor\": true }\n  ]\n}\n";
    std::ofstream(Path, std::ios::binary | std::ios::trunc) << out;
}

// ---------------------------------------------------------------- GML helpers

Value Builtin(const char* Name, std::initializer_list<Value> Args) {
    if (!gm::FindBuiltin(Name)) Log(std::string("[gml] unknown built-in ") + Name);
    return gm::Call(Name, Args);
}

std::string Num(double Number) {
    if (std::abs(Number - std::round(Number)) < 1e-9) return std::format("{}", static_cast<long long>(std::round(Number)));
    std::string text = std::format("{:.3f}", Number);
    while (text.back() == '0') text.pop_back();
    if (text.back() == '.') text.pop_back();
    return text;
}

bool RawIsNumber(const gm::RValue* Raw) {
    if (!Raw) return false;
    const uint32_t kind = Raw->kind & 0xFFFFFF;
    return kind == gm::KIND_REAL || kind == gm::KIND_INT32 || kind == gm::KIND_INT64 || kind == gm::KIND_BOOL;
}

double RawNumber(const gm::RValue* Raw) {
    if (!Raw) return 0;
    switch (Raw->kind & 0xFFFFFF) {
    case gm::KIND_REAL:
    case gm::KIND_BOOL: return Raw->real;
    case gm::KIND_INT32: return Raw->i32;
    case gm::KIND_INT64: return static_cast<double>(Raw->i64);
    default: return 0;
    }
}

std::string RawDescribe(const gm::RValue* Raw) {
    return Raw ? Value::Copy(*Raw).Describe() : "null";
}

void RawSetNumber(gm::RValue& Raw, double Number) {
    Value::Adopt(Raw);  // releases a string / array / object held there
    Raw.real = Number;
    Raw.flags = 0;
    Raw.kind = gm::KIND_REAL;
}

DsKind DsKindOf(const Value& Item) {
    if (Item.Kind() != gm::KIND_REF && !(Item.IsNumber() && Item.Number() >= 0)) return DsKind::None;
    // ds_type_map = 1, ds_type_list = 2, ds_type_grid = 5
    if (Builtin("ds_exists", { Item, Value(2) }).Bool()) return DsKind::List;
    if (Builtin("ds_exists", { Item, Value(1) }).Bool()) return DsKind::Map;
    if (Builtin("ds_exists", { Item, Value(5) }).Bool()) return DsKind::Grid;
    return DsKind::None;
}

const char* DsKindName(DsKind Kind) {
    switch (Kind) {
    case DsKind::Map: return "ds_map";
    case DsKind::List: return "ds_list";
    case DsKind::Grid: return "ds_grid";
    default: return "none";
    }
}

size_t ContainerSize(const Value& Item, DsKind Kind) {
    if (Item.Kind() == gm::KIND_ARRAY) return static_cast<size_t>(Builtin("array_length", { Item }).Number());
    switch (Kind) {
    case DsKind::List: return static_cast<size_t>(Builtin("ds_list_size", { Item }).Number());
    case DsKind::Map: return static_cast<size_t>(Builtin("ds_map_size", { Item }).Number());
    case DsKind::Grid: return static_cast<size_t>(Builtin("ds_grid_width", { Item }).Number() * Builtin("ds_grid_height", { Item }).Number());
    default: return 0;
    }
}

Value ContainerGet(const Value& Item, DsKind Kind, size_t Index) {
    if (Index >= ContainerSize(Item, Kind)) return Value();
    const Value index(static_cast<double>(Index));
    if (Item.Kind() == gm::KIND_ARRAY) return Builtin("array_get", { Item, index });
    switch (Kind) {
    case DsKind::List: return Builtin("ds_list_find_value", { Item, index });
    case DsKind::Grid: {
        const double width = Builtin("ds_grid_width", { Item }).Number();
        const double height = Builtin("ds_grid_height", { Item }).Number();
        if (height == 1) return Builtin("ds_grid_get", { Item, index, Value(0.0) });
        if (width == 1) return Builtin("ds_grid_get", { Item, Value(0.0), index });
        return Value();
    }
    default: return Value();
    }
}

Value FindPlayer() {
    static Value player_object;
    static bool resolved = false;
    if (!resolved) {
        player_object = Builtin("asset_get_index", { Value("obj_player") });
        resolved = true;
        Log("obj_player asset index: " + player_object.Describe());
    }
    Value instance = Builtin("instance_find", { player_object, Value(0) });
    if (!Builtin("instance_exists", { instance }).Bool()) return Value();
    return instance;
}

// obj_player's Step event runs every frame while a character is in the world; its self is the player.
void NotePlayerStep(CInstance* Self) {
    g_Player = Self;
    g_PlayerSeen = GetTickCount64();
}

CInstance* PlayerInstance() {
    if (!g_Player || GetTickCount64() - g_PlayerSeen > 1000) return nullptr;
    return g_Player;
}

int PlayerId() {
    Value player = FindPlayer();
    if (player.IsUndefined()) return -1;
    Value id = Builtin("variable_instance_get", { player, Value("ID") });
    return id.IsNumber() ? id.Int() : -1;
}

// ---------------------------------------------------------------- hooks

bool HookFunction(const char* Name, void* Detour, void** Original) {
    void* target = gm::FindFunction(Name);
    if (!target) {
        Log(std::format("[hook] {} NOT FOUND", Name));
        return false;
    }
    std::string why;
    const bool ok = hook::Install(target, Detour, Original, &why);
    Log(std::format("[hook] {} at {} -> {}{}", Name, target, ok ? "ok" : "REFUSED: ", why));
    return ok;
}

}  // namespace cp
