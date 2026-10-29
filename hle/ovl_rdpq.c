/**
 * C port of libdragon's rdpq RSP overlay (src/rdpq/rsp_rdpq.S + include/rsp_rdpq.inc).
 * Command numbering and semantics follow the microcode exactly; the generated RDP
 * commands are handed to the virtual RDP. Triangles assembled by the RSP become
 * screen-space F3DEX2 geometry instead of raw RDP triangles.
 */
#include <libdragon.h>
#include "rdpq_constants.h"
#include "rspq_internal.h"
#include "rdpq/rdpq_internal.h"
#include "interrupt_internal.h"
#include "hle.h"
#include "gbi.h"

#define Q       ((rsp_queue_t*)__vrsp_dmem)
#define MODE    (Q->rdp_mode)
#define SOM_HI  (((uint32_t*)&MODE.other_modes)[0])
#define SOM_LO  (((uint32_t*)&MODE.other_modes)[1])

// Saved-state fields that only this overlay uses live in C statics, in one struct so a
// block replay (replay.c) saves and compares them in one piece.
static struct {
    uint32_t address_table[RDPQ_ADDRESS_TABLE_SIZE];
    uint16_t autotmem_addr, autotmem_prev, autotmem_limit;
    uint8_t autotmem_enabled;
    uint32_t prim_color_ex, prim_color_rgba;
    rspq_rdp_mode_t mode_stack[3];
    int staged;
} rs;
// (not in it: every triangle sends its data first)
static struct { uint32_t xy, z, rgba, st, w, inv_w; } tri_data[3];
#define address_table    rs.address_table
#define autotmem_addr    rs.autotmem_addr
#define autotmem_prev    rs.autotmem_prev
#define autotmem_limit   rs.autotmem_limit
#define autotmem_enabled rs.autotmem_enabled
#define prim_color_ex    rs.prim_color_ex
#define prim_color_rgba  rs.prim_color_rgba
#define mode_stack       rs.mode_stack
#define staged           rs.staged

// RDPQ_CMD_STAGING + RDPQ_Write8/16 + RDPQ_Finalize
static uint32_t staging[44];

static void write8(uint32_t a0, uint32_t a1) { staging[staged++] = a0; staging[staged++] = a1; }
static void write16(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    write8(a0, a1); write8(a2, a3);
}
static void finalize(void) {
#ifdef HLE_TEST_PERF
    extern uint32_t hle_fin_ticks; uint32_t t = TICKS_READ();
#endif
    vrsp_rdpq_send(staging, staged * 4);
    staged = 0;
#ifdef HLE_TEST_PERF
    hle_fin_ticks += TICKS_READ() - t;
#endif
}

static bool fillcopy(void) { return SOM_HI & (1u << 21); }

static void write_scissor(uint32_t a0, uint32_t a1)
{
    ((uint32_t*)&Q->rdp_scissor_rect)[0] = a0;
    ((uint32_t*)&Q->rdp_scissor_rect)[1] = a1;
    if (fillcopy()) a1 -= 1 << 12;
    write8(a0, a1);
}

static void write_fill_color(uint32_t color)
{
    Q->rdp_fill_color = color;
    if ((Q->rdp_target_bitdepth & 3) == 3) {
        write8(0xF7000000, color);
        return;
    }
    uint32_t c16 = ((color >> (24 + 3 - 11)) & (0x1F << 11)) |
                   ((color >> (16 + 3 - 6))  & (0x1F << 6)) |
                   ((color >> (8 + 3 - 1))   & (0x1F << 1)) |
                   ((color >> 7) & 1);
    write8(0xF7000000, c16 | (c16 << 16));
}

static uint32_t fixup_address(uint32_t a1)
{
    return (a1 + address_table[(a1 >> 24) >> 2]) & 0x00FFFFFF;
}

static void write_other_modes(uint32_t hi, uint32_t lo)
{
    write8((hi & 0x00FFFFFF) | 0xEF000000, lo);
    write_scissor(((uint32_t*)&Q->rdp_scissor_rect)[0], ((uint32_t*)&Q->rdp_scissor_rect)[1]);
    finalize();
}

