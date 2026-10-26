#!/bin/zsh
# usage: runvar.sh <variant> ; replays the recorded presses, logs to var<N>/run.log
v=$1
cd "$(dirname $0)"; rm -rf var$v; mkdir -p var$v/files; cd var$v
G=$(ls -d ../../../gamefiles/*/ | head -1); G=${G%/}
PES17_ENUM_VARIANT=$v PES17_FILES=$PWD/files PES17_LOG_TIME=1 \
PES17_INPUT_SCRIPT="15.4:a,16.2:back,20.4:a,21.6:start,54.3:start,54.7:start,55.7:a" \
  ../../build-mac/pes17 "$G/default.xex" "$G" > run.log 2>&1 &
P=$!; sleep 100; kill $P 2>/dev/null; wait $P 2>/dev/null
