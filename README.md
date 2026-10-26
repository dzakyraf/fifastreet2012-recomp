# FIFA Street (Xbox 360) → Android, via XenonRecomp

Same approach as `../../PES2017-RECOMP/pes2017-android`: XenonRecomp turns the PowerPC code
into C++, and the NDK builds it for arm64. The XenonRecomp copy in `third_party/XenonRecomp`
comes from that project, with its local changes. The tools in `tools/` come from there too,
made module-aware.

## What is different from PES 2017: two code modules

| module | file on disc | base | `.text` | functions |
|---|---|---|---|---|
| `exe` | `default.xex` | `0x82000000` | `0x82050000` +`0x119748` | 7,394 |
| `dll` | `fifadllzf.xex.dll` (the game) | `0x82300000` | `0x82610000` +`0x164318C` | 156,893 |

- **Imports:** both import only `xboxkrnl.exe` and `xam.xex`, 321 kernel imports in all.
- **Loading the DLL:** the exe loads the DLL at run time through `XexLoadImage` and
  `XexGetProcedureAddress`. The DLL has an `.edata` export table.
- **No relocation:** the images don't overlap, so each module is recompiled at its own base.

`tools/merge_modules.py` makes the two outputs link into one library. It does four things:

- **Mapping tables:** renames them to `PPCFuncMappings_exe` and `PPCFuncMappings_dll`.
- **Shared names:** adds an `_exe` suffix to the exe's copies of the names both modules define.
  These are the register save/restore helpers and `_xstart`.
- **Address range:** sets `PPC_IMAGE_*` and `PPC_CODE_*` to one range covering both modules,
  so indirect calls work in either direction. The per-module values stay as `FIFA_EXE_*` and
  `FIFA_DLL_*`.
- **Runtime outputs:** writes `ppc/ppc_modules.h`, the module table for `XexLoadImage`. It also
  writes one `ppc/stubs_generated.cpp` for the union of both modules' imports.

## Build and run

```bash
./recompile.sh
```

