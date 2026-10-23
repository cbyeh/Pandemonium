#include <libdragon.h>
#include "gpu_frame.h"
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>
#include <t3d/t3dmodel.h>
#include <math.h>
#include <string.h>

#include "sun_boss.h"
#include "character.h"
#include "globals.h"
#include "general_utility.h"
#include "game_time.h"
#include "fx/proc_mesh.h"
#include "fx/skill_fx.h"
#include "fx/screen_shake.h"
#include "controllers/audio_controller.h"
#include "../../scenes/guardian/guardian_scene_sfx.h"
#include "../../scenes/guardian/guardian_scene_bounds.h"

// ---- Tuning ----
#define SUN_R          14.0f           // world radius of the eye
#define HOVER_H        24.0f           // centre height above the floor (in sword reach)
#define REST_H         12.0f           // resting on the floor after a dive
#define DROP_H         140.0f          // starts up in the ceiling
#define INTRO_TIME     3.0f
#define DRIFT_SPEED    32.0f
#define KEEP_DIST      36.0f           // hovers within sword reach (at 55 it backed out of every swing)
#define MESH_R         16.0f           // the eye mesh is built at this radius (1/8-unit storage)

typedef enum { SB_INTRO, SB_HOVER, SB_GLARE, SB_CORONA, SB_DIVE, SB_REST, SB_LASH, SB_SWEEP, SB_BREAK, SB_DEAD } SbState;

// ---- Shackles: iron chains hanging from the sun to weights that drag on the floor ----
#define NCH        4               // chains
#define NN         8               // rope nodes per chain (NN-1 links): few and heavy, as each costs CPU
#define SEG        (55.0f / 7)     // node spacing
#define CHAIN_LEN  (SEG * (NN - 1))
#define LINK_LEN   10.5f
#define NLINKS     (NCH * (NN - 1))
#define MAT_RING   3               // frames the RSP may still be reading
#define LK_NS      6               // a link: a flat oval band of LK_NS segments (every other one turned
#define LK_V       (LK_NS * 2)     // a quarter, so the chain reads round): outer and inner edge
#define LK_PER_LOAD 5              // links per vertex load (60 of the 70-entry cache)

typedef struct {
    float p[NN][3], q[NN][3];      // positions, previous positions (verlet)
    float az, el;                  // root on the sphere, relative to the eye's yaw
    bool kin;                      // posed by an attack (lash/sweep) instead of hanging free
    float kt[NN][3], kk;           // the pose and how hard it pulls
    bool hot;                      // glowing: about to strike
} Chain;

#define RAIN_MAX 8

static struct {
    bool ready;
    ProcMesh eye, eyeHot;
    ProcMesh weight;
    ProcMesh corona[2], coronaHot[2];   // two rings of flames, turning opposite ways
    T3DMat4FP *chainMat;           // MAT_RING * (1 + NCH): the links' origin, then the weights
    T3DVertPacked *linkVB[MAT_RING];
    rspq_block_t *linkBlock[MAT_RING];  // draws its buffer's links: the topology never changes
    int matRing;
    Chain ch[NCH];
    bool chainsReady, weights;
    int lashChain, lashLeft;
    float strikeDir[2], sweepFrom, sweepSpan;
    bool hitDone;
    SbState state;
    float t, life;          // time in state, total time
    int step;
    float groundY, orbit;
    bool unshackled;
    float cdGlare, cdRain, cdCorona, cdDive, cdLash, cdSweep;
    float rainT[RAIN_MAX], rainPos[RAIN_MAX][2];
    float diveTarget[2];
    float raysSpin;
    float chainAcc;         // time gathered for the chains' next step (every other frame)
    float downT;            // knocked down by a full stagger: time since it hit the floor (< 0: not)
    float knock[2];         // a half stagger's shove, per second, easing off
    bool chainStepped;      // stepped this frame: drawn halfway from the last state
} s;


static void links_init(void);

static inline float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

static float to_player(const Boss *b, float *dx, float *dz)
{
    *dx = character.pos[0] - b->pos[0];
    *dz = character.pos[2] - b->pos[2];
    return sqrtf(*dx * *dx + *dz * *dz);
}

static void face_player(Boss *b, float dt, float rate)
{
    float dx, dz;
    to_player(b, &dx, &dz);
    float d = atan2f(dx, dz) - b->rot[1];
    while (d >  T3D_PI) d -= 2.0f * T3D_PI;
    while (d < -T3D_PI) d += 2.0f * T3D_PI;
    float m = rate * dt;
    b->rot[1] += (d > m) ? m : (d < -m) ? -m : d;
}

static void move_toward(Boss *b, float x, float z, float speed, float dt)
{
    float dx = x - b->pos[0], dz = z - b->pos[2], d = sqrtf(dx * dx + dz * dz);
    if (d < 1e-3f) return;
    float step = fminf(d, speed * dt);
    float nx = b->pos[0] + dx / d * step, nz = b->pos[2] + dz / d * step;
    if (scene_bounds_check_room(nx, s.groundY, nz)) { b->pos[0] = nx; b->pos[2] = nz; }
}

static void enter(SbState st) { s.state = st; s.t = 0.0f; s.step = 0; }

static void sfx_boom(void) { audio_play_scene_sfx_dist(SCENE1_SFX_BOSS_SMASH1, 1.0f, 0.0f); }
static void sfx_cast(void) { audio_play_scene_sfx_dist(SCENE1_SFX_BOSS_SWING2, 1.0f, 0.0f); }
static void sfx_chain(int k) { audio_play_scene_sfx_dist(k ? SCENE1_SFX_CHAIN2 : SCENE1_SFX_CHAIN1, 1.0f, 0.0f); }

static float speed_mult(void) { return s.unshackled ? 1.35f : 1.0f; }
static float hurt(float d) { return s.unshackled ? d * 1.2f : d; }     // unshackled, it hits 20% harder

// ---- Lifetime ----

void sun_boss_free(void)
{
    if (!s.ready) return;
    gpu_wait();
    proc_mesh_free(&s.eye);
    proc_mesh_free(&s.eyeHot);
    for (int i = 0; i < MAT_RING; i++) { rspq_block_free(s.linkBlock[i]); free_uncached(s.linkVB[i]); }
    proc_mesh_free(&s.weight);
    for (int i = 0; i < 2; i++) { proc_mesh_free(&s.corona[i]); proc_mesh_free(&s.coronaHot[i]); }
    free_uncached(s.chainMat);
    memset(&s, 0, sizeof(s));
}

