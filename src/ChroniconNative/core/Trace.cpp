// Live script tracing: "trace <script>" hooks any gml_Script_* while the game runs and logs each call (self, arguments,
// result) to ChroniconPlus.log; "untrace <script>" removes it. A fixed pool of detours, one per slot, since a detour
// cannot carry its own state.
#include "Core.hpp"
#include "hook.hpp"

#include <array>
#include <format>

namespace cp::trace {
namespace {

constexpr int kSlots = 32;

struct Slot {
    std::string Name;
    void* Target = nullptr;
    gm::ScriptFn Original = nullptr;
    int Remaining = 0;  // calls still to log
    uint64_t Calls = 0;
};
std::array<Slot, kSlots> g_Slots;
int g_Depth = 0;

template <int N>
gm::RValue& Detour(CInstance* Self, CInstance* Other, gm::RValue& Result, int ArgumentCount, gm::RValue** Arguments) {
    Slot& slot = g_Slots[N];
    slot.Calls++;
    const bool log = slot.Remaining > 0;
    std::string line;
    if (log) {
        slot.Remaining--;
        line = std::format("[trace] {}{}(", std::string(g_Depth * 2, ' '), slot.Name);
        for (int i = 0; i < ArgumentCount; i++) line += (i ? ", " : "") + RawDescribe(Arguments[i]);
        line += std::format(") self={}", static_cast<void*>(Self));
    }
    g_Depth++;
    gm::RValue& result = slot.Original(Self, Other, Result, ArgumentCount, Arguments);
    g_Depth--;
    if (log) Log(line + " -> " + RawDescribe(&result));
    return result;
}

template <int... N>
constexpr std::array<gm::ScriptFn, sizeof...(N)> MakeDetours(std::integer_sequence<int, N...>) {
    return { &Detour<N>... };
}
const auto kDetours = MakeDetours(std::make_integer_sequence<int, kSlots>{});

std::string Qualified(const std::string& Name) {
    return Name.rfind("gml_", 0) == 0 ? Name : "gml_Script_" + Name;
}

}  // namespace

bool HandlesCommand(const std::string& Verb) { return Verb == "trace" || Verb == "untrace"; }

std::string Command(const std::vector<std::string>& Words) {
    if (Words.size() < 2) {
        std::string out = "OK izlenenler:";
        for (const Slot& slot : g_Slots)
            if (slot.Target) out += std::format("\n  {}  {} çağrı, {} log hakkı kaldı", slot.Name, slot.Calls, slot.Remaining);
        return out;
    }
    const std::string name = Qualified(Words[1]);
    Slot* existing = nullptr;
    for (Slot& slot : g_Slots)
        if (slot.Target && slot.Name == name) existing = &slot;

    if (Words[0] == "untrace") {
        if (!existing) return "ERR izlenmiyor: " + name;
        std::string why;
        if (!hook::Remove(existing->Target, &why)) return "ERR kaldırılamadı: " + why;
        *existing = {};
        return "OK " + name + " izlemesi kaldırıldı";
    }

    int count = 50;
    if (Words.size() > 2) {
        try { count = std::stoi(Words[2]); }
        catch (...) { return "ERR sayı bekleniyordu: " + Words[2]; }
    }
    if (existing) {
        existing->Remaining = count;
        return std::format("OK {} zaten izleniyor; sonraki {} çağrı loglanacak", name, count);
    }
    void* target = gm::FindFunction(name);
    if (!target || name.rfind("gml_Script_", 0) != 0) return "ERR script bulunamadı: " + name;
    for (int i = 0; i < kSlots; i++) {
        Slot& slot = g_Slots[i];
        if (slot.Target) continue;
        std::string why;
        if (!hook::Install(target, reinterpret_cast<void*>(kDetours[i]), reinterpret_cast<void**>(&slot.Original), &why))
            return "ERR hook kurulamadı: " + why;
        slot.Name = name;
        slot.Target = target;
        slot.Remaining = count;
        return std::format("OK {} izleniyor; sonraki {} çağrı ChroniconPlus.log'a yazılacak", name, count);
    }
    return "ERR boş izleme yuvası yok (untrace ile birini kaldır)";
}

}  // namespace cp::trace
