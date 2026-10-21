#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>
#include <math.h>
#include <string.h>

#include "skill_fx.h"
#include "blood_particles_fx.h"
#include "screen_shake.h"
#include "game/boss/boss.h"
#include "controllers/audio_controller.h"
#include "../scenes/guardian/guardian_scene_sfx.h"
#include "utilities/general_utility.h"
#include "character/character.h"

#define SKILL_FX_MAX 24

typedef enum { FX_NONE, FX_ARC, FX_WAVE, FX_PILLAR, FX_BURST } FxType;

typedef struct {
    FxType type;
    SkillWaveShape shape;
    float pos[3], vel[3];
    float yaw;
    float age, life;
    float radius;           // arc: final radius
    float damage;
    int   hitsLeft;
    float hitInterval, hitTimer;
    bool  pillarOnEnd;
    bool  hostile;          // hits the player (Fallen Knight) instead of the boss
} SkillFx;

static bool s_spawnHostile;     // palette/target for effects spawned now

static SkillFx s_fx[SKILL_FX_MAX];

// ---- Spawning ----

static SkillFx* fx_alloc(FxType type)
{
    for (int i = 0; i < SKILL_FX_MAX; i++) {
        if (s_fx[i].type == FX_NONE) {
            memset(&s_fx[i], 0, sizeof(s_fx[i]));
            s_fx[i].type = type;
            s_fx[i].hostile = s_spawnHostile;
            return &s_fx[i];
        }
    }
    return NULL;    // pool full: drop the effect
}

void skill_fx_init(void) { skill_fx_reset(); }
void skill_fx_reset(void) { memset(s_fx, 0, sizeof(s_fx)); }
void skill_fx_clear_hostile(void) { for (int i = 0; i < SKILL_FX_MAX; i++) if (s_fx[i].hostile) s_fx[i].type = FX_NONE; }

void skill_fx_spawn_arc(const float pos[3], float yaw, float radius, float damage)
{
    SkillFx *f = fx_alloc(FX_ARC);
    if (!f) return;
    memcpy(f->pos, pos, sizeof(f->pos));
    f->yaw = yaw;
    f->radius = radius;
    f->damage = damage;
    f->hitsLeft = 1;
    f->life = 0.55f;
}

void skill_fx_spawn_wave(SkillWaveShape shape, const float pos[3], const float vel[3],
                         float damage, int hits, float hitInterval, float life, bool pillarOnEnd)
{
    SkillFx *f = fx_alloc(FX_WAVE);
    if (!f) return;
    f->shape = shape;
    memcpy(f->pos, pos, sizeof(f->pos));
    memcpy(f->vel, vel, sizeof(f->vel));
    f->damage = damage;
    f->hitsLeft = hits;
    f->hitInterval = hitInterval;
    f->life = life;
    f->pillarOnEnd = pillarOnEnd;
}

void skill_fx_spawn_burst_hostile(const float pos[3], float yaw)
{
    s_spawnHostile = true;
    skill_fx_spawn_burst(pos, yaw);
    s_spawnHostile = false;
}

void skill_fx_spawn_arc_hostile(const float pos[3], float yaw, float radius, float damage)
{
    s_spawnHostile = true;
    skill_fx_spawn_arc(pos, yaw, radius, damage);
    s_spawnHostile = false;
}

void skill_fx_spawn_wave_hostile(SkillWaveShape shape, const float pos[3], const float vel[3],
                                 float damage, int hits, float hitInterval, float life, bool pillarOnEnd)
{
    s_spawnHostile = true;
    skill_fx_spawn_wave(shape, pos, vel, damage, hits, hitInterval, life, pillarOnEnd);
    s_spawnHostile = false;
}

void skill_fx_spawn_pillar(const float pos[3])
{
    SkillFx *f = fx_alloc(FX_PILLAR);
    if (!f) return;
    memcpy(f->pos, pos, sizeof(f->pos));
    f->life = 0.6f;
}