void sun_boss_begin(Boss* boss)
{
    if (!s.ready) {
        proc_mesh_begin(); proc_mesh_eye_sphere(MESH_R, true, false); proc_mesh_end(&s.eye);
        proc_mesh_begin(); proc_mesh_eye_sphere(MESH_R, true, true);  proc_mesh_end(&s.eyeHot);
        links_init();
        for (int i = 0; i < MAT_RING; i++) {
            s.linkVB[i] = malloc_uncached(sizeof(T3DVertPacked) * NLINKS * LK_V / 2);
            gpu_block_begin();
            for (int l = 0; l < NLINKS; l += LK_PER_LOAD) {
                int cnt = NLINKS - l < LK_PER_LOAD ? NLINKS - l : LK_PER_LOAD;
                t3d_vert_load(&s.linkVB[i][l * LK_V / 2], 0, cnt * LK_V);
                for (int j = 0; j < cnt; j++)
                    for (int k = 0; k < LK_NS; k++) {
                        int o = j * LK_V, k1 = (k + 1) % LK_NS;
                        t3d_tri_draw(o + k * 2, o + k1 * 2, o + k1 * 2 + 1);
                        t3d_tri_draw(o + k * 2, o + k1 * 2 + 1, o + k * 2 + 1);
                    }
                t3d_tri_sync();
            }
            s.linkBlock[i] = gpu_block_end();
        }
        proc_mesh_begin();
        proc_mesh_frustum((float[3]){ 0, -3.0f, 0 }, 5.0f, 3.6f, 6.0f, 6, proc_rgb(70, 60, 56));
        proc_mesh_box((float[3]){ 0, 3.6f, 0 }, (float[3]){ 1.2f, 1.0f, 1.2f }, proc_rgb(95, 82, 74));
        proc_mesh_end(&s.weight);
        for (int i = 0; i < 2; i++) {
            proc_mesh_begin(); proc_mesh_corona(MESH_R, i ? 11 : 16, MESH_R * (i ? 0.45f : 0.8f), i, false); proc_mesh_end(&s.corona[i]);
            proc_mesh_begin(); proc_mesh_corona(MESH_R, i ? 11 : 16, MESH_R * (i ? 0.5f : 0.95f), i, true); proc_mesh_end(&s.coronaHot[i]);
        }
        s.chainMat = malloc_uncached(sizeof(T3DMat4FP) * MAT_RING * (1 + NCH));
        s.ready = true;
    }
    boss->phaseIndex = SUN_BOSS_PHASE;
    boss->name = "The Shackled Sun";
    boss->health = boss->maxHealth;
    boss->mode = BOSS_MODE_CINEMATIC;       // not damageable until it has come down
    boss->state = BOSS_STATE_INTRO;
    boss->poise = boss->poiseIdleT = boss->staggerT = boss->recoilT = 0.0f; boss->halfDone = false;
    boss->damageFlashTimer = 0.0f;
    boss->velX = boss->velZ = 0.0f;
    boss->handAttackColliderActive = false;
    boss->sphereAttackColliderActive = false;
    boss->visible = true;
    // The ball and the base of its corona (centre at pos): a little past the eye drawn, as a
    // blade that grazed the eye (radius 14) missed a capsule of 12 most swings
    // reaching down to the floor at full width: a low swing under the ball still counts
    boss->capsuleCollider.localCapA = (T3DVec3){{ 0.0f, -20.0f, 0.0f }};
    boss->capsuleCollider.localCapB = (T3DVec3){{ 0.0f, 6.0f, 0.0f }};
    boss->capsuleCollider.radius = SUN_R + 4.0f;

    s.groundY = character.pos[1];
    boss->pos[1] = s.groundY + DROP_H;
    s.unshackled = false;
    s.life = 0.0f;
    s.orbit = 0.0f;
    s.cdGlare = 2.0f; s.cdRain = 6.0f; s.cdCorona = 10.0f; s.cdDive = 14.0f; s.cdLash = 4.0f; s.cdSweep = 9.0f;
    s.chainsReady = false;
    s.weights = true;
    s.downT = -1.0f; s.knock[0] = s.knock[1] = 0.0f;
    for (int i = 0; i < RAIN_MAX; i++) s.rainT[i] = -1.0f;
    enter(SB_INTRO);
}

void sun_boss_skip_intro(Boss* boss)
{
    if (!s.ready || s.state != SB_INTRO) return;
    boss->pos[1] = s.groundY + HOVER_H;
    boss->mode = BOSS_MODE_COMBAT;
    boss->state = BOSS_STATE_NEUTRAL;
    enter(SB_HOVER);
}

// ---- Attacks (hostile skill effects, crimson) ----

static void glare_update(Boss *b, float dt)
{
    // The eye narrows on the player, then looses crossed waves one after another,
    // each aimed where the player is: keep moving
    face_player(b, dt, 3.0f);
    int n = s.unshackled ? 4 : 3;
    const float WIND = 1.0f / speed_mult(), GAP = 0.32f, SPEED = 58.0f;
    if (s.step < n && s.t >= WIND + s.step * GAP) {
        float dx, dz, d = to_player(b, &dx, &dz);
        float a = atan2f(dx, dz) + ((s.step & 1) ? 0.12f : -0.12f) * (s.step > 0);
        float dy = (character.pos[1] + 10.0f) - b->pos[1];
        float h = sqrtf(d * d + dy * dy);
        float vy = h > 1e-3f ? dy / h * SPEED : 0.0f, vh = h > 1e-3f ? d / h * SPEED : SPEED;
        float o[3] = { b->pos[0] + sinf(a) * SUN_R, b->pos[1], b->pos[2] + cosf(a) * SUN_R };
        skill_fx_spawn_wave_hostile(SKILL_WAVE_X, o, (float[3]){ sinf(a) * vh, vy, cosf(a) * vh },
                                    hurt(10.0f), 1, 0.0f, 2.4f, false);
        sfx_cast();
        s.step++;
    }
    if (s.t > WIND + n * GAP + 0.5f) enter(SB_HOVER);
}

