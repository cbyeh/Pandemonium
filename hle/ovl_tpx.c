/**
 * C port of Tiny3D's particle microcode (tiny3d/src/t3d/rsp/rsp_tinypx.rspl).
 * Like the ucode, each particle becomes SET_PRIM_COLOR + SET_PRIM_DEPTH + a
 * fill or texture rectangle; the math is done in float on the CPU.
 */
#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/tpx.h>
#include "rsp/rsp_tinypx.h"
#include "rspq_internal.h"
#include "hle.h"

#define D(sym)  (RSP_TPX_##sym & 0xFFF)
#define Q       ((rsp_queue_t*)__vrsp_dmem)

static hle_mat4_t proj, mvp, stack[16];
static int sp;

static void update_mvp(void) { hle_mat_mul(mvp, proj, stack[sp]); }

static void matrix_stack(uint32_t a0, uint32_t a1)
{
    int advance = (int16_t)(a0 >> 8) / (int)sizeof(T3DMat4FP);
    sp += advance;
    assertf(sp >= 0 && sp < 16, "HLE: tpx matrix stack out of range");
    if (a0 & 2) return;
    if (advance >= 0) {
        float m[4][4];
        hle_mat_from_fp(m, hle_rdram(a1));
        if ((a0 & 1) && sp > 0) hle_mat_mul(stack[sp], stack[sp - 1], m);
        else memcpy(stack[sp], m, sizeof(m));
    }
    update_mvp();
}

static void sync_t3d(uint32_t w_norm)
{
    const float (*p)[4], (*mv)[4];
    hle_t3d_matrices(&p, &mv);
    memcpy(proj, p, sizeof(proj));
    memcpy(stack[sp], mv, sizeof(stack[sp]));
    VDMEM16(D(NORM_SCALE_W) + 6) = w_norm;
    update_mvp();
}

