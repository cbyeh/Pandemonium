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
#include "utilities/game_math.h"
#include "utilities/simple_collision_utility.h"

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
#define FK_WIND           0.65f     // a basic swing's windup: as the Guardian's (0.8 s), a little quicker

enum {
    FKA_IDLE, FKA_WALK, FKA_RUN, FKA_ROLL, FKA_ATTACK1, FKA_ATTACK2, FKA_ATTACK3,
    FKA_CHARGED, FKA_CLEAVE, FKA_HALFMOON, FKA_DEATH, FKA_BACKFLIP, FKA_KNOCKDOWN, FKA_COUNT
};
static const char *ANIM_NAMES[FKA_COUNT] = {
    "Idle", "Walk1", "Run", "Roll", "Attack1", "Attack2", "Attack3",
    "AttackCharged", "SkillCleave", "SkillHalfmoon", "Death", "SkillBackflip", "Knockdown",
};
static const bool ANIM_LOOP[FKA_COUNT] = { true, true, true };

typedef enum {
    FK_INTRO, FK_CHASE, FK_COMBO, FK_ROLL, FK_CLEAVE, FK_HALFMOON, FK_MOONBLADE, FK_RECOVER, FK_STAGGER, FK_DEAD,
    FK_SPLITTER, FK_UPDOWN, FK_TRISLAM, FK_HOLYSLAM,
} FkState;

// The strike of each swing clip (clip seconds), measured from the blade's speed: the
// windups before it can't hurt. Attack1-3 (Attack2 rises from low in front: the up-down's
// first), then the charged clip's overhead slam (played from DN_FROM: the slam, no more)
static const float WIN[3][2] = { { 0.55f, 0.85f }, { 0.42f, 0.75f }, { 0.9f, 1.2f } };
static const float W_DN[2] = { 1.88f, 2.08f };
#define DN_FROM 1.72f     // the sword already raised: held there, glowing, then down

static struct {
    bool ready;
    T3DSkeleton skel, skelBlend;
    T3DAnim anims[FKA_COUNT];
    int anim;
    float blendT, blendDur; // crossfade timer (0 = done), and its length
    T3DMat4FP *mat;
    int swordBone;          // "Hand-Right": the blade runs along its -X

    FkState state;
    float t;                // time in state
    int step;               // events fired in the state / combo index
    bool hitDone;           // the current swing already connected
    float groundY, velY;
    float moveX, moveZ;     // roll / jump drift
    float cdCleave, cdHalfmoon, cdMoonblade, cdRoll, cdSplitter, cdUpdown, cdTri, cdHoly;
    float tri[3][2];        // the triangle slam's corners on the floor
    float windK;            // 0..1 through the current swing's windup (its glow), 0 once it strikes
    bool enraged;           // the war cry at half health
} s;

// ---- Helpers ----