void skill_fx_spawn_burst(const float pos[3], float yaw)
{
    SkillFx *f = fx_alloc(FX_BURST);
    if (!f) return;
    memcpy(f->pos, pos, sizeof(f->pos));
    f->yaw = yaw;
    f->life = 0.4f;
}

// ---- Hits against the boss ----

static bool boss_capsule(const Boss *b, float a[3], float c[3])
{
    if (!b || b->mode != BOSS_MODE_COMBAT || b->health <= 0.0f) return false;
    for (int i = 0; i < 3; i++) {
        a[i] = b->pos[i] + b->capsuleCollider.localCapA.v[i];
        c[i] = b->pos[i] + b->capsuleCollider.localCapB.v[i];
    }
    return true;
}

static void closest_on_segment(const float a[3], const float b[3], const float p[3], float out[3])
{
    float ab[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
    float len2 = ab[0]*ab[0] + ab[1]*ab[1] + ab[2]*ab[2];
    float t = (len2 > 1e-6f) ? ((p[0]-a[0])*ab[0] + (p[1]-a[1])*ab[1] + (p[2]-a[2])*ab[2]) / len2 : 0.0f;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    for (int i = 0; i < 3; i++) out[i] = a[i] + ab[i] * t;
}

/** Sphere vs boss capsule; `contact` gets the point on the boss surface. */
static bool sphere_hits_boss(Boss *b, const float p[3], float r, float contact[3])
{
    float a[3], c[3], q[3];
    if (!boss_capsule(b, a, c)) return false;
    closest_on_segment(a, c, p, q);
    float d[3] = { p[0]-q[0], p[1]-q[1], p[2]-q[2] };
    float dist = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    float br = b->capsuleCollider.radius;
    if (dist > r + br) return false;
    float k = dist > 1e-4f ? br / dist : 0.0f;
    for (int i = 0; i < 3; i++) contact[i] = q[i] + d[i] * k;
    return true;
}

static void damage_boss(Boss *b, float damage, const float contact[3], float strength)
{
    boss_apply_damage(b, damage);
    blood_particles_fx_spawn_burst(contact[0], contact[1], contact[2], strength);
    audio_play_scene_sfx_dist(SCENE1_SFX_CHAR_ATTACK_HIT1 + (int)(rand_custom_u32() % 6), 1.0f, 0.0f);
}

#define ARC_GROW_TIME 0.35f

/** The crescent rolls outward from the player like a wave. */
static inline float arc_radius(const SkillFx *f)
{
    return f->radius * (0.2f + 0.8f * fminf(1.0f, f->age / ARC_GROW_TIME));
}

/** Sphere vs the player's capsule. */
static bool sphere_hits_player(const float p[3], float r)
{
    float a[3], c[3], q[3];
    for (int i = 0; i < 3; i++) {
        a[i] = character.pos[i] + character.capsuleCollider.localCapA.v[i];
        c[i] = character.pos[i] + character.capsuleCollider.localCapB.v[i];
    }
    closest_on_segment(a, c, p, q);
    float d[3] = { p[0]-q[0], p[1]-q[1], p[2]-q[2] };
    return d[0]*d[0] + d[1]*d[1] + d[2]*d[2] <= (r + character.capsuleCollider.radius) * (r + character.capsuleCollider.radius);
}

static void update_arc_hit(SkillFx *f, Boss *b)
{
    // One hit, when the wave front reaches the target
    if (f->hitsLeft <= 0) return;
    if (f->hostile) {
        float dx = character.pos[0] - f->pos[0], dz = character.pos[2] - f->pos[2];
        float dist = sqrtf(dx*dx + dz*dz);
        float fx = -fm_sinf(f->yaw), fz = fm_cosf(f->yaw);
        if (dist - character.capsuleCollider.radius > arc_radius(f)) return;
        if (dist > 1e-3f && (dx*fx + dz*fz) / dist < -0.2f) return;
        f->hitsLeft = 0;
        character_apply_damage(f->damage);
        return;
    }
    float a[3], c[3];
    if (!boss_capsule(b, a, c)) return;
    float dx = b->pos[0] - f->pos[0], dz = b->pos[2] - f->pos[2];
    float dist = sqrtf(dx*dx + dz*dz);
    if (dist - b->capsuleCollider.radius > arc_radius(f)) return;
    float fx = -fm_sinf(f->yaw), fz = fm_cosf(f->yaw);
    if (dist > 1e-3f && (dx*fx + dz*fz) / dist < -0.2f) return;    // behind the player
    f->hitsLeft = 0;
    float contact[3] = { b->pos[0] - dx / (dist + 1e-3f) * b->capsuleCollider.radius,
                         f->pos[1], b->pos[2] - dz / (dist + 1e-3f) * b->capsuleCollider.radius };
    damage_boss(b, f->damage, contact, 2.5f);
    screen_shake_set_shake_mag(14.0f);
}

static void update_wave_hit(SkillFx *f, Boss *b, float dt)
{
    if (f->hitsLeft <= 0) return;
    f->hitTimer -= dt;
    if (f->hitTimer > 0.0f) return;
    float c[3], contact[3];
    float r;
    if (f->shape == SKILL_WAVE_BLADE) {
        if (f->age < 0.2f) return;      // still rising out of the ground
        c[0] = f->pos[0]; c[1] = f->pos[1] + 14.0f; c[2] = f->pos[2];
        r = 7.0f;
    } else {
        memcpy(c, f->pos, sizeof(c));
        r = f->hostile ? 12.0f : 14.0f;     // the player's crosses are wider
    }
    if (f->hostile) {
        if (!sphere_hits_player(c, r)) return;
        character_apply_damage(f->damage);
    } else {
        if (!sphere_hits_boss(b, c, r, contact)) return;
        damage_boss(b, f->damage, contact, 1.0f);
    }
    f->hitsLeft--;
    f->hitTimer = f->hitInterval;
    if (f->shape == SKILL_WAVE_X && f->hitsLeft <= 0) f->life = f->age;   // spent
}

void skill_fx_update(float dt)
{
    Boss *b = boss_get_instance();
    for (int i = 0; i < SKILL_FX_MAX; i++) {
        SkillFx *f = &s_fx[i];
        if (f->type == FX_NONE) continue;
        f->age += dt;
        if (f->type == FX_WAVE) {
            for (int k = 0; k < 3; k++) f->pos[k] += f->vel[k] * dt;
            update_wave_hit(f, b, dt);
        } else if (f->type == FX_ARC) {
            update_arc_hit(f, b);
        }
        if (f->age >= f->life) {
            if (f->type == FX_WAVE && f->shape == SKILL_WAVE_X) {
                float ground[3] = { f->pos[0], f->pos[1], f->pos[2] };
                float yaw = atan2f(-f->vel[0], f->vel[2]);
                f->type = FX_NONE;      // free the slot before spawning the impact
                s_spawnHostile = f->hostile;
                skill_fx_spawn_burst(ground, yaw);
                if (f->pillarOnEnd) skill_fx_spawn_pillar(ground);
                s_spawnHostile = false;
            } else {
                f->type = FX_NONE;
            }
        }
    }
}

// ---- Drawing: an immediate-mode triangle builder over Tiny3D ----

#define BUILD_MAX_VERTS 68              // < T3D vertex cache (70), even
#define BUILD_MAX_TRIS  96
#define BUILD_RING      24              // buffers in flight (the RSP reads them later)

static T3DVertPacked *s_ring[BUILD_RING];
static T3DMat4FP *s_identity;
static int s_ringIdx;

static T3DVertPacked *s_vb;
static int s_nverts, s_ntris;
static uint8_t s_tris[BUILD_MAX_TRIS][3];
static uint16_t s_norm;

static void build_flush(void)
{
    if (s_ntris > 0) {
        t3d_vert_load(s_vb, 0, (uint32_t)((s_nverts + 1) & ~1));
        for (int i = 0; i < s_ntris; i++)
            t3d_tri_draw(s_tris[i][0], s_tris[i][1], s_tris[i][2]);
        t3d_tri_sync();
    }
    s_vb = s_ring[s_ringIdx];
    s_ringIdx = (s_ringIdx + 1) % BUILD_RING;
    s_nverts = s_ntris = 0;
}

/** Makes room for `nv` vertices / `nt` triangles in the current batch. */
static int build_reserve(int nv, int nt)
{
    if (s_nverts + nv > BUILD_MAX_VERTS || s_ntris + nt > BUILD_MAX_TRIS) build_flush();
    return s_nverts;
}

static int build_vert(const float p[3], uint32_t rgba)
{
    int vi = s_nverts++;
    T3DVertPacked *v = &s_vb[vi >> 1];
    int16_t x = (int16_t)p[0], y = (int16_t)p[1], z = (int16_t)p[2];
    if (vi & 1) { v->posB[0] = x; v->posB[1] = y; v->posB[2] = z; v->normB = s_norm; v->rgbaB = rgba; }
    else        { v->posA[0] = x; v->posA[1] = y; v->posA[2] = z; v->normA = s_norm; v->rgbaA = rgba; }
    return vi;
}

static void build_tri(int a, int b, int c)
{
    s_tris[s_ntris][0] = a; s_tris[s_ntris][1] = b; s_tris[s_ntris][2] = c;
    s_ntris++;
}

static bool s_drawHostile;      // crimson palette for the effect being drawn

static inline uint32_t rgba(int r, int g, int b, float a)
{
    if (s_drawHostile) {        // cyan/white -> crimson/rose, gold -> violet
        int nr = b, ng = g / 4, nb = r / 2;
        r = nr; g = ng; b = nb;
    }
    int ai = (int)(a * 255.0f);
    ai = ai < 0 ? 0 : ai > 255 ? 255 : ai;
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | (uint32_t)ai;
}

/** Ribbon between two point lists (n <= 34), as a strip of quads. */
static void build_ribbon(float (*outer)[3], float (*inner)[3], const uint32_t *cOuter,
                         const uint32_t *cInner, int n)
{
    int base = build_reserve(n * 2, (n - 1) * 2);
    for (int i = 0; i < n; i++) {
        build_vert(outer[i], cOuter[i]);
        build_vert(inner[i], cInner[i]);
    }
    for (int i = 0; i < n - 1; i++) {
        int o0 = base + i*2, i0 = o0 + 1, o1 = o0 + 2, i1 = o0 + 3;
        build_tri(o0, i0, o1);
        build_tri(i0, i1, o1);
    }
}

static inline float fade(const SkillFx *f, float in, float out)
{
    float t = f->age / f->life;
    float a = in > 0.0f ? fminf(1.0f, t / in) : 1.0f;
    if (t > 1.0f - out) a *= fmaxf(0.0f, (1.0f - t) / out);
    return a;
}

static void draw_arc(const SkillFx *f)
{
    enum { N = 17 };
    float outer[N][3], inner[N][3];
    uint32_t co[N], ci[N];
    float R = arc_radius(f);
    float a = fade(f, 0.0f, 0.4f);
    float fx = -fm_sinf(f->yaw), fz = fm_cosf(f->yaw);
    float rx = fz, rz = -fx;
    // Layer 0: flat crescent on the slash plane; layer 1: a blade-wall standing on its
    // outer edge, so the slash also reads from a low camera
    for (int layer = 0; layer < 2; layer++) {
        for (int i = 0; i < N; i++) {
            float th = -T3D_PI * 0.5f + T3D_PI * i / (N - 1);
            float dx = fx * cosf(th) + rx * sinf(th), dz = fz * cosf(th) + rz * sinf(th);
            float taper = cosf(th);           // crescent: thin at the tips
            outer[i][0] = f->pos[0] + dx * R; outer[i][2] = f->pos[2] + dz * R;
            if (layer == 0) {
                float w = 16.0f * taper;
                outer[i][1] = f->pos[1];
                inner[i][0] = f->pos[0] + dx * (R - w); inner[i][1] = f->pos[1]; inner[i][2] = f->pos[2] + dz * (R - w);
                co[i] = rgba(110, 210, 255, a);
                ci[i] = rgba(40, 120, 255, 0.0f);
            } else {
                float h = 12.0f * taper;
                outer[i][1] = f->pos[1] + h;
                inner[i][0] = outer[i][0]; inner[i][1] = f->pos[1] - h * 0.5f; inner[i][2] = outer[i][2];
                co[i] = rgba(240, 252, 255, 0.0f);
                ci[i] = rgba(200, 240, 255, a);
            }
        }
        build_ribbon(outer, inner, co, ci, N);
    }
}

/** Basis around a direction: d (normalised), s (horizontal side), u (up-ish). */
static void dir_basis(const float v[3], float d[3], float s[3], float u[3])
{
    float len = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len < 1e-4f) { d[0] = 0; d[1] = 0; d[2] = 1; } else { for (int i = 0; i < 3; i++) d[i] = v[i] / len; }
    float h = sqrtf(d[0]*d[0] + d[2]*d[2]);
    if (h < 1e-4f) { s[0] = 1; s[1] = 0; s[2] = 0; } else { s[0] = d[2] / h; s[1] = 0; s[2] = -d[0] / h; }
    u[0] = d[1]*s[2] - d[2]*s[1]; u[1] = d[2]*s[0] - d[0]*s[2]; u[2] = d[0]*s[1] - d[1]*s[0];
}

