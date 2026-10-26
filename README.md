# FIFA Street (Xbox 360) on Android and macOS

A port of **FIFA Street** (EA Sports, 2012, Xbox 360) that runs natively on **Android phones** and
**Apple Silicon Macs**. It is not an emulator: the game's PowerPC code was translated ahead of time to C++
with [XenonRecomp](third_party/XenonRecomp) and compiled for arm64, and a runtime stands in for the Xbox 360's
kernel, GPU (drawn with Vulkan, through MoltenVK on the Mac) and audio hardware.

**No game data is included.** You need your own copy of the game, extracted from a disc you own.

## What works

| | |
|---|---|
| Boot, title screen, main menu | works |
| Saving settings and progress | works |
| Team select, Match Setup | works |
| A full 5-a-side match: players, ball, camera, HUD, crowd, sound | works |
| Match intro and goal celebrations | works |
| Other modes (World Tour, Practice Arena), online, DLC | not tested |

Tested on a Samsung Galaxy S20 FE, a POCO X7 Pro and an Apple Silicon Mac.

## Performance

Measured in the match the game plays by itself at the title screen, 60 seconds per run, with the phone's
default settings (the 3D scene is drawn at 75% resolution, without multisampling):

| device | GPU | average FPS | seconds at 55+ FPS (of 60) |
|---|---|---|---|
| Galaxy S20 FE (Snapdragon 865) | Adreno 650 | 57.6 | 48 |
| POCO X7 Pro (Dimensity 8400) | Mali-G720 | 58.7 | 55 |
| Apple Silicon Mac | Apple GPU (MoltenVK) | 60 | all |

- The game aims for 60 FPS. Close-up scenes (the match intro, goals) are the heaviest and can dip into the 40s–50s on
  the phones.
- Menus and cutscenes run at 60 here, where the console capped them at 30. Set `PES17_CINEMATIC_60=0` to get the
  console's 30 back.
- Two changes made the biggest difference on phones: the render and main threads no longer burn a core
  waiting (`PES17_FENCE_SLEEP`, `PES17_LIMITER_SLEEP`, on by default), and the renderer no longer stalls the
  GPU three times a frame. On the X7 Pro that took the match from 46 to 59 FPS; on the S20 FE it kept 58 FPS
  and cut CPU use from 358% to 228% of a core, so it runs cooler.

## Requirements

- **Game files:** the contents of the FIFA Street disc (title ID `4541097B`), extracted to a folder.
- **Android:** an arm64 phone, Android 8.0 or newer, with Vulkan and ARMv8.2 cores (Snapdragon 845 or newer). 8 GB
  of RAM or more is recommended. About 4.5 GB of free storage.
- **Mac:** Apple Silicon, with Homebrew `llvm`, `molten-vk`, `vulkan-loader`, `cmake` and `ninja`.
- **To build:** CMake 3.20+, Ninja, Python 3, Clang; for Android, the Android SDK with NDK 28 and `adb`.

## How to run it

All commands run from this folder (`app/`). The game folder goes next to it, in `../gamefiles/`, for example
`../gamefiles/FIFA Street/default.xex`.

**1. Generate the code** (about 3 minutes; needs the game files):

```bash
./recompile.sh
```

**2a. Run on the Mac** (the first build takes about 15 minutes):

```bash
cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
```

```bash
ninja -C build-mac pes17
```

```bash
G=$(ls -d ../gamefiles/*/ | head -1); ./build-mac/pes17 "${G%/}/default.xex" "${G%/}"
```

Controls: a gamepad, or the keyboard: Return = START, Space or Z = A, X or C = B, arrow keys = D-pad, Esc = BACK.

**2b. Run on Android** (phone connected over USB with USB debugging on). Configure once:

```bash
NDK=$(ls -d ~/Library/Android/sdk/ndk/28.* | tail -1)
cmake -S . -B build-android -G Ninja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared -DPES17_DIAGNOSTICS=OFF -DPES17_USE_SDL=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG"
```

Copy the game to the phone (the whole disc except `$SystemUpdate`, about 4.3 GB; the folders have to exist first):

```bash
G=$(ls -d ../gamefiles/*/ | head -1); D=/sdcard/Android/data/com.fifastreet.recomp/files/game
adb shell mkdir -p $D
adb push "${G%/}/." $D/
```

Build, install and start it:

```bash
tools/deploy.sh 30
```

With more than one device connected, set `ANDROID_SERIAL=<serial>` first. Controls: the on-screen pad, or any
Bluetooth or USB gamepad. The first start shows the game's autosave notice; press A.

## Settings

Settings are environment variables. On the Mac, put them before the command. On Android, write one `NAME=value`
per line to `files/env_override` in the app's folder:

```bash
adb shell "echo PES17_CINEMATIC_60=0 > /sdcard/Android/data/com.fifastreet.recomp/files/env_override"
```

| setting | effect |
|---|---|
| `PES17_CINEMATIC_60=0` | menus, the match intro and goal scenes at 30 FPS, as on the console (default: 60) |
| `PES17_SCENE_SCALE=<fraction>` | resolution of the 3D scene, 1 is full (Android default 0.75) |
| `PES17_NO_MSAA=1` | no multisampling (Android default) |
| `PES17_FPS_LIMIT=<n>` | cap the frame rate |
| `PES17_FENCE_SLEEP=<us>` | default 20; `0` restores the game's busy-wait on the GPU |
| `PES17_LIMITER_SLEEP=0` | restores the game's busy-wait in its frame limiter (default: sleeps) |

## Known limitations

- Only the 5-a-side street match from "Hit the Streets" has been played end to end.
- Close-up scenes dip below 60 on phones. On the phones tested the limit is the CPU, not the GPU.
- The side-by-side seam at the middle of the screen in some multi-tile frames is not fixed.
- The game's downloadable content module is not recompiled yet, and the Android installer copies only the top-level
  game files, so the `data/` and `dlc/` folders have to be pushed by hand as shown above.

## Documentation for developers

- [Technical notes](docs/technical-notes.md): how the two code modules and the recompiler changes work.
- [Developer notes](../CLAUDE.md): every fix so far, open problems, and the debugging tools (scripted runs and
  screenshots on the Mac, profiling on the phone).

## Legal

FIFA Street is © Electronic Arts. This project contains no game code or data and does not help with obtaining
either; you need a copy you own. The project is licensed under GPL-3.0 (see [LICENSE](LICENSE)). It builds on
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the recompiler,
[Xenia](https://github.com/xenia-project/xenia) as the reference for Xbox 360 behaviour, and
[libadrenotools](https://github.com/bylaws/libadrenotools) for loading custom Adreno drivers.