static void play(int a, bool restart)
{
    if (a == s.anim && !restart) return;
    t3d_skeleton_blend(&s.skelBlend, &s.skel, &s.skel, 0.0f);    // snapshot the current pose
    // swings cut in fast (the blade's hits are read from the pose); going down, dying and
    // moving about ease over longer
    s.blendDur = (a == FKA_KNOCKDOWN || a == FKA_DEATH) ? 0.35f : (a == FKA_IDLE || a == FKA_WALK || a == FKA_RUN) ? 0.3f : 0.15f;
    s.blendT = s.blendDur;
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

static inline float clamp01f(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

static void enter(FkState st)
{
    s.state = st;
    s.t = 0.0f;
    s.step = 0;
    s.hitDone = false;
    s.windK = 0.0f;
}

static void swing_sfx(void) { audio_play_scene_sfx_dist(SCENE1_SFX_CHAR_SWING1, 1.0f, 0.0f); }

/** The blade in world space, from the last pose (as the player's sword: 640 units along the hand's -X) */
static bool sword_segment(float base[3], float tip[3])
{
    if (s.swordBone < 0) return false;
    const T3DMat4FP *B = &s.skel.boneMatricesFP[s.swordBone];
    float m0[3], m1[3];
    mat4fp_mul_point_f32_row3_colbasis(B, (float[3]){ 0.0f, 0.0f, 0.0f }, m0);
    mat4fp_mul_point_f32_row3_colbasis(B, (float[3]){ -640.0f, 0.0f, 0.0f }, m1);
    mat4fp_mul_point_f32_row3_colbasis(s.mat, m0, base);
    mat4fp_mul_point_f32_row3_colbasis(s.mat, m1, tip);
    return true;
}

/** Melee hit: once per swing, inside the clip's damage window, only where the blade meets the player */
static void melee_check(Boss *b, float t0, float t1, float damage)
{
    (void)b;
    float clip = t3d_anim_get_time(&s.anims[s.anim]);
    if (s.hitDone || clip < t0 || clip > t1) return;
    float base[3], tip[3];
    if (!sword_segment(base, tip)) return;
    const float *p = character.pos;
    const T3DVec3 *a = &character.capsuleCollider.localCapA, *c = &character.capsuleCollider.localCapB;
    float pa[3] = { p[0] + a->v[0], p[1] + a->v[1], p[2] + a->v[2] };
    float pb[3] = { p[0] + c->v[0], p[1] + c->v[1], p[2] + c->v[2] };
    if (!scu_capsule_vs_capsule_f(base, tip, 3.0f, pa, pb, character.capsuleCollider.radius)) return;
    s.hitDone = true;
    character_apply_damage(damage);
#ifdef HLE_TEST_DUMMY
    debugf("[fk] hit by clip %d at %.2f s\n", s.anim, (double)clip);
#endif
}

static bool clip_done(void)
{
    return !s.anims[s.anim].isPlaying;
}

static float clip_time(void) { return t3d_anim_get_time(&s.anims[s.anim]); }

/** Slows the clip (begun at `from`) before its strike so the windup takes `wind` seconds, then plays it at speed */
static void swing_pace(float from, float strike, float wind)
{
    float pre = strike - 0.08f, clip = clip_time();     // the blade starts moving a little before the strike
    t3d_anim_set_speed(&s.anims[s.anim], clip < pre ? fminf(1.0f, (pre - from) / wind) : 1.0f);
    s.windK = clip < pre ? clamp01f((clip - from) / (pre - from)) : 0.0f;
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
        s.swordBone = t3d_skeleton_find_bone(&s.skel, "Hand-Right");
        s.ready = true;
    }
    s.anim = -1;
    play(FKA_IDLE, true);

    boss->phaseIndex = FALLEN_KNIGHT_PHASE;
    boss->name = "Fallen Warrior";
    boss->health = boss->maxHealth;
    boss->mode = BOSS_MODE_CINEMATIC;       // not damageable until it lands
    boss->state = BOSS_STATE_INTRO;
    boss->poise = boss->poiseIdleT = boss->staggerT = boss->recoilT = 0.0f; boss->halfDone = false;
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
    s.cdCleave = 5.0f; s.cdHalfmoon = 12.0f; s.cdMoonblade = 9.0f; s.cdRoll = 0.0f; s.cdSplitter = 6.0f; s.cdUpdown = 3.0f; s.cdTri = 14.0f; s.cdHoly = 20.0f;
    s.enraged = false;
    enter(FK_INTRO);
}

void fallen_knight_skip_intro(Boss* boss)
{
    if (!s.ready || s.state != FK_INTRO) return;
    if (s.step == 0) {      // skipped before it landed
        float p[3] = { boss->pos[0], s.groundY, boss->pos[2] };
        skill_fx_spawn_burst_hostile(p, 0.0f);
    }
    boss->pos[1] = s.groundY;
    boss->mode = BOSS_MODE_COMBAT;
    boss->state = BOSS_STATE_NEUTRAL;
    enter(FK_CHASE);
}

bool fallen_knight_stagger(Boss* b)
{
    // Knocked down where it stands; not in the air (the leap) or already down
    if (!s.ready || s.state == FK_INTRO || s.state == FK_MOONBLADE || s.state == FK_STAGGER || s.state == FK_DEAD) return false;
    b->pos[1] = s.groundY;
    enter(FK_STAGGER);
    play(FKA_KNOCKDOWN, true);
    b->staggerT = 2.2f;
    return true;
}

void fallen_knight_half_stagger(Boss* b)
{
    (void)b;
    // Flinches: a windup (a swing's, a skill's charge or crouch) is broken off, and it recovers
    if (!s.ready || s.state == FK_INTRO || s.state == FK_MOONBLADE || s.state == FK_STAGGER || s.state == FK_DEAD) return;
    bool winding = s.windK > 0.0f || (s.state == FK_HALFMOON && s.step == 0) || (s.state == FK_CLEAVE && s.t < 0.8f);
    if (winding) { enter(FK_RECOVER); s.t = 0.4f; }    // a 1 s opening
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
        play(FKA_BACKFLIP, true);
        s.step = 1;
    }
    s.velY -= 120.0f * ((s.step >= 1 && s.step <= 5 && s.velY < 15.0f) ? 0.15f : 1.0f) * dt;
    if (s.velY < -60.0f) s.velY = -60.0f;
    b->pos[1] += s.velY * dt;
    face_player(b, dt, 6.0f);
    // the crosses come slower and further apart than the player's own: readable, dodgeable
    if (s.step >= 1 && s.step <= 5 && s.t >= 0.55f + (s.step - 1) * 0.4f) {
        float o[3] = { b->pos[0], b->pos[1] + 20.0f, b->pos[2] };
        float side = (s.step - 3) * 8.0f;
        float fx = -fm_sinf(b->rot[1]), fz = fm_cosf(b->rot[1]);
        float tgt[3] = { character.pos[0] + fz * side, s.groundY + 4.0f, character.pos[2] - fx * side };
        float d[3] = { tgt[0] - o[0], tgt[1] - o[1], tgt[2] - o[2] };
        float len = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        if (len < 1.0f) len = 1.0f;
        const float SPEED = 58.0f;
        skill_fx_spawn_wave_hostile(SKILL_WAVE_X, o, (float[3]){ d[0] / len * SPEED, d[1] / len * SPEED, d[2] / len * SPEED },
                                    8.0f, 1, 0.0f, len / SPEED, s.step == 5);
        swing_sfx();
        // One full swing per cross, like the player's: cycle the three clips, sped up
        // to fit the gap between waves
        static const int swings[3] = { FKA_ATTACK1, FKA_ATTACK2, FKA_ATTACK3 };
        play(swings[(s.step - 1) % 3], true);
        t3d_anim_set_speed(&s.anims[s.anim], 1.2f);
        s.step++;
    }
    if (b->pos[1] <= s.groundY && s.velY < 0.0f) {
        b->pos[1] = s.groundY;
        skill_fx_spawn_burst_hostile((float[3]){ b->pos[0], s.groundY, b->pos[2] }, b->rot[1]);
        screen_shake_set_shake_mag(6.0f);
        enter(FK_RECOVER);
    }
}

