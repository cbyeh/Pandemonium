#!/usr/bin/env python3
"""
Replaces libdragon's EMUX opcodes (emulator-extension probes, see emux.h) in the
code sections of an N64 ELF. Hardware ignores them and recent emulators trap
them, but Delta's 2019 Mupen64Plus core halts emulation on them. EMUX ops that
return a value in rd (XDETECT, XPROFREAD) become "ori rd, $zero, 0" (= EMUX not
present); all the others become NOPs.

usage: patch_emux.py file.elf   (patches in place)
"""
import struct, sys

def main(path):
    d = bytearray(open(path, 'rb').read())
    assert d[:4] == b'\x7fELF' and d[4] == 1 and d[5] == 2, "expected a 32-bit big-endian ELF"
    shoff, = struct.unpack_from('>I', d, 0x20)
    shentsize, shnum = struct.unpack_from('>HH', d, 0x2E)
    patched = 0
    for i in range(shnum):
        sh = shoff + i * shentsize
        sh_type, sh_flags, _, sh_offset, sh_size = struct.unpack_from('>IIIII', d, sh + 4)
        if sh_type != 1 or not (sh_flags & 4):      # SHT_PROGBITS + SHF_EXECINSTR
            continue
        for off in range(sh_offset, sh_offset + sh_size - 3, 4):
            w, = struct.unpack_from('>I', d, off)
            # COP0 with the CO bit set and function 0x20-0x3F: only EMUX uses these
            if (w >> 25) != 0x21 or (w & 0x3F) < 0x20:
                continue
            func, rd = w & 0x3F, (w >> 20) & 0x1F
            new = (0x34000000 | (rd << 16)) if func in (0x20, 0x29) and rd else 0
            struct.pack_into('>I', d, off, new)
            patched += 1
    open(path, 'wb').write(d)
    print(f"    [EMUX] {path}: {patched} opcodes patched")

if __name__ == '__main__':
    main(sys.argv[1])
