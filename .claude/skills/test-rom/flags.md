# Test aids reference

All scripts live in `hle/autoplay.c` and need `-DHLE_AUTOPLAY` (HLE: `HLE_EXTRA_CFLAGS`; hardware/Ares: `TEST_CFLAGS`, the only way to drive Ares). Check a flag still exists with `git grep -n FLAG src hle` before relying on it.

## Put the game in a state
- **Straight to a boss:** `HLE_TEST_DUMMY HLE_DUMMY_BOSS=1..5` (Guardian, Unbound, Fallen Warrior, Sun, Shadowborne). Taps through menus, skips cutscenes; player is invincible and logs `[dummy]` damage over 120 s plus openings.
- **Second half / phase 2:** `HLE_DUMMY_HALF` (boss starts at 45% HP). `HLE_DRAGON_DARK` starts Shadowborne dark.
- **Full fights that never die:** `HLE_TEST_BONUS` (+ `HLE_TEST_BONUS_SUN` / `_DRAGON`), `HLE_TEST_UNBOUND`, `HLE_TEST_STRIKE`, `HLE_TEST_CRUCIFIX`: refill the player below 60 HP, log `[dmg]`.
- **One attack on a loop:** `HLE_FK_FORCE=1..9` (6 Triangle Slam, 7 Holy Slam, 8 Plunge, 9 Moon Blade), `HLE_SUN_FORCE=1..7` (`11` = lash into sweep), `HLE_DRAGON_FORCE=<DgState>`, `HLE_SUN_HITME` (a sword hit every 1.2 s, every third a finisher: the Sun's reactions), Unbound's `HLE_THROW_FORCE` and `HLE_SWEEP_FORCE` (the ground sweep at any range: the dummy stays close). `HLE_DRAGON_CYCLE`: every Shadowborne move in turn (a gallery, `[cycle]` lines).
- **Long stagger:** `HLE_STAGGER_LONG` gives Crushing Blows outside the Tower; `[stagger]` lines log when each stagger starts, ends and the boss is up.
- **Into phase 2:** `HLE_TEST_TWO_RUNS` (kill the Guardian, reach phase 2, twice); `HLE_2R_NOSKIP` lets the cutscenes play (about 3000 frames), `HLE_2R_LOCK` locks on before the kill, `HLE_2R_KILL_T=<frames>` fights that long first (topped up, moving, swinging: a real fight records far more blocks).
- **Display lists:** `HLE_TEST_DL_CHECK` walks every task as the plugin will (into each `G_DL`) and logs any command this backend never emits (`[dlcheck] BAD`: Delta's plugin aborts on one), cache resets and replays while one is pending.
- **Boss held still** (before/after shots that line up): `HLE_TEST_FREEZE_BOSS`. Boss AI is otherwise non-deterministic.
- **Ending:** `HLE_TEST_ENDING`.
- **Lock-on at the kill:** `HLE_DUMMY_LOCK_KILL` (with `HLE_TEST_DUMMY`): locked on and circling, the boss dies at 4 s; `[lockkill]` logs the camera.
- **Bot playtest:** `HLE_DUMMY_REAL` + `HLE_DUMMY_STRIKE` / `_ROLL`, `HLE_STRIKE_OPEN`, `HLE_STRIKE_STAMINA`; potions below 40%, prints `[bot] ... beaten/died`.
- **Player:** `HLE_DUMMY_ORBIT` / `KEEP` / `PIN` / `CENTER` / `MOVE` / `SIDESTEP`, `HLE_DUMMY_SKIN=<n>`, `HLE_PAD_LAYOUT`, `HLE_SKILLS_OFF`.
- **Skills:** off on a fresh save (pressing buttons casts nothing). `HLE_TEST_SKILL_DMG=<1-based>` zeroes cooldowns and casts on a loop.
- **Tower:** `HLE_TEST_TOWER_RUN`, `HLE_RUN_CLIMB`, `HLE_RUN_FLOORS`, `HLE_RUN_PICK`, `HLE_TOWER_GIVE`, `HLE_TOWER_RELIC`, `HLE_PACT_CURSE`, `HLE_DRAFT_GALLERY`, `HLE_DRAFT_HOLD`.
- **Saves:** `HLE_TEST_SAVE_FLOW`, `SAVE_FRESH`, `SKIN_SAVE`, `SKIN_SLOT` dump EEPROM block 0; `HLE_LOG_RELIC` after a reboot. Isolate a run's saves with `M64P_EXTRA="--set Core[SaveSRAMPath]=/tmp/x"`; back up `Pandemonium-72AF2497.eep` before editing one.
- No timescale, fast-forward or RNG seed aid exists (boss order seeds from the clock). Speed up by skipping to the state above, not by running faster.

## Capture
- **Cameras:** `HLE_DUMMY_SIDECAM` (`HLE_SIDECAM_K`), `HLE_DUMMY_CAM` (`HLE_DUMMY_CAM_YAW` turns it round the boss, 1.57 = side; `HLE_DUMMY_CAM_DIST`, 120), `CAM_NEAR`, `CAM_HIGH`, `CAM_PORTRAIT` (`HLE_PORTRAIT_D/H/LOOK/TURN`, `HLE_PORTRAIT_FLIPX` for the Shackled Sun, whose front is the other way; with `HLE_SUN_FORCE=9` it only hovers, its face at rest). `HLE_SKIN_GATE` poses skins at the fog gate.
- **Clean README shots:** `HLE_SHOT_CLEAN` / `HLE_NO_HUD`.
- **Frame-exact video:** shot every frame, `run.sh rom "$(seq -s, 900 1 1100)"` (comma list, not a range), then
  `ffmpeg -framerate 30 -pattern_type glob -i '*.png' -vf scale=640:-2,format=yuv420p -c:v libx264 -crf 18 out.mp4`. Lower `-framerate` or `setpts=4*PTS` for slow motion.
- **Sheets / side-by-sides:** `-vf "scale=320:-1,tile=6x4" -frames:v 1`; `hstack`/`xstack` with `crop=...,scale=...:flags=neighbor`.
- **Audio:** the emulator runs `--audio dummy`, so sound is never captured from it. Build A/B clips from the asset files with ffmpeg `concat` (old, 1 s `anullsrc`, new); loudness via `astats` / `volumedetect` / `ebur128`. `tools/boss_sfx_map.py` makes a page of every Bonus move's sounds (LUFS, gain); `tools/boss_sfx_levels.py` needs numpy (Blender's Python has it). Music loops: `tools/music_loop.py` (beat-grid loop points written as a WAV `smpl` loop; needs `--bpm`, see its header), `HLE_LOG_MUSICPOS` logs `[musicpos]` every 2 s (the jump back shows as the position dropping).
- **LLE reference shots:** `~/n64/emu/ref/make_lle_refs.sh` (angrylion + cxd4 via `run_lle.sh`).

## Measure
- **FPS / CPU:** `HLE_LOG_FPS` (`[fps]` every 2 s, per-part ms, tri/vert counts; meant for Ares). `HLE_TEST_PERF` (rsp-hle CPU %, audio queue, worst frame). GPU: `HLE_PROF_SERIAL`, `_DRAGON` (also the Shackled Sun's parts: `[sunprof]`), `_BOSS`, `_WAITEND`.
- **Pacing:** `HLE_LOG_HITCH`, `HLE_LOG_JITTER`, `HLE_LOG_LATENCY`. `HLE_TEST_TASKS` checks one graphics task per frame (catches black-frame flicker shots can't).
- **Memory:** `HLE_TEST_BONUS` logs the lowest free heap.
- **Move starts on Mupen's frame numbers:** `HLE_LOG_MOVE_MS` (Shadowborne: `[movems] <state> <VI frame>`, counted from boot; shot `N` is VI frame `N`): line up before/after videos of builds that drift apart.
- **Choices:** `HLE_LOG_CHOOSE` (Shadowborne: the distance, angle and candidate moves at each pick, then the pick).
- **Boss hit:** `HLE_DRAGON_HITME` (Shadowborne takes a sword hit every 1.5 s, a finisher's every 6 s: the flinch; `[flinch]` with `HLE_LOG_MOVES`). The passive dummy and the bots never damage the boss.
- **Rig:** `HLE_LOG_RIG` (Shadowborne, per move: worst leg reach, neck and torso stretch as drawn, the head's nearest pass of its own body).
- **Hits:** `HLE_LOG_MOVES` (`[move]` then `[hit]`), `HLE_LOG_HURTBOX`, `HLE_LOG_HITVOL`, `HLE_FK_HITLOG`, `HLE_TEST_ARC_REACH`, `HLE_LOG_JUMPS`. Sound timing: `HLE_LOG_SFX` with `HLE_LOG_MOVES` (`[sfx] ms index d <distance heard from>`).
- **Same pixels after a rewrite:** `HLE_TEST_T3D_EXACT`.
- **HLE block replay** (hle/replay.c): `HLE_TEST_REPLAY_CHECK` runs every replayable block normally and compares it with its recording (display list, scratch, matrix sources, state left): `[hle] replay: ... check N ok M BAD` with `HLE_TEST_PERF`, `[replay] MISMATCH` on any difference. `HLE_NO_REPLAY` interprets every block (the reference for pixel comparisons). `HLE_TEST_VCACHE_VERIFY` checksums the source of every vertex/strip cache hit (`[vcache] CHANGED` if data cached as static was rewritten). With `HLE_TEST_PERF`: replay counts, replay time by part, time inside recorded blocks.
- **Memory used before it's set:** `HLE_TEST_DIRTY_HEAP` fills freed heap with garbage at frames 400 and 900; compare shots with a clean run (it found the Fallen Warrior's unset matrix).
- **Emulated CPU cost:** `HLE_TEST_MEMBENCH` (Mupen charges every instruction the same, cached or not). `HLE_LOG_SIDECAM` logs boss/player positions with `HLE_DUMMY_SIDECAM`.
- **Compare like with like:** live fights diverge between builds (AI, timing): compare pixels on frozen scenes (`HLE_TEST_FREEZE_BOSS`, best with `HLE_DUMMY_SIDECAM`) and fps over long live runs.
- **Logs only:** one huge shot frame (`run.sh rom 40000`) plus `TIMEOUT`; batch in the background, then `grep 'IS64: \[dummy\]'`.

## Gotchas
- Mupen numbers new shots after old ones: empty the out dir first.
- The Read tool may show a cached image for a reused path: copy to a new name.
- Leftover Ares instances write to the same log ("Booted" twice): kill them first.
- Passive-dummy fps flatters a fight (59.8 vs 57.3 in a real Shadowborne fight): confirm with a full fight.
- Mupen won't boot the hardware ROM; its LLE run crashes at boot.
