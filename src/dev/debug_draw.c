#include "game/dragon_boss/dragon_boss.h"
#include "debug_draw.h"
#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>
#include <stdlib.h>  // abs()

#include "globals.h"
#include "game_math.h"
#include "display_utility.h"
#include "simple_collision_utility.h" 

#include <math.h> // for isfinite

#ifdef HLE_SHOW_HURTBOX
static uint8_t s_hbAlpha = 170;
#endif
uint16_t DEBUG_COLORS[6] = {
    0xf800, // Red     (#ff0000)
    0x0fe0, // Green   (#08ff00)
    0x001f, // Blue    (#0000ff)
    0xff80, // Yellow  (#fff300)
    0xf01f, // Magenta (#fa00ff)
    0xf8a5, // Orange  (#ffa500)
};

static void debug_draw_line(int px0, int py0, int px1, int py1, uint16_t color)
{
#ifdef HLE_SHOW_HURTBOX     // test aid: lines as RDP quads (2 px wide, translucent), so HLE plugins show them too
    {
        float dx = px1 - px0, dy = py1 - py0, l = sqrtf(dx * dx + dy * dy);
        if (l < 0.5f || abs(px0) > 2000 || abs(py0) > 2000 || abs(px1) > 2000 || abs(py1) > 2000) return;
        float nx = -dy / l, ny = dx / l;
        float a[2] = { px0 + nx, py0 + ny }, b[2] = { px1 + nx, py1 + ny }, c[2] = { px1 - nx, py1 - ny }, d[2] = { px0 - nx, py0 - ny };
        color_t col = color_from_packed16(color); col.a = s_hbAlpha;
        rdpq_set_prim_color(col);
        rdpq_triangle(&TRIFMT_FILL, a, b, c);
        rdpq_triangle(&TRIFMT_FILL, a, c, d);
        return;
    }
#endif
    uint16_t *fb = offscreenBuffer.buffer;
    if (!fb) return;

    uint32_t width = display_get_width();
    uint32_t height = display_get_height();
    if((px0 > width + 200) || (px1 > width + 200) ||
        (py0 > height + 200) || (py1 > height + 200)) {
        return;
    }

    float pos[2] = {px0, py0};
    int dx = px1 - px0;
    int dy = py1 - py0;
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if(steps <= 0)return;
    float xInc = dx / (float)steps;
    float yInc = dy / (float)steps;

    for(int i=0; i<steps; ++i)
    {
        if(pos[1] >= 0 && pos[1] < height && pos[0] >= 0 && pos[0] < width) {
        fb[(int)pos[1] * width + (int)pos[0]] = color;
        }
        pos[0] += xInc;
        pos[1] += yInc;
    }
}

static inline void debug_draw_line_vec3(const T3DVec3 *p0, const T3DVec3 *p1, uint16_t color)
{
    // must be finite before any comparisons/casts
    if (!isfinite(p0->v[0]) || !isfinite(p0->v[1]) || !isfinite(p0->v[2]) ||
        !isfinite(p1->v[0]) || !isfinite(p1->v[1]) || !isfinite(p1->v[2]))
        return;

    // draw only if both points are in a reasonable projected depth range
    if (p0->v[2] <= 0.0f || p0->v[2] >= 1.0f ||
        p1->v[2] <= 0.0f || p1->v[2] >= 1.0f)
        return;

    // avoid float->int overflow
    if (fabsf(p0->v[0]) > 1e6f || fabsf(p0->v[1]) > 1e6f ||
        fabsf(p1->v[0]) > 1e6f || fabsf(p1->v[1]) > 1e6f)
        return;

    debug_draw_line((int)p0->v[0], (int)p0->v[1], (int)p1->v[0], (int)p1->v[1], color);
}

