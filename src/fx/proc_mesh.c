#include <libdragon.h>
#include "gpu_frame.h"
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>
#include <math.h>
#include <string.h>

#include "proc_mesh.h"

// The build scratch (43 KB), static: not on the heap. Freed after each build it left a hole in
// the middle of the heap that a fight's later buffers fell into wherever it happened to be, and
// Shadowborne's frame took up to 2.6 ms more RSP time in some layouts (Ares)
#define PM_MAX_TRIS   900           // the largest, the ending's whole sun (ball, iris, pupil): 812 before its degenerate ones go
#define PM_UNIT       8.0f          // stored at 1/8 unit precision (int16 positions)

typedef struct { int16_t p[3][3]; uint32_t c[3]; int16_t st[3][2]; } PmTri;     // a colour and UV per corner

static PmTri s_tris[PM_MAX_TRIS];
static int s_ntris;
static float s_uvAround, s_uvAlong;     // proc_mesh_uv (0: no UVs)
static float s_uvShift[2];              // proc_mesh_uv_shift
static uint32_t s_uvFlatCol;            // proc_mesh_uv_flat_color (0: none)
#define PM_FLAT_ST (2 * 32)             // unlit faces sample texel (2, 2): proc_mesh_uv's bright corner

void proc_mesh_begin(void)
{
    s_ntris = 0;
    s_uvAround = s_uvAlong = s_uvShift[0] = s_uvShift[1] = 0.0f;
    s_uvFlatCol = 0;
}

void proc_mesh_uv(float around, float along) { s_uvAround = around; s_uvAlong = along; }
void proc_mesh_uv_shift(float du, float dv) { s_uvShift[0] = du; s_uvShift[1] = dv; }
void proc_mesh_uv_flat_color(uint32_t rgba) { s_uvFlatCol = rgba; }

static int16_t st_px(float px) { return (int16_t)lrintf(fminf(fmaxf(px, -1000.0f), 1000.0f) * 32.0f); }    // 10.5 fixed

// Planar UVs for a flat face: its two axes that the normal turns least to (a box face, a membrane)
static void planar_st(PmTri *t, const float *v[3], const float n[3])
{
    float ax = fabsf(n[0]), ay = fabsf(n[1]), az = fabsf(n[2]);
    int iu = ax >= ay && ax >= az ? 2 : 0, iv = ay >= ax && ay >= az ? 2 : 1;
    for (int i = 0; i < 3; i++) { t->st[i][0] = st_px(v[i][iu] * s_uvAlong); t->st[i][1] = st_px(v[i][iv] * s_uvAlong); }
}

static uint32_t shade(uint32_t rgba, float k)
{
    int r = (int)((rgba >> 24) * k), g = (int)(((rgba >> 16) & 0xFF) * k), b = (int)(((rgba >> 8) & 0xFF) * k);
    r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | (rgba & 0xFF);
}

void proc_mesh_tri(const float a[3], const float b[3], const float c[3], uint32_t rgba, bool unlit)
{
    assertf(s_ntris < PM_MAX_TRIS, "proc_mesh: more than %d triangles", PM_MAX_TRIS);
    if (s_ntris >= PM_MAX_TRIS) return;
    PmTri *t = &s_tris[s_ntris++];
    const float *v[3] = { a, b, c };
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) t->p[i][k] = (int16_t)lrintf(v[i][k] * PM_UNIT);
    // Unlit (glowing): alpha follows brightness, so proc_mesh_draw_glow fades the dark parts out
    if (unlit) {
        int r = rgba >> 24, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF, m = r > g ? (r > b ? r : b) : (g > b ? g : b);
        t->c[0] = t->c[1] = t->c[2] = (rgba & ~0xFFu) | (uint32_t)m;
        for (int i = 0; i < 3; i++) t->st[i][0] = t->st[i][1] = s_uvAlong > 0.0f ? PM_FLAT_ST : 0;     // glows: untextured
        return;
    }
    // Two-sided flat shading: a key light from above-front plus ambient
    float e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] }, e2[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
    float n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
    planar_st(t, v, n);
    float len = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
    float d = len > 1e-6f ? fabsf(n[0] * -0.45f + n[1] * 0.8f + n[2] * 0.4f) / len : 0.5f;
    t->c[0] = t->c[1] = t->c[2] = shade(rgba, 0.4f + 0.75f * d);
}

// The same key light as the flat faces, one-sided for a smooth surface (plus a little bounce)
static float smooth_light(const float n[3])
{
    float l = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
    float d = l > 1e-6f ? (n[0] * -0.45f + n[1] * 0.8f + n[2] * 0.4f) / l : 0.0f;
    return 0.42f + 0.72f * (d > 0.0f ? d : 0.0f) + 0.14f * (d < 0.0f ? -d : 0.0f);
}

// col: each corner's colour; uv: its UV in pixels (NULL: none)
static void tri_smooth_uv(const float a[3], const float b[3], const float c[3],
                          const float na[3], const float nb[3], const float nc[3], const uint32_t col[3], const float uv[3][2])
{
    assertf(s_ntris < PM_MAX_TRIS, "proc_mesh: more than %d triangles", PM_MAX_TRIS);
    if (s_ntris >= PM_MAX_TRIS) return;
    PmTri *t = &s_tris[s_ntris++];
    const float *v[3] = { a, b, c }, *n[3] = { na, nb, nc };
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) t->p[i][k] = (int16_t)lrintf(v[i][k] * PM_UNIT);
        t->c[i] = shade(col[i], smooth_light(n[i]));
        t->st[i][0] = uv ? st_px(uv[i][0] + s_uvShift[0]) : 0; t->st[i][1] = uv ? st_px(uv[i][1] + s_uvShift[1]) : 0;
    }
}

void proc_mesh_tri_smooth(const float a[3], const float b[3], const float c[3],
                          const float na[3], const float nb[3], const float nc[3], uint32_t rgba)
{
    tri_smooth_uv(a, b, c, na, nb, nc, (uint32_t[3]){ rgba, rgba, rgba }, NULL);
}

