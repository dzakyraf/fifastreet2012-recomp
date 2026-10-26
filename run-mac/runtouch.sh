#!/bin/zsh
# usage: runtouch.sh <name> <at seconds> <total seconds> <file to touch in files/>; MACRO as runfix.sh
n=$1; at=$2; secs=$3; what=$4
cd "$(dirname $0)"; rm -rf $n; mkdir -p $n/files; cd $n
G=$(ls -d ../../../gamefiles/*/ | head -1); G=${G%/}
PES17_FILES=$PWD/files PES17_LOG_TIME=1 PES17_SHOT_EVERY=${SHOT_EVERY:-0} PES17_INPUT_SCRIPT="${MACRO:-17:start,19:start,20:a}" \
  ../../build-mac/pes17 "$G/default.xex" "$G" > run.log 2>&1 &
P=$!; sleep $at; touch files/$what; sleep $((secs - at)); kill $P 2>/dev/null; wait $P 2>/dev/null
