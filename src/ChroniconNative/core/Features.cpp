// Gameplay features beyond the first multipliers: combat, loot, world, progression.
// Each hook leaves the game untouched while its setting is at the neutral default.
//
// Targets and their arguments, from Chronicon.exe 1.54.1 (argument counts from the game's own call sites):
//   skillGetCooldown(skill, player) / skillGetCost(skill, player, ?) -> number; the result is scaled
//   enchantGetMin(3) / enchantGetMax(3)    -> roll bounds; "perfect rolls" answers Min with Max
//   itemRollQuality(1)                     -> an item's quality (event drops, vendors); "quality rolls" keeps the best of N
//   itemGenerate(6) -> item ds_map         -> normal drops roll quality inside; the best of N items is kept
//   currencyDeduct(type, amount)           -> takes currency; skipped when "no deduction" is on
//   endgameGetXPReward/CrystalReward/EssenceReward -> endgame rewards; scaled
//   chestRollTrap() self = chest           -> sets chest.trap; cleared afterwards when traps are off
//   dunSpawnEnemies(2)                     -> populates an area; reads global championrate / eliterate
//                                             (scaled during the call); creates a monsterSquad per pack
//   gml_Object_monsterSquad_Step_0         -> spawns squad.monsters monsters; density scales that count
//   minimapExplore()                       -> the map-revealing relic's own call; run once per area (global worldlocation)
//   camSetConfig()                         -> sets global cam_wview / cam_hview for view camid
//   playerLevelUp()                        -> adds to global talentpoints[player]
//   playerdyeUnlock("class_n"), gamblerUnlock(), waypointUnlock(area) with global waypoint[player] (ds_map)
#include "Core.hpp"
#include "Stats.hpp"
#include "Menu.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <format>
#include <optional>


