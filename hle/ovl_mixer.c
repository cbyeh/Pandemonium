/**
 * C port of libdragon's audio mixer RSP overlay (src/audio/rsp_mixer.S): PCM and
 * in-mixer VADPCM. Semantics follow the ucode and libdragon's tests/test_wav64.c
 * reference (the VADPCM decode is bit-exact), but resampling is linear instead
 * of 4-tap Hermite to keep the CPU cost low.
 */
#include <libdragon.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
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

// ---- Native audio: a round as a standard N64 audio task ----
//
// HLE emulators (rsp-hle in Mupen64Plus and Delta) run libultra's audio microcode
// natively, so a round's VADPCM channels go out as one ABI1 audio list (decode,
// resample, mix, interleave) instead of being mixed above on the emulated CPU, which
// took ~20% of a fight's frame. A channel the list can't express (PCM, 2/3-bit VADPCM,
// a loop wrapping inside the round, 2x pitch or more) is mixed in C as before, and that
// mix is the list's starting point. The list also copies a marker: if the emulator didn't
// run it (LLE audio), the round is redone in C and the C mixer is used from then on.
//
// The task's DMEM holds 128 samples of L and R at a time, so the round goes in chunks;
// each chunk saves, for the next one, the decoder state it needs (the 8 samples before
// its first frame) next to RDRAM where that chunk's ADPCM command loads it.

enum { A_ADPCM = 1, A_CLEARBUFF = 2, A_LOADBUFF = 4, A_RESAMPLE = 5, A_SAVEBUFF = 6,
       A_SETBUFF = 8, A_LOADADPCM = 11, A_MIXER = 12, A_INTERLEAVE = 13 };
#define ABI_DMEM    2624        // DMEM the list can use (0x5C0..0xFFF)
#define NAT_CHUNK   128
#define NAT_CHUNKS  (MAX_SAMPLES / NAT_CHUNK)
#define NAT_L       ((NAT_CHUNK + 16) * 2)      // L and R (+ the mixer's overrun)
#define NAT_W       (2 * NAT_L)                 // work area, and the interleaved output
#define NAT_PRO     (NAT_CHUNKS * 8)            // words: per chunk, load or clear L and R
#define FRAC_MASK   ((1u << FRAC_BITS) - 1)
#define UC(p)       ((__typeof__(&(p)[0]))hle_rdram(PhysicalAddr(p)))

static int nat_state;           // 0 untested, 1 runs natively, -1 C only
static uint32_t alist[4096] __attribute__((aligned(16)));
static uint32_t *al;
// ADPCM state per channel and chunk: 16 samples loaded from st + ofs, the last 8 of which
// a chunk (or the CPU, for the first) saves at st + 16
static int16_t nat_st[MAX_CHANNELS][NAT_CHUNKS][24] __attribute__((aligned(16)));
static int16_t nat_shadow[MAX_CHANNELS][16] __attribute__((aligned(16)));  // round-end states
static uint16_t nat_rs[MAX_CHANNELS][8] __attribute__((aligned(16)));      // resample states
static int16_t nat_stage[2][MAX_SAMPLES + 8] __attribute__((aligned(16))); // the C part (L, R)
static uint32_t nat_magic[2] __attribute__((aligned(16))) = { 0x50414E44, 0x41554449 };
static uint32_t nat_mark[2] __attribute__((aligned(16)));
static uint32_t nat_text[64] __attribute__((aligned(16)));                 // break: an LLE RSP halts
// rsp-hle tells the ABI from these words of the ucode data (libultra's "audio" ucode)
static uint32_t nat_data[16] __attribute__((aligned(16))) = { [0] = 1, [10] = 0x1e24138c, [12] = 0xf0000f00 };
static struct {
    uint32_t args[9], state, cb, pos, step, ptr, nframes, ef;
    int ch, nv, ofs; bool direct; int16_t vol[2], d[2], t[2];
} nat_ch[MAX_CHANNELS];
static int nat_n, nat_ns;
static bool nat_round, c_used;