void debug_draw_aabb(
    T3DViewport *vp,
    const T3DVec3 *min,
    const T3DVec3 *max,
    uint16_t color)
{
    T3DVec3 points[8];

    // Corner world positions (no fixed / MODEL_SCALE stuff)
    T3DVec3 p0 = *min;
    T3DVec3 p1 = {{ max->v[0], min->v[1], min->v[2] }};
    T3DVec3 p2 = {{ min->v[0], max->v[1], min->v[2] }};
    T3DVec3 p3 = {{ max->v[0], max->v[1], min->v[2] }};
    T3DVec3 p4 = {{ min->v[0], max->v[1], max->v[2] }};
    T3DVec3 p5 = {{ max->v[0], max->v[1], max->v[2] }};
    T3DVec3 p6 = {{ min->v[0], min->v[1], max->v[2] }};
    T3DVec3 p7 = {{ max->v[0], min->v[1], max->v[2] }};

    // Project to view space
    t3d_viewport_calc_viewspace_pos(vp, &points[0], &p0);
    t3d_viewport_calc_viewspace_pos(vp, &points[1], &p1);
    t3d_viewport_calc_viewspace_pos(vp, &points[2], &p2);
    t3d_viewport_calc_viewspace_pos(vp, &points[3], &p3);
    t3d_viewport_calc_viewspace_pos(vp, &points[4], &p4);
    t3d_viewport_calc_viewspace_pos(vp, &points[5], &p5);
    t3d_viewport_calc_viewspace_pos(vp, &points[6], &p6);
    t3d_viewport_calc_viewspace_pos(vp, &points[7], &p7);

    const int indices[24] = {
        0, 1, 1, 3, 3, 2, 2, 0,
        4, 5, 5, 7, 7, 6, 6, 4,
        0, 6, 1, 7, 2, 4, 3, 5
    };

    for (int i = 0; i < 24; i += 2)
        debug_draw_line_vec3(&points[indices[i]], &points[indices[i+1]], color);
}

void debug_draw_circle(T3DViewport *vp, const T3DVec3 *center, float radius, const T3DVec3 *normal, uint16_t color)
{
    // Generate orthonormal basis (u, v) in circle's plane
    T3DVec3 u = {{1, 0, 0}};
    if (fabsf(normal->v[0]) > 0.9f) u.v[1] = 1, u.v[0] = 0;

    // u = normalize(cross(normal, u))
    T3DVec3 u_cross = {{
        normal->v[1]*u.v[2] - normal->v[2]*u.v[1],
        normal->v[2]*u.v[0] - normal->v[0]*u.v[2],
        normal->v[0]*u.v[1] - normal->v[1]*u.v[0]
    }};
    float len = sqrtf(u_cross.v[0]*u_cross.v[0] + u_cross.v[1]*u_cross.v[1] + u_cross.v[2]*u_cross.v[2]);
    if (len < 1e-6f) return; // Avoid division by zero
    for (int i = 0; i < 3; i++) u.v[i] = u_cross.v[i] / len;

    // v = cross(normal, u)
    T3DVec3 v = {{
        normal->v[1]*u.v[2] - normal->v[2]*u.v[1],
        normal->v[2]*u.v[0] - normal->v[0]*u.v[2],
        normal->v[0]*u.v[1] - normal->v[1]*u.v[0]
    }};

    const int segments = 32;
    for (int i = 0; i < segments; ++i)
    {
        float angle0 = (float)i / segments * 2.0f * T3D_PI;
        float angle1 = (float)(i + 1) / segments * 2.0f * T3D_PI;

        T3DVec3 p0, p1, sp0, sp1;
        for (int j = 0; j < 3; ++j) {
            p0.v[j] = center->v[j] + radius * (cosf(angle0) * u.v[j] + sinf(angle0) * v.v[j]);
            p1.v[j] = center->v[j] + radius * (cosf(angle1) * u.v[j] + sinf(angle1) * v.v[j]);
        }

        t3d_viewport_calc_viewspace_pos(vp, &sp0, &p0);
        t3d_viewport_calc_viewspace_pos(vp, &sp1, &p1);
        debug_draw_line_vec3(&sp0, &sp1, color);
    }
}

