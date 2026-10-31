// Command channel: a client writes "<nonce>\n<command>" to ipc\cmd.txt (via a temp file and rename), the game
// thread answers "<nonce>\n<reply>" in ipc\reply.txt. Lets the mod be driven and checked from code.
#include "Core.hpp"
#include "Menu.hpp"
#include "Session.hpp"
#include "Stats.hpp"
#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>

namespace cp
{
    namespace
    {
        fs::path g_IpcFolder;

        std::vector<std::string> Split(const std::string& Line)
        {
            std::vector<std::string> words;
            std::istringstream in(Line);
            for (std::string word; in >> word;)
                words.push_back(word);
            return words;
        }

        std::string Help()
        {
            return
                "OK komutlar:\n"
                "  status                       tüm ayarlar ve durum\n"
                "  get <ayar> | set <ayar> <sayı>   ayarlar: exp currency loot godmode mana nocd\n"
                "  stats [filtre|bonus]          stat listesi (alias, tür, değer, cap, bonus, ad)\n"
                "  stat <alias|index> [add|mul|set|cap <sayı|off> | reset]\n"
                "  statreset                    tüm stat bonuslarını kaldır\n"
                "  recalc                       oyunun stat hesaplamasını sıraya al\n"
                "  global <ad> [sayı]           global değişken oku / yaz\n"
                "  pvar <ad> [sayı]             oyuncu değişkeni oku / yaz\n"
                "  inspect <global|p:ad> [i j..] dizi / ds içeriğine bak (salt okunur)\n"
                "  load [kayıt|recent]          ana menüden karakter yükle\n"
                "  instances <obje> [değişken..] | ivar <id> <değişken> [sayı]\n"
                "  trace <script> [adet] | untrace <script> | trace   script çağrılarını logla\n"
                "  do <eylem>                   dyes, gambler, waypoints\n"
                "  call <script> [@player|@global] [argümanlar]   bir script'i çağır (dikkat: yanlış argüman oyunu düşürür)";
        }

        std::string Status()
        {
            std::string out = "OK ChroniconPlus\n";
            for (const Setting& item : Settings())
                out += std::format("  {:<9} {}\n", item.Key, ValueText(item));
            const bool player = PlayerInstance() != nullptr;
            out += std::format("  oyuncu    {}\n", player ? "yüklü" : "yok");
            if (player)
                out += std::format("  statlar   {} satır", stats::RowCount());
            return out;
        }

        std::string SettingCommand(const std::vector<std::string>& Words)
        {
            if (Words.size() < 2)
                return "ERR ayar adı eksik";
            Setting* item = FindSetting(Words[1]);
            if (!item)
                return "ERR böyle bir ayar yok: " + Words[1];
            // Never treat a read or a value restore as an instruction to unlock progression.
            if (Words[0] == "do" || item->Kind == SettingKind::Action)
            {
                if (item->Kind != SettingKind::Action || !item->Run)
                    return std::string("ERR ") + item->Key + " bir eylem değil";
                if (Words[0] != "do")
                    return std::string("ERR action requires explicit do: ") + item->Key;
                return item->Run();
            }
            if (Words[0] == "set")
            {
                if (Words.size() < 3)
                    return "ERR değer eksik";
                double value = 0;
                if (Words[2] == "on" || Words[2] == "ac")
                    value = 1;
                else if (Words[2] == "off" || Words[2] == "kapat")
                    value = 0;
                else
                {
                    try { value = std::stod(Words[2]); }
                    catch (...) { return "ERR sayı bekleniyordu: " + Words[2]; }
                }
                SetValue(*item, value);
            }
            return std::format("OK {} = {} ({}..{}, varsayılan {})", item->Key, Num(item->Value), Num(item->Minimum), Num(item->Maximum), Num(item->Default));
        }

