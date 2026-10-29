/**
 * C port of Tiny3D's RSP microcode (tiny3d/src/t3d/rsp/rsp_tiny3d.rspl).
 * Instead of transforming vertices and rasterizing triangles, it forwards the
 * untransformed geometry as F3DEX2 G_VTX/G_TRI with the same matrices, so the
 * emulator's graphics plugin does T&L on the host GPU.
 *
 * This runs on the emulated N64 CPU, in place of the RSP, so the hot paths
 * (triangles, vertex loads, matrices) are kept as cheap as possible.
 */
#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmodel.h>
#include "rsp/rsp_tiny3d.h"
#include "rspq_internal.h"
#include "hle.h"
#include "gbi.h"

#define D(sym)      (RSP_T3D_##sym & 0xFFF)
#define VTX_SIZE    36
#define Q           ((rsp_queue_t*)__vrsp_dmem)

static hle_mat4_t mat_proj, mat_mv;
static bool state_dirty = true;     // geometry mode / texture need recomputing

static inline uint32_t resolve_segment(uint32_t addr)
{
    return (addr + VDMEM32(D(SEGMENT_TABLE) + ((addr >> 24) & 0x1C))) & 0xFFFFFF;
}

// DMEM vertex address -> index; 36 does not divide cheaply, (x*1821)>>16 is exact here
__attribute__((always_inline)) static inline int vtx_index(uint32_t dmem)
{
    return ((((dmem & 0xFFF) - D(VERT_BUFFER)) & 0xFFF) * 1821) >> 16;
}

// ---- Matrices (inc/matrixStack.rspl) ----
//
// The ucode keeps a matrix stack where each slot is either loaded or multiplied
// by the slot below. That maps onto the F3DEX2 modelview stack: "multiply" is
// G_MTX(MUL|PUSH) from the slot below, "load" is G_MTX(LOAD), pops are POPMTX.
// So the plugin does the products; the CPU only keeps, per slot, the clip-space
// Z row (proj * modelview) needed for fog, plus how to rebuild the GBI stack
// after the plugin lost it (new task, 2D draws).

#define STACK_SIZE  16              // = dram_stack_size / 64 in vrdp.c

typedef struct { uint32_t addr; bool mul; } slot_t;
static slot_t slots[STACK_SIZE];
// Integer (16.16) so the emulated CPU avoids FPU conversions; values stay far below 2^47
static int64_t zrow[STACK_SIZE][4]; // clip z = sum_c zrow[k][c] * v[c]
static int64_t proj_z[4];           // z row of the projection
static int zvalid = -1;             // zrow[0..zvalid] are up to date (computed lazily, for fog)
static int cur_slot;                // ucode stack slot of the current matrix
static int gdepth;                  // GBI modelview stack depth (== cur_slot when in sync)
static uint32_t gbi_epoch = ~0u;    // gsp_epoch the GBI stack was built in
static uint32_t stack_base = 0xFFFFFFFF;

void hle_mat_from_fp(float out[4][4], const void *fp_)
{
    const int16_t *p = fp_;
    for (int c = 0; c < 4; c++, p += 8) {
        for (int r = 0; r < 4; r++)
            out[c][r] = (int32_t)(((uint32_t)(uint16_t)p[r] << 16) | (uint16_t)p[4 + r]) * (1.0f / 65536.0f);
    }
}

void hle_mat_mul(float out[4][4], const float l[4][4], const float r[4][4])
{
    float res[4][4];
    for (int c = 0; c < 4; c++) {
        float r0 = r[c][0], r1 = r[c][1], r2 = r[c][2], r3 = r[c][3];
        for (int j = 0; j < 4; j++)
            res[c][j] = l[0][j]*r0 + l[1][j]*r1 + l[2][j]*r2 + l[3][j]*r3;
    }
    memcpy(out, res, sizeof(res));
}

/** T3DMat4FP in RDRAM -> GBI Mtx in scratch (integer reshuffle only). */
static uint32_t mtx_conv(uint32_t addr)
{
    const uint32_t *src = hle_rdram(addr);      // per column: 2 words of int halves, 2 of frac halves
    uint32_t *dst = gbi_alloc(sizeof(gbi_mtx_t));
    for (int c = 0; c < 4; c++) {
        dst[c*2+0] = src[c*4+0]; dst[c*2+1] = src[c*4+1];
        dst[8+c*2+0] = src[c*4+2]; dst[8+c*2+1] = src[c*4+3];
    }
    return PhysicalAddr(dst);
}

/** zrow of a slot: proj (load) or the slot below (multiply), times the matrix at addr. */
static inline int32_t fp_elem(const int16_t *col, int r)
{
    return ((uint32_t)(uint16_t)col[r] << 16) | (uint16_t)col[4 + r];
}

