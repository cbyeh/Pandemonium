#!/bin/bash
# Rebuilds ipl3_hle.z64: libdragon's IPL3 plus ipl3-emu.patch, which lets
# it boot on emulators whose RDRAM register emulation only supports Nintendo's IPL3
# (Mupen64Plus/Delta: rdram_init detects 0 bytes and the boot jumps into nowhere),
# and that do not mask virtual addresses written to PI_DRAM_ADDR (Delta's 2019 core).
#
# usage: LIBDRAGON_SRC=/path/to/libdragon N64_INST=/path/to/toolchain ./build_ipl3.sh [dev]
#   dev: debug build that logs the boot via ISViewer (for troubleshooting)
set -euo pipefail
: "${LIBDRAGON_SRC:?set LIBDRAGON_SRC to your libdragon checkout}"
: "${N64_INST:?set N64_INST to your toolchain}"
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# boot/Makefile writes ../tools/ipl3.h, so keep the same layout
mkdir -p "$TMP/tools"
cp -R "$LIBDRAGON_SRC/boot" "$TMP/boot"
rm -rf "$TMP/boot/build"
(cd "$TMP" && patch -p1 < "$HERE/ipl3-emu.patch")
if [ "${1:-}" = dev ]; then
    make -C "$TMP/boot" >/dev/null
    cp "$TMP/boot/bin/ipl3_dev.z64" "$HERE/ipl3_hle_dev.z64"
    echo "built $HERE/ipl3_hle_dev.z64"
else
    make -C "$TMP/boot" PROD=1 >/dev/null
    cp "$TMP/boot/bin/ipl3_prod.z64" "$HERE/ipl3_hle.z64"
    # Header CRC (unused by libdragon's IPL3) of a Mupen64Plus ROM-database entry with
    # CountPerOp=1, EEPROM 4K, 1 player and nothing else (Mischief Makers (E)): Mupen/Delta
    # then emulate 1 count per CPU instruction instead of 2, doubling the CPU time per frame.
    printf '\x41\x8b\xda\x98\x24\x8a\x0f\x58' | dd of="$HERE/ipl3_hle.z64" bs=1 seek=16 conv=notrunc 2>/dev/null
    echo "built $HERE/ipl3_hle.z64"
fi
