# FIFA Street (Xbox 360) recompilation: working notes

Goal: run FIFA Street on Android. Debug loop: run the same code natively on macOS (Vulkan via
MoltenVK), fix there, then rebuild for Android.

**Work in `app/` (XenonRecomp).** `app-rexglue/` is a second attempt; the user said not to use it.
`app/README.md` has the architecture (two modules: `default.xex` and `fifadllzf.xex.dll`) and the
Android build/deploy steps (`tools/deploy.sh`). Read it first.

Reference implementation: Xenia Canary's source is at
`/Users/macdzky/Development/playground/PES2017-RECOMP/xenia-canary` (`src/xenia/kernel/xam/xam_table.inc` names
xam ordinals; `xam_enum.cc`, `util/object_table.cc`, `xboxkrnl/xboxkrnl_audio_xma.cc`, `cpu/ppc/ppc_opcode_lookup_gen.cc`).
Compare with it before guessing at kernel behaviour.

## Run it on the Mac

```bash
cd app
cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
ninja -C build-mac pes17            # first build ~15 min (generated PPC code), then incremental
```

Needs Homebrew `molten-vk`, `vulkan-loader`, `llvm`, CMake, Ninja. `ppc/` (generated, 440 MB) must exist;
if not, `./recompile.sh` (about 3 min).

Scripted runs, from `app/run-mac/` (each makes `<name>/` with `run.log`, `files/`, `saves/`):

```bash
MACRO="26:start,27:start,30:a" PES17_SHOT_EVERY=2 ./runfix.sh s1 60       # run 60 s, a PNG every 2 s
python3 ../tools/shots_sheet.py s1/files/shots --from 20                  # one contact sheet of the shots
TRACE=1 MACRO=... ./runfix.sh s2 40                                       # with trace_all from the start
MACRO=... ./runtouch.sh f1 100 104 dump_frame                             # touch files/<x> at 100 s
MACRO=... ./runtouches.sh f2 130 dump_frame 90 110                        # ... at several times
MACRO=... ./runsample.sh h1 105 110                                       # macOS `sample` of all threads at 105 s
MACRO=... ./runboth.sh g1 104 108                                         # dump_frame + 2 GB guest dump at 104 s
```
`PES17_NO_BLEND=1` draws everything opaque (debug).

**The macro that reaches a match** (PRESS START appears ~24 s on this Mac, not 17 s):
`26:start,27:start,30:a` (autosave notice; settings save; main menu ~43 s), `46:a` HIT THE STREETS,
`50:a` 5-A-SIDE, `54:left,56:a` sides, `62:a` league, then `66:a,70:a,74:a,78:a,82:a,86:a` (teams, kick-off,
controls screen), `95:a` -> match running ~90 s.

- Window title shows elapsed seconds `t = N s`. **`PES17_INPUT_SCRIPT` uses that same clock**
  (seconds since process start), so a time read off the window can be put straight into a script.
  Names: `start a b x y up down left right back lb rb lt rt`, optional `:holdSeconds`. `"45:shot"` takes a screenshot.
- Screenshots (`runtime/gpu/vulkan/screenshot.h`): `"<t>:shot"` in the script, `PES17_SHOT_EVERY=<s>`,
  `touch $PES17_FILES/screenshot`, or F2. PNGs go to `$PES17_FILES/shots/shot_<sec>s_<why>.png`. Read them with the
  Read tool; for many, make a sheet with `tools/shots_sheet.py`.
- Keyboard: Return = START, Space/Z = A, X/C = B, arrows = D-pad, Esc = Back. Every newly pressed
  button is logged as `input: macro <sec>:<button>`, so a played run can be replayed as a script.
  **Extra real key presses during a scripted run interfere with it**; leave the keyboard alone.
- Touch-file switches in `$PES17_FILES` work on the desktop too: `trace_all` (every import, huge),
  `dump_frame` (one frame's draws, texel samples, every 8888 resolve >= 320 wide as `frame_images/at_<dest>_<draw>.pnm`,
  and the presented image `present.pnm`), `dump_guest` (`echo "40000000 80000000" > files/dump_guest` writes
  `guest_dump.bin`), `report_memory`, `check_heap`.