/** RDPQ_UpdateRenderMode: derive the final SOM + combiner from the mode state. */
static void update_render_mode(void)
{
    uint32_t som_hi = SOM_HI, som_lo = SOM_LO;
    if (som_hi & (SOMX_UPDATE_FREEZE >> 32)) { finalize(); return; }

    if (som_hi & (1u << 21)) {      // fill/copy: just the SOM
        write8((som_hi & 0x00FFFFFF) | 0xEF000000, som_lo);
        finalize();
        return;
    }

    // The same inputs give the same mode: models switch between a few, so keep the last ones
    typedef struct { uint64_t comb, mask; uint32_t hi, lo, t0, t1, out[4]; bool valid; } rm_memo_t;
    static rm_memo_t memo[64];
    uint32_t h = som_hi * 31 + som_lo;
    h = h * 31 + (uint32_t)MODE.combiner; h = h * 31 + (uint32_t)(MODE.combiner >> 32);
    h = h * 31 + MODE.blend_step0; h = h * 31 + MODE.blend_step1;
    rm_memo_t *m = &memo[(h * 2654435761u) >> 26];
    if (m->valid && m->hi == som_hi && m->lo == som_lo && m->comb == MODE.combiner && m->mask == MODE.combiner_mipmapmask &&
            m->t0 == MODE.blend_step0 && m->t1 == MODE.blend_step1) {
#ifdef HLE_TEST_PERF
        { extern uint32_t hle_rm_calls[4]; hle_rm_calls[2]++; }
#endif
        SOM_HI = (m->out[2] & 0x00FFFFFF) | (som_hi & 0xFF000000);
        SOM_LO = m->out[3];
        write16(m->out[0], m->out[1], m->out[2], m->out[3]);
        finalize();
        return;
    }
    *m = (rm_memo_t){ .comb = MODE.combiner, .mask = MODE.combiner_mipmapmask, .hi = som_hi, .lo = som_lo,
                      .t0 = MODE.blend_step0, .t1 = MODE.blend_step1 };

    uint64_t comb = MODE.combiner, comb1 = 0, comb2;
    if ((int64_t)comb < 0) {
        assertf(!(som_hi & (SOMX_LOD_INTERPOLATE >> 32)), "Interpolated mipmap cannot work with a custom 2-pass combiner");
        comb2 = comb;
    } else {
        if (som_hi & (SOMX_FOG >> 32)) {
            // SHADE alpha carries fog: swap the standard combiners for fog-aware ones
            uint64_t noid = comb & 0x00FFFFFFFFFFFFFFull;
            if (noid == RDPQ_COMBINER_TEX_SHADE)
                comb = RDPQ_COMBINER1((TEX0,0,SHADE,0), (0,0,0,TEX0));
            else if (noid == RDPQ_COMBINER_SHADE)
                comb = RDPQ_COMBINER1((0,0,0,SHADE), (0,0,0,1));
        }
        uint32_t interp = (som_hi & (SOMX_LOD_INTERP_MASK >> 32)) >> (SOMX_LOD_INTERP_SHIFT - 32);
        if (interp) {
            static const uint64_t mipmaps[2] = {
                (RDPQ_COMBINER2((TEX1, TEX0, LOD_FRAC, TEX0), (TEX1, TEX0, LOD_FRAC, TEX0), (0,0,0,0), (0,0,0,0)) & RDPQ_COMB0_MASK) | RDPQ_COMBINER_2PASS,
                (RDPQ_COMBINER2((TEX0, TEX1, K5, 0), (0, 0, 0, TEX1), (0,0,0,0), (0,0,0,0)) & RDPQ_COMB0_MASK) | RDPQ_COMBINER_2PASS,
            };
            comb2 = (comb & MODE.combiner_mipmapmask) | mipmaps[interp - 1];
        } else {
            comb1 = comb;
            comb2 = comb & RDPQ_COMB0_MASK;
            // nearest mipmapping needs 2-cycle mode for the RDP to switch LOD
            comb2 |= (uint64_t)((som_hi >> (SOM_TEXTURE_LOD_SHIFT - 32)) & 1) << 63;
        }
    }

    // Blender steps (step0 = fog, step1 = blending)
    uint32_t t0 = MODE.blend_step0, t1 = MODE.blend_step1;
    bool bkg_blending = t1 != 0;
    if (!t1 && (som_lo & SOM_AA_ENABLE)) {
        t1 = (som_hi & (SOMX_AA_REDUCED >> 32))
            ? (RDPQ_BLENDER((IN_RGB, IN_ALPHA, MEMORY_RGB, MEMORY_CVG)) & ~SOM_READ_ENABLE)
            : RDPQ_BLENDER((IN_RGB, IN_ALPHA, MEMORY_RGB, MEMORY_CVG));
        t0 &= ~SOM_BLENDING;
    }
    uint32_t blend_1cyc, blend_2cyc;
    if (t0 && t1) {
        blend_1cyc = t0;
        blend_2cyc = (t0 & SOM_BLEND0_MASK) | (t1 & SOM_BLEND1_MASK) | SOMX_BLEND_2PASS;
    } else {
        blend_1cyc = t0 ? t0 : t1;
        blend_2cyc = blend_1cyc & SOM_BLEND1_MASK;
        blend_1cyc &= SOM_BLEND0_MASK;
    }

    // YUV bilinear filtering
    if ((som_hi & ((SOM_TF_MASK | SOM_SAMPLE_MASK) >> 32)) == ((SOM_SAMPLE_BILINEAR | SOM_TF0_YUV | SOM_TF1_YUV) >> 32))
        som_hi |= (SOM_TF1_YUVTEX0 | SOM_TF0_RGB) >> 32;

    // 1-cycle / 2-cycle selection
    uint64_t final_comb;
    uint32_t blend_final, cycle;
    if ((uint32_t)(comb2 >> 32) & 0x80000000 || (blend_2cyc & SOMX_BLEND_2PASS)) {
        final_comb = comb2; blend_final = blend_2cyc; cycle = SOM_CYCLE_2 >> 32;
    } else {
        final_comb = comb1; blend_final = blend_1cyc; cycle = SOM_CYCLE_1 >> 32;
    }
    bool alphacompare_x = som_hi & (SOMX_ALPHACOMPARE >> 32);
    uint8_t ext_flags = som_hi >> 24;
    som_hi = (som_hi & ~(SOM_CYCLE_MASK >> 32) & 0x00FFFFFF) | cycle | 0xEF000000;

    // Antialias & coverage
    static const uint32_t aa_table[4] = {
        SOM_COVERAGE_DEST_ZAP, SOM_COVERAGE_DEST_ZAP,
        SOM_BLALPHA_CVG | SOM_COVERAGE_DEST_CLAMP,
        SOM_COLOR_ON_CVG_OVERFLOW | SOM_COVERAGE_DEST_WRAP,
    };
    const uint32_t aa_mask = SOM_COVERAGE_DEST_MASK | SOM_BLEND_MASK | SOM_BLALPHA_MASK | SOM_COLOR_ON_CVG_OVERFLOW;
    uint32_t bits = aa_table[((som_lo & SOM_AA_ENABLE) ? 2 : 0) | (bkg_blending ? 1 : 0)] | blend_final;
    som_lo = (som_lo & ~aa_mask) | (bits & aa_mask);

    // AA + alpha compare: modulate coverage by alpha and drop the compare
    if ((som_lo & (1u << (SOM_BLALPHA_SHIFT + 1))) && alphacompare_x) {
        som_lo |= SOM_BLALPHA_CVG_TIMES_CC;
        som_lo &= ~SOM_ALPHACOMPARE_MASK;
    }

    // Standard Z mode: opaque or transparent depending on blending
    if (!(som_lo & (1u << 10))) {
        if (som_lo & SOM_BLENDING) som_lo |= 2u << 10;
        else som_lo &= ~(2u << 10);
    }

    SOM_HI = (som_hi & 0x00FFFFFF) | ((uint32_t)ext_flags << 24);
    SOM_LO = som_lo;
    write16(((uint32_t)(final_comb >> 32) & 0x00FFFFFF) | 0xFC000000, (uint32_t)final_comb, som_hi, som_lo);
    m->out[0] = ((uint32_t)(final_comb >> 32) & 0x00FFFFFF) | 0xFC000000; m->out[1] = (uint32_t)final_comb;
    m->out[2] = som_hi; m->out[3] = som_lo; m->valid = true;
    finalize();
}

