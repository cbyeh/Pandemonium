#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>
#include <math.h>
#include <string.h>

#include "proc_mesh.h"

#define PM_MAX_TRIS   1500
#define PM_BATCH_TRIS 22            // 66 vertices per load (< the 70-entry vertex cache)
#define PM_UNIT       8.0f          // stored at 1/8 unit precision (int16 positions)

typedef struct { int16_t p[3][3]; uint32_t c; } PmTri;

static PmTri *s_tris;
static int s_ntris;

void proc_mesh_begin(void)
{
    if (!s_tris) s_tris = malloc(sizeof(PmTri) * PM_MAX_TRIS);
    s_ntris = 0;
}

static uint32_t shade(uint32_t rgba, float k)
{
    int r = (int)((rgba >> 24) * k), g = (int)(((rgba >> 16) & 0xFF) * k), b = (int)(((rgba >> 8) & 0xFF) * k);
    r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | (rgba & 0xFF);
}

void proc_mesh_tri(const float a[3], const float b[3], const float c[3], uint32_t rgba, bool unlit)
{
    if (s_ntris >= PM_MAX_TRIS) return;
    PmTri *t = &s_tris[s_ntris++];
    const float *v[3] = { a, b, c };
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) t->p[i][k] = (int16_t)lrintf(v[i][k] * PM_UNIT);
    // Unlit (glowing): alpha follows brightness, so proc_mesh_draw_glow fades the dark parts out
    if (unlit) {
        int r = rgba >> 24, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF, m = r > g ? (r > b ? r : b) : (g > b ? g : b);
        t->c = (rgba & ~0xFFu) | (uint32_t)m;
        return;
    }
    // Two-sided flat shading: a key light from above-front plus ambient
    float e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] }, e2[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
    float n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
    float len = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
    float d = len > 1e-6f ? fabsf(n[0] * -0.45f + n[1] * 0.8f + n[2] * 0.4f) / len : 0.5f;
    t->c = shade(rgba, 0.4f + 0.75f * d);
}

void proc_mesh_quad(const float a[3], const float b[3], const float c[3], const float d[3], uint32_t rgba, bool unlit)
{
    proc_mesh_tri(a, b, c, rgba, unlit);
    proc_mesh_tri(a, c, d, rgba, unlit);
}

void proc_mesh_box(const float o[3], const float h[3], uint32_t rgba)
{
    float v[8][3];
    for (int i = 0; i < 8; i++) {
        v[i][0] = o[0] + ((i & 1) ? h[0] : -h[0]);
        v[i][1] = o[1] + ((i & 2) ? h[1] : -h[1]);
        v[i][2] = o[2] + ((i & 4) ? h[2] : -h[2]);
    }
    static const int f[6][4] = { {0,1,3,2}, {4,6,7,5}, {0,4,5,1}, {2,3,7,6}, {0,2,6,4}, {1,5,7,3} };
    for (int i = 0; i < 6; i++) proc_mesh_quad(v[f[i][0]], v[f[i][1]], v[f[i][2]], v[f[i][3]], rgba, false);
}

void proc_mesh_spike(const float base[3], const float tip[3], float radius, int sides, uint32_t rgba)
{
    float d[3] = { tip[0]-base[0], tip[1]-base[1], tip[2]-base[2] };
    float len = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (len < 1e-4f) return;
    for (int i = 0; i < 3; i++) d[i] /= len;
    // Any two axes perpendicular to d
    float up[3] = { 0, 1, 0 };
    if (fabsf(d[1]) > 0.9f) { up[0] = 1; up[1] = 0; }
    float s[3] = { d[1]*up[2]-d[2]*up[1], d[2]*up[0]-d[0]*up[2], d[0]*up[1]-d[1]*up[0] };
    float sl = sqrtf(s[0]*s[0] + s[1]*s[1] + s[2]*s[2]);
    for (int i = 0; i < 3; i++) s[i] /= sl;
    float u[3] = { d[1]*s[2]-d[2]*s[1], d[2]*s[0]-d[0]*s[2], d[0]*s[1]-d[1]*s[0] };
    for (int i = 0; i < sides; i++) {
        float a0 = 2.0f * T3D_PI * i / sides, a1 = 2.0f * T3D_PI * (i + 1) / sides;
        float p0[3], p1[3];
        for (int k = 0; k < 3; k++) {
            p0[k] = base[k] + (s[k] * cosf(a0) + u[k] * sinf(a0)) * radius;
            p1[k] = base[k] + (s[k] * cosf(a1) + u[k] * sinf(a1)) * radius;
        }
        proc_mesh_tri(p0, p1, tip, rgba, false);
    }
}