static void draw_xwave(const SkillFx *f)
{
    enum { N = 11 };
    float d[3], s[3], u[3];
    dir_basis(f->vel, d, s, u);
    float a = fade(f, 0.1f, 0.3f);
    for (int k = 0; k < 2; k++) {
        float ca = 0.7071f, sa = k ? -0.7071f : 0.7071f;    // +-45 degrees
        float e[3] = { s[0]*ca + u[0]*sa, s[1]*ca + u[1]*sa, s[2]*ca + u[2]*sa };
        float outer[N][3], inner[N][3];
        uint32_t co[N], ci[N];
        if (!f->hostile) {
            // The player's: wide blades whose width lies across the arm (so the X reads
            // from behind), bright down the middle and fading to the edges
            float n[3] = { s[0]*ca - u[0]*sa, s[1]*ca - u[1]*sa, s[2]*ca - u[2]*sa };
            for (int side = -1; side <= 1; side += 2) {
                for (int i = 0; i < N; i++) {
                    float t = -1.0f + 2.0f * i / (N - 1);
                    float bulge = 1.0f - t * t;
                    for (int c = 0; c < 3; c++) {
                        inner[i][c] = f->pos[c] + e[c] * t * 28.0f + d[c] * bulge * 4.0f;
                        outer[i][c] = inner[i][c] + n[c] * side * (0.8f + bulge * 3.5f);
                    }
                    ci[i] = rgba(240, 252, 255, a);
                    co[i] = rgba(90, 200, 255, a * 0.25f);
                }
                build_ribbon(outer, inner, co, ci, N);
            }
            continue;
        }
        for (int i = 0; i < N; i++) {
            float t = -1.0f + 2.0f * i / (N - 1);
            float bulge = 1.0f - t * t;
            for (int c = 0; c < 3; c++) {
                outer[i][c] = f->pos[c] + e[c] * t * 24.0f + d[c] * bulge * 3.0f;
                inner[i][c] = outer[i][c] - d[c] * bulge * 4.0f;
            }
            co[i] = rgba(235, 250, 255, a);
            ci[i] = rgba(90, 200, 255, a * 0.2f);
        }
        build_ribbon(outer, inner, co, ci, N);
    }
}

