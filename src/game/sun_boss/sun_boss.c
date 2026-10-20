#include <libdragon.h>
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
#define KEEP_DIST      55.0f
#define MESH_R         16.0f           // the eye mesh is built at this radius (1/8-unit storage)

typedef enum { SB_INTRO, SB_HOVER, SB_GLARE, SB_CORONA, SB_DIVE, SB_REST, SB_BREAK, SB_DEAD } SbState;

#define RAIN_MAX 8

static struct {
    bool ready;
    ProcMesh eye, eyeHot;
    T3DModel *rays, *shackles;
    T3DMat4FP *raysMat, *shacklesMat, *shackles2Mat;
    SbState state;
    float t, life;          // time in state, total time
    int step;
    float groundY, orbit;
    bool unshackled;
    float cdGlare, cdRain, cdCorona, cdDive;
    float rainT[RAIN_MAX], rainPos[RAIN_MAX][2];
    float diveTarget[2];
    float raysSpin;
} s;

static ScrollParams s_raysScroll = { .xSpeed = 30.0f, .ySpeed = 0.0f, .scale = 64 };
static ScrollParams s_shackScroll = { .xSpeed = 50.0f, .ySpeed = 0.0f, .scale = 64 };
static ScrollParams s_shackScroll2 = { .xSpeed = -50.0f, .ySpeed = 0.0f, .scale = 64 };

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

static float speed_mult(void) { return s.unshackled ? 1.35f : 1.0f; }

// ---- Lifetime ----

void sun_boss_free(void)
{
    if (!s.ready) return;
    rspq_wait();
    proc_mesh_free(&s.eye);
    proc_mesh_free(&s.eyeHot);
    t3d_model_free(s.rays);
    t3d_model_free(s.shackles);
    free_uncached(s.raysMat);
    free_uncached(s.shacklesMat);
    free_uncached(s.shackles2Mat);
    memset(&s, 0, sizeof(s));
}