- `PES17_NO_MSAA=1` draws the title's 2x/4x surfaces at 1x (made no difference to the match bugs).
- `PES17_EXIT_ON_BAD_CALL=1` makes a null indirect call exit the process (desktop used to do that always).
- Saves go to `saves/` next to the working directory. Delete it to test a first boot.
- Kill a stuck game with `pkill -f build-mac/pes17`.

## Gotchas

- **The game folder name has a non-breaking space before `[RF]`.** Never type it; resolve it with
  `ls -d gamefiles/*/ | head -1`.
- zsh: `echo "== x"` fails (`=` expansion). Use `printf -- '--- x\n'`. No `timeout` command on macOS.
- `ppc/stubs_generated.cpp` is generated. It defines "unimplemented" stubs for every import the runtime does
  not implement. When you implement an import in `runtime/`, delete its stub line there or the link fails with a
  duplicate symbol (a fresh `./recompile.sh` regenerates the file without it).
- macOS cannot create the SDL window off the main thread. `main()` in `runtime/main.cpp` runs the guest on a
  256 MB thread and the main thread runs the presenter (`vk::RunHostedPresenter`). The SIGTRAP at exit
  (`vk::Stop` -> `SDL_DestroyWindow` off the main thread) is a shutdown artefact, not a game assert.
- Host pages are 16 KB on Apple Silicon; the GPU memory watch (shadow of guest memory) stays off there.
- The team database is `data/db/fifa_street_db.db` inside `data0.big` (LZX "chunklzx"); it loads and
  decompresses correctly (probe `sub_82753EA8` in `runtime/fifa_probe.cpp` logs every file load).

## Fixed (2026-10-26)

Save flow (first boot now: autosave notice -> settings save -> main menu, no error dialogs):
1. `XMsgInProcessCall(0xFE, 0x2000E)` enumeration (`EnumerateMessage`, xam.cpp) and `XMsgCompleteIORequest`.
2. xam ordinal 625 = `XamContentCreateInternal` (ours: `XamContentCreateTitle`), ordinal 624 =
   `XamContentDeleteInternal` (ours: `XamContentDeleteTitle`), both in `content.cpp` + `system_modules.cpp`.
   After the first save the game deletes it and creates it again; without 624: "Unable to save Settings File 1".
3. `XamEnumerate` completed the overlapped with the item count as the extended error (Xenia: 0) -> the save
   library read status 1 -> "Your selected storage device is no longer available".

Clubs / "undefined" team / LOCKED ITEM:
4. Pseudo-handles were swapped (`GetCurrentThread()` is 0xFFFFFFFE, process 0xFFFFFFFF) and `NtDuplicateObject`
   failed on the current thread. The game keys per-thread allocators (`sub_8263C310`) by that handle; every
   thread got none, the DB setup allocated from a null allocator (null call at lr 8263EF44). `sync_objects.cpp`.

Match:
5. XMA context API (`XMAInitializeContext`, `XMASetInputBuffer0/1(Valid)`, `XMAEnableContext`, ...) implemented
   (`kernel/audio.cpp` -> `apu/xma.cpp`), as Xenia. Stubbed, the match intro never faded in (black screen) and
   there was no sound. Now ~47k XMA frames decode per run, 0 errors.
6. Predicated tiling: the match frame is 2x MSAA in two 640-wide tiles. `PA_SC_WINDOW_OFFSET` (0x2080) is now
   applied to the window scissor and (with `PA_SU_SC_MODE_CNTL` bit 16) the viewport, and a resolve whose base
   lies inside a tiled surface resolved earlier this frame lands at that tile's x/y (`renderer_resolve.cpp`).
   Before, the right tile was dropped.
7. Depth-only mode (`RB_MODECONTROL` 5) no longer writes colour (`renderer_draw.cpp`).

Tools: screenshots, `shots_sheet.py`, desktop touch-file switches and frame dumps (above).