static void splitter_update(Boss *b, float dt)
{
    // Moonlight Splitter: plants its feet in a crimson glow, then three wound-up swings, each
    // loosing a crescent at the player (level, the other diagonal, upright): step aside
    static const int clips[3] = { FKA_ATTACK1, FKA_ATTACK2, FKA_ATTACK3 };
    static const float roll[3] = { 0.25f, -0.6f, 1.45f };
    if (s.step == 0) {
        face_player(b, dt, 4.0f);
        play(FKA_IDLE, false);
        s.windK = clamp01f(s.t / 0.5f);
        if (s.t >= 0.6f) { s.step = 1; play(clips[0], true); swing_sfx(); }
        return;
    }
    const float *w = WIN[s.step - 1];
    float clip = clip_time();
    if (clip < w[0]) face_player(b, dt, 3.0f);
    swing_pace(0.0f, w[0], 0.55f);
    if (!s.hitDone && clip >= w[0]) {
        s.hitDone = true;
        float fx = -fm_sinf(b->rot[1]), fz = fm_cosf(b->rot[1]);
        float o[3] = { b->pos[0] + fx * 14.0f, s.groundY + 14.0f, b->pos[2] + fz * 14.0f };
        float dx = character.pos[0] - o[0], dz = character.pos[2] - o[2], d = sqrtf(dx * dx + dz * dz);
        if (d < 1.0f) { dx = fx; dz = fz; d = 1.0f; }
        const float SPEED = 80.0f;
        skill_fx_spawn_crescent_hostile(o, (float[3]){ dx / d * SPEED, 0.0f, dz / d * SPEED }, roll[s.step - 1], 12.0f, 2.0f);
        screen_shake_set_shake_mag(3.0f);
    }
    if (s.hitDone && (clip >= w[1] || clip_done())) {
        if (s.step < 3) { s.step++; s.hitDone = false; play(clips[s.step - 1], true); swing_sfx(); }
        else enter(FK_RECOVER);
    }
}