void debug_draw_sphere(T3DViewport *vp, const T3DVec3 *center, float radius, uint16_t color)
{
    rspq_wait();
    // uint16_t *fb = offscreenBuffer.buffer;

    T3DVec3 up = {{0, 1, 0}};     // Y is up
    T3DVec3 right = {{1, 0, 0}};  // Horizontal ring
    T3DVec3 forward = {{0, 0, 1}}; // Forward direction
    debug_draw_circle(vp, center, radius, &up, color);     // XZ plane
    debug_draw_circle(vp, center, radius, &right, color);  // YZ plane
    debug_draw_circle(vp, center, radius, &forward, color); // XY plane
}

void debug_draw_cross(T3DViewport *vp, const T3DVec3 *center, float half_length, uint16_t color)
{
    // X axis
    T3DVec3 p_x0 = *center, p_x1 = *center;
    p_x0.v[0] -= half_length;
    p_x1.v[0] += half_length;

    // Y axis
    T3DVec3 p_y0 = *center, p_y1 = *center;
    p_y0.v[1] -= half_length;
    p_y1.v[1] += half_length;

    // Z axis
    T3DVec3 p_z0 = *center, p_z1 = *center;
    p_z0.v[2] -= half_length;
    p_z1.v[2] += half_length;

    // Project and draw
    T3DVec3 sp0, sp1;
    t3d_viewport_calc_viewspace_pos(vp, &sp0, &p_x0);
    t3d_viewport_calc_viewspace_pos(vp, &sp1, &p_x1);
    debug_draw_line_vec3(&sp0, &sp1, color);

    t3d_viewport_calc_viewspace_pos(vp, &sp0, &p_y0);
    t3d_viewport_calc_viewspace_pos(vp, &sp1, &p_y1);
    debug_draw_line_vec3(&sp0, &sp1, color);

    t3d_viewport_calc_viewspace_pos(vp, &sp0, &p_z0);
    t3d_viewport_calc_viewspace_pos(vp, &sp1, &p_z1);
    debug_draw_line_vec3(&sp0, &sp1, color);
}

void debug_draw_dot(T3DViewport *vp, const T3DVec3 *center, float radius, uint16_t color)
{
    // Small sphere as a dot
    debug_draw_sphere(vp, center, radius, color);
}

void debug_draw_tri_wire(
    T3DViewport *vp,
    const T3DVec3 *p0,
    const T3DVec3 *p1,
    const T3DVec3 *p2,
    uint16_t color)
{
    // Project world-space vertices to viewspace
    T3DVec3 sp0, sp1, sp2;
    t3d_viewport_calc_viewspace_pos(vp, &sp0, p0);
    t3d_viewport_calc_viewspace_pos(vp, &sp1, p1);
    t3d_viewport_calc_viewspace_pos(vp, &sp2, p2);
    
    // Draw three lines to form a triangle wireframe
    debug_draw_line_vec3(&sp0, &sp1, color);
    debug_draw_line_vec3(&sp1, &sp2, color);
    debug_draw_line_vec3(&sp2, &sp0, color);
}

void debug_draw_capsule(
    T3DViewport *vp,
    const T3DVec3 *a,
    const T3DVec3 *b,
    float radius,
    uint16_t color)
{
    // Draw end-cap spheres
    debug_draw_sphere(vp, a, radius, color);
    debug_draw_sphere(vp, b, radius, color);

    // Connect ends with a line
    T3DVec3 sp0, sp1;
    t3d_viewport_calc_viewspace_pos(vp, &sp0, a);
    t3d_viewport_calc_viewspace_pos(vp, &sp1, b);
    debug_draw_line_vec3(&sp0, &sp1, color);
}