8. `vsldoi128` was mis-decoded by XenonRecomp as the PPC405 `mullhwu.` and dropped (3 sites in `sub_8292E290`).
   `third_party/XenonRecomp/thirdparty/disasm/ppc-dis.c`: the cell dialect no longer enables 403 instructions.
   `logs/recomp-*.txt` now has 0 "Unrecognized instruction".
9. `PM4_EVENT_WRITE_EXT` (screen extents) was unhandled; now writes Xenia's full-screen extent. The game brackets
   each cullable object with one; with nothing written the characters were never submitted.
10. **Reversed depth.** The match pass sets viewport z scale -1, offset 1 and tests GEQUAL. `ViewportOf`
   (`renderer_draw.cpp`) folded that into minDepth 0 / maxDepth 1, so the stadium behind the players won the depth
   test and every player body vanished (shadows stayed). Now minDepth = offset, maxDepth = offset + scale (Vulkan
   allows min > max). `PES17_NO_DEPTH_TEST=1` is the quick check for this kind of bug. The frame dump prints z scale/offset.
11. **`rlwimi.` did not set CR0** (XenonRecomp `recompiler.cpp`). The camera file parser `sub_833086D8` byteswaps
   the entry count with `rlwimi.` + `ble`; with a stale CR0 it skipped all 55 entries of `fifacamera.dat`, the camera
   toolkit (`A0D68BF0`, base list at +28, stadium list at +48) stayed empty, the name lookup `sub_83307470`
   ("Tele Pos_Large") returned null and `sub_83307500` handed back the default record: a 45-degree orbit 43.6 cm
   from the origin (the "100x too close" camera). A scan of all generated code found no other record-form
   instruction missing its CR update. (`fifacamera_<stadium>.dat` / `FEMenuCamValues_1300.dat` "file not found"
   is normal: only some stadiums have one, the base file covers the rest.)

12. **Resolve scale within a frame** (`renderer_resolve.cpp`). Android renders the scene at
   `PES17_SCENE_SCALE=0.75`; Match Setup resolves the whole frame at 1x, then its scene-scaled predicated tiles into
   the same copy, and the right tile's backdrop samples that copy. The scale change recreated the image (contents
   lost), so the right half of Match Setup was black on the phone. A copy already written this frame now keeps its
   scale. Reproduce phone-only rendering on the Mac with `PES17_SCENE_SCALE=0.75 PES17_NO_MSAA=1`.

13. **Phone 30 -> 60 FPS: resolve images recreated every frame** (`renderer_resolve.cpp` `MakeResolveImage`). The
   match resolves depth three times a frame into one address at three shapes (1280x736, 1280x720, scene-scaled);
   each new shape destroyed the last, and destroying waits for the GPU to go idle (`RetireBeforeDestroy`) --
   three full stalls a frame. Shapes are now kept in a spare pool (swapped, never freed below 48), and each image
   has its own texture key (`Resolved::shape`, bits 36+ of the key in `renderer_draw.cpp`).
14. **Recorder wakes batched** (`recorder.cpp`): the command processor woke the recorder thread with a futex
   syscall per call (21% of its time on the phone); now every 16th call, on Host work, before a wait, and when the
   ring runs dry (`vk::renderer::RingIdle`).

15. **Cinematics at 60** (`d3d_hooks.cpp`, `title.h`). The title runs its menus, match intro and goal
   celebrations at D3D present interval 2 (`sub_8270F040`, interval in r3 bits 8-11; D3D's vblank handler
   `sub_8270EF10` flips at `last + interval`) and caps the render thread at 40 Hz there (`sub_82619AE0` calls the
   limiter `sub_82613E70` with 40). Both are lifted by default; `PES17_CINEMATIC_60=0` keeps the console's 30. Their
   animation is on the clock: the intro lasts the same (stadium ~87 s, play ~99 s with the match macro) either way.
   The recorder also naps in 50 us polls for 4 ms before a futex sleep, so the command processor rarely wakes it.
