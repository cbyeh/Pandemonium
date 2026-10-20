/**
 * C port of libdragon's audio mixer RSP overlay (src/audio/rsp_mixer.S): PCM and
 * in-mixer VADPCM. Semantics follow the ucode and libdragon's tests/test_wav64.c
 * reference (the VADPCM decode is bit-exact), but resampling is linear instead
 * of 4-tap Hermite to keep the CPU cost low.
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
#define CH_VLOOP      (1<<6)
#define CH_CLEAR      (1<<7)

#define MAX_CHANNELS  32
#define VWIN          32        // frames of the VADPCM decode window

static int32_t accum[MAX_SAMPLES * 2];
static int round_ns;

// Channel table filled by MIX_SETCHANNEL (RDRAM addresses)
static uint32_t ch_codebook[MAX_CHANNELS], ch_state[MAX_CHANNELS], ch_loop_state[MAX_CHANNELS];

// Per-codebook products r * coeff for every residual value r, which turn the
// residual convolution of the decoder into table lookups. t[p][m][r+8] is
// r * 2048 for m = 0 and r * c[m-1] otherwise, c being the second vector of
// predictor p. A few codebooks are cached (music planes + SFX).
#define VTABS 8
typedef struct {
    uint32_t addr;
    int16_t cb[64];             // predictors, also validates the slot
    int32_t t[4][8][16];
    int64_t sp[4][2][4];        // state vectors, lanes i and i+4 packed in one word
} vtab_t;
static vtab_t vtabs[VTABS];
static int vtab_next;

// Decoded samples of frames [vf, vf+vn) start at vbuf[8]. The state that
// decodes a frame is just the eight samples before it, so vbuf[0..7] is the
// state of frame vf and the decoder runs in place.
static int16_t vbuf[8 + VWIN * 16 + 3];
static struct {
    const uint8_t *ptr;         // compressed frames, frame f at ptr + f*stride
    const int16_t *cb;          // 8 predictor vectors + 3 loop taps
    const vtab_t *tab;
    uint32_t loop_state;
    uint32_t vf, vn, nframes, lens;
    uint32_t avail;             // samples below this absolute index can be read
    int bits, stride;
} V;

/** Volume ramp step: v4 is the volume in 1/4 units, d the per-sample delta, stops at t. */
static inline void ramp(int32_t *v4, int16_t *d, int16_t t)
{
    if (!*d) return;
    *v4 += *d;
    if ((*d > 0) == (*v4 >= t * 4)) { *v4 = t * 4; *d = 0; }
}

/** Accumulate s at volumes vl4/vr4 (1/4 units). Stereo planes leave one side silent. */
static inline void mix(int32_t *acc, int32_t s, int32_t vl4, int32_t vr4)
{
    if (vl4 == vr4) {
        int32_t v = (s * (vl4 >> 2)) >> 15;
        acc[0] += v; acc[1] += v;
        return;
    }
    if (vl4 >> 2) acc[0] += (s * (vl4 >> 2)) >> 15;
    if (vr4 >> 2) acc[1] += (s * (vr4 >> 2)) >> 15;
}

/** Mix n consecutive samples at a constant volume (fx15): the no-ramp, unit-step case. */
static void mix_run(int32_t *acc, const int16_t *w, int n, int32_t vl, int32_t vr)
{
    if (vl == vr) {
        if (!vl) return;
        for (int i = 0; i < n; i++) { int32_t v = (w[i] * vl) >> 15; acc[2*i] += v; acc[2*i+1] += v; }
        return;
    }
    if (vl) for (int i = 0; i < n; i++) acc[2*i] += (w[i] * vl) >> 15;
    if (vr) for (int i = 0; i < n; i++) acc[2*i+1] += (w[i] * vr) >> 15;
}

static inline int32_t clamp16(int32_t v)
{
    return (uint32_t)(v + 32768) > 65535 ? (v >> 31) ^ 0x7FFF : v;
}

