#!/usr/bin/env bash
# The runtime's own log on the phone (runtime/android/log.cpp writes every line
# to game.log in the app's folder, line-buffered, a marker per launch): faster
# to read than logcat and holds only the game's lines.
#
#   tools/logtail.sh                 follow it live (Ctrl-C to stop)
#   tools/logtail.sh follow PATTERN  follow, keeping lines matching PATTERN (grep -E)
#   tools/logtail.sh run [PATTERN]   the current run so far, optionally filtered
#   tools/logtail.sh errors          the current run's warnings and errors, deduplicated
set -euo pipefail
LOG=/sdcard/Android/data/com.fifastreet.recomp/files/game.log
# The file only grows (it is trimmed when the app opens it), so the run is found
# on the phone and only its lines cross adb: the last marker's line number, then tail.
current_run() {
    local n; n=$(adb shell "grep -n '^===== new run' $LOG | tail -1 | cut -d: -f1")
    adb exec-out "tail -n +${n:-1} $LOG"
}
case "${1:-follow}" in
    follow)
        if [ -n "${2:-}" ]; then adb exec-out tail -n 0 -f "$LOG" | grep --line-buffered -E "$2"
        else adb exec-out tail -n 40 -f "$LOG"; fi ;;
    run)
        if [ -n "${2:-}" ]; then current_run | grep -E "$2" || true; else current_run; fi ;;
    errors)
        current_run | grep -E "\[[wWeE]\]" | grep -vE "sampled|\[E\] +#|-----" | sed -E 's/[0-9A-F]{8}/X/g' | sort | uniq -c | sort -rn | head -40 ;;
    *) echo "usage: $0 [follow [PATTERN] | run [PATTERN] | errors]"; exit 1 ;;
esac
