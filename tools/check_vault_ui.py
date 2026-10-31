"""Native UI smoke checks over ChroniconPlus IPC. No desktop input is sent.

The running game must already have a character loaded. Reversible settings are restored
in finally; progression actions and counter edits are never invoked.
"""
from pathlib import Path
import json
import hashlib
import os
import shutil
import sys
import time
from cp import send

sys.stdout.reconfigure(encoding="utf-8")
GAME=Path(__file__).resolve().parents[2]
RUN=Path((GAME/"_ModKit/design/current-vault-run.txt").read_text(encoding="utf-8-sig").strip())
CAPTURE=Path(os.environ["LOCALAPPDATA"])/"Chronicon"/RUN.name
CAPTURE.mkdir(exist_ok=True)
(RUN/"screenshots").mkdir(exist_ok=True)
report={"desktopInputSent":False,"testedDllSha256":hashlib.sha256((GAME/"ChroniconPlus/ChroniconPlus.Core.dll").read_bytes()).hexdigest().upper(),"checks":[],"nativeScreenshots":[],"snapshots":[],"passed":False}
original={}
original_lang="tr"
catalog=json.loads((GAME/"ChroniconPlus/menu-catalog.json").read_text(encoding="utf-8-sig"))
action_keys={s["key"] for tab in catalog["tabs"] for s in tab.get("settings",[]) if s["kind"]=="action"}


def cmd(text):
    reply=send(text)
    if not reply.startswith("OK "):raise RuntimeError(f"{text}: {reply}")
    time.sleep(.12)
    return reply[3:]


def state():return json.loads(cmd("menu inspect"))
def check(label,condition):
    report["checks"].append({"name":label,"passed":bool(condition)})
    if not condition:raise AssertionError(label)


def shot(name):
    name=name+".png"
    path=CAPTURE/name
    cmd("shot "+RUN.name+"/"+name)
    until=time.monotonic()+8
    while time.monotonic()<until:
        if path.exists() and path.stat().st_size>1000:
            # Wait for the native writer to finish, not just for path creation.
            size=path.stat().st_size;time.sleep(.1)
            if path.stat().st_size==size:break
        time.sleep(.1)
    if not path.exists():raise RuntimeError("Missing native capture "+str(path))
    shutil.copy2(path,RUN/"screenshots"/name)
    report["nativeScreenshots"].append(name)


def click(x,y):cmd(f"menu pointer {x} {y}")


