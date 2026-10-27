---
name: test-rom
description: Build a scripted Pandemonium test ROM and run it in Mupen (HLE) or Ares (hardware) with logs and screenshots. Use to reproduce a bug, verify a fix, measure perf/memory/balance, or capture images for the user.
---

# Test a ROM

Emulator scripts live outside the repo in `~/n64/emu`. Every flag for jumping to a boss or phase, forcing attacks, cameras, clean shots, video/audio capture and measurement is in [flags.md](flags.md): read it before writing a new test aid.

## HLE build in Mupen (Delta-like)
```sh
make pandemonium-hle.z64 HLE=1 BUILD_DIR=/tmp/tb_NAME N64_INST=$HOME/n64/toolchain T3D_INST=$HOME/n64/tiny3d \
  HLE_EXTRA_CFLAGS="-DHLE_AUTOPLAY -DHLE_TEST_..." && mv pandemonium-hle.z64 /tmp/tb_NAME.z64
TIMEOUT=500 ~/n64/emu/run.sh /tmp/tb_NAME.z64 300,900,1800,99999 /tmp/out_NAME
```
- `debugf` output shows as `Core: IS64:` lines in `out/log.txt`.
- The emulator quits after the last shot frame: end the list with a big one, soon after the last log you need. VI frames drift against game time: log `get_ticks_ms()` and take dense shots.
- Delta's own core: `M64P_EXTRA="--corelib ~/n64/emu/core-delta/projects/unix/libmupen64plus.dylib"` (no debug log; check with shots).
- Saves persist in `~/Library/Application Support/Mupen64Plus/save/`.

## Hardware build in Ares (Mupen can't run it)
```sh
make pandemonium.z64 BUILD_DIR=/tmp/th_NAME N64_INST=$HOME/n64/toolchain T3D_INST=$HOME/n64/tiny3d \
  TEST_CFLAGS="-DHLE_AUTOPLAY -DHLE_TEST_..." && mkdir -p /tmp/th_NAME_rom && mv pandemonium.z64 /tmp/th_NAME_rom/
~/n64/emu/run_ares.sh /tmp/th_NAME_rom/pandemonium.z64 <secs> <shot-every-s> /tmp/out_NAME   # run in background
```
- Measure memory at 4 MB: add `--setting Nintendo64/ExpansionPak=false` (Ares defaults to the pak).
- Ares keeps `<rom>.eeprom` next to the ROM: one dir per ROM.
- Perf varies with heap layout: try a few layouts (malloc pads) before trusting one run. Ares doesn't model RDP fill time, so hardware may be slower where the RDP is busy.

## Rules
- **Emulators run behind the user's windows:** launch only through `run.sh` / `run_lle.sh` (they hand focus back to the app that was in front) and `run_ares.sh` (`open -g`). Never start `mupen64plus` or Ares directly.
- **Never `screencapture` the whole screen** (it grabs the user's other work); window id only, as run_ares.sh does. Another session's Ares may be running: `ares_winid`/`pgrep -n` can grab the wrong one.
- Fresh `BUILD_DIR` per flag set; one build at a time per checkout.
- Baseline: `git worktree add /tmp/base_wt <rev>` and build the same flags there.
- Report numbers from the log, and show the screenshots to the user.