static void rain_spawn(void)
{
    // Telegraphs on and around the player; each turns into a pillar of light
    int n = s.unshackled ? 8 : 5;
    for (int i = 0; i < n && i < RAIN_MAX; i++) {
        float ox = 0.0f, oz = 0.0f;
        if (i > 0) {
            float a = (rand_custom_u32() % 628) / 100.0f, r = 12.0f + (rand_custom_u32() % 30);
            ox = cosf(a) * r; oz = sinf(a) * r;
        }
        s.rainPos[i][0] = character.pos[0] + ox;
        s.rainPos[i][1] = character.pos[2] + oz;
        s.rainT[i] = 1.0f + i * 0.12f;
        skill_fx_spawn_burst_hostile((float[3]){ s.rainPos[i][0], s.groundY, s.rainPos[i][1] }, 0.0f);
    }
}

static void rain_tick(float dt)
{
    for (int i = 0; i < RAIN_MAX; i++) {
        if (s.rainT[i] < 0.0f) continue;
        s.rainT[i] -= dt;
        if (s.rainT[i] < 0.0f) {
            float p[3] = { s.rainPos[i][0], s.groundY, s.rainPos[i][1] };
            skill_fx_spawn_wave_hostile(SKILL_WAVE_BLADE, p, (float[3]){ 0, 0, 0 }, hurt(14.0f), 1, 0.0f, 0.9f, false);
            if (i == 0) screen_shake_set_shake_mag(4.0f);
        }
    }
}

static void corona_update(Boss *b, float dt)
{
    // A long wind-up (the rays spin up, the ground glows), then a ring of fire all
    // around: roll through it or be far away
    if (s.step == 0) {
        s.raysSpin = 6.0f;
        if ((int)(s.t * 6.0f) != (int)((s.t - dt) * 6.0f))
            skill_fx_spawn_burst_hostile((float[3]){ b->pos[0], s.groundY, b->pos[2] }, s.t * 4.0f);
        if (s.t >= 1.3f / speed_mult()) {
            float o[3] = { b->pos[0], s.groundY + 6.0f, b->pos[2] };
            skill_fx_spawn_arc_hostile(o, b->rot[1], 85.0f, hurt(22.0f));
            skill_fx_spawn_arc_hostile(o, b->rot[1] + T3D_PI, 85.0f, hurt(22.0f));
            screen_shake_set_shake_mag(12.0f);
            sfx_boom();
            s.step = 1;
        }
    }
    if (s.t > 2.2f) enter(SB_HOVER);
}

static void dive_update(Boss *b, float dt)
{
    // Rise, track the player, then drop on it and rest on the floor (open to attack)
    if (s.step == 0) {
        b->pos[1] += (s.groundY + 75.0f - b->pos[1]) * fminf(1.0f, dt * 4.0f);
        move_toward(b, character.pos[0], character.pos[2], 70.0f * speed_mult(), dt);
        if (s.t >= 1.1f) { s.diveTarget[0] = character.pos[0]; s.diveTarget[1] = character.pos[2]; s.step = 1; s.t = 0.0f;
                           skill_fx_spawn_burst_hostile((float[3]){ s.diveTarget[0], s.groundY, s.diveTarget[1] }, 0.0f); }
    } else if (s.step == 1) {
        if (s.t >= 0.35f) { s.step = 2; s.t = 0.0f; }
    } else {
        move_toward(b, s.diveTarget[0], s.diveTarget[1], 200.0f, dt);
        b->pos[1] -= 260.0f * dt;
        if (b->pos[1] <= s.groundY + REST_H) {
            b->pos[1] = s.groundY + REST_H;
            float dx, dz;
            // A shockwave along the floor: wide, but jumping over it (Moon Blade Dance) avoids it
            if (to_player(b, &dx, &dz) < 34.0f && character.pos[1] - s.groundY < 6.0f) character_apply_damage(hurt(20.0f));
            skill_fx_spawn_burst_hostile((float[3]){ b->pos[0], s.groundY, b->pos[2] }, 0.0f);
            skill_fx_spawn_burst_hostile((float[3]){ b->pos[0], s.groundY, b->pos[2] }, 0.5f);
            screen_shake_set_shake_mag(14.0f);
            sfx_boom();
            enter(SB_REST);
        }
    }
}


bool sun_boss_stagger(Boss* b)
{
    // Knocked out of the air onto the floor, dimmed (as after a dive); not mid-dive or breaking free
    if (!s.ready || s.state == SB_INTRO || s.state == SB_DIVE || s.state == SB_REST || s.state == SB_BREAK || s.state == SB_DEAD) return false;
    for (int k = 0; k < NCH; k++) s.ch[k].kin = s.ch[k].hot = false;
    skill_fx_spawn_burst_hostile((float[3]){ b->pos[0], s.groundY, b->pos[2] }, 0.0f);
    enter(SB_REST);
    s.t = 3.0f / speed_mult() - 2.5f;     // 2.5 s on the floor
    s.downT = 0.0f;         // it falls from where it was: a bounce and a rocking on the floor
    b->staggerT = 2.5f;
    return true;
}

void sun_boss_half_stagger(Boss* b)
{
    // Flinches: shoved back through the air, the corona flaring; a windup is broken off
    if (!s.ready || s.state == SB_INTRO || s.state == SB_DIVE || s.state == SB_REST || s.state == SB_BREAK || s.state == SB_DEAD) return;
    float dx, dz, d = to_player(b, &dx, &dz);
    if (d > 1e-3f) { s.knock[0] = -dx / d * 70.0f; s.knock[1] = -dz / d * 70.0f; }
    s.raysSpin = 7.0f;
    b->damageFlashTimer = 0.3f;
    bool winding = (s.state == SB_GLARE && s.step == 0) || (s.state == SB_CORONA && s.step == 0) ||
                   ((s.state == SB_LASH || s.state == SB_SWEEP) && s.step == 0);
    if (winding) {
        for (int k = 0; k < NCH; k++) s.ch[k].kin = s.ch[k].hot = false;
        enter(SB_HOVER);
    }
}

// ---- Shackles ----

static const float CHAIN_AZ[NCH] = { 1.2f, -1.2f, 2.35f, -2.35f };    // sides, then back sides

static void chain_root(const Boss *b, const Chain *c, float out[3])
{
    float a = b->rot[1] + c->az, ce = cosf(c->el);
    out[0] = b->pos[0] + sinf(a) * ce * SUN_R * 0.92f;
    out[1] = b->pos[1] + sinf(c->el) * SUN_R * 0.92f;
    out[2] = b->pos[2] + cosf(a) * ce * SUN_R * 0.92f;
}