static inline void acmd(uint32_t w1, uint32_t w2) { al[0] = w1; al[1] = w2; al += 2; }
#define SETBUFF(in, out, cnt) acmd((uint32_t)A_SETBUFF << 24 | (in), (uint32_t)(out) << 16 | (cnt))

static void nat_begin(int ns)
{
#ifdef HLE_NO_NATIVE_AUDIO  // test aid: always the C mixer
    nat_state = -1;
#endif
    nat_n = 0; nat_ns = ns; c_used = false;
    nat_round = nat_state >= 0 && ns > 0 && ns <= MAX_SAMPLES;
}

/** Takes a VADPCM channel for the round's audio list; false = mix it in C. */
static bool nat_channel(const uint32_t *a, uint32_t flags, int ns)
{
    if (!nat_round || ns != nat_ns || !(flags & CH_VADPCM) || nat_n == MAX_CHANNELS) return false;
    int ch = (a[0] >> 19) & 0x1F;
    uint32_t cbp = ch_codebook[ch];
    uint32_t pos = a[2], step = a[3], len = a[4], loop_len = a[5];
    if ((cbp & 3) != 2 || !step || step >= 2u << FRAC_BITS) return false;     // 4-bit, pitch < 2x
    uint32_t nframes = ((len >> FRAC_BITS) + 15) >> 4;
    if (!nframes || pos >= len) return false;
    int nv = ns;                                    // samples before the waveform ends
    if (pos + (uint64_t)(ns - 1) * step >= len) {
        if (loop_len) return false;                 // wraps inside the round
        nv = (len - pos + step - 1) / step;
    }
    typeof(nat_ch[0]) *c = &nat_ch[nat_n++];
    memcpy(c->args, a, sizeof(c->args));
    c->args[0] &= ~((uint32_t)CH_CLEAR << 11);      // a redo in C must not clear the mix
    c->state = ch_state[ch]; c->cb = cbp; c->ch = ch;
    c->pos = pos; c->step = step; c->ptr = a[6]; c->nframes = nframes; c->nv = nv;
    c->ef = (pos + (uint32_t)nv * step) >> (FRAC_BITS + 4);
    c->direct = step == 1u << FRAC_BITS && !(pos & FRAC_MASK);
    c->vol[0] = a[1] >> 16; c->vol[1] = a[1]; c->d[0] = a[7] >> 16; c->d[1] = a[7];
    c->t[0] = a[8] >> 16; c->t[1] = a[8];
    memcpy(UC(nat_st[ch][0]) + 8, hle_rdram((flags & CH_VLOOP) ? ch_loop_state[ch] : ch_state[ch]), 16);
    c->ofs = 0;
    return true;
}

