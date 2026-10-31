// Presentation-only localization. Stable setting keys and gameplay data never change language.
#pragma once
#include "Core.hpp"
#include <algorithm>
#include <cwctype>
#include <string_view>

namespace cp::vault {
inline bool g_English = false;
inline bool g_LocaleLoaded = false;
inline void LoadLanguage() {
    if (g_LocaleLoaded) return;
    g_English = IniReadString("Interface", "Language") == "en";
    g_LocaleLoaded = true;
}
inline void SetLanguage(bool English) {
    LoadLanguage();
    g_English = English;
    IniWrite("Interface", "Language", English ? "en" : "tr");
}
inline const char* L(const char* Tr, const char* En) { LoadLanguage(); return g_English ? En : Tr; }
struct Translation { const char* Key; const char* Label; const char* Description; };
inline constexpr Translation kEnglishSettings[] = {
    {"exp", "EXP multiplier", "Multiplies experience earned from enemies and quests."},
    {"currency", "Crystal multiplier", "Multiplies crystals you earn."},
    {"loot", "Loot rolls", "Rolls a defeated enemy's loot table this many times."},
    {"rarity", "Quality rolls", "Generates each item this many times and keeps the highest quality."},
    {"perfect", "Perfect affixes", "Affix values always roll at the top of their range."},
    {"endgame", "Endgame rewards", "Multiplies endgame experience, crystal and essence rewards."},
    {"dmgdealt", "Damage dealt", "Multiplies the damage you deal to enemies."},
    {"dmgtaken", "Damage taken", "Multiplies incoming damage. Zero means no damage."},
    {"cooldown", "Skill cooldowns", "Multiplies skill cooldowns. Zero removes the wait."},
    {"manacost", "Mana cost", "Multiplies skill mana costs. Zero makes skills free."},
    {"speed", "Game speed", "Changes the entire game's speed. Below 1 is slower; above 1 is faster."},
    {"godmode", "God mode", "Prevents damage and refills your health every frame."},
    {"mana", "Infinite mana", "Refills your mana every frame."},
    {"nocd", "No cooldowns", "Clears all skill cooldowns every frame."},
    {"freeshop", "Free purchases", "Purchases and payments do not deduct crystals."},
    {"density", "Monster density", "Multiplies the number of monsters in each group in newly entered areas."},
    {"champions", "Champions / elites", "Multiplies champion and elite spawn chances in newly entered areas."},
    {"reveal", "Reveal map", "Fully reveals the map of each area you enter."},
    {"zoom", "Camera distance", "Changes the camera view. Higher values show more of the world."},
    {"notraps", "No chest traps", "Removes traps from chests."},
    {"radius", "Extra pickup radius", "Adds to your item pickup radius."},
    {"dps", "Damage meter (DPS)", "Shows damage per second over the last five seconds and total damage."},
    {"deathrecap", "Death recap", "Records the last ten incoming hits to show what killed you."},
    {"talents", "Talent point multiplier", "Multiplies talent points awarded when you level up."},
    {"dyes", "Unlock all dyes", "Unlocks all 40 class dyes in the wardrobe."},
    {"gambler", "Unlock Gambler", "Unlocks the Gambler vendor."},
    {"waypoints", "Unlock all waypoints", "Unlocks every area's waypoint. This skips story progression."},
};
inline std::string Label(const Setting& Item) {
    if (!g_English) return Item.Label;
    for (const auto& t : kEnglishSettings) if (std::string_view(Item.Key) == t.Key) return t.Label;
    return Item.Label;
}
inline std::string Description(const Setting& Item) {
    if (!g_English) return Item.Description;
    for (const auto& t : kEnglishSettings) if (std::string_view(Item.Key) == t.Key) return t.Description;
    return Item.Description;
}
inline std::string Category(const std::string& Name) {
    if (!g_English) return Name;
    constexpr const char* tr[] = {"Ödüller","Savaş","Hileler","Dünya","Arayüz","İlerleme","Statlar"};
    constexpr const char* en[] = {"Rewards","Combat","Cheats","World","Interface","Progression","Stats"};
    for (int i=0;i<7;i++) if (Name==tr[i]) return en[i];
    return Name;
}
inline std::wstring Wide(const std::string& Text) {
    const int n = MultiByteToWideChar(CP_UTF8,0,Text.data(),static_cast<int>(Text.size()),nullptr,0);
    std::wstring out(n,L'\0');
    if(n) MultiByteToWideChar(CP_UTF8,0,Text.data(),static_cast<int>(Text.size()),out.data(),n);
    return out;
}
inline std::string Utf8(const std::wstring& Text) {
    const int n=WideCharToMultiByte(CP_UTF8,0,Text.data(),static_cast<int>(Text.size()),nullptr,0,nullptr,nullptr);
    std::string out(n,'\0');
    if(n) WideCharToMultiByte(CP_UTF8,0,Text.data(),static_cast<int>(Text.size()),out.data(),n,nullptr,nullptr);
    return out;
}
inline std::string Fold(const std::string& Text) {
    std::wstring out=Wide(Text);
    for(auto& ch:out) {
        if(ch==L'ı'||ch==L'İ')ch=L'i';
        else if(ch==L'ş'||ch==L'Ş')ch=L's';
        else if(ch==L'ğ'||ch==L'Ğ')ch=L'g';
        else if(ch==L'ü'||ch==L'Ü')ch=L'u';
        else if(ch==L'ö'||ch==L'Ö')ch=L'o';
        else if(ch==L'ç'||ch==L'Ç')ch=L'c';
        else ch=static_cast<wchar_t>(std::towlower(ch));
    }
    return Utf8(out);
}
inline void PopUtf8(std::string& Text) {
    if(Text.empty())return;
    size_t pos=Text.size()-1;
    while(pos && (static_cast<unsigned char>(Text[pos])&0xC0)==0x80)--pos;
    Text.resize(pos);
}
inline void Replace(std::string& Text, std::string_view From, std::string_view To) {
    size_t pos=0;
    while((pos=Text.find(From,pos))!=std::string::npos){Text.replace(pos,From.size(),To);pos+=To.size();}
}
inline std::string Result(std::string Text) {
    if(Text.rfind("OK ",0)==0)Text.erase(0,3);
    if(!g_English)return Text;
    constexpr const char* tr[]={"Karakter yüklü değil","karakter yüklü değil","waypoint tablosu bulunamadı","değer okunamadı","sayı bekleniyordu","Bu satırın cap'i yok","boyalar:","zaten açıktı","açıldı","şimdi","açık","gambler","bonus kaldırıldı","bonusu kaldırıldı","cap kaldırıldı","oyunun cap'i","oyunun değeri","sıfırlandı","tek seferlik yazıldı","sayaç","yeniden hesaplama sıraya alındı","kapalı","yok"};
    constexpr const char* en[]={"No character loaded","No character loaded","Waypoint table not found","Could not read value","Expected a number","This stat has no cap","Dyes:","already unlocked","unlocked","now","open","Gambler","bonus removed","bonus removed","cap removed","game cap","game value","reset","written once","counter","recalculation queued","off","none"};
    for(size_t i=0;i<std::size(tr);i++)Replace(Text,tr[i],en[i]);
    return Text;
}
struct StatTranslation { const char* Alias; const char* Tr; const char* En; };
inline constexpr StatTranslation kStatNames[] = {
#include "VaultStatNames.inc"
};
inline std::string StatName(const std::string& Alias,const std::string& Fallback) {
    for(const auto& t:kStatNames)if(Alias==t.Alias)return g_English?t.En:t.Tr;
    return Fallback;
}
} // namespace cp::vault