static void update_zrow(int k)
{
#ifdef HLE_TEST_PERF
    extern uint32_t hle_vl_zrow; hle_vl_zrow++;
#endif
    const int64_t *base = (slots[k].mul && k > 0) ? zrow[k - 1] : proj_z;
    if (!slots[k].addr) return;
    const int16_t *p = hle_rdram(slots[k].addr);
    for (int c = 0; c < 4; c++, p += 8)
        zrow[k][c] = (base[0] * fp_elem(p, 0) + base[1] * fp_elem(p, 1) +
                      base[2] * fp_elem(p, 2) + base[3] * fp_elem(p, 3)) >> 16;
}

/** Emits slot k on top of a GBI stack that holds slots 0..k-1 below the current depth. */
static void gbi_slot(int k)
{
    uint32_t m = slots[k].addr ? mtx_conv(slots[k].addr) : 0;
    if (!m) return;
    uint32_t op = (slots[k].mul && k > 0) ? G_MTX_MUL : G_MTX_LOAD;
    if (k > gdepth) {
        while (gdepth < k - 1) {            // empty slots in between: placeholders
            GBI_MTX(m, G_MTX_LOAD | G_MTX_PUSH | G_MTX_MODELVIEW);
            gdepth++;
        }
        GBI_MTX(m, op | G_MTX_PUSH | G_MTX_MODELVIEW);
        gdepth = k;
    } else {
        if (gdepth > k) {
            gbi((uint32_t)G_POPMTX << 24 | 0x380002, (gdepth - k) * 64);
            gdepth = k;
        }
        if (op == G_MTX_MUL) {              // multiply from the slot below
            gbi((uint32_t)G_POPMTX << 24 | 0x380002, 64);
            GBI_MTX(m, G_MTX_MUL | G_MTX_PUSH | G_MTX_MODELVIEW);
        } else {
            GBI_MTX(m, G_MTX_LOAD | G_MTX_NOPUSH | G_MTX_MODELVIEW);
        }
    }
}

/** Rebuilds the GBI modelview stack up to the current slot if the plugin lost it. */
static void sync_modelview(void)
{
    if (gbi_epoch == gsp_epoch) return;
    gbi((uint32_t)G_POPMTX << 24 | 0x380002, STACK_SIZE * 64);
    gdepth = 0;
    for (int k = 0; k <= cur_slot; k++) {
        if (k == 0) {
            uint32_t m = slots[0].addr ? mtx_conv(slots[0].addr) : 0;
            if (m) GBI_MTX(m, G_MTX_LOAD | G_MTX_NOPUSH | G_MTX_MODELVIEW);
        } else {
            gbi_slot(k);
        }
    }
    gbi_epoch = gsp_epoch;
}

static void matrix_stack(uint32_t a0, uint32_t a1)
{
    int16_t advance = (int16_t)(a0 >> 8);
    if (stack_base == 0xFFFFFFFF) stack_base = VDMEM32(D(MATRIX_STACK_PTR));
    int32_t ptr = (int32_t)VDMEM32(D(MATRIX_STACK_PTR)) + advance;
    VDMEM32(D(MATRIX_STACK_PTR)) = ptr;
    int slot = (ptr - (int32_t)stack_base) / (int)sizeof(T3DMat4FP);
    assertf(slot >= 0 && slot < STACK_SIZE, "HLE: t3d matrix stack out of range (%d)", slot);
    cur_slot = slot;
    if (a0 & 2) return;     // only move the stack pointer

    gbi_reserve(STACK_SIZE + 4, sizeof(gbi_mtx_t));
    bool in_sync = gbi_epoch == gsp_epoch;
    if (advance < 0) {
        // pop: the slot's matrix is already on the GBI stack
        if (in_sync && gdepth > slot) {
            gbi((uint32_t)G_POPMTX << 24 | 0x380002, (gdepth - slot) * 64);
            gdepth = slot;
        }
        return;
    }
    slots[slot].addr = resolve_segment(a1);
    slots[slot].mul = (a0 & 1) && slot > 0;
    if (zvalid >= slot) zvalid = slot - 1;
    if (in_sync)
        gbi_slot(slot);
}

/** Full modelview of the current slot (only tinypx needs it). */
static void full_modelview(float out[4][4])
{
    float m[4][4];
    for (int k = 0; k <= cur_slot; k++) {
        if (!slots[k].addr) continue;
        hle_mat_from_fp(m, hle_rdram(slots[k].addr));
        if (slots[k].mul && k > 0) hle_mat_mul(out, out, m);
        else memcpy(out, m, sizeof(m));
    }
}

// ---- Render state -> F3DEX2 state ----

static bool fog_enabled(void) { return VDMEM8(D(FOG_STORE_OFFSET)) == 0x0B; }

/**
 * Tiny3D fog is linear in clip-space Z and written into the vertex alpha, where
 * rdpq's fog blender reads it. GBI fog (G_FOG) is linear in NDC Z instead, which
 * fogs far too early, so the alpha is computed here like the ucode does:
 * alpha = offset + slope * clip_z, folded into one plane equation.
 */
