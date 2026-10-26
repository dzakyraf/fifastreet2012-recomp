#!/bin/zsh
n=$1
cd "$(dirname $0)"; rm -rf $n; mkdir -p $n/files; cd $n
G=$(ls -d ../../../gamefiles/*/ | head -1); G=${G%/}
PES17_FILES=$PWD/files PES17_LOG_TIME=1 PES17_INPUT_SCRIPT="${MACRO}" ../../build-mac/pes17 "$G/default.xex" "$G" > run.log 2>&1 &
P=$!; sleep ${FIND_AT:-98}; echo "90000000 8000000" > files/dump_guest; sleep 3
A=$(python3 - <<'PY'
import mmap, struct
f=open('files/guest_dump.bin','rb'); m=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
# view matrix row 0x93b9f310 pattern: (tiny, 0.7071, 0.7071, 0) then camera pos 0xF0 later; find "row3 w=1.0" with z 40..50 and pos 0xC0 later with |x|,|y| ~ 20..40
for i in range(0, len(m)-0x100, 16):
    w=m[i+0x3C:i+0x40]
    if w!=b'\x3f\x80\x00\x00': continue
    z=struct.unpack('>f',m[i+0x38:i+0x3C])[0]
    if not (20<z<80): continue
    px,py=struct.unpack('>2f',m[i+0xF0:i+0xF8])
    if 10<abs(px)<80 and 10<abs(py)<80:
        print(hex(0x90000000+i+0xF0)); break
PY
)
echo "cam $A" > cam.txt
[ -n "$A" ] && B=$(printf "%X" $((A - ${BACK:-0}))); echo "$B 1 ${EXACT:-10} ${READS:-0}" > files/watch_guest
sleep ${WATCH_FOR:-4}; kill $P 2>/dev/null; wait $P 2>/dev/null