// vcols: a colour per point (ring-major), else cols: per quad, else rgba
static void loft(const float *pts, int rings, int sides, const uint32_t *cols, const uint32_t *vcols, uint32_t rgba, bool capEnd)
{
    bool shut[rings];       // the rings closed on a point
    for (int r = 0; r < rings; r++) {
        shut[r] = true;
        for (int i = 1; i < sides; i++)
            for (int k = 0; k < 3; k++) if (fabsf(pts[(r * sides + i) * 3 + k] - pts[r * sides * 3 + k]) > 1e-3f) shut[r] = false;
    }
    // Normals point out from each ring's centre: a smooth tube
    for (int r = 0; r + 1 < rings; r++) {
        float c0[3] = { 0 }, c1[3] = { 0 };
        for (int i = 0; i < sides; i++)
            for (int k = 0; k < 3; k++) { c0[k] += pts[(r * sides + i) * 3 + k] / sides; c1[k] += pts[((r + 1) * sides + i) * 3 + k] / sides; }
        for (int i = 0; i < sides; i++) {
            int j = (i + 1) % sides;
            const float *p00 = &pts[(r * sides + i) * 3], *p01 = &pts[(r * sides + j) * 3];
            const float *p10 = &pts[((r + 1) * sides + i) * 3], *p11 = &pts[((r + 1) * sides + j) * 3];
            float n00[3], n01[3], n10[3], n11[3];
            for (int k = 0; k < 3; k++) { n00[k] = p00[k] - c0[k]; n01[k] = p01[k] - c0[k]; n10[k] = p10[k] - c1[k]; n11[k] = p11[k] - c1[k]; }
            uint32_t col = cols ? cols[r * sides + i] : rgba;
            uint32_t c00 = vcols ? vcols[r * sides + i] : col, c01 = vcols ? vcols[r * sides + j] : col;
            uint32_t c10 = vcols ? vcols[(r + 1) * sides + i] : col, c11 = vcols ? vcols[(r + 1) * sides + j] : col;
            // UVs: round the ring (i / sides of `around`), along by the rings' z (the tubes run along +z);
            // a ring closed on a point (a dome's tip) one UV, so its corners stay shared
            float u0 = s_uvAround * i / sides, u1 = s_uvAround * (i + 1) / sides, v0 = c0[2] * s_uvAlong, v1 = c1[2] * s_uvAlong;
            float a0 = shut[r] ? 0.0f : u0, a1 = shut[r] ? 0.0f : u1, b0 = shut[r + 1] ? 0.0f : u0, b1 = shut[r + 1] ? 0.0f : u1;
            bool uv = s_uvAlong > 0.0f;
            if (uv && s_uvFlatCol && col == s_uvFlatCol) {      // untextured: the flat texel
                const float f = PM_FLAT_ST / 32.0f - s_uvShift[0], g = PM_FLAT_ST / 32.0f - s_uvShift[1];
                a0 = a1 = b0 = b1 = f; v0 = v1 = g;
            }
            tri_smooth_uv(p00, p01, p11, n00, n01, n11, (uint32_t[3]){ c00, c01, c11 }, uv ? (const float[3][2]){ { a0, v0 }, { a1, v0 }, { b1, v1 } } : NULL);
            tri_smooth_uv(p00, p11, p10, n00, n11, n10, (uint32_t[3]){ c00, c11, c10 }, uv ? (const float[3][2]){ { a0, v0 }, { b1, v1 }, { b0, v1 } } : NULL);
        }
    }
    if (capEnd && rings > 0) {     // a fan to the last ring's centre
        int r = rings - 1;
        float c[3] = { 0 }, axis[3];
        for (int i = 0; i < sides; i++) for (int k = 0; k < 3; k++) c[k] += pts[(r * sides + i) * 3 + k] / sides;
        for (int k = 0; k < 3; k++) axis[k] = c[k] - (rings > 1 ? 0.0f : c[k]);
        if (rings > 1) { float p[3] = { 0 }; for (int i = 0; i < sides; i++) for (int k = 0; k < 3; k++) p[k] += pts[((r - 1) * sides + i) * 3 + k] / sides;
                         for (int k = 0; k < 3; k++) axis[k] = c[k] - p[k]; }
        for (int i = 0; i < sides; i++) {
            int j = (i + 1) % sides;
            float u0 = s_uvAround * i / sides, u1 = s_uvAround * (i + 1) / sides, v = c[2] * s_uvAlong;
            uint32_t cq = cols ? cols[(r > 0 ? r - 1 : 0) * sides + i] : rgba;
            uint32_t ci = vcols ? vcols[r * sides + i] : cq, cj = vcols ? vcols[r * sides + j] : cq;
            tri_smooth_uv(&pts[(r * sides + i) * 3], &pts[(r * sides + j) * 3], c, axis, axis, axis, (uint32_t[3]){ ci, cj, vcols ? vcols[r * sides] : cq },
                          s_uvAlong > 0.0f ? (const float[3][2]){ { u0, v }, { u1, v }, { (u0 + u1) * 0.5f, v } } : NULL);
        }
    }
}

void proc_mesh_loft(const float *pts, int rings, int sides, const uint32_t *cols, uint32_t rgba, bool capEnd) { loft(pts, rings, sides, cols, NULL, rgba, capEnd); }
void proc_mesh_loft_vcol(const float *pts, int rings, int sides, const uint32_t *vcols, bool capEnd) { loft(pts, rings, sides, NULL, vcols, 0, capEnd); }

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
    // wound outward, as the tubes and spikes (a closed mesh can be drawn with its far side culled)
    static const int f[6][4] = { {2,3,1,0}, {5,7,6,4}, {1,5,4,0}, {6,7,3,2}, {4,6,2,0}, {3,7,5,1} };
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
        int n0 = s_ntris;
        proc_mesh_tri(p0, p1, tip, rgba, false);
        if (s_uvAlong > 0.0f && s_ntris > n0) {     // round it and along it, as a tube (not planar)
            PmTri *t = &s_tris[s_ntris - 1];
            float u0 = s_uvShift[0] + s_uvAround * i / sides, u1 = s_uvShift[0] + s_uvAround * (i + 1) / sides;
            t->st[0][0] = st_px(u0); t->st[1][0] = st_px(u1); t->st[2][0] = st_px((u0 + u1) * 0.5f);
            t->st[0][1] = t->st[1][1] = st_px(s_uvShift[1]); t->st[2][1] = st_px(s_uvShift[1] + len * s_uvAlong);
        }
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

static float clamp01f(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

static uint32_t mix_rgb(const int a[3], const int b[3], float t, float k)
{
    int c[3];
    for (int i = 0; i < 3; i++) {
        float v = (a[i] + (b[i] - a[i]) * t) * k;
        c[i] = v < 0 ? 0 : v > 255 ? 255 : (int)v;
    }
    return proc_rgb(c[0], c[1], c[2]);
}

// An unlit triangle with a colour and a UV (pixels) per corner
static void tri_unlit_uv(const float a[3], const float b[3], const float c[3], const uint32_t col[3], const float uv[3][2])
{
    assertf(s_ntris < PM_MAX_TRIS, "proc_mesh: more than %d triangles", PM_MAX_TRIS);
    if (s_ntris >= PM_MAX_TRIS) return;
    PmTri *t = &s_tris[s_ntris++];
    const float *v[3] = { a, b, c };
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) t->p[i][k] = (int16_t)lrintf(v[i][k] * PM_UNIT);
        int r = col[i] >> 24, g = (col[i] >> 16) & 0xFF, bl = (col[i] >> 8) & 0xFF, m = r > g ? (r > bl ? r : bl) : (g > bl ? g : bl);
        // alpha follows brightness, as proc_mesh_tri's unlit faces, unless given (not 0xFF)
        t->c[i] = (col[i] & 0xFF) != 0xFF ? col[i] : (col[i] & ~0xFFu) | (uint32_t)m;
        t->st[i][0] = st_px(uv[i][0]); t->st[i][1] = st_px(uv[i][1]);
    }
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

// (textured, the flat texel: proc_mesh_uv's clear corner)
static void tri_unlit3(const float a[3], const float b[3], const float c[3], uint32_t ca, uint32_t cb, uint32_t cc)
{
    const float f = s_uvAlong > 0.0f ? PM_FLAT_ST / 32.0f : 0.0f;
    tri_unlit_uv(a, b, c, (uint32_t[3]){ ca, cb, cc }, (const float[3][2]){ { f, f }, { f, f }, { f, f } });
}

