#include "Stats.hpp"
#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <unordered_map>

namespace cp::stats
{
    namespace
    {
        struct CoreDef { const char* Alias; const char* Variable; const char* Name; };

        // Player instance variables derived by gearRecalculate and shown on the stat screen.
        const CoreDef kCore[] = {
            { "maxhp",       "maxhp",       "Maksimum can" },
            { "maxmana",     "maxmana",     "Maksimum mana" },
            { "dmg",         "dmg",         "Hasar" },
            { "hpreg",       "hpreg",       "Can yenilenmesi" },
            { "manareg",     "manareg",     "Mana yenilenmesi" },
            { "attackspeed", "attackspeed", "Saldırı hızı" },
            { "thorns",      "thorns",      "Diken hasarı" },
            { "mf",          "mf",          "Magic find" },
        };

        struct RowDef
        {
            std::string Alias;
            std::string Name;
            const char* Variable = nullptr; // core rows
            int Index = -1;                 // playerstats rows
        };

        gm::ScriptFn g_OrigRecalc = nullptr;
        gm::ScriptFn g_OrigStatSet = nullptr;
        gm::ScriptFn g_OrigStatGetCap = nullptr;
        gm::ScriptFn g_StatGet = nullptr;
        gm::ScriptFn g_QueueRecalc = nullptr;

        int g_RecalcDepth = 0;
        bool g_RecalcSeen = false;
        uint64_t g_RecalcCount = 0;
        std::set<int> g_Computed;

        bool g_Discovered = false;
        std::vector<int> g_StatKeys; // stat numbers present in playerstats[player]
        std::vector<RowDef> g_Rows;
        std::unordered_map<std::string, size_t> g_RowByAlias;
        std::unordered_map<int, size_t> g_RowByIndex;

        std::map<std::string, Bonus> g_Bonuses;
        std::map<std::string, double> g_FeatureAdd;  // flat additions owned by other features (e.g. loot radius)
        std::unordered_map<int, double> g_CapOverrides;

        // What the game computed before our bonus and what we wrote, per alias. If a value still equals what
        // we wrote, the recalculation did not touch it, so the bonus starts from the remembered base again
        // instead of stacking.
        struct Applied { double Base; double Written; };
        std::unordered_map<std::string, Applied> g_Applied;

        // ------------------------------------------------------------ helpers