/** Crescent Cleave blade: a tall, slightly curved sliver rising out of the ground. */
static void draw_blade(const SkillFx *f)
{
    enum { N = 9 };
    float d[3], s[3], u[3];
    float flat[3] = { f->vel[0], 0.0f, f->vel[2] };
    dir_basis(flat, d, s, u);
    float rise = fminf(1.0f, f->age / 0.25f);
    float a = fade(f, 0.0f, 0.3f);
    if (a < 0.2f) return;   // nearly faded: very low alphas render dark on GLideN64
    const float H = 70.0f, BOW = 2.5f, W = 4.0f;
    float outer[N][3], inner[N][3];
    uint32_t co[N], ci[N];
    for (int i = 0; i < N; i++) {
        float t = (float)i / (N - 1);           // 0 at the ground, 1 at the tip
        float bow = sinf(T3D_PI * t);           // curves forward like ")" seen from the side
        float w = W * sinf(T3D_PI * (0.15f + 0.85f * t));
        for (int c = 0; c < 3; c++) {
            float up = (c == 1) ? 1.0f : 0.0f;
            float base = f->pos[c] + up * t * H * rise + d[c] * bow * BOW;
            outer[i][c] = base + s[c] * w * 0.5f;
            inner[i][c] = base - s[c] * w * 0.5f;
        }
        co[i] = rgba(150 + (int)(90 * t), 225 + (int)(25 * t), 255, a * (0.4f + 0.6f * t));
        ci[i] = co[i];
    }
    build_ribbon(outer, inner, co, ci, N);
    // Bright core line
    for (int i = 0; i < N; i++) {
        for (int c = 0; c < 3; c++) {
            float mid = (outer[i][c] + inner[i][c]) * 0.5f;
            outer[i][c] = mid + s[c] * 0.6f;
            inner[i][c] = mid - s[c] * 0.6f;
        }
        co[i] = ci[i] = rgba(245, 252, 255, a);
    }
    build_ribbon(outer, inner, co, ci, N);
}

