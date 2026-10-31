// Chronicle Vault. All drawing and hit testing share one 960x540 coordinate system.
// No external textures, input injection, gameplay hooks or per-frame file IO.
#include "Menu.hpp"
#include "Core.hpp"
#include "Stats.hpp"
#include "VaultLocale.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <sstream>
#include <unordered_map>

namespace cp::menu {
namespace {
using vault::L;
constexpr int GmColor(unsigned c) { return ((c&255)<<16)|(c&0xFF00)|((c>>16)&255); }
constexpr int Back=GmColor(0x14171E), Raised=GmColor(0x20242C), Row=GmColor(0x1B2028), Hover=GmColor(0x2D343E),
    Selected=GmColor(0x343028), Line=GmColor(0x64533A), Accent=GmColor(0xD6AC62), Bright=GmColor(0xF4D993),
    Text=GmColor(0xEEE5CE), Muted=GmColor(0xA9A398), Changed=GmColor(0x91D99B), Warn=GmColor(0xE1A071),
    Disabled=GmColor(0x686973), Danger=GmColor(0xD97E7E);
constexpr int VisibleStats=14;
struct Rect {
    double X=0,Y=0,W=0,H=0;
    bool Has(double x,double y)const{return x>=X&&y>=Y&&x<X+W&&y<Y+H;}
};
bool g_Open=false,g_SearchFocus=false,g_RecapOpen=false;
bool g_FilterDirty=true;
std::string g_Shot,g_Status,g_Search;
ULONGLONG g_StatusUntil=0,g_StatRefresh=0;
int g_Tab=0,g_StatSelection=0,g_StatScroll=0,g_Group=0;
std::array<int,16> g_Selection{};
std::array<int,256> g_Held{},g_TestPressed{},g_TestDown{};
std::vector<size_t> g_Filtered;
std::vector<stats::RowView> g_Metadata;
std::unordered_map<size_t,stats::RowView> g_StatValues;
std::unordered_map<std::string,double> g_Widths;
std::vector<DamageHit> g_Death;
std::vector<DamageHit> g_PreviewDeath;
ULONGLONG g_PreviewUntil=0;
std::string g_PendingText;
bool g_QueuedPointer=false;
double g_TestX=0,g_TestY=0,g_MouseX=-1000,g_MouseY=-1000;
int g_TestButton=1,g_DrawCalls=0;
double g_LastScale=1,g_LastWidth=960,g_LastHeight=540,g_DrawMicros=0;
int g_InputSerial=0;
std::vector<Rect> g_StatRects;

bool Pressed(int k){return g_TestPressed[k]||Builtin("keyboard_check_pressed",{Value(k)}).Bool();}
bool Down(int k){return g_TestDown[k]||Builtin("keyboard_check",{Value(k)}).Bool();}
bool Repeat(int k){if(!Down(k)&&!g_TestPressed[k]){g_Held[k]=0;return false;}int n=++g_Held[k];return n==1||(n>20&&n%3==0);}
std::vector<std::string> Tabs(){
    std::vector<std::string> out;
    for(const auto& s:Settings())if(std::find(out.begin(),out.end(),s.Category)==out.end())out.push_back(s.Category);
    out.push_back("Statlar");return out;
}
std::vector<Setting*> Items(const std::string& category){std::vector<Setting*> out;for(auto& s:Settings())if(category==s.Category)out.push_back(&s);return out;}
bool IsStatTab(){const auto tabs=Tabs();return g_Tab==static_cast<int>(tabs.size())-1;}
bool Modified(const Setting& s){return s.Kind!=SettingKind::Action&&std::abs(s.Value-s.Default)>1e-9;}
void Status(const std::string& text){g_Status=text;g_StatusUntil=GetTickCount64()+4000;}
void InvalidateStats(){g_StatRefresh=0;g_StatValues.clear();}
void Language(bool english){vault::SetLanguage(english);g_StatusUntil=0;g_FilterDirty=true;InvalidateStats();}
std::string Number(const Setting& s,double v){return (s.Kind==SettingKind::Multiplier?"x":"")+Num(v);}
double Next(const Setting& s,int direction){
    if(s.Steps.empty())return std::clamp(s.Value+direction,s.Minimum,s.Maximum);
    if(direction>0){for(double v:s.Steps)if(v>s.Value+1e-9)return v;}
    else for(auto i=s.Steps.rbegin();i!=s.Steps.rend();++i)if(*i<s.Value-1e-9)return *i;
    return s.Value;
}
void Change(Setting& s,int direction){
    if(s.Kind==SettingKind::Action)return;
    SetValue(s,s.Kind==SettingKind::Toggle?!s.Value:Next(s,direction));
}
void Action(Setting& s){if(s.Kind==SettingKind::Action)Status(s.Run?s.Run():"ERR action unavailable");}

// RAII also restores vertical alignment; the old menu and DPS did not both restore it.
struct DrawState {
    Value Color=Builtin("draw_get_color"),Alpha=Builtin("draw_get_alpha"),Font=Builtin("draw_get_font"),
        H=Builtin("draw_get_halign"),V=Builtin("draw_get_valign");
    DrawState(){
        static Value font=Builtin("asset_get_index",{Value("font_small_GUI_latin")});
        if(!font.IsUndefined()&&(!font.IsNumber()||font.Number()>=0))Builtin("draw_set_font",{font});
        Builtin("draw_set_alpha",{Value(1)});Builtin("draw_set_halign",{Value(0)});Builtin("draw_set_valign",{Value(0)});
    }
    ~DrawState(){Builtin("draw_set_color",{Color});Builtin("draw_set_alpha",{Alpha});Builtin("draw_set_font",{Font});Builtin("draw_set_halign",{H});Builtin("draw_set_valign",{V});}
};
struct Canvas {
    double S=1,OX=0,OY=0,W=960,H=540;
    Canvas(){
        W=Builtin("display_get_gui_width").Number();H=Builtin("display_get_gui_height").Number();
        S=(std::max)(0.25,(std::min)(W/960.0,H/540.0));OX=(W-960*S)/2;OY=(H-540*S)/2;
    }
    double X(double x)const{return std::round(OX+x*S);}
    double Y(double y)const{return std::round(OY+y*S);}
    double Width(const std::string& text,int scale=1)const{
        auto it=g_Widths.find(text);if(it!=g_Widths.end())return it->second*scale;
        double w=Builtin("string_width",{Value(text)}).Number();
        if(g_Widths.size()>2048)g_Widths.clear();g_Widths.emplace(text,w);return w*scale;
    }
    std::string Fit(std::string text,double width,int scale=1)const{
        if(Width(text,scale)<=width)return text;
        while(!text.empty()&&Width(text+"...",scale)>width)vault::PopUtf8(text);
        return text.empty()?"":text+"...";
    }
    void Fill(Rect r,int color,double alpha=1)const{
        ++g_DrawCalls;Builtin("draw_set_alpha",{Value(alpha)});Builtin("draw_set_color",{Value(color)});
        Builtin("draw_rectangle",{Value(X(r.X)),Value(Y(r.Y)),Value(X(r.X+r.W)-1),Value(Y(r.Y+r.H)-1),Value(false)});
        Builtin("draw_set_alpha",{Value(1)});
    }
    void Outline(Rect r,int color)const{
        ++g_DrawCalls;Builtin("draw_set_color",{Value(color)});
        Builtin("draw_rectangle",{Value(X(r.X)),Value(Y(r.Y)),Value(X(r.X+r.W)-1),Value(Y(r.Y+r.H)-1),Value(true)});
    }
    void Box(Rect r,int fill,int border=-1,double alpha=1)const{Fill(r,fill,alpha);if(border>=0)Outline(r,border);}
    void T(double x,double y,std::string text,int color=Text,int scale=1,int align=0,double maxWidth=0)const{
        if(maxWidth>0)text=Fit(text,maxWidth,scale);
        if(align==1)x-=Width(text,scale)/2;else if(align==2)x-=Width(text,scale);
        ++g_DrawCalls;Builtin("draw_set_color",{Value(color)});
        // This game's font_small_GUI_latin has a 4px uppercase top bearing.
        Builtin("draw_text_transformed",{Value(X(x)),Value(Y(y-4*scale)),Value(text),Value(S*scale),Value(S*scale),Value(0)});
    }
    void Wrap(double x,double y,const std::string& text,double width,int color=Muted,int maxLines=5)const{
        std::istringstream in(text);std::vector<std::string> lines;std::string word,line;
        while(in>>word){const std::string next=line.empty()?word:line+" "+word;if(Width(next)>width&&!line.empty()){lines.push_back(line);line=word;}else line=next;}
        if(!line.empty())lines.push_back(line);
        for(int i=0;i<static_cast<int>(lines.size())&&i<maxLines;i++){
            std::string s=lines[i];if(i==maxLines-1&&static_cast<int>(lines.size())>maxLines)s+=" ...";
            T(x,y+i*15,s,color,1,0,width);
        }
    }
    bool Hovered(Rect r)const{return r.Has(g_MouseX,g_MouseY);}
    void Button(Rect r,const std::string& label,bool selected=false,bool disabled=false,int border=-1)const{
        int fill=selected?Selected:!disabled&&Hovered(r)?Hover:Raised;
        Box(r,fill,border>=0?border:selected||(!disabled&&Hovered(r))?Accent:Line);
        T(r.X+r.W/2,r.Y+(r.H-9)/2,label,disabled?Disabled:selected?Bright:Text,1,1,r.W-8);
    }
    void Frame(Rect r)const{
        Fill({r.X+3,r.Y+4,r.W,r.H},0x080808,0.78);Box(r,Back,Accent,250.0/255.0);
        Outline({r.X+3,r.Y+3,r.W-6,r.H-6},Line);
        for(int i=0;i<4;i++){
            const double x=r.X+(i&1?r.W-1:0),y=r.Y+(i&2?r.H-1:0),dx=i&1?-1:1,dy=i&2?-1:1;
            Fill({(std::min)(x+dx*2,x+dx*13),y+dy*2,12,1},Bright);
            Fill({x+dx*2,(std::min)(y+dy*2,y+dy*13),1,12},Bright);
        }
    }
};
Rect LanguageRect(bool english){return {english?659.0:620.0,36,34,26};}
Rect CloseRect(){return {712,36,86,26};}
Rect TabRect(int i,int n){const int pitch=(std::min)(37,333/(std::max)(1,n));return {36,128.0+i*pitch,147.0,static_cast<double>(pitch-5)};}
Rect SettingRect(int i,int count){bool compact=count>6;return {208,156.0+i*(compact?25:42),390.0,compact?23.0:38.0};}
Rect ControlRect(int i,int count){auto r=SettingRect(i,count);return {482,r.Y+(count>6?1:6),104.0,count>6?21.0:26.0};}

void DrawStepper(const Canvas& c,Rect r,const Setting& s){
    const bool minus=Next(s,-1)!=s.Value,plus=Next(s,1)!=s.Value;
    c.Button({r.X,r.Y,24,r.H},"-",false,!minus);
    c.Box({r.X+24,r.Y,r.W-48,r.H},Back,Line);
    c.T(r.X+r.W/2,r.Y+(r.H-9)/2,Number(s,s.Value),Modified(s)?Changed:Text,1,1,r.W-50);
    c.Button({r.X+r.W-24,r.Y,24,r.H},"+",false,!plus);
}
void DrawToggle(const Canvas& c,Rect r,bool on){
    c.Box(r,c.Hovered(r)?Hover:Raised,on?Changed:Line);
    c.T(r.X+9,r.Y+(r.H-9)/2,on?L("AÇIK","ON"):L("KAPALI","OFF"),on?Changed:Muted);
    c.Box({r.X+r.W-27,r.Y+(r.H-10)/2,18,10},Back,Line);
    c.Fill({r.X+r.W-(on?16:25),r.Y+(r.H-6)/2,7,6},on?Changed:Muted);
}
void DrawShell(const Canvas& c,const std::vector<std::string>& tabs){
    c.Frame({20,18,800,490});c.T(42,36,"ChroniconPlus",Bright,2);c.T(43,65,"C H R O N I C L E   V A U L T",Muted);
    c.Button(LanguageRect(false),"TR",!vault::g_English);c.Button(LanguageRect(true),"EN",vault::g_English);
    c.Button(CloseRect(),L("F8 KAPAT","F8 CLOSE"));
    c.Fill({40,87,759,1},Line);c.Fill({417,86,7,3},Accent);c.Fill({419,84,3,7},Accent);c.Fill({420,86,1,3},Bright);
    c.T(42,106,L("BÖLÜMLER","CHAPTERS"),Muted);
    for(int i=0;i<static_cast<int>(tabs.size());i++){
        Rect r=TabRect(i,static_cast<int>(tabs.size()));bool selected=i==g_Tab;
        if(selected||c.Hovered(r))c.Box(r,selected?Selected:Hover,selected?Accent:Line);
        if(selected)c.Fill({r.X,r.Y,3,r.H},Bright);
        c.T(48,r.Y+(r.H-9)/2,std::format("{:02}",i+1),selected?Accent:Disabled);
        c.T(73,r.Y+(r.H-9)/2,vault::Category(tabs[i]),selected?Bright:Text,1,0,103);
    }
    c.Fill({192,105,1,361},Line);c.Fill({40,477,759,1},Line);
    if(IsStatTab())c.T(43,488,L("SOL/SAĞ +/-1  SHIFT x10  CTRL x100  C Cap  BACKSPACE Sıfırla","LEFT/RIGHT +/-1  SHIFT x10  CTRL x100  C Cap  BACKSPACE Reset"),Muted,1,0,635);
    else{c.T(43,488,L("YUKARI/AŞAĞI Seç","UP/DOWN Select"),Muted);c.T(225,488,L("SOL/SAĞ Değiştir","LEFT/RIGHT Change"),Muted);c.T(433,488,L("TAB Bölüm","TAB Chapter"),Muted);}
    c.T(798,488,IsStatTab()?"PgUp/PgDn  F8":L("F8 Kapat","F8 Close"),Accent,1,2);
}
void DrawSettings(const Canvas& c,const std::string& category){
    const auto items=Items(category);const int n=static_cast<int>(items.size());int& selected=g_Selection[g_Tab];
    if(n)selected=std::clamp(selected,0,n-1);
    c.T(210,105,vault::Category(category),Text,2,0,340);
    int changed=0;for(const auto* s:items)changed+=Modified(*s);
    c.T(797,110,vault::g_English?std::format("{} SETTINGS / {} CHANGED",n,changed):std::format("{} AYAR / {} DEĞİŞİKLİK",n,changed),Muted,1,2,240);
    c.T(211,132,L("Ayarları seç; ayrıntıları sağda incele.","Select a setting to inspect its details."),Muted);
    for(int i=0;i<n&&i<10;i++){
        const Setting& s=*items[i];Rect r=SettingRect(i,n),ctrl=ControlRect(i,n);bool focus=i==selected;
        c.Box(r,focus?Selected:c.Hovered(r)?Hover:Row,focus?Accent:-1);
        if(focus)c.Fill({r.X,r.Y,3,r.H},Accent);
        if(n>6&&Modified(s))c.Fill({r.X+6,r.Y+9,3,3},Changed);
        c.T(220,r.Y+(n>6?7:7),vault::Label(s),focus?Bright:Text,1,0,252);
        if(n<=6)c.T(220,r.Y+22,s.Kind==SettingKind::Action?L("TEK SEFERLİK","ONE-TIME ACTION"):Modified(s)?L("DEĞİŞTİRİLDİ","MODIFIED"):L("VARSAYILAN","DEFAULT"),Modified(s)?Changed:Muted);
        if(s.Kind==SettingKind::Toggle)DrawToggle(c,ctrl,s.Value!=0);
        else if(s.Kind==SettingKind::Action)c.Button(ctrl,L("UYGULA","EXECUTE"),false,false,std::string_view(s.Key)=="waypoints"?Warn:-1);
        else DrawStepper(c,ctrl,s);
    }
    if(items.empty())return;
    const Setting& s=*items[selected];
    c.Box({614,156,183,248},Raised,Line);c.T(629,172,L("SEÇİLİ AYAR","SELECTED SETTING"),Accent);
    std::string value=s.Kind==SettingKind::Action?L("EYLEM","ACTION"):s.Kind==SettingKind::Toggle?(s.Value?L("AÇIK","ON"):L("KAPALI","OFF")):Number(s,s.Value);
    int large=c.Width(value,3)<=151?3:c.Width(value,2)<=151?2:1;
    c.T(629,199,value,Modified(s)?Changed:Bright,large,0,151);c.T(629,239,vault::Label(s),Text,1,0,151);
    c.Fill({629,262,151,1},Line);c.Wrap(629,278,vault::Description(s),151,Muted,4);
    if(s.Kind!=SettingKind::Action)c.T(629,351,std::string(L("Varsayılan: ","Default: "))+(s.Kind==SettingKind::Toggle?(s.Default?L("Açık","On"):L("Kapalı","Off")):Number(s,s.Default)),Text,1,0,151);
    c.T(629,373,s.Kind==SettingKind::Action?L("Enter ile uygula","Enter to execute"):Modified(s)?L("Değişiklik etkin","Modification active"):L("Normal oyun değeri","Normal game value"),Modified(s)?Changed:Muted,1,0,151);
    bool warning=std::string_view(s.Key)=="waypoints"||std::string_view(s.Key)=="godmode"||(std::string_view(s.Key)=="dmgtaken"&&s.Value==0);
    const bool status=GetTickCount64()<g_StatusUntil;const bool error=g_Status.rfind("ERR",0)==0;
    c.Box({208,423,589,41},Row,status?(error?Danger:Changed):warning?Warn:Line);
    if(status)c.Wrap(220,431,vault::Result(g_Status),565,error?Danger:Changed,2);
    else if(warning)c.Wrap(220,431,"! "+vault::Description(s),565,Warn,2);
    else{
        c.Fill({220,438,5,5},Changed);c.T(234,431,L("Yeşil işaret: varsayılandan farklı.","Green marker: different from the default."),Text);
        c.T(234,446,L("Değişiklikler otomatik kaydedilir.","Changes are saved automatically."),Muted);
    }
    if(g_Tab==4&&Enabled(S_DEATH_RECAP))c.Button({208,389,184,23},L("SON ÖLÜMÜ GÖSTER","SHOW LAST DEATH"));
}

bool GroupMatch(stats::RowKind kind){return g_Group==0||(g_Group==1&&kind==stats::RowKind::Core)||(g_Group==2&&(kind==stats::RowKind::Computed||kind==stats::RowKind::Unknown))||(g_Group==3&&kind==stats::RowKind::Counter);}
void BuildFilter(){
    const size_t count=stats::RowCount();
    if(!g_FilterDirty&&g_Metadata.size()==count)return;
    if(g_Metadata.size()!=count){g_Metadata.clear();for(size_t i=0;i<count;i++)if(auto row=stats::Metadata(i))g_Metadata.push_back(*row);}
    g_Filtered.clear();const std::string query=vault::Fold(g_Search);
    for(size_t i=0;i<g_Metadata.size();i++){
        const auto& row=g_Metadata[i];if(!GroupMatch(row.Kind))continue;
        bool nameMatch=query.empty()||vault::Fold(row.Alias+" "+row.Name+" "+vault::StatName(row.Alias,row.Name)).find(query)!=std::string::npos;
        if(!nameMatch)for(const auto& t:vault::kStatNames)if(row.Alias==t.Alias&&vault::Fold(std::string(t.En)+" "+t.Tr).find(query)!=std::string::npos){nameMatch=true;break;}
        if(nameMatch)g_Filtered.push_back(i);
    }
    g_StatSelection=std::clamp(g_StatSelection,0,(std::max)(0,static_cast<int>(g_Filtered.size())-1));
    g_StatScroll=std::clamp(g_StatScroll,0,(std::max)(0,static_cast<int>(g_Filtered.size())-VisibleStats));
    g_FilterDirty=false;
}
void FilterChanged(){g_FilterDirty=true;g_StatSelection=0;g_StatScroll=0;BuildFilter();InvalidateStats();}
void KeepVisible(){
    if(g_StatSelection<g_StatScroll)g_StatScroll=g_StatSelection;
    if(g_StatSelection>=g_StatScroll+VisibleStats)g_StatScroll=g_StatSelection-VisibleStats+1;
}
std::string Bonus(const stats::RowView& row){
    if(row.Kind==stats::RowKind::Counter)return L("tek sefer","one-time");
    std::string text=stats::BonusText(row);if(vault::g_English)vault::Replace(text,"yok","none");return text.empty()?"-":text;
}
void RefreshValues(){
    if(GetTickCount64()<g_StatRefresh)return;
    g_StatValues.clear();for(int i=g_StatScroll;i<static_cast<int>(g_Filtered.size())&&i<g_StatScroll+VisibleStats;i++)if(auto row=stats::Row(g_Filtered[i]))g_StatValues.emplace(g_Filtered[i],*row);
    g_StatRefresh=GetTickCount64()+200;
}
void DrawStats(const Canvas& c){
    BuildFilter();RefreshValues();g_StatRects.clear();
    c.T(210,105,L("Stat editörü","Stat editor"),Text,2,0,390);c.T(797,110,std::format("{} STAT",g_Metadata.size()),Muted,1,2);
    c.Box({208,139,589,25},Raised,g_SearchFocus?Accent:Line);
    const std::string search=g_Search.empty()?L("Ara: stat adı yaz...","Search: type a stat..."):g_Search+(g_SearchFocus&&GetTickCount64()%1000<500?"|":"");
    c.T(220,148,search,g_Search.empty()?Muted:Text,1,0,445);c.T(783,148,g_SearchFocus?"ESC": "CTRL+F",Muted,1,2);
    const char* groupsTr[]={"Tümü","Ana değerler","Statlar","Sayaçlar"};const char* groupsEn[]={"All","Core","Stats","Counters"};
    for(int i=0;i<4;i++)c.Button({208.0+i*148,174,142,23},vault::g_English?groupsEn[i]:groupsTr[i],g_Group==i);
    c.T(220,209,"STAT",Muted);c.T(557,209,L("DEĞER","VALUE"),Muted,1,2);c.T(637,209,"CAP",Muted,1,2);c.T(775,209,L("BONUS","BONUS"),Muted,1,2);
    c.Fill({208,225,589,1},Line);
    for(int i=g_StatScroll;i<static_cast<int>(g_Filtered.size())&&i<g_StatScroll+VisibleStats;i++){
        Rect r={208,231.0+(i-g_StatScroll)*14,581,14};g_StatRects.push_back(r);
        auto found=g_StatValues.find(g_Filtered[i]);if(found==g_StatValues.end())continue;
        const auto& row=found->second;bool selected=i==g_StatSelection, counter=row.Kind==stats::RowKind::Counter;
        c.Fill(r,selected?Selected:c.Hovered(r)?Hover:(i-g_StatScroll)%2==0?Row:Back);if(selected)c.Fill({208,r.Y,2,14},Accent);
        c.T(220,r.Y+2,vault::StatName(row.Alias,row.Name),counter?Muted:Text,1,0,285);
        c.T(557,r.Y+2,row.Current?Num(*row.Current):"-",row.Bonus.Empty()?Text:Changed,1,2,64);
        const bool noCap=row.Bonus.Cap&&*row.Bonus.Cap>=1e8;
        std::string cap=noCap?L("yok","none"):row.Bonus.Cap?Num(*row.Bonus.Cap):row.GameCap&&*row.GameCap>0?Num(*row.GameCap):"-";
        c.T(637,r.Y+2,cap,noCap?Warn:Muted,1,2,64);c.T(775,r.Y+2,Bonus(row),counter||row.Bonus.Empty()?Muted:Changed,1,2,126);
    }
    int count=static_cast<int>(g_Filtered.size());
    if(!count)c.T(220,245,stats::Ready()?L("Eşleşen stat yok.","No matching stats."):L("Önce bir karakter yükle.","Load a character first."),Muted);
    if(count>VisibleStats){double h=(std::max)(10.0,196.0*VisibleStats/count),y=231+(196-h)*g_StatScroll/(count-VisibleStats);c.Fill({793,231,4,196},Raised);c.Fill({793,y,4,h},Accent);}
    c.T(210,439,std::format("{}-{} / {}",count?g_StatScroll+1:0,(std::min)(g_StatScroll+VisibleStats,count),count),Muted);
    c.T(797,439,L("Sayaçlar tek seferlik değiştirilir.","Counters are changed once, not as bonuses."),Muted,1,2,435);
    c.Button({208,452,46,19},"-1");c.Button({260,452,46,19},"+1");
    bool capDisabled=count==0||(count&&g_Metadata[g_Filtered[g_StatSelection]].Kind==stats::RowKind::Core);
    c.Button({318,452,96,19},L("C  Cap","C  Cap"),false,capDisabled);c.Button({420,452,118,19},L("SIFIRLA","RESET"));
    if(count)c.T(795,457,g_Metadata[g_Filtered[g_StatSelection]].Alias,Muted,1,2,245);
    if(GetTickCount64()<g_StatusUntil)c.T(210,128,vault::Result(g_Status),g_Status.rfind("ERR",0)==0?Danger:Changed,1,0,580);
}
void EditStat(int mode,double delta=0){
    if(g_Filtered.empty()||!stats::Ready())return;
    size_t index=g_Filtered[g_StatSelection];
    std::string result=mode==0?stats::AdjustAdd(index,delta):mode==1?stats::ToggleCap(index):stats::Reset(index);
    for(const auto& row:g_Metadata)vault::Replace(result,row.Name,vault::StatName(row.Alias,row.Name));
    Status(result);InvalidateStats();
}
void AddSearch(const std::string& text){
    if(vault::Wide(g_Search).size()<48)g_Search+=text;
    auto wide=vault::Wide(g_Search);if(wide.size()>48)g_Search=vault::Utf8(wide.substr(0,48));FilterChanged();
}
void SearchInput(){
    if(!g_PendingText.empty()){AddSearch(g_PendingText);g_PendingText.clear();}
    if(Pressed(VK_ESCAPE)){g_Search.clear();g_SearchFocus=false;FilterChanged();return;}
    if(Pressed(VK_RETURN)){g_SearchFocus=false;return;}
    if(Repeat(VK_BACK)){vault::PopUtf8(g_Search);FilterChanged();}
    if(Down(VK_CONTROL)||Down(VK_MENU))return;
    BYTE state[256]={};if(!GetKeyboardState(state))return;
    HKL layout=GetKeyboardLayout(0);
    for(int k=32;k<255;k++){
        if(k==VK_DELETE||(k>=VK_PRIOR&&k<=VK_HELP)||(k>=VK_F1&&k<=VK_F24)||k==VK_LWIN||k==VK_RWIN||k==VK_APPS||k==VK_NUMLOCK||k==VK_SCROLL||k==VK_LSHIFT||k==VK_RSHIFT||k==VK_LCONTROL||k==VK_RCONTROL||k==VK_LMENU||k==VK_RMENU)continue;
        if(!Pressed(k))continue;
        wchar_t chars[8]={};int n=ToUnicodeEx(k,MapVirtualKeyExW(k,MAPVK_VK_TO_VSC,layout),state,chars,8,4,layout);
        if(n>0&&chars[0]>=32)AddSearch(vault::Utf8(std::wstring(chars,n)));
    }
}
void StatKeys(){
    if(Down(VK_CONTROL)&&Pressed('F')){g_SearchFocus=true;return;}
    if(g_SearchFocus){SearchInput();return;}
    if(!g_PendingText.empty()){AddSearch(g_PendingText);g_PendingText.clear();}
    int move=0;if(Repeat(VK_UP))move=-1;if(Repeat(VK_DOWN))move=1;if(Repeat(VK_PRIOR))move=-VisibleStats;if(Repeat(VK_NEXT))move=VisibleStats;
    if(move){g_StatSelection=std::clamp(g_StatSelection+move,0,(std::max)(0,static_cast<int>(g_Filtered.size())-1));KeepVisible();InvalidateStats();}
    double delta=(Down(VK_SHIFT)?10:1)*(Down(VK_CONTROL)?100:1);
    if(Repeat(VK_LEFT))EditStat(0,-delta);if(Repeat(VK_RIGHT))EditStat(0,delta);
    if(Pressed('C'))EditStat(1);if(Pressed(VK_BACK))EditStat(2);
}
void DrawRecap(const Canvas& c,const std::vector<DamageHit>& hits){
    c.Frame({188,116,584,310});c.T(204,132,L("Ölüm özeti","Death recap"),Bright,2);c.T(204,159,L("Son 10 vuruş","Last 10 hits"),Muted);
    c.Button({687,132,67,25},L("KAPAT","CLOSE"));c.Fill({204,177,550,1},Danger);
    c.T(204,187,L("ZAMAN","TIME"),Muted);c.T(350,187,L("HASAR","DAMAGE"),Muted,1,2);c.T(374,187,L("KAYNAK","SOURCE"),Muted);c.T(746,187,L("KALAN CAN","HP LEFT"),Muted,1,2);
    if(hits.empty())c.T(204,214,L("Henüz vuruş kaydı yok.","No recorded hits yet."),Muted);
    int start=(std::max)(0,static_cast<int>(hits.size())-10);
    for(int i=start;i<static_cast<int>(hits.size());i++){
        const auto& hit=hits[i];double y=207+(i-start)*17;bool fatal=hit.HpAfter<=0;
        c.Fill({204,y,550,17},fatal?GmColor(0x3E252A):(i-start)%2==0?Row:Back);
        c.T(211,y+4,std::format("-{:.1f}s",hit.SecondsAgo),Muted);c.T(350,y+4,Num(std::round(hit.Amount)),fatal?Danger:Text,1,2,70);
        c.T(374,y+4,hit.Source,Text,1,0,240);c.T(746,y+4,Num(std::round(hit.HpAfter)),fatal?Danger:Text,1,2,95);
    }
    c.T(204,399,L("Son ölüm anındaki kayıt. F8 ile kapat.","Recorded at your last death. F8 to close."),Muted,1,0,540);
}
void Pointer(const std::vector<std::string>& tabs,bool right){
    double x=g_MouseX,y=g_MouseY;
    if(g_RecapOpen){if(Rect{687,132,67,25}.Has(x,y)){g_RecapOpen=false;}return;}
    if(CloseRect().Has(x,y)){g_Open=false;g_SearchFocus=false;return;}
    if(LanguageRect(false).Has(x,y)){Language(false);return;}if(LanguageRect(true).Has(x,y)){Language(true);return;}
    for(int i=0;i<static_cast<int>(tabs.size());i++)if(TabRect(i,static_cast<int>(tabs.size())).Has(x,y)){g_Tab=i;g_SearchFocus=false;g_StatusUntil=0;InvalidateStats();return;}
    if(IsStatTab()){
        if(Rect{208,139,589,25}.Has(x,y)){g_SearchFocus=true;return;}g_SearchFocus=false;
        for(int i=0;i<4;i++)if(Rect{208.0+i*148,174,142,23}.Has(x,y)){g_Group=i;FilterChanged();return;}
        if(Rect{208,452,46,19}.Has(x,y)){EditStat(0,-1);return;}if(Rect{260,452,46,19}.Has(x,y)){EditStat(0,1);return;}
        if(Rect{318,452,96,19}.Has(x,y)){if(!g_Filtered.empty()&&g_Metadata[g_Filtered[g_StatSelection]].Kind!=stats::RowKind::Core)EditStat(1);return;}
        if(Rect{420,452,118,19}.Has(x,y)){EditStat(2);return;}
        for(int i=0;i<VisibleStats&&g_StatScroll+i<static_cast<int>(g_Filtered.size());i++)if(Rect{208,231.0+i*14,581,14}.Has(x,y)){g_StatSelection=g_StatScroll+i;return;}
        return;
    }
    const auto items=Items(tabs[g_Tab]);int count=static_cast<int>(items.size());
    if(g_Tab==4&&Enabled(S_DEATH_RECAP)&&Rect{208,389,184,23}.Has(x,y)){g_RecapOpen=true;return;}
    for(int i=0;i<count&&i<10;i++){
        if(!SettingRect(i,count).Has(x,y))continue;g_Selection[g_Tab]=i;Rect r=ControlRect(i,count);Setting& s=*items[i];
        if(r.Has(x,y)){
            if(s.Kind==SettingKind::Action){if(!right)Action(s);}
            else if(s.Kind==SettingKind::Toggle)Change(s,1);
            else if(x<r.X+24)Change(s,-1);else if(x>=r.X+r.W-24)Change(s,1);
            // The value well is deliberately read-only; plus/minus have unambiguous hit areas.
        }
        return;
    }
}
void Draw(){
    DrawState guard;Canvas c;g_DrawCalls=0;g_LastScale=c.S;g_LastWidth=c.W;g_LastHeight=c.H;
    const auto tabs=Tabs();DrawShell(c,tabs);if(IsStatTab())DrawStats(c);else DrawSettings(c,tabs[g_Tab]);if(g_RecapOpen)DrawRecap(c,g_Death);
}
void TakeShot(){if(g_Shot.empty())return;Builtin("screen_save",{Value(g_Shot)});Log("[shot] "+g_Shot);g_Shot.clear();}
std::string Quote(const std::string& text){std::string out="\"";for(char ch:text){if(ch=='"'||ch=='\\')out+='\\';if(ch=='\n'){out+="\\n";continue;}out+=ch;}return out+"\"";}
std::string Inspect(){
    const auto tabs=Tabs();const auto items=IsStatTab()?std::vector<Setting*>{}:Items(tabs[g_Tab]);
    std::string out=std::format("{{\"open\":{},\"language\":\"{}\",\"tab\":{},\"title\":{},\"scale\":{},\"guiWidth\":{},\"guiHeight\":{},\"drawCalls\":{},\"drawMicroseconds\":{},\"inputSerial\":{},\"search\":{},\"searchFocused\":{},\"group\":{},\"filteredStats\":{},\"statSelection\":{},\"statScroll\":{},\"recapOpen\":{},\"settings\":[",g_Open,vault::g_English?"en":"tr",g_Tab+1,Quote(vault::Category(tabs[g_Tab])),g_LastScale,g_LastWidth,g_LastHeight,g_DrawCalls,g_DrawMicros,g_InputSerial,Quote(g_Search),g_SearchFocus,g_Group,g_Filtered.size(),g_StatSelection,g_StatScroll,g_RecapOpen);
    for(size_t i=0;i<items.size();i++){if(i)out+=",";Rect r=ControlRect(static_cast<int>(i),static_cast<int>(items.size()));out+=std::format("{{\"key\":{},\"label\":{},\"description\":{},\"value\":{},\"modified\":{},\"selected\":{},\"control\":[{},{},{},{}]}}",Quote(items[i]->Key),Quote(vault::Label(*items[i])),Quote(vault::Description(*items[i])),items[i]->Value,Modified(*items[i]),g_Selection[g_Tab]==static_cast<int>(i),r.X,r.Y,r.W,r.H);}
    out+="],\"visibleStats\":[";
    for(int i=g_StatScroll;i<static_cast<int>(g_Filtered.size())&&i<g_StatScroll+VisibleStats;i++){if(i!=g_StatScroll)out+=",";const auto& row=g_Metadata[g_Filtered[i]];out+="{\"alias\":"+Quote(row.Alias)+",\"name\":"+Quote(vault::StatName(row.Alias,row.Name))+"}";}
    return "OK "+out+"]}";
}
} // namespace

void DrawDps(double dps,double total){
    vault::LoadLanguage();if(g_Open||g_RecapOpen)return;DrawState guard;Canvas c;
    // Below the stock quest box, rather than on top of its top-right title.
    Rect r={772,68,176,88};c.Box(r,Back,Accent,0.94);c.T(782,76,L("HASAR ÖLÇER","DAMAGE METER"),Accent);
    std::string value=Num(std::round(dps));int scale=c.Width(value,2)<=115?2:1;
    c.T(782,93,value,dps>0?Bright:Muted,scale,0,115);c.T(938,99,"DPS",Muted,1,2);
    c.Fill({782,117,156,1},Line);c.T(782,124,L("Son 5 saniye","Last 5 seconds"),Muted);
    c.T(782,139,L("Toplam","Total"),Muted);c.T(938,139,Num(std::round(total)),Text,1,2,110);
}
bool Visible(){return g_Open||g_RecapOpen;}
void NotifyDeath(const std::vector<DamageHit>& hits){vault::LoadLanguage();g_Death=hits;g_RecapOpen=true;}
void Frame(){
    vault::LoadLanguage();
    if(Pressed(Adv().MenuKey)){
        if(g_RecapOpen){g_RecapOpen=false;g_Open=false;}else g_Open=!g_Open;g_SearchFocus=false;InvalidateStats();
    }
    bool draw=g_Open||g_RecapOpen;
    if(draw){
        Canvas c;
        double mx=Builtin("device_mouse_x_to_gui",{Value(0)}).Number(),my=Builtin("device_mouse_y_to_gui",{Value(0)}).Number();
        g_MouseX=(mx-c.OX)/c.S;g_MouseY=(my-c.OY)/c.S;
        if(g_QueuedPointer){g_MouseX=g_TestX;g_MouseY=g_TestY;}
        const auto tabs=Tabs();g_Tab=std::clamp(g_Tab,0,static_cast<int>(tabs.size())-1);
        if(IsStatTab())BuildFilter();
        bool left=g_QueuedPointer?g_TestButton==1:Builtin("mouse_check_button_pressed",{Value(1)}).Bool();
        bool right=g_QueuedPointer?g_TestButton==2:Builtin("mouse_check_button_pressed",{Value(2)}).Bool();
        if(left||right){Pointer(tabs,right);++g_InputSerial;}
        if(g_Open&&!g_RecapOpen){
            if(Pressed(VK_TAB)&&!g_SearchFocus){g_Tab=(g_Tab+(Down(VK_SHIFT)?static_cast<int>(tabs.size())-1:1))%tabs.size();g_StatusUntil=0;InvalidateStats();}
            if(IsStatTab())StatKeys();
            else{
                auto items=Items(tabs[g_Tab]);int& row=g_Selection[g_Tab];int n=static_cast<int>(items.size());
                if(n){if(Repeat(VK_UP))row=(row+n-1)%n;if(Repeat(VK_DOWN))row=(row+1)%n;row=std::clamp(row,0,n-1);
                    if(Repeat(VK_LEFT))Change(*items[row],-1);if(Repeat(VK_RIGHT))Change(*items[row],1);if(Pressed(VK_RETURN))Action(*items[row]);}
            }
        }
        if(g_Open){LARGE_INTEGER a,b,f;QueryPerformanceCounter(&a);Draw();QueryPerformanceCounter(&b);QueryPerformanceFrequency(&f);g_DrawMicros=(b.QuadPart-a.QuadPart)*1e6/f.QuadPart;}
        else if(g_RecapOpen){DrawState guard;Canvas cc;DrawRecap(cc,g_Death);}
    }
    if(GetTickCount64()<g_PreviewUntil){DrawState guard;Canvas c;DrawRecap(c,g_PreviewDeath);}
    g_QueuedPointer=false;g_TestPressed.fill(0);g_TestDown.fill(0);TakeShot();
}

std::string Command(const std::vector<std::string>& words){
    vault::LoadLanguage();
    if(words[0]=="shot"){g_Shot=words.size()>1?words[1]:"chroniconplus.png";return "OK screenshot queued";}
    const std::string page=words.size()>1?words[1]:"";
    if(page=="inspect")return Inspect();
    if(page=="language"&&words.size()>2){if(words[2]!="tr"&&words[2]!="en")return "ERR language: tr or en";Language(words[2]=="en");return "OK "+words[2];}
    if(page=="pointer"&&words.size()>3){g_TestX=std::stod(words[2]);g_TestY=std::stod(words[3]);g_TestButton=words.size()>4?std::stoi(words[4]):1;g_QueuedPointer=true;return "OK pointer queued inside mod; no desktop input";}
    if(page=="key"&&words.size()>2){
        const std::pair<const char*,int> keys[]={{"left",VK_LEFT},{"right",VK_RIGHT},{"up",VK_UP},{"down",VK_DOWN},{"tab",VK_TAB},{"enter",VK_RETURN},{"back",VK_BACK},{"esc",VK_ESCAPE},{"pgdn",VK_NEXT},{"pgup",VK_PRIOR},{"f8",119},{"c",'C'}};
        for(auto [name,key]:keys)if(words[2]==name){g_TestPressed[key]=1;g_TestDown[key]=1;g_Held[key]=0;if(words.size()>3){if(words[3]=="shift")g_TestDown[VK_SHIFT]=1;if(words[3]=="ctrl")g_TestDown[VK_CONTROL]=1;}return "OK key queued inside mod; no desktop input";}
        return "ERR unsupported UI key";
    }
    if(page=="search"){
        g_Search.clear();for(size_t i=2;i<words.size();i++){if(i>2)g_Search+=' ';g_Search+=words[i];}FilterChanged();return "OK search updated";
    }
    if(page=="type"&&words.size()>2){g_PendingText=words[2];return "OK text queued";}
    if(page=="group"&&words.size()>2){g_Group=std::clamp(std::stoi(words[2]),0,3);FilterChanged();return "OK group updated";}
    if(page=="recap"){g_RecapOpen=true;return "OK showing recorded death";}
    if(page=="preview-recap"){
        // Isolated renderer fixture only: never inserts fake hits into real combat telemetry.
        g_PreviewDeath.clear();for(int i=0;i<10;i++)g_PreviewDeath.push_back({(9-i)*0.3,static_cast<double>(i+1),"UI preview / obj_ghost",static_cast<double>((9-i)*10)});
        g_PreviewUntil=GetTickCount64()+4000;return "OK isolated recap renderer preview (4 seconds)";
    }
    if(page=="off"||page=="kapat"){g_Open=false;g_RecapOpen=false;g_SearchFocus=false;return "OK closed";}
    const auto tabs=Tabs();
    for(int i=0;i<static_cast<int>(tabs.size());i++)if(page.empty()||std::to_string(i+1)==page||vault::Fold(tabs[i]).rfind(vault::Fold(page),0)==0||vault::Fold(vault::Category(tabs[i])).rfind(vault::Fold(page),0)==0){g_Tab=page.empty()?g_Tab:i;g_Open=true;g_RecapOpen=false;g_SearchFocus=false;g_StatusUntil=0;InvalidateStats();return "OK "+vault::Category(tabs[g_Tab]);}
    return "ERR menu: 1..7, off, language tr|en, inspect";
}
} // namespace cp::menu