// The molten surface at a point `cosPolar` round from the eye: it always faces the player, so the
// limb darkening is baked in, white-gold at the eye through orange to a deep red rim
static uint32_t sun_surface(float cosPolar, bool hot, float g)
{
    static const int RIM[3] = { 170, 34, 10 }, MID[3] = { 240, 100, 25 }, CORE[3] = { 255, 232, 165 };
    static const int RIM_H[3] = { 170, 14, 26 }, MID_H[3] = { 235, 45, 40 }, CORE_H[3] = { 255, 236, 220 };
    float k = clamp01f((cosPolar + 0.7f) / 1.7f);   // the back still glows (seen from above)
    return k < 0.55f ? mix_rgb(hot ? RIM_H : RIM, hot ? MID_H : MID, k / 0.55f, g)
                     : mix_rgb(hot ? MID_H : MID, hot ? CORE_H : CORE, (k - 0.55f) / 0.45f, g);
}

// The ball's UVs at (x, y, z) (built looking down +z): round its up axis and down from its top,
// so the texture's poles are at the top and bottom, not pinched into the middle of the face
static void ball_uv(float x, float y, float z, float uv[2])
{
    uv[0] = (atan2f(x, z) / (2.0f * T3D_PI) + 0.5f) * s_uvAround;
    uv[1] = acosf(fmaxf(-1.0f, fminf(1.0f, y))) / T3D_PI * s_uvAlong;
}

// Corners i, j, k of a quad (p: placed, l: as built)
static void ball_tri(const float p[4][3], const float l[4][3], int i0, int i1, int i2, uint32_t c0, uint32_t c1, uint32_t c2)
{
    const int ix[3] = { i0, i1, i2 };
    float uv[3][2];
    for (int i = 0; i < 3; i++) ball_uv(l[ix[i]][0], l[ix[i]][1], l[ix[i]][2], uv[i]);
    // a triangle across the seam at the back: its corners onto one side of it
    float mn = fminf(uv[0][0], fminf(uv[1][0], uv[2][0])), mx = fmaxf(uv[0][0], fmaxf(uv[1][0], uv[2][0]));
    if (mx - mn > s_uvAround * 0.5f) for (int i = 0; i < 3; i++) if (uv[i][0] < s_uvAround * 0.5f) uv[i][0] += s_uvAround;
    tri_unlit_uv(p[i0], p[i1], p[i2], (uint32_t[3]){ c0, c1, c2 }, (const float (*)[2])uv);
}

void proc_mesh_sun_ball(float radius, bool facingZ, bool hot)
{
    // Every corner its own colour, shared by the quads round it: one smooth gradient, mottled
    // in soft blotches; its veins are the texture's (proc_mesh_uv: `around` pixels round it,
    // `along` from its top to its bottom)
    enum { LAT = 9, LON = 18 };     // rings close at the eye, wide over the back (the RSP's cost is in the vertices)
    uint32_t col[LAT + 1][LON];
    float a[LAT + 1];
    for (int i = 0; i <= LAT; i++) {
        a[i] = T3D_PI * powf((float)i / LAT, 1.4f);
        for (int j = 0; j < LON; j++)
            col[i][j] = sun_surface(cosf(a[i]), hot, 1.0f + 0.16f * (hash01(i, j) - 0.5f) * sinf(a[i]));
    }
    for (int i = 0; i < LAT; i++)
        for (int j = 0; j < LON; j++) {
            int j1 = (j + 1) % LON;
            float b0 = 2.0f * T3D_PI * j / LON, b1 = 2.0f * T3D_PI * (j + 1) / LON, l[4][3], p[4][3];
            const float ai[4] = { a[i], a[i + 1], a[i + 1], a[i] }, bi[4] = { b0, b0, b1, b1 };
            for (int q = 0; q < 4; q++) {
                l[q][0] = sinf(ai[q]) * cosf(bi[q]); l[q][1] = sinf(ai[q]) * sinf(bi[q]); l[q][2] = cosf(ai[q]);
                eye_point(l[q][0], l[q][1], l[q][2], radius, facingZ, p[q]);
            }
            ball_tri((const float (*)[3])p, (const float (*)[3])l, 0, 1, 2, col[i][j], col[i + 1][j], col[i + 1][j1]);
            ball_tri((const float (*)[3])p, (const float (*)[3])l, 0, 2, 3, col[i][j], col[i + 1][j1], col[i][j1]);
        }
}

static void eye_iris(float radius, bool facingZ, bool hot)
{
    // Rings of fire blending into each other, from the surface's own colour at the edge through a
    // dark limbal ring to white-gold at the middle
    enum { SEG = 16, NR = 5 };
    static const float RR[NR] = { 0.58f, 0.5f, 0.36f, 0.2f, 0.0f };
    static const int IR[NR - 1][3] = { { 0 }, { 140, 18, 6 }, { 245, 105, 22 }, { 255, 200, 90 } };
    static const int IR_H[NR - 1][3] = { { 0 }, { 115, 0, 12 }, { 245, 60, 50 }, { 255, 185, 165 } };
    float rs = radius * 1.012f;
    for (int k = 0; k + 1 < NR; k++)
        for (int j = 0; j < SEG; j++) {
            float b[2] = { 2.0f * T3D_PI * j / SEG, 2.0f * T3D_PI * (j + 1) / SEG }, p[4][3];
            uint32_t c[4];
            for (int e = 0; e < 4; e++) {
                int ring = k + (e == 1 || e == 2), side = e >= 2;
                float u = RR[ring] * cosf(b[side]), v = RR[ring] * 1.15f * sinf(b[side]);
                eye_patch_point(u, v, rs, facingZ, p[e]);
                const int *ic = ring == NR - 1 ? (hot ? (const int[3]){ 255, 248, 240 } : (const int[3]){ 255, 250, 215 }) : (hot ? IR_H : IR)[ring];
                c[e] = ring == 0 ? sun_surface(1.0f / sqrtf(1.0f + u * u + v * v), hot, 1.0f) : proc_rgb(ic[0], ic[1], ic[2]);
            }
            if (k + 2 < NR) { tri_unlit3(p[0], p[1], p[2], c[0], c[1], c[2]); tri_unlit3(p[0], p[2], p[3], c[0], c[2], c[3]); }
            else tri_unlit3(p[0], p[1], p[3], c[0], c[1], c[3]);      // the last ring closes on the middle
        }
}

static void eye_pupil(float radius, bool facingZ, bool hot)
{
    // The slit (a pointed lens), darkest down its middle
    enum { SL = 8 };
    uint32_t edge = hot ? proc_rgb(110, 0, 8) : proc_rgb(70, 8, 3), mid = hot ? proc_rgb(40, 0, 4) : proc_rgb(8, 2, 1);
    float rs = radius * 1.02f;
    for (int j = 0; j < SL; j++) {
        float v0 = -0.42f + 0.84f * j / SL, v1 = -0.42f + 0.84f * (j + 1) / SL;
        float w0 = 0.07f * sqrtf(fmaxf(0.0f, 1.0f - (v0 / 0.42f) * (v0 / 0.42f)));
        float w1 = 0.07f * sqrtf(fmaxf(0.0f, 1.0f - (v1 / 0.42f) * (v1 / 0.42f)));
        float p[6][3];
        eye_patch_point(-w0, v0, rs, facingZ, p[0]); eye_patch_point(0, v0, rs, facingZ, p[1]); eye_patch_point(w0, v0, rs, facingZ, p[2]);
        eye_patch_point(-w1, v1, rs, facingZ, p[3]); eye_patch_point(0, v1, rs, facingZ, p[4]); eye_patch_point(w1, v1, rs, facingZ, p[5]);
        for (int h = 0; h < 2; h++) {     // columns -w, 0, +w: edge, middle, edge
            uint32_t ca = h ? mid : edge, cb = h ? edge : mid;
            tri_unlit3(p[h], p[h + 1], p[h + 4], ca, cb, cb);
            tri_unlit3(p[h], p[h + 4], p[h + 3], ca, cb, ca);
        }
    }
}