void proc_mesh_frustum(const float b[3], float r0, float r1, float h, int sides, uint32_t rgba)
{
    float top[3] = { b[0], b[1] + h, b[2] };
    for (int i = 0; i < sides; i++) {
        float a0 = 2.0f * T3D_PI * i / sides, a1 = 2.0f * T3D_PI * (i + 1) / sides;
        float p0[3] = { b[0] + cosf(a0) * r0, b[1], b[2] + sinf(a0) * r0 };
        float p1[3] = { b[0] + cosf(a1) * r0, b[1], b[2] + sinf(a1) * r0 };
        float q0[3] = { b[0] + cosf(a0) * r1, b[1] + h, b[2] + sinf(a0) * r1 };
        float q1[3] = { b[0] + cosf(a1) * r1, b[1] + h, b[2] + sinf(a1) * r1 };
        proc_mesh_quad(p0, p1, q1, q0, rgba, false);
        if (r1 > 0.0f) proc_mesh_tri(q0, q1, top, rgba, false);
    }
}

static float hash01(int a, int b)
{
    uint32_t h = (uint32_t)a * 374761393u + (uint32_t)b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

static uint32_t mix_rgb(const int a[3], const int b[3], float t, float k)
{
    int c[3];
    for (int i = 0; i < 3; i++) {
        float v = (a[i] + (b[i] - a[i]) * t) * k;
        c[i] = v < 0 ? 0 : v > 255 ? 255 : (int)v;
    }
    return proc_rgb(c[0], c[1], c[2]);
}

// Built looking down +z, then turned to -y unless facingZ
static void eye_point(float x, float y, float z, float r, bool facingZ, float out[3])
{
    if (facingZ) { out[0] = x * r; out[1] = y * r; out[2] = z * r; }
    else         { out[0] = x * r; out[1] = -z * r; out[2] = y * r; }
}

// Tangent-plane point (u, v) at the eye, pushed onto the sphere
static void eye_patch_point(float u, float v, float r, bool facingZ, float out[3])
{
    float l = sqrtf(u * u + v * v + 1.0f);
    eye_point(u / l, v / l, 1.0f / l, r, facingZ, out);
}

void proc_mesh_eye_sphere(float radius, bool facingZ, bool hot)
{
    // Molten body: it always faces the player, so the limb darkening is baked in,
    // white-gold at the eye through orange to a deep red rim; mottled granulation
    // and a few dark spots
    enum { LAT = 14, LON = 20 };
    static const int RIM[3] = { 170, 34, 10 }, MID[3] = { 240, 100, 25 }, CORE[3] = { 255, 232, 165 };
    static const int RIM_H[3] = { 170, 14, 26 }, MID_H[3] = { 235, 45, 40 }, CORE_H[3] = { 255, 236, 220 };
    for (int i = 0; i < LAT; i++) {
        float a0 = T3D_PI * powf((float)i / LAT, 1.3f), a1 = T3D_PI * powf((float)(i + 1) / LAT, 1.3f);
        for (int j = 0; j < LON; j++) {
            float b0 = 2.0f * T3D_PI * j / LON, b1 = 2.0f * T3D_PI * (j + 1) / LON;
            float p[4][3];
            eye_point(sinf(a0) * cosf(b0), sinf(a0) * sinf(b0), cosf(a0), radius, facingZ, p[0]);
            eye_point(sinf(a1) * cosf(b0), sinf(a1) * sinf(b0), cosf(a1), radius, facingZ, p[1]);
            eye_point(sinf(a1) * cosf(b1), sinf(a1) * sinf(b1), cosf(a1), radius, facingZ, p[2]);
            eye_point(sinf(a0) * cosf(b1), sinf(a0) * sinf(b1), cosf(a0), radius, facingZ, p[3]);
            float k = (cosf((a0 + a1) * 0.5f) + 0.7f) / 1.7f;   // the back still glows (seen from above)
            k = k < 0 ? 0 : k > 1 ? 1 : k;
            float g = 0.93f + 0.12f * hash01(i, j);
            if (hash01(j + 31, i + 7) < 0.035f && k < 0.8f) g *= 0.6f;       // sunspot
            uint32_t c = k < 0.55f ? mix_rgb(hot ? RIM_H : RIM, hot ? MID_H : MID, k / 0.55f, g)
                                   : mix_rgb(hot ? MID_H : MID, hot ? CORE_H : CORE, (k - 0.55f) / 0.45f, g);
            proc_mesh_quad(p[0], p[1], p[2], p[3], c, true);
        }
    }
    // Iris: rings of fire round a slit pupil, just above the surface
    enum { SEG = 16 };
    static const float RR[5] = { 0.5f, 0.4f, 0.3f, 0.2f, 0.12f };
    static const int IR[4][3] = { { 150, 20, 6 }, { 255, 90, 15 }, { 255, 170, 50 }, { 255, 225, 120 } };
    static const int IR_H[4][3] = { { 120, 0, 10 }, { 255, 40, 40 }, { 255, 140, 120 }, { 255, 230, 220 } };
    for (int k = 0; k < 4; k++)
        for (int j = 0; j < SEG; j++) {
            float b0 = 2.0f * T3D_PI * j / SEG, b1 = 2.0f * T3D_PI * (j + 1) / SEG;
            float p[4][3];
            float rs = radius * 1.012f;
            eye_patch_point(RR[k] * cosf(b0), RR[k] * 1.15f * sinf(b0), rs, facingZ, p[0]);
            eye_patch_point(RR[k + 1] * cosf(b0), RR[k + 1] * 1.15f * sinf(b0), rs, facingZ, p[1]);
            eye_patch_point(RR[k + 1] * cosf(b1), RR[k + 1] * 1.15f * sinf(b1), rs, facingZ, p[2]);
            eye_patch_point(RR[k] * cosf(b1), RR[k] * 1.15f * sinf(b1), rs, facingZ, p[3]);
            const int *col = hot ? IR_H[k] : IR[k];
            proc_mesh_quad(p[0], p[1], p[2], p[3], proc_rgb(col[0], col[1], col[2]), true);
        }
    // the inner disc, then the slit (a pointed lens) over it
    for (int j = 0; j < SEG; j++) {
        float b0 = 2.0f * T3D_PI * j / SEG, b1 = 2.0f * T3D_PI * (j + 1) / SEG, p[3][3];
        eye_patch_point(0, 0, radius * 1.012f, facingZ, p[0]);
        eye_patch_point(RR[4] * cosf(b1), RR[4] * 1.15f * sinf(b1), radius * 1.012f, facingZ, p[1]);
        eye_patch_point(RR[4] * cosf(b0), RR[4] * 1.15f * sinf(b0), radius * 1.012f, facingZ, p[2]);
        proc_mesh_tri(p[0], p[1], p[2], hot ? proc_rgb(255, 245, 235) : proc_rgb(255, 245, 200), true);
    }
    enum { SL = 8 };
    for (int j = 0; j < SL; j++) {
        float v0 = -0.42f + 0.84f * j / SL, v1 = -0.42f + 0.84f * (j + 1) / SL;
        float w0 = 0.07f * sqrtf(fmaxf(0.0f, 1.0f - (v0 / 0.42f) * (v0 / 0.42f)));
        float w1 = 0.07f * sqrtf(fmaxf(0.0f, 1.0f - (v1 / 0.42f) * (v1 / 0.42f)));
        float p[4][3], rs = radius * 1.02f;
        eye_patch_point(-w0, v0, rs, facingZ, p[0]); eye_patch_point(w0, v0, rs, facingZ, p[1]);
        eye_patch_point(w1, v1, rs, facingZ, p[2]); eye_patch_point(-w1, v1, rs, facingZ, p[3]);
        proc_mesh_quad(p[0], p[1], p[2], p[3], hot ? proc_rgb(50, 0, 4) : proc_rgb(12, 3, 2), true);
    }
}

void proc_mesh_corona(float radius, int tongues, float reach, int seed, bool hot)
{
    // Flame tongues round a disc facing +z, bright at the root and fading to the tip
    // (drawn additively: dark is see-through)
    enum { STEPS = 3 };
    static const int COL[STEPS][3] = { { 255, 150, 40 }, { 190, 70, 10 }, { 90, 22, 0 } };
    static const int COL_H[STEPS][3] = { { 255, 90, 80 }, { 200, 30, 30 }, { 90, 0, 10 } };
    for (int k = 0; k < tongues; k++) {
        float a = 2.0f * T3D_PI * (k + 0.5f * hash01(k, seed)) / tongues;
        float len = reach * (0.55f + 0.45f * hash01(seed, k));
        float w = 2.0f * T3D_PI / tongues * 0.55f, bend = (hash01(k + 7, seed) - 0.5f) * 0.5f;
        for (int st = 0; st < STEPS; st++) {
            float t0 = (float)st / STEPS, t1 = (float)(st + 1) / STEPS;
            float r0 = radius * 0.92f + len * t0, r1 = radius * 0.92f + len * t1;
            float h0 = w * (1.0f - t0), h1 = w * (1.0f - t1);
            float c0 = a + bend * t0, c1 = a + bend * t1;
            float p[4][3] = {
                { cosf(c0 - h0) * r0, sinf(c0 - h0) * r0, 0 }, { cosf(c0 + h0) * r0, sinf(c0 + h0) * r0, 0 },
                { cosf(c1 + h1) * r1, sinf(c1 + h1) * r1, 0 }, { cosf(c1 - h1) * r1, sinf(c1 - h1) * r1, 0 },
            };
            const int *c = hot ? COL_H[st] : COL[st];
            proc_mesh_quad(p[0], p[1], p[2], p[3], proc_rgb(c[0], c[1], c[2]), true);
        }
    }
}

void proc_mesh_draw_glow(const ProcMesh *m)
{
    if (!m->block) return;
    rdpq_sync_pipe();
    rdpq_set_mode_standard();
    rdpq_mode_zbuf(true, false);
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    // Blended by brightness, not added: the real RDP wraps an overflowing sum (the Sun's
    // corona tips speckled green on hardware; HLE renderers clamp, so it never showed there)
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT);
    t3d_matrix_set(m->mat, true);
    rspq_block_run(m->block);
}