static void draw_pillar(const SkillFx *f)
{
    float grow = fminf(1.0f, f->age / 0.1f);
    float a = fade(f, 0.0f, 0.5f);
    if (a < 0.2f) return;   // see draw_blade
    const float W = 3.5f;
    float H = 70.0f * grow;
    for (int k = 0; k < 2; k++) {
        float ox = k ? 0.0f : W, oz = k ? W : 0.0f;
        float outer[2][3] = { { f->pos[0] - ox, f->pos[1], f->pos[2] - oz },
                              { f->pos[0] - ox, f->pos[1] + H, f->pos[2] - oz } };
        float inner[2][3] = { { f->pos[0] + ox, f->pos[1], f->pos[2] + oz },
                              { f->pos[0] + ox, f->pos[1] + H, f->pos[2] + oz } };
        uint32_t c[2] = { rgba(230, 248, 255, a), rgba(120, 210, 255, 0.0f) };
        build_ribbon(outer, inner, c, c, 2);
    }
}

static void draw_burst(const SkillFx *f)
{
    enum { SPIKES = 7 };
    float grow = fminf(1.0f, f->age / 0.08f);
    float a = fade(f, 0.0f, 0.6f);
    int base = build_reserve(SPIKES * 3, SPIKES);
    for (int k = 0; k < SPIKES; k++) {
        float ang = f->yaw + 2.0f * T3D_PI * k / SPIKES + (k & 1) * 0.3f;
        float len = ((k & 1) ? 9.0f : 14.0f) * grow;
        float c = cosf(ang), s = sinf(ang), cw = cosf(ang + 0.35f), sw = sinf(ang + 0.35f);
        float p0[3] = { f->pos[0] + c * 2.0f,  f->pos[1],              f->pos[2] + s * 2.0f };
        float p1[3] = { f->pos[0] + cw * 2.0f, f->pos[1],              f->pos[2] + sw * 2.0f };
        float tip[3] = { f->pos[0] + c * len,  f->pos[1] + len * 0.8f, f->pos[2] + s * len };
        build_vert(p0, rgba(255, 210, 80, a));
        build_vert(p1, rgba(255, 210, 80, a));
        build_vert(tip, rgba(255, 245, 190, a * 0.3f));
        build_tri(base + k*3, base + k*3 + 1, base + k*3 + 2);
    }
}