static void set_combine(uint32_t a0, uint32_t a1)
{
    a0 = (a0 | 0x7F000000) ^ (0x7F000000 ^ 0x7C000000);
    MODE.combiner = ((uint64_t)a0 << 32) | a1;
    update_render_mode();
}

static void set_fog_mode(uint32_t a1)
{
    MODE.blend_step0 = a1;
    update_render_mode();
}

static void sync_full(uint32_t a0, uint32_t a1)
{
    uint32_t *state = VDMEM_PTR(__vrsp_state_start);   // RDPQ_SYNCFULL is the first field
    uint32_t rdram_state = state[4];
    uint32_t rdram_syncpoint = state[5];
    state[0] = a0; state[1] = a1;
    state[2] = *(volatile uint32_t*)hle_rdram(rdram_syncpoint);
    uint32_t *out = hle_rdram(rdram_state);
    out[0] = state[0]; out[1] = state[1]; out[2] = state[2];

    Q->rdp_syncfull_ongoing = 0;
    write8(a0, a1);
    finalize();         // the virtual RDP submits the task here, which runs synchronously

    // Completion: what rdpq's DP interrupt handler (__rdpq_interrupt) would do.
    // (the hardware DP interrupt is masked, see task_submit)
    uint32_t sr = __disable_interrupts();
    __rdpq_syncpoint_at_syncfull = state[2];
    uint32_t w0 = a0 & 0x00FFFFFF;
    if (w0) {
        void (*callback)(void*) = (void (*)(void*))CachedAddr(w0 | 0x80000000);
        callback((void*)a1);
    }
    __enable_interrupts(sr);
}