static const vtab_t *vadpcm_tables(uint32_t addr, const int16_t *cb)
{
    for (int i = 0; i < VTABS; i++) {
        vtab_t *v = &vtabs[i];
        if (v->addr != addr) continue;
        const uint32_t *x = (const uint32_t*)v->cb, *y = (const uint32_t*)cb;
        int j = 0;
        while (j < 32 && x[j] == y[j]) j++;
        if (j == 32) return v;
    }
    vtab_t *v = &vtabs[vtab_next++ % VTABS];
    v->addr = addr;
    memcpy(v->cb, cb, sizeof(v->cb));
    for (int p = 0; p < 4; p++)
        for (int m = 0; m < 8; m++)
            for (int r = 0; r < 16; r++)
                v->t[p][m][r] = (r - 8) * (m ? v->cb[p * 16 + 8 + m - 1] : 2048);
    for (int p = 0; p < 4; p++)
        for (int k = 0; k < 2; k++)
            for (int i = 0; i < 4; i++)
                v->sp[p][k][i] = ((int64_t)v->cb[p * 16 + k * 8 + i + 4] << 32) + v->cb[p * 16 + k * 8 + i];
    return v;
}

/**
 * Decode a vector of 8 samples into out[0..7] from residuals packed MSB first in
 * w; out[-2], out[-1] are the previous samples. Classic order-2 VADPCM in the
 * ucode's exact arithmetic: a linear function of the two samples before the
 * vector and of its residuals.
 */
static inline __attribute__((always_inline)) void vadpcm_vec(const int32_t *t,
    const int64_t *p0, const int64_t *p1, uint32_t w, int bits, int scale, int16_t *out)
{
    int sh = 32 - bits;
    #define R(k) const int32_t *a##k = t + ((int32_t)w >> sh); w <<= bits
    R(0); R(1); R(2); R(3); R(4); R(5); R(6); R(7);
    #undef R
    // The contribution of the state, two outputs per 64-bit product (each fits 32 bits)
    int32_t s2 = out[-2], s1 = out[-1];
    #define PAIR(i, j) int64_t P##i = s2 * p0[i] + s1 * p1[i]; int32_t S##i = P##i, S##j = (P##i - S##i) >> 32
    PAIR(0, 4); PAIR(1, 5); PAIR(2, 6); PAIR(3, 7);
    #undef PAIR
    #define OUT(i, conv) out[i] = clamp16((int32_t)((uint32_t)S##i + ((uint32_t)(conv) << scale)) >> 11)
    OUT(0, a0[0]);
    OUT(1, a1[0] + a0[16]);
    OUT(2, a2[0] + a1[16] + a0[32]);
    OUT(3, a3[0] + a2[16] + a1[32] + a0[48]);
    OUT(4, a4[0] + a3[16] + a2[32] + a1[48] + a0[64]);
    OUT(5, a5[0] + a4[16] + a3[32] + a2[48] + a1[64] + a0[80]);
    OUT(6, a6[0] + a5[16] + a4[32] + a3[48] + a2[64] + a1[80] + a0[96]);
    OUT(7, a7[0] + a6[16] + a5[32] + a4[48] + a3[64] + a2[80] + a1[96] + a0[112]);
    #undef OUT
}

/**
 * Decode one frame (16 samples) into out[0..15].
 * shortcut: assumes predictor < 4 and residual << scale fitting 16 bits (the
 * ucode saturates it), which audioconv64 guarantees (scale <= 12).
 */
static void vadpcm_frame(const uint8_t *f, int bits, const vtab_t *vt, int16_t *out)
{
    int scale = f[0] >> 4, pi = f[0] & 3;
    const int32_t *t = vt->t[pi][0] + 8;
    uint64_t pl;
    memcpy(&pl, f + 1, 8);      // big-endian: residual 0 is in the top bits
    vadpcm_vec(t, vt->sp[pi][0], vt->sp[pi][1], pl >> 32, bits, scale, out);
    vadpcm_vec(t, vt->sp[pi][0], vt->sp[pi][1], pl << (8 * bits) >> 32, bits, scale, out + 8);
}