namespace cp::features {
namespace {

using gm::RValue;
using gm::ScriptFn;

// ---------------------------------------------------------------- helpers

void ScaleResult(RValue& Result, double Multiplier, bool Round = false) {
    if (Multiplier == 1.0 || !RawIsNumber(&Result)) return;
    double scaled = RawNumber(&Result) * Multiplier;
    if (Round) scaled = std::round(scaled);
    RawSetNumber(Result, scaled);
}

// The first few times each feature changes something, for checking it from the log.
void Note(const char* Tag, const std::string& Text) {
    static std::map<std::string, int> counts;
    if (++counts[Tag] <= 5) Log(std::format("[{}] {}", Tag, Text));
}

CInstance* Self() {
    CInstance* player = PlayerInstance();
    return player ? player : gm::Global();
}

// ---------------------------------------------------------------- combat

ScriptFn g_OrigCooldown = nullptr, g_OrigCost = nullptr;

RValue& HookCooldown(CInstance* Self, CInstance* Other, RValue& Result, int Count, RValue** Args) {
    RValue& result = g_OrigCooldown(Self, Other, Result, Count, Args);
    const double before = RawNumber(&result);
    ScaleResult(result, SettingValue(S_COOLDOWN_MULT));
    if (SettingValue(S_COOLDOWN_MULT) != 1.0) Note("cooldown", std::format("{} -> {}", Num(before), Num(RawNumber(&result))));
    return result;
}

RValue& HookCost(CInstance* Self, CInstance* Other, RValue& Result, int Count, RValue** Args) {
    RValue& result = g_OrigCost(Self, Other, Result, Count, Args);
    const double before = RawNumber(&result);
    ScaleResult(result, SettingValue(S_COST_MULT));
    if (SettingValue(S_COST_MULT) != 1.0 && before != 0) Note("manacost", std::format("{} -> {}", Num(before), Num(RawNumber(&result))));
    return result;
}

// actorDamage(dmg, crit, element, trigger, attacker, skill) with self = the actor being hit (the projectile
// hit event builds them in that order inside with(dmgtarget)).
ScriptFn g_OrigDamage = nullptr;
double g_PlayerRef = -1;  // real(player instance), refreshed every frame

struct Hit {
    ULONGLONG Time;
    double Amount;
    std::string Source;
    double HpAfter;
};
std::vector<std::pair<ULONGLONG, double>> g_Dealt;  // player damage in the last seconds, for DPS
double g_DealtTotal = 0;
ULONGLONG g_DealtSince = 0;
std::vector<Hit> g_Taken;                            // last hits on the player
std::vector<Hit> g_LastDeath;                        // g_Taken frozen when the player died
int g_Deaths = 0;

double InstanceHp(CInstance* Actor) {
    const Value hp = Builtin("variable_instance_get", { Value(Actor), Value("hp") });
    return hp.IsNumber() ? hp.Number() : 0;
}

std::string SourceName(const RValue* Attacker) {
    if (!Attacker) return "?";
    const Value attacker = Value::Copy(*Attacker);
    if (!Builtin("instance_exists", { attacker }).Bool()) return "?";
    // Actors carry the name the game shows ("Fallen Guard", "Ghoul"); the object name is only a fallback.
    const Value shown = Builtin("variable_instance_get", { attacker, Value("name") });
    if (shown.IsString() && !shown.Text().empty()) return shown.Text();
    const Value object = Builtin("variable_instance_get", { attacker, Value("object_index") });
    const std::string name = Builtin("object_get_name", { object }).Text();
    return name.empty() ? "?" : name;
}

bool IsPlayerRef(const RValue* Attacker) {
    return Attacker && g_PlayerRef >= 0 && Builtin("real", { Value::Copy(*Attacker) }).Number() == g_PlayerRef;
}

RValue& HookDamage(CInstance* Self, CInstance* Other, RValue& Result, int Count, RValue** Args) {
    if (Count < 5 || !RawIsNumber(Args[0])) return g_OrigDamage(Self, Other, Result, Count, Args);
    {
        std::string line;
        for (int i = 0; i < Count; i++) line += (i ? ", " : "") + RawDescribe(Args[i]);
        Note("dmgargs", "actorDamage(" + line + ")");
    }
    CInstance* player = PlayerInstance();
    const bool to_player = player && Self == player;
    const bool by_player = !to_player && IsPlayerRef(Args[4]);
    const double multiplier = to_player ? SettingValue(S_DMG_TAKEN) : by_player ? SettingValue(S_DMG_DEALT) : 1.0;
    const bool measure = (by_player && Enabled(S_DPS_METER)) || (to_player && Enabled(S_DEATH_RECAP));
    const double hp_before = measure ? InstanceHp(Self) : 0;

    RValue scaled{};
    std::vector<RValue*> args(Args, Args + Count);
    if (multiplier != 1.0) {
        scaled.kind = gm::KIND_REAL;
        scaled.real = RawNumber(Args[0]) * multiplier;
        args[0] = &scaled;
        Note(to_player ? "taken" : "dealt", std::format("{} -> {}", Num(RawNumber(Args[0])), Num(scaled.real)));
    }
    RValue& result = g_OrigDamage(Self, Other, Result, Count, args.data());

    if (measure) {
        const ULONGLONG now = GetTickCount64();
        const double hp_after = InstanceHp(Self);
        // Training dummies (obj_dummy) keep a garbage hp (int64 min, then ~2e19) that never drops; measure them by the
        // damage argument instead, so the meter still works where players test their builds.
        const bool sane_hp = std::abs(hp_before) < 1e15 && std::abs(hp_after) < 1e15;
        const double amount = sane_hp ? (std::max)(0.0, hp_before - hp_after) : (std::max)(0.0, RawNumber(args[0]));
        if (by_player && amount > 0) {
            if (!g_DealtSince) g_DealtSince = now;
            g_Dealt.push_back({ now, amount });
            g_DealtTotal += amount;
        }
        if (to_player && amount > 0) {
            g_Taken.push_back({ now, amount, SourceName(Args[4]), hp_after });
            if (g_Taken.size() > 10) g_Taken.erase(g_Taken.begin());
            if (hp_after <= 0) {
                g_LastDeath = g_Taken;
                g_Deaths++;
                std::vector<menu::DamageHit> recap;
                for (const auto& hit : g_LastDeath)
                    recap.push_back({ (now - hit.Time) / 1000.0, hit.Amount, hit.Source, hit.HpAfter });
                menu::NotifyDeath(recap);
                Log(std::format("[recap] death #{}: last hit {} from {}", g_Deaths, Num(amount), g_Taken.back().Source));
            }
        }
    }
    return result;
}

double Dps() {
    const ULONGLONG now = GetTickCount64();
    std::erase_if(g_Dealt, [&](const auto& hit) { return now - hit.first > 5000; });
    double sum = 0;
    for (const auto& hit : g_Dealt) sum += hit.second;
    return sum / 5.0;
}

double g_BaseSpeed = 0;
double g_AppliedSpeed = 1;

void ApplyGameSpeed() {
    const double wanted = SettingValue(S_GAME_SPEED);
    if (wanted == g_AppliedSpeed) return;
    if (g_BaseSpeed <= 0) g_BaseSpeed = Builtin("game_get_speed", { Value(0) }).Number();  // gamespeed_fps
    if (g_BaseSpeed <= 0) return;
    Builtin("game_set_speed", { Value(g_BaseSpeed * wanted), Value(0) });
    Log(std::format("[speed] {} fps (x{})", Num(g_BaseSpeed * wanted), Num(wanted)));
    g_AppliedSpeed = wanted;
}

// ---------------------------------------------------------------- loot and crafting

ScriptFn g_OrigEnchantMin = nullptr, g_EnchantMax = nullptr, g_OrigQuality = nullptr, g_OrigDeduct = nullptr;
ScriptFn g_OrigGenerate = nullptr;

// An item from itemGenerate is a ds_map; its "quality" key is the rarity (higher is better).
double ItemQuality(const Value& Item) {
    if (DsKindOf(Item) != DsKind::Map) return -1e9;
    const Value quality = Builtin("ds_map_find_value", { Item, Value("quality") });
    return quality.IsNumber() ? quality.Number() : -1e9;
}

void DestroyItem(const Value& Item) {
    if (DsKindOf(Item) == DsKind::Map) Builtin("ds_map_destroy", { Item });
}

// itemGenerate(6) rolls the quality inside (drop pools and magic find): generate N items and keep the best quality.
RValue& HookGenerate(CInstance* Self, CInstance* Other, RValue& Result, int Count, RValue** Args) {
    RValue& first = g_OrigGenerate(Self, Other, Result, Count, Args);
    const int rolls = static_cast<int>(SettingValue(S_RARITY_ROLLS));
    if (rolls <= 1) return first;
    Value best = Value::Copy(first);
    double best_quality = ItemQuality(best);
    const double first_quality = best_quality;
    if (best_quality <= -1e9) return first;
    bool replaced = false;
    for (int i = 1; i < rolls; i++) {
        RValue again{};
        again.kind = gm::KIND_UNDEFINED;
        RValue& roll = g_OrigGenerate(Self, Other, again, Count, Args);
        Value candidate = &roll == &again ? Value::Adopt(again) : Value::Copy(roll);
        const double quality = ItemQuality(candidate);
        if (quality > best_quality) {
            if (replaced) DestroyItem(best);
            else replaced = true;  // the first item is still in Result; destroyed below
            best = candidate;
            best_quality = quality;
        } else
            DestroyItem(candidate);
    }
    if (replaced) {
        DestroyItem(Value::Copy(first));
        gm::Assign(first, best);
    }
    Note("quality", std::format("item quality {} -> best of {}: {}", Num(first_quality), rolls, Num(best_quality)));
    return first;
}

RValue& HookEnchantMin(CInstance* Self, CInstance* Other, RValue& Result, int Count, RValue** Args) {
    if (Enabled(S_PERFECT_ROLLS) && g_EnchantMax) return g_EnchantMax(Self, Other, Result, Count, Args);
    return g_OrigEnchantMin(Self, Other, Result, Count, Args);
}

RValue& HookQuality(CInstance* Self, CInstance* Other, RValue& Result, int Count, RValue** Args) {
    RValue& first = g_OrigQuality(Self, Other, Result, Count, Args);
    const int rolls = static_cast<int>(SettingValue(S_RARITY_ROLLS));
    if (rolls <= 1 || !RawIsNumber(&first)) return first;
    double best = RawNumber(&first);
    for (int i = 1; i < rolls; i++) {
        RValue again{};
        again.kind = gm::KIND_UNDEFINED;
        RValue& roll = g_OrigQuality(Self, Other, again, Count, Args);
        if (RawIsNumber(&roll)) best = (std::max)(best, RawNumber(&roll));
        Value::Adopt(again);
    }
    Note("quality", std::format("best of {}: {}", rolls, Num(best)));
    RawSetNumber(first, best);
    return first;
}

RValue& HookDeduct(CInstance* Self, CInstance* Other, RValue& Result, int Count, RValue** Args) {
    if (Enabled(S_FREE_SHOP)) {
        Note("freeshop", "skipped currencyDeduct(" + (Count > 1 ? RawDescribe(Args[0]) + ", " + RawDescribe(Args[1]) : std::string()) + ")");
        return Result;
    }
    return g_OrigDeduct(Self, Other, Result, Count, Args);
}

ScriptFn g_OrigChestTrap = nullptr;

RValue& HookChestTrap(CInstance* Self, CInstance* Other, RValue& Result, int Count, RValue** Args) {
    RValue& result = g_OrigChestTrap(Self, Other, Result, Count, Args);
    if (Enabled(S_NO_TRAPS) && Self) {
        Note("notraps", "chest trap was " + Builtin("variable_instance_get", { Value(Self), Value("trap") }).Describe());
        Builtin("variable_instance_set", { Value(Self), Value("trap"), Value(0) });
    }
    return result;
}

// ---------------------------------------------------------------- endgame rewards

constexpr const char* kRewardScripts[] = {
    "gml_Script_endgameGetXPReward", "gml_Script_endgameGetCrystalReward", "gml_Script_endgameGetEssenceReward",
};
std::array<ScriptFn, 3> g_OrigRewards{};

template <int N>
RValue& HookReward(CInstance* Self, CInstance* Other, RValue& Result, int Count, RValue** Args) {
    RValue& result = g_OrigRewards[N](Self, Other, Result, Count, Args);
    const double before = RawNumber(&result);
    ScaleResult(result, SettingValue(S_ENDGAME), N != 0);
    if (SettingValue(S_ENDGAME) != 1.0) Note("endgame", std::format("{} {} -> {}", kRewardScripts[N] + 11, Num(before), Num(RawNumber(&result))));
    return result;
}

// ---------------------------------------------------------------- world

ScriptFn g_OrigSpawnEnemies = nullptr;
gm::EventFn g_OrigSquadStep = nullptr;
ScriptFn g_MinimapExplore = nullptr, g_OrigCamConfig = nullptr;

// championrate / eliterate (and their difficulty parts) are read by dunSpawnEnemies; scale them for the call.
constexpr const char* kRateGlobals[] = { "championrate", "eliterate", "d_champ", "d_elite" };

RValue& HookSpawnEnemies(CInstance* Self, CInstance* Other, RValue& Result, int Count, RValue** Args) {
    const double champions = SettingValue(S_CHAMPIONS);
    std::array<Value, 4> saved;
    std::string rates;
    if (champions != 1.0)
        for (size_t i = 0; i < std::size(kRateGlobals); i++) {
            saved[i] = Builtin("variable_global_get", { Value(kRateGlobals[i]) });
            rates += std::format(" {}={}", kRateGlobals[i], saved[i].Describe());
            if (saved[i].IsNumber())
                Builtin("variable_global_set", { Value(kRateGlobals[i]), Value(saved[i].Number() * champions) });
        }
    RValue& result = g_OrigSpawnEnemies(Self, Other, Result, Count, Args);
    if (champions != 1.0) {
        Note("champions", std::format("area populated with rates x{}:{}", Num(champions), rates));
        for (size_t i = 0; i < std::size(kRateGlobals); i++)
            if (saved[i].IsNumber()) Builtin("variable_global_set", { Value(kRateGlobals[i]), saved[i] });
    }
    return result;
}

// dunSpawnEnemies creates a monsterSquad per pack and sets its "monsters" (how many it spawns); the squad spawns
// them in its Step. Density scales that count once per squad (marked with cp_density). Copying squads instead
// crashed: the copies missed the variables set after instance_create.
void HookSquadStep(CInstance* Self, CInstance* Other) {
    const int density = static_cast<int>(SettingValue(S_DENSITY));
    if (density > 1 && Self) {
        const Value squad(Self);
        if (Builtin("variable_instance_get", { squad, Value("cp_density") }).IsUndefined()) {
            const Value monsters = Builtin("variable_instance_get", { squad, Value("monsters") });
            if (monsters.IsNumber() && monsters.Number() > 0) {
                Builtin("variable_instance_set", { squad, Value("monsters"), Value(std::round(monsters.Number() * density)) });
                Note("density", std::format("squad of {} -> {}", Num(monsters.Number()), Num(std::round(monsters.Number() * density))));
            }
            Builtin("variable_instance_set", { squad, Value("cp_density"), Value(1) });
        }
    }
    g_OrigSquadStep(Self, Other);
}

// minimapExplore overflows the stack when called while a world is being built (from minimapSetArea), so the map is
// revealed from the frame loop instead: once per area, a second after the player is in it.
bool g_Revealing = false;

void RevealMap() {
    if (!g_MinimapExplore || g_Revealing) return;
    g_Revealing = true;
    gm::CallScript(g_MinimapExplore, Self(), {});
    g_Revealing = false;
}

// camSetConfig does not always rebuild cam_wview / cam_hview, so the size we wrote is remembered: while it is
// still there, zoom starts from the game's own size again instead of compounding.
double g_ViewBaseW = 0, g_ViewBaseH = 0, g_ViewWrittenW = -1, g_ViewWrittenH = -1;

void ApplyZoom() {
    const double zoom = SettingValue(S_ZOOM);
    if (zoom == 1.0 && g_ViewWrittenW < 0) return;
    const Value width = Builtin("variable_global_get", { Value("cam_wview") });
    const Value height = Builtin("variable_global_get", { Value("cam_hview") });
    const Value camera = Builtin("variable_global_get", { Value("camid") });
    if (!width.IsNumber() || !height.IsNumber() || !camera.IsNumber()) return;
    if (width.Number() != g_ViewWrittenW || height.Number() != g_ViewWrittenH) {
        g_ViewBaseW = width.Number();
        g_ViewBaseH = height.Number();
    }
    const double w = std::round(g_ViewBaseW * zoom), h = std::round(g_ViewBaseH * zoom);
    Builtin("variable_global_set", { Value("cam_wview"), Value(w) });
    Builtin("variable_global_set", { Value("cam_hview"), Value(h) });
    Builtin("camera_set_view_size", { camera, Value(w), Value(h) });
    if (zoom == 1.0)
        g_ViewWrittenW = g_ViewWrittenH = -1;  // back to the game's size; stop touching it
    else
        g_ViewWrittenW = w, g_ViewWrittenH = h;
}

RValue& HookCamConfig(CInstance* Self, CInstance* Other, RValue& Result, int Count, RValue** Args) {
    RValue& result = g_OrigCamConfig(Self, Other, Result, Count, Args);
    ApplyZoom();
    return result;
}

// ---------------------------------------------------------------- progression

ScriptFn g_OrigLevelUp = nullptr, g_DyeUnlock = nullptr, g_DyeIsUnlocked = nullptr, g_GamblerUnlock = nullptr;
ScriptFn g_WaypointUnlock = nullptr;

std::optional<double> TalentPoints() {
    const int player = PlayerId();
    Value all = Builtin("variable_global_get", { Value("talentpoints") });
    if (player < 0 || all.Kind() != gm::KIND_ARRAY) return std::nullopt;
    Value points = ContainerGet(all, DsKind::None, player);
    if (!points.IsNumber()) return std::nullopt;
    return points.Number();
}

RValue& HookLevelUp(CInstance* Self, CInstance* Other, RValue& Result, int Count, RValue** Args) {
    const std::optional<double> before = TalentPoints();
    RValue& result = g_OrigLevelUp(Self, Other, Result, Count, Args);
    const int multiplier = static_cast<int>(SettingValue(S_TALENTS));
    const std::optional<double> after = TalentPoints();
    if (multiplier > 1 && before && after && *after > *before) {
        const double extra = (*after - *before) * (multiplier - 1);
        Value all = Builtin("variable_global_get", { Value("talentpoints") });
        Builtin("array_set", { all, Value(PlayerId()), Value(*after + extra) });
        Log(std::format("[talents] level up: +{} points, +{} extra", Num(*after - *before), Num(extra)));
    }
    return result;
}

std::string UnlockDyes() {
    CInstance* player = PlayerInstance();
    if (!player || !g_DyeUnlock) return "ERR karakter yüklü değil";
    int unlocked = 0, already = 0;
    for (int c = 1; c <= 5; c++)
        for (int n = 1; n <= 8; n++) {
            if (g_DyeIsUnlocked && gm::CallScript(g_DyeIsUnlocked, player, { Value(c), Value(n) }).Bool()) {
                already++;
                continue;
            }
            gm::CallScript(g_DyeUnlock, player, { Value(std::format("{}_{}", c, n)) });
            unlocked++;
        }
    int now = 0;
    if (g_DyeIsUnlocked)
        for (int c = 1; c <= 5; c++)
            for (int n = 1; n <= 8; n++) now += gm::CallScript(g_DyeIsUnlocked, player, { Value(c), Value(n) }).Bool();
    return std::format("OK boyalar: {} açıldı, {} zaten açıktı; şimdi {}/40 açık", unlocked, already, now);
}

std::string UnlockGambler() {
    CInstance* player = PlayerInstance();
    if (!player || !g_GamblerUnlock) return "ERR karakter yüklü değil";
    gm::CallScript(g_GamblerUnlock, player, {});
    return "OK gambler açıldı";
}

std::string UnlockWaypoints() {
    CInstance* player = PlayerInstance();
    const int id = PlayerId();
    Value all = Builtin("variable_global_get", { Value("waypoint") });
    if (!player || id < 0 || all.Kind() != gm::KIND_ARRAY || !g_WaypointUnlock) return "ERR karakter yüklü değil";
    Value map = ContainerGet(all, DsKind::None, id);
    if (DsKindOf(map) != DsKind::Map) return "ERR waypoint tablosu bulunamadı";
    std::vector<Value> areas;
    for (Value key = Builtin("ds_map_find_first", { map }); !key.IsUndefined(); key = Builtin("ds_map_find_next", { map, key }))
        if (!Builtin("ds_map_find_value", { map, key }).Bool()) areas.push_back(key);
    for (const Value& area : areas) gm::CallScript(g_WaypointUnlock, player, { area });
    int open = 0, total = 0;
    for (Value key = Builtin("ds_map_find_first", { map }); !key.IsUndefined(); key = Builtin("ds_map_find_next", { map, key })) {
        total++;
        open += Builtin("ds_map_find_value", { map, key }).Bool();
    }
    return std::format("OK {} waypoint açıldı; şimdi {}/{} açık", areas.size(), open, total);
}

double g_RevealedArea = -1;  // global worldlocation already revealed
ULONGLONG g_AreaSince = 0;
double g_SeenArea = -1;

std::string HitsText(const std::vector<Hit>& Hits) {
    if (Hits.empty()) return "  (kayıt yok)";
    std::string out;
    const ULONGLONG last = Hits.back().Time;
    for (const Hit& hit : Hits)
        out += std::format("\n  -{:.1f}s  {} hasar  {}  (kalan can {})", (last - hit.Time) / 1000.0, Num(std::round(hit.Amount)), hit.Source, Num(std::round(hit.HpAfter)));
    return out;
}

}  // namespace

bool HandlesCommand(const std::string& Verb) { return Verb == "dps" || Verb == "recap"; }

std::string Command(const std::vector<std::string>& Words) {
    if (Words[0] == "dps") {
        if (Words.size() > 1 && Words[1] == "reset") {
            g_Dealt.clear();
            g_DealtTotal = 0;
            g_DealtSince = 0;
            return "OK hasar ölçer sıfırlandı";
        }
        const double seconds = g_DealtSince ? (GetTickCount64() - g_DealtSince) / 1000.0 : 0;
        return std::format("OK DPS (son 5 sn) {}  toplam {}  süre {:.0f} sn  ortalama {}", Num(std::round(Dps())), Num(std::round(g_DealtTotal)),
                           seconds, Num(seconds > 0 ? std::round(g_DealtTotal / seconds) : 0));
    }
    return std::format("OK ölüm sayısı {}\nson ölüm:{}\nşu anki son vuruşlar:{}", g_Deaths, HitsText(g_LastDeath), HitsText(g_Taken));
}

// Per-frame work from the Draw GUI hook (main thread, in the world).
void Frame() {
    const Value player = FindPlayer();
    g_PlayerRef = player.IsUndefined() ? -1 : Builtin("real", { player }).Number();
    if (Enabled(S_DPS_METER)) menu::DrawDps(Dps(), g_DealtTotal);
    ApplyGameSpeed();
    stats::SetFeatureAdd("pickup_rad", SettingValue(S_LOOT_RADIUS));
    if (Enabled(S_MAP_REVEAL) && PlayerInstance()) {
        const Value location = Builtin("variable_global_get", { Value("worldlocation") });
        const double area = location.IsNumber() ? location.Number() : -1;
        const ULONGLONG now = GetTickCount64();
        if (area != g_SeenArea) {
            g_SeenArea = area;
            g_AreaSince = now;
        }
        if (area != g_RevealedArea && now - g_AreaSince > 1000) {
            g_RevealedArea = area;
            RevealMap();
        }
    } else
        g_RevealedArea = -1;
}

void Install() {
    Hook("gml_Script_actorDamage", &HookDamage, g_OrigDamage);
    Hook("gml_Script_skillGetCooldown", &HookCooldown, g_OrigCooldown);
    Hook("gml_Script_skillGetCost", &HookCost, g_OrigCost);

    g_EnchantMax = gm::FindScript("gml_Script_enchantGetMax");
    Hook("gml_Script_enchantGetMin", &HookEnchantMin, g_OrigEnchantMin);
    Hook("gml_Script_itemRollQuality", &HookQuality, g_OrigQuality);
    Hook("gml_Script_itemGenerate", &HookGenerate, g_OrigGenerate);
    Hook("gml_Script_currencyDeduct", &HookDeduct, g_OrigDeduct);
    Hook("gml_Script_chestRollTrap", &HookChestTrap, g_OrigChestTrap);

    Hook(kRewardScripts[0], &HookReward<0>, g_OrigRewards[0]);
    Hook(kRewardScripts[1], &HookReward<1>, g_OrigRewards[1]);
    Hook(kRewardScripts[2], &HookReward<2>, g_OrigRewards[2]);

    Hook("gml_Script_dunSpawnEnemies", &HookSpawnEnemies, g_OrigSpawnEnemies);
    Hook("gml_Object_monsterSquad_Step_0", &HookSquadStep, g_OrigSquadStep);
    g_MinimapExplore = gm::FindScript("gml_Script_minimapExplore");
    Hook("gml_Script_camSetConfig", &HookCamConfig, g_OrigCamConfig);

    Hook("gml_Script_playerLevelUp", &HookLevelUp, g_OrigLevelUp);
    g_DyeUnlock = gm::FindScript("gml_Script_playerdyeUnlock");
    g_DyeIsUnlocked = gm::FindScript("gml_Script_playerdyeIsUnlocked");
    g_GamblerUnlock = gm::FindScript("gml_Script_gamblerUnlock");
    g_WaypointUnlock = gm::FindScript("gml_Script_waypointUnlock");
    SetAction(S_UNLOCK_DYES, &UnlockDyes);
    SetAction(S_UNLOCK_GAMBLER, &UnlockGambler);
    SetAction(S_UNLOCK_WAYPOINTS, &UnlockWaypoints);
}

}  // namespace cp::features