void proc_mesh_eye_sphere(float radius, bool facingZ, bool hot)
{
    proc_mesh_sun_ball(radius, facingZ, hot);
    eye_iris(radius, facingZ, hot);
    eye_pupil(radius, facingZ, hot);
}

// c with alpha a (0..1): kept as given by tri_unlit_uv (0xFE at most: 0xFF means "by brightness")
static uint32_t with_alpha(uint32_t c, float a) { return (c & ~0xFFu) | (uint32_t)(clamp01f(a) * 254.0f); }

// ---- The Shackled Sun's face, looking down +z. Each part is built in its own frame: an eye's
// or the mouth's axis out of the ball (proc_mesh_face_frame), +z out, +y up ----

void proc_mesh_face_frame(float yaw, float pitch, float F[3][3])
{
    float z[3] = { sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch) };
    float x[3] = { cosf(yaw), 0.0f, -sinf(yaw) };
    float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
    memcpy(F[0], x, sizeof(x)); memcpy(F[1], y, sizeof(y)); memcpy(F[2], z, sizeof(z));
}

static const float F_ID[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

// Tangent-plane point (u, v) of frame F pushed out to radius r (mirrored in x: the left side)
static void fpt(const float F[3][3], float u, float v, float r, bool mir, float out[3])
{
    float l = r / sqrtf(u * u + v * v + 1.0f);
    for (int k = 0; k < 3; k++) out[k] = (F[0][k] * u + F[1][k] * v + F[2][k]) * l;
    if (mir) out[0] = -out[0];
}

static void norm3(float v[3]) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-6f) for (int k = 0; k < 3; k++) v[k] /= l; }

// A tube through n centres, each ring of `sides` in the plane of a (across) and the outward
// normal at the centre, a ring's half-widths wa (across) and wn (out); `cap` closes its end.
// Lit from the front (the face always looks at the player): its middle as bright as the glowing
// ball round it, darker toward its sides and underneath, so it stands out of the face
static void face_tube(const float (*c)[3], const float (*a)[3], const float *wa, const float *wn, int n, int sides, uint32_t col, bool cap)
{
    float P[8][8][3];
    uint32_t K[8][8];
    static const float L[3] = { -0.25f, 0.45f, 0.86f };
    for (int i = 0; i < n; i++) {
        float o[3] = { c[i][0], c[i][1], c[i][2] };
        norm3(o);
        for (int j = 0; j < sides; j++) {
            float ph = 2.0f * T3D_PI * j / sides, ca = cosf(ph), cn = sinf(ph), nn[3];
            for (int k = 0; k < 3; k++) { P[i][j][k] = c[i][k] + a[i][k] * ca * wa[i] + o[k] * cn * wn[i]; nn[k] = a[i][k] * ca + o[k] * cn; }
            float d = nn[0] * L[0] + nn[1] * L[1] + nn[2] * L[2];
            K[i][j] = shade(col, 0.74f + 0.3f * (d > 0.0f ? d : 0.0f));
        }
    }
    for (int i = 0; i + 1 < n; i++)
        for (int j = 0; j < sides; j++) {
            int k = (j + 1) % sides;
            tri_unlit3(P[i][j], P[i + 1][j], P[i + 1][k], K[i][j], K[i + 1][j], K[i + 1][k]);
            tri_unlit3(P[i][j], P[i + 1][k], P[i][k], K[i][j], K[i + 1][k], K[i][k]);
        }
    if (cap) {
        float e[3] = { 0, 0, 0 };
        for (int j = 0; j < sides; j++) for (int k = 0; k < 3; k++) e[k] += P[n - 1][j][k] / sides;
        uint32_t ce = shade(col, 0.8f);
        for (int j = 0; j < sides; j++) tri_unlit3(P[n - 1][j], P[n - 1][(j + 1) % sides], e, K[n - 1][j], K[n - 1][(j + 1) % sides], ce);
    }
}

// A soft-edged disc in frame F: colour col, alpha a0 at the middle fading to 0 at radius rad
static void face_disc_glow(const float F[3][3], float u0, float v0, float rad, float r, bool mir, uint32_t col, float a0)
{
    enum { SEG = 8 };
    float c[3], p[2][SEG][3];
    fpt(F, u0, v0, r, mir, c);
    for (int j = 0; j < SEG; j++) {
        float b = 2.0f * T3D_PI * j / SEG;
        fpt(F, u0 + cosf(b) * rad * 0.5f, v0 + sinf(b) * rad * 0.5f, r, mir, p[0][j]);
        fpt(F, u0 + cosf(b) * rad, v0 + sinf(b) * rad, r, mir, p[1][j]);
    }
    uint32_t cm = with_alpha(col, a0), ch = with_alpha(col, a0 * 0.6f), ce = with_alpha(col, 0.0f);
    for (int j = 0; j < SEG; j++) {
        int k = (j + 1) % SEG;
        tri_unlit3(c, p[0][j], p[0][k], cm, ch, ch);
        tri_unlit3(p[0][j], p[1][j], p[1][k], ch, ce, ce);
        tri_unlit3(p[0][j], p[1][k], p[0][k], ch, ce, ch);
    }
}