static void draw(uint32_t a0, uint32_t a1, bool textured)
{
    bool s16 = (int32_t)a1 < 0;
    const uint8_t *in = hle_rdram(a1 & 0xFFFFFF);
    int stride = s16 ? sizeof(TPXParticleS16) : sizeof(TPXParticleS8);
    int count = (a0 & 0xFFFF) / stride * 2;

    // Screen mapping, identical to Tiny3D's (gsp_3d.vp is its viewport, 1/4 px units)
    float cx = gsp_3d.vp[4], cy = gsp_3d.vp[5], hx = gsp_3d.vp[0], hy = gsp_3d.vp[1];
    float norm_w = VDMEM16(D(NORM_SCALE_W) + 6) / 65535.0f;
    if (norm_w <= 0) norm_w = 1;
    float scale_x = VDMEM16(D(PARTICLE_SCALE)) / 32767.0f, scale_y = VDMEM16(D(PARTICLE_SCALE) + 2) / 32767.0f;
    float base = VDMEM16(D(BASE_SIZE));
    int tex_offset = (int16_t)VDMEM16(D(TEX_OFFSET));

    uint64_t sc = Q->rdp_scissor_rect;
    int min_x = (sc >> 44) & 0xFFF, min_y = (sc >> 32) & 0xFFF;
    int max_x = (sc >> 12) & 0xFFF, max_y = sc & 0xFFF;

    float m[4][3], mw[4];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 3; r++) m[c][r] = mvp[c][r];
        mw[c] = mvp[c][3];
    }

    uint32_t out[8];
    for (int i = 0; i < count; i++) {
        const uint8_t *pt = in + (i >> 1) * stride;
        float x, y, z; int size, toff; const uint8_t *col;
        if (s16) {
            const TPXParticleS16 *p = (const TPXParticleS16*)pt;
            const int16_t *pos = (i & 1) ? p->posB : p->posA;
            x = pos[0]; y = pos[1]; z = pos[2];
            size = (i & 1) ? p->sizeB : p->sizeA;
            toff = (i & 1) ? p->texOffsetB : p->texOffsetA;
            col = (i & 1) ? p->colorB : p->colorA;
        } else {
            const TPXParticleS8 *p = (const TPXParticleS8*)pt;
            const int8_t *pos = (i & 1) ? p->posB : p->posA;
            x = pos[0]; y = pos[1]; z = pos[2];
            size = (i & 1) ? p->sizeB : p->sizeA;
            col = (i & 1) ? p->colorB : p->colorA;
            toff = col[3];
        }
        float w = mw[0]*x + mw[1]*y + mw[2]*z + mw[3];
        if (w <= 0) continue;
        float cz = m[0][2]*x + m[1][2]*y + m[2][2]*z + m[3][2];
        if (cz > w || cz < -w) continue;            // outside near/far
        float inv = 1.0f / w;
        float px = cx + (m[0][0]*x + m[1][0]*y + m[2][0]*z + m[3][0]) * inv * hx;
        float py = cy - (m[0][1]*x + m[1][1]*y + m[2][1]*z + m[3][1]) * inv * hy;
        float half = (size / 128.0f) * base * 0.5f / (w * norm_w);   // 1/4 px
        float hw = half * scale_x, hh = half * scale_y;
        int x0 = px - hw, y0 = py - hh, x1 = px + hw, y1 = py + hh;
        int cx0 = x0 < min_x ? min_x : x0, cy0 = y0 < min_y ? min_y : y0;
        int cx1 = x1 > max_x ? max_x : x1, cy1 = y1 > max_y ? max_y : y1;
        if (cx0 >= cx1 || cy0 >= cy1) continue;

        int depth = 0x3FFF + (int)(cz * inv * 0x3FFF);
        depth = depth < 0 ? 0 : depth > 0x7FFF ? 0x7FFF : depth;
        int n = 0;
        out[n++] = 0xFA000000;
        out[n++] = ((uint32_t)col[0] << 24) | (col[1] << 16) | (col[2] << 8) | col[3];
        out[n++] = 0xEE000000;
        out[n++] = (uint32_t)depth << 16;
        if (!textured) {
            out[n++] = 0xF6000000 | (cx1 << 12) | cy1;
            out[n++] = (cx0 << 12) | cy0;
        } else {
            // One 8x8 texel section per particle, offset by the per-particle index
            float span_x = (x1 - x0) / 4.0f, span_y = (y1 - y0) / 4.0f;   // pixels
            float dsdx = span_x > 0 ? 8.0f / span_x : 0, dtdy = span_y > 0 ? 8.0f / span_y : 0;
            float s = (toff * 8 + tex_offset / 4.0f) + (cx0 - x0) / 4.0f * dsdx;
            float t = (cy0 - y0) / 4.0f * dtdy;
            out[n++] = 0xE4000000 | (cx1 << 12) | cy1;
            out[n++] = (cx0 << 12) | cy0;                   // tile 0
            out[n++] = ((uint32_t)(uint16_t)(int16_t)(s * 32) << 16) | (uint16_t)(int16_t)(t * 32);
            out[n++] = ((uint32_t)(uint16_t)(int16_t)(dsdx * 1024) << 16) | (uint16_t)(int16_t)(dtdy * 1024);
        }
        vrsp_rdpq_send(out, n * 4);
    }
}

void ovl_tpx(uint32_t cmd, const uint32_t *a, int n)
{
    switch (cmd) {
    case TPX_CMD_SYNC_T3D:
        sync_t3d(a[2] & 0xFFFF);
        return;
    case TPX_CMD_DRAW_COLOR:
        draw(a[0], a[1], false);
        return;
    case TPX_CMD_MATRIX_STACK:
        matrix_stack(a[0], a[1]);
        return;
    case TPX_CMD_SET_DMEM: {
        uint32_t addr = a[0] & 0xFFFF;
        if (addr & 0x8000) VDMEM16(addr) = a[1];
        else VDMEM32(addr) = a[1];
        return;
    }
    case TPX_CMD_DRAW_TEXTURE:
        draw(a[0], a[1], true);
        return;
    }
}
