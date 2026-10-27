---
name: release
description: Rebuild both Pandemonium ROMs from main, check them, and publish the GitHub release on cbyeh/Pandemonium. Use when the user says ship, publish or release.
---

# Release

1. Make sure `main` is pushed to `cbyeh` and the worktree is clean. Squash related commits first if the user asked.
2. Clear stale assets: deleted or renamed sounds/sprites stay in `filesystem/` (and `build*/filesystem/`) and still get packed. Remove any output with no source in `assets/`, then `rm` the `.dfs` files.
3. Build both ROMs from HEAD (in zsh, `rm -f *.z64 && ...` aborts when no ROM exists):
   ```sh
   E="N64_INST=$HOME/n64/toolchain T3D_INST=$HOME/n64/tiny3d"
   rm -f pandemonium.z64 pandemonium-hle.z64
   make pandemonium.z64 $E > /tmp/rel-hw.log 2>&1
   make pandemonium-hle.z64 HLE=1 $E > /tmp/rel-hle.log 2>&1
   grep "error:" /tmp/rel-hw.log /tmp/rel-hle.log   # must print nothing
   ```
4. Smoke-test both ROMs boot (`/test-rom`).
5. Until the user calls 1.0 stable, replace v1.0 (after that, create a new numbered release instead and leave old ones up):
   ```sh
   mkdir -p /tmp/rel && cp pandemonium.z64 "/tmp/rel/Pandemonium.z64" && cp pandemonium-hle.z64 "/tmp/rel/The Pandemonium.z64"
   gh release delete v1.0 --repo cbyeh/Pandemonium --cleanup-tag --yes
   git tag -d v1.0 2>/dev/null
   gh release create v1.0 --repo cbyeh/Pandemonium --target main --title "Pandemonium 1.0" \
     --notes-file docs/release-notes.md "/tmp/rel/Pandemonium.z64" "/tmp/rel/The Pandemonium.z64"
   ```
   Asset names carry no version so saves import/export cleanly (GitHub shows the space as a dot).
6. Notes ([docs/release-notes.md](../../../docs/release-notes.md)) stay short selling copy: the which-ROM line, then four bullets (bosses, Tower, combat, N64). A notable feature gets a line in a bullet, not a changelog. No spoilers: no numbers, unlock conditions or mechanic math.

**Mono release** (`v1.0-mono`, a full release beside v1.0, never Latest): the same two ROMs built with `MUSIC_MONO=1` (the music in mono, ~10.8 MiB), same asset names so saves carry over; replace it whenever v1.0 is replaced: `gh release create v1.0-mono --latest=false ...`. Clear `filesystem/` between the stereo and mono hardware builds.

Feature branches get pre-releases instead (`--prerelease --target <branch>`); main's v1.0 stays put until the user approves a merge. Never release on upstream.