        std::string Lower(std::string Text)
        {
            std::transform(Text.begin(), Text.end(), Text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return Text;
        }

        // True when global.<Table>[player][Index] exists and holds a number. The game's stat scripts do
        // arithmetic on the entry without checking it, so a missing entry is a fatal GML error: always ask first.
        bool EntryPresent(const char* Table, int Index)
        {
            const int player = PlayerId();
            if (player < 0 || Index < 0)
                return false;
            Value all = Builtin("variable_global_get", { Value(Table) });
            if (all.Kind() != gm::KIND_ARRAY || player >= static_cast<int>(ContainerSize(all, DsKind::None)))
                return false;
            Value table = ContainerGet(all, DsKind::None, player);
            const DsKind kind = DsKindOf(table);
            Value entry;
            if (kind == DsKind::Map)
            {
                if (!Builtin("ds_map_exists", { table, Value(Index) }).Bool())
                    return false;
                entry = Builtin("ds_map_find_value", { table, Value(Index) });
            }
            else if (static_cast<size_t>(Index) < ContainerSize(table, kind))
                entry = ContainerGet(table, kind, Index);
            return entry.IsNumber();
        }

        bool ValidStat(int Index)
        {
            return EntryPresent("playerstats", Index);
        }

        struct StatKey { int Index; const char* Key; };
        const StatKey kStatKeys[] = {
#include "StatKeys.inc"
        };

        bool Discover()
        {
            if (g_Discovered)
                return true;

            // global.playerstats[player ID] is a ds_map of stat number -> obfuscated value in this build (arrays and
            // lists are accepted too); every stat script takes the player ID first.
            const int player = PlayerId();
            Value all = Builtin("variable_global_get", { Value("playerstats") });
            if (player < 0 || all.Kind() != gm::KIND_ARRAY)
                return false;
            Value stats = ContainerGet(all, DsKind::None, player);
            const DsKind stats_kind = DsKindOf(stats);
            g_StatKeys.clear();
            if (stats_kind == DsKind::Map)
            {
                Value key = Builtin("ds_map_find_first", { stats });
                for (int guard = 0; !key.IsUndefined() && guard < 10000; guard++)
                {
                    if (key.IsNumber())
                        g_StatKeys.push_back(key.Int());
                    key = Builtin("ds_map_find_next", { stats, key });
                }
                std::sort(g_StatKeys.begin(), g_StatKeys.end());
            }
            else
                for (size_t i = 0; i < ContainerSize(stats, stats_kind); i++)
                    g_StatKeys.push_back(static_cast<int>(i));

            if (g_StatKeys.size() < 20)
            {
                static bool logged = false;
                if (!logged)
                    Log(std::format("[stats] unexpected playerstats[{}] = {} {} size {}: stat editor stays off", player, stats.Describe(), DsKindName(stats_kind), g_StatKeys.size()));
                logged = true;
                return false;
            }

            // Display names: global.stattext is a ds_map "stat_<key>" -> name in the current language.
            Value names = Builtin("variable_global_get", { Value("stattext") });
            const bool names_map = DsKindOf(names) == DsKind::Map;
            auto display_name = [&](const std::string& Key)
            {
                if (names_map)
                {
                    Value name = Builtin("ds_map_find_value", { names, Value("stat_" + Key) });
                    if (name.Kind() == gm::KIND_STRING && !name.Text().empty())
                        return name.Text();
                }
                return Key;
            };

            g_Rows.clear();
            for (const CoreDef& core : kCore)
                g_Rows.push_back({ core.Alias, core.Name, core.Variable, -1 });

            const std::set<int> present(g_StatKeys.begin(), g_StatKeys.end());
            std::set<int> named;
            for (const StatKey& stat : kStatKeys)
            {
                if (!present.contains(stat.Index) || named.contains(stat.Index))
                    continue;
                named.insert(stat.Index);
                g_Rows.push_back({ stat.Key, display_name(stat.Key), nullptr, stat.Index });
            }
            for (const int index : g_StatKeys)
                if (!named.contains(index))
                    g_Rows.push_back({ std::format("s{}", index), std::format("Stat #{}", index), nullptr, index });

            g_RowByAlias.clear();
            g_RowByIndex.clear();
            for (size_t i = 0; i < g_Rows.size(); i++)
            {
                g_RowByAlias[g_Rows[i].Alias] = i;
                if (g_Rows[i].Index >= 0)
                    g_RowByIndex[g_Rows[i].Index] = i;
            }

            g_Discovered = true;
            Log(std::format("[stats] {} rows", g_Rows.size()));
            return true;
        }

        void RebuildCaps()
        {
            g_CapOverrides.clear();
            for (const auto& [alias, bonus] : g_Bonuses)
            {
                auto row = g_RowByAlias.find(alias);
                if (bonus.Cap && row != g_RowByAlias.end() && g_Rows[row->second].Index >= 0)
                    g_CapOverrides[g_Rows[row->second].Index] = *bonus.Cap;
            }
        }

        std::string Field(const std::optional<double>& Number) { return Number ? Num(*Number) : std::string(); }

        void SaveBonuses()
        {
            IniDeleteSection("Stats");
            for (const auto& [alias, bonus] : g_Bonuses)
                if (!bonus.Empty())
                    IniWrite("Stats", alias.c_str(), std::format("{}|{}|{}|{}", Num(bonus.Add), Num(bonus.Mul), Field(bonus.Set), Field(bonus.Cap)));
        }

        void LoadBonuses()
        {
            g_Bonuses.clear();
            for (const std::string& alias : IniKeys("Stats"))
            {
                std::vector<std::string> parts;
                std::string text = IniReadString("Stats", alias.c_str());
                for (size_t start = 0;;)
                {
                    const size_t bar = text.find('|', start);
                    parts.push_back(text.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
                    if (bar == std::string::npos)
                        break;
                    start = bar + 1;
                }
                parts.resize(4);
                auto number = [](const std::string& s) -> std::optional<double>
                {
                    if (s.empty())
                        return std::nullopt;
                    try { return std::stod(s); }
                    catch (...) { return std::nullopt; }
                };
                Bonus bonus;
                bonus.Add = number(parts[0]).value_or(0);
                bonus.Mul = number(parts[1]).value_or(1);
                bonus.Set = number(parts[2]);
                bonus.Cap = number(parts[3]);
                if (!bonus.Empty())
                    g_Bonuses[alias] = bonus;
            }
            Log(std::format("[stats] {} saved bonuses", g_Bonuses.size()));
        }

        CInstance* Self()
        {
            CInstance* player = PlayerInstance();
            return player ? player : gm::Global();
        }

        std::optional<double> StatGet(int Index)
        {
            if (!ValidStat(Index))
                return std::nullopt;
            Value value = CallScript(g_StatGet, Self(), { Value(PlayerId()), Value(Index) });
            if (!value.IsNumber())
                return std::nullopt;
            return value.Number();
        }

        void StatSet(int Index, double Amount)
        {
            if (ValidStat(Index))
                CallScript(g_OrigStatSet, Self(), { Value(PlayerId()), Value(Index), Value(Amount) });
        }

        std::optional<double> GameCap(int Index)
        {
            if (!EntryPresent("statCap", Index))
                return std::nullopt;
            Value value = CallScript(g_OrigStatGetCap, Self(), { Value(Index), Value(PlayerId()) });
            if (!value.IsNumber())
                return std::nullopt;
            return value.Number();
        }

        std::optional<double> VarGet(const char* Variable)
        {
            Value player = FindPlayer();
            if (player.IsUndefined())
                return std::nullopt;
            Value value = Builtin("variable_instance_get", { player, Value(Variable) });
            if (!value.IsNumber())
                return std::nullopt;
            return value.Number();
        }

        void VarSet(const char* Variable, double Amount)
        {
            Value player = FindPlayer();
            if (!player.IsUndefined())
                Builtin("variable_instance_set", { player, Value(Variable), Value(Amount) });
        }

        double Compose(double Base, const Bonus& B)
        {
            return B.Set ? *B.Set : Base * B.Mul + B.Add;
        }

        RowKind KindOfRow(const RowDef& Row)
        {
            if (Row.Variable)
                return RowKind::Core;
            if (!g_RecalcSeen)
                return RowKind::Unknown;
            return g_Computed.contains(Row.Index) ? RowKind::Computed : RowKind::Counter;
        }

        const char* KindName(RowKind Kind)
        {
            switch (Kind)
            {
            case RowKind::Core: return "core";
            case RowKind::Computed: return "stat";
            case RowKind::Counter: return "counter";
            default: return "unknown";
            }
        }

        std::optional<double> Current(const RowDef& Row)
        {
            return Row.Variable ? VarGet(Row.Variable) : StatGet(Row.Index);
        }

        void ApplyBonuses()
        {
            if ((g_Bonuses.empty() && g_FeatureAdd.empty()) || !Discover() || !PlayerInstance())
                return;

            std::map<std::string, Bonus> effective = g_Bonuses;
            for (const auto& [alias, add] : g_FeatureAdd)
                effective[alias].Add += add;

            for (const auto& [alias, bonus] : effective)
            {
                auto found = g_RowByAlias.find(alias);
                if (found == g_RowByAlias.end() || (!bonus.Set && bonus.Add == 0 && bonus.Mul == 1))
                    continue;
                const RowDef& row = g_Rows[found->second];
                if (KindOfRow(row) == RowKind::Counter)
                    continue;
                std::optional<double> value = Current(row);
                if (!value)
                    continue;

                double base = *value;
                if (auto previous = g_Applied.find(alias); previous != g_Applied.end() && previous->second.Written == *value)
                    base = previous->second.Base;
                const double result = Compose(base, bonus);
                if (row.Variable)
                    VarSet(row.Variable, result);
                else
                    StatSet(row.Index, result);
                // statSet may round or clamp; remember what the game actually holds now.
                g_Applied[alias] = { base, Current(row).value_or(result) };
            }
        }

        void QueueRecalc()
        {
            if (CInstance* player = PlayerInstance())
                CallScript(g_QueueRecalc, player, { Value(PlayerId()), Value(1) });  // queueGearRecalc(player, frames until it runs); the game passes 30
        }

        // ------------------------------------------------------------ hooks

        gm::RValue& HookRecalc(CInstance* Self, CInstance* Other, gm::RValue& Result, int ArgumentCount, gm::RValue** Arguments)
        {
            g_RecalcDepth++;
            gm::RValue& result = g_OrigRecalc(Self, Other, Result, ArgumentCount, Arguments);
            g_RecalcDepth--;
            if (g_RecalcDepth == 0)
            {
                if (!g_RecalcSeen)
                    Log(std::format("[stats] first gearRecalculate: {} stats set by the game", g_Computed.size()));
                g_RecalcSeen = true;
                g_RecalcCount++;
                ApplyBonuses();
            }
            return result;
        }

        gm::RValue& HookStatSet(CInstance* Self, CInstance* Other, gm::RValue& Result, int ArgumentCount, gm::RValue** Arguments)
        {
            // statSet(player, stat, value)
            if (g_RecalcDepth > 0 && ArgumentCount > 1 && Arguments[1] && RawIsNumber(Arguments[1]))
                g_Computed.insert(static_cast<int>(RawNumber(Arguments[1])));
            return g_OrigStatSet(Self, Other, Result, ArgumentCount, Arguments);
        }

        gm::RValue& HookStatGetCap(CInstance* Self, CInstance* Other, gm::RValue& Result, int ArgumentCount, gm::RValue** Arguments)
        {
            // statGetCap(stat, player): the reverse of statGet / statSet (it indexes global.statCap[player][stat]).
            if (!g_CapOverrides.empty() && ArgumentCount > 0 && Arguments[0] && RawIsNumber(Arguments[0]))
            {
                auto found = g_CapOverrides.find(static_cast<int>(RawNumber(Arguments[0])));
                if (found != g_CapOverrides.end())
                {
                    RawSetNumber(Result, found->second);
                    return Result;
                }
            }
            return g_OrigStatGetCap(Self, Other, Result, ArgumentCount, Arguments);
        }

        // ------------------------------------------------------------ edits

        const RowDef* FindRow(const std::string& Key)
        {
            if (!Discover())
                return nullptr;
            if (auto found = g_RowByAlias.find(Lower(Key)); found != g_RowByAlias.end())
                return &g_Rows[found->second];
            if (!Key.empty() && std::all_of(Key.begin(), Key.end(), ::isdigit))
                if (auto found = g_RowByIndex.find(std::stoi(Key)); found != g_RowByIndex.end())
                    return &g_Rows[found->second];
            return nullptr;
        }

        RowView ViewOf(const RowDef& Row)
        {
            RowView view{ Row.Alias, Row.Name, KindOfRow(Row), Current(Row), std::nullopt, {} };
            if (Row.Index >= 0)
                view.GameCap = GameCap(Row.Index);
            if (auto found = g_Bonuses.find(Row.Alias); found != g_Bonuses.end())
                view.Bonus = found->second;
            return view;
        }

        void StoreBonus(const std::string& Alias, const Bonus& NewBonus)
        {
            if (NewBonus.Empty())
            {
                g_Bonuses.erase(Alias);
                // Put back what the game had, in case the recalculation does not rebuild this value.
                auto applied = g_Applied.find(Alias);
                auto row = g_RowByAlias.find(Alias);
                if (applied != g_Applied.end() && row != g_RowByAlias.end() && Current(g_Rows[row->second]) == applied->second.Written)
                {
                    const RowDef& def = g_Rows[row->second];
                    def.Variable ? VarSet(def.Variable, applied->second.Base) : StatSet(def.Index, applied->second.Base);
                }
                if (applied != g_Applied.end())
                    g_Applied.erase(applied);
            }
            else
                g_Bonuses[Alias] = NewBonus;
            SaveBonuses();
            RebuildCaps();
            QueueRecalc();
        }

        // Applies "add/mul/set <value>" to a counter once: the game keeps the result in the save itself.
        std::string WriteCounterOnce(const RowDef& Row, const std::string& Mode, double Argument)
        {
            std::optional<double> value = StatGet(Row.Index);
            if (!value)
                return "ERR değer okunamadı";
            double result = Mode == "add" ? *value + Argument : Mode == "mul" ? *value * Argument : Argument;
            StatSet(Row.Index, result);
            return std::format("OK {} sayaç {} -> {} (tek seferlik yazıldı)", Row.Alias, Num(*value), Num(StatGet(Row.Index).value_or(result)));
        }

        std::string Edit(const RowDef& Row, const std::string& Mode, const std::string& Argument)
        {
            const RowKind kind = KindOfRow(Row);
            Bonus bonus;
            if (auto found = g_Bonuses.find(Row.Alias); found != g_Bonuses.end())
                bonus = found->second;

            if (Mode == "reset")
            {
                StoreBonus(Row.Alias, {});
                return "OK " + Row.Alias + " bonusu kaldırıldı";
            }

            const bool off = Argument == "off" || Argument == "kapat";
            double number = 0;
            if (!off)
            {
                try { number = std::stod(Argument); }
                catch (...) { return "ERR sayı bekleniyordu: " + Argument; }
            }

            if (Mode == "cap")
            {
                if (Row.Variable)
                    return "ERR cap yalnızca playerstats satırlarında var";
                bonus.Cap = off ? std::nullopt : std::optional<double>(number);
                StoreBonus(Row.Alias, bonus);
                return std::format("OK {} cap {}", Row.Alias, off ? std::string("oyunun değeri") : Num(number));
            }

            if (kind == RowKind::Unknown)
                return "ERR oyun statları henüz hesaplamadı: bir karakter yükle";
            if (kind == RowKind::Counter && !off)
            {
                if (Mode != "add" && Mode != "mul" && Mode != "set")
                    return "ERR bilinmeyen mod: " + Mode;
                return WriteCounterOnce(Row, Mode, number);
            }

            if (Mode == "add")
                bonus.Add = off ? 0 : number;
            else if (Mode == "mul")
                bonus.Mul = off ? 1 : number;
            else if (Mode == "set")
                bonus.Set = off ? std::nullopt : std::optional<double>(number);
            else
                return "ERR bilinmeyen mod: " + Mode + " (add, mul, set, cap, reset)";

            StoreBonus(Row.Alias, bonus);
            return std::format("OK {} {} {} (yeniden hesaplama sıraya alındı)", Row.Alias, Mode, off ? std::string("kapalı") : Num(number));
        }

        std::string Line(const RowView& View)
        {
            return std::format("{}\t{}\t{}\t{}\t{}\t{}", View.Alias, KindName(View.Kind), View.Current ? Num(*View.Current) : "-",
                View.GameCap ? Num(*View.GameCap) : "-", BonusText(View), View.Name);
        }
    }

    void Install()
    {
        LoadBonuses();
        g_StatGet = gm::FindScript("gml_Script_statGet");
        g_QueueRecalc = gm::FindScript("gml_Script_queueGearRecalc");
        if (!g_StatGet || !g_QueueRecalc)
            Log("[stats] statGet / queueGearRecalc not found: stat editor disabled");

        Hook("gml_Script_statSet", HookStatSet, g_OrigStatSet);
        Hook("gml_Script_statGetCap", HookStatGetCap, g_OrigStatGetCap);
        Hook("gml_Script_gearRecalculate", HookRecalc, g_OrigRecalc);
    }

    bool Ready()
    {
        return g_StatGet && g_OrigStatSet && g_OrigRecalc && PlayerInstance() && Discover();
    }

    size_t RowCount()
    {
        return Discover() ? g_Rows.size() : 0;
    }

    std::optional<RowView> Row(size_t Index)
    {
        if (!Discover() || Index >= g_Rows.size())
            return std::nullopt;
        return ViewOf(g_Rows[Index]);
    }

    std::optional<RowView> Metadata(size_t Index)
    {
        if (!Discover() || Index >= g_Rows.size()) return std::nullopt;
        const RowDef& row = g_Rows[Index];
        return RowView{ row.Alias, row.Name, KindOfRow(row), std::nullopt, std::nullopt, {} };
    }

    std::string BonusText(const RowView& View)
    {
        const Bonus& b = View.Bonus;
        std::string text;
        if (b.Set)
            text += "=" + Num(*b.Set);
        else
        {
            if (b.Mul != 1)
                text += "x" + Num(b.Mul);
            if (b.Add != 0)
                text += (text.empty() ? "" : " ") + std::string(b.Add > 0 ? "+" : "") + Num(b.Add);
        }
        if (b.Cap)
            text += (text.empty() ? "" : " ") + std::string("cap ") + (*b.Cap >= 1e8 ? std::string("yok") : Num(*b.Cap));
        return text;
    }

    std::string AdjustAdd(size_t Index, double Delta)
    {
        if (!Ready() || Index >= g_Rows.size())
            return "Karakter yüklü değil";
        const RowDef& row = g_Rows[Index];
        if (KindOfRow(row) == RowKind::Counter)
            return WriteCounterOnce(row, "add", Delta).substr(3);

        Bonus bonus;
        if (auto found = g_Bonuses.find(row.Alias); found != g_Bonuses.end())
            bonus = found->second;
        if (bonus.Set)
            bonus.Set = *bonus.Set + Delta;
        else
            bonus.Add += Delta;
        StoreBonus(row.Alias, bonus);
        return row.Name + ": " + BonusText(ViewOf(row));
    }

    std::string ToggleCap(size_t Index)
    {
        if (!Ready() || Index >= g_Rows.size())
            return "Karakter yüklü değil";
        const RowDef& row = g_Rows[Index];
        if (row.Variable)
            return "Bu satırın cap'i yok";
        Bonus bonus;
        if (auto found = g_Bonuses.find(row.Alias); found != g_Bonuses.end())
            bonus = found->second;
        bonus.Cap = bonus.Cap ? std::nullopt : std::optional<double>(1e9);
        StoreBonus(row.Alias, bonus);
        return row.Name + (bonus.Cap ? ": cap kaldırıldı" : ": oyunun cap'i");
    }

    std::string Reset(size_t Index)
    {
        if (!Discover() || Index >= g_Rows.size())
            return "Karakter yüklü değil";
        StoreBonus(g_Rows[Index].Alias, {});
        return g_Rows[Index].Name + ": sıfırlandı";
    }

    void SetFeatureAdd(const std::string& Alias, double Add)
    {
        const double previous = g_FeatureAdd.contains(Alias) ? g_FeatureAdd[Alias] : 0;
        if (previous == Add)
            return;
        if (Add == 0)
            g_FeatureAdd.erase(Alias);
        else
            g_FeatureAdd[Alias] = Add;
        QueueRecalc();
    }

    bool HandlesCommand(const std::string& Verb)
    {
        return Verb == "stats" || Verb == "stat" || Verb == "statreset" || Verb == "recalc";
    }

    std::string Command(const std::vector<std::string>& Words)
    {
        const std::string& verb = Words[0];
        if (verb == "statreset")
        {
            std::vector<std::string> aliases;
            for (const auto& [alias, bonus] : g_Bonuses)
                aliases.push_back(alias);
            for (const std::string& alias : aliases)
                StoreBonus(alias, {});
            SaveBonuses();
            return std::format("OK {} stat bonusu kaldırıldı", aliases.size());
        }

        if (!Ready())
            return "ERR karakter yüklü değil ya da stat tabloları bulunamadı";

        if (verb == "recalc")
        {
            QueueRecalc();
            return std::format("OK yeniden hesaplama sıraya alındı (şimdiye kadar {} hesaplama)", g_RecalcCount);
        }

        if (verb == "stats")
        {
            const std::string filter = Words.size() > 1 ? Lower(Words[1]) : "";
            std::string out = std::format("OK {} satır, {} hesaplama, {} oyun statı\nalias\ttür\tdeğer\tcap\tbonus\tad\n", g_Rows.size(), g_RecalcCount, g_Computed.size());
            for (const RowDef& row : g_Rows)
            {
                const bool has_bonus = g_Bonuses.contains(row.Alias);
                if (filter == "bonus" ? !has_bonus : !filter.empty() && Lower(row.Alias + " " + row.Name).find(filter) == std::string::npos)
                    continue;
                out += Line(ViewOf(row)) + "\n";
            }
            return out;
        }

        // stat <alias|index> [add|mul|set|cap <value|off> | reset]
        if (Words.size() < 2)
            return "ERR kullanım: stat <alias|index> [add|mul|set|cap <sayı|off> | reset]";
        const RowDef* row = FindRow(Words[1]);
        if (!row)
            return "ERR böyle bir stat yok: " + Words[1] + " (stats ile listele)";
        if (Words.size() == 2)
            return "OK alias\ttür\tdeğer\tcap\tbonus\tad\n" + Line(ViewOf(*row));
        return Edit(*row, Lower(Words[2]), Words.size() > 3 ? Words[3] : "");
    }
}