static void updown_update(Boss *b, float dt)
{
    // Two wound-up slashes: rising from low in front, then held overhead (glowing) and down
    // with a burst where it lands: back off or roll each
    float clip = clip_time();
    const float *w = s.step == 0 ? WIN[1] : W_DN;
    if (clip < w[0]) face_player(b, dt, 3.0f);
    swing_pace(s.step == 0 ? 0.0f : DN_FROM, w[0], 0.7f);
    melee_check(b, w[0], w[1], s.step == 0 ? 14.0f : 18.0f);
    if (s.step == 0) {
        if (clip >= w[1] || clip_done()) { s.step = 1; s.hitDone = false; play(FKA_CHARGED, true);
                                             t3d_anim_set_time(&s.anims[s.anim], DN_FROM); t3d_anim_set_speed(&s.anims[s.anim], 0.1f); swing_sfx(); }
    } else {
        if (s.step == 1 && clip >= 2.02f) {    // the blade meets the floor
            float fx = -fm_sinf(b->rot[1]), fz = fm_cosf(b->rot[1]);
            skill_fx_spawn_burst_hostile((float[3]){ b->pos[0] + fx * 22.0f, s.groundY, b->pos[2] + fz * 22.0f }, b->rot[1]);
            screen_shake_set_shake_mag(6.0f);
            s.step = 2;
        }
        if (clip >= 2.35f || clip_done()) enter(FK_RECOVER);
    }
}

static void tri_point(int i, float k, float out[3])     // along the edge from corner i to the next
{
    const float *a = s.tri[i], *c = s.tri[(i + 1) % 3];
    out[0] = a[0] + (c[0] - a[0]) * k; out[1] = s.groundY; out[2] = a[1] + (c[1] - a[1]) * k;
}

static void trislam_update(Boss *b, float dt)
{
    // Triangle slam: the sword raised, glowing, then driven into the floor; a triangle of
    // warnings closes round the player and blades erupt along it, striking all inside: get out
    const float ERUPT = 0.7f;
    float clip = clip_time();
    if (s.step == 0) {
        face_player(b, dt, 3.0f);
        swing_pace(DN_FROM, W_DN[0], 0.75f);
        if (clip >= 2.02f) {    // the blade meets the floor: mark the triangle where the player stands
            float fx = -fm_sinf(b->rot[1]), fz = fm_cosf(b->rot[1]);
            skill_fx_spawn_burst_hostile((float[3]){ b->pos[0] + fx * 22.0f, s.groundY, b->pos[2] + fz * 22.0f }, b->rot[1]);
            screen_shake_set_shake_mag(8.0f);
            float a0 = (rand_custom_u32() % 628) / 100.0f;
            for (int i = 0; i < 3; i++) {
                float a = a0 + i * 2.0944f;
                s.tri[i][0] = character.pos[0] + sinf(a) * 40.0f; s.tri[i][1] = character.pos[2] + cosf(a) * 40.0f;
            }
            for (int i = 0; i < 3; i++)
                for (int k = 0; k < 2; k++) { float p[3]; tri_point(i, k * 0.5f, p); skill_fx_spawn_burst_hostile(p, (float)(i * 2 + k)); }
            s.step = 1; s.t = 0.0f;
        }
    } else if (s.step == 1) {
        if (s.t >= ERUPT) {
            for (int i = 0; i < 3; i++)
                for (int k = 0; k < 2; k++) {
                    float p[3]; tri_point(i, k * 0.5f, p);
                    skill_fx_spawn_wave_hostile(SKILL_WAVE_BLADE, p, (float[3]){ 0, 0, 0 }, 0.0f, 0, 1.0f, 0.9f, false);
                    if (k == 0) skill_fx_spawn_pillar_hostile(p);       // columns of light at the corners
                }
            // inside the triangle (or just on its edge), on the ground
            float px = character.pos[0], pz = character.pos[2];
            bool in = true;
            for (int i = 0; i < 3 && in; i++) {
                const float *a = s.tri[i], *c = s.tri[(i + 1) % 3];
                float ex = c[0] - a[0], ez = c[1] - a[1], l = sqrtf(ex * ex + ez * ez);
                in = (ex * (pz - a[1]) - ez * (px - a[0])) / l <= 6.0f;     // the corners go clockwise: inside is negative
            }
            if (in && character.pos[1] - s.groundY < 6.0f) character_apply_damage(20.0f);
            screen_shake_set_shake_mag(10.0f);
            audio_play_scene_sfx_dist(SCENE1_SFX_ROCK_BLAST, 1.0f, 0.0f);
            s.step = 2; s.t = 0.0f;
        }
    } else if (s.t >= 0.5f) enter(FK_RECOVER);
}

