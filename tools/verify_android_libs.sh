#!/usr/bin/env bash
# Post-link checks on libpes17.so (and any apk given as $2), borrowed from the
# Skate3-Port build: a library linked for 4 KB pages does not load on Android 15
# devices that use 16 KB pages, and it fails at install time on the phone, not here.
#   usage: verify_android_libs.sh <lib.so> [app.apk]
set -euo pipefail
lib=${1:?usage: verify_android_libs.sh <lib.so> [app.apk]}
NDK=${ANDROID_NDK_ROOT:-$(ls -d "$HOME"/Library/Android/sdk/ndk/28.* 2>/dev/null | tail -1)}
readelf=$(find "$NDK/toolchains/llvm/prebuilt" -path '*/bin/llvm-readelf' -print -quit)
saw=false
while IFS= read -r alignment; do
    saw=true
    (( alignment >= 0x4000 )) || { echo "FAIL: $(basename "$lib") has a LOAD segment aligned below 16 KB ($alignment)"; exit 1; }
done < <("$readelf" -lW "$lib" | awk '$1 == "LOAD" { print $NF }')
$saw || { echo "FAIL: no LOAD segments in $lib"; exit 1; }
echo "ok: $(basename "$lib") LOAD segments are 16 KB aligned"
# No ARMv8.0-only outline atomics should be left when built for ARMv8.2.
if "$readelf" -sW "$lib" | grep -q '__aarch64_\(cas\|ldadd\|swp\|ldset\|ldclr\)'; then
    echo "note: $(basename "$lib") still calls outline-atomics helpers (built without PES_ARMV82?)"
fi
if [ -n "${2:-}" ]; then
    zipalign=$(ls -d "${ANDROID_HOME:-$HOME/Library/Android/sdk}"/build-tools/*/zipalign 2>/dev/null | tail -1)
    "$zipalign" -c -P 16 4 "$2" && echo "ok: $(basename "$2") native libraries are 16 KB zip-aligned"
fi