void proc_mesh_sun_face(float radius, bool hot)
{
    const float R = radius;
    proc_mesh_sun_ball(R, true, hot);
    float E[3][3], C[3][3], N[3][3];
    proc_mesh_face_frame(SUN_FACE_EYE_YAW, SUN_FACE_EYE_PITCH, E);
    proc_mesh_face_frame(SUN_FACE_CHEEK_YAW, SUN_FACE_CHEEK_PITCH, C);
    proc_mesh_face_frame(0.0f, 0.0f, N);
    for (int side = 0; side < 2; side++) {
        bool mir = side;
        // The eye's white: a lens, pale in the middle, shadowed toward its corners and lids
        enum { NU = 5 };
        const float W = SUN_FACE_EYE_W, H = SUN_FACE_EYE_H;
        static const int IN[3] = { 252, 242, 222 }, OUT[3] = { 222, 160, 105 }, IN_H[3] = { 255, 236, 228 }, OUT_H[3] = { 225, 130, 115 };
        float p[NU][3][3];
        uint32_t c[NU][3];
        for (int i = 0; i < NU; i++) {
            float u = -W + 2.0f * W * i / (NU - 1), h = H * powf(fmaxf(0.0f, 1.0f - (u / W) * (u / W)), 0.7f);
            for (int s = 0; s < 3; s++) {
                fpt(E, u, (s - 1) * h, R * 1.004f, mir, p[i][s]);
                c[i][s] = mix_rgb(hot ? IN_H : IN, hot ? OUT_H : OUT, 0.65f * (u / W) * (u / W) + 0.35f * (s != 1), 1.0f);
            }
        }
        for (int i = 0; i + 1 < NU; i++)
            for (int s = 0; s < 2; s++) {
                tri_unlit3(p[i][s], p[i + 1][s], p[i + 1][s + 1], c[i][s], c[i + 1][s], c[i + 1][s + 1]);
                tri_unlit3(p[i][s], p[i + 1][s + 1], p[i][s + 1], c[i][s], c[i + 1][s + 1], c[i][s + 1]);
            }
        // A red spiral on the cheek (under its blush, proc_mesh_face_blush)
        enum { SS = 14 };
        float sp[SS + 1][2][3];
        for (int i = 0; i <= SS; i++) {
            float t = (float)i / SS, a = t * 2.25f * 2.0f * T3D_PI, rr = 0.012f + 0.07f * t, w = 0.005f + 0.004f * t;
            fpt(C, cosf(a) * (rr - w), sinf(a) * (rr - w), R * 1.008f, mir, sp[i][0]);
            fpt(C, cosf(a) * (rr + w), sinf(a) * (rr + w), R * 1.008f, mir, sp[i][1]);
        }
        uint32_t sc = hot ? proc_rgb(190, 20, 30) : proc_rgb(200, 52, 22);
        for (int i = 0; i < SS; i++) {
            tri_unlit3(sp[i][0], sp[i + 1][0], sp[i + 1][1], sc, sc, sc);
            tri_unlit3(sp[i][0], sp[i + 1][1], sp[i][1], sc, sc, sc);
        }
        // The nose's wing on this side: a small dome
        float wc[3][3], wa[3][3], w1[3] = { 0.034f * R, 0.03f * R, 0.0f }, w2[3] = { 0.034f * R, 0.03f * R, 0.0f };
        for (int i = 0; i < 3; i++) {
            static const float OUTW[3] = { 0.0f, 0.03f, 0.05f };
            fpt(N, 0.052f, -0.16f, R * (1.0f + OUTW[i]), mir, wc[i]);
            float up[3]; fpt(N, 0.052f, -0.12f, R, mir, up);
            for (int k = 0; k < 3; k++) wa[i][k] = up[k] - wc[i][k];
            norm3(wa[i]);
        }
        face_tube((const float (*)[3])wc, (const float (*)[3])wa, w1, w2, 3, 5, hot ? proc_rgb(255, 190, 170) : proc_rgb(255, 200, 125), true);
    }
    // The mouth's inside where the shut lips cover it (their own carry it further as they part)
    {
        float Mf[3][3], q[7][2][3];
        proc_mesh_face_frame(0.0f, SUN_FACE_MOUTH_PITCH, Mf);
        for (int i = 0; i < 7; i++) {
            float u = SUN_FACE_MOUTH_W * 0.95f * (i - 3) / 3.0f, k = powf(fmaxf(0.0f, 1.0f - (u / SUN_FACE_MOUTH_W) * (u / SUN_FACE_MOUTH_W)), 0.6f);
            fpt(Mf, u, 0.03f * k, R * 1.002f, false, q[i][0]);
            fpt(Mf, u, -0.055f * k, R * 1.002f, false, q[i][1]);
        }
        uint32_t d = proc_rgb(35, 7, 3);
        for (int i = 0; i < 6; i++) { tri_unlit3(q[i][0], q[i + 1][0], q[i + 1][1], d, d, d); tri_unlit3(q[i][0], q[i + 1][1], q[i][1], d, d, d); }
    }
    // The nose: a ridge from between the eyes down to a rounded tip, sunk half into the ball
    enum { NR = 6 };
    static const float NV[NR] = { 0.05f, -0.01f, -0.07f, -0.12f, -0.155f, -0.18f };
    static const float NO[NR] = { -0.01f, 0.015f, 0.045f, 0.075f, 0.085f, 0.065f };
    static const float NW[NR] = { 0.035f, 0.042f, 0.05f, 0.065f, 0.062f, 0.03f };
    float nc[NR][3], na[NR][3], nwa[NR], nwn[NR];
    for (int i = 0; i < NR; i++) {
        fpt(N, 0.0f, NV[i], R * (1.0f + NO[i]), false, nc[i]);
        na[i][0] = 1.0f; na[i][1] = na[i][2] = 0.0f;
        nwa[i] = NW[i] * R; nwn[i] = NW[i] * R * 0.85f;
    }
    face_tube((const float (*)[3])nc, (const float (*)[3])na, nwa, nwn, NR, 5, hot ? proc_rgb(255, 200, 180) : proc_rgb(255, 212, 135), true);
}

void proc_mesh_face_blush(float radius, bool hot)
{
    float C[3][3];
    proc_mesh_face_frame(SUN_FACE_CHEEK_YAW, SUN_FACE_CHEEK_PITCH, C);
    for (int side = 0; side < 2; side++)
        face_disc_glow(C, 0.0f, 0.0f, 0.13f, radius * 1.005f, side, hot ? proc_rgb(235, 50, 50) : proc_rgb(240, 90, 50), 0.8f);
}

void proc_mesh_face_iris(float radius, bool hot)
{
    // Round, its rings blending from a dark rim to gold round the pupil; a glint at the top left
    enum { SEG = 12, NRI = 3 };
    static const float RR[NRI] = { SUN_FACE_IRIS_R, SUN_FACE_IRIS_R * 0.62f, 0.0f };
    static const int IR[NRI][3] = { { 85, 25, 8 }, { 220, 130, 40 }, { 255, 215, 120 } };
    static const int IR_H[NRI][3] = { { 90, 5, 10 }, { 230, 80, 60 }, { 255, 190, 160 } };
    const float rs = radius * 1.009f;
    for (int k = 0; k + 1 < NRI; k++)
        for (int j = 0; j < SEG; j++) {
            float b0 = 2.0f * T3D_PI * j / SEG, b1 = 2.0f * T3D_PI * (j + 1) / SEG, p[4][3];
            fpt(F_ID, RR[k] * cosf(b0), RR[k] * sinf(b0), rs, false, p[0]);
            fpt(F_ID, RR[k + 1] * cosf(b0), RR[k + 1] * sinf(b0), rs, false, p[1]);
            fpt(F_ID, RR[k + 1] * cosf(b1), RR[k + 1] * sinf(b1), rs, false, p[2]);
            fpt(F_ID, RR[k] * cosf(b1), RR[k] * sinf(b1), rs, false, p[3]);
            const int *c0 = (hot ? IR_H : IR)[k], *c1 = (hot ? IR_H : IR)[k + 1];
            uint32_t a = proc_rgb(c0[0], c0[1], c0[2]), b = proc_rgb(c1[0], c1[1], c1[2]);
            if (k + 2 < NRI) { tri_unlit3(p[0], p[1], p[2], a, b, b); tri_unlit3(p[0], p[2], p[3], a, b, a); }
            else tri_unlit3(p[0], p[1], p[3], a, b, a);
        }
    enum { GS = 5 };
    float g[3], gp[GS][3];
    fpt(F_ID, -0.03f, 0.035f, radius * 1.016f, false, g);
    for (int j = 0; j < GS; j++) fpt(F_ID, -0.03f + 0.016f * cosf(2.0f * T3D_PI * j / GS), 0.035f + 0.016f * sinf(2.0f * T3D_PI * j / GS), radius * 1.016f, false, gp[j]);
    uint32_t w = proc_rgb(255, 250, 238);
    for (int j = 0; j < GS; j++) tri_unlit3(g, gp[j], gp[(j + 1) % GS], w, w, w);
}

void proc_mesh_face_pupil(float radius)
{
    enum { SEG = 9 };
    float c[3], p[SEG][3];
    fpt(F_ID, 0.0f, 0.0f, radius * 1.013f, false, c);
    for (int j = 0; j < SEG; j++) fpt(F_ID, SUN_FACE_PUPIL_R * cosf(2.0f * T3D_PI * j / SEG), SUN_FACE_PUPIL_R * sinf(2.0f * T3D_PI * j / SEG), radius * 1.013f, false, p[j]);
    uint32_t m = proc_rgb(4, 2, 1), e = proc_rgb(28, 9, 3);
    for (int j = 0; j < SEG; j++) tri_unlit3(c, p[j], p[(j + 1) % SEG], m, e, e);
}

