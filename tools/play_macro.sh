#!/usr/bin/env bash
# Restart the game on the phone and play a macro once the title is up.
#   usage: tools/play_macro.sh <name> [title-wait-seconds=45]
# The macro is pushed from tools/macros/<name>.macro first. It starts counting when it is played, so
# the first press should be the one that leaves the title screen.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
NAME=${1:?macro name}; WAIT=${2:-45}
F=/sdcard/Android/data/com.fifastreet.recomp/files
adb push "tools/macros/$NAME.macro" "$F/macros/$NAME.macro" >/dev/null
adb shell touch "$F/macros/.remote_control"
adb shell am force-stop com.fifastreet.recomp
adb shell am start -n com.fifastreet.recomp/com.pes2017.recomp.MainActivity >/dev/null
sleep "$WAIT"
adb shell am broadcast -a com.pes2017.recomp.MACRO --es cmd play --es name "$NAME" >/dev/null
echo "playing $NAME"