static void holyslam_update(Boss *b, float dt)
{
    // A swing, then the sword held high, glowing ever brighter as the light gathers round
    // it, then slammed down: a blast all round it. Get far away, roll it or jump it
    float clip = clip_time();
    if (s.step == 0) {          // the opening swing (wound up like the combo's)
        if (clip < WIN[0][0]) face_player(b, dt, 2.5f);
        swing_pace(0.0f, WIN[0][0], FK_WIND);
        melee_check(b, WIN[0][0], WIN[0][1], FK_MELEE_DAMAGE);
        if (clip >= WIN[0][1] || clip_done()) {
            s.step = 1; s.t = 0.0f; s.hitDone = false;
            play(FKA_CHARGED, true); t3d_anim_set_time(&s.anims[s.anim], DN_FROM); t3d_anim_set_speed(&s.anims[s.anim], 0.05f);
        }
    } else if (s.step == 1) {   // the gathering
        face_player(b, dt, 1.5f);
        swing_pace(DN_FROM, W_DN[0], 1.5f);
        if ((int)(s.t * 5.0f) != (int)((s.t - dt) * 5.0f)) {    // the light drawn in toward it
            float a = s.t * 5.0f, r = 45.0f - 25.0f * clamp01f(s.t / 1.5f);
            skill_fx_spawn_burst_hostile((float[3]){ b->pos[0] + sinf(a) * r, s.groundY, b->pos[2] + cosf(a) * r }, a);
        }
        if (clip >= 2.02f) {
            float o[3] = { b->pos[0], s.groundY + 4.0f, b->pos[2] };
            skill_fx_spawn_arc_hostile(o, b->rot[1], 105.0f, 26.0f);
            skill_fx_spawn_arc_hostile(o, b->rot[1] + T3D_PI, 105.0f, 26.0f);
            skill_fx_spawn_pillar_hostile((float[3]){ b->pos[0], s.groundY, b->pos[2] });
            screen_shake_set_shake_mag(16.0f);
            audio_play_scene_sfx_dist(SCENE1_SFX_ROCK_BLAST, 1.0f, 0.0f);
            s.step = 2; s.t = 0.0f;
        }
    } else if (s.t >= 0.8f) enter(FK_RECOVER);     // spent a moment: the opening
}

// ---- Decisions ----

static void choose(Boss *b, float dist)
{
    // Skills off cooldown come first, by range; otherwise close in and combo
#ifdef HLE_TEST_FK_LEAP
    if (s.cdMoonblade <= 0.0f) { s.cdMoonblade = 4.0f; enter(FK_MOONBLADE); debugf("[fk] leap\n"); return; }   // test aid: leap often
#endif
#ifdef HLE_FK_FORCE   // test aid: 1 = halfmoon, 2 = cleave, back to back
    if (HLE_FK_FORCE == 1) { enter(FK_HALFMOON); play(FKA_HALFMOON, true); debugf("[fk] halfmoon\n"); return; }
    if (HLE_FK_FORCE == 2) { enter(FK_CLEAVE); play(FKA_CLEAVE, true); debugf("[fk] cleave\n"); return; }
    if (HLE_FK_FORCE == 3) { enter(FK_SPLITTER); debugf("[fk] splitter\n"); return; }      // 3 = splitter, 4 = up-down, 5 = combo
    if (HLE_FK_FORCE == 7) { enter(FK_HOLYSLAM); play(FKA_ATTACK1, true); swing_sfx(); debugf("[fk] holyslam\n"); return; }
    if (HLE_FK_FORCE == 6) { enter(FK_TRISLAM); play(FKA_CHARGED, true); t3d_anim_set_time(&s.anims[s.anim], DN_FROM); debugf("[fk] trislam\n"); return; }
    if (HLE_FK_FORCE >= 4 && dist > FK_MELEE_RANGE - 4.0f) return;     // the melee ones: in reach first
    if (HLE_FK_FORCE == 4) { enter(FK_UPDOWN); play(FKA_ATTACK2, true); swing_sfx(); debugf("[fk] updown\n"); return; }
    if (HLE_FK_FORCE == 5) { enter(FK_COMBO); s.step = 1; play(FKA_ATTACK1, true); swing_sfx(); debugf("[fk] combo\n"); return; }
#endif
    if (s.cdHalfmoon <= 0.0f && dist < 70.0f) { s.cdHalfmoon = 16.0f; enter(FK_HALFMOON); play(FKA_HALFMOON, true); return; }
    if (s.cdMoonblade <= 0.0f && dist > 40.0f) { s.cdMoonblade = 12.0f; enter(FK_MOONBLADE); return; }
    if (s.enraged && s.cdHoly <= 0.0f && dist < 50.0f) { s.cdHoly = 20.0f; enter(FK_HOLYSLAM); play(FKA_ATTACK1, true); swing_sfx(); return; }
    if (s.cdTri <= 0.0f && dist > 40.0f && dist < 140.0f) {
        s.cdTri = 14.0f; enter(FK_TRISLAM); play(FKA_CHARGED, true); t3d_anim_set_time(&s.anims[s.anim], DN_FROM); swing_sfx(); return;
    }
    if (s.cdSplitter <= 0.0f && dist > 35.0f && dist < 130.0f) { s.cdSplitter = 11.0f; enter(FK_SPLITTER); return; }
    if (s.cdCleave <= 0.0f && dist < 60.0f) { s.cdCleave = 9.0f; enter(FK_CLEAVE); play(FKA_CLEAVE, true); return; }
    if (s.cdUpdown <= 0.0f && dist < FK_MELEE_RANGE + 8.0f) { s.cdUpdown = 7.0f; enter(FK_UPDOWN); play(FKA_ATTACK2, true); swing_sfx(); return; }
    if (dist < FK_MELEE_RANGE - 4.0f) { enter(FK_COMBO); s.step = 1; play(FKA_ATTACK1, true); swing_sfx(); return; }
}