void proc_mesh_face_lid(float radius, bool hot)
{
    // The upper lid, shut: its edge on the eye's level line, a dark lash line along it, skin above.
    // Point (psi across, theta up over the ball) = (sin psi, cos psi sin theta, cos psi cos theta).
    // Drawn as a glow: opaque past the eye's corners and well over it, fading out into the ball
    enum { NP = 5, NT = 4 };
    static const float TH[NT] = { 0.0f, 0.035f, 0.18f, 0.3f };
    static const int LASH[3] = { 95, 25, 8 }, LASH_H[3] = { 95, 8, 15 };
    const float PSI = 0.29f, rs = radius * 1.02f;
    // skin as the ball's round the eyes (it always faces the player), the lid's edge a shade deeper
    uint32_t sk = sun_surface(cosf(0.4f), hot, 1.0f), sl = sun_surface(cosf(0.4f), hot, 0.9f);
    float p[NP][NT][3];
    uint32_t c[NP][NT];
    for (int i = 0; i < NP; i++) {
        float ps = -PSI + 2.0f * PSI * i / (NP - 1);
        for (int t = 0; t < NT; t++) {
            float th = TH[t];
            p[i][t][0] = sinf(ps) * rs; p[i][t][1] = cosf(ps) * sinf(th) * rs; p[i][t][2] = cosf(ps) * cosf(th) * rs;
            // the lash line only along the eye, fading to skin past its corners
            int s3[3] = { (int)(sk >> 24), (int)((sk >> 16) & 0xFF), (int)((sk >> 8) & 0xFF) };
            float lash = clamp01f((SUN_FACE_EYE_W + 0.01f - fabsf(ps)) / 0.05f);
            uint32_t col = t == 0 ? mix_rgb(s3, hot ? LASH_H : LASH, lash, 1.0f) : t == 1 ? sl : sk;
            float al = clamp01f((PSI - fabsf(ps)) / 0.06f) * clamp01f((0.3f - th) / 0.1f);
            c[i][t] = with_alpha(col, al);
        }
    }
    for (int i = 0; i + 1 < NP; i++)
        for (int t = 0; t + 1 < NT; t++) {
            tri_unlit3(p[i][t], p[i + 1][t], p[i + 1][t + 1], c[i][t], c[i + 1][t], c[i + 1][t + 1]);
            tri_unlit3(p[i][t], p[i + 1][t + 1], p[i][t + 1], c[i][t], c[i + 1][t + 1], c[i][t + 1]);
        }
}

void proc_mesh_face_brow(float radius)
{
    // An arched brow above the eye, thick at its inner end (-x, toward the nose), tapering out
    enum { NB = 6 };
    float p[NB][2][3];
    for (int i = 0; i < NB; i++) {
        float t = (float)i / (NB - 1), u = -0.16f + 0.4f * t, v = 0.19f + 0.04f * (1.0f - (2.0f * t - 1.1f) * (2.0f * t - 1.1f)), h = 0.006f + 0.02f * (1.0f - t);
        fpt(F_ID, u, v - h, radius * 1.03f, false, p[i][0]);
        fpt(F_ID, u, v + h, radius * 1.03f, false, p[i][1]);
    }
    uint32_t lo = proc_rgb(120, 40, 12), hi = proc_rgb(90, 25, 8);
    for (int i = 0; i + 1 < NB; i++) {
        tri_unlit3(p[i][0], p[i + 1][0], p[i + 1][1], lo, lo, hi);
        tri_unlit3(p[i][0], p[i + 1][1], p[i][1], lo, hi, hi);
    }
}

// The lips' curves (mouth frame, the right half, u from the middle out to W): centre height and thickness
static void lip_curve(bool upper, float u, float *v, float *t)
{
    const float W = SUN_FACE_MOUTH_W, k = 1.0f - powf(u / W, 1.6f), e = sqrtf(fmaxf(0.0f, 1.0f - (u / W) * (u / W)));
    if (upper) { *v = 0.02f * k - 0.009f * expf(-(u / 0.03f) * (u / 0.03f)); *t = 0.022f * e + 0.004f; }   // a cupid's bow
    else       { *v = -0.03f * k; *t = 0.03f * e + 0.004f; }
}

void proc_mesh_face_lip(float radius, bool upper, bool hot)
{
    // The right half of a lip, a lit tube along its curve (the middle ring open: its mirror meets
    // it), and behind it the mouth's dark inside reaching toward the other lip (seen when it
    // opens); the upper one carries the teeth
    enum { NK = 5 };
    const float R = radius, W = SUN_FACE_MOUTH_W;
    float c[NK][3], a[NK][3], wa[NK], wn[NK];
    for (int i = 0; i < NK; i++) {
        float u = W * i / (NK - 1), v, t;
        lip_curve(upper, u, &v, &t);
        fpt(F_ID, u, v, R * 1.012f, false, c[i]);
        // across: up the face, square to the curve
        float v2, t2; lip_curve(upper, fminf(W, u + 0.01f), &v2, &t2);
        float c2[3]; fpt(F_ID, u + 0.01f, v2, R * 1.012f, false, c2);
        float T[3] = { c2[0] - c[i][0], c2[1] - c[i][1], c2[2] - c[i][2] }, o[3] = { c[i][0], c[i][1], c[i][2] };
        norm3(T); norm3(o);
        a[i][0] = o[1] * T[2] - o[2] * T[1]; a[i][1] = o[2] * T[0] - o[0] * T[2]; a[i][2] = o[0] * T[1] - o[1] * T[0];
        norm3(a[i]);
        wa[i] = t * R; wn[i] = t * R * 0.8f;
    }
    face_tube((const float (*)[3])c, (const float (*)[3])a, wa, wn, NK, 4, upper ? (hot ? proc_rgb(200, 30, 40) : proc_rgb(205, 62, 32))
                                                                                : (hot ? proc_rgb(220, 45, 50) : proc_rgb(225, 80, 40)), false);
    // the inside of the mouth
    float in[NK][2][3];
    for (int i = 0; i < NK; i++) {
        float u = W * i / (NK - 1), v, t, k = 1.0f - (u / W) * (u / W);
        lip_curve(upper, u, &v, &t);
        float v0 = upper ? v - t * 0.5f : v + t * 0.5f, v1 = upper ? v0 - 0.045f * k : v0 + 0.045f * k;
        fpt(F_ID, u, v0, R * 1.003f, false, in[i][0]);
        fpt(F_ID, u, v1, R * 1.003f, false, in[i][1]);
    }
    uint32_t d0 = proc_rgb(60, 12, 5), d1 = proc_rgb(25, 5, 2);
    for (int i = 0; i + 1 < NK; i++) {
        tri_unlit3(in[i][0], in[i + 1][0], in[i + 1][1], d0, d0, d1);
        tri_unlit3(in[i][0], in[i + 1][1], in[i][1], d0, d1, d1);
    }
    if (!upper) return;
    // teeth: three a side, under the upper lip
    for (int k = 0; k < 3; k++) {
        float u0 = W * 0.22f * k + 0.003f, u1 = W * 0.22f * (k + 1) - 0.003f, v, t, q[4][3];
        lip_curve(true, (u0 + u1) * 0.5f, &v, &t);
        float top = v - t * 0.4f, bot = top - (0.03f - 0.004f * k);
        fpt(F_ID, u0, top, R * 1.004f, false, q[0]); fpt(F_ID, u1, top, R * 1.004f, false, q[1]);
        fpt(F_ID, u1, bot, R * 1.004f, false, q[2]); fpt(F_ID, u0, bot, R * 1.004f, false, q[3]);
        uint32_t ct = proc_rgb(250, 238, 212), cb = proc_rgb(222, 200, 168);
        tri_unlit3(q[0], q[1], q[2], ct, ct, cb);
        tri_unlit3(q[0], q[2], q[3], ct, cb, cb);
    }
}

