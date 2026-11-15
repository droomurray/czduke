#!/bin/sh
# Usage: tests/smoke.sh <czduke-binary> <dir-with-duke3d.grp> <output-dir>
#
# Runs the game off-device: a plain file stands in for the 320x170 RGB565
# panel and a file of timed evdev events stands in for the keyboard. Writes
# PNG screenshots and fails if the picture is blank or the keys had no effect.
set -eu

BIN="$(realpath "$1")"
GRPDIR="$(realpath "$2")"
OUT="$3"
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
mkdir -p "$OUT"

export SDL_AUDIODRIVER=dummy CZDUKE_FAKEFB=320x170x16 DUKE3DGRP=duke3d.grp

run() { # name frames keys... -- engine args
    name="$1"; frames="$2"; shift 2
    keys=""
    while [ "$1" != "--" ]; do keys="$keys $1"; shift; done
    shift
    mkdir -p "$WORK/$name"
    python3 "$HERE/evkeys.py" "$WORK/$name.ev" $keys
    : >"$WORK/$name.fb"
    : >"$WORK/$name/user_profiles_disabled"
    cp "$HERE/../packaging/duke3d.cfg" "$WORK/$name/duke3d.cfg"
    ( cd "$WORK/$name" && CZDUKE_FB="$WORK/$name.fb" CZDUKE_KBD="$WORK/$name.ev" CZDUKE_MAX_FRAMES="$frames" \
        timeout 120 "$BIN" "-j$GRPDIR" "$@" >"$OUT/$name.log" 2>&1 ) || { cat "$OUT/$name.log"; exit 1; }
    python3 "$HERE/fb2png.py" "$WORK/$name.fb" 320 170 "$OUT/$name.png"
}

LEVEL="-v1 -l1 -s2 -m"

# 1. start-up: the title screen, then the main menu
run title 200 --
run menu 450 --
# 2. first level, no input
run level-idle 300 -- $LEVEL
# 3. first level, holding D (turn right): the view must differ from run 2
run level-turn 300 8500:32:1 -- $LEVEL
# 4. first level, holding K (fire): the weapon must differ from run 2
run level-fire 300 9700:37:1 -- $LEVEL
# 5. 4:3 mode
CZDUKE_ASPECT=4:3 run aspect43 300 -- $LEVEL

# 6. main menu: S (which plays as the down arrow) moves the selection
run menu-down 450 21500:31:1 21800:31:0 --

grep -h "stopping after" "$OUT"/*.log
python3 "$HERE/check.py" "$WORK"
grep -h "^czduke:\|^czaudio:\|GRP file" "$OUT/level-turn.log"

# The launcher wrapper, when the package's files are installed (the Dockerfile
# does that inside the test container).
WRAPPER=/usr/share/APPLaunch/bin/czduke
if [ -x "$WRAPPER" ]; then
    # 7. no game data: a clear message, not a crash
    if HOME="$WORK/home1" "$WRAPPER"; then echo "wrapper started without a GRP"; exit 1; fi
    grep "duke3d.grp not found" /tmp/czduke.log

    # 8. no usable framebuffer: also a clear message
    mkdir -p "$WORK/home2/duke3d"
    ln -s "$GRPDIR/duke3d.grp" "$WORK/home2/duke3d/DUKE3D.GRP"
    if HOME="$WORK/home2" CZDUKE_FB="$WORK/missing.fb" CZDUKE_KBD="$WORK/title.ev" "$WRAPPER"; then
        echo "game started without a framebuffer"; exit 1
    fi
    grep "cannot open .*missing.fb" /tmp/czduke.log

    # 9. real-time run through the wrapper, GRP under another spelling, ended
    #    with SIGTERM the way the launcher's Home key ends it
    #    and found in the web file manager's upload folder
    mkdir -p "$WORK/home3/CardputerFiles"
    ln -s "$GRPDIR/duke3d.grp" "$WORK/home3/CardputerFiles/DUKE3D.GRP"
    : >"$WORK/wrapper.fb"
    HOME="$WORK/home3" CZDUKE_FB="$WORK/wrapper.fb" CZDUKE_KBD="$WORK/title.ev" "$WRAPPER" &
    pid=$!
    sleep 5
    kill -TERM "$pid"
    wait "$pid" || { echo "wrapper run failed"; cat /tmp/czduke.log; exit 1; }
    cp /tmp/czduke.log "$OUT/wrapper.log"
    python3 "$HERE/fb2png.py" "$WORK/wrapper.fb" 320 170 "$OUT/wrapper.png"
    grep "GRP file: DUKE3D.GRP" /tmp/czduke.log
    CFG="$WORK/home3/.local/share/czduke/duke3d.cfg"
    grep -q "KeyDefinitions" "$CFG" || { echo "settings were not saved on SIGTERM"; exit 1; }
    grep -q "^RunMode = 1" "$CFG" || { echo "starting settings were not applied"; exit 1; }
fi
echo "smoke test passed"