static void chains_reset(const Boss *b)
{
    for (int k = 0; k < NCH; k++) {
        Chain *c = &s.ch[k];
        c->az = CHAIN_AZ[k]; c->el = -0.35f; c->kin = false; c->hot = false;
        float r[3]; chain_root(b, c, r);
        float a = b->rot[1] + c->az;
        for (int i = 0; i < NN; i++) {   // down to the floor, then out along it
            float drop = fminf(i * SEG, r[1] - s.groundY - 1.0f), out = sqrtf(fmaxf(0.0f, i * SEG * i * SEG - drop * drop));
            c->p[i][0] = r[0] + sinf(a) * out; c->p[i][1] = r[1] - drop; c->p[i][2] = r[2] + cosf(a) * out;
            memcpy(c->q[i], c->p[i], sizeof(c->p[i]));
        }
    }
    s.chainsReady = true;
}

static void chains_step(const Boss *b, float h)
{
    const float g = -320.0f * h * h;
    for (int k = 0; k < NCH; k++) {
        Chain *c = &s.ch[k];
        float r[3]; chain_root(b, c, r);
        for (int i = 1; i < NN; i++) {
            float *p = c->p[i], *q = c->q[i];
            for (int m = 0; m < 3; m++) { float v = (p[m] - q[m]) * 0.985f; q[m] = p[m]; p[m] += v; }
            p[1] += g;
            if (c->kin) {   // the pose pulls hardest at the root, so the tip lags like a whip
                float kk = c->kk * (1.0f - 0.55f * i / NN);
                for (int m = 0; m < 3; m++) p[m] += (c->kt[i][m] - p[m]) * kk;
            }
        }
        for (int it = 0; it < 5; it++) {
            memcpy(c->p[0], r, sizeof(r));
            for (int i = 0; i < NN - 1; i++) {
                float *a = c->p[i], *bb = c->p[i + 1];
                float d[3] = { bb[0] - a[0], bb[1] - a[1], bb[2] - a[2] };
                float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (l < 1e-4f) continue;
                // the root is pinned; a shackle weight is heavy
                float wa = (i == 0) ? 0.0f : 1.0f, wb = (i + 1 == NN - 1 && s.weights) ? 0.15f : 1.0f;
                float kf = (l - SEG) / l / (wa + wb);
                for (int m = 0; m < 3; m++) { a[m] += d[m] * kf * wa; bb[m] -= d[m] * kf * wb; }
            }
            for (int i = 2; i < NN; i++) {
                float *p = c->p[i];
                float dx = p[0] - b->pos[0], dy = p[1] - b->pos[1], dz = p[2] - b->pos[2];
                float d2 = dx * dx + dy * dy + dz * dz, rr = SUN_R + 1.5f;
                if (d2 < rr * rr && d2 > 1e-6f) { float kf = rr / sqrtf(d2); p[0] = b->pos[0] + dx * kf; p[1] = b->pos[1] + dy * kf; p[2] = b->pos[2] + dz * kf; }
                float floorY = s.groundY + ((i == NN - 1 && s.weights) ? 3.0f : 0.9f);
                if (p[1] < floorY) {   // on the floor: friction (the weights barely slide)
                    p[1] = floorY;
                    float f = (i == NN - 1 && s.weights) ? 0.2f : 0.55f;
                    c->q[i][0] = p[0] - (p[0] - c->q[i][0]) * f;
                    c->q[i][2] = p[2] - (p[2] - c->q[i][2]) * f;
                }
            }
        }
    }
}

// Straight pose from the root: angle `al` from straight up, in the plane of (dx, dz);
// nodes past `flat` are pressed onto the floor
static void chain_pose(const Boss *b, Chain *c, float dx, float dz, float al, bool flat)
{
    float r[3]; chain_root(b, c, r);
    for (int i = 0; i < NN; i++) {
        float d = i * SEG * 0.97f;
        c->kt[i][0] = r[0] + dx * sinf(al) * d;
        c->kt[i][1] = r[1] + cosf(al) * d;
        c->kt[i][2] = r[2] + dz * sinf(al) * d;
        if (flat) c->kt[i][1] += (s.groundY + 1.0f - c->kt[i][1]) * clamp01(i / 2.0f);
    }
}