void debug_draw_capsule_vs_aabb_list(
    T3DViewport   *vp,
    const T3DVec3 *capA,
    const T3DVec3 *capB,
    float          capRadius,
    const AABB    *aabbs,
    int            aabbCount,
    uint16_t       colorNoHit,
    uint16_t       colorHit)
{
    float capAF[3] = { capA->v[0], capA->v[1], capA->v[2] };
    float capBF[3] = { capB->v[0], capB->v[1], capB->v[2] };

    bool anyHit = false;

    for (int i = 0; i < aabbCount; ++i)
    {
        const AABB *box = &aabbs[i];

        float rectMin[3] = {
            box->min.v[0],
            box->min.v[1],
            box->min.v[2]
        };

        float rectMax[3] = {
            box->max.v[0],
            box->max.v[1],
            box->max.v[2]
        };

        bool hit = scu_capsule_vs_rect_f(
            capAF, capBF, capRadius,
            rectMin, rectMax);

        if (hit) anyHit = true;

        uint16_t c = hit ? colorHit : colorNoHit;
        debug_draw_aabb(vp, &box->min, &box->max, c);
    }

    uint16_t capColor = anyHit ? colorHit : colorNoHit;
    debug_draw_capsule(vp, capA, capB, capRadius, capColor);

    if (anyHit)
    {
        T3DVec3 mid = {{
            0.5f * (capA->v[0] + capB->v[0]),
            0.5f * (capA->v[1] + capB->v[1]),
            0.5f * (capA->v[2] + capB->v[2]),
        }};
        debug_draw_cross(vp, &mid, capRadius, capColor);
    }
}