/**
 * Decode up to frame f, and ahead up to fmax (the last one the command can
 * need) when it fits, stopping at the end of the waveform. The window slides
 * to keep the frames from `keep` on.
 */
static void vadpcm_fill(uint32_t f, uint32_t fmax, uint32_t keep)
{
    if (fmax < f) fmax = f;
    if (fmax > keep + VWIN - 1) fmax = keep + VWIN - 1;
    if (fmax > V.nframes - 1) fmax = V.nframes - 1;
    while (V.vf + V.vn <= fmax && V.vf + V.vn < V.nframes) {
        if (V.vn == VWIN) {
            uint32_t d = keep - V.vf < V.vn ? keep - V.vf : V.vn;
            memmove(vbuf, vbuf + d * 16, (8 + (V.vn - d) * 16) * 2);
            V.vf += d; V.vn -= d;
        }
        uint32_t n = V.vn, end = fmax - V.vf + 1 < VWIN ? fmax - V.vf + 1 : VWIN;
        const uint8_t *in = V.ptr + (V.vf + n) * V.stride;
        int stride = V.stride, bits = V.bits;
        const vtab_t *tab = V.tab;
        for (; n < end; n++, in += stride)
            vadpcm_frame(in, bits, tab, vbuf + 8 + n * 16);
        V.vn = n;
    }
    V.avail = (V.vf + V.vn) * 16;
    if (V.vf + V.vn >= V.nframes && V.vf < V.nframes) {
        // Interpolation reads past the last sample: those taps are the first
        // three of the loop when there is one, the last sample held otherwise.
        int16_t *t = vbuf + 8 + V.lens - V.vf * 16;
        if (V.loop_state) { t[0] = V.cb[64]; t[1] = V.cb[65]; t[2] = V.cb[66]; }
        else t[0] = t[1] = t[2] = t[-1];
        V.avail = V.lens + 3;
    }
}

/** Restart decoding at the frame of `pos` from the 16-byte state in vbuf[0..7]. */
static inline void vadpcm_seek(uint32_t pos)
{
    V.vf = pos >> (FRAC_BITS + 4); V.vn = 0; V.avail = V.vf * 16;
}

