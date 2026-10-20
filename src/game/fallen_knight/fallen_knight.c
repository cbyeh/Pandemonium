#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>
#include <t3d/t3dmodel.h>
#include <t3d/t3dskeleton.h>
#include <t3d/t3danim.h>
#include <math.h>
#include <string.h>

#include "fallen_knight.h"
#include "character.h"
#include "globals.h"
#include "fx/skill_fx.h"
#include "fx/screen_shake.h"
#include "controllers/audio_controller.h"
#include "../../scenes/guardian/guardian_scene_sfx.h"
#include "../../scenes/guardian/guardian_scene_bounds.h"
#include "utilities/general_utility.h"

extern T3DModel* characterModel;       // the player's knight model (character.c)

// ---- Tuning ----
#define FK_SCALE          (MODEL_SCALE * 0.8f)     // 1.6x the player knight
#define FK_YAW_OFFSET     (T3D_PI * -0.5f)         // same model as the player
#define FK_RUN_SPEED      46.0f
#define FK_WALK_SPEED     18.0f
#define FK_TURN_RATE      4.0f
#define FK_MELEE_RANGE    24.0f
#define FK_MELEE_DAMAGE   12.0f
#define FK_INTRO_TIME     2.4f
#define FK_DROP_HEIGHT    90.0f

enum {
    FKA_IDLE, FKA_WALK, FKA_RUN, FKA_ROLL, FKA_ATTACK1, FKA_ATTACK2, FKA_ATTACK3,
    FKA_CHARGED, FKA_CLEAVE, FKA_HALFMOON, FKA_DEATH, FKA_COUNT
};
static const char *ANIM_NAMES[FKA_COUNT] = {
    "Idle", "Walk1", "Run", "Roll", "Attack1", "Attack2", "Attack3",
    "AttackCharged", "SkillCleave", "SkillHalfmoon", "Death",
};
static const bool ANIM_LOOP[FKA_COUNT] = { true, true, true };

typedef enum {
    FK_INTRO, FK_CHASE, FK_COMBO, FK_ROLL, FK_CLEAVE, FK_HALFMOON, FK_MOONBLADE, FK_RECOVER, FK_DEAD,
} FkState;

static struct {
    bool ready;
    T3DSkeleton skel, skelBlend;
    T3DAnim anims[FKA_COUNT];
    int anim;
    float blendT;           // crossfade timer (0 = done)
    T3DMat4FP *mat;

    FkState state;
    float t;                // time in state
    int step;               // events fired in the state / combo index
    bool hitDone;           // the current swing already connected
    float groundY, velY;
    float moveX, moveZ;     // roll / jump drift
    float cdCleave, cdHalfmoon, cdMoonblade, cdRoll;
} s;

// ---- Helpers ----

static void play(int a, bool restart)
{
    if (a == s.anim && !restart) return;
    t3d_skeleton_blend(&s.skelBlend, &s.skel, &s.skel, 0.0f);    // snapshot the current pose
    s.blendT = 0.15f;
    if (s.anim >= 0) t3d_anim_set_playing(&s.anims[s.anim], false);
    s.anim = a;
    t3d_anim_attach(&s.anims[a], &s.skel);
    t3d_anim_set_time(&s.anims[a], 0.0f);
    t3d_anim_set_speed(&s.anims[a], 1.0f);
    t3d_anim_set_playing(&s.anims[a], true);
}

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
    float target = atan2f(-dx, dz);
    float d = target - b->rot[1];
    while (d >  T3D_PI) d -= 2.0f * T3D_PI;
    while (d < -T3D_PI) d += 2.0f * T3D_PI;
    float m = rate * dt;
    b->rot[1] += (d > m) ? m : (d < -m) ? -m : d;
}

static void move(Boss *b, float vx, float vz, float dt)
{
    float nx = b->pos[0] + vx * dt, nz = b->pos[2] + vz * dt;
    if (scene_bounds_check_room(nx, b->pos[1], nz)) { b->pos[0] = nx; b->pos[2] = nz; }
}

static void enter(FkState st)
{
    s.state = st;
    s.t = 0.0f;
    s.step = 0;
    s.hitDone = false;
}

static void swing_sfx(void) { audio_play_scene_sfx_dist(SCENE1_SFX_CHAR_SWING1, 1.0f, 0.0f); }

