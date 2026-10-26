#!/bin/zsh
# usage: runtouches.sh <name> <total seconds> <file> <t1> [t2 ...]; MACRO as runfix.sh
n=$1; secs=$2; what=$3; shift 3
cd "$(dirname $0)"; rm -rf $n; mkdir -p $n/files; cd $n
G=$(ls -d ../../../gamefiles/*/ | head -1); G=${G%/}
PES17_FILES=$PWD/files PES17_LOG_TIME=1 PES17_SHOT_EVERY=${SHOT_EVERY:-0} PES17_INPUT_SCRIPT="${MACRO:-17:start,19:start,20:a}" \
  ../../build-mac/pes17 "$G/default.xex" "$G" > run.log 2>&1 &
P=$!; now=0
for t in "$@"; do sleep $((t - now)); touch files/$what; now=$t; done
sleep $((secs - now)); kill $P 2>/dev/null; wait $P 2>/dev/null