16. **Audio crackle on the phone** (`platform.cpp` `AudioThreadPriority`, `apu/xma.cpp`, `apu/audio.cpp`). All guest
   threads share the three performance cores; behind the busy render thread the title's audio thread decoded its
   XMA late and the title mixed silent frames (dropouts of exactly 256/512/768 samples). The thread that kicks XMA
   and the mixer worker now run at Android's audio priority (-16). The sink (`android/audio.cpp`) also waits for a
   21 ms cushion before playing, AAudio's buffer is 1024 frames, the mixer period is exactly 5333.333 us (5333
   grew the queue without end), and late mixer frames are made up. `PES17_AUDIO_DUMP=<file>` (desktop env, or
   `files/env_override` on the phone) writes what the title mixed, 6-channel float 48 kHz, for click hunting; the
   sink logs underruns, device xruns and clipping every 10 s.

17. **`NtQueryDirectoryFile` was a stub that always said "no more files"** (`kernel/files.cpp` now lists the directory, one
   XDIRECTORY_INFORMATION entry per call: end of file +0x28, allocation +0x30, attributes +0x38, name length +0x3C, name
   +0x40). The DLC thread scans `d:\dlc\dlc_*` and (non-packaged DLC) wants `info.dlc` + `dlc.toc` in each folder
   (parsers `sub_826A7650` / `sub_826A7250`: `DLC_INFO` v2 + name/type strings + language table; `DLC_TOC_` ver, count, 12-byte
   entries). This dump's `dlc_FootballCompEng` has only the xex.dll, so the game logs "no toc, skipping". With hand-made
   `info.dlc`/`dlc.toc` it registers the content, but nothing in World Tour or Play Now asks to load the module
   (`FootballCompEng` is FIFA 12 career/competition code that FIFA Street does not seem to use). Not wired up.
   `PES17_DUMP_LOADS=<dir>` saves the `data/dime/*.xml` loads (store catalogue: Online Pass, Season Ticket only).

17. **Spin-wait settings, on by default** (`d3d_hooks.cpp`, `title.h`). `PES17_FENCE_SLEEP=<us>` (default 20, 0 = off) hooks
   `sub_826FCC30` (one pass of D3D's busy-wait on the GPU; the render thread's fence wait `sub_82713710` loops it) and
   sleeps between polls; `PES17_LIMITER_SLEEP` (default on, 0 = off) makes the title's frame limiter `sub_82613E70` sleep until 0.5 ms
   before its target (timer: start u64 at +0, ticks per second u64 at +24) instead of spinning the timebase. On the
   X7 Pro demo match: off 46.2 FPS, limiter 44.4, fence 56.2, both 58.7 (55 of 60 s at 55+), CPU 354% -> 266% of a core.
   Profile of the cause: the render thread spent 60% of its CPU in the fence spin and the main thread 79% in the
   limiter, on the same three performance cores as the command thread. On the S20 FE (Adreno, already 58-61 FPS) both on = 57.6 FPS vs 58.2 off (noise), CPU 228% vs 358% of a core: no
   regression, so both are on by default.

**Android (2026-10-26):** rebuilt with `tools/deploy.sh` and tested on the SM-G780G. The match plays with the
broadcast camera and all players, Match Setup draws whole. The Android build has `PES17_DIAGNOSTICS=OFF`,
so `PES17_INPUT_SCRIPT` / screenshots do nothing there: drive it with `adb shell input swipe X Y X Y 150` on the
on-screen pad (a plain `input tap` is too short) and `adb exec-out screencap -p`. `files/env_override` takes
`PES17_*=value` lines.

## Fixed (2026-10-28, on the phone)

