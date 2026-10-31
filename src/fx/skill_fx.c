#include <libdragon.h>
#include "gpu_frame.h"
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
#include "utilities/simple_collision_utility.h"

static void dir_basis(const float v[3], float d[3], float s[3], float u[3]);

#define SKILL_FX_MAX 40     // the Shackled Sun's spiral has 20 embers in the air

typedef enum { FX_NONE, FX_ARC, FX_WAVE, FX_PILLAR, FX_BURST, FX_CROWN } FxType;

typedef struct {
    FxType type;
    SkillWaveShape shape;
    float pos[3], vel[3];
    float yaw;
    float age, life;
    float radius;           // arc: final radius
    float half;             // arc: how far round it reaches each side of yaw
    float damage;
    float poise;            // share of the boss's stagger meter a hit fills (< 0: the damage's)
    int   hitsLeft;
    float hitInterval, hitTimer;
    bool  pillarOnEnd;
    bool  hostile;          // hits the player (Fallen Knight) instead of the boss
    bool  warn;             // a hostile burst marking where a hit will land: drawn red
    float scale;            // a wave's size (hit and drawn), 1 by default
    float delay;            // charging where it was spawned: still, harmless and growing until then
    float homing;           // turn rate toward the player (an orb), 0 once it has committed
} SkillFx;

static bool s_spawnHostile;     // palette/target for effects spawned now
static bool s_spawnWarn;        // a warning burst (skill_fx_spawn_burst_warn)
static float s_spawnRoll;       // a crescent's tilt, for waves spawned now
static float s_spawnPoise = -1.0f;   // for the next spawn only (skill_fx_next_poise)
static float s_spawnScale = 1.0f, s_spawnDelay, s_spawnHoming;     // for the next spawn only (skill_fx_next_shape)

void skill_fx_next_poise(float poise) { s_spawnPoise = poise; }
void skill_fx_next_shape(float scale, float delay) { s_spawnScale = scale; s_spawnDelay = delay; }
static float s_spawnHalf = T3D_PI * 0.5f;
void skill_fx_next_arc_half(float half) { s_spawnHalf = half; }

static SkillFx s_fx[SKILL_FX_MAX];

// The Fallen Warrior's fire trail: points along its path, each burning FIRE_BURN then smoke for
// FIRE_SMOKE; drawn as two seamless lines of fire either side of the path (one strip each)
#define FIRE_PTS   24
#define FIRE_SMOKE 0.5f
#define FIRE_SIDE  5.0f     // each line this far off the path
#define FIRE_BAND  7.0f     // the fire's reach off the path (the lines and their flames)
typedef struct { float x, y, z, sx, sz, fx, fz, born, burn; uint8_t id; } FirePt;     // where, its side and forward, when lit, how long it burns
static FirePt s_fire[FIRE_PTS];
static int s_fireN;
static float s_fireT;
static uint8_t s_fireId;

// ---- Spawning ----

static SkillFx* fx_alloc(FxType type)
{
    for (int i = 0; i < SKILL_FX_MAX; i++) {
        if (s_fx[i].type == FX_NONE) {
            memset(&s_fx[i], 0, sizeof(s_fx[i]));
            s_fx[i].type = type;
            s_fx[i].hostile = s_spawnHostile;
            s_fx[i].warn = s_spawnWarn;
            return &s_fx[i];
        }
    }
    return NULL;    // pool full: drop the effect
}

void skill_fx_init(void) { skill_fx_reset(); }
void skill_fx_reset(void) { memset(s_fx, 0, sizeof(s_fx)); s_fireN = 0; }
void skill_fx_clear_hostile(void) { for (int i = 0; i < SKILL_FX_MAX; i++) if (s_fx[i].hostile) s_fx[i].type = FX_NONE; s_fireN = 0; }

void skill_fx_fire_trail_begin(void) { s_fireId++; }

bool skill_fx_fire_trail_add(const float pos[3], float yaw, float burn)
{
    if (s_fireN >= FIRE_PTS) return false;
    float fx = -sinf(yaw), fz = cosf(yaw);
    s_fire[s_fireN++] = (FirePt){ pos[0], pos[1], pos[2], fz, -fx, fx, fz, s_fireT, burn, s_fireId };
    return true;
}

/** True if p (on the floor) is within r of the fire still burning (smoke doesn't): the band along
 *  each stretch between two of a trail's points, as drawn */
bool skill_fx_fire_trail_touches(const float p[3], float r)
{
    float R = r + FIRE_BAND;
    for (int i = 0; i + 1 < s_fireN; i++) {
        const FirePt *a = &s_fire[i], *b = &s_fire[i + 1];
        if (a->id != b->id || s_fireT - a->born >= a->burn) continue;
        float ex = b->x - a->x, ez = b->z - a->z, l2 = ex * ex + ez * ez;
        float u = l2 > 1e-6f ? fmaxf(0.0f, fminf(1.0f, ((p[0] - a->x) * ex + (p[2] - a->z) * ez) / l2)) : 0.0f;
        float dx = p[0] - (a->x + ex * u), dz = p[2] - (a->z + ez * u);
        if (dx * dx + dz * dz < R * R) return true;
    }
    return false;
}

void skill_fx_spawn_arc(const float pos[3], float yaw, float radius, float damage)
{
    SkillFx *f = fx_alloc(FX_ARC);
    if (!f) return;
    memcpy(f->pos, pos, sizeof(f->pos));
    f->yaw = yaw;
    f->radius = radius;
    f->half = s_spawnHalf; s_spawnHalf = T3D_PI * 0.5f;
    f->damage = damage;
    f->poise = s_spawnPoise; s_spawnPoise = -1.0f;
    f->hitsLeft = 1;
    f->life = 0.55f;
}