/** One channel's part of chunk k (samples [i0, i0 + n) of the round); false if it doesn't fit. */
static bool nat_chunk_channel(typeof(nat_ch[0]) *c, int k, int i0, int n, int ns, uint32_t *cb)
{
    int nv = c->nv - i0; if (nv > n) nv = n;
    if (nv <= 0) return true;                       // ended in an earlier chunk
    bool last = i0 + n >= c->nv;
    uint32_t pos = c->pos + (uint32_t)i0 * c->step, step = c->step;
    // The ucode's taps put the output sample at (pos >> frac) + 1: a 4-tap resample from
    // pos >> frac centres there, a straight copy starts there
    uint32_t t0 = pos >> FRAC_BITS;
    uint32_t tl = ((pos + (uint32_t)(nv - 1) * step) >> FRAC_BITS) + (c->direct ? 1 : 3);
    uint32_t f0 = pos >> (FRAC_BITS + 4), f1 = tl >> 4;
    // the next chunk starts decoding at g; the round's end state is frame e's
    uint32_t g = last ? 0 : (pos + (uint32_t)n * step) >> (FRAC_BITS + 4);
    uint32_t e = c->ef;
    if (!last && g > f1 + 1) f1 = g - 1;
    if (last && e > f1 + 1) f1 = e - 1;
    if (f1 > c->nframes - 1) f1 = c->nframes - 1;
    uint32_t nf = f1 - f0 + 1;
    if (e > f1 + 1) e = f1 + 1;

    // DMEM: L, R | compressed frames | decoded (16 history + frames + 16 zeros) | resampled.
    // The plugin mixes 32-bit words: the first sample read and L/R must align alike
    uint32_t src = c->ptr + f0 * 9, cs = (src & 7) + nf * 9, kk = c->direct ? t0 + 1 : t0;
    uint32_t DEC = NAT_W + ((cs + 15) & ~15u) + 2 * (kk & 1);
    uint32_t RES = (DEC + 64 + nf * 32 + 15) & ~15u;
    if (RES + (c->direct ? 0 : ((nv * 2 + 15) & ~15u) + 32) > ABI_DMEM) return false;
    if (al + 64 + 4 * ((nv + 15) / 16 + 1) > alist + sizeof(alist) / 4 - 48) return false;

    int ch = c->ch;
    if (c->cb != *cb) { acmd((uint32_t)A_LOADADPCM << 24 | 128, c->cb & ~3u); *cb = c->cb; }
    SETBUFF(NAT_W, 0, cs);
    acmd((uint32_t)A_LOADBUFF << 24, src & ~7u);
    SETBUFF(NAT_W + (src & 7), DEC, nf * 32);
    acmd((uint32_t)A_ADPCM << 24, PhysicalAddr(nat_st[ch][k]) + c->ofs);
    acmd((uint32_t)A_CLEARBUFF << 24 | (DEC + 32 + nf * 32), 32);
    if (!last) {        // the 8 samples before frame g, for the next chunk
        uint32_t s = DEC + (8 + 16 * (g - f0)) * 2;
        SETBUFF(0, s & ~3u, 24);
        acmd((uint32_t)A_SAVEBUFF << 24, PhysicalAddr(nat_st[ch][k + 1]) + 16);
        c->ofs = s & 3;
    } else {            // and before frame e, the state of the next round
        uint32_t s = DEC + (8 + 16 * (e - f0)) * 2;
        SETBUFF(0, s & ~3u, 24);
        acmd((uint32_t)A_SAVEBUFF << 24, PhysicalAddr(nat_shadow[ch]));
        c->ofs = s & 3;
    }

    if (!(c->vol[0] || c->vol[1] || c->d[0] || c->d[1])) return true;
    uint32_t S;
    if (c->direct) {
        S = DEC + (16 + t0 + 1 - 16 * f0) * 2;
    } else {
        uint32_t T = DEC + (16 + t0 - 16 * f0) * 2;      // the 4 taps, saved as the state
        UC(nat_rs[ch])[4] = (pos & FRAC_MASK) << 4;
        SETBUFF(0, T, 8);
        acmd((uint32_t)A_SAVEBUFF << 24, PhysicalAddr(nat_rs[ch]));
        SETBUFF(T + 8, RES, nv * 2);
        acmd((uint32_t)A_RESAMPLE << 24 | (step << 3), PhysicalAddr(nat_rs[ch]));
        S = RES;
    }
    if (nv < n) acmd((uint32_t)A_CLEARBUFF << 24 | (S + nv * 2), 32);     // the mixer reads 16 at a time
    for (int s = 0; s < 2; s++) {
        uint32_t dst = s ? NAT_L : 0;
        if (!c->d[s]) {
            if (!c->vol[s]) continue;
            SETBUFF(0, 0, nv * 2);
            acmd((uint32_t)A_MIXER << 24 | (uint16_t)c->vol[s], S << 16 | dst);
            continue;
        }
        // A ramp: each block of 16 at the volume of its middle sample. The channel's
        // volume moves on with the samples (vol and d are 1/4 units here)
        int32_t v4 = c->vol[s] * 4; int16_t dd = c->d[s];
        SETBUFF(0, 0, 32);
        for (int i = 0; i < nv; i += 16) {
            int32_t m4 = v4; int16_t md = dd;
            for (int j = 0; j < 8; j++) ramp(&m4, &md, c->t[s]);
            acmd((uint32_t)A_MIXER << 24 | (uint16_t)(m4 >> 2), (S + i * 2) << 16 | (dst + i * 2));
            for (int j = 0; j < 16; j++) ramp(&v4, &dd, c->t[s]);
        }
        c->vol[s] = v4 >> 2; c->d[s] = dd;
    }
    return true;
}