/** Melee hit: once per swing, inside the clip's damage window, if the player is in front. */
static void melee_check(Boss *b, float t0, float t1, float damage)
{
    float clip = t3d_anim_get_time(&s.anims[s.anim]);
    if (s.hitDone || clip < t0 || clip > t1) return;
    float dx, dz, dist = to_player(b, &dx, &dz);
    float fx = -fm_sinf(b->rot[1]), fz = fm_cosf(b->rot[1]);
    if (dist > FK_MELEE_RANGE || (dist > 1e-3f && (dx * fx + dz * fz) / dist < 0.3f)) return;
    s.hitDone = true;
    character_apply_damage(damage);
}

static bool clip_done(void)
{
    return !s.anims[s.anim].isPlaying;
}

// ---- Lifetime ----

void fallen_knight_free(void)
{
    if (!s.ready) return;
    rspq_wait();
    for (int i = 0; i < FKA_COUNT; i++) t3d_anim_destroy(&s.anims[i]);
    t3d_skeleton_destroy(&s.skel);
    t3d_skeleton_destroy(&s.skelBlend);
    free_uncached(s.mat);
    memset(&s, 0, sizeof(s));
}

void fallen_knight_begin(Boss* boss)
{
    if (!s.ready) {
        s.skel = t3d_skeleton_create(characterModel);
        s.skelBlend = t3d_skeleton_clone(&s.skel, false);
        for (int i = 0; i < FKA_COUNT; i++) {
            s.anims[i] = t3d_anim_create(characterModel, ANIM_NAMES[i]);
            t3d_anim_set_looping(&s.anims[i], ANIM_LOOP[i]);
            t3d_anim_set_playing(&s.anims[i], false);
        }
        s.mat = malloc_uncached(sizeof(T3DMat4FP));
        s.ready = true;
    }
    s.anim = -1;
    play(FKA_IDLE, true);

    boss->phaseIndex = FALLEN_KNIGHT_PHASE;
    boss->name = "Fallen Knight";
    boss->health = boss->maxHealth;
    boss->mode = BOSS_MODE_CINEMATIC;       // not damageable until it lands
    boss->state = BOSS_STATE_INTRO;
    boss->damageFlashTimer = 0.0f;
    boss->velX = boss->velZ = 0.0f;
    boss->handAttackColliderActive = false;
    boss->sphereAttackColliderActive = false;
    boss->visible = true;
    // Knight-sized capsule (1.6x the player's)
    boss->capsuleCollider.localCapA = (T3DVec3){{ 0.0f, 6.0f, 0.0f }};
    boss->capsuleCollider.localCapB = (T3DVec3){{ 0.0f, 26.0f, 0.0f }};
    boss->capsuleCollider.radius = 11.0f;

    s.groundY = character.pos[1];
    boss->pos[1] = s.groundY + FK_DROP_HEIGHT;
    s.velY = 0.0f;
    s.cdCleave = 5.0f; s.cdHalfmoon = 12.0f; s.cdMoonblade = 9.0f; s.cdRoll = 0.0f;
    enter(FK_INTRO);
}

void fallen_knight_skip_intro(Boss* boss)
{
    if (!s.ready || s.state != FK_INTRO) return;
    boss->pos[1] = s.groundY;
    boss->mode = BOSS_MODE_COMBAT;
    boss->state = BOSS_STATE_NEUTRAL;
    enter(FK_CHASE);
}

// ---- Skills (hostile versions of the player's) ----

static void cleave_update(Boss *b)
{
    // crouch, rising swing, then five crimson blades rise one by one toward the player
    const float RELEASE = 0.85f, GAP = 0.12f;
    if (s.step < 5 && s.t >= RELEASE + s.step * GAP) {
        static const float spread[] = { 0.0f, -0.3f, 0.3f, -0.6f, 0.6f };
        float a = b->rot[1] + spread[s.step];
        float dist = 22.0f + s.step * 10.0f;
        float dx = -fm_sinf(a), dz = fm_cosf(a);
        float o[3] = { b->pos[0] + dx * dist, s.groundY, b->pos[2] + dz * dist };
        skill_fx_spawn_wave_hostile(SKILL_WAVE_BLADE, o, (float[3]){ dx * 6.0f, 0.0f, dz * 6.0f }, 5.0f, 2, 0.4f, 1.5f, false);
        if (s.step == 0) { swing_sfx(); screen_shake_set_shake_mag(5.0f); }
        s.step++;
    }
    if (s.t > 1.8f) enter(FK_RECOVER);
}