static void fog_plane(int64_t k[4])
{
    int32_t slope = (int32_t)(((uint32_t)VDMEM16(D(SCREEN_SCALE) + 6) << 16) | VDMEM16(D(SCREEN_SCALE) + 14));
    int32_t offset = (int16_t)VDMEM16(D(SCREEN_OFFSET) + 6) + 32768;   // 0x80FF -> alpha 255
    for (; zvalid < cur_slot; zvalid++)
        update_zrow(zvalid + 1);
    for (int c = 0; c < 4; c++)
        k[c] = (zrow[cur_slot][c] * slope) >> 16;
    k[3] += (int64_t)offset << 16;
}

#ifndef HLE_FOG_GBI_ERR
#define HLE_FOG_GBI_ERR 0.08f   // most fog alpha (0..1) G_FOG may be off by, else CPU fog
#endif
/**
 * The same fog done by the graphics plugin (G_FOG), so vertices need no CPU work.
 * Tiny3D's alpha is affine in depth d, G_FOG's in NDC z (affine in 1/d): fit the
 * G_FOG closest over the fog range. Returns the G_MW_FOG word, 0 = keep CPU fog.
 * Refit only when the fog or projection values change.
 */
static bool fog_dirty = true;   // the projection or a DMEM word changed since the last fit
static uint32_t fog_gbi(void)
{
#ifdef HLE_FOG_CPU      // test aid: always the exact CPU fog
    return 0;
#endif
#ifdef HLE_TEST_FOG_AB  // test aid: autoplay flips between CPU and G_FOG every 120 frames
    extern bool hle_fog_ab_cpu;
    if (hle_fog_ab_cpu) return 0;
#endif
    static float key[6];
    static uint32_t word;
    if (!fog_dirty) return word;
    fog_dirty = false;
    float sl = (int32_t)(((uint32_t)VDMEM16(D(SCREEN_SCALE) + 6) << 16) | VDMEM16(D(SCREEN_SCALE) + 14)) / 65536.0f;
    float ofs = (int16_t)VDMEM16(D(SCREEN_OFFSET) + 6) + 32768;
    const float (*P)[4] = mat_proj;
    float now[6] = { sl, ofs, P[2][2], P[3][2], P[2][3], P[3][3] };
    if (!memcmp(key, now, sizeof(now))) return word;
    memcpy(key, now, sizeof(now));
    word = 0;
    if (P[0][2] != 0 || P[1][2] != 0 || P[0][3] != 0 || P[1][3] != 0) return 0;   // not a plain perspective
    // eye point at depth d: clip z = az*d + bz, clip w = aw*d + bw; Tiny3D alpha = sl*clip_z + ofs
    float az = -P[2][2], bz = P[3][2], aw = -P[2][3], bw = P[3][3];
    if (sl * az == 0) return 0;
    // depths where the alpha leaves its near value (va) and reaches its far one (1 - va)
    float va = sl * az < 0;
    float dn = (255 * va - ofs - sl*bz) / (sl*az), df = (255 * (1 - va) - ofs - sl*bz) / (sl*az);
    if (!(dn > 0 && df > dn)) return 0;
    float w = df - dn, best = 2;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) {
        float d0 = dn + w * (-0.1f + 0.4f * i / 15), d1 = df + w * (-0.1f + 0.6f * j / 15);
        if (d0 <= 0 || d1 <= d0) continue;
        float u0 = (az*d0 + bz) / (aw*d0 + bw), u1 = (az*d1 + bz) / (aw*d1 + bw);
        float fm = roundf(256 * (1 - 2 * va) / (u1 - u0)), fo = roundf(256 * va - u0 * fm);
        if (!(fabsf(fm) <= 32767 && fo >= -32768 && fo <= 32767)) continue;
        float err = 0;
        for (int n = 0; n < 24 && err < best; n++) {
            float d = dn + w * (-0.25f + 1.75f * n / 23);
            if (d <= 0) continue;
            float t = (sl*(az*d + bz) + ofs) / 255, g = (fm * (az*d + bz) / (aw*d + bw) + fo) / 256;
            t = t < 0 ? 0 : t > 1 ? 1 : t; g = g < 0 ? 0 : g > 1 ? 1 : g;
            err = fmaxf(err, fabsf(t - g));
        }
        if (err < best) { best = err; word = (uint32_t)(int)fm << 16 | (uint16_t)(int)fo; }
    }
    if (best > HLE_FOG_GBI_ERR) word = 0;
#ifdef HLE_LOG_FOG
    debugf("[fog] range %.0f-%.0f: G_FOG %s (error %.3f)\n", (double)dn, (double)df, word ? "on" : "off", (double)best);
#endif
    return word;
}