`./recompile.sh` takes about 3 minutes and writes `ppc/exe` and `ppc/dll`, then merges them.
`MODULES=dll` limits it to one module. Files whose text did not change keep their timestamps,
so ninja only rebuilds what changed. A change to `ppc_context.h` (XenonRecomp's own header)
still rebuilds everything, about 25 minutes.

```bash
NDK=$(ls -d ~/Library/Android/sdk/ndk/28.* | tail -1)
cmake -S . -B build-android -G Ninja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared \
      -DPES17_DIAGNOSTICS=OFF -DPES17_USE_SDL=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG"
```

```bash
ANDROID_SERIAL=RR8RB04X2SV tools/deploy.sh 60
```

`tools/deploy.sh` builds `libpes17.so`, packages it through Gradle, installs
`com.fifastreet.recomp` and launches it. It then saves the run's log to `logs/device.txt`.
`tools/logtail.sh run|errors|follow` reads `game.log` on the device.

The game folder has to be at `/sdcard/Android/data/com.fifastreet.recomp/files/game/`. It is
the whole disc except `$SystemUpdate`: 104 files, 4.3 GB. `adb push` cannot create
subdirectories there, so make them with `adb shell mkdir -p` first. The game folder name on
the Mac has a non-breaking space before `[RF]`, so `recompile.sh` finds it with a glob. Pass
`GAME=` to override that.

## The runtime (from pes2017-android)

`runtime/` and `android/` are pes2017-android's. Internal names are unchanged: `libpes17.so`,
`PES17_*`, and the Kotlin package `com.pes2017.recomp`. Only the `applicationId`
(`com.fifastreet.recomp`) and the app name differ, so FIFA installs beside PES. These parts
are FIFA-specific:

- **`runtime/title.h`:** FIFA's entry points. The PES hook addresses are not defined, so
  those hooks compile out.
- **`runtime/kernel/modules.cpp`:** maps a module at its base and copies its XEX header in. It
  resolves that module's data imports, fills its slice of the function table, and builds an
  `LDR_DATA_TABLE_ENTRY` as its handle. `main.cpp` maps `default.xex` this way.
- **`kernel/misc.cpp`:**
  - `XexLoadImage` maps the DLL from the game folder and runs
    `DllMain(handle, DLL_PROCESS_ATTACH, 0)`.
  - `XexGetProcedureAddress` resolves ordinals through the XEX security info's export table.
  - `XexGetModuleHandle` and `XexGetModuleSection` are implemented too.
- **`kernel/pes_imports.cpp` `ResolveDataImports(header)`:** runs once per module.
  `KeTimeStampBundle` and `KeDebugMonitorData` are shared objects. The old filter treated the
  record's library byte as a constant, so it left xam's import slots unresolved. It now reads
  that byte as the library index.
- **Installer:** FIFA's title ID (`0x4541097B`) and its `default.xex` SHA-256. It recognises
  the `.big`/`.bh` archives and the DLL. Both installers (the folder import in
  `InstallActivity.kt` and the ISO path in `install.cpp`) still copy only top-level files, not
  `data/` and `dlc/`.

## Changes to XenonRecomp and the tools

- **`bdz`/`bdnz`/`bdnzf` to a target outside the function:** now a tail call, as the other
  conditional branches already were. Before, they emitted a `goto` to a label that does not
  exist.
- **Newly implemented instructions:** these used to compile to `__builtin_debugtrap()` and
  crashed the game's vector trig routine.
  - `vcmpbfp`/`vcmpbfp128`, the bounds compare.
  - `vupkd3d128` `float16_2` and `float16_4`, the inverses of the existing packs.
  - `blrl`.
- **Vector and integer instructions that were silently dropped:** about 1,800 sites in the
  game DLL hit `Unrecognized instruction`. XenonRecomp prints that line and emits
  nothing, so the instruction just does not happen. They were the pixel arithmetic of the game's
  video decoder, and the boot logo video came out as corrupt Y/Cb/Cr planes (green, striped).
  Now implemented: `vsububm`, `vsubuwm`, `vsubsbs`, `vaddsbs`, `vmaxsb`, `vminsb`, `vmaxub`,
  `vminub`, `vminsw`, `vsrb`, `vcmpgtsw`, `vcmpgtsb`, `vcmpequh`, `vsl`, `vslo`/`vslo128`,
  `vsro`/`vsro128`, the saturating packs `vpkuhus128`, `vpkuwus128`, `vpkuwum128`,
  `vpkshss128`, and the scalar `lhaux` and `lfdux`. The whole-vector shifts and the saturating
  narrowing helpers are in `XenonUtils/ppc_context.h`, which means a change there rebuilds the
  whole game (about 25 minutes). After this, `grep "Unrecognized" logs/recomp-*.txt` shows only
  3 words in the dll (below).
- **Functions reached only through a vtable (`function_starts_file_path`):** a method that is only
  called through a vtable has no `.pdata` entry and is no `bl` target, so the recompiler folded it
  into the function before it and a call through the table found no code ("indirect call to
  8338F5B8 has no recompiled function", 23,000 times; the front end looped on it and no menu was ever
  built). `tools/vtable_starts.py` finds them (a code pointer in the data, right after a `blr` or an
  unconditional `b`, one of a run of code pointers, not a jump-table label), `recompile.sh` writes
  `config/<m>_function_starts.txt`, and XenonRecomp analyses each entry like a call target. About 16,000
  such starts in the dll, 51 of them swallowed; `fixbounds.py` shares the same finder. The old rule
  accepted only a `blr` before the entry, which missed everything ending in a tail jump.
- **`find_missed_switches.py`:**
  - It now finds unguarded tables of halfword offsets from a base label (`lhzx … add; mtctr;
    bctr`). FIFA has 2: `826E6E28` and `83AA132C`. Without them the job threads called into
    the middle of a function and crashed.
  - It no longer walks back across a basic block's end. Before, a call through a
    function-pointer struct (`83B89FB8`) became a one-label "switch".
- **`fix_ladders.py`:** a branch that XenonRecomp could only write as `// ERROR` plus a silent
  `return` now gets its own function, when the target block runs straight to its `blr`. These
  are the leaf ladders' shared tails and MSVC's exception-handling epilogues. Each module
  still has some left that branch internally: exe 6, dll 17, mostly CRT/SEH unwinding.

## State (2026-10-26)

The match plays on the Galaxy S20 FE and on the Mac, with sound, saving and the broadcast camera:
58-61 FPS in play on the phone, 46-51 in the match intro and goal scenes, 60 on the Mac. How to
build, install and run is in the [top-level README](../README.md); every fix since the state below
(saving, clubs, XMA audio, tiling, the camera, frame rate, the audio crackle) is listed in
[CLAUDE.md](../CLAUDE.md) under "Fixed", with what is still open.

### Debugging aids added

- `touch .../files/trace_all`: every XAM/kernel import is logged with its arguments and result (`->`
  lines), not just the first three calls.
- `touch .../files/capture_request` (the CAPTURE button): prints every thread's stack. On Android it
  now works (SIGURG), but the frame-pointer walk is unreliable for guest frames. **For real stacks
  use simpleperf on the rooted phone**: `simpleperf record -p <pid> --call-graph fp --duration 6`,
  then `report --comms "guest thread N" --sort symbol --children` (see how thread 26 was found
  spending 96% of its time printing backtraces for null calls).
- `touch .../files/dump_frame` also writes `plane_*.bin` (raw 8-bit video planes) and `raw_*.bin`
  (raw DXT5 texture bytes) next to the frame images.
- Cross-thread APCs (`NtQueueApcThread`, `KeInitializeApc`, `KeInsertQueueApc`; `kernel/apc.cpp`) are
  implemented. They were not what the front end needed, but the game's `SleepEx(INFINITE)` workers
  expect them.

## Next

See "Open" in [CLAUDE.md](../CLAUDE.md). Still untouched from the original list: the DLC module
(`FootballCompEngzf.xex.dll`) is not recompiled, and the installers do not copy `data/` and `dlc/`.