void fallen_knight_update(Boss* b, float dt)
{
    if (!s.ready) return;
    if (b->damageFlashTimer > 0.0f) b->damageFlashTimer -= dt;
    s.t += dt;
    s.cdCleave -= dt; s.cdHalfmoon -= dt; s.cdMoonblade -= dt; s.cdRoll -= dt; s.cdSplitter -= dt; s.cdUpdown -= dt; s.cdTri -= dt; s.cdHoly -= dt;

    float dx, dz, dist = to_player(b, &dx, &dz);

    if (b->state == BOSS_STATE_DEAD && s.state != FK_DEAD) {
        enter(FK_DEAD);
        play(FKA_DEATH, true);
        b->pos[1] = s.groundY;
    }


    // Half health: a war cry and a crimson burst (the fight's turn, like the others' breaks)
    if (!s.enraged && b->mode == BOSS_MODE_COMBAT && b->health > 0.0f && b->health <= b->maxHealth * 0.5f) {
        s.enraged = true;
        audio_play_scene_sfx_dist(SCENE1_SFX_WAR_CRY, 1.0f, 0.0f);
        skill_fx_spawn_burst_hostile((float[3]){ b->pos[0], s.groundY, b->pos[2] }, b->rot[1]);
        screen_shake_set_shake_mag(6.0f);
    }

#ifdef HLE_TEST_DUMMY
    { extern void hle_open_tick(bool, float); hle_open_tick((s.state == FK_CHASE || s.state == FK_RECOVER), dt); }   // test aid: measure the openings
#endif
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
        break;      // waits, landed, until the intro cutscene ends (fallen_knight_skip_intro)

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
        if (s.t > 0.7f) choose(b, dist);
        break;
    }

    case FK_COMBO:
        {   // each swing wound up, tracking the player until it commits to the strike
            const float *w = WIN[s.anim == FKA_ATTACK2 ? 1 : s.anim == FKA_ATTACK3 ? 2 : 0];
            if (clip_time() < w[0]) face_player(b, dt, 2.5f);
            swing_pace(0.0f, w[0], FK_WIND);
            melee_check(b, w[0], w[1], FK_MELEE_DAMAGE);
        }
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
    case FK_SPLITTER:  splitter_update(b, dt); break;
    case FK_UPDOWN:    updown_update(b, dt);   break;
    case FK_TRISLAM:   trislam_update(b, dt);  break;
    case FK_HOLYSLAM:  holyslam_update(b, dt); break;

    case FK_STAGGER:
        // Down from the stagger: it gets up when the clip ends and the stagger is over
        if (b->staggerT <= 0.0f && clip_done()) enter(FK_RECOVER);
        break;

    case FK_RECOVER:
        // A breather after every move: the opening to hit back
        play(FKA_IDLE, false);
        if (s.t > 1.4f) enter(FK_CHASE);
        break;

    case FK_DEAD:
        break;
    }

    // Animate (with a short crossfade after clip changes)
    t3d_anim_update(&s.anims[s.anim], dt);
    if (s.blendT > 0.0f) {
        s.blendT -= dt;
        float k = 1.0f - fmaxf(0.0f, s.blendT) / s.blendDur;
        t3d_skeleton_blend(&s.skel, &s.skelBlend, &s.skel, k * k * (3.0f - 2.0f * k));     // eased in and out
    }
    t3d_skeleton_update(&s.skel);

    // A half stagger: jolted back and down a little, squashed
    float k = boss_recoil(b), p[3] = { b->pos[0] + dx / (dist + 1e-3f) * -6.0f * k, b->pos[1] - 2.0f * k, b->pos[2] + dz / (dist + 1e-3f) * -6.0f * k };
    const float sc[3] = { FK_SCALE * (1.0f + 0.05f * k), FK_SCALE * (1.0f - 0.07f * k), FK_SCALE * (1.0f + 0.05f * k) };
    const float rot[3] = { 0.0f, b->rot[1] + FK_YAW_OFFSET, 0.0f };
    t3d_mat4fp_from_srt_euler(s.mat, sc, rot, p);
