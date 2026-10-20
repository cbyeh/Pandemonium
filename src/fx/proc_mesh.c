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
    if (unlit) { t->c = rgba; return; }
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