static void draw_begin(void)
{
    if (!s_identity) {
        s_identity = malloc_uncached(sizeof(T3DMat4FP));
        T3DMat4 id;
        t3d_mat4_identity(&id);
        t3d_mat4_to_fixed(s_identity, &id);
        for (int i = 0; i < BUILD_RING; i++)
            s_ring[i] = malloc_uncached(sizeof(T3DVertPacked) * (BUILD_MAX_VERTS / 2));
        s_norm = t3d_vert_pack_normal(&(T3DVec3){{ 0, 0, 1 }});
    }

    t3d_fog_set_enabled(false);
    rdpq_sync_pipe();
    rdpq_set_mode_standard();
    rdpq_mode_zbuf(true, false);
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_mode_dithering(DITHER_NONE_BAYER);
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT);
    t3d_state_set_depth_offset(-0x20);
    t3d_matrix_push(s_identity);

    s_nverts = s_ntris = 0;
    s_vb = s_ring[s_ringIdx];
    s_ringIdx = (s_ringIdx + 1) % BUILD_RING;
}

static void draw_end(void)
{
    build_flush();
    s_drawHostile = false;

    t3d_matrix_pop(1);
    t3d_state_set_depth_offset(0);
    t3d_fog_set_enabled(true);
    rdpq_sync_pipe();
}