static void sync_state(void)
{
    if (state_dirty) {
        uint32_t op = (VDMEM32(D(TRI_COMMAND)) >> 8) & 0xFF;
        uint32_t geom = G_CLIPPING;
        if (op & 1) geom |= G_ZBUFFER;
        if (op & 4) geom |= G_SHADE | G_SHADING_SMOOTH;
        switch (VDMEM8(D(FACE_CULLING))) {
        case 0: geom |= G_CULL_FRONT; break;
        case 1: geom |= G_CULL_BACK; break;
        }
        gsp_3d.geom = geom;
        uint32_t levels = (((uint32_t*)&Q->rdp_mode.other_modes)[0] >> 24 & 0x38) >> 3;
        gsp_3d.tex = (op & 2) ? ((levels << 11) | (1 << 1)) : 0;
        state_dirty = false;
    }
    gsp_sync_3d();
    sync_modelview();
}

/**
 * Reserves room for `ncmds` commands / `nbytes` of scratch plus a full state
 * re-emit, then syncs the state: a task split can only happen here, before it.
 */
static void t3d_begin(int ncmds, int nbytes)
{
    gbi_reserve(ncmds + 3 * STACK_SIZE + 16, nbytes + (STACK_SIZE + 3) * (int)sizeof(gbi_mtx_t) + 32);
    sync_state();
}

static void screen_size(const uint32_t *a)
{
    int guard = a[0] & 0xF; if (guard < 1) guard = 1;
    float norm_w = (a[3] & 0xFFFF) / 65535.0f;
    if (norm_w <= 0) norm_w = 1;
    float sx = ((int16_t)(a[2] >> 16) + (a[4] >> 16) / 65536.0f) / (norm_w * 4 * guard);
    float sy = -((int16_t)(a[2] & 0xFFFF) + (a[4] & 0xFFFF) / 65536.0f) / (norm_w * 4 * guard);
    int cx2 = (a[1] >> 17) & 0x7FFF;    // offset*2 + size == 2 * center
    int cy2 = (a[1] >> 1) & 0xFFFF;
    gsp_3d.vp[0] = (int16_t)(sx * 2); gsp_3d.vp[1] = (int16_t)(sy * 2);
    gsp_3d.vp[4] = cx2 * 2;           gsp_3d.vp[5] = cy2 * 2;
    gsp_3d.vp_ver++;
}

static void apply_depth_offset(uint32_t z)
{
    // SCREEN_OFFSET.z is 0x3FFF + offset (Tiny3D depth units, 0..0x7FFF)
    gsp_3d.vp[2] = 0x1FF;
    gsp_3d.vp[6] = 0x1FF + ((int)z - 0x3FFF) / 32;
    gsp_3d.vp_ver++;
}

// ---- Geometry ----

// T3DVertPacked (2 vertices): posA normA | posB normB | rgbaA | rgbaB | stA | stB
// gbi_vtx_t: ob[0..1] | ob[2] flag | tc | cn
// One copy loop per combination of UV offset, ambient and fog (always inlined with constants),
// so the per-vertex work has no branches on them; positions are read as int16 (no sign-extending)
__attribute__((always_inline)) static inline void vert_copy(uint32_t *o, const uint32_t *in, int count, bool uv, bool scale, bool fog,
                                                             int16_t uv_s, int16_t uv_t, const uint8_t (*amb_tbl)[256], const int64_t *k)
{
    for (int i = 0; i < count; i += 2, in += 8, o += 8) {
        o[0] = in[0]; o[1] = in[1] & 0xFFFF0000; o[2] = in[6]; o[3] = in[4];
        o[4] = in[2]; o[5] = in[3] & 0xFFFF0000; o[6] = in[7]; o[7] = in[5];
        if (uv) {
            o[2] = ((uint32_t)(uint16_t)((o[2] >> 16) + uv_s) << 16) | (uint16_t)(o[2] + uv_t);
            o[6] = ((uint32_t)(uint16_t)((o[6] >> 16) + uv_s) << 16) | (uint16_t)(o[6] + uv_t);
        }
        if (scale) {
            for (int j = 3; j <= 7; j += 4) {
                uint8_t *cn = (uint8_t*)&o[j];
                cn[0] = amb_tbl[0][cn[0]]; cn[1] = amb_tbl[1][cn[1]];
                cn[2] = amb_tbl[2][cn[2]]; cn[3] = amb_tbl[3][cn[3]];
            }
        }
        if (fog) {
            const int16_t *p = (const int16_t*)in;      // x y z of A at 0 1 2, of B at 4 5 6
            for (int j = 0; j < 2; j++, p += 4) {
                int64_t f = k[0] * p[0] + k[1] * p[1] + k[2] * p[2] + k[3];
                int32_t a = f >> 16;
                o[j*4+3] = (o[j*4+3] & ~0xFFu) | (a <= 0 ? 0 : a >= 255 ? 255 : a);
            }
        }
    }
}