#ifdef HLE_FK_TRACE     // test aid: the blade tip through each swing, to find its strike
    {
        static float last[3]; float base[3], tip[3];
        if (s.state != FK_CHASE && s.state != FK_RECOVER && sword_segment(base, tip) && dt > 0.0f) {
            float v = sqrtf((tip[0]-last[0])*(tip[0]-last[0]) + (tip[1]-last[1])*(tip[1]-last[1]) + (tip[2]-last[2])*(tip[2]-last[2])) / dt;
            float fx = -fm_sinf(b->rot[1]), fz = fm_cosf(b->rot[1]), tx = tip[0] - b->pos[0], tz = tip[2] - b->pos[2];
            float px = character.pos[0] - b->pos[0], pz = character.pos[2] - b->pos[2];
            debugf("[fkt] st %d step %d clip %d t %.2f tipY %.0f v %.0f wind %.2f | tip fwd %.0f side %.0f, baseY %.0f | player fwd %.0f side %.0f\n", s.state, s.step, s.anim, (double)clip_time(), (double)(tip[1] - s.groundY), (double)v, (double)s.windK,
                   (double)(tx * fx + tz * fz), (double)(tx * fz - tz * fx), (double)(base[1] - s.groundY), (double)(px * fx + pz * fz), (double)(px * fz - pz * fx));
        }
        if (sword_segment(base, tip)) memcpy(last, tip, sizeof(last));
    }
#endif
}

void fallen_knight_draw(Boss* b)
{
    if (!s.ready || !b->visible) return;
    // Dark crimson armour; brighter while it flashes from a hit, and glowing red, pulsing,
    // while it winds up a skill (the Halfmoon's charge, the Cleave's crouch), softer before a swing
    bool flash = b->damageFlashTimer > 0.0f;
    float warn = s.state == FK_HALFMOON && s.step == 0 ? clamp01f(s.t / 0.6f)
               : s.state == FK_CLEAVE && s.step == 0 ? clamp01f(s.t / 0.4f)
               : s.state == FK_COMBO ? 0.5f * s.windK
               : s.state == FK_SPLITTER || s.state == FK_UPDOWN || s.state == FK_TRISLAM ? 0.8f * s.windK
               : s.state == FK_HOLYSLAM ? (s.step == 1 ? s.windK : 0.5f * s.windK) : 0.0f;
    warn *= 0.7f + 0.3f * sinf(s.t * 18.0f);
    uint8_t amb[4] = { (uint8_t)(120 + 135 * warn), (uint8_t)(45 - 30 * warn), (uint8_t)(70 - 55 * warn), 255 };
    t3d_light_set_ambient(flash ? (uint8_t[4]){ 255, 120, 120, 255 } : amb);
    t3d_matrix_set(s.mat, true);
    t3d_model_draw_skinned(characterModel, &s.skel);
    t3d_light_set_ambient((uint8_t[4]){ 255, 255, 255, 255 });
}
