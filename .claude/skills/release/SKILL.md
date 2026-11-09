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
4. QA: `tools/release_qa.sh <Pandemonium.z64> "<The Pandemonium.z64>"` (from the repo root; ~7 min). It builds test ROMs from HEAD with `HLE_TEXT_CHECK` and runs every menu, the Tower's UI walk and a 4-floor climb (no assert, no bad string, 0 FAIL, floor 4 reached), then boots both release ROMs: look at its two shots (the title menu). Don't publish on FAILED. (2026-11-06: a Tower menu row drew stack garbage, passed every menu test, and froze the user's game on Delta: the text check catches that.)
5. **Ask the user where it goes before publishing**, even when they said "release": update an existing release (which one), a pre-release, or a new numbered release. Never delete a public release: its date and download counts can't be brought back (2026-11-08: v1.0 was deleted and recreated without asking, and both were lost).
   Stage the ROMs in a per-session dir (other sessions use /tmp/rel too). Asset names carry no version so saves import/export cleanly (GitHub shows the space as a dot):
   ```sh
   D=/tmp/rel_$$; mkdir -p $D && cp pandemonium.z64 "$D/Pandemonium.z64" && cp pandemonium-hle.z64 "$D/The Pandemonium.z64"
   ```
   - **Update an existing release** (keeps its title, notes and date; each file's download count resets, so tell the user first):
     ```sh
     gh release upload <tag> --repo cbyeh/Pandemonium --clobber "$D/Pandemonium.z64" "$D/The Pandemonium.z64"
     git tag -f <tag> <full sha> && git push -f cbyeh <tag>     # the release follows its tag to the new build
     ```
   - **New release** (`--target` needs the full SHA from `git rev-parse`):
     ```sh
     gh release create <tag> --repo cbyeh/Pandemonium --target <full sha> --title "..." \
       --notes-file docs/release-notes.md "$D/Pandemonium.z64" "$D/The Pandemonium.z64"     # add --prerelease or --latest=false as agreed
     ```
   Then download the release and compare md5s with the staged files. `gh` is at /opt/homebrew/bin/gh if it isn't on PATH.
6. Notes ([docs/release-notes.md](../../../docs/release-notes.md)) stay short selling copy: the which-ROM line, then four bullets (bosses, Tower, combat, N64). A notable feature gets a line in a bullet, not a changelog. No spoilers: no numbers, unlock conditions or mechanic math.

**Mono release** (`v1.0-mono`, a full release beside v1.0, never Latest): the same two ROMs built with `MUSIC_MONO=1` (the music in mono, ~12.7 MiB), same asset names so saves carry over; when v1.0 is updated, ask whether to update it too, the same way as above. Before each mono build clear its assets, or the ROM comes out stereo-sized: `rm -rf filesystem build` for hardware and `rm -rf build-hle` for HLE (its own `build-hle/filesystem`), then delete the `.dfs` files with `find . -maxdepth 2 -name "*.dfs" -delete` (in zsh a bare `*.dfs` with no match aborts the whole command).

Feature branches get pre-releases (`--prerelease --target <full sha>`); main's releases stay put until the user approves a merge. Never release on upstream.