void skill_fx_spawn_wave(SkillWaveShape shape, const float pos[3], const float vel[3],
                         float damage, int hits, float hitInterval, float life, bool pillarOnEnd)
{
    SkillFx *f = fx_alloc(FX_WAVE);
    if (!f) { s_spawnScale = 1.0f; s_spawnDelay = s_spawnHoming = 0.0f; s_spawnPoise = -1.0f; return; }
    f->shape = shape;
    memcpy(f->pos, pos, sizeof(f->pos));
    memcpy(f->vel, vel, sizeof(f->vel));
    f->damage = damage;
    f->poise = s_spawnPoise; s_spawnPoise = -1.0f;
    f->hitsLeft = hits;
    f->hitInterval = hitInterval;
    f->life = life;
    f->pillarOnEnd = pillarOnEnd;
    f->yaw = s_spawnRoll;
    f->scale = s_spawnScale; f->delay = s_spawnDelay; f->homing = s_spawnHoming;
    s_spawnScale = 1.0f; s_spawnDelay = 0.0f; s_spawnHoming = 0.0f;
}

void skill_fx_spawn_orb_hostile(const float pos[3], const float vel[3], float damage, float life, float homing)
{
    s_spawnHoming = homing;
    skill_fx_spawn_wave_hostile(SKILL_WAVE_ORB, pos, vel, damage, 1, 0.0f, life, false);
    s_spawnHoming = 0.0f;   // (a full pool dropped it)
}

void skill_fx_spawn_crescent_hostile(const float pos[3], const float vel[3], float roll, float damage, float life)
{
    s_spawnRoll = roll;
    skill_fx_spawn_wave_hostile(SKILL_WAVE_CRESCENT, pos, vel, damage, 1, 0.0f, life, false);
    s_spawnRoll = 0.0f;
}

void skill_fx_spawn_slash_hostile(const float pos[3], const float vel[3], float roll, float damage, float life)
{
    s_spawnRoll = roll;
    skill_fx_spawn_wave_hostile(SKILL_WAVE_SLASH, pos, vel, damage, 1, 0.0f, life, false);
    s_spawnRoll = 0.0f;
}

void skill_fx_spawn_pillar_hostile(const float pos[3])
{
    s_spawnHostile = true;
    skill_fx_spawn_pillar(pos);
    s_spawnHostile = false;
}

void skill_fx_spawn_burst_hostile(const float pos[3], float yaw)
{
    s_spawnHostile = true;
    skill_fx_spawn_burst(pos, yaw);
    s_spawnHostile = false;
}