void proc_mesh_sun_rays(float radius, bool wavy, bool hot)
{
    // A medieval sun's rays round a disc facing +z, hard-edged: straight gold spikes, each two-toned
    // down its crease, with a ring across them; or wavy red-orange flames between them
    const float R = radius;
    if (!wavy) {
        enum { NS = 12 };
        uint32_t lr = hot ? proc_rgb(240, 110, 80) : proc_rgb(240, 175, 60), lt = hot ? proc_rgb(255, 160, 120) : proc_rgb(255, 215, 110);
        uint32_t dr = hot ? proc_rgb(190, 50, 45) : proc_rgb(200, 130, 35), dt = hot ? proc_rgb(225, 90, 70) : proc_rgb(230, 165, 60);
        for (int k = 0; k < NS; k++) {
            float a = 2.0f * T3D_PI * k / NS, hw = 0.17f, r0 = R * 0.93f, r1 = R * ((k & 1) ? 1.45f : 1.62f);
            float L[3] = { cosf(a - hw) * r0, sinf(a - hw) * r0, 0 }, Rt[3] = { cosf(a + hw) * r0, sinf(a + hw) * r0, 0 };
            float C[3] = { cosf(a) * r0, sinf(a) * r0, 0 }, T[3] = { cosf(a) * r1, sinf(a) * r1, 0 };
            tri_unlit3(L, C, T, lr, lr, lt);
            tri_unlit3(C, Rt, T, dr, dr, dt);
        }
        enum { RS = 24 };
        uint32_t ri = hot ? proc_rgb(210, 50, 40) : proc_rgb(205, 80, 30), ro = hot ? proc_rgb(240, 110, 80) : proc_rgb(240, 175, 60);
        for (int j = 0; j < RS; j++) {
            float b0 = 2.0f * T3D_PI * j / RS, b1 = 2.0f * T3D_PI * (j + 1) / RS, r0 = R * 1.14f, r1 = R * 1.21f;
            float p[4][3] = { { cosf(b0) * r0, sinf(b0) * r0, 0.05f }, { cosf(b1) * r0, sinf(b1) * r0, 0.05f },
                              { cosf(b1) * r1, sinf(b1) * r1, 0.05f }, { cosf(b0) * r1, sinf(b0) * r1, 0.05f } };
            tri_unlit3(p[0], p[1], p[2], ri, ri, ro);
            tri_unlit3(p[0], p[2], p[3], ri, ro, ro);
        }
        return;
    }
    enum { NW = 12, ST = 5 };
    uint32_t root = hot ? proc_rgb(220, 40, 40) : proc_rgb(225, 85, 35), tip = hot ? proc_rgb(255, 120, 90) : proc_rgb(250, 150, 60);
    for (int k = 0; k < NW; k++) {
        float a = 2.0f * T3D_PI * (k + 0.5f) / NW, p[ST][2][3];
        uint32_t c[ST];
        for (int i = 0; i < ST; i++) {
            float t = (float)i / (ST - 1), r = R * (0.93f + 0.6f * t), ca = a + 0.07f * sinf(t * 3.0f * T3D_PI) * (0.3f + t);
            float hw = (0.08f * (1.0f - 0.75f * t) + 0.004f) * R / r;
            p[i][0][0] = cosf(ca - hw) * r; p[i][0][1] = sinf(ca - hw) * r; p[i][0][2] = -0.05f;
            p[i][1][0] = cosf(ca + hw) * r; p[i][1][1] = sinf(ca + hw) * r; p[i][1][2] = -0.05f;
            int a3[3] = { (int)(root >> 24), (int)((root >> 16) & 0xFF), (int)((root >> 8) & 0xFF) }, b3[3] = { (int)(tip >> 24), (int)((tip >> 16) & 0xFF), (int)((tip >> 8) & 0xFF) };
            c[i] = mix_rgb(a3, b3, t, 1.0f);
        }
        for (int i = 0; i + 1 < ST; i++) {
            tri_unlit3(p[i][0], p[i + 1][0], p[i + 1][1], c[i], c[i + 1], c[i + 1]);
            tri_unlit3(p[i][0], p[i + 1][1], p[i][1], c[i], c[i + 1], c[i]);
        }
    }
}