18. **Dark stadium lighting and huge, rough shadows** (`renderer_targets.cpp` `DepthKey`, `renderer_draw.cpp`). The depth
   key had no pitch, so the shadow cascades (a 560x4096 colour surface) shared one depth image with the 1280-wide
   screen-space shadow pass. That image grew to 1280x4096 and took the 0.75 scene scale from its width: cascades were
   drawn at 3/4 size beside a full-size colour target and resolved as if 1x, so shadows came out 1.33x larger (2x at the heat
   scale's 0.5), coarse, and in the Futsal Centre the whole court sat in shadow (pitch black). Now, while the scene is
   scaled, a pass whose colour target keeps scale 1 gets its own 1x depth image (`TargetKey::unscaled`). A depth resolve
   reads the image drawn into last (`FindDepthTarget`, `Target::depthBound`), and a depth clear empties both.
   Also ~2-5 ms less GPU a frame. `PES17_SHARED_DEPTH=1` restores the old sharing for A/B.
   **Still open:** at `PES17_SCENE_SCALE=1` (the Mac, or a phone set to 100%) the Futsal Centre court is still black.
   Splitting by width there too (colour < 1152 wide) turned the whole game black: something narrow relies on the shared image.
19. **Direct3D 9 maths in the shader translator** (`shader_translator.cpp`, as Xenia): the scalar unit's muls (`MULS`,
   `MUL_PREV(2)`, `MULSC`) give 0 when either side is 0 (0 * inf, 0 * NaN), `RCPC/RSQC/LOGC` clamp inf to +-FLT_MAX,
   `RCPF/RSQF` give +-0, `SNE/SETPNE/KILLNE` are unordered. The vector unit's mul/mad/dp and max/min keep plain IEEE:
   the full rule there cost 3-7 ms GPU a frame on the Adreno 650 with no visible change. `PES17_LEGACY_MATH=1` applies
   it to the vector unit too, `PES17_IEEE_MATH=1` turns it off everywhere.

20. **Goal celebrations blurred** (the depth of field, `renderer_resolve.cpp`, `renderer.cpp`). Goals run an extra DOF chain:
   prepass `48F6D97D4A0B2F29` and composite `A7F7BFFCA5CA084B` read the depth resolve 04870000 (k_24_8_FLOAT) and blur by
   CoC = clamp(|c0.x - c0.y * d| + c0.z, 0, 1). That depth is the full-frame surface the restore pass `DBFEA8AE08630EE7`
   writes as 1 - d from the match tiles' (reversed) depth resolve. Two bugs: (a) the right-hand predicated tile's depth
   resolve goes to 04870000 + 0x14000, which in the tiled layout is 20 tiles across (x 640). The depth path read it as linear rows
   (16 rows down), so half the frame had stale depth ("half blurred, half not"). It now places by 32x32 tiles, as the colour
   path does. (b) On the stock Adreno 650 driver (Vulkan 1.1, legacy path) a multisampled depth read back as all zeros even
   through a classic render-pass resolve (`DepthResolvePass`, kept; Adreno probably skips an empty pass), so the DOF
   blurred everything. MSAA is now off on the legacy path. On Turnip (Vulkan 1.3, dynamic rendering) MSAA stays on and goals
   are sharp. With MSAA off the crowd in the Futsal Centre turns magenta (not yet looked at).

21. **Courts lit at a third of the console's brightness** (`renderer_resolve.cpp` write-back). FIFA Street resolves a
   512x512 image to 05C20000 every frame and its CPU reads the bytes back to light the courts. Our write-back stops a
   destination after 12 resolves (or 3 frames in a row), so the guest copy froze on an early frame and the Futsal Centre
   floor came out dark brown instead of light wood. Found from the PC port's notes: it needs "Readback Resolve: Full", and
   its "Some" mode was reworked for "floor data". That resolve is now written back on every resolve (`courtLighting`),
   costing ~5 fps (58 -> 53 in play). Tools: `touch files/no_writeback_cap` (everything, ~8-13 fps),
   `files/writeback_always` (hex addresses), and the log line `write-back stops for <dest>` lists capped destinations.
   A rule of "write back what the GPU never samples" didn't work: that 512x512 copy is sampled on the GPU too.
   Still open: in an outdoor 5-a-side frame the final grade (two 3D LUTs in composite `BFA3C136B52E21A1`) took the
   red channel from 94 to 9, which may be another colour bug.

**Android workflow (2026-10-28).** The user wants these bugs fixed and tested on the phone, not the Mac. The current
Android build has diagnostics on (`PES17_DIAGNOSTICS:BOOL=ON` in `build-android/CMakeCache.txt`), so `files/env_override`,
`touch files/dump_frame` and `PES17_GPU_TIMING` work there. `tools/deploy.sh` takes ~40 s incremental.
- `tools/play_macro.sh futsal` reaches a Futsal match about 60 s after it starts (`futsal_paused` freezes a paused
  fly-camera at ~110 s; `futsal_replay` opens the instant replay tool). The macro randomises teams and about half the runs stop at a
  LOCKED ITEM / "connecting to the EA Servers" dialog. Check for a yellow dialog button in the middle of the screen and restart.