try:
    cmd("menu 1")
    initial=state();original_lang=initial["language"]
    for tab in range(1,7):
        cmd(f"menu {tab}")
        for setting in state()["settings"]:
            if setting["key"] not in action_keys:original[setting["key"]]=setting["value"]
    check("Action keys excluded from value restoration",not (original.keys() & action_keys))
    for key in sorted(action_keys):
        check("set rejects action "+key,send(f"set {key} 0").startswith("ERR action requires explicit do:"))
        check("get rejects action "+key,send(f"get {key}").startswith("ERR action requires explicit do:"))
    check("Character's 208 stats are available","208" in cmd("status"))
    for lang,x in (("tr",637),("en",676)):
        cmd("menu 1");click(x,49)
        check("Language button "+lang,state()["language"]==lang)
        for tab in range(1,8):
            # Use actual tab hit testing, rather than only the direct page command.
            click(105,144+(tab-1)*37)
            snap=state();report["snapshots"].append(snap)
            check(f"{lang} tab {tab}",snap["tab"]==tab and snap["open"])
            check(f"{lang} viewport fits panel {tab}",snap["scale"]*820<=snap["guiWidth"] and snap["scale"]*508<=snap["guiHeight"])
            if tab<=6:
                check(f"{lang} complete labels {tab}",all(s["label"] and s["description"] for s in snap["settings"]))
            shot(f"{lang}-tab{tab}")
        if lang=="en":
            cmd("menu 1");check("English setting and description",state()["settings"][0]["label"]=="EXP multiplier")
    cmd("menu 1")
    old=original["exp"]
    click(574,175);after_plus=state()["settings"][0]["value"]
    click(494,175);after_minus=state()["settings"][0]["value"]
    check("Distinct plus and minus hit areas",after_plus>old and after_minus==old)
    click(535,175);check("Number well does not accidentally change value",state()["settings"][0]["value"]==old)
    click(534,343);check("Toggle on",state()["settings"][4]["value"]==1-original["perfect"])
    click(534,343);check("Toggle restored",state()["settings"][4]["value"]==original["perfect"])
    cmd("set loot 1");click(494,259);check("Disabled lower endpoint stays at 1",state()["settings"][2]["value"]==1)
    cmd("set loot 10");click(574,259);check("Upper endpoint stays at 10",state()["settings"][2]["value"]==10)
    cmd(f"set loot {original['loot']}")
    cmd("menu key tab");check("Keyboard next tab",state()["tab"]==2)
    cmd("menu key tab shift");check("Keyboard previous tab",state()["tab"]==1)
    cmd("menu key f8");check("F8 closes",not state()["open"])
    cmd("menu key f8");check("F8 reopens",state()["open"])
    click(752,49);check("Header close button",not state()["open"])
    cmd("menu 7")
    cmd("menu search");cmd("menu group 0")
    base=state();check("Fourteen visible stat rows",len(base["visibleStats"])==14)
    cmd("menu key pgdn");check("Stat paging follows selection",state()["statScroll"]>0)
    cmd("menu key pgup");check("Stat page returns to top",state()["statScroll"]==0)
    for group,x in ((1,427),(2,575),(3,723),(0,279)):
        click(x,185);snap=state()
        check(f"Stat group {group}",snap["group"]==group and snap["filteredStats"]>0)
    check("All 208 rows remain reachable",state()["filteredStats"]==208)
    click(290,151);check("Search input focus",state()["searchFocused"])
    cmd("menu type crit");snap=state()
    check("Typed search filters names/aliases",snap["search"]=="crit" and 0<snap["filteredStats"]<208)
    shot("en-stat-search")
    cmd("menu key back");check("Search Backspace edits query",state()["search"]=="cri")
    cmd("menu key esc");check("Search Esc clears without closing",state()["search"]=="" and state()["open"] and not state()["searchFocused"])
    click(637,49);cmd("menu search saldiri")
    check("Turkish accent-insensitive search",any(s["alias"]=="attackspeed" for s in state()["visibleStats"]))
    cmd("menu search qqqqq-no-such-stat");check("Empty search result is safe",state()["filteredStats"]==0)
    shot("tr-stat-no-results")
    cmd("menu search");cmd("menu group 1")
    before=cmd("stat maxhp")
    click(280,461);time.sleep(.3)
    check("Stat +1 routes to existing editor",cmd("stat maxhp")!=before)
    click(230,461);time.sleep(.3)
    check("Stat -1 restores original bonus",cmd("stat maxhp")==before)
    cmd("menu group 0");cmd("menu 5");cmd("set deathrecap 1")
    click(285,400);check("Open recorded death from Interface",state()["recapOpen"])
    shot("tr-empty-recap");click(717,144);check("Close recorded death overlay",not state()["recapOpen"])
    cmd("menu off");cmd("set dps 1");shot("tr-dps-hud")
    cmd("menu preview-recap");shot("tr-death-recap-10-row-fixture")
    # Preview is a separate four-second renderer fixture, not fabricated combat evidence.
    report["deathRecapEvidence"]="native renderer with empty real history and isolated ten-row fixture; no player killed"
    cmd("menu language en");cmd("menu 1")
    report["passed"]=True
finally:
    for key,value in original.items():
        try:cmd(f"set {key} {value}")
        except Exception as e:report.setdefault("restoreErrors",[]).append(str(e))
    try:report["finalState"]=state()
    except Exception as e:report.setdefault("restoreErrors",[]).append(str(e))
    report["passed"]=report["passed"] and not report.get("restoreErrors")
    report["languageLeftForRestartCheck"]="en"
    (RUN/"native-ui-tests.json").write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf8")
    print(json.dumps({"passed":report["passed"],"checks":len(report["checks"]),"screenshots":len(report["nativeScreenshots"]),"report":str(RUN/"native-ui-tests.json")},ensure_ascii=False))