static int nearest_chain(const Boss *b, float dx, float dz)
{
    // the chain whose root faces the player most (the eye always does, so a side one)
    float a = atan2f(dx, dz) - b->rot[1];
    int best = 0; float bd = 1e9f;
    for (int k = 0; k < NCH; k++) {
        float d = fabsf(remainderf(a - s.ch[k].az, 2.0f * T3D_PI)) + (k == s.lashChain ? 0.6f : 0.0f);
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}

// Shackle Lash: a chain glows, rears up behind the sun, then slams down along a line
// at the player (step aside). Unshackled, two in a row.
static void lash_start(Boss *b, int count)
{
    float dx, dz;
    to_player(b, &dx, &dz);
    s.lashChain = nearest_chain(b, dx, dz);
    s.lashLeft = count;
    s.hitDone = false;
    s.ch[s.lashChain].hot = true;
    enter(SB_LASH);
#ifdef HLE_AUTOPLAY
    debugf("[sun] lash x%d\n", count);
#endif
}

static void lash_update(Boss *b, float dt)
{
    Chain *c = &s.ch[s.lashChain];
    float wind = (s.unshackled ? 0.75f : 0.95f) * (s.lashLeft == 1 && s.unshackled ? 0.7f : 1.0f);
    face_player(b, dt, 2.0f);
    if (s.step == 0) {          // wind-up: track the player, the chain rises behind the sun
        float dx, dz, d = to_player(b, &dx, &dz);
        if (s.t < wind - 0.2f && d > 1e-3f) { s.strikeDir[0] = dx / d; s.strikeDir[1] = dz / d; }
        float k = clamp01(s.t / wind);
        c->kin = true; c->kk = 0.18f;
        chain_pose(b, c, s.strikeDir[0], s.strikeDir[1], T3D_PI * (1.0f + 0.82f * k), false);
        if (s.t >= wind * 0.45f && s.t - dt < wind * 0.45f)     // where it will land
            for (int i = 1; i <= 2; i++)
                skill_fx_spawn_burst_hostile((float[3]){ b->pos[0] + s.strikeDir[0] * CHAIN_LEN * 0.4f * i, s.groundY,
                                                         b->pos[2] + s.strikeDir[1] * CHAIN_LEN * 0.4f * i }, 0.0f);
        if (s.t >= wind) { s.step = 1; s.t = 0.0f; sfx_cast(); sfx_chain(0); }
    } else if (s.step == 1) {   // over the top and down onto the floor
        float k = clamp01(s.t / 0.2f);
        c->kk = 0.6f;
        chain_pose(b, c, s.strikeDir[0], s.strikeDir[1], T3D_PI * (1.82f + 0.72f * k * k), true);
        if (k >= 1.0f && !s.hitDone) {
            s.hitDone = true;
            // the player within reach of the line, on the ground
            float px = character.pos[0] - b->pos[0], pz = character.pos[2] - b->pos[2];
            float along = px * s.strikeDir[0] + pz * s.strikeDir[1], side = fabsf(px * s.strikeDir[1] - pz * s.strikeDir[0]);
#ifdef HLE_SUN_FORCE
            { float r[3]; chain_root(b, c, r); const float *tp = c->p[NN - 1];
              debugf("[sun] strike along %.0f side %.0f | tip from root %.0f,%.0f,%.0f kt %.0f,%.0f,%.0f\n", (double)along, (double)side,
                     (double)(tp[0]-r[0]), (double)(tp[1]-r[1]), (double)(tp[2]-r[2]),
                     (double)(c->kt[NN-1][0]-r[0]), (double)(c->kt[NN-1][1]-r[1]), (double)(c->kt[NN-1][2]-r[2])); }
#endif
            if (along > 0.0f && along < CHAIN_LEN + SUN_R + 4.0f && side < 9.0f && character.pos[1] - s.groundY < 8.0f)
                character_apply_damage(hurt(16.0f));
            for (int i = 1; i <= 3; i++)
                skill_fx_spawn_burst_hostile((float[3]){ b->pos[0] + s.strikeDir[0] * (SUN_R + CHAIN_LEN * 0.3f * i), s.groundY,
                                                         b->pos[2] + s.strikeDir[1] * (SUN_R + CHAIN_LEN * 0.3f * i) }, i * 0.7f);
            screen_shake_set_shake_mag(10.0f);
            sfx_boom();
            sfx_chain(1);
            s.step = 2; s.t = 0.0f;
        }
    } else {                    // let it lie, then let go
        if (s.t > 0.15f) c->kin = false;
        if (s.t > 0.4f) c->hot = false;
        if (s.t > (s.lashLeft > 1 ? 0.35f : 0.7f)) {
            if (--s.lashLeft > 0) {
                float dx, dz;
                to_player(b, &dx, &dz);
                int prev = s.lashChain;
                s.lashChain = nearest_chain(b, dx, dz);
                if (s.lashChain == prev) s.lashChain = (prev + 1) % NCH;
                s.ch[prev].hot = s.ch[prev].kin = false;    // it went on to the next before this one cooled (stayed orange)
                s.ch[s.lashChain].hot = true; s.hitDone = false;
                s.state = SB_LASH; s.t = 0.0f; s.step = 0;
            } else enter(SB_HOVER);
        }
    }
}

// Shackle Sweep (unshackled): a chain drops to the floor and scythes round the sun;
// jump it or roll through
static void sweep_update(Boss *b, float dt)
{
    Chain *c = &s.ch[s.lashChain];
    const float WIND = 1.0f, SWING = 0.7f;
    float dx, dz;
    to_player(b, &dx, &dz);
    if (s.step == 0) {
        if (s.t == 0.0f || s.t < dt * 1.5f) {
            float pa = atan2f(dx, dz);
            s.sweepSpan = (rand_custom_u32() & 1) ? 3.6f : -3.6f;
            s.sweepFrom = pa - s.sweepSpan * 0.5f;
        }
        c->kin = true; c->kk = 0.15f; c->hot = true;
        chain_pose(b, c, sinf(s.sweepFrom), cosf(s.sweepFrom), T3D_PI * 0.5f, true);
        if ((int)(s.t * 5.0f) != (int)((s.t - dt) * 5.0f)) {   // a ring of warnings at its reach
            float a = s.sweepFrom + s.sweepSpan * s.t / WIND;
            skill_fx_spawn_burst_hostile((float[3]){ b->pos[0] + sinf(a) * CHAIN_LEN * 0.8f, s.groundY,
                                                     b->pos[2] + cosf(a) * CHAIN_LEN * 0.8f }, a);
        }
        if (s.t >= WIND) { s.step = 1; s.t = 0.0f; s.hitDone = false; sfx_cast(); sfx_chain(1); }
    } else if (s.step == 1) {
        float k = clamp01(s.t / SWING), a0 = s.sweepFrom + s.sweepSpan * clamp01((s.t - dt) / SWING), a = s.sweepFrom + s.sweepSpan * k;
        c->kk = 0.5f;
        chain_pose(b, c, sinf(a), cosf(a), T3D_PI * 0.5f, true);
        // the chain swept past the player this frame
        float pa = atan2f(dx, dz), dist = sqrtf(dx * dx + dz * dz);
        float r0 = remainderf(pa - a0, 2.0f * T3D_PI), r1 = remainderf(pa - a, 2.0f * T3D_PI);
        if (!s.hitDone && (r0 > 0) != (r1 > 0) && fabsf(r0) < 1.0f && dist < SUN_R + CHAIN_LEN + 4.0f
            && character.pos[1] - s.groundY < 6.0f) {
            s.hitDone = true;
            character_apply_damage(hurt(14.0f));
        }
        if (k >= 1.0f) { s.step = 2; s.t = 0.0f; screen_shake_set_shake_mag(6.0f); }
    } else {
        if (s.t > 0.1f) c->kin = false;
        if (s.t > 0.3f) c->hot = false;
        if (s.t > 0.6f) enter(SB_HOVER);
    }
}

static void choose(Boss *b, float dist)
{
#ifdef HLE_SUN_FORCE   // test aid: 1 = lash, 2 = sweep, 3 = dive, 4 = corona, back to back
    if (HLE_SUN_FORCE != 9) { float dx, dz, d = to_player(b, &dx, &dz);   // within reach of the player
      if (d > 50.0f) { b->pos[0] = character.pos[0] - dx / d * 50.0f; b->pos[2] = character.pos[2] - dz / d * 50.0f; } }
    if (HLE_SUN_FORCE == 9) return;     // 9 = never attacks: it only hovers (can a sword reach it?)
    if (HLE_SUN_FORCE == 1) { lash_start(b, s.unshackled ? 2 : 1); return; }
    if (HLE_SUN_FORCE == 3) { enter(SB_DIVE); debugf("[sun] dive\n"); return; }
    if (HLE_SUN_FORCE == 4) { enter(SB_CORONA); debugf("[sun] corona\n"); return; }
    if (HLE_SUN_FORCE == 2) { float dx, dz; to_player(b, &dx, &dz); s.lashChain = nearest_chain(b, dx, dz); enter(SB_SWEEP); debugf("[sun] sweep\n"); return; }
#endif
    if (s.cdDive <= 0.0f && dist < 90.0f) { s.cdDive = 13.0f; enter(SB_DIVE); return; }
    if (s.cdCorona <= 0.0f && dist < 60.0f) { s.cdCorona = 11.0f; enter(SB_CORONA); return; }
    if (s.cdLash <= 0.0f && dist < CHAIN_LEN + SUN_R) { s.cdLash = s.unshackled ? 4.5f : 6.0f; lash_start(b, s.unshackled ? 2 : 1); return; }
    if (s.unshackled && s.cdSweep <= 0.0f && dist < CHAIN_LEN + SUN_R) {
        float dx, dz; to_player(b, &dx, &dz);
        s.cdSweep = 10.0f; s.lashChain = nearest_chain(b, dx, dz); enter(SB_SWEEP);
#ifdef HLE_AUTOPLAY
        debugf("[sun] sweep\n");
#endif
        return;
    }
    if (s.cdRain <= 0.0f) { s.cdRain = s.unshackled ? 6.0f : 8.0f; rain_spawn(); sfx_cast(); s.t = -0.3f; return; }
    if (s.cdGlare <= 0.0f) { s.cdGlare = s.unshackled ? 3.0f : 4.5f; enter(SB_GLARE); return; }
}

void sun_boss_update(Boss* b, float dt)
{
    if (!s.ready) return;
    if (b->damageFlashTimer > 0.0f) b->damageFlashTimer -= dt;
    s.t += dt;
    s.life += dt;
    float sm = speed_mult();
    s.cdGlare -= dt * sm; s.cdRain -= dt * sm; s.cdCorona -= dt * sm; s.cdDive -= dt * sm; s.cdLash -= dt * sm; s.cdSweep -= dt * sm;
    s.raysSpin += (1.0f - s.raysSpin) * fminf(1.0f, dt * 2.0f);
    rain_tick(dt);
    if (!s.chainsReady) chains_reset(b);

    if (s.knock[0] != 0.0f || s.knock[1] != 0.0f) {      // a half stagger's shove
        move_toward(b, b->pos[0] + s.knock[0], b->pos[2] + s.knock[1], sqrtf(s.knock[0] * s.knock[0] + s.knock[1] * s.knock[1]), dt);
        float k = fmaxf(0.0f, 1.0f - 5.0f * dt);
        s.knock[0] *= k; s.knock[1] *= k;
        if (fabsf(s.knock[0]) + fabsf(s.knock[1]) < 2.0f) s.knock[0] = s.knock[1] = 0.0f;
    }
    float dx, dz, dist = to_player(b, &dx, &dz);

    if (b->state == BOSS_STATE_DEAD && s.state != SB_DEAD) {
        enter(SB_DEAD);
        for (int k = 0; k < NCH; k++) s.ch[k].kin = s.ch[k].hot = false;
        for (int i = 0; i < RAIN_MAX; i++) s.rainT[i] = -1.0f;
    }
    // Half health: the shackles break
    if (!s.unshackled && s.state != SB_DEAD && b->health <= b->maxHealth * 0.5f) {
        s.unshackled = true;
        for (int k = 0; k < NCH; k++) s.ch[k].kin = s.ch[k].hot = false;
        b->mode = BOSS_MODE_CINEMATIC;
        enter(SB_BREAK);
    }


#ifdef HLE_TEST_DUMMY
    { extern void hle_open_tick(bool, float); hle_open_tick((s.state == SB_HOVER || s.state == SB_REST), dt); }   // test aid: measure the openings
#endif
    switch (s.state) {
    case SB_INTRO:
        // Comes down from the ceiling in its chains
        b->pos[1] = s.groundY + HOVER_H + (DROP_H - HOVER_H) * (1.0f - clamp01(s.t / (INTRO_TIME * 0.8f)));
        face_player(b, dt, 4.0f);
        // then waits, hovering, until the intro cutscene ends (sun_boss_skip_intro)
        break;

    case SB_HOVER: {
#ifdef HLE_SUN_FORCE    // test aid: a chain left glowing after an attack
        if (s.t <= dt) { int hot = 0; for (int k = 0; k < NCH; k++) hot += s.ch[k].hot; debugf("[sun] hover, %d chains hot\n", hot); }
#endif
        // Drift around the player at a distance, bobbing
        face_player(b, dt, 3.0f);
        s.orbit += dt * 0.35f * sm;
        float away = atan2f(-dx, -dz) + s.orbit * 0.3f;
        move_toward(b, character.pos[0] + sinf(away) * KEEP_DIST, character.pos[2] + cosf(away) * KEEP_DIST, DRIFT_SPEED * sm, dt);
        float hy = s.groundY + HOVER_H + sinf(s.life * 2.0f) * 3.0f;
        b->pos[1] += (hy - b->pos[1]) * fminf(1.0f, dt * 3.0f);
        if (s.t > 1.5f / sm) choose(b, dist);     // a pause between attacks: the opening
        break;
    }

    case SB_GLARE:  glare_update(b, dt); break;
    case SB_CORONA: corona_update(b, dt); break;
    case SB_DIVE:   dive_update(b, dt);  break;
    case SB_LASH:   lash_update(b, dt);  break;
    case SB_SWEEP:  sweep_update(b, dt); break;

    case SB_REST:
        // Dimmed on the floor: the opening (a stagger drops it there from the air)
        if (s.downT >= 0.0f) {      // knocked down: it falls, bounces twice, rocks, then gets up
            s.downT += dt;
            float fall = clamp01(s.downT / 0.25f), bt = s.downT - 0.25f;
            float bounce = bt > 0.0f ? fabsf(sinf(bt * 9.0f)) * 9.0f * expf(-bt * 4.0f) : 0.0f;
            float h = s.groundY + REST_H + bounce;
            b->pos[1] += (h - b->pos[1]) * (fall < 1.0f ? fminf(1.0f, dt * 14.0f) : 1.0f);
            if (bt > 0.0f && bt - dt <= 0.0f) { screen_shake_set_shake_mag(9.0f); sfx_boom();
                skill_fx_spawn_burst_hostile((float[3]){ b->pos[0], s.groundY, b->pos[2] }, 0.0f); }
        } else
        b->pos[1] += (s.groundY + REST_H - b->pos[1]) * fminf(1.0f, dt * 8.0f);
        if (s.t > 3.0f / sm) s.downT = -1.0f;
        if (s.t > 3.0f / sm) enter(SB_HOVER);
        break;

    case SB_BREAK:
        // The chains snap: bursts all around, then it burns hotter
        b->pos[1] += (s.groundY + HOVER_H + 10.0f - b->pos[1]) * fminf(1.0f, dt * 3.0f);
        s.raysSpin = 8.0f;
        if (s.weights && s.t >= 0.5f) {   // the weights burst and the chains whip free
            s.weights = false;
            sfx_chain(0); sfx_chain(1);
            for (int k = 0; k < NCH; k++) {
                skill_fx_spawn_burst_hostile((float[3]){ s.ch[k].p[NN - 1][0], s.groundY, s.ch[k].p[NN - 1][2] }, k * 0.8f);
                for (int i = 1; i < NN; i++) s.ch[k].q[i][1] -= 0.6f * i;
            }
            screen_shake_set_shake_mag(10.0f);
        }
        if (s.step < 6 && s.t >= s.step * 0.25f) {
            float a = s.step * 1.05f;
            skill_fx_spawn_burst_hostile((float[3]){ b->pos[0] + sinf(a) * 20.0f, s.groundY, b->pos[2] + cosf(a) * 20.0f }, a);
            screen_shake_set_shake_mag(8.0f);
            if (s.step == 0) sfx_boom();
            s.step++;
        }
        if (s.t > 2.0f) { b->mode = BOSS_MODE_COMBAT; s.cdCorona = 0.0f; enter(SB_HOVER); }
        break;

    case SB_DEAD:
        // Gutters out and sinks to the floor
        b->pos[1] += (s.groundY + 6.0f - b->pos[1]) * fminf(1.0f, dt * 1.5f);
        break;
    }
    b->rot[0] = b->rot[2] = 0.0f;

#ifdef HLE_LOG_CLOTH     // test aid: the chains' time a frame (simulation; drawing logged in sun_boss_draw)
    uint32_t t0 = TICKS_READ();
#endif
    // The chains step every other frame, on both frames' time, and are drawn in between: half
    // the cost (0.7 ms a frame on hardware), still moving every frame
    s.chainAcc += dt;
    s.chainStepped = !s.chainStepped && dt > 0.0f;
    if (s.chainStepped) { chains_step(b, fminf(s.chainAcc, 0.08f)); s.chainAcc = 0.0f; }
#ifdef HLE_LOG_CLOTH
    { static uint32_t sum; static int n; sum += TICKS_READ() - t0; if (++n == 120) { debugf("[chains] step %lu us a frame\n", (unsigned long)(TICKS_TO_US(sum) / n)); sum = 0; n = 0; } }
#endif
}

// ---- Links: every link of every chain in one vertex buffer a frame, transformed here.
// A matrix and a vertex load per link cost the fight 20 frames a second ----

static int16_t s_lkP[LK_V][2];  // one link, LINK_LEN long along y, in the xy plane, in 1/16 units

static void links_init(void)
{
    const float width = 2.6f, band = 1.6f, a = LINK_LEN * 0.5f - width;
    for (int i = 0; i < LK_NS; i++) {
        float t = 2.0f * T3D_PI * (i + 0.5f) / LK_NS, cx = cosf(t), cy = sinf(t);
        for (int e = 0; e < 2; e++) {
            float w = width + (e ? -band : band) * 0.5f;
            s_lkP[i * 2 + e][0] = (int16_t)lrintf(cx * w * 16.0f);
            s_lkP[i * 2 + e][1] = (int16_t)lrintf((cy * w + (cy >= 0 ? a : -a)) * 16.0f);
        }
    }
}

static inline void put_vert(T3DVertPacked *vb, int i, const int16_t pos[3], uint32_t rgba)
{
    T3DVertPacked *v = &vb[i >> 1];
    if (i & 1) { v->posB[0] = pos[0]; v->posB[1] = pos[1]; v->posB[2] = pos[2]; v->normB = 0; v->rgbaB = rgba; }
    else       { v->posA[0] = pos[0]; v->posA[1] = pos[1]; v->posA[2] = pos[2]; v->normA = 0; v->rgbaA = rgba; }
}

// One link between two rope nodes, every other one turned a quarter, into vb (1/8 units from o)
static void link_verts(T3DVertPacked *vb, int base, const float a[3], const float b[3], bool twist, bool hot, const float o[3])
{
    float y[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    float l = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    if (l < 1e-4f) { y[0] = 0; y[1] = 1; y[2] = 0; l = 1; }
    for (int m = 0; m < 3; m++) y[m] /= l;
    float up[3] = { 0, 1, 0 };
    if (fabsf(y[1]) > 0.9f) { up[0] = 1; up[1] = 0; }
    float x[3] = { y[1] * up[2] - y[2] * up[1], y[2] * up[0] - y[0] * up[2], y[0] * up[1] - y[1] * up[0] };
    float xl = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (int m = 0; m < 3; m++) x[m] /= xl;
    float z[3] = { x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0] };
    if (twist) for (int m = 0; m < 3; m++) { float t = x[m]; x[m] = z[m]; z[m] = -t; }
    float c[3] = { (a[0] + b[0]) * 0.5f - o[0], (a[1] + b[1]) * 0.5f - o[1], (a[2] + b[2]) * 0.5f - o[2] };
    uint32_t col = proc_rgb(255, 120, 40);     // hot: glowing, about to strike
    if (!hot) {     // iron, two-sided, lit by the key light of the proc meshes
        float d = fabsf(z[0] * -0.45f + z[1] * 0.8f + z[2] * 0.4f), k = 0.4f + 0.75f * d;
        col = proc_rgb((int)(118 * k), (int)(100 * k), (int)(88 * k));
    }
    // Integer from here: 1/8 units = c*8 + (axis*8*256 * local*16) >> 12
    int32_t C[3], X[3], Y[3];
    for (int m = 0; m < 3; m++) { C[m] = (int32_t)(c[m] * 8.0f); X[m] = (int32_t)(x[m] * 2048.0f); Y[m] = (int32_t)(y[m] * 2048.0f); }
    for (int v = 0; v < LK_V; v++) {
        int32_t px = s_lkP[v][0], py = s_lkP[v][1];
        int16_t pos[3];
        for (int m = 0; m < 3; m++) pos[m] = (int16_t)(C[m] + ((X[m] * px + Y[m] * py) >> 12));
        put_vert(vb, base + v, pos, col);
    }
}

static void chains_draw(const Boss *b)
{
    T3DMat4FP *mats = s.chainMat + s.matRing * (1 + NCH);
    int ring = s.matRing;
    // Written through the cache, then flushed for the RSP's DMA (uncached stores are slow)
    T3DVertPacked *vb = CachedAddr(s.linkVB[ring]);
    s.matRing = (s.matRing + 1) % MAT_RING;
    int n = 0;
    const float f = s.chainStepped ? 0.5f : 1.0f;      // from the last state (q) to p
    for (int k = 0; k < NCH; k++) {
        float P[NN][3];
        chain_root(b, &s.ch[k], P[0]);     // on the eye now, wherever it has moved since the step
        for (int i = 1; i < NN; i++)
            for (int m = 0; m < 3; m++) P[i][m] = s.ch[k].q[i][m] + (s.ch[k].p[i][m] - s.ch[k].q[i][m]) * f;
        for (int i = 0; i < NN - 1; i++, n++)
            link_verts(vb, n * LK_V, P[i], P[i + 1], i & 1, s.ch[k].hot, b->pos);
    }
    data_cache_hit_writeback(vb, sizeof(T3DVertPacked) * NLINKS * LK_V / 2);

    rdpq_sync_pipe();
    rdpq_mode_push();       // restored at the end: the caller's fog and modes stay
    rdpq_set_mode_standard();
    rdpq_mode_zbuf(true, true);
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT);
    t3d_mat4fp_from_srt_euler(&mats[0], (float[3]){ 1 / 8.0f, 1 / 8.0f, 1 / 8.0f }, (float[3]){ 0, 0, 0 }, b->pos);
    t3d_matrix_set(&mats[0], true);
    rspq_block_run(s.linkBlock[ring]);
    if (s.weights)
        for (int k = 0; k < NCH; k++) {
            float w[3];
            for (int m = 0; m < 3; m++) w[m] = s.ch[k].q[NN - 1][m] + (s.ch[k].p[NN - 1][m] - s.ch[k].q[NN - 1][m]) * (s.chainStepped ? 0.5f : 1.0f);
            t3d_mat4fp_from_srt_euler(&mats[1 + k], (float[3]){ 1 / 8.0f, 1 / 8.0f, 1 / 8.0f }, (float[3]){ 0, k * 0.7f, 0 }, w);
            t3d_matrix_set(&mats[1 + k], true);
            rspq_block_run(s.weight.block);
        }
    rdpq_mode_pop();
}