- Compare GPU time, not FPS: the S20 FE heat-throttles (heat status 4 drops the scene to 0.5 with the player's
  heat scale on). Use `PES17_GPU_TIMING=1 PES17_HEAT_SCALE=0` and read `gpu: N frames, ... ms of pass time a frame`.
- The Android frame dump saves only listed resolves (`renderer_resolve.cpp`). Saving every one with a mid-frame
  `Submit(true)` faults the Adreno recorder in `vkCmdClearColorImage`. Pull `game.log` ~45 s after the dump starts, or it is cut short.
- Read a shader: build `shader_translator.cpp` + `spirv.cpp` into a small host tool, feed it the `DUMP ucode ps` dwords,
  and run `spirv-cross` on the output (both from Homebrew).
- Goals: the AI scores while the pad sits idle. The GOAL! banner can be detected at x 860-970, y 90-118 (white text)
  plus x 985-1015, y 80-110 (dark) on a 1600x720 screencap.

## Open

- The match now plays with the broadcast camera and all players (checked to 200 s, a shot every 8 s).
  Keep watching for glitches in longer matches, replays, goals and the other modes.
- Small seam at x = 640 between the two predicated tiles.
- POCO X7 Pro (Dimensity 8400, Mali-G720, Android 16; serial `JZFUB6VCIJDEN7GI`): runs, renders correctly, demo
  match 53-58 FPS, audio clean. Left idle at the title the game plays an AI demo match: a quick test scene with no
  input. Its USB debugging dropped every second or two until `adb tcpip 5555` then `adb usb` (adbd restarted).
  The audio queue started at 150 ms there (the mixer filled the ring before the device pulled); the callback now
  trims anything past 43 ms back to 21 ms (`kMaxQueuedSamples`, android/audio.cpp).
- Speed on the phone (SM-G780G): active play 58-61 FPS, Match Setup ~50, match intro 46-51 (heavy close-ups,
  ~1000 draws). There the command processor's per-draw cost is the limit (~19 us a draw: WriteConstants,
  TextureSetFor, WriteIndices, memcpy); the render thread waits on its fences (`sub_82713710`) for it. Next: cut
  that per-draw cost, or -O3 for the generated code. Timeline tool idea: simpleperf `--trace-offcpu` samples per
  thread, bucketed per millisecond (what each thread runs or waits on).
- Profiling the phone: the app is `profileable`, so
  `adb shell "cd /data/local/tmp && simpleperf record --app com.fifastreet.recomp -e cpu-clock --trace-offcpu -g --duration 8 -o /data/local/tmp/x.data"`
  then `simpleperf report -i ... --comms '<thread>' --children --sort symbol` (wall time, waits included).
  `/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage` gives Adreno load.
- README "Next": DLC module, installers copying `data/`/`dlc/` are untouched.

## Debugging notes

- Look at the generated code: `ppc/dll/ppc_recomp.*.cpp`, e.g.
  `awk '/^PPC_FUNC_IMPL\(__imp__sub_83573D70\)/{f=1} f{print} /^}/{if(f)exit}' ppc/dll/ppc_recomp.NNN.cpp`.
  `grep -l "__imp__sub_XXXXXXXX)" ppc/dll/*.cpp` finds the file.
- `runtime/fifa_probe.cpp` hooks guest functions (`GUEST_HOOK(addr)`, `GUEST_ORIG(addr)`) to log arguments and
  results. Keep probes capped (they print the first N calls).
- The save library's result codes: `sub_83383A68` maps Win32 errors to status (0 ok, 4 not found/no more files,
  2 failure/device gone, 7 denied, 9 pending); `sub_83383DD8` polls the overlapped.
- A hang or spin: `runsample.sh` gives native stacks with `sub_XXXXXXXX` frames; the sync watchdog in the log only
  shows threads waiting on kernel objects.
