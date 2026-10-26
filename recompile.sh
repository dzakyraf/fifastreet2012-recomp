#!/bin/bash
# FIFA Street (Xbox 360) -> C++ via XenonRecomp, one code module at a time.
# Same pipeline as pes2017-android/build.sh steps 1-7, run for each module:
#   exe  default.xex      small loader, base 0x82000000
#   dll  fifadllzf.xex    the game,     base 0x82300000 (loaded by the exe at run time)
#
#   MODULES="exe dll"  which modules to recompile (default both)
#   PROFILE=safe|balanced|aggressive   register locals, as in the PES build
#   GAME=<extracted disc folder>       default ../gamefiles/FIFA Street [RF]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd); cd "$ROOT"
GAME=${GAME:-$(ls -d "$ROOT"/../gamefiles/FIFA*/ 2>/dev/null | head -1)}
MODULES=${MODULES:-"exe dll"}
PROFILE=${PROFILE:-balanced}
CMAKE_CC=${CMAKE_CC:-/usr/bin/clang}; CMAKE_CXX=${CMAKE_CXX:-/usr/bin/clang++}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
unset PYTHONPATH PYTHONHOME

case "$PROFILE" in
    safe)       LOCALS="" ;;
    balanced)   LOCALS="ctr_as_local xer_as_local reserved_as_local cr_as_local" ;;
    aggressive) LOCALS="skip_lr skip_msr ctr_as_local xer_as_local reserved_as_local cr_as_local non_argument_as_local non_volatile_as_local" ;;
    *) echo "PROFILE must be safe, balanced or aggressive"; exit 1 ;;
esac

XENON=third_party/XenonRecomp
echo "==> build XenonRecomp / XenonAnalyse"
cmake -B $XENON/build -S $XENON -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=$CMAKE_CC -DCMAKE_CXX_COMPILER=$CMAKE_CXX >/dev/null 2>&1
cmake --build $XENON/build -j"$JOBS" >/dev/null 2>&1

mkdir -p fifa logs
[ -f fifa/default.xex ]   || cp "$GAME/default.xex" fifa/default.xex
[ -f fifa/fifadllzf.xex ] || cp "$GAME/fifadllzf.xex.dll" fifa/fifadllzf.xex

activate() {   # config/<m>.toml with the profile's locals switched on -> config/<m>.active.toml
    python3 - "config/$1.toml" "config/$1.active.toml" $LOCALS <<'PY'
import re, sys
s = open(sys.argv[1]).read()
for key in sys.argv[3:]:
    s = re.sub(rf"^{key} = false", f"{key} = true", s, flags=re.M)
open(sys.argv[2], "w").write(s)
PY
}

for M in $MODULES; do
    export FIFA_MODULE=$M
    # The last output, kept so unchanged files get their old times back at the end
    # (tools/keep_mtimes.py): most generated units do not change between runs.
    rm -rf ppc/.$M.previous; [ -d ppc/$M ] && cp -pR ppc/$M ppc/.$M.previous
    XEX=$(cd tools && python3 -c "import title; print(title.XEX)")
    PE=${XEX%.xex}.pe
    TABLES=config/${M}_switch_tables.toml
    LOG=logs/recomp-$M.txt
    echo "==> [$M] decrypt/decompress XEX -> flat PE image"
    [ -f "$PE" ] || python3 tools/xexdump.py "$XEX" "$PE" >"logs/xexdump-$M.txt"

    echo "==> [$M] save/restore helpers, jump tables"
    python3 tools/find_helpers.py --write config/$M.toml >/dev/null
    ./$XENON/build/XenonAnalyse/XenonAnalyse "$XEX" $TABLES >logs/analyse-$M.txt 2>&1
    python3 tools/trim_switch_tables.py $TABLES
    python3 tools/fix_switch_regs.py $TABLES --apply >/dev/null || true
    python3 tools/find_missed_switches.py $TABLES config/${M}_switch_extra.toml
    cat config/${M}_switch_extra.toml >> $TABLES
    echo "    tables: $(grep -c '^\[\[switch\]\]' $TABLES)"
    # Functions reached only through vtables: entry points for the recompiler (tools/vtable_starts.py).
    python3 tools/vtable_starts.py --write config/${M}_function_starts.txt || true

    # Function bounds the analyser cannot derive; each pass can only grow a bound.
    echo "==> [$M] recompile PPC -> C++ (profile: $PROFILE)"
    changed=0
    for pass in 1 2 3 4 5 6; do
        activate $M
        rm -rf ppc/$M && mkdir -p ppc/$M
        ( cd config && ../$XENON/build/XenonRecomp/XenonRecomp $M.active.toml ../$XENON/XenonUtils/ppc_context.h ) >$LOG 2>&1
        changed=0
        python3 tools/fixbounds.py $TABLES $LOG --apply config/$M.toml >/dev/null || changed=1
        python3 tools/fix_ladders.py ppc/$M --apply config/$M.toml >/dev/null 2>&1 || changed=1
        echo "    pass $pass: jump-table errors $(grep -c 'trying to jump outside function' $LOG || true)," \
             "undecodable words $(grep -c 'Unable to decode' $LOG || true)$([ $changed = 1 ] && echo ', bounds changed -> again')"
        [ $changed = 0 ] && break
    done
    [ $changed = 0 ] || { echo "[$M] bounds did not converge; see $LOG"; exit 1; }
    echo "$PROFILE" > ppc/$M/PROFILE

    echo "==> [$M] declarations + kernel import list"
    python3 tools/genshared.py
    python3 tools/gen_kernel_stubs.py | head -1
    echo "    ppc/$M: $(ls ppc/$M | wc -l | tr -d ' ') files"
done

# Both modules link into one library: unique mapping tables, one code span, one set of stubs.
if [ -f ppc/exe/ppc_func_mapping.cpp ] && [ -f ppc/dll/ppc_func_mapping.cpp ]; then
    echo "==> merge modules"
    ( cd tools && python3 merge_modules.py )
fi

for M in $MODULES; do
    [ -d ppc/.$M.previous ] || continue
    python3 tools/keep_mtimes.py ppc/$M ppc/.$M.previous
    rm -rf ppc/.$M.previous
done