void proc_mesh_end(ProcMesh *out)
{
#ifdef HLE_LOG_HITCH
    uint32_t t0 = TICKS_READ();
#endif
    memset(out, 0, sizeof(*out));
    // Drop the triangles with two corners in one place (a tube's dome closes on a point): they
    // draw nothing, and the RSP would set each one up
    int kept = 0;
    for (int t = 0; t < s_ntris; t++) {
        const int16_t (*p)[3] = s_tris[t].p;
        if (!memcmp(p[0], p[1], 6) || !memcmp(p[1], p[2], 6) || !memcmp(p[0], p[2], 6)) continue;
        s_tris[kept++] = s_tris[t];
    }
    s_ntris = kept;
    if (s_ntris == 0) return;
    float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
    for (int t = 0; t < s_ntris; t++)
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < 3; k++) { float v = s_tris[t].p[i][k]; mn[k] = fminf(mn[k], v); mx[k] = fmaxf(mx[k], v); }
    for (int k = 0; k < 3; k++) out->bc[k] = 0.5f * (mn[k] + mx[k]);
    for (int t = 0; t < s_ntris; t++)
        for (int i = 0; i < 3; i++) {
            float d[3] = { s_tris[t].p[i][0] - out->bc[0], s_tris[t].p[i][1] - out->bc[1], s_tris[t].p[i][2] - out->bc[2] };
            out->br = fmaxf(out->br, sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
        }
    // Batches of up to a vertex cache's worth, the corners that match (position and colour)
    // shared: the RSP's vertex work bounds the frame, and a smooth tube shares each corner 6 ways
    int nc = s_ntris * 3;
    uint16_t *ref = malloc(sizeof(uint16_t) * nc);      // each batch's vertices: a corner each (t * 3 + i)
    uint8_t *idx = malloc(nc);                          // each corner's slot in its batch
    int *batchTri = malloc(sizeof(int) * (s_ntris + 1)), *batchRef = malloc(sizeof(int) * (s_ntris + 1));
    int batches = 0, nref = 0, nv = 0;
    for (int t = 0; t < s_ntris; t++) {
        int slot[3], fresh = 0;
        for (int i = 0; i < 3; i++) {
            slot[i] = -1;
            for (int k = 0; k < nv && slot[i] < 0; k++) {
                int r = ref[nref - nv + k];
                if (!memcmp(s_tris[r / 3].p[r % 3], s_tris[t].p[i], 6) && s_tris[r / 3].c[r % 3] == s_tris[t].c[i] && !memcmp(s_tris[r / 3].st[r % 3], s_tris[t].st[i], 4)) slot[i] = k;
            }
            if (slot[i] < 0) fresh++;
        }
        // the batch's index buffer is loaded into the free end of the vertex cache (36 bytes a slot)
        int nIdx = (t - (batches ? batchTri[batches - 1] : 0) + 1) * 3;
        if (t == 0 || nv + fresh + (nIdx * 2 + 8 + 35) / 36 > T3D_VERTEX_CACHE_SIZE) {   // a new batch
            batchTri[batches] = t; batchRef[batches++] = nref; nv = 0;
            for (int i = 0; i < 3; i++) slot[i] = -1;
        }
        for (int i = 0; i < 3; i++) {
            if (slot[i] < 0) { slot[i] = nv++; ref[nref++] = (uint16_t)(t * 3 + i); }
            idx[t * 3 + i] = (uint8_t)slot[i];
        }
    }
    batchTri[batches] = s_ntris; batchRef[batches] = nref;
    int packed = 0;     // each batch starts on a whole T3DVertPacked (two vertices)
    for (int b = 0; b < batches; b++) packed += (batchRef[b + 1] - batchRef[b] + 1) / 2;
    out->verts = malloc_uncached(sizeof(T3DVertPacked) * packed);
    memset(out->verts, 0, sizeof(T3DVertPacked) * packed);
    uint16_t norm = t3d_vert_pack_normal(&(T3DVec3){{ 0, 1, 0 }});
    // Each batch's triangles in one strip command (a restart on each: a list), not one command a
    // triangle: the RSP's command overhead per triangle was most of a mesh's cost
    int nidx = 0;
    for (int b = 0; b < batches; b++) nidx += ((batchTri[b + 1] - batchTri[b]) * 3 + 3) & ~3;   // 8-byte aligned each
    out->idx = malloc_uncached(sizeof(int16_t) * nidx);
    gpu_block_begin();
    for (int b = 0, base = 0, ib = 0; b < batches; b++) {
        int n = batchRef[b + 1] - batchRef[b];
        for (int k = 0; k < n; k++) {
            int r = ref[batchRef[b] + k];
            T3DVertPacked *v = &out->verts[base + k / 2];
            const int16_t *p = s_tris[r / 3].p[r % 3];
            if (k & 1) { memcpy(v->posB, p, 6); v->normB = norm; v->rgbaB = s_tris[r / 3].c[r % 3]; memcpy(v->stB, s_tris[r / 3].st[r % 3], 4); }
            else       { memcpy(v->posA, p, 6); v->normA = norm; v->rgbaA = s_tris[r / 3].c[r % 3]; memcpy(v->stA, s_tris[r / 3].st[r % 3], 4); }
        }
        t3d_vert_load(&out->verts[base], 0, (uint32_t)((n + 1) & ~1));
        int16_t *ix = &out->idx[ib];
        int ni = 0;
        for (int t = batchTri[b]; t < batchTri[b + 1]; t++) {
            ix[ni] = (int16_t)(idx[t * 3] | (ni ? 0x8000 : 0));
            ix[ni + 1] = idx[t * 3 + 1];
            ix[ni + 2] = idx[t * 3 + 2];
            ni += 3;
        }
        t3d_indexbuffer_convert(ix, ni);
        t3d_tri_draw_strip_and_sync(ix, ni);
        base += (n + 1) / 2;
        ib += (ni + 3) & ~3;
    }
#ifdef HLE_LOG_FPS
    debugf("[procmesh] %d tris, %d vertices (was %d)\n", s_ntris, nref, nc);   // test aid
#endif
    out->block = gpu_block_end();
    free(ref); free(idx); free(batchTri); free(batchRef);
    out->mat = malloc_uncached(sizeof(T3DMat4FP));
    proc_mesh_set_transform(out, 1.0f, (float[3]){ 0, 0, 0 }, (float[3]){ 0, 0, 0 });
#ifdef HLE_LOG_HITCH
    { static uint32_t sum; sum += TICKS_READ() - t0; debugf("[load] proc mesh %d tris: %lu us (%lu ms so far)\n", s_ntris, (unsigned long)TICKS_TO_US(TICKS_READ() - t0), (unsigned long)TICKS_TO_MS(sum)); }
#endif
}

void proc_mesh_set_transform(ProcMesh *m, const float scale, const float rot[3], const float pos[3])
{
    if (!m->mat) return;
    float s = scale / PM_UNIT;
    t3d_mat4fp_from_srt_euler(m->mat, (float[3]){ s, s, s }, rot, pos);
}

void proc_mesh_tex_begin(sprite_t *tex)
{
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
    rdpq_mode_filter(FILTER_BILINEAR);
    rdpq_mode_persp(true);
    rdpq_sprite_upload(TILE0, tex, &(rdpq_texparms_t){ .s.repeats = REPEAT_INFINITE, .t.repeats = REPEAT_INFINITE });
}

static bool s_batchLit;

void proc_mesh_batch_begin(bool glow, color_t tint, sprite_t *tex)
{
    bool tinted = color_to_packed32(tint) != 0xFFFFFFFF;
    rdpq_sync_pipe();
    rdpq_mode_push();       // restored at the end: the caller's fog and modes stay
    rdpq_set_mode_standard();
    rdpq_mode_zbuf(true, !glow);
    if (tex) proc_mesh_tex_begin(tex);
    else if (!tinted) rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    else { rdpq_mode_combiner(RDPQ_COMBINER1((SHADE, 0, PRIM, 0), (0, 0, 0, SHADE))); rdpq_set_prim_color(tint); }
    // Glow is blended by brightness, not added: the real RDP wraps an overflowing sum (the Sun's
    // corona tips speckled green on hardware; HLE renderers clamp, so it never showed there)
    if (glow) rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    // Textured, the combiner has no room for the tint: the ambient light carries it (the scene has
    // no other lights), lighting only while tinted
    s_batchLit = tex && tinted;
    if (s_batchLit) t3d_light_set_ambient((uint8_t[4]){ tint.r, tint.g, tint.b, 255 });
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | (tex ? T3D_FLAG_TEXTURED : 0) | (s_batchLit ? 0 : T3D_FLAG_NO_LIGHT));
}

void proc_mesh_batch_draw(const ProcMesh *m)
{
    if (!m->block) return;
    t3d_matrix_set(gpu_mat(m->mat), true);     // proc_mesh_set_transform may change it next frame
    rspq_block_run(m->block);
}

void proc_mesh_batch_end(void)
{
    if (s_batchLit) t3d_light_set_ambient((uint8_t[4]){ 255, 255, 255, 255 });
    rdpq_mode_pop();
}

// tint multiplies the vertex colours (white: as built), e.g. a boss glowing red before a strike
static void draw(const ProcMesh *m, bool glow, color_t tint, sprite_t *tex)
{
    if (!m->block) return;
    proc_mesh_batch_begin(glow, tint, tex);
    proc_mesh_batch_draw(m);
    proc_mesh_batch_end();
}

void proc_mesh_draw(const ProcMesh *m) { draw(m, false, RGBA32(255, 255, 255, 255), NULL); }
void proc_mesh_draw_glow(const ProcMesh *m) { draw(m, true, RGBA32(255, 255, 255, 255), NULL); }
void proc_mesh_draw_tinted(const ProcMesh *m, bool glow, color_t tint) { draw(m, glow, tint, NULL); }
void proc_mesh_draw_textured(const ProcMesh *m, sprite_t *tex, color_t tint) { draw(m, false, tint, tex); }

void proc_mesh_free(ProcMesh *m)
{
    if (m->block) { gpu_wait(); rspq_block_free(m->block); }
    if (m->verts) free_uncached(m->verts);
    if (m->idx) free_uncached(m->idx);
    if (m->mat) free_uncached(m->mat);
    memset(m, 0, sizeof(*m));
}