void proc_mesh_chain_link(float len, float width, float thick, uint32_t rgba, bool unlit)
{
    // An oval ring in the xy plane, long along y, with a diamond cross-section
    enum { NS = 8, NT = 4 };
    float a = len * 0.5f - width, pts[NS][3], nrm[NS][3];
    for (int i = 0; i < NS; i++) {
        float t = 2.0f * T3D_PI * (i + 0.5f) / NS;
        float cx = cosf(t), cy = sinf(t);
        pts[i][0] = cx * width; pts[i][1] = cy * width + (cy >= 0 ? a : -a); pts[i][2] = 0;
        nrm[i][0] = cx; nrm[i][1] = cy; nrm[i][2] = 0;
    }
    for (int i = 0; i < NS; i++) {
        int i1 = (i + 1) % NS;
        for (int k = 0; k < NT; k++) {
            float c0 = cosf(T3D_PI * 0.5f * k), s0 = sinf(T3D_PI * 0.5f * k);
            float c1 = cosf(T3D_PI * 0.5f * (k + 1)), s1 = sinf(T3D_PI * 0.5f * (k + 1));
            float q[4][3];
            for (int m = 0; m < 3; m++) {
                q[0][m] = pts[i][m] + (nrm[i][m] * c0 + (m == 2) * s0) * thick;
                q[1][m] = pts[i1][m] + (nrm[i1][m] * c0 + (m == 2) * s0) * thick;
                q[2][m] = pts[i1][m] + (nrm[i1][m] * c1 + (m == 2) * s1) * thick;
                q[3][m] = pts[i][m] + (nrm[i][m] * c1 + (m == 2) * s1) * thick;
            }
            proc_mesh_quad(q[0], q[1], q[2], q[3], rgba, unlit);
        }
    }
}