void sun_boss_draw(Boss* b)
{
    if (!s.ready || !b->visible) return;
    float dying = (s.state == SB_DEAD) ? clamp01(s.t / 3.0f) : 0.0f;
    float pulse = 1.0f + 0.04f * sinf(s.life * 5.0f) + (b->damageFlashTimer > 0.0f ? 0.08f : 0.0f);
    float scale = (SUN_R / MESH_R) * pulse * (1.0f - 0.5f * dying);
    static float restK;     // shrinks a little on the floor, easing (a stagger drops it there)
    restK += ((s.state == SB_REST ? 1.0f : 0.0f) - restK) * fminf(1.0f, deltaTime * 6.0f);
    scale *= 1.0f - 0.1f * restK;

    float yaw = -b->rot[1];     // Tiny3D's yaw turns +z the other way round
    // Knocked down: it rocks side to side on the floor, settling (and squashes as it lands)
    float rock = s.state == SB_REST && s.downT > 0.25f ? 0.45f * sinf((s.downT - 0.25f) * 7.0f) * expf(-(s.downT - 0.25f) * 1.2f) : 0.0f;
    scale *= 1.0f - 0.06f * boss_recoil(b);
    proc_mesh_set_transform(s.unshackled ? &s.eyeHot : &s.eye, scale, (float[3]){ rock, yaw, 0.0f }, b->pos);

    // Corona: two rings of flame tongues behind the eye, turning opposite ways
    static float spinAngle;
    spinAngle += deltaTime * 0.6f * s.raysSpin;
    float cs = scale * (1.0f + 0.05f * sinf(s.life * 7.0f)) * (1.0f - dying);
    ProcMesh *cor = s.unshackled ? s.coronaHot : s.corona;
    for (int i = 0; i < 2; i++)
        proc_mesh_set_transform(&cor[i], cs, (float[3]){ rock, yaw, i ? -spinAngle * 1.3f : spinAngle }, b->pos);

    // Glows blood red, pulsing, before it slams down (the dive) or bursts (the corona)
    float warn = s.state == SB_DIVE ? (s.step ? 1.0f : clamp01((s.t - 0.2f) / 0.6f))
               : s.state == SB_CORONA && s.step == 0 ? clamp01(s.t / 0.5f) : 0.0f;
    warn *= 0.7f + 0.3f * sinf(s.life * 18.0f);
    color_t tint = RGBA32(255, 255 - (int)(225 * warn), 255 - (int)(235 * warn), 255);

#ifdef HLE_LOG_CLOTH
    uint32_t t0 = TICKS_READ();
#endif
    if (s.chainsReady) chains_draw(b);
#ifdef HLE_LOG_CLOTH
    { static uint32_t sum; static int n; sum += TICKS_READ() - t0; if (++n == 120) { debugf("[chains] draw %lu us a frame\n", (unsigned long)(TICKS_TO_US(sum) / n)); sum = 0; n = 0; } }
#endif
    proc_mesh_draw_tinted(s.unshackled ? &s.eyeHot : &s.eye, false, tint);
    if (dying < 1.0f) for (int i = 0; i < 2; i++) proc_mesh_draw_tinted(&cor[i], true, tint);
}