/** RSP-assembled triangle (rdpq_triangle): drawn as F3DEX2 screen-space geometry. */
static void triangle(uint32_t a0)
{
    uint32_t op = (a0 >> 8) & 0x3F;
    uint32_t geom = 0, tex = 0;
    if (op & 4) geom |= G_SHADE | G_SHADING_SMOOTH;
    if (op & 1) geom |= G_ZBUFFER;
    if (op & 2) tex = (((a0 >> 3) & 7) << 11) | ((a0 & 7) << 8) | (1 << 1);
    gsp_sync_2d(geom, tex);
    gbi_reserve(2, 48);
    gbi_vtx_t *v = gbi_alloc(3 * sizeof(gbi_vtx_t));
    for (int i = 0; i < 3; i++) {
        v[i].ob[0] = (int16_t)(tri_data[i].xy >> 16);
        v[i].ob[1] = (int16_t)tri_data[i].xy;
        v[i].ob[2] = 0;
        v[i].flag = 0;
        v[i].tc[0] = (int16_t)(tri_data[i].st >> 16);
        v[i].tc[1] = (int16_t)tri_data[i].st;
        uint32_t c = tri_data[i].rgba;
        v[i].cn[0] = c >> 24; v[i].cn[1] = c >> 16; v[i].cn[2] = c >> 8; v[i].cn[3] = c;
    }
    // high slots, so Tiny3D's vertex cache (0..69) survives
    GBI_VTX(77, 3, PhysicalAddr(v));
    gbi((uint32_t)G_TRI1 << 24 | GBI_TRI_IDX(77, 78, 79), 0);
}