void proc_mesh_end(ProcMesh *out)
{
    memset(out, 0, sizeof(*out));
    if (s_ntris == 0) return;
    int batches = (s_ntris + PM_BATCH_TRIS - 1) / PM_BATCH_TRIS;
    int perBatch = PM_BATCH_TRIS * 3;               // even
    out->verts = malloc_uncached(sizeof(T3DVertPacked) * (perBatch / 2) * batches);
    uint16_t norm = t3d_vert_pack_normal(&(T3DVec3){{ 0, 1, 0 }});
    for (int t = 0; t < s_ntris; t++) {
        for (int i = 0; i < 3; i++) {
            int vi = (t / PM_BATCH_TRIS) * perBatch + (t % PM_BATCH_TRIS) * 3 + i;
            T3DVertPacked *v = &out->verts[vi >> 1];
            const int16_t *p = s_tris[t].p[i];
            if (vi & 1) { memcpy(v->posB, p, 6); v->normB = norm; v->rgbaB = s_tris[t].c; }
            else        { memcpy(v->posA, p, 6); v->normA = norm; v->rgbaA = s_tris[t].c; }
        }
    }
    rspq_block_begin();
    for (int bi = 0; bi < batches; bi++) {
        int n = s_ntris - bi * PM_BATCH_TRIS;
        if (n > PM_BATCH_TRIS) n = PM_BATCH_TRIS;
        t3d_vert_load(&out->verts[bi * perBatch / 2], 0, (uint32_t)((n * 3 + 1) & ~1));
        for (int i = 0; i < n; i++) t3d_tri_draw(i * 3, i * 3 + 1, i * 3 + 2);
        t3d_tri_sync();
    }
    out->block = rspq_block_end();
    out->mat = malloc_uncached(sizeof(T3DMat4FP));
    proc_mesh_set_transform(out, 1.0f, (float[3]){ 0, 0, 0 }, (float[3]){ 0, 0, 0 });
}

void proc_mesh_set_transform(ProcMesh *m, const float scale, const float rot[3], const float pos[3])
{
    if (!m->mat) return;
    float s = scale / PM_UNIT;
    t3d_mat4fp_from_srt_euler(m->mat, (float[3]){ s, s, s }, rot, pos);
}

void proc_mesh_draw(const ProcMesh *m)
{
    if (!m->block) return;
    rdpq_sync_pipe();
    rdpq_set_mode_standard();
    rdpq_mode_zbuf(true, true);
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT);
    t3d_matrix_set(m->mat, true);
    rspq_block_run(m->block);
}

void proc_mesh_free(ProcMesh *m)
{
    if (m->block) { rspq_wait(); rspq_block_free(m->block); }
    if (m->verts) free_uncached(m->verts);
    if (m->mat) free_uncached(m->mat);
    memset(m, 0, sizeof(*m));
}