void skill_fx_spawn_burst_warn(const float pos[3], float yaw)
{
    s_spawnWarn = true;
    skill_fx_spawn_burst_hostile(pos, yaw);
    s_spawnWarn = false;
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

// The Fallen Warrior's Moon Blade take-off and landing: a ring of pointed spikes spreading over the
// floor, pale gold (the player's keep their sparks)
void skill_fx_spawn_crown_hostile(const float pos[3])
{
    s_spawnHostile = true;
    SkillFx *f = fx_alloc(FX_CROWN);
    s_spawnHostile = false;
    if (!f) return;
    memcpy(f->pos, pos, sizeof(f->pos));
    f->yaw = (rand() % 628) / 100.0f;
    f->life = 0.5f;
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

static void damage_boss(Boss *b, float damage, float poise, const float contact[3], float strength)
{
    if (poise >= 0.0f) boss_apply_hit(b, damage, poise);
    else boss_apply_damage(b, damage);
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

// The crescent rolls along the floor: it reaches what the sword can (the tip comes ~26 above the
// floor), so a boss up in the air (Unbound's barrage, Shadowborne flying, a leap) is out of it
#define ARC_REACH 20.0f     // above the wave's centre, which is 10 above the floor
bool skill_arc_reaches(const Boss *b, float arcY)
{
    float a[3], c[3];
    if (!boss_capsule(b, a, c)) return false;
    return fminf(a[1], c[1]) - b->capsuleCollider.radius <= arcY + ARC_REACH;
}

static void update_arc_hit(SkillFx *f, Boss *b)
{
    // One hit, when the wave front reaches the target
    if (f->hitsLeft <= 0) return;
    if (f->hostile) {
        if (f->damage <= 0.0f) return;      // drawn only (its blow already landed)
        float dx = character.pos[0] - f->pos[0], dz = character.pos[2] - f->pos[2];
        float dist = sqrtf(dx*dx + dz*dz);
        float fx = -fm_sinf(f->yaw), fz = fm_cosf(f->yaw);
        if (dist - character.capsuleCollider.radius > arc_radius(f)) return;
        float rr = character.capsuleCollider.radius;     // only as far round as it's drawn (with the body's width)
        if (dist > rr && acosf(fmaxf(-1.0f, fminf(1.0f, (dx*fx + dz*fz) / dist))) > f->half + asinf(rr / dist)) return;
        if (character.pos[1] > f->pos[1] + 2.0f) return;     // a wave along the floor: one in the air clears it
#if defined(HLE_FK_TRACE) || defined(HLE_FK_HITLOG)
        debugf("[arc] hit: player at %.0f deg, %.0f away (its body's edge %.0f), the ring drawn out to %.0f, age %.2f, player %.0f above it\n", (double)(acosf(fmaxf(-1.0f, fminf(1.0f, (dx*fx + dz*fz) / (dist + 1e-3f)))) * 57.3f), (double)dist,
               (double)(dist - character.capsuleCollider.radius), (double)arc_radius(f), (double)f->age, (double)(character.pos[1] - f->pos[1]));
#endif
        f->hitsLeft = 0;
#ifdef HLE_LOG_MOVES     // test aid: a hostile wave's hit (the [hit] after it is this one)
        debugf("[fxhit] wave\n");
#endif
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
    if (!skill_arc_reaches(b, f->pos[1])) return;      // over the wave: it may come down into it while it rolls
    f->hitsLeft = 0;
    float contact[3] = { b->pos[0] - dx / (dist + 1e-3f) * b->capsuleCollider.radius,
                         f->pos[1], b->pos[2] - dz / (dist + 1e-3f) * b->capsuleCollider.radius };
    damage_boss(b, f->damage, f->poise, contact, 2.5f);
    screen_shake_set_shake_mag(14.0f);
}

// The Fallen Warrior's Moon Blade beam: one long slash through its path, tilted f->yaw about the
// flight, SLASH_L each way (60 long, longer than the player's X arms), wide across itself; hit
// along it, SLASH_R round, as drawn
#define SLASH_L   30.0f
#define SLASH_R   3.5f      // hit; drawn up to 3.7 wide each side at its middle, tapering to its tips
static void slash_axis(const SkillFx *f, const float sd[3], const float u[3], float e[3], float n[3])
{
    float ca = cosf(f->yaw), sa = sinf(f->yaw);
    for (int m = 0; m < 3; m++) { e[m] = sd[m] * ca + u[m] * sa; n[m] = u[m] * ca - sd[m] * sa; }
}

static void update_wave_hit(SkillFx *f, Boss *b, float dt)
{
    if (f->hitsLeft <= 0 || f->age < f->delay) return;
    f->hitTimer -= dt;
    if (f->hitTimer > 0.0f) return;
    float c[3], contact[3];
    float r;
    if (f->shape == SKILL_WAVE_BLADE) {
        if (f->age < 0.2f) return;      // still rising out of the ground
        c[0] = f->pos[0]; c[1] = f->pos[1] + 14.0f; c[2] = f->pos[2];
        r = f->hostile ? 4.0f : 7.0f;   // a hostile one is drawn 4 wide: no further (the player's own reaches wider)
    } else {
        memcpy(c, f->pos, sizeof(c));
        r = (f->shape == SKILL_WAVE_ORB ? 5.0f : f->shape == SKILL_WAVE_CRESCENT ? 14.0f : f->hostile ? 12.0f : 7.5f) * f->scale;     // the player's X is drawn 15 long (hit as drawn)
    }
    if (f->hostile) {
        if (f->shape == SKILL_WAVE_SLASH) {
            // The beam as drawn (draw_xwave): a capsule along it
            float d[3], sd[3], u[3], e[3], n[3], pa[3], pb[3], ea[3], eb[3];
            dir_basis(f->vel, d, sd, u);
            slash_axis(f, sd, u, e, n);
            for (int m = 0; m < 3; m++) {
                ea[m] = f->pos[m] + e[m] * SLASH_L * f->scale; eb[m] = f->pos[m] - e[m] * SLASH_L * f->scale;
                pa[m] = character.pos[m] + character.capsuleCollider.localCapA.v[m];
                pb[m] = character.pos[m] + character.capsuleCollider.localCapB.v[m];
            }
            if (!scu_capsule_vs_capsule_f(ea, eb, SLASH_R * f->scale, pa, pb, character.capsuleCollider.radius)) return;
        } else if (f->shape == SKILL_WAVE_CRESCENT) {
            // A crescent is one long thin blade (draw_xwave): hit along it, not with a ball round its
            // middle (that missed the ends of a level one and hit well clear of an upright one)
            float d[3], sd[3], u[3], a[3], b[3], pa[3], pb[3];
            dir_basis(f->vel, d, sd, u);
            float ca = cosf(f->yaw), sa = sinf(f->yaw), L = 28.0f * f->scale, k = 3.0f * f->scale;
            for (int m = 0; m < 3; m++) {
                float e = sd[m] * ca + u[m] * sa;
                a[m] = f->pos[m] + d[m] * k + e * L; b[m] = f->pos[m] + d[m] * k - e * L;
                pa[m] = character.pos[m] + character.capsuleCollider.localCapA.v[m];
                pb[m] = character.pos[m] + character.capsuleCollider.localCapB.v[m];
            }
            bool hit = scu_capsule_vs_capsule_f(a, b, k, pa, pb, character.capsuleCollider.radius);
#ifdef HLE_LOG_HITVOL     // test aid: the old ball (14) against the blade
            static int said;
            if (hit != sphere_hits_player(c, r) && said++ < 60) { float q[3] = { character.pos[0] - f->pos[0], character.pos[1] + 8.0f - f->pos[1], character.pos[2] - f->pos[2] };
                debugf("[hitvol] crescent tilt %.2f: the old ball %s, the blade %s; player %.0f to its side, %.0f up\n", (double)f->yaw, hit ? "misses" : "hits", hit ? "hits" : "misses",
                       (double)(q[0] * sd[0] + q[2] * sd[2]), (double)q[1]); }
#endif
            if (!hit) return;
        } else {
            bool hit = sphere_hits_player(c, r);
#ifdef HLE_LOG_HITVOL     // test aid: an ember's centre against the player's body (its axis), near it
            if (f->shape == SKILL_WAVE_ORB) {
                float a[3], b2[3], q[3];
                for (int m = 0; m < 3; m++) { a[m] = character.pos[m] + character.capsuleCollider.localCapA.v[m]; b2[m] = character.pos[m] + character.capsuleCollider.localCapB.v[m]; }
                closest_on_segment(a, b2, c, q);
                float e = sqrtf((c[0]-q[0])*(c[0]-q[0]) + (c[1]-q[1])*(c[1]-q[1]) + (c[2]-q[2])*(c[2]-q[2]));
                if (e < r + character.capsuleCollider.radius + 6.0f)
                    debugf("[hitvol] orb x%.1f age %.2f: centre %.1f from the body's axis (hits within %.1f + %.0f; drawn core %.1f, flame %.1f), %.0f up: %s\n", (double)f->scale, (double)f->age,
                           (double)e, (double)r, (double)character.capsuleCollider.radius, (double)(3.4f * f->scale), (double)(7.2f * f->scale), (double)(c[1] - character.pos[1]), hit ? "HIT" : "miss");
            }
#endif
            if (!hit) return;
        }
#ifdef HLE_FK_TRACE
        debugf("[fxhit] shape %d age %.2f at %.0f,%.0f,%.0f player %.0f,%.0f,%.0f vel %.0f,%.0f\n", f->shape, (double)f->age, (double)c[0], (double)c[1], (double)c[2],
               (double)character.pos[0], (double)character.pos[1], (double)character.pos[2], (double)f->vel[0], (double)f->vel[2]);
#endif
#ifdef HLE_LOG_MOVES     // test aid: a hostile shot's hit (the [hit] after it is this one)
        debugf("[fxhit] shape %d\n", f->shape);
#endif
        character_apply_damage(f->damage);
    } else {
        if (!sphere_hits_boss(b, c, r, contact)) return;
        damage_boss(b, f->damage, f->poise, contact, 1.0f);
    }
    f->hitsLeft--;
    f->hitTimer = f->hitInterval;
    if (f->shape != SKILL_WAVE_BLADE && f->hitsLeft <= 0) f->life = f->age;   // spent
}

void skill_fx_update(float dt)
{
    Boss *b = boss_get_instance();
    s_fireT += dt;
    {   // the trail's points out in the order they were lit, once their smoke has gone
        int k = 0;
        while (k < s_fireN && s_fireT - s_fire[k].born >= s_fire[k].burn + FIRE_SMOKE) k++;
        if (k) { memmove(s_fire, s_fire + k, sizeof(s_fire[0]) * (s_fireN - k)); s_fireN -= k; }
    }
    for (int i = 0; i < SKILL_FX_MAX; i++) {
        SkillFx *f = &s_fx[i];
        if (f->type == FX_NONE) continue;
        f->age += dt;
        if (f->type == FX_WAVE) {
            if (f->age < f->delay) { f->life += dt; continue; }     // charging: its life starts on release
            if (f->homing > 0.0f) {     // steer at the player's chest, until close: then straight on (dodge it late)
                float d[3] = { character.pos[0] - f->pos[0], character.pos[1] + 10.0f - f->pos[1], character.pos[2] - f->pos[2] };
                float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), sp = sqrtf(f->vel[0] * f->vel[0] + f->vel[1] * f->vel[1] + f->vel[2] * f->vel[2]);
                if (l < 32.0f) f->homing = 0.0f;
                else if (sp > 1e-3f) {
                    float k = fminf(1.0f, f->homing * dt), v[3], vl = 0.0f;
                    for (int m = 0; m < 3; m++) { v[m] = f->vel[m] / sp + (d[m] / l - f->vel[m] / sp) * k; vl += v[m] * v[m]; }
                    vl = sqrtf(vl);
                    for (int m = 0; m < 3; m++) f->vel[m] = v[m] / vl * sp;
                }
            }
            for (int k = 0; k < 3; k++) f->pos[k] += f->vel[k] * dt;
            update_wave_hit(f, b, dt);
        } else if (f->type == FX_ARC) {
            update_arc_hit(f, b);
        }
        if (f->age >= f->life) {
            if (f->type == FX_WAVE && (f->shape == SKILL_WAVE_X || f->shape == SKILL_WAVE_SLASH)) {
                f->type = FX_NONE;      // the Moon Blades' waves leave nothing where they end (no spark, no pillar)
            } else if (f->type == FX_WAVE && f->shape != SKILL_WAVE_BLADE) {
                float ground[3] = { f->pos[0], f->pos[1], f->pos[2] };
                float yaw = atan2f(-f->vel[0], f->vel[2]);
                f->type = FX_NONE;      // free the slot before spawning the impact
                s_spawnHostile = f->hostile;
                if (!f->hostile) skill_fx_spawn_burst(ground, yaw);     // the enemies' leave no spark
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

static T3DMat4FP *s_quarter;

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
    s_vb = gpu_alloc(sizeof(T3DVertPacked) * (BUILD_MAX_VERTS / 2));    // this frame's (the RSP reads it later)
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
    // in quarter units (drawn through a 1/4 scale, draw_begin): whole units snapped a small
    // effect's edges into teeth (the player's X). Arenas are well inside +-8191
    int16_t x = (int16_t)lroundf(p[0] * 4.0f), y = (int16_t)lroundf(p[1] * 4.0f), z = (int16_t)lroundf(p[2] * 4.0f);
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
#ifdef HLE_LOG_HITVOL     // test aid: a ring (the Sun's flare) as drawn this frame
    if (f->hostile && f->half > 3.0f) debugf("[hitvol] ring drawn: age %.2f, outer edge %.1f, alpha %.2f\n", (double)f->age, (double)R, (double)a);
#endif
    float fx = -fm_sinf(f->yaw), fz = fm_cosf(f->yaw);
    float rx = fz, rz = -fx;
    // Layer 0: flat crescent on the slash plane; layer 1: a blade-wall standing on its
    // outer edge, so the slash also reads from a low camera
    for (int layer = 0; layer < 2; layer++) {
        for (int i = 0; i < N; i++) {
            float th = f->half * (-1.0f + 2.0f * i / (N - 1));
            float dx = fx * cosf(th) + rx * sinf(th), dz = fz * cosf(th) + rz * sinf(th);
            float taper = cosf(th * (T3D_PI * 0.5f) / f->half);     // crescent: thin at the tips
            if (f->hostile) taper = 0.35f + 0.65f * taper;  // but a hostile one hurts right out to them: show it (a ring's two halves met in nothing at its sides)
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
    if (f->shape == SKILL_WAVE_SLASH) {
        // The Fallen Warrior's beam: long, wide across itself (it reads from behind), bright down
        // its middle and fading to its edges, curving forward a little
        float e[3], n[3];
        slash_axis(f, s, u, e, n);
        for (int side = -1; side <= 1; side += 2) {
            float outer[N][3], inner[N][3];
            uint32_t co[N], ci[N];
            for (int i = 0; i < N; i++) {
                float t = -1.0f + 2.0f * i / (N - 1), bulge = 1.0f - t * t;
                for (int c = 0; c < 3; c++) {
                    inner[i][c] = f->pos[c] + e[c] * t * SLASH_L * f->scale + d[c] * bulge * 3.5f * f->scale;
                    outer[i][c] = inner[i][c] + n[c] * side * (0.7f + bulge * 3.0f) * f->scale;
                }
                ci[i] = rgba(240, 252, 255, a);
                co[i] = rgba(90, 200, 255, a * 0.25f);
            }
            build_ribbon(outer, inner, co, ci, N);
        }
        return;
    }
    bool one = f->shape == SKILL_WAVE_CRESCENT;
    for (int k = 0; k < (one ? 1 : 2); k++) {
        float ca = one ? cosf(f->yaw) : 0.7071f, sa = one ? sinf(f->yaw) : k ? -0.7071f : 0.7071f;    // +-45 degrees, or its tilt
        float e[3] = { s[0]*ca + u[0]*sa, s[1]*ca + u[1]*sa, s[2]*ca + u[2]*sa };
        float outer[N][3], inner[N][3];
        uint32_t co[N], ci[N];
        if (!f->hostile) {
            // The player's: wide blades whose width lies across the arm (so the X reads from
            // behind), bright down the middle and fading to the edges; XL long each way. No seam
            // where they cross: the first arm is solid where the second meets it, and the second
            // is drawn in two pieces ending at the first's edges (the arms are square to each
            // other, so its cut lies along the first's edge): nothing is blended twice
            const float XL = 15.0f, XK = XL / 22.0f;      // its size, and the width scaled to it
            const float HALF = (0.8f + 3.5f) * XK;           // an arm's half-width at the crossing
            float n[3] = { s[0]*ca - u[0]*sa, s[1]*ca - u[1]*sa, s[2]*ca - u[2]*sa };
            float tc = HALF / XL;                            // the crossing, along an arm
            int pieces = k ? 2 : 1;
            for (int p = 0; p < pieces; p++) {
                float t0 = k ? (p ? tc : -1.0f) : -1.0f, t1 = k ? (p ? 1.0f : -tc) : 1.0f;
                for (int side = -1; side <= 1; side += 2) {
                    for (int i = 0; i < N; i++) {
                        float t = t0 + (t1 - t0) * i / (N - 1);
                        float bulge = 1.0f - t * t;
                        for (int c = 0; c < 3; c++) {
                            inner[i][c] = f->pos[c] + e[c] * t * XL + d[c] * bulge * 4.0f * XK;
                            outer[i][c] = inner[i][c] + n[c] * side * (0.8f + bulge * 3.5f) * XK;
                        }
                        // solid across the first arm where the second crosses it (no faded edge line)
                        bool solid = !k && fabsf(t) <= tc;
                        ci[i] = rgba(240, 252, 255, a);
                        co[i] = solid ? ci[i] : rgba(90, 200, 255, a * 0.25f);
                    }
                    build_ribbon(outer, inner, co, ci, N);
                }
            }
            continue;
        }
        // charging (a delay): it grows where it will be loosed from
        float sc = f->scale * (f->delay > 0.0f ? 0.3f + 0.7f * fminf(1.0f, f->age / f->delay) : 1.0f);
        for (int i = 0; i < N; i++) {
            float t = -1.0f + 2.0f * i / (N - 1);
            float bulge = 1.0f - t * t;
            for (int c = 0; c < 3; c++) {
                outer[i][c] = f->pos[c] + e[c] * t * (one ? 30.0f : 24.0f) * sc + d[c] * bulge * (one ? 6.0f : 3.0f) * sc;
                inner[i][c] = outer[i][c] - d[c] * bulge * (one ? 6.0f : 4.0f) * sc;
            }
            co[i] = rgba(235, 250, 255, a);
            ci[i] = rgba(90, 200, 255, a * 0.2f);
        }
        build_ribbon(outer, inner, co, ci, N);
    }
}

/** An ember (the Shackled Sun's): a fire star, a white-gold core in an orange flame turning
 *  the other way. Its own colours, not the crimson palette: it is the sun's fire */
static void draw_orb(const SkillFx *f)
{
    float g = f->delay > 0.0f ? fminf(1.0f, f->age / f->delay) : 1.0f;     // charging: it swells
    float a = fade(f, 0.0f, 0.15f);
    if (a < 0.2f) return;   // see draw_blade
    float R = 4.0f * f->scale * (0.25f + 0.75f * g) * (1.0f + 0.12f * sinf(f->age * 23.0f));
    bool hostile = s_drawHostile;
    s_drawHostile = false;
    for (int layer = 1; layer >= 0; layer--) {     // the flame, then the core over it (no depth writes)
        float r = layer ? R * 1.8f : R * 0.85f, sp = (layer ? -5.0f : 7.0f) * f->age + layer * 0.785f, c = cosf(sp), sn = sinf(sp);
        uint32_t tip = layer ? rgba(255, 60, 10, 0.0f) : rgba(255, 255, 235, a), mid = layer ? rgba(255, 120, 30, a * 0.6f) : rgba(255, 220, 110, a);
        int base = build_reserve(6, 8);
        const float *p = f->pos;
        build_vert((float[3]){ p[0], p[1] + r, p[2] }, tip);
        build_vert((float[3]){ p[0], p[1] - r, p[2] }, tip);
        for (int k = 0; k < 4; k++) {
            float ang = k * T3D_PI * 0.5f, x = cosf(ang), z = sinf(ang);
            build_vert((float[3]){ p[0] + (x * c - z * sn) * r, p[1], p[2] + (x * sn + z * c) * r }, mid);
        }
        for (int k = 0; k < 4; k++) {
            build_tri(base, base + 2 + k, base + 2 + (k + 1) % 4);
            build_tri(base + 1, base + 2 + (k + 1) % 4, base + 2 + k);
        }
    }
    s_drawHostile = hostile;
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
    bool hostile = s_drawHostile;
    if (f->warn) s_drawHostile = false;     // red as it is (the palette swap would turn it blue)
    for (int k = 0; k < SPIKES; k++) {
        float ang = f->yaw + 2.0f * T3D_PI * k / SPIKES + (k & 1) * 0.3f;
        float len = ((k & 1) ? 9.0f : 14.0f) * grow;
        float c = cosf(ang), s = sinf(ang), cw = cosf(ang + 0.35f), sw = sinf(ang + 0.35f);
        float p0[3] = { f->pos[0] + c * 2.0f,  f->pos[1],              f->pos[2] + s * 2.0f };
        float p1[3] = { f->pos[0] + cw * 2.0f, f->pos[1],              f->pos[2] + sw * 2.0f };
        float tip[3] = { f->pos[0] + c * len,  f->pos[1] + len * 0.8f, f->pos[2] + s * len };
        uint32_t c0 = f->warn ? rgba(255, 40, 25, a) : rgba(255, 210, 80, a);
        build_vert(p0, c0);
        build_vert(p1, c0);
        build_vert(tip, f->warn ? rgba(255, 140, 110, a * 0.3f) : rgba(255, 245, 190, a * 0.3f));
        build_tri(base + k*3, base + k*3 + 1, base + k*3 + 2);
    }
    s_drawHostile = hostile;
}

// Its fire's heat (when loaded: the Fallen Warrior's fight): n64guy's CC0 "N64 Fire" (the Flat
// loop, as intensity) running up through the tongues, its frames in step with the clock: a texel's
// heat between the tongue's own colour (hot) and a deep red (cool), the tongue's shape and fade its
// own. The two lines a half-tile apart along (never in step)
static sprite_t *s_fireTex;
static bool s_fireST;           // draw_fire_strip: give the strip's corners UVs
void skill_fx_fire_texture(bool load)
{
    if (load && !s_fireTex) s_fireTex = sprite_load("rom:/fx/fire_flat.sprite");
    else if (!load && s_fireTex) { rspq_wait(); sprite_free(s_fireTex); s_fireTex = NULL; }
}

static void vert_st(int vi, float s0, float t0)
{
    T3DVertPacked *v = &s_vb[vi >> 1];
    int16_t ss = (int16_t)lroundf(s0 * 32.0f), tt = (int16_t)lroundf(t0 * 32.0f);     // 10.5 texels
    if (vi & 1) { v->stB[0] = ss; v->stB[1] = tt; } else { v->stA[0] = ss; v->stA[1] = tt; }
}

/** The fire trail: per line a strip from the floor up to a ragged, flickering edge: tongues
 *  (FIRE_SUB to a stretch, each its own height, licking up and down and swaying) with low
 *  troughs between, over a yellow core strip. Lit, it rises in 0.15 s; burnt out, it fades to grey
 *  and away */
#define FIRE_SUB 4          // the outer flame's vertices to a stretch: tongue, trough, tongue, trough
static void draw_fire_strip(int i0, int n, float side, int layer)
{
    enum { MAXV = (FIRE_PTS - 1) * FIRE_SUB + 1, CHUNK = 33 };
    float P[FIRE_PTS][3], H[FIRE_PTS], S[FIRE_PTS];     // each point's base, flame height, smoke (0..1)
    for (int j = 0; j < n; j++) {
        const FirePt *p = &s_fire[i0 + j];
        float age = s_fireT - p->born, up = fminf(1.0f, age / 0.15f), sm = fmaxf(0.0f, fminf(1.0f, (age - p->burn) / FIRE_SMOKE));
        P[j][0] = p->x + p->sx * side * FIRE_SIDE; P[j][1] = p->y + 0.3f; P[j][2] = p->z + p->sz * side * FIRE_SIDE;
        H[j] = up * (layer ? 4.0f * (1.0f - sm) : 12.0f); S[j] = sm;
    }
    int sub = layer ? 1 : FIRE_SUB, nv = (n - 1) * sub + 1;
    static float top[MAXV][3], base[MAXV][3];
    static uint32_t ct[MAXV], cb[MAXV];
    const FirePt *f0 = &s_fire[i0];
    for (int v = 0; v < nv; v++) {
        int j = v / sub; float u = (float)(v - j * sub) / sub;
        if (j >= n - 1) { j = n - 2; u = 1.0f; }
        float h = H[j] + (H[j + 1] - H[j]) * u, sm = S[j] + (S[j + 1] - S[j]) * u;
        float ph = f0->born * 37.0f + v * 1.93f + side * 1.3f;     // each tongue's own flicker
        if (!layer) h *= (v & 1) ? 0.62f + 0.38f * fm_sinf(s_fireT * 14.0f + ph) * fm_sinf(s_fireT * 5.3f + ph * 0.7f) + 0.25f : 0.3f + 0.1f * fm_sinf(s_fireT * 9.0f + ph);
        else h *= 0.8f + 0.2f * fm_sinf(s_fireT * 11.0f + ph);
        float lick = (v & 1) && !layer ? 1.8f * fm_sinf(s_fireT * 8.0f + ph) : 0.0f;    // a tongue's tip swaying along the line
        for (int c = 0; c < 3; c++) base[v][c] = P[j][c] + (P[j + 1][c] - P[j][c]) * u;
        top[v][0] = base[v][0] + f0->fx * lick; top[v][1] = base[v][1] + h; top[v][2] = base[v][2] + f0->fz * lick;
        if (layer) { cb[v] = rgba(255, 230, 130, fmaxf(0.0f, 1.0f - 2.0f * sm)); ct[v] = rgba(255, 160, 50, 0.0f); }
        else {
            float a = 1.0f - sm;
            cb[v] = rgba((int)(255 - 140 * sm), (int)(130 - 25 * sm), (int)(30 + 70 * sm), a * (1.0f - 0.35f * sm));     // to grey smoke
            ct[v] = rgba((int)(230 - 140 * sm), (int)(50 + 35 * sm), (int)(12 + 70 * sm), (v & 1) ? 0.0f : a * 0.35f);
        }
    }
    static float along[MAXV];
    along[0] = side > 0.0f ? 16.0f : 0.0f;
    for (int v = 1; v < nv; v++) along[v] = along[v - 1] + sqrtf((base[v][0] - base[v - 1][0]) * (base[v][0] - base[v - 1][0]) + (base[v][2] - base[v - 1][2]) * (base[v][2] - base[v - 1][2])) * 1.6f;
    float rise = s_fireT * 40.0f;       // its heat rising through it
    for (int v = 0; v + 1 < nv; v += CHUNK - 1) {       // build_ribbon takes up to 34 at once
        int m = nv - v < CHUNK ? nv - v : CHUNK;
        build_ribbon(&top[v], &base[v], &ct[v], &cb[v], m);
        if (s_fireST) {
            int b0 = s_nverts - 2 * m;
            for (int i = 0; i < m; i++) {
                vert_st(b0 + 2 * i, along[v + i], rise - (top[v + i][1] - base[v + i][1]) * 2.0f);
                vert_st(b0 + 2 * i + 1, along[v + i], rise);
            }
        }
    }
}

static void draw_fire_trail(void)
{
    bool hostile = s_drawHostile;
    s_drawHostile = false;      // fire's own colours
    if (s_fireTex) {        // with its heat in it
        build_flush();
        surface_t all = sprite_get_pixels(s_fireTex), sub = surface_make_sub(&all, 0, ((int)(s_fireT * 30.0f) % 30) * 32, 32, 32);
        rdpq_sync_pipe();
        rdpq_mode_combiner(RDPQ_COMBINER1((SHADE, ENV, TEX0, ENV), (0, 0, 0, SHADE)));     // cool texels deep orange, hot ones the tongue's own
        rdpq_set_env_color(RGBA32(215, 88, 16, 255));     // (deep red dimmed it all)
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_tex_upload(TILE0, &sub, &(rdpq_texparms_t){ .s.repeats = REPEAT_INFINITE, .t.repeats = REPEAT_INFINITE });
        t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_TEXTURED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT);
        s_fireST = true;
    }
    for (int i = 0; i < s_fireN; ) {
        int n = 1;
        while (i + n < s_fireN && s_fire[i + n].id == s_fire[i].id) n++;
        if (n >= 2)
            for (int side = -1; side <= 1; side += 2)
                for (int layer = 0; layer < 2; layer++) draw_fire_strip(i, n, (float)side, layer);
        i += n;
    }
    if (s_fireST) {
        build_flush();
        s_fireST = false;
        rdpq_sync_pipe();
        rdpq_mode_filter(FILTER_POINT);
        rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
        t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT);
    }
    s_drawHostile = hostile;
}

/** The Fallen Warrior's landing crown: a ring spreading out over the floor, a circle of spikes
 *  rising from it, pointed, leaning out; fading as it spreads (no hit) */
static void draw_crown(const SkillFx *f)
{
    enum { SPIKES = 14 };
    bool hostile = s_drawHostile;
    float k = fminf(1.0f, f->age / f->life), e = 1.0f - (1.0f - k) * (1.0f - k);
    float R = 4.0f + 16.0f * e, H = 7.0f * (1.0f - 0.6f * k), a = 1.0f - k * k;
    s_drawHostile = false;      // its own colours (the palette swap would turn pale gold to mauve)
    uint32_t base = rgba(255, 236, 160, a), tipc = rgba(255, 250, 215, a * 0.35f);     // pale gold, the Fallen Warrior's too
    int b0 = build_reserve(SPIKES * 3, SPIKES);
    for (int i = 0; i < SPIKES; i++) {
        float a0 = f->yaw + 2.0f * T3D_PI * i / SPIKES, a1 = a0 + 2.0f * T3D_PI / SPIKES, am = (a0 + a1) * 0.5f;
        float h = H * ((i & 1) ? 0.7f : 1.0f);      // alternating points: a crown
        build_vert((float[3]){ f->pos[0] + cosf(a0) * R, f->pos[1] + 0.5f, f->pos[2] + sinf(a0) * R }, base);
        build_vert((float[3]){ f->pos[0] + cosf(a1) * R, f->pos[1] + 0.5f, f->pos[2] + sinf(a1) * R }, base);
        build_vert((float[3]){ f->pos[0] + cosf(am) * (R + 2.5f), f->pos[1] + h, f->pos[2] + sinf(am) * (R + 2.5f) }, tipc);
        build_tri(b0 + i * 3, b0 + i * 3 + 1, b0 + i * 3 + 2);
    }
    s_drawHostile = hostile;
}

static void draw_begin(void)
{
    if (!s_quarter) {
        s_quarter = malloc_uncached(sizeof(T3DMat4FP));
        T3DMat4 id;
        t3d_mat4_identity(&id);
        id.m[0][0] = id.m[1][1] = id.m[2][2] = 0.25f;     // the vertices are in quarter units (build_vert)
        t3d_mat4_to_fixed(s_quarter, &id);
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
    t3d_matrix_push(s_quarter);

    s_nverts = s_ntris = 0;
    s_vb = gpu_alloc(sizeof(T3DVertPacked) * (BUILD_MAX_VERTS / 2));
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
    any = any || s_fireN > 1;
    if (!any) return;

    draw_begin();
    for (int i = 0; i < SKILL_FX_MAX; i++) {
        const SkillFx *f = &s_fx[i];
        s_drawHostile = f->hostile;
        switch (f->type) {
            case FX_ARC:    draw_arc(f); break;
            case FX_WAVE:   if (f->shape == SKILL_WAVE_ORB) draw_orb(f); else if (f->shape != SKILL_WAVE_BLADE) draw_xwave(f); else draw_blade(f); break;
            case FX_PILLAR: draw_pillar(f); break;
            case FX_BURST:  draw_burst(f); break;
            case FX_CROWN:  draw_crown(f); break;
            default: break;
        }
    }
#ifndef HLE_FIRE_NODRAW     // test aid: the fire trail's cost (not drawn)
    draw_fire_trail();
#endif
    draw_end();
}

void skill_fx_draw_vortex(const float top[3], float depth, float radius, float time)
{
    // The ending's tunnel, as the video's: a solid funnel of near-straight streaks, red and
    // orange, slightly twisted, with narrow violet streaks spinning faster through it
    enum { BASE = 16, STREAKS = 12, SEG = 8 };
    draw_begin();
    for (int pass = 0; pass < 2; pass++) {
        int n = pass ? STREAKS : BASE;
        for (int k = 0; k < n; k++) {
            float outer[SEG][3], inner[SEG][3];
            uint32_t co[SEG], ci[SEG];
            float spin = time * (pass ? 1.9f : 0.7f) + (pass ? k * 0.37f : 0.0f);
            for (int i = 0; i < SEG; i++) {
                float u = (float)i / (SEG - 1);
                float r = radius * powf(1.0f - u, 1.25f) + 3.0f;
                float a = 2.0f * T3D_PI * k / n + spin + u * (pass ? 1.0f : 0.6f);
                float w = pass ? 0.07f + 0.05f * (1.0f - u) : T3D_PI / n + 0.02f;   // the base strands touch: no gaps
                float y = top[1] - depth * u;
                outer[i][0] = top[0] + r * cosf(a - w); outer[i][1] = y; outer[i][2] = top[2] + r * sinf(a - w);
                inner[i][0] = top[0] + r * cosf(a + w); inner[i][1] = y; inner[i][2] = top[2] + r * sinf(a + w);
                float al = pass ? 0.85f - 0.4f * u : 0.95f;
                if (pass) { co[i] = rgba(150, 50, 235, al); ci[i] = rgba(215, 110, 255, al); }
                else if (k & 1) { co[i] = rgba(235, 70, 30, al); ci[i] = rgba(255, 140, 40, al); }
                else { co[i] = rgba(255, 120, 30, al); ci[i] = rgba(250, 70, 40, al); }
            }
            build_ribbon(outer, inner, co, ci, SEG);
        }
    }
    draw_end();
}