        // call <script> [@player|@global] [args...]: numbers become reals, anything else a string.
        std::string CallCommand(const std::vector<std::string>& Words)
        {
            if (Words.size() < 2)
                return "ERR kullanım: call <script> [@player|@global] [argümanlar]";
            const std::string name = Words[1].rfind("gml_", 0) == 0 ? Words[1] : "gml_Script_" + Words[1];
            gm::ScriptFn script = gm::FindScript(name);
            const bool builtin = !script && gm::FindBuiltin(Words[1]);
            if (!script && !builtin)
                return "ERR script ya da built-in bulunamadı: " + Words[1];
            size_t first = 2;
            CInstance* self = PlayerInstance();
            if (Words.size() > 2 && Words[2][0] == '@')
            {
                self = Words[2] == "@global" ? gm::Global() : PlayerInstance();
                first = 3;
            }
            if (!self)
                self = gm::Global();
            std::vector<Value> args;
            for (size_t i = first; i < Words.size(); i++)
            {
                try
                {
                    size_t used = 0;
                    const double number = std::stod(Words[i], &used);
                    args.push_back(used == Words[i].size() ? Value(number) : Value(Words[i]));
                }
                catch (...) { args.push_back(Value(Words[i])); }
            }
            auto run = [&](std::initializer_list<Value> list) {
                return builtin ? gm::Call(Words[1].c_str(), list) : gm::CallScript(script, self, list);
            };
            Value result;
            switch (args.size())
            {
            case 0: result = run({}); break;
            case 1: result = run({ args[0] }); break;
            case 2: result = run({ args[0], args[1] }); break;
            case 3: result = run({ args[0], args[1], args[2] }); break;
            case 4: result = run({ args[0], args[1], args[2], args[3] }); break;
            default: return "ERR en fazla 4 argüman";
            }
            return "OK " + (builtin ? Words[1] : name) + " -> " + result.Describe();
        }

        std::string VariableCommand(const std::vector<std::string>& Words)
        {
            if (Words.size() < 2)
                return "ERR değişken adı eksik";
            const bool global = Words[0] == "global";
            Value player;
            if (!global)
            {
                player = FindPlayer();
                if (player.IsUndefined())
                    return "ERR oyuncu yok";
            }

            if (Words.size() > 2)
            {
                double value = 0;
                try { value = std::stod(Words[2]); }
                catch (...) { return "ERR sayı bekleniyordu: " + Words[2]; }
                if (global)
                    Builtin("variable_global_set", { Value(Words[1]), Value(value) });
                else
                    Builtin("variable_instance_set", { player, Value(Words[1]), Value(value) });
            }

            Value value = global
                ? Builtin("variable_global_get", { Value(Words[1]) })
                : Builtin("variable_instance_get", { player, Value(Words[1]) });
            return std::format("OK {} = {}", Words[1], value.Describe());
        }

        // Read-only look into a global (or p:<player variable>), following array indexes; numbers that are
        // live ds_map / ds_list / ds_grid ids are summarised too.
        std::string Inspect(const std::vector<std::string>& Words)
        {
            if (Words.size() < 2)
                return "ERR ad eksik";
            Value value;
            if (Words[1].rfind("p:", 0) == 0)
            {
                Value player = FindPlayer();
                if (player.IsUndefined())
                    return "ERR oyuncu yok";
                value = Builtin("variable_instance_get", { player, Value(Words[1].substr(2)) });
            }
            else
                value = Builtin("variable_global_get", { Value(Words[1]) });

            // Each further word indexes into an array / ds_list / 1-D ds_grid, or "k:<key>" into a ds_map.
            std::string path = Words[1];
            for (size_t w = 2; w < Words.size(); w++)
            {
                const DsKind ds = DsKindOf(value);
                if (Words[w].rfind("k:", 0) == 0)
                {
                    if (ds != DsKind::Map)
                        return std::format("ERR {} bir ds_map değil ({})", path, value.Describe());
                    value = Builtin("ds_map_find_value", { value, Value(Words[w].substr(2)) });
                    path += "[?" + Words[w].substr(2) + "]";
                    continue;
                }
                size_t index = 0;
                try { index = std::stoul(Words[w]); }
                catch (...) { return "ERR index sayı ya da k:<anahtar> olmalı: " + Words[w]; }
                if (value.Kind() != gm::KIND_ARRAY && ds != DsKind::List && ds != DsKind::Grid)
                    return std::format("ERR {} indekslenemez ({})", path, value.Describe());
                if (index >= ContainerSize(value, ds))
                    return std::format("ERR {} sınır dışı: {}", path, index);
                value = ContainerGet(value, ds, index);
                path += std::format("[{}]", index);
            }

            const DsKind ds = DsKindOf(value);
            std::string out = std::format("OK {} = {}{}", path, value.Describe(), ds == DsKind::None ? "" : std::string(" ") + DsKindName(ds));
            constexpr size_t kShown = 60;
            if (value.Kind() == gm::KIND_ARRAY || ds == DsKind::List)
            {
                const size_t size = ContainerSize(value, ds);
                out += std::format(" boyut {}", size);
                for (size_t i = 0; i < (std::min)(size, kShown); i++)
                {
                    Value element = ContainerGet(value, ds, i);
                    out += std::format("\n  [{}] {}", i, element.Describe());
                }
            }
            else if (ds == DsKind::Map)
            {
                out += std::format(" boyut {}", ContainerSize(value, ds));
                Value key = Builtin("ds_map_find_first", { value });
                for (size_t i = 0; i < kShown && !key.IsUndefined(); i++)
                {
                    Value element = Builtin("ds_map_find_value", { value, key });
                    out += std::format("\n  {} = {}", key.Describe(), element.Describe());
                    key = Builtin("ds_map_find_next", { value, key });
                }
            }
            else if (ds == DsKind::Grid)
            {
                const int width = static_cast<int>(Builtin("ds_grid_width", { value }).Number());
                const int height = static_cast<int>(Builtin("ds_grid_height", { value }).Number());
                out += std::format(" {}x{}", width, height);
                size_t shown = 0;
                for (int y = 0; y < height && shown < kShown; y++)
                    for (int x = 0; x < width && shown < kShown; x++, shown++)
                    {
                        Value element = Builtin("ds_grid_get", { value, Value(static_cast<double>(x)), Value(static_cast<double>(y)) });
                        out += std::format("\n  ({},{}) {}", x, y, element.Describe());
                    }
            }
            return out;
        }
    }