static void halfmoon_update(Boss *b, float dt)
{
    // a long telegraphed charge (crimson pulses), then a wide rolling slash: roll through it
    const float CHARGE = 1.6f;
    if (s.step == 0) {
        face_player(b, dt, 1.5f);
        static float pulse;
        pulse -= dt;
        if (pulse <= 0.0f) {
            skill_fx_spawn_burst_hostile((float[3]){ b->pos[0], s.groundY, b->pos[2] }, b->rot[1] + s.t * 3.0f);
            pulse = 0.35f;
        }
        if (s.t >= CHARGE) {
            skill_fx_spawn_arc_hostile((float[3]){ b->pos[0], b->pos[1] + 12.0f, b->pos[2] }, b->rot[1], 90.0f, 30.0f);
            screen_shake_set_shake_mag(12.0f);
            swing_sfx();
            s.step = 1;
        }
    }
    if (s.t > 2.9f) enter(FK_RECOVER);
}

static void moonblade_update(Boss *b, float dt)
{
    // leap high and rain five crimson crosses on the player
    if (s.step == 0) {
        s.velY = 120.0f;
        play(FKA_ROLL, true);
        s.step = 1;
    }
    s.velY -= 120.0f * ((s.step >= 1 && s.step <= 5 && s.velY < 15.0f) ? 0.15f : 1.0f) * dt;
    if (s.velY < -60.0f) s.velY = -60.0f;
    b->pos[1] += s.velY * dt;
    face_player(b, dt, 6.0f);
    if (s.step >= 1 && s.step <= 5 && s.t >= 0.55f + (s.step - 1) * 0.28f) {
        float o[3] = { b->pos[0], b->pos[1] + 20.0f, b->pos[2] };
        float side = (s.step - 3) * 8.0f;
        float fx = -fm_sinf(b->rot[1]), fz = fm_cosf(b->rot[1]);
        float tgt[3] = { character.pos[0] + fz * side, s.groundY + 4.0f, character.pos[2] - fx * side };
        float d[3] = { tgt[0] - o[0], tgt[1] - o[1], tgt[2] - o[2] };
        float len = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        if (len < 1.0f) len = 1.0f;
        const float SPEED = 90.0f;
        skill_fx_spawn_wave_hostile(SKILL_WAVE_X, o, (float[3]){ d[0] / len * SPEED, d[1] / len * SPEED, d[2] / len * SPEED },
                                    8.0f, 1, 0.0f, len / SPEED, s.step == 5);
        swing_sfx();
        // One full swing per cross, like the player's: cycle the three clips, sped up
        // to fit the gap between waves
        static const int swings[3] = { FKA_ATTACK1, FKA_ATTACK2, FKA_ATTACK3 };
        play(swings[(s.step - 1) % 3], true);
        t3d_anim_set_speed(&s.anims[s.anim], 1.6f);
        s.step++;
    }
    if (b->pos[1] <= s.groundY && s.velY < 0.0f) {
        b->pos[1] = s.groundY;
        skill_fx_spawn_burst_hostile((float[3]){ b->pos[0], s.groundY, b->pos[2] }, b->rot[1]);
        screen_shake_set_shake_mag(6.0f);
        enter(FK_RECOVER);
    }
}

// ---- Decisions ----

static void choose(Boss *b, float dist)
{
    // Skills off cooldown come first, by range; otherwise close in and combo
#ifdef HLE_TEST_FK_LEAP
    if (s.cdMoonblade <= 0.0f) { s.cdMoonblade = 4.0f; enter(FK_MOONBLADE); debugf("[fk] leap\n"); return; }   // test aid: leap often
#endif
    if (s.cdHalfmoon <= 0.0f && dist < 70.0f) { s.cdHalfmoon = 16.0f; enter(FK_HALFMOON); play(FKA_HALFMOON, true); return; }
    if (s.cdMoonblade <= 0.0f && dist > 40.0f) { s.cdMoonblade = 12.0f; enter(FK_MOONBLADE); return; }
    if (s.cdCleave <= 0.0f && dist < 60.0f) { s.cdCleave = 9.0f; enter(FK_CLEAVE); play(FKA_CLEAVE, true); return; }
    if (dist < FK_MELEE_RANGE - 4.0f) { enter(FK_COMBO); s.step = 1; play(FKA_ATTACK1, true); swing_sfx(); return; }
}

