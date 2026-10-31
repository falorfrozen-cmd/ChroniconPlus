#!/usr/bin/env bash
# Runs the native UI suite at several window sizes by editing the game's options.cfg (restored afterwards).
#   bash resolution_check.sh "1920x1080 16:9" "1680x1050 16:10" "1280x720 16:9"
set -u
GAME="$(cd "$(dirname "$0")/../.." && pwd)"
CFG="$LOCALAPPDATA/Chronicon/options.cfg"
cp "$CFG" "$CFG.rescheck-backup"
restore() { taskkill //IM Chronicon.exe >/dev/null 2>&1; sleep 4; cp "$CFG.rescheck-backup" "$CFG"; rm -f "$CFG.rescheck-backup"; }
trap restore EXIT

for spec in "$@"; do
    size="${spec% *}"; aspect="${spec#* }"
    taskkill //IM Chronicon.exe >/dev/null 2>&1; sleep 4
    sed -i -e "s/^fullscreen=.*/fullscreen=\"0\"/" -e "s/^windowed_resolution=.*/windowed_resolution=\"$size\"/" \
        -e "s/^aspect_ratio=.*/aspect_ratio=\"$aspect\"/" "$CFG"
    run="$GAME/_ModKit/design/release-check-$(date +%Y%m%d-%H%M%S)-${size}"
    mkdir -p "$run"
    printf '%s' "$(cygpath -w "$run")" > "$GAME/_ModKit/design/current-vault-run.txt"
    echo "=== $size $aspect"
    bash "$GAME/_ModKit/tools/session.sh" "menu 1" >/dev/null 2>&1
    python -I "$GAME/_ModKit/tools/cp.py" "menu inspect" | grep -o '"scale":[0-9.]*,"guiWidth":[0-9]*,"guiHeight":[0-9]*'
    (cd "$GAME/_ModKit/tools" && timeout 300 python check_vault_ui.py 2>&1 | tail -2)
    grep -ac "\[fault\]" "$GAME/ChroniconPlus/ChroniconPlus.log"
done