// ---- Converted-vertex cache ----
//
// Model vertices (t3d_model_load) never change, so without CPU fog their G_VTX copy
// only depends on the UV offset and ambient: it is made once and reused. Buffers
// rewritten every frame (cloth, trails, chains) live outside models and are copied
// each time. The pool is only reused between tasks: the plugin reads a task's
// vertices when it is submitted.

#define VC_BYTES    (96 * 1024)
#define VC_SLOTS    1024                // power of 2
#define VC_MODELS   64

typedef struct { uint32_t src, uv, amb, out; uint16_t count; } vc_ent_t;
static vc_ent_t vc_tab[VC_SLOTS];
static uint8_t *vc_pool;
static uint32_t vc_used, vc_task;
static bool vc_reset_pending;
static struct { uint32_t lo, hi; } vc_models[VC_MODELS];
static int vc_nmodels;
#ifdef HLE_TEST_PERF
uint32_t hle_vc_hits, hle_vc_misses, hle_vc_resets, hle_vc_peak;
#endif

static void vc_reset_later(void)
{
    memset(vc_tab, 0, sizeof(vc_tab));
    vc_reset_pending = true;
    vc_task = gsp_tasks;
}

static void hle_vcache_model(const void *verts, uint32_t bytes, bool add)
{
    uint32_t lo = PhysicalAddr(verts), hi = lo + bytes;
    for (int i = 0; i < vc_nmodels; i++) {
        if (vc_models[i].lo != lo) continue;
        if (!add) { vc_models[i] = vc_models[--vc_nmodels]; vc_reset_later(); }
        return;
    }
    if (add && vc_nmodels < VC_MODELS) vc_models[vc_nmodels++] = (typeof(vc_models[0])){ lo, hi };
}

/** The entry for this copy (out != 0 when cached), a free one to fill (out == 0), or NULL. */
static vc_ent_t *vc_find(uint32_t src, int count, uint32_t uv, uint32_t amb)
{
    if (vc_reset_pending && vc_task != gsp_tasks) { vc_used = 0; vc_reset_pending = false; }
    uint32_t h = ((src >> 4) * 2654435761u) >> 22;
    for (int n = 0; n < 8; n++, h = (h + 1) & (VC_SLOTS - 1)) {
        vc_ent_t *e = &vc_tab[h];
        if (!e->out) {
            if (vc_reset_pending) return NULL;      // pool full until the next task
            int i = 0;
            while (i < vc_nmodels && (src < vc_models[i].lo || src + count * 16 > vc_models[i].hi)) i++;
            if (i == vc_nmodels) return NULL;
            e->src = src; e->uv = uv; e->amb = amb; e->count = count;
            return e;
        }
        if (e->src == src && e->uv == uv && e->amb == amb && e->count == count) return e;
    }
    return NULL;
}

static uint32_t *vc_alloc(int bytes)
{
    if (!vc_pool) vc_pool = malloc_uncached(VC_BYTES);
    if (!vc_pool || vc_used + bytes > VC_BYTES) {
#ifdef HLE_TEST_PERF
        hle_vc_resets++;
#endif
        vc_reset_later();
        return NULL;
    }
    uint32_t *p = (uint32_t*)(vc_pool + vc_used);
    vc_used += bytes;
#ifdef HLE_TEST_PERF
    if (vc_used > hle_vc_peak) hle_vc_peak = vc_used;
#endif
    return p;
}

