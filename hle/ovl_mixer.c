/**
 * C port of libdragon's audio mixer RSP overlay (src/audio/rsp_mixer.S), PCM only:
 * the HLE build ships uncompressed wav64 files (VADPCM decoding on the emulated
 * CPU would cost too much). Semantics follow the ucode and libdragon's
 * tests/test_wav64.c reference, but resampling is linear instead of 4-tap
 * Hermite to keep the CPU cost low.
 */
#include <libdragon.h>
#include <string.h>
#include "hle.h"

#define MAX_SAMPLES   512
#define FRAC_BITS     12        // WAVEFORM_POS_FRAC_BITS

#define CH_BPS_MASK   (3<<0)
#define CH_16BIT      (1<<2)
#define CH_STEREO_SUB (1<<4)
#define CH_VADPCM     (1<<5)
#define CH_CLEAR      (1<<7)

static int32_t accum[MAX_SAMPLES * 2];
static int round_ns;

/** Volume ramp step: v4 is the volume in 1/4 units, d the per-sample delta, stops at t. */
static inline void ramp(int32_t *v4, int16_t *d, int16_t t)
{
    if (!*d) return;
    *v4 += *d;
    if ((*d > 0) == (*v4 >= t * 4)) { *v4 = t * 4; *d = 0; }
}

static void mix_channel(const uint32_t *a)
{
    uint32_t flags = (a[0] >> 11) & 0xFF;
    int ns = (a[0] & 0x1FF) * 2;
    round_ns = ns;
    if (flags & CH_CLEAR)
        memset(accum, 0, ns * 2 * sizeof(int32_t));

    uint32_t ptr = a[6];
    if (!ptr || ns == 0) return;
    if (flags & CH_VADPCM) {
        hle_warn_once("vadpcm", "VADPCM audio is not supported by the HLE mixer (build audio with --wav-compress 0)");
        return;
    }

    int16_t lvol = a[1] >> 16, rvol = a[1];
    uint32_t pos = a[2], step = a[3], len = a[4], loop_len = a[5];
    int16_t dl = a[7] >> 16, dr = a[7];
    int16_t tl = a[8] >> 16, tr = a[8];
    if (!lvol && !rvol && !dl && !dr) return;     // silent channel

    int bps = flags & CH_BPS_MASK;              // log2(bytes per frame)
    bool is16 = flags & CH_16BIT;
    int sub = (flags & CH_STEREO_SUB) ? (is16 ? 2 : 1) : 0;
    const uint8_t *base = hle_rdram(ptr);
    int32_t *acc = accum;

    int32_t vl4 = lvol * 4, vr4 = rvol * 4;
    // Fast path: 16-bit mono (the common case for this game)
    if (is16 && bps == 1) {
        const int16_t *wave = (const int16_t*)base;
        uint32_t frac_mask = (1u << (FRAC_BITS + 1)) - 1;
        for (int i = 0; i < ns; i++, acc += 2) {
            if (pos >= len) {
                if (!loop_len) break;
                pos -= loop_len;
            }
            // The ucode's taps put the output sample at (pos >> frac) + 1
            uint32_t si = (pos >> (FRAC_BITS + 1)) + 1;
            int32_t s = wave[si];
            uint32_t x = pos & frac_mask;
            if (x) s += ((wave[si + 1] - s) * (int32_t)(x >> 1)) >> FRAC_BITS;
            if (vl4 == vr4) {
                int32_t v = (s * (vl4 >> 2)) >> 15;
                acc[0] += v; acc[1] += v;
            } else {
                acc[0] += (s * (vl4 >> 2)) >> 15;
                acc[1] += (s * (vr4 >> 2)) >> 15;
            }
            ramp(&vl4, &dl, tl); ramp(&vr4, &dr, tr);
            pos += step;
        }
        return;
    }

    uint32_t frac_mask = (1u << (FRAC_BITS + bps)) - 1;
    for (int i = 0; i < ns; i++, acc += 2) {
        if (pos >= len) {
            if (!loop_len) break;
            pos -= loop_len;
        }
        uint32_t off = ((pos >> (FRAC_BITS + bps)) + 1) << bps;
        int32_t y1, y2;
        if (is16) {
            y1 = *(const int16_t*)(base + off + sub);
            y2 = *(const int16_t*)(base + off + (1 << bps) + sub);
        } else {
            y1 = (int8_t)base[off + sub] << 8;
            y2 = (int8_t)base[off + (1 << bps) + sub] << 8;
        }
        int32_t s = y1;
        uint32_t x = pos & frac_mask;
        if (x) s += ((y2 - y1) * (int32_t)(x >> bps)) >> FRAC_BITS;
        acc[0] += (s * (vl4 >> 2)) >> 15;
        acc[1] += (s * (vr4 >> 2)) >> 15;
        ramp(&vl4, &dl, tl); ramp(&vr4, &dr, tr);
        pos += step;
    }
}

void ovl_mixer(uint32_t cmd, const uint32_t *a, int n)
{
    switch (cmd) {
    case 0:     // MIX_CHANNEL
        mix_channel(a);
        return;
    case 1:     // MIX_SETCHANNEL (VADPCM pointers)
    case 3:     // MIX_SETSTATE (VADPCM state)
        return;
    case 2: {   // MIX_FLUSH
        int ns = a[0] & 0xFFFF;
        if (ns > MAX_SAMPLES) ns = MAX_SAMPLES;
        int16_t *out = hle_rdram(a[1]);
        for (int i = 0; i < ns * 2; i++) {
            int32_t v = accum[i];
            out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        }
#ifdef HLE_TEST_AUDIOLOG
        { static int rounds; int peak = 0;
          for (int i = 0; i < ns * 2; i++) { int v = out[i] < 0 ? -out[i] : out[i]; if (v > peak) peak = v; }
          if ((rounds++ % 50) == 0) hle_logf("audio round %d: ns=%d peak=%d", rounds, ns, peak); }
#endif
        uint32_t *mark = hle_rdram(a[3]);
        mark[0] = a[2]; mark[1] = a[2];
        (void)round_ns;
        return;
    }
    case 4:     // MIX_COPY: RDRAM to RDRAM
        memmove(hle_rdram(a[1]), hle_rdram(a[2]), a[0] & 0xFFFF);
        return;
    }
}