void ovl_rdpq(uint32_t cmd, const uint32_t *a, int n)
{
    uint32_t a0 = a[0], a1 = n > 1 ? a[1] : 0;
    switch (cmd) {
    case 0x00:                                  // NOOP
    case 0x15: case 0x17: case 0x20 ... 0x23:
        if (cmd == 0x00) { write8(a0, a1); finalize(); }
        return;
    case 0x01:                                  // SetLookupAddress
        address_table[(a0 & 0xFFF) >> 2] = a1;
        return;
    case 0x02: case 0x10: {                     // FillRectEx / TextureRectEx
        uint32_t a2 = n > 2 ? a[2] : 0, a3 = n > 3 ? a[3] : 0;
        if (fillcopy()) {
            a0 -= (4 << 12) + 4;
            a3 = (a3 & 0xFFFF) | ((a3 & 0xFFFF0000) << 2);
        }
        a0 ^= 0x34000000;
        if (cmd == 0x02) write8(a0, a1);
        else write16(a0, a1, a2, a3);
        finalize();
        return;
    }
    case 0x03:                                  // ClearZBuffer
        vrdp_clear_z(a0 & 0xFFFFFF, a1, a[3]);
        return;
    case 0x04: {                                // ResetMode
        uint32_t a2 = a[2] | (SOM_HI & (SOMX_UPDATE_FREEZE >> 32)), a3 = a[3];
        uint32_t old_hi = SOM_HI;
        memset(&MODE, 0, sizeof(MODE));
        SOM_HI = a2; SOM_LO = a3;
        if ((old_hi ^ a2) & (1u << 21))
            write_scissor(((uint32_t*)&Q->rdp_scissor_rect)[0], ((uint32_t*)&Q->rdp_scissor_rect)[1]);
        // falls into SetCombineMode_1Pass with a2/a3 (= SOM) as mipmap mask, like the ucode
        MODE.combiner_mipmapmask = ((uint64_t)a2 << 32) | a3;
        set_combine(a0 ^ 0x80000000, a1);
        return;
    }
    case 0x05:                                  // SetCombineMode_2Pass
        set_combine(a0, a1);
        return;
    case 0x06:                                  // PushMode
        mode_stack[2] = mode_stack[1];
        mode_stack[1] = mode_stack[0];
        mode_stack[0] = MODE;
        return;
    case 0x07:                                  // PopMode
        MODE = mode_stack[0];
        mode_stack[0] = mode_stack[1];
        mode_stack[1] = mode_stack[2];
        write_scissor(((uint32_t*)&Q->rdp_scissor_rect)[0], ((uint32_t*)&Q->rdp_scissor_rect)[1]);
        update_render_mode();
        return;
    case 0x08 ... 0x0F:                         // PassthroughTriangle (raw)
        vrsp_rdpq_send(a, n * 4);
        return;
    case 0x11:                                  // SetDebugMode
        Q->rdp_target_bitdepth = (Q->rdp_target_bitdepth & 0x7F) | ((a0 & 0xFFFFFF) ? 0x80 : 0);
        return;
    case 0x12:                                  // SetScissorEx
        write_scissor(a0 ^ 0x3F000000, a1);
        finalize();
        return;
    case 0x13: {                                // SetPrimColorComponent
        a0 ^= 0x29000000;
        switch ((a0 >> 16) & 3) {
        case 0: a0 |= prim_color_ex & 0x0000FFFF; prim_color_rgba = a1; break;
        case 1: a0 |= prim_color_ex & 0x0000FF00; break;
        default: a0 |= prim_color_ex & 0x000000FF; break;
        }
        prim_color_ex = a0;
        write8(prim_color_ex, prim_color_rgba);
        finalize();
        return;
    }
    case 0x14: {                                // ModifyOtherModes
#ifdef HLE_TEST_PERF
        { extern uint32_t hle_rm_calls[4]; hle_rm_calls[(a0 & 0x8000) ? 1 : 0]++; }
#endif
        uint32_t *w = (a0 & 4) ? &SOM_LO : &SOM_HI;
        *w = (*w & a1) | a[2];
        if (a0 & 0x8000) update_render_mode();
        else write_other_modes(SOM_HI, SOM_LO);
        return;
    }
    case 0x16:                                  // SetFillColor32
        write_fill_color(a1);
        finalize();
        return;
    case 0x18: {                                // SetBlendingMode
        uint32_t t0 = MODE.blend_step0;
        MODE.blend_step1 = a1;
        if (a1 & SOMX_BLEND_2PASS) set_fog_mode(a1);
        else if (t0 & SOMX_BLEND_2PASS) set_fog_mode(0);
        else update_render_mode();
        return;
    }
    case 0x19:                                  // SetFogMode
        set_fog_mode(a1);
        return;
    case 0x1A:                                  // RdpWaitIdle: the RDP is always idle
        return;
    case 0x1B:                                  // SetCombineMode_1Pass
        MODE.combiner_mipmapmask = ((uint64_t)a[2] << 32) | a[3];
        set_combine(a0 ^ 0x80000000, a1);
        return;
    case 0x1C: {                                // AutoTmem_SetAddr
        int16_t v = (int16_t)a0;
        if (v < 0) { autotmem_enabled--; return; }
        if (v == 0) {
            if (!autotmem_enabled) { autotmem_limit = 4096/8; autotmem_prev = 0; autotmem_addr = 0; }
            autotmem_enabled++;
            return;
        }
        autotmem_prev = autotmem_addr;
        autotmem_addr += v;
        assertf(autotmem_addr <= autotmem_limit, "HLE: auto-TMEM full");
        return;
    }
    case 0x1D: {                                // AutoTmem_SetTile
        uint32_t t = (a0 & (1 << 18)) ? autotmem_prev : autotmem_addr;
        a0 = (a0 ^ 0x28000000) + t;
        uint32_t fmt = (a0 >> 19) & 0x1F;
        if (fmt == 3 || fmt == 6 || fmt == 8 || fmt == 9) autotmem_limit = 2048/8;
        write8(a0, a1);
        finalize();
        return;
    }
    case 0x1E:                                  // Triangle
        triangle(a0);
        return;
    case 0x1F: {                                // TriangleData
        int i = (a0 & 0xFF) / 32;
        tri_data[i].xy = a1; tri_data[i].z = a[2]; tri_data[i].rgba = a[3];
        tri_data[i].st = a[4]; tri_data[i].w = a[5]; tri_data[i].inv_w = a[6];
        return;
    }
    case 0x24: case 0x25:                       // TEXTURE_RECTANGLE (+FLIP)
        write16(a0, a1, a[2], a[3]);
        finalize();
        return;
    case 0x29:                                  // SYNC_FULL
        sync_full(a0, a1);
        return;
    case 0x2F:                                  // SET_OTHER_MODES
        SOM_HI = a0 & 0x00FFFFFF;
        SOM_LO = a1;
        write_other_modes(a0, a1);
        return;
    case 0x3A:                                  // SET_PRIM_COLOR
        prim_color_ex = a0; prim_color_rgba = a1;
        write8(a0, a1);
        finalize();
        return;
    case 0x3D: case 0x3E:                       // SET_TEXTURE_IMAGE / SET_Z_IMAGE
        write8(a0 | 0xF0000000, fixup_address(a1));
        finalize();
        return;
    case 0x3F:                                  // SET_COLOR_IMAGE
        Q->rdp_target_bitdepth = ((a0 >> 19) & 3) | (Q->rdp_target_bitdepth & 0x80);
        write8(a0 | 0xF0000000, fixup_address(a1));
        write_fill_color(Q->rdp_fill_color);
        finalize();
        return;
    default:                                    // Passthrough8
        write8(a0, a1);
        finalize();
        return;
    }
}

// ---- Block replay: this overlay's state ----

void hle_rdpq_state(hle_st_t *s)
{
    HLE_ST(s, rs);
}