#ifdef HLE_TEST_PERF
uint32_t hle_vl_ticks[6], hle_vl_verts, hle_vl_amb, hle_vl_zrow, hle_vl_fog;     // test aid: vert_load's setup, t3d_begin, the loop, the G_VTX
#define VL_MARK(i) do { uint32_t _n = TICKS_READ(); hle_vl_ticks[i] += _n - _vl; _vl = _n; } while (0)
#else
#define VL_MARK(i) do {} while (0)
#endif
static void vert_load(const uint32_t *a)
{
#ifdef HLE_TEST_PERF
    uint32_t _vl = TICKS_READ();
#endif
    int count = (a[0] & 0xFFFF) / 16;   // always an even number of vertices
    int dest = vtx_index(a[2] & 0xFFFF);
    if (dest + count > T3D_VERTEX_CACHE_SIZE) count = T3D_VERTEX_CACHE_SIZE - dest;
    if (count <= 0) return;
    const T3DVertPacked *src = hle_rdram(resolve_segment(a[1]));

    uint32_t fx = VDMEM16(D(VERTEX_FX_FUNC));
    if (fx != (RSP_T3D_CODE_LOOP_TRAIL & 0xFFF))
        hle_warn_once("t3dfx", "Tiny3D vertex FX %04lx not supported", fx);
    uint32_t uv_ofs = VDMEM32(D(UV_GEN_PARAMS));
    int16_t uv_s = uv_ofs >> 16, uv_t = uv_ofs;

    bool unlit = VDMEM8(D(TRI_COMMAND)) & 1;
    uint32_t amb = VDMEM32(D(COLOR_AMBIENT));
    bool scale = !unlit && amb != 0xFFFFFFFF;
    if (!unlit && VDMEM8(D(ACTIVE_LIGHT_SIZE)))
        hle_warn_once("t3dlights", "Tiny3D directional/point lights are ignored (ambient only)");

    VL_MARK(4);
    bool fog = fog_enabled();
    gsp_3d.fog = fog ? fog_gbi() : 0;
    if (gsp_3d.fog) fog = false;
    int64_t k[4] = {0};
    if (fog) fog_plane(k);
    VL_MARK(5);
#ifdef HLE_TEST_PERF
    hle_vl_fog += fog;
#endif
    static uint8_t amb_tbl[4][256];     // c * ambient / 255 per channel
    static uint32_t amb_tbl_for = 0xFFFFFFFF;
    if (scale && amb != amb_tbl_for) {
        for (int ch = 0; ch < 4; ch++)
            for (int v = 0; v < 256; v++)
                amb_tbl[ch][v] = v * ((amb >> (24 - 8*ch)) & 0xFF) / 255;
        amb_tbl_for = amb;
#ifdef HLE_TEST_PERF
        hle_vl_amb++;
#endif
    }
    VL_MARK(0);
#ifdef HLE_NO_VCACHE    // test aid: copy every load
    vc_ent_t *e = NULL;
#else
    vc_ent_t *e = fog ? NULL : vc_find(PhysicalAddr(src), count, uv_ofs, scale ? amb : 0xFFFFFFFF);
#endif
    if (e && e->out) {
        t3d_begin(2, 0);
        GBI_VTX(dest, count, e->out);
#ifdef HLE_TEST_PERF
        hle_vc_hits++;
        hle_vl_verts += count;
#endif
        return;
    }
    t3d_begin(2, count * sizeof(gbi_vtx_t));
    VL_MARK(1);
    uint32_t *out = e ? vc_alloc(count * sizeof(gbi_vtx_t)) : NULL;
    if (out) {
        e->out = PhysicalAddr(out);
#ifdef HLE_TEST_PERF
        hle_vc_misses++;
#endif
    } else {
        if (e) e->src = 0, e->count = 0;    // the slot stays free (out == 0)
        out = gbi_alloc(count * sizeof(gbi_vtx_t));
    }
    const uint32_t *in = (const uint32_t*)src;
    switch ((uv_ofs != 0) | scale << 1 | fog << 2) {
    case 0: vert_copy(out, in, count, false, false, false, uv_s, uv_t, amb_tbl, k); break;
    case 1: vert_copy(out, in, count, true,  false, false, uv_s, uv_t, amb_tbl, k); break;
    case 2: vert_copy(out, in, count, false, true,  false, uv_s, uv_t, amb_tbl, k); break;
    case 3: vert_copy(out, in, count, true,  true,  false, uv_s, uv_t, amb_tbl, k); break;
    case 4: vert_copy(out, in, count, false, false, true,  uv_s, uv_t, amb_tbl, k); break;
    case 5: vert_copy(out, in, count, true,  false, true,  uv_s, uv_t, amb_tbl, k); break;
    case 6: vert_copy(out, in, count, false, true,  true,  uv_s, uv_t, amb_tbl, k); break;
    default: vert_copy(out, in, count, true, true,  true,  uv_s, uv_t, amb_tbl, k); break;
    }
#ifdef HLE_TEST_T3D_EXACT   // test aid: the loop as it was, on the same input: every word must match
    {
        extern uint32_t hle_exact_calls, hle_exact_bad;
        static uint32_t ref[T3D_VERTEX_CACHE_SIZE * 4];
        const uint32_t *in = (const uint32_t*)src; uint32_t *o = ref;
        for (int i = 0; i < count; i += 2, in += 8, o += 8) {
            o[0] = in[0]; o[1] = in[1] & 0xFFFF0000; o[2] = in[6]; o[3] = in[4];
            o[4] = in[2]; o[5] = in[3] & 0xFFFF0000; o[6] = in[7]; o[7] = in[5];
            if (uv_ofs) {
                o[2] = ((uint32_t)(uint16_t)((o[2] >> 16) + uv_s) << 16) | (uint16_t)(o[2] + uv_t);
                o[6] = ((uint32_t)(uint16_t)((o[6] >> 16) + uv_s) << 16) | (uint16_t)(o[6] + uv_t);
            }
            if (scale) for (int j = 3; j <= 7; j += 4) {
                uint8_t *cn = (uint8_t*)&o[j];
                cn[0] = amb_tbl[0][cn[0]]; cn[1] = amb_tbl[1][cn[1]]; cn[2] = amb_tbl[2][cn[2]]; cn[3] = amb_tbl[3][cn[3]];
            }
            if (fog) for (int j = 0; j < 8; j += 4) {
                int64_t f = k[0] * (int16_t)(o[j] >> 16) + k[1] * (int16_t)o[j] + k[2] * (int16_t)(o[j+1] >> 16) + k[3];
                int32_t a = f >> 16;
                o[j+3] = (o[j+3] & ~0xFFu) | (a <= 0 ? 0 : a >= 255 ? 255 : a);
            }
        }
        hle_exact_calls++;
        if (memcmp(ref, out, count * sizeof(gbi_vtx_t))) hle_exact_bad++;
    }
#endif
    VL_MARK(2);
    GBI_VTX(dest, count, PhysicalAddr(out));
    VL_MARK(3);
#ifdef HLE_TEST_PERF
    hle_vl_verts += count;
#endif
}