#ifdef HLE_SHOW_HURTBOX
#include "character.h"
#include "game/boss/boss.h"
#include "camera_controller.h"
int hle_hb_frame;     // the autoplay frame, shown as a barcode (bottom left) to match shots to logs
// to the screen, unless it is behind or right at the camera (there the projection wraps round)
static bool hb_proj(T3DViewport *vp, const float w[3], T3DVec3 *out)
{
    float f[3], d = 0.0f, fl = 0.0f;
    for (int m = 0; m < 3; m++) { f[m] = customCamTarget.v[m] - customCamPos.v[m]; fl += f[m] * f[m]; }
    fl = sqrtf(fl) + 1e-6f;
    for (int m = 0; m < 3; m++) d += (w[m] - customCamPos.v[m]) * f[m] / fl;
    if (d < 4.0f) return false;
    T3DVec3 p = {{ w[0], w[1], w[2] }};
    t3d_viewport_calc_viewspace_pos(vp, out, &p);
    return out->v[2] > 0.0f && out->v[2] < 1.0f;
}
static void hb_line(const T3DVec3 *a, const T3DVec3 *b, uint16_t c) { debug_draw_line((int)a->v[0], (int)a->v[1], (int)b->v[0], (int)b->v[1], c); }
// A capsule seen from the test camera (customCamPos): its outline in the plane through the axis
// facing the camera (convex: a half circle round each end), filled translucent, then stroked,
// plus a ring round each end of the axis
static void hb_capsule(T3DViewport *vp, const T3DVec3 *a, const T3DVec3 *b, float r, color_t col, bool rings)
{
    float v[3], u[3], sd[3], w[3];
    for (int m = 0; m < 3; m++) { v[m] = (a->v[m] + b->v[m]) * 0.5f - customCamPos.v[m]; u[m] = b->v[m] - a->v[m]; }
    float vl = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) + 1e-6f;
    for (int m = 0; m < 3; m++) v[m] /= vl;
    float ud = u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
    for (int m = 0; m < 3; m++) w[m] = u[m] - ud * v[m];     // the axis across the view
    float wl = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (wl < 1e-3f) { w[0] = 0; w[1] = 1; w[2] = 0; wl = 1; }   // end on (or a ball): any way up
    for (int m = 0; m < 3; m++) w[m] /= wl;
    sd[0] = w[1] * v[2] - w[2] * v[1]; sd[1] = w[2] * v[0] - w[0] * v[2]; sd[2] = w[0] * v[1] - w[1] * v[0];
    enum { H = 6, N = 2 * H + 2, NR = 12 };     // few: past ~1000 triangles the HLE frame splits in two tasks (the scene went black)
    T3DVec3 sp[N]; float cx = 0.0f, cy = 0.0f;
    for (int i = 0; i < N; i++) {
        const T3DVec3 *c = i <= H ? b : a;
        float t = T3D_PI * (float)(i <= H ? i : i - 1) / H;     // 0..pi round B, pi..2pi round A
        float p[3];
        for (int m = 0; m < 3; m++) p[m] = c->v[m] + r * (cosf(t) * sd[m] + sinf(t) * w[m]);
        if (!hb_proj(vp, p, &sp[i])) return;
        cx += sp[i].v[0] / N; cy += sp[i].v[1] / N;
    }
    color_t f = col; f.a = 60;
    rdpq_set_prim_color(f);
    for (int i = 0; i < N; i++) {
        float p0[2] = { cx, cy }, p1[2] = { sp[i].v[0], sp[i].v[1] }, p2[2] = { sp[(i + 1) % N].v[0], sp[(i + 1) % N].v[1] };
        rdpq_triangle(&TRIFMT_FILL, p0, p1, p2);
    }
    uint16_t c16 = color_to_packed16(col);
    s_hbAlpha = 230;
    for (int i = 0; i < N; i++) hb_line(&sp[i], &sp[(i + 1) % N], c16);
    s_hbAlpha = 140;
    // rings round the axis at both ends: e1, e2 across the axis
    float ul = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    float un[3] = { 0, 1, 0 };
    if (ul > 1e-3f) for (int m = 0; m < 3; m++) un[m] = u[m] / ul;
    float e1[3] = { 1, 0, 0 };
    if (fabsf(un[0]) > 0.9f) { e1[0] = 0; e1[2] = 1; }
    float e2[3] = { un[1] * e1[2] - un[2] * e1[1], un[2] * e1[0] - un[0] * e1[2], un[0] * e1[1] - un[1] * e1[0] };
    float l2 = sqrtf(e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2]);
    for (int m = 0; m < 3; m++) e2[m] /= l2;
    e1[0] = e2[1] * un[2] - e2[2] * un[1]; e1[1] = e2[2] * un[0] - e2[0] * un[2]; e1[2] = e2[0] * un[1] - e2[1] * un[0];
    for (int e = 0; e < (rings ? 2 : 0); e++) {
        const T3DVec3 *c = e ? b : a;
        T3DVec3 q0, q1; bool ok0 = false;
        for (int i = 0; i <= NR; i++) {
            float t = 2.0f * T3D_PI * i / NR, p[3];
            for (int m = 0; m < 3; m++) p[m] = c->v[m] + r * (cosf(t) * e1[m] + sinf(t) * e2[m]);
            bool ok = hb_proj(vp, p, &q1);
            if (ok && ok0) hb_line(&q0, &q1, c16);
            q0 = q1; ok0 = ok;
        }
    }
    { T3DVec3 p0, p1; if (hb_proj(vp, a->v, &p0) && hb_proj(vp, b->v, &p1)) hb_line(&p0, &p1, c16); }
}