void skill_fx_draw(void)
{
    bool any = false;
    for (int i = 0; i < SKILL_FX_MAX && !any; i++) any = s_fx[i].type != FX_NONE;
    if (!any) return;

    draw_begin();
    for (int i = 0; i < SKILL_FX_MAX; i++) {
        const SkillFx *f = &s_fx[i];
        s_drawHostile = f->hostile;
        switch (f->type) {
            case FX_ARC:    draw_arc(f); break;
            case FX_WAVE:   if (f->shape == SKILL_WAVE_X) draw_xwave(f); else draw_blade(f); break;
            case FX_PILLAR: draw_pillar(f); break;
            case FX_BURST:  draw_burst(f); break;
            default: break;
        }
    }
    draw_end();
}

void skill_fx_draw_vortex(const float top[3], float depth, float radius, float time)
{
    enum { STRANDS = 12, SEG = 9 };
    draw_begin();
    for (int k = 0; k < STRANDS; k++) {
        float outer[SEG][3], inner[SEG][3];
        uint32_t co[SEG], ci[SEG];
        bool hot = (k & 1) == 0;
        float spin = time * (hot ? 1.6f : 1.1f);
        for (int i = 0; i < SEG; i++) {
            float u = (float)i / (SEG - 1);
            float r = radius * powf(1.0f - u, 1.4f) + 3.0f;
            float a = 2.0f * T3D_PI * k / STRANDS + spin + u * 2.8f;
            float w = 0.2f + 0.12f * (1.0f - u);
            float y = top[1] - depth * u;
            outer[i][0] = top[0] + r * cosf(a - w); outer[i][1] = y; outer[i][2] = top[2] + r * sinf(a - w);
            inner[i][0] = top[0] + r * cosf(a + w); inner[i][1] = y; inner[i][2] = top[2] + r * sinf(a + w);
            float al = 0.9f - 0.5f * u;
            co[i] = hot ? rgba(255, 150, 40, al) : rgba(170, 60, 240, al);
            ci[i] = hot ? rgba(255, 210, 120, al * 0.5f) : rgba(230, 120, 255, al * 0.5f);
        }
        build_ribbon(outer, inner, co, ci, SEG);
    }
    draw_end();
}