// Triangles are paired into G_TRI2, also across commands: hle_t3d_flush() is
// called before anything else reaches the display list.
#ifdef HLE_TEST_T3D_EXACT
uint32_t hle_exact_calls, hle_exact_strips, hle_exact_bad;
#endif
static uint32_t tri_pending;
static bool tri_has_pending;

void hle_t3d_flush(void)
{
    if (!tri_has_pending) return;
    tri_has_pending = false;
    gbi((uint32_t)G_TRI1 << 24 | tri_pending, 0);
}

/** Triangle in RDPQ_Triangle vertex order; `flip` = culling flag inverted by the caller. */
static inline void tri_idx(int a, int b, int c, bool flip)
{
    uint32_t idx = flip ? GBI_TRI_IDX(a, c, b) : GBI_TRI_IDX(a, b, c);
    if (tri_has_pending) {
        tri_has_pending = false;
        gbi((uint32_t)G_TRI2 << 24 | tri_pending, idx);
    } else {
        tri_pending = idx;
        tri_has_pending = true;
    }
}

static inline void tri(uint32_t d0, uint32_t d1, uint32_t d2, bool flip)
{
    tri_idx(vtx_index(d0), vtx_index(d1), vtx_index(d2), flip);
}

static void tri_strip(uint32_t a0, uint32_t a1)
{
    int count = ((a1 & 0xFFFF) + 1) / 2;
    const uint16_t *e = hle_rdram(a0 & 0xFFFFFF);
    t3d_begin(count / 2 + 2, 0);
    // Literal port of the STRIP_LOOP: strips alternate winding, and an index
    // with the MSB set restarts a new strip.
#ifdef HLE_TEST_T3D_EXACT   // test aid: the strip walked as it was: the same triangles, in order
    static uint32_t ref[4096]; int nref = 0;
    {
        int end = count - 2, p = 0; bool flip = true;
        while (p < end && nref < 4096) {
            int16_t i1 = (int16_t)e[p + 2];
            if (i1 < 0) { p += 2; flip = true; continue; }
            int a = vtx_index(e[p + 1]), b = vtx_index(i1), c = vtx_index(e[p]);
            ref[nref++] = flip ? GBI_TRI_IDX(a, c, b) : GBI_TRI_IDX(a, b, c);
            p += 1; flip = !flip;
        }
    }
    uint64_t *dl0 = __gbi_ptr; bool pend0 = tri_has_pending; uint32_t pidx0 = tri_pending;
#endif
    // Each index is converted once: a triangle shares two with the one before
    int end = count - 2;
    int p = 0;
    bool flip = true;
    int v0 = vtx_index(e[0]), v1 = vtx_index(e[1]);
    while (p < end) {
        int16_t i1 = (int16_t)e[p + 2];
        if (i1 < 0) {
            p += 2; flip = true;
            if (p < end) { v0 = vtx_index(e[p]); v1 = vtx_index(e[p + 1]); }
            continue;
        }
        int v2 = vtx_index(i1);
        tri_idx(v1, v2, v0, flip);      // = tri(e[p + 1], i1, e[p], flip)
        v0 = v1; v1 = v2;
        p += 1;
        flip = !flip;
    }
#ifdef HLE_TEST_T3D_EXACT
    {   // what went out: pairs of G_TRI2 (plus the one pending before and after)
        extern uint32_t hle_exact_strips, hle_exact_bad;
        static uint32_t got[4100]; int ngot = 0;
        if (pend0) got[ngot++] = pidx0;
        for (uint64_t *d = dl0; d < __gbi_ptr && ngot < 4098; d++) { got[ngot++] = (uint32_t)(*d >> 32) & 0xFFFFFF; got[ngot++] = (uint32_t)*d; }
        if (tri_has_pending) got[ngot++] = tri_pending;
        int off = pend0 ? 1 : 0;
        hle_exact_strips++;
        if (nref < 4096 && (ngot - off != nref || memcmp(got + off, ref, nref * 4))) hle_exact_bad++;
    }
#endif
}