/** Runs the round as an audio task writing ns stereo samples at out; false = flush in C. */
static bool nat_flush(int ns, uint32_t out)
{
    if (!nat_n) return false;
    bool ok = ns == nat_ns && !(out & 7);
#ifdef HLE_TEST_AUDIO_AB    // test aid: mix the round in C too and compare
    static int16_t ref[MAX_SAMPLES * 2], refst[MAX_CHANNELS][8], keep[MAX_CHANNELS][8];
    static int32_t acc0[MAX_SAMPLES * 2];
    if (ns == nat_ns) {
        memcpy(acc0, accum, ns * 2 * sizeof(int32_t));
        for (int i = 0; i < nat_n; i++) memcpy(keep[i], hle_rdram(nat_ch[i].state), 16);
        for (int i = 0; i < nat_n; i++) mix_channel(nat_ch[i].args);
        for (int i = 0; i < ns * 2; i++) ref[i] = clamp16(accum[i]);
        for (int i = 0; i < nat_n; i++) { memcpy(refst[i], hle_rdram(nat_ch[i].state), 16); memcpy(hle_rdram(nat_ch[i].state), keep[i], 16); }
        memcpy(accum, acc0, ns * 2 * sizeof(int32_t));
    }
#endif
    if (ok && c_used) {         // the list starts from what was mixed in C
        int16_t *sl = UC(nat_stage[0]), *sr = UC(nat_stage[1]);
        for (int i = 0; i < ns; i++) { sl[i] = clamp16(accum[2*i]); sr[i] = clamp16(accum[2*i+1]); }
    }
    al = alist;
    uint32_t cb = 0;
    for (int i0 = 0, k = 0; ok && i0 < ns; i0 += NAT_CHUNK, k++) {
        int n = ns - i0 < NAT_CHUNK ? ns - i0 : NAT_CHUNK;
        if (c_used) {
            SETBUFF(0, 0, n * 2); acmd((uint32_t)A_LOADBUFF << 24, PhysicalAddr(&nat_stage[0][i0]));
            SETBUFF(NAT_L, 0, n * 2); acmd((uint32_t)A_LOADBUFF << 24, PhysicalAddr(&nat_stage[1][i0]));
        } else {
            acmd((uint32_t)A_CLEARBUFF << 24, n * 2);
            acmd((uint32_t)A_CLEARBUFF << 24 | NAT_L, n * 2);
        }
        for (int j = 0; ok && j < nat_n; j++)
            ok = nat_chunk_channel(&nat_ch[j], k, i0, n, ns, &cb);
        SETBUFF(0, NAT_W, n * 2);
        acmd((uint32_t)A_INTERLEAVE << 24, NAT_L);
        SETBUFF(0, NAT_W, n * 4);
        acmd((uint32_t)A_SAVEBUFF << 24, out + i0 * 4);
    }
    if (ok) {
        SETBUFF(NAT_W, 0, 8);
        acmd((uint32_t)A_LOADBUFF << 24, PhysicalAddr(nat_magic));
        SETBUFF(0, NAT_W, 8);
        acmd((uint32_t)A_SAVEBUFF << 24, PhysicalAddr(nat_mark));
        UC(nat_mark)[0] = 0;
        if (UC(nat_text)[0] != 0x0000000D) for (int i = 0; i < 64; i++) UC(nat_text)[i] = 0x0000000D;
        data_cache_hit_writeback(alist, (al - alist) * 4);

        *SP_STATUS = SP_WSTATUS_SET_HALT;
        volatile uint32_t *t = (volatile uint32_t*)0xA4000FC0;   // OSTask in DMEM
        t[0] = 2;                                   // M_AUDTASK
        t[1] = 0;
        t[2] = PhysicalAddr(nat_text); t[3] = sizeof(nat_text);     // boot (size must be <= 0x1000)
        t[4] = PhysicalAddr(nat_text); t[5] = sizeof(nat_text);
        t[6] = PhysicalAddr(nat_data); t[7] = sizeof(nat_data);
        t[8] = 0; t[9] = 0; t[10] = 0; t[11] = 0;
        t[12] = PhysicalAddr(alist);  t[13] = (al - alist) * 4;
        t[14] = 0; t[15] = 0;
        MEMORY_BARRIER();
        // The emulator runs the whole task inside this write (no interrupt-on-break)
        *SP_STATUS = SP_WSTATUS_CLEAR_INTR_BREAK | SP_WSTATUS_CLEAR_SSTEP |
                     SP_WSTATUS_CLEAR_BROKE | SP_WSTATUS_CLEAR_HALT;
        MEMORY_BARRIER();
        ok = UC(nat_mark)[0] == nat_magic[0] && UC(nat_mark)[1] == nat_magic[1];
        if (!nat_state)
            hle_logf("audio: %s", ok ? "mixed by the emulator (an ABI1 audio task)" : "the emulator didn't run the audio task, mixing in C");
#ifdef HLE_TEST_REQUIRE_NATIVE  // test aid: stop on the assert screen if the emulator didn't mix
        assertf(ok, "HLE test: the emulator didn't run the audio task");
#endif
        if (!nat_state || !ok) nat_state = ok ? 1 : -1;
        if (!ok) *SP_STATUS = SP_WSTATUS_SET_HALT;
    }
    if (ok) {
        for (int i = 0; i < nat_n; i++)
            memcpy(hle_rdram(nat_ch[i].state), (uint8_t*)UC(nat_shadow[nat_ch[i].ch]) + nat_ch[i].ofs, 16);
#ifdef HLE_TEST_AUDIO_AB
        static double se, sr; static int maxd, rounds, stbad, resampled;
        const int16_t *o = hle_rdram(out);
        static double sh[2];    // the error with the output a sample early / late: alignment
        for (int i = 0; i < ns * 2; i++) { int dd = o[i] - ref[i]; se += (double)dd * dd; sr += (double)ref[i] * ref[i]; if (abs(dd) > maxd) maxd = abs(dd); }
        for (int i = 2; i < ns * 2 - 2; i++) { double a = o[i] - ref[i - 2], b = o[i] - ref[i + 2]; sh[0] += a * a; sh[1] += b * b; }
        for (int i = 0; i < nat_n; i++) { if (memcmp(refst[i], hle_rdram(nat_ch[i].state), 16)) stbad++; if (!nat_ch[i].direct) resampled++; }
        if (++rounds % 200 == 0) {
            hle_logf("audio A/B: %d rounds, error %.1f dB below the signal, max diff %d, %d end states differ, %d resampled channel-rounds",
                rounds, sr > 0 && se > 0 ? 10.0 * log10(sr / se) : 999.0, maxd, stbad, resampled);
            hle_logf("audio A/B: shifted a sample: %.1f / %.1f dB", 10.0 * log10(sr / sh[0]), 10.0 * log10(sr / sh[1]));
            se = sr = sh[0] = sh[1] = 0; maxd = stbad = resampled = 0;
        }
#endif
    } else {
        // Mix them in C after all (their states weren't touched)
        for (int i = 0; i < nat_n; i++) mix_channel(nat_ch[i].args);
    }
    nat_n = 0;
    return ok;
}

void ovl_mixer(uint32_t cmd, const uint32_t *a, int n)
{
    switch (cmd) {
    case 0: {   // MIX_CHANNEL
        uint32_t flags = (a[0] >> 11) & 0xFF;
        int ns = (a[0] & 0x1FF) * 2;
        if (flags & CH_CLEAR) nat_begin(ns);
        if (a[6] && ns && nat_channel(a, flags, ns)) {
            if (flags & CH_CLEAR) memset(accum, 0, ns * 2 * sizeof(int32_t));
            return;
        }
        mix_channel(a);
        if (a[6] && ns) c_used = true;
        return;
    }
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
        if (!nat_flush(ns, a[1]))
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
