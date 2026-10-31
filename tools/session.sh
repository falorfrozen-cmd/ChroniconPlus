#!/usr/bin/env bash
# Test session driver: (re)start Chronicon with the built core, wait for the auto-loaded character, then run each
# argument as a ChroniconPlus command ("sleep N" waits). Prints replies and whether the game is still running.
#   bash session.sh "keepalive on" "call world_goto @player 2" "sleep 10" "fight 5 50"
set -u
GAME="$(cd "$(dirname "$0")/../.." && pwd)"
cp_() { python -I "$GAME/_ModKit/tools/cp.py" "$@"; }
alive() { tasklist //FI "IMAGENAME eq Chronicon.exe" | grep -qi chronicon; }

if [ "${RESTART:-1}" = 1 ]; then
    taskkill //IM Chronicon.exe >/dev/null 2>&1
    for i in $(seq 1 10); do alive || break; sleep 1; done
    alive && taskkill //F //IM Chronicon.exe >/dev/null 2>&1
    sleep 2
    cp "$GAME/_ModKit/src/ChroniconNative/build/ChroniconPlus.Core.dll" "$GAME/ChroniconPlus/"
    cmd //c start "" "steam://rungameid/375480"
    sleep 6
    for i in $(seq 1 50); do
        grep -qE "\[load\] (character loaded|gave up|the |no )" "$GAME/ChroniconPlus/ChroniconPlus.log" 2>/dev/null && break
        if ! alive && [ "$i" -gt 5 ]; then echo "GAME EXITED DURING LOAD"; exit 1; fi
        sleep 2
    done
    for i in $(seq 1 20); do cp_ status 2>/dev/null | grep -q "yüklü" && break; sleep 2; done
fi

for step in "$@"; do
    case "$step" in
    sleep\ *) sleep "${step#sleep }" ;;
    *) echo "> $step"; cp_ "$step" | head -40 ;;
    esac
    if ! alive; then echo "GAME EXITED after: $step"; exit 1; fi
done
echo "game running"