static void tri_seq(uint32_t a0, uint32_t a1)
{
    uint32_t base = a0 & 0xFFFF, end = a1 & 0xFFFF;
    uint32_t is_quad = a1 >> 16;
    uint32_t incr = is_quad + VTX_SIZE * 3;
    t3d_begin(((end - base) / VTX_SIZE) + 4, 0);
    for (uint32_t b = base; b != end; b += incr)
        tri(b + VTX_SIZE * 2, b + VTX_SIZE, b, false);
    if (is_quad) {
        for (uint32_t b = base + VTX_SIZE; b != end + VTX_SIZE; b += incr)
            tri(b + VTX_SIZE * 2, b + VTX_SIZE, b, true);
    }
}

void hle_t3d_matrices(const float (**proj)[4], const float (**mv)[4])
{
    full_modelview(mat_mv);
    *proj = mat_proj;
    *mv = mat_mv;
}

int hle_t3d_tri_run(const uint32_t *a, uint32_t cmd_byte)
{
    // the ucode feeds RDPQ_Triangle with (v0, v2, v1)
    const uint32_t *p = a;
    do {
        t3d_begin(64, 0);
        for (int i = 0; i < 64; i++) {
            tri(p[0] & 0xFFFF, p[1] & 0xFFFF, p[1] >> 16, false);
            p += 2;
            if ((p[0] >> 24) != cmd_byte) break;
        }
    } while ((p[0] >> 24) == cmd_byte);
    return (p - a) * 4;
}

void ovl_t3d(uint32_t cmd, const uint32_t *a, int n)
{
    hle_t3d_flush();

    switch (cmd) {
    case T3D_CMD_SCREEN_SIZE:
        screen_size(a);
        VDMEM16(D(NORM_SCALE_W) + 6) = a[3] & 0xFFFF;
        return;
    case T3D_CMD_MATRIX_STACK:
        matrix_stack(a[0], a[1]);
        return;
    case T3D_CMD_SET_WORD: {
        uint32_t addr = a[0] & 0xFFFF;
        if (addr & 0x8000) VDMEM16(addr) = a[1];
        else VDMEM32(addr) = a[1];
        fog_dirty = true;
        if ((addr & 0xFFE) == D(SCREEN_OFFSET) + 4)
            apply_depth_offset(VDMEM16(D(SCREEN_OFFSET) + 4));
        return;
    }
    case T3D_CMD_VERT_LOAD:
        vert_load(a);
        return;
    case T3D_CMD_LIGHT_SET: {
        uint32_t addr = a[0] & 0xFFF;
        VDMEM32(addr) = a[1];
        if (a[2]) { VDMEM32(addr + 8) = a[2]; VDMEM32(addr + 12) = a[3]; }
        VDMEM32(addr + 4) = a[1];
        return;
    }
    case T3D_CMD_DRAWFLAGS: {
        VDMEM8(D(FACE_CULLING)) = a[0] & 0xFF;
        uint32_t mip = (((uint32_t*)&Q->rdp_mode.other_modes)[0] >> 24) & 0x38;
        VDMEM32(D(TRI_COMMAND)) = a[1] | mip;
        state_dirty = true;
        return;
    }
    case T3D_CMD_PROJ_SET:
        hle_mat_from_fp(mat_proj, hle_rdram(a[0] & 0xFFFFFF));
        gsp_3d_proj(&mat_proj);
        for (int m = 0; m < 4; m++)
            proj_z[m] = fp_elem((const int16_t*)hle_rdram(a[0] & 0xFFFFFF) + m * 8, 2);
        zvalid = -1;                            // fog rows depend on the projection
        fog_dirty = true;
        return;
    case T3D_CMD_FOG_STATE:
        VDMEM8(D(FOG_STORE_OFFSET)) = a[0] & 0xFF;
        return;
    case T3D_CMD_TRI_SYNC:
        return;
    case T3D_CMD_TRI_STRIP:
        tri_strip(a[0], a[1]);
        return;
    case T3D_CMD_TRI_SEQ:
        tri_seq(a[0], a[1]);
        return;
    default:    // IMEM patches (lighting mode / cull invert), metrics
        hle_warn_once("t3dcmd", "Tiny3D command %ld ignored", cmd);
        return;
    }
}

// Models register their vertex buffer with the cache (linked with --wrap)
T3DModel *__real_t3d_model_load(const char *path);
void __real_t3d_model_free(T3DModel *model);
T3DModel *__wrap_t3d_model_load(const char *path)
{
    T3DModel *m = __real_t3d_model_load(path);
    hle_vcache_model(t3d_model_get_vertices(m), m->totalVertCount * 16, true);
    return m;
}
void __wrap_t3d_model_free(T3DModel *model)
{
    hle_vcache_model(t3d_model_get_vertices(model), model->totalVertCount * 16, false);
    __real_t3d_model_free(model);
}
