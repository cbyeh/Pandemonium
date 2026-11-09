#!/bin/bash
# Release QA (the release skill's step 4): from this checkout's HEAD, build and run in Mupen
#   - every menu (HLE_TEST_MENUS), the Tower's UI walk (HLE_TEST_TOWER_NAV) and a 4-floor climb
#     (HLE_TEST_TOWER_RUN), each with HLE_TEXT_CHECK (every drawn string checked for garbage)
#   - the given release ROMs to the title (shots; release ROMs print no log)
# and fail on any assert or exception, any [textcheck] BAD, a menu or nav FAIL, or a climb that
# doesn't reach floor 4. Usage: tools/release_qa.sh [<Pandemonium.z64> <"The Pandemonium.z64">]
# Run from the repo root, one at a time per checkout (every HLE build writes pandemonium-hle.z64 here).
set -u
OUT=/tmp/rqa_$$; mkdir -p "$OUT"
E="N64_INST=$HOME/n64/toolchain T3D_INST=$HOME/n64/tiny3d"
fail=0
say() { echo "[qa] $*"; }
run_test() {     # name, flags, frames
    local n=$1 f=$2 frames=$3
    rm -f pandemonium-hle.z64
    if ! make pandemonium-hle.z64 HLE=1 BUILD_DIR="$OUT/b_$n" $E HLE_EXTRA_CFLAGS="-DHLE_AUTOPLAY -DHLE_TEXT_CHECK $f" > "$OUT/$n.build.log" 2>&1; then
        say "FAIL $n: build (see $OUT/$n.build.log)"; fail=1; return; fi
    mv pandemonium-hle.z64 "$OUT/$n.z64"
    TIMEOUT=900 ~/n64/emu/run.sh "$OUT/$n.z64" "$frames,$((frames + 10))" "$OUT/o_$n" > /dev/null 2>&1
    LOG="$OUT/o_$n/log.txt"
    local asserts bad
    asserts=$(LC_ALL=C grep -cE "ASSERTION FAILED|Exception|exception at" "$LOG")
    bad=$(LC_ALL=C grep -c "\[textcheck\] BAD" "$LOG")
    [ "$asserts" -gt 0 ] && { say "FAIL $n: $asserts assert/exception lines"; LC_ALL=C grep -m3 -A2 "ASSERTION\|xception" "$LOG"; fail=1; }
    [ "$bad" -gt 0 ] && { say "FAIL $n: $bad bad strings"; LC_ALL=C grep -m5 "\[textcheck\] BAD" "$LOG" | sed 's/.*IS64: //'; fail=1; }
}
run_test menus "-DHLE_TEST_MENUS" 14000
d=$(LC_ALL=C grep "\[menus\] done" "$OUT/o_menus/log.txt" | tail -1 | sed 's/.*IS64: //')
case "$d" in *" 0 FAIL"*) say "menus: $d";; *) say "FAIL menus: ${d:-never finished}"; fail=1;; esac
run_test nav "-DHLE_TEST_TOWER_NAV" 30000
d=$(LC_ALL=C grep "\[nav\] done" "$OUT/o_nav/log.txt" | tail -1 | sed 's/.*IS64: //')
case "$d" in *" 0 FAIL"*) say "nav: $d";; *) say "FAIL nav: ${d:-never finished}"; fail=1;; esac
run_test climb "-DHLE_TEST_TOWER_RUN -DHLE_RUN_CLIMB -DHLE_RUN_STRIKE -DHLE_RUN_TOPUP -DHLE_RUN_FLOORS=4" 30000
top=$(LC_ALL=C grep -oE "\[run\] floor [0-9]+" "$OUT/o_climb/log.txt" | awk '{print $3}' | sort -n | tail -1)
if [ "${top:-0}" -ge 4 ]; then say "climb: reached floor $top"; else say "FAIL climb: reached floor ${top:-0} of 4"; fail=1; fi
if [ $# -ge 2 ]; then     # the release ROMs themselves: to the title
    cp "$2" "$OUT/rel_hle.z64"; TIMEOUT=200 ~/n64/emu/run.sh "$OUT/rel_hle.z64" "900,910" "$OUT/o_rel_hle" > /dev/null 2>&1
    mkdir -p "$OUT/hw"; cp "$1" "$OUT/hw/Pandemonium.z64"; ~/n64/emu/run_ares.sh "$OUT/hw/Pandemonium.z64" 25 12 "$OUT/o_rel_hw" > /dev/null 2>&1
    say "release ROM shots (look at them: the title menu): $OUT/o_rel_hle/the_pandemonium-000.png $OUT/o_rel_hw/shot_002.png"
fi
rm -f pandemonium-hle.z64
[ $fail = 0 ] && say "PASS (logs in $OUT)" || say "FAILED (logs in $OUT)"
exit $fail