void fallen_knight_update(Boss* b, float dt)
{
    if (!s.ready) return;
    if (b->damageFlashTimer > 0.0f) b->damageFlashTimer -= dt;
    s.t += dt;
    s.cdCleave -= dt; s.cdHalfmoon -= dt; s.cdMoonblade -= dt; s.cdRoll -= dt;

    float dx, dz, dist = to_player(b, &dx, &dz);

    if (b->state == BOSS_STATE_DEAD && s.state != FK_DEAD) {
        enter(FK_DEAD);
        play(FKA_DEATH, true);
        b->pos[1] = s.groundY;
    }

    switch (s.state) {
    case FK_INTRO:
        // Falls from above in a crimson light, lands with a burst, then fights
        b->pos[1] = s.groundY + FK_DROP_HEIGHT * fmaxf(0.0f, 1.0f - s.t / (FK_INTRO_TIME * 0.6f));
        face_player(b, dt, 10.0f);
        if (s.step == 0 && b->pos[1] <= s.groundY) {
            float p[3] = { b->pos[0], s.groundY, b->pos[2] };
            skill_fx_spawn_wave_hostile(SKILL_WAVE_BLADE, p, (float[3]){ 0, 0, 0 }, 0.0f, 0, 1.0f, 1.0f, false);
            skill_fx_spawn_burst_hostile(p, 0.0f);
            screen_shake_set_shake_mag(10.0f);
            s.step = 1;
        }
        if (s.t >= FK_INTRO_TIME) {
            b->mode = BOSS_MODE_COMBAT;
            b->state = BOSS_STATE_NEUTRAL;
            enter(FK_CHASE);
        }
        break;

    case FK_CHASE: {
        face_player(b, dt, FK_TURN_RATE);
        float speed = (dist > 45.0f) ? FK_RUN_SPEED : (dist > FK_MELEE_RANGE - 6.0f) ? FK_WALK_SPEED : 0.0f;
        play(speed >= FK_RUN_SPEED ? FKA_RUN : speed > 0.0f ? FKA_WALK : FKA_IDLE, false);
        if (speed > 0.0f && dist > 1e-3f) move(b, dx / dist * speed, dz / dist * speed, dt);
        // Dodge the player's swings sometimes
        CharacterState ps = character_get_state();
        if ((ps == CHAR_STATE_ATTACKING || ps == CHAR_STATE_ATTACKING_STRONG) && dist < 28.0f && s.cdRoll <= 0.0f) {
            s.cdRoll = 2.5f;
            if ((rand_custom_u32() % 100) < 40) {
                float side = (rand_custom_u32() & 1) ? 1.0f : -1.0f;
                s.moveX = (-dx / dist * 0.6f + dz / dist * side) * 70.0f;
                s.moveZ = (-dz / dist * 0.6f - dx / dist * side) * 70.0f;
                enter(FK_ROLL);
                play(FKA_ROLL, true);
                break;
            }
        }
        if (s.t > 0.4f) choose(b, dist);
        break;
    }

    case FK_COMBO:
        face_player(b, dt, 2.5f);
        melee_check(b, 0.25f, 0.6f, FK_MELEE_DAMAGE);
        if (clip_done()) {
            if (s.step < 3 && dist < FK_MELEE_RANGE + 6.0f) {
                s.step++;
                s.hitDone = false;
                play(s.step == 2 ? FKA_ATTACK2 : FKA_ATTACK3, true);
                swing_sfx();
            } else {
                enter(FK_RECOVER);
            }
        }
        break;

    case FK_ROLL:
        move(b, s.moveX, s.moveZ, dt);
        if (s.t > 0.55f) enter(FK_CHASE);
        break;

    case FK_CLEAVE:    cleave_update(b);       break;
    case FK_HALFMOON:  halfmoon_update(b, dt); break;
    case FK_MOONBLADE: moonblade_update(b, dt); break;

    case FK_RECOVER:
        play(FKA_IDLE, false);
        if (s.t > 0.7f) enter(FK_CHASE);
        break;

    case FK_DEAD:
        break;
    }

    // Animate (with a short crossfade after clip changes)
    t3d_anim_update(&s.anims[s.anim], dt);
    if (s.blendT > 0.0f) {
        s.blendT -= dt;
        t3d_skeleton_blend(&s.skel, &s.skelBlend, &s.skel, 1.0f - fmaxf(0.0f, s.blendT) / 0.15f);
    }
    t3d_skeleton_update(&s.skel);

    const float sc[3] = { FK_SCALE, FK_SCALE, FK_SCALE };
    const float rot[3] = { 0.0f, b->rot[1] + FK_YAW_OFFSET, 0.0f };
    t3d_mat4fp_from_srt_euler(s.mat, sc, rot, b->pos);
}

void fallen_knight_draw(Boss* b)
{
    if (!s.ready || !b->visible) return;
    // Dark crimson armour; brighter while it flashes from a hit
    bool flash = b->damageFlashTimer > 0.0f;
    t3d_light_set_ambient(flash ? (uint8_t[4]){ 255, 120, 120, 255 } : (uint8_t[4]){ 120, 45, 70, 255 });
    t3d_matrix_set(s.mat, true);
    t3d_model_draw_skinned(characterModel, &s.skel);
    t3d_light_set_ambient((uint8_t[4]){ 255, 255, 255, 255 });
}