    void CommandsInit(const fs::path& Folder)
    {
        g_IpcFolder = Folder / "ipc";
        std::error_code error;
        fs::create_directories(g_IpcFolder, error);
        fs::remove(g_IpcFolder / "cmd.txt", error);
        fs::remove(g_IpcFolder / "reply.txt", error);
    }

    std::string RunCommand(const std::string& Line)
    {
        const std::vector<std::string> words = Split(Line);
        if (words.empty() || words[0] == "help")
            return Help();
        const std::string& verb = words[0];
        if (verb == "status")
            return Status();
        if (verb == "get" || verb == "set")
            return SettingCommand(words);
        if (verb == "global" || verb == "pvar")
            return VariableCommand(words);
        if (verb == "inspect")
            return Inspect(words);
        if (verb == "objtest")  // an instance pointer passed as a GML value must read like the instance id does
        {
            CInstance* player = PlayerInstance();
            if (!player)
                return "ERR oyuncu yok";
            return std::format("OK pointer ile maxhp={}, id ile maxhp={}",
                Builtin("variable_instance_get", { Value(player), Value("maxhp") }).Describe(),
                Builtin("variable_instance_get", { FindPlayer(), Value("maxhp") }).Describe());
        }
        if (autotest::HandlesCommand(verb))
            return autotest::Command(words);
        if (features::HandlesCommand(verb))
            return features::Command(words);
        if (verb == "do")
            return SettingCommand(words);
        if (verb == "call")
            return CallCommand(words);
        if (verb == "menu" || verb == "shot")
            return menu::Command(words);
        if (trace::HandlesCommand(verb))
            return trace::Command(words);
        if (session::HandlesCommand(verb))
            return session::Command(words);
        if (stats::HandlesCommand(verb))
            return stats::Command(words);
        return "ERR bilinmeyen komut: " + verb + " (help)";
    }

    void PollCommands()
    {
        if (g_IpcFolder.empty())
            return;

        const fs::path command_file = g_IpcFolder / "cmd.txt";
        if (GetFileAttributesW(command_file.c_str()) == INVALID_FILE_ATTRIBUTES)
            return;

        std::string nonce, line;
        {
            std::ifstream in(command_file);
            std::getline(in, nonce);
            std::getline(in, line);
        }
        std::error_code error;
        fs::remove(command_file, error);
        if (!nonce.empty() && nonce.back() == '\r')
            nonce.pop_back();
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        std::string reply;
        try { reply = RunCommand(line); }
        catch (const std::exception& e) { reply = std::string("ERR ") + e.what(); }
        Log("[cmd] " + line + " -> " + reply.substr(0, reply.find('\n')));

        const fs::path temporary = g_IpcFolder / "reply.tmp";
        {
            std::ofstream out(temporary, std::ios::trunc | std::ios::binary);
            out << nonce << '\n' << reply;
        }
        MoveFileExW(temporary.c_str(), (g_IpcFolder / "reply.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}
