#!/bin/zsh
# usage: runfix.sh <name> [seconds]; the user's macro, logs to <name>/run.log
n=$1; secs=${2:-120}
cd "$(dirname $0)"; rm -rf $n; mkdir -p $n/files; [ -n "$TRACE" ] && touch $n/files/trace_all; cd $n
G=$(ls -d ../../../gamefiles/*/ | head -1); G=${G%/}
PES17_FILES=$PWD/files PES17_LOG_TIME=1 PES17_INPUT_SCRIPT="${MACRO:-17:start,19:start,20:a}" \
  ../../build-mac/pes17 "$G/default.xex" "$G" > run.log 2>&1 &
P=$!; sleep $secs; kill $P 2>/dev/null; wait $P 2>/dev/null
