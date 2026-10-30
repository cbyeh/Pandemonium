# Pandemonium (cbyeh fork)

N64 souls-like boss rush built with libdragon + tiny3d. Design numbers (balance, Tower rules, budgets) live in [docs/design/ROADMAP.md](docs/design/ROADMAP.md).

## Git and releases
- Work on `main` of the fork (remote `cbyeh`) and push as work lands. Never push, tag or open PRs on upstream (`origin` = Boxingbruin). Use `--repo cbyeh/Pandemonium` with `gh`.
- Release only when the user asks (`/release`); pushing doesn't ship one. Until the user calls 1.0 stable, a release replaces v1.0; after that, ship numbered releases.
- Squash fix-up commits into the commit they fix.
- Other Claude sessions may share this checkout: do work in a `git worktree`.

## Hard rules
- **Guardian and Guardian Unbound stay pure:** never change their attack patterns or timing. Tune only the Bonus bosses (`src/game/fallen_knight`, `src/game/sun_boss`, `src/game/dragon_boss`). Shared fixes (hazards, stagger, player feel) are fine; say so explicitly. Looks changes only on request. The one approved addition: Unbound's Sword Throw (a passive summon).
- **Bonus bosses telegraph with their bodies,** like the Guardian: no extra glows, rings, sparks or floor warnings.
- **Never lock or cap the frame rate** (no `display_set_fps_limit`, no cadence lock), on any build. Smooth uneven frames another way (dt-independent easing, per-refresh interpolation).
- **4 MB, no Expansion Pak.** Keep 400-500 KB heap free at the lowest point. Measure in Ares with `--setting Nintendo64/ExpansionPak=false`.
- The user plays on **Delta (iPhone, no JIT)**: judge HLE performance and saves there.
- Never use `RDPQ_BLENDER_ADDITIVE`: real RDP wraps past 255 (HLE clamps).
- Never call a block with tiny3d triangles from a block that has its own RDP commands (the triangles overwrite them).
- Saves: use save_controller's `eep_read` (8-byte read); never `eeprom_read`/`eepfs_read` (Mupen/Delta return blank data).
- Every boss move must land where it's drawn: no move "for show". Skins never change hitboxes or framing.
- Animation transitions are smooth, especially into and out of stagger.
- Menus work the same with D-pad, stick and A, in every button layout.
- Bonus boss sounds sit at the Guardian's loudness.
- A move's impact sound is heard from where it lands, not from the boss: blasts, bursts, rain and eruptions use the boss's `sfx_at(id, vol, x, z)`; a beam or stream that moves is heard from its nearest point each frame (`audio_set_scene_sfx_dist` on the channel the sound started on, as Shadowborne's `beam_at`). Wind-ups, casts and launches stay at the boss. Timed from the boss, a long-range hit by the player played faint or silent (the falloff is silent past 200).
- Looks upgrades should pay for themselves (no perf cost if avoidable).

## Working with the user
- Ask questions before long or overnight work. If you have a better idea than the one asked for, propose it instead of building theirs.
- Balance changes need a numbers case (the metric below, Elden Ring / Nightreign as reference); don't make one that doesn't add up.
- Before publishing: screenshots, videos or before/after side-by-sides for anything visible, the sound file for any audio change, and a test playthrough.

## README and release notes
- Short selling copy that keeps some mystery: no exact numbers, unlock conditions, formulas or move lists. "Get deep into the Tower", not "reach floor 10"; name a mechanic, don't explain its math.
- Keep it current with the game; screenshots show no HUD or player, bosses centered.
- CC0 credits stay at the bottom under Team members.

## Building
- `N64_INST=$HOME/n64/toolchain T3D_INST=$HOME/n64/tiny3d` (use `$HOME`, not `~`, in make args).
- Hardware: `make pandemonium.z64`. HLE (Delta/Mupen): `make pandemonium-hle.z64 HLE=1`. Name the target: plain `make` exits 0 even when the build fails.
- Check failures with `grep "error:"` (the compiler colours the word).
- Test flags (`TEST_CFLAGS` for hardware, `HLE_EXTRA_CFLAGS` for HLE) need a fresh `BUILD_DIR` per flag set: make won't rebuild when only `-D` flags change.
- Don't build in parallel in one checkout: builds write the ROM to the repo root.

## Done means tested
Reproduce the bug on a baseline build, then show logged numbers (not "looks fine") proving the fix, in the emulator: see `/test-rom`. Rerun the `HLE_TEST_*` scripts covering shared code you touched.

## Balance
Fight cost = boss damage/min on the passive dummy (`HLE_TEST_DUMMY`, 120 s) x 30% x time to kill, over ~352 player resources. Re-measure after any boss change. Targets in ROADMAP.md.