static void mix_vadpcm(const uint32_t *a, uint32_t flags, int ns)
{
    int ch = (a[0] >> 19) & 0x1F;
    uint32_t cbp = ch_codebook[ch];
    V.bits = (cbp & 3) + 2;
    V.stride = 2 * V.bits + 1;
    V.cb = hle_rdram(cbp & ~3u);
    V.tab = vadpcm_tables(cbp, V.cb);
    V.loop_state = ch_loop_state[ch];
    V.ptr = hle_rdram(a[6]);

    uint32_t pos = a[2], step = a[3], len = a[4], loop_len = a[5];
    int16_t lvol = a[1] >> 16, rvol = a[1];
    int16_t dl = a[7] >> 16, dr = a[7];
    int16_t tl = a[8] >> 16, tr = a[8];
    V.lens = len >> FRAC_BITS;
    V.nframes = (V.lens + 15) >> 4;
    if (!V.nframes) ns = 0;         // nothing to decode: the ucode only saves the state
    memcpy(vbuf, hle_rdram((flags & CH_VLOOP) ? V.loop_state : ch_state[ch]), 16);
    vadpcm_seek(pos);

    // Decoded even when silent: the saved state must follow the position
    int32_t *acc = accum;
    int32_t vl4 = lvol * 4, vr4 = rvol * 4;
    uint32_t avail = V.avail, vb = V.vf * 16 - 8;     // vbuf[si - vb] is sample si
    for (int i = 0; i < ns; ) {
        if (pos >= len) {
            if (!loop_len) break;
            pos -= loop_len;
            if (V.loop_state) memcpy(vbuf, hle_rdram(V.loop_state), 16);
            else memmove(vbuf, vbuf + V.vn * 16, 16);
            vadpcm_seek(pos);
            avail = V.avail; vb = V.vf * 16 - 8;
        }
        // The ucode's taps put the output sample at (pos >> frac) + 1
        uint32_t si = (pos >> FRAC_BITS) + 1;
        if (si + 1 >= avail) {
            uint32_t last = ((pos + (uint32_t)(ns - 1 - i) * step) >> FRAC_BITS) + 2;
            vadpcm_fill((si + 1) >> 4, last >> 4, si >> 4);
            avail = V.avail; vb = V.vf * 16 - 8;
        }
        const int16_t *w = vbuf + (si - vb);
        if (step == 1 << FRAC_BITS && !(pos & ((1u << FRAC_BITS) - 1)) && !dl && !dr) {
            // Up to the end of the window or of the waveform
            int n = avail - si, ne = ((len - pos - 1) >> FRAC_BITS) + 1;
            if (n > ne) n = ne;
            if (n > ns - i) n = ns - i;
            mix_run(acc, w, n, vl4 >> 2, vr4 >> 2);
            i += n; acc += 2 * n; pos += n << FRAC_BITS;
            continue;
        }
        int32_t s = w[0];
        uint32_t x = pos & ((1u << FRAC_BITS) - 1);
        if (x) s += ((w[1] - s) * (int32_t)x) >> FRAC_BITS;
        mix(acc, s, vl4, vr4);
        ramp(&vl4, &dl, tl); ramp(&vr4, &dr, tr);
        pos += step;
        i++; acc += 2;
    }

    // Save the state of the frame the round ends on
    uint32_t ef = pos >> (FRAC_BITS + 4);
    if (ef > V.vf + V.vn) vadpcm_fill(ef - 1, ef - 1, ef);
    memcpy(hle_rdram(ch_state[ch]), vbuf + (ef <= V.vf + V.vn ? ef - V.vf : V.vn) * 16, 16);
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
        mix_vadpcm(a, flags, ns);
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
            if (step == 2 << FRAC_BITS && !(pos & frac_mask) && !dl && !dr) {
                int n = (len - pos - 1) / step + 1;
                if (n > ns - i) n = ns - i;
                mix_run(acc, wave + si, n, vl4 >> 2, vr4 >> 2);
                i += n - 1; acc += 2 * (n - 1); pos += n * step;
                continue;
            }
            int32_t s = wave[si];
            uint32_t x = pos & frac_mask;
            if (x) s += ((wave[si + 1] - s) * (int32_t)(x >> 1)) >> FRAC_BITS;
            mix(acc, s, vl4, vr4);
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
        mix(acc, s, vl4, vr4);
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
    case 1: {   // MIX_SETCHANNEL: VADPCM pointers of a channel
        int ch = (a[0] >> 16) & (MAX_CHANNELS - 1);
        ch_codebook[ch] = a[1]; ch_state[ch] = a[2]; ch_loop_state[ch] = a[3];
        return;
    }
    case 3:     // MIX_SETSTATE: seed a VADPCM state
        memcpy(hle_rdram(a[1]), &a[2], 16);
        return;
    case 2: {   // MIX_FLUSH
        int ns = a[0] & 0xFFFF;
        if (ns > MAX_SAMPLES) ns = MAX_SAMPLES;
        int16_t *out = hle_rdram(a[1]);
        uint32_t *o = (uint32_t*)out;       // one store per stereo sample (big-endian: L first)
        for (int i = 0; i < ns; i++)
            o[i] = (clamp16(accum[2*i]) << 16) | (clamp16(accum[2*i+1]) & 0xFFFF);
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