void sun_boss_begin(Boss* boss)
{
    if (!s.ready) {
        proc_mesh_begin(); proc_mesh_eye_sphere(MESH_R, true, false); proc_mesh_end(&s.eye);
        proc_mesh_begin(); proc_mesh_eye_sphere(MESH_R, true, true);  proc_mesh_end(&s.eyeHot);
        s.rays = t3d_model_load("rom:/shackled_sun/shackled_sun_rays.t3dm");
        s.shackles = t3d_model_load("rom:/shackled_sun/shackles.t3dm");
        s.raysMat = malloc_uncached(sizeof(T3DMat4FP));
        s.shacklesMat = malloc_uncached(sizeof(T3DMat4FP));
        s.shackles2Mat = malloc_uncached(sizeof(T3DMat4FP));
        s.ready = true;
    }
    boss->phaseIndex = SUN_BOSS_PHASE;
    boss->name = "The Shackled Sun";
    boss->health = boss->maxHealth;
    boss->mode = BOSS_MODE_CINEMATIC;       // not damageable until it has come down
    boss->state = BOSS_STATE_INTRO;
    boss->damageFlashTimer = 0.0f;
    boss->velX = boss->velZ = 0.0f;
    boss->handAttackColliderActive = false;
    boss->sphereAttackColliderActive = false;
    boss->visible = true;
    // The ball itself (centre at pos)
    boss->capsuleCollider.localCapA = (T3DVec3){{ 0.0f, -6.0f, 0.0f }};
    boss->capsuleCollider.localCapB = (T3DVec3){{ 0.0f, 6.0f, 0.0f }};
    boss->capsuleCollider.radius = SUN_R - 2.0f;

    s.groundY = character.pos[1];
    boss->pos[1] = s.groundY + DROP_H;
    s.unshackled = false;
    s.life = 0.0f;
    s.orbit = 0.0f;
    s.cdGlare = 2.0f; s.cdRain = 6.0f; s.cdCorona = 10.0f; s.cdDive = 14.0f;
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
    // The eye narrows on the player, then looses crossed waves in a fan
    face_player(b, dt, 3.0f);
    if (s.step == 0 && s.t >= 0.9f / speed_mult()) {
        int n = s.unshackled ? 5 : 3;
        float dx, dz, d = to_player(b, &dx, &dz);
        float base = atan2f(dx, dz);
        float dy = (character.pos[1] + 10.0f) - b->pos[1];
        for (int i = 0; i < n; i++) {
            float a = base + (i - (n - 1) * 0.5f) * 0.28f;
            const float SPEED = 85.0f;
            float h = sqrtf(d * d + dy * dy);
            float vy = h > 1e-3f ? dy / h * SPEED : 0.0f, vh = h > 1e-3f ? d / h * SPEED : SPEED;
            float o[3] = { b->pos[0] + sinf(a) * SUN_R, b->pos[1], b->pos[2] + cosf(a) * SUN_R };
            skill_fx_spawn_wave_hostile(SKILL_WAVE_X, o, (float[3]){ sinf(a) * vh, vy, cosf(a) * vh },
                                        10.0f, 1, 0.0f, 1.6f, false);
        }
        sfx_cast();
        s.step = 1;
    }
    if (s.t > 1.6f) enter(SB_HOVER);
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
            skill_fx_spawn_wave_hostile(SKILL_WAVE_BLADE, p, (float[3]){ 0, 0, 0 }, 14.0f, 1, 0.0f, 0.9f, false);
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
            skill_fx_spawn_arc_hostile(o, b->rot[1], 85.0f, 22.0f);
            skill_fx_spawn_arc_hostile(o, b->rot[1] + T3D_PI, 85.0f, 22.0f);
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
            if (to_player(b, &dx, &dz) < 26.0f) character_apply_damage(20.0f);
            skill_fx_spawn_burst_hostile((float[3]){ b->pos[0], s.groundY, b->pos[2] }, 0.0f);
            skill_fx_spawn_burst_hostile((float[3]){ b->pos[0], s.groundY, b->pos[2] }, 0.5f);
            screen_shake_set_shake_mag(14.0f);
            sfx_boom();
            enter(SB_REST);
        }
    }
}