#ifdef HLE_HB_PROPOSALS
// Test aid: a boss's attack volume while it can hit, yellow: on even frames today's, on odd ones the
// proposal (shots of two frames in a row: before | after). Returns true if it drew one (the
// boss's own hurtboxes are left out then)
static void hb_hand(const Boss *b, const float l[3], float w[3])
{
    const T3DMat4FP *B = &((T3DSkeleton *)b->skeleton)->boneMatricesFP[b->handRightBoneIndex];
    float m[3]; mat4fp_mul_point_f32_row3_colbasis(B, l, m); mat4fp_mul_point_f32_row3_colbasis((const T3DMat4FP *)b->modelMat, m, w);
}
static void hb_seg(T3DViewport *vp, const float a[3], const float b[3], float r, color_t c)
{
    T3DVec3 p = {{ a[0], a[1], a[2] }}, q = {{ b[0], b[1], b[2] }};
    hb_capsule(vp, &p, &q, r, c, false);
}
static void hb_disc(T3DViewport *vp, const float c[3], float r, float h, color_t col)     // a flat cylinder r round, h high
{
    uint16_t c16 = color_to_packed16(col); s_hbAlpha = 230;
    for (int e = 0; e < 2; e++) { T3DVec3 q0, q1; bool ok0 = false;
        for (int i = 0; i <= 24; i++) { float t = 6.2832f * i / 24, p[3] = { c[0] + r * cosf(t), c[1] + e * h, c[2] + r * sinf(t) };
            bool ok = hb_proj(vp, p, &q1); if (ok && ok0) hb_line(&q0, &q1, c16); q0 = q1; ok0 = ok; } }
    s_hbAlpha = 140;
}
static void hb_drawn_blade(T3DViewport *vp, const Boss *b, color_t c)
{
    static const float L[6][2][3] = { { { -112, 34, -24 }, { -751, 15, -80 } }, { { -111, 1, -21 }, { -595, -22, -63 } }, { { -113, 66, -27 }, { -599, 52, -70 } },
                                      { { -595, -22, -63 }, { -781, -74, -76 } }, { { -599, 52, -70 }, { -724, 95, -85 } }, { { -724, 95, -85 }, { -781, -74, -76 } } };
    for (int i = 0; i < 6; i++) { float p[3], q[3]; hb_hand(b, L[i][0], p); hb_hand(b, L[i][1], q); hb_seg(vp, p, q, 0.8f, c); }
}
#include "game_time.h"
#include "simple_collision_utility.h"
static void hb_freeze(bool now, const char *what, int id)     // the moment of contact held still 4 s (real) for the shots, the first 3 times
{
    static int n, last = -10000;
#ifdef HLE_HB_ONLY
    if (id != HLE_HB_ONLY) return;
#endif
    extern int hle_hb_hold; hle_hb_hold = hle_hb_frame - last < 240;
    if (hle_hb_hold) { game_time_clear_effects(); game_time_slowmo(0.1f, 0.0001f); return; }     // held (a hit's own slow motion would end it)
    if (!now || n >= 3 || hle_hb_frame - last < 600) return;
    n++; last = hle_hb_frame; hle_hb_hold = 1; game_time_clear_effects(); game_time_slowmo(0.1f, 0.0001f);
    debugf("[hbfreeze] f%d %s\n", hle_hb_frame, what);
}
static bool hb_proposals(T3DViewport *vp, Boss *b)
{
    extern bool bossWeaponCollision;
    float pa[3] = { character.pos[0] + character.capsuleCollider.localCapA.v[0], character.pos[1] + character.capsuleCollider.localCapA.v[1], character.pos[2] + character.capsuleCollider.localCapA.v[2] };
    float pb[3] = { character.pos[0] + character.capsuleCollider.localCapB.v[0], character.pos[1] + character.capsuleCollider.localCapB.v[1], character.pos[2] + character.capsuleCollider.localCapB.v[2] };
    bool prop = hle_hb_frame & 1;
    color_t Y = RGBA32(255, 225, 0, 255);
    const CapsuleCollider *k = &b->capsuleCollider;
    float ba[3] = { b->pos[0] + k->localCapA.v[0], b->pos[1] + k->localCapA.v[1], b->pos[2] + k->localCapA.v[2] };
    float bb[3] = { b->pos[0] + k->localCapB.v[0], b->pos[1] + k->localCapB.v[1], b->pos[2] + k->localCapB.v[2] };
    if (b->phaseIndex <= 2 && b->skeleton && b->handRightBoneIndex >= 0) {
        if (b->state == BOSS_STATE_FLIP_ATTACK && b->sphereAttackColliderActive) {     // (c) the burst
            float c[3] = { b->pos[0] - cosf(b->rot[1]) * 40.0f, b->pos[1], b->pos[2] - sinf(b->rot[1]) * 40.0f };
            hb_freeze(sqrtf((character.pos[0] - c[0]) * (character.pos[0] - c[0]) + (character.pos[2] - c[2]) * (character.pos[2] - c[2])) <= 20.0f, "flip burst", 3);
            hb_freeze(true, "flip burst live", 7);
            if (!prop) hb_disc(vp, c, 20.0f, 60.0f, Y);
            else { hb_seg(vp, ba, bb, k->radius, Y); hb_drawn_blade(vp, b, Y); }
            return true;
        }
        if (b->handAttackColliderActive) {
            if (b->state == BOSS_STATE_COMBO_LUNGE) { hb_freeze(scu_capsule_vs_capsule_f(ba, bb, k->radius, pa, pb, character.capsuleCollider.radius), "lunge body", 2); hb_drawn_blade(vp, b, Y); if (prop) hb_seg(vp, ba, bb, k->radius, Y); }     // (b)
            else if ((hb_freeze(bossWeaponCollision, "blade", 1), !prop)) { float p[3], q[3]; hb_hand(b, (float[3]){ 0, 0, 0 }, p); hb_hand(b, (float[3]){ -640, 0, 0 }, q); hb_seg(vp, p, q, 5.0f, Y); }     // (a)
            else hb_drawn_blade(vp, b, Y);
            return true;
        }
    } else if (b->phaseIndex == DRAGON_BOSS_PHASE) {     // (d) the charge
        extern int hle_dragon_charge_vols(float a[][3], float b[][3], float r[], float *extra);
        float a[16][3], q[16][3], r[16], ex; int n = hle_dragon_charge_vols(a, q, r, &ex);
        { float xa[3] = { character.pos[0], character.pos[1] + 4.0f, character.pos[2] }, xb[3] = { character.pos[0], character.pos[1] + 16.0f, character.pos[2] }; bool t = false;
          for (int i = 0; i < n; i++) { t |= scu_capsule_vs_capsule_f(a[i], q[i], r[i] + ex, xa, xb, 6.0f); }
          hb_freeze(t, "charge shove", 4); }
        for (int i = 0; i < n; i++) hb_seg(vp, a[i], q[i], r[i] + (prop ? 0.0f : ex), Y);
        extern float dragon_boss_push_radius(void);     // the circle its body pushes the player out to (white: the hit capsule's, or the rush's)
        if (n > 0) { float c[3] = { b->pos[0], b->pos[1], b->pos[2] }, pr = dragon_boss_push_radius(); if (pr >= 0.0f) hb_disc(vp, c, pr > 0.0f ? pr : k->radius, 2.0f, RGBA32(255, 255, 255, 255)); }     // (none: no push yet)
        return n > 0;
    } else if (b->phaseIndex == 3) {     // (e) the Fallen Warrior: the Triangle Slam's area (kept)
        extern bool hle_fk_tri(float t[3][2], float *y, int *step); float t[3][2], y; int st;
        if (hle_fk_tri(t, &y, &st)) { hb_freeze(st == 2, "triangle slam", 5); for (int i = 0; i < 3; i++) { float p[3] = { t[i][0], y, t[i][1] }, q[3] = { t[(i + 1) % 3][0], y, t[(i + 1) % 3][1] }; hb_seg(vp, p, q, 6.0f, Y); } return true; }
    } else if (b->phaseIndex == 4) {     // (e) the Sun: the dive's landing (kept)
        extern bool hle_sun_dive(float c[3], bool *landed); float c[3]; bool ld;
        if (hle_sun_dive(c, &ld)) { hb_freeze(ld, "dive landing", 6); hb_disc(vp, c, 34.0f, 6.0f, Y); return false; }     // (the ball's capsule too)
    }
    return false;
}
#endif
// Test aid: the player's hit capsule (green), the boss's (magenta) and the Guardian's limbs (orange) over the frame, as the hit
// checks build them (pos + local ends, no scale)
void hle_hurtbox_draw(T3DViewport *vp)
{
    rdpq_sync_pipe();
    rdpq_mode_push();     // the frame's own mode back after (the next frame's drawing went black without it)
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    Boss *boss = boss_get_instance();
    if (boss) {
        const CapsuleCollider *k = &boss->capsuleCollider;
        T3DVec3 ba = {{ boss->pos[0] + k->localCapA.v[0], boss->pos[1] + k->localCapA.v[1], boss->pos[2] + k->localCapA.v[2] }};
        T3DVec3 bb = {{ boss->pos[0] + k->localCapB.v[0], boss->pos[1] + k->localCapB.v[1], boss->pos[2] + k->localCapB.v[2] }};
#ifdef HLE_HB_PROPOSALS
        if (hb_proposals(vp, boss)) goto player;     // (the boss's attack volumes only: few triangles)
#endif
        hb_capsule(vp, &ba, &bb, k->radius, RGBA32(255, 40, 220, 255), true);
        // the Guardian's (and Unbound's) limbs, which the sword also meets (collision_system.c)
        extern int hle_guardian_limbs(T3DVec3 p[][2], float r[]);
        T3DVec3 lp[8][2]; float lr[8];
        int nl = hle_guardian_limbs(lp, lr);
        for (int i = 0; i < nl; i++) hb_capsule(vp, &lp[i][0], &lp[i][1], lr[i], RGBA32(255, 150, 30, 255), false);
        // the Guardian's and Unbound's blade, while it can hit (cyan: its hit segment)
        if (boss->handAttackColliderActive) {
            extern T3DVec3 bossWeaponCapA, bossWeaponCapB; extern float bossWeaponRadius;
            hb_capsule(vp, &bossWeaponCapA, &bossWeaponCapB, bossWeaponRadius, RGBA32(40, 230, 255, 255), false);
        }
        // Shadowborne's tail, its second hit volume (the sword and the skills)
        if (boss->phaseIndex == DRAGON_BOSS_PHASE) {
            float ta[DRAGON_TAIL_VOLUMES][3], tb[DRAGON_TAIL_VOLUMES][3], tr[DRAGON_TAIL_VOLUMES];
            int nt = dragon_boss_tail_volumes(ta, tb, tr);
            for (int i = 0; i < nt; i++) { T3DVec3 p0 = {{ ta[i][0], ta[i][1], ta[i][2] }}, p1 = {{ tb[i][0], tb[i][1], tb[i][2] }}; hb_capsule(vp, &p0, &p1, tr[i], RGBA32(255, 150, 30, 255), false); }
        }
    }
#ifdef HLE_HB_PROPOSALS
player:
#endif
    const CapsuleCollider *c = &character.capsuleCollider;
    T3DVec3 a = {{ character.pos[0] + c->localCapA.v[0], character.pos[1] + c->localCapA.v[1], character.pos[2] + c->localCapA.v[2] }};
    T3DVec3 b = {{ character.pos[0] + c->localCapB.v[0], character.pos[1] + c->localCapB.v[1], character.pos[2] + c->localCapB.v[2] }};
    hb_capsule(vp, &a, &b, c->radius, RGBA32(40, 255, 60, 255), true);
    // the frame as 14 bits, 4 px squares from the bottom left (white 1)
    for (int i = 0; i < 14; i++) {
        if (!((hle_hb_frame >> i) & 1)) continue;
        rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
        float x = 2.0f + i * 5.0f, y = 232.0f, q0[2] = { x, y }, q1[2] = { x + 4, y }, q2[2] = { x + 4, y + 4 }, q3[2] = { x, y + 4 };
        rdpq_triangle(&TRIFMT_FILL, q0, q1, q2);
        rdpq_triangle(&TRIFMT_FILL, q0, q2, q3);
    }
    rdpq_mode_pop();
    rdpq_sync_pipe();
}
#endif
