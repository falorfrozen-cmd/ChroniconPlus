// Playing from code for tests: hits on enemies sent through the game's own actorDamage (attacker = the player), and
// keeping the player alive without the god mode flag, so damage taken is still processed.
//   fight <count> [damage]   the next <count> living enemies that step take one hit each
//   keepalive on|off         refill the player's hp below half
//   approach                 move the player next to the nearest living enemy (the game deactivates actors outside
//                            the view, so they are activated for this one lookup)
// gml_Object_actor_Step_0 runs for every actor (player and monsters inherit it); its self is that actor.
#include "Core.hpp"

#include <format>

namespace cp::autotest {
namespace {

gm::EventFn g_OrigActorStep = nullptr;
gm::ScriptFn g_ActorDamage = nullptr;  // the hooked entry, so the mod's own damage features apply
int g_PendingHits = 0;
double g_HitDamage = 50;
int g_HitsDone = 0;
bool g_KeepAlive = false;
Value g_EnemyObject;

bool IsLivingEnemy(CInstance* Actor) {
    const Value self(Actor);
    const Value object = Builtin("variable_instance_get", { self, Value("object_index") });
    if (!Builtin("object_is_ancestor", { object, g_EnemyObject }).Bool()) return false;
    const Value hp = Builtin("variable_instance_get", { self, Value("hp") });
    return hp.IsNumber() && hp.Number() > 0;
}

void HookActorStep(CInstance* Self, CInstance* Other) {
    g_OrigActorStep(Self, Other);
    if (g_PendingHits <= 0 || !Self || Self == PlayerInstance() || !g_ActorDamage) return;
    if (g_EnemyObject.IsUndefined()) g_EnemyObject = Builtin("asset_get_index", { Value("enemy") });
    if (!IsLivingEnemy(Self)) return;
    const Value player = FindPlayer();
    if (player.IsUndefined()) return;
    g_PendingHits--;
    g_HitsDone++;
    const Value self(Self);
    const double before = Builtin("variable_instance_get", { self, Value("hp") }).Number();
    const std::string name = Builtin("object_get_name", { Builtin("variable_instance_get", { self, Value("object_index") }) }).Text();
    // actorDamage(dmg, crit, element, trigger, attacker, skill)
    gm::CallScript(g_ActorDamage, Self, { Value(g_HitDamage), Value(false), Value(0), Value(false), player, Value(-1) });
    const Value after = Builtin("variable_instance_get", { self, Value("hp") });
    Log(std::format("[fight] hit {} {}: hp {} -> {}", g_HitsDone, name, Num(before), after.Describe()));
}

}  // namespace

void Install() {
    g_ActorDamage = gm::FindScript("gml_Script_actorDamage");
    Hook("gml_Object_actor_Step_0", &HookActorStep, g_OrigActorStep);
}

void Frame() {
    if (!g_KeepAlive) return;
    const Value player = FindPlayer();
    if (player.IsUndefined()) return;
    const Value hp = Builtin("variable_instance_get", { player, Value("hp") });
    const Value max_hp = Builtin("variable_instance_get", { player, Value("maxhp") });
    if (hp.IsNumber() && max_hp.IsNumber() && hp.Number() < max_hp.Number() / 2)
        Builtin("variable_instance_set", { player, Value("hp"), max_hp });
}

std::string Approach() {
    const Value player = FindPlayer();
    if (player.IsUndefined()) return "ERR oyuncu yok";
    if (g_EnemyObject.IsUndefined()) g_EnemyObject = Builtin("asset_get_index", { Value("enemy") });
    Builtin("instance_activate_all");
    const double px = Builtin("variable_instance_get", { player, Value("x") }).Number();
    const double py = Builtin("variable_instance_get", { player, Value("y") }).Number();
    const int count = static_cast<int>(Builtin("instance_number", { g_EnemyObject }).Number());
    double best = 1e18, bx = 0, by = 0;
    int living = 0;
    for (int i = 0; i < count; i++) {
        const Value enemy = Builtin("instance_find", { g_EnemyObject, Value(i) });
        const Value hp = Builtin("variable_instance_get", { enemy, Value("hp") });
        if (!hp.IsNumber() || hp.Number() <= 0) continue;
        living++;
        const double x = Builtin("variable_instance_get", { enemy, Value("x") }).Number();
        const double y = Builtin("variable_instance_get", { enemy, Value("y") }).Number();
        const double d = (x - px) * (x - px) + (y - py) * (y - py);
        if (d < best) best = d, bx = x, by = y;
    }
    if (!living) return std::format("OK bu bölgede canlı düşman yok ({} düşman nesnesi)", count);
    Builtin("variable_instance_set", { player, Value("x"), Value(bx + 48) });
    Builtin("variable_instance_set", { player, Value("y"), Value(by) });
    return std::format("OK {} canlı düşman; oyuncu ({}, {}) noktasına taşındı", living, Num(bx + 48), Num(by));
}

bool HandlesCommand(const std::string& Verb) { return Verb == "fight" || Verb == "keepalive" || Verb == "approach"; }

std::string Command(const std::vector<std::string>& Words) {
    if (Words[0] == "approach") return Approach();
    if (Words[0] == "keepalive") {
        g_KeepAlive = Words.size() > 1 && (Words[1] == "on" || Words[1] == "1");
        return std::string("OK keepalive ") + (g_KeepAlive ? "açık" : "kapalı");
    }
    try {
        g_PendingHits = Words.size() > 1 ? std::stoi(Words[1]) : 1;
        if (Words.size() > 2) g_HitDamage = std::stod(Words[2]);
    } catch (...) {
        return "ERR kullanım: fight <adet> [hasar]";
    }
    return std::format("OK sıradaki {} düşmana {} hasar uygulanacak (şimdiye kadar {} vuruş)", g_PendingHits, Num(g_HitDamage), g_HitsDone);
}

}  // namespace cp::autotest