static void choose(Boss *b, float dist)
{
    if (s.cdDive <= 0.0f && dist < 90.0f) { s.cdDive = 13.0f; enter(SB_DIVE); return; }
    if (s.cdCorona <= 0.0f && dist < 60.0f) { s.cdCorona = 11.0f; enter(SB_CORONA); return; }
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
    s.cdGlare -= dt * sm; s.cdRain -= dt * sm; s.cdCorona -= dt * sm; s.cdDive -= dt * sm;
    s.raysSpin += (1.0f - s.raysSpin) * fminf(1.0f, dt * 2.0f);
    rain_tick(dt);

    float dx, dz, dist = to_player(b, &dx, &dz);

    if (b->state == BOSS_STATE_DEAD && s.state != SB_DEAD) {
        enter(SB_DEAD);
        for (int i = 0; i < RAIN_MAX; i++) s.rainT[i] = -1.0f;
    }
    // Half health: the shackles break
    if (!s.unshackled && s.state != SB_DEAD && b->health <= b->maxHealth * 0.5f) {
        s.unshackled = true;
        b->mode = BOSS_MODE_CINEMATIC;
        enter(SB_BREAK);
    }

    switch (s.state) {
    case SB_INTRO:
        // Comes down from the ceiling in its chains
        b->pos[1] = s.groundY + HOVER_H + (DROP_H - HOVER_H) * (1.0f - clamp01(s.t / (INTRO_TIME * 0.8f)));
        face_player(b, dt, 4.0f);
        if (s.t >= INTRO_TIME) { b->mode = BOSS_MODE_COMBAT; b->state = BOSS_STATE_NEUTRAL; enter(SB_HOVER); }
        break;

    case SB_HOVER: {
        // Drift around the player at a distance, bobbing
        face_player(b, dt, 3.0f);
        s.orbit += dt * 0.35f * sm;
        float away = atan2f(-dx, -dz) + s.orbit * 0.3f;
        move_toward(b, character.pos[0] + sinf(away) * KEEP_DIST, character.pos[2] + cosf(away) * KEEP_DIST, DRIFT_SPEED * sm, dt);
        float hy = s.groundY + HOVER_H + sinf(s.life * 2.0f) * 3.0f;
        b->pos[1] += (hy - b->pos[1]) * fminf(1.0f, dt * 3.0f);
        if (s.t > 0.8f / sm) choose(b, dist);
        break;
    }

    case SB_GLARE:  glare_update(b, dt); break;
    case SB_CORONA: corona_update(b, dt); break;
    case SB_DIVE:   dive_update(b, dt);  break;

    case SB_REST:
        // Dimmed on the floor: the opening
        if (s.t > 2.6f / sm) enter(SB_HOVER);
        break;

    case SB_BREAK:
        // The chains snap: bursts all around, then it burns hotter
        b->pos[1] += (s.groundY + HOVER_H + 10.0f - b->pos[1]) * fminf(1.0f, dt * 3.0f);
        s.raysSpin = 8.0f;
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
}

void sun_boss_draw(Boss* b)
{
    if (!s.ready || !b->visible) return;
    float dying = (s.state == SB_DEAD) ? clamp01(s.t / 3.0f) : 0.0f;
    float pulse = 1.0f + 0.04f * sinf(s.life * 5.0f) + (b->damageFlashTimer > 0.0f ? 0.08f : 0.0f);
    float scale = (SUN_R / MESH_R) * pulse * (1.0f - 0.5f * dying);
    if (s.state == SB_REST) scale *= 0.9f;

    float yaw = -b->rot[1];     // Tiny3D's yaw turns +z the other way round
    proc_mesh_set_transform(s.unshackled ? &s.eyeHot : &s.eye, scale, (float[3]){ 0.0f, yaw, 0.0f }, b->pos);

    // Rays: a card facing +x, turned to face the same way as the eye and spinning
    static float spinAngle;
    spinAngle += deltaTime * 0.6f * s.raysSpin;
    float rs = MODEL_SCALE * 2.6f * (1.0f - dying);
    t3d_mat4fp_from_srt_euler(s.raysMat, (float[3]){ rs, rs, rs }, (float[3]){ spinAngle, yaw - T3D_PI * 0.5f, 0.0f }, b->pos);

    rdpq_sync_pipe();
    rdpq_mode_zbuf(false, false);
    if (dying < 1.0f) {
        t3d_matrix_set(s.raysMat, true);
        t3d_model_draw_custom(s.rays, (T3DModelDrawConf){ .userData = &s_raysScroll, .tileCb = tile_scroll });
    }
    if (!s.unshackled) {
        float ss = MODEL_SCALE * 1.8f;
        t3d_mat4fp_from_srt_euler(s.shacklesMat, (float[3]){ ss, ss, ss }, (float[3]){ 0.0f, yaw, 0.0f }, b->pos);
        t3d_mat4fp_from_srt_euler(s.shackles2Mat, (float[3]){ -ss, -ss, -ss }, (float[3]){ 0.0f, yaw, 0.0f }, b->pos);
        t3d_matrix_set(s.shacklesMat, true);
        t3d_model_draw_custom(s.shackles, (T3DModelDrawConf){ .userData = &s_shackScroll, .tileCb = tile_scroll });
        t3d_matrix_set(s.shackles2Mat, true);
        t3d_model_draw_custom(s.shackles, (T3DModelDrawConf){ .userData = &s_shackScroll2, .tileCb = tile_scroll });
    }
    proc_mesh_draw(s.unshackled ? &s.eyeHot : &s.eye);
}
