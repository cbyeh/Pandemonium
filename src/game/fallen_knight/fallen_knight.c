#include <libdragon.h>
#include "gpu_frame.h"
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
#include "../boss/environmental_mechanics/boss_ground_crush.h"
#include "controllers/audio_controller.h"
#include "../../scenes/guardian/guardian_scene_sfx.h"
#include "../../scenes/guardian/guardian_scene_bounds.h"
#include "utilities/general_utility.h"
#include "utilities/game_math.h"
#include "utilities/simple_collision_utility.h"

extern T3DModel* characterModel;       // the player's knight model (character.c)

// ---- Tuning ----
#define FK_SCALE          (MODEL_SCALE * 0.8f)     // 1.6x the player knight
#define FK_CRACK          0.6f                     // its slams' floor cracks, against the Guardian's
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
    FKA_CHARGED, FKA_CLEAVE, FKA_HALFMOON, FKA_DEATH, FKA_BACKFLIP, FKA_KNOCKDOWN, FKA_BACKSTEP, FKA_COUNT
};
static const char *ANIM_NAMES[FKA_COUNT] = {
    "Idle", "Walk1", "Run", "Roll", "Attack1", "Attack2", "Attack3",
    "AttackCharged", "SkillCleave", "SkillHalfmoon", "Death", "SkillBackflip", "Knockdown", "WalkBackwards",
};
static const bool ANIM_LOOP[FKA_COUNT] = { true, true, true, [FKA_BACKSTEP] = true };

typedef enum {
    FK_INTRO, FK_CHASE, FK_COMBO, FK_ROLL, FK_CLEAVE, FK_HALFMOON, FK_MOONBLADE, FK_RECOVER, FK_STAGGER, FK_DEAD,
    FK_SPLITTER, FK_UPDOWN, FK_TRISLAM, FK_HOLYSLAM, FK_ROLLCUT, FK_PLUNGE,
} FkState;

// The combo's third blow: the plain overhead, the same held at the top of its rise (a roll
// on its first motion is tracked and caught), or a lunging thrust
enum { FIN_SLASH, FIN_DELAY, FIN_THRUST };

// The strike of each swing clip (clip seconds), measured from the blade's speed: the
// windups before it can't hurt. Attack1-3 (Attack2 rises from low in front: the up-down's
// first), then the charged clip's overhead slam (played from DN_FROM: the slam, no more)
static const float WIN[3][2] = { { 0.55f, 0.85f }, { 0.42f, 0.75f }, { 0.9f, 1.2f } };
static const float W_DN[2] = { 1.88f, 2.08f };
#define DN_FROM 1.72f     // the sword already raised: held there, then down

static struct {
    bool ready;
    T3DSkeleton skel, skelBlend;
    T3DAnim anims[FKA_COUNT];
    int anim;
    float blendT, blendDur; // crossfade timer (0 = done), and its length
    T3DMat4FP *mat;
    int swordBone;          // "Hand-Right": the blade runs along its -X
    int headBone;           // "Head": the body capsule's top while it lies staggered

    FkState state;
    float t;                // time in state
    int step;               // events fired in the state / combo index
    bool hitDone;           // the current swing already connected
    bool striking;          // inside a swing's damage window this frame
    float groundY, velY;
    float moveX, moveZ;     // roll / jump drift
    float cdCleave, cdHalfmoon, cdMoonblade, cdRoll, cdSplitter, cdUpdown, cdTri, cdHoly, cdPlunge;
    int finisher;           // the combo's third blow (FIN_*)
    float holdT;            // time held in a finisher's pause
    bool counter;           // the roll comes up into a rising cut
    float tri[3][2];        // the triangle slam's corners on the floor
    float windK;            // 0..1 through the current swing's windup, 0 once it strikes
    bool enraged;           // the war cry at half health
    float bladeTgt, bladeW; // the held blade pose (blade_pose): wanted and current weight
    float bladeDir[3];      // its direction: forward, side, up (the boss's frame)
    T3DQuat handBase, handPosed;
} s;

// ---- Helpers ----

#ifdef HLE_TEST_DUMMY
int hle_fk_commit, hle_fk_swing;    // test aid: swings begun, and aims committed (the sidestep test reacts to them)
static bool committed;
static bool hle_counts(void)    // HLE_SIDESTEP_NEW: only the new moves (the combo's third, the roll's cut, the plunge)
{
#ifdef HLE_SIDESTEP_NEW
    return (s.state == FK_COMBO && s.step == 3) || s.state == FK_ROLLCUT || s.state == FK_PLUNGE;
#else
    return true;
#endif
}
static void hle_commit(void) { if (!committed) { committed = true; if (hle_counts()) hle_fk_commit++; } }
#else
static inline void hle_commit(void) {}
#endif

static void play(int a, bool restart)
{
    if (a == s.anim && !restart) return;
#ifdef HLE_TEST_DUMMY
    committed = false;
    if (a >= FKA_ATTACK1 && a <= FKA_HALFMOON && hle_counts()) hle_fk_swing++;
#endif
    t3d_skeleton_blend(&s.skelBlend, &s.skel, &s.skel, 0.0f);    // snapshot the current pose
    // swings cut in fast (the blade's hits are read from the pose); going down, dying and
    // moving about ease over longer
    s.blendDur = (a == FKA_KNOCKDOWN || a == FKA_DEATH) ? 0.35f : (a == FKA_IDLE || a == FKA_WALK || a == FKA_RUN || s.anim == FKA_ROLL) ? 0.3f : 0.15f;
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
#ifdef HLE_LOG_MOVES     // test aid: each move as it starts (the hits after it are its: [hit])
    debugf("[move] %d\n", st);
#endif
    s.state = st;
    s.t = 0.0f;
    s.step = 0;
    s.hitDone = false;
    s.windK = 0.0f;
    s.holdT = 0.0f;
#ifdef HLE_FK_LOG   // test aid: each move as it starts
    debugf("[fk] state %d\n", st);
#endif
}

static void sfx(int id, float vol) { audio_play_scene_sfx_dist(id, vol * BONUS_SFX_VOL, 0.0f); }
static void swing_sfx(void) { sfx(SCENE1_SFX_SWING_BOSS, 1.0f); }

/** The drawn blade in world space, from the last pose: guard to tip, as the knight model has it
 *  in the hand's space. (The player's 640 along the hand's -X follows its sword trail; drawn
 *  without one, that reached 6 past this tip and ran 5 to the side of it) */
static bool sword_segment(float base[3], float tip[3])
{
    if (s.swordBone < 0) return false;
    const T3DMat4FP *B = &s.skel.boneMatricesFP[s.swordBone];
    float m0[3], m1[3];
    mat4fp_mul_point_f32_row3_colbasis(B, (float[3]){ -29.0f, 46.0f, -3.0f }, m0);
    mat4fp_mul_point_f32_row3_colbasis(B, (float[3]){ -522.0f, 69.0f, -68.0f }, m1);
    mat4fp_mul_point_f32_row3_colbasis(s.mat, m0, base);
    mat4fp_mul_point_f32_row3_colbasis(s.mat, m1, tip);
    return true;
}

/** Melee hit: once per swing, inside the clip's damage window, only where the blade meets the player */
static void melee_check(Boss *b, float t0, float t1, float damage)
{
    (void)b;
    float clip = t3d_anim_get_time(&s.anims[s.anim]);
#ifdef HLE_FK_HITLOG    // test aid: each window's closest approach of the drawn blade to the player (a miss), or the hit
    static bool inWin; static float minGap;
    if (inWin && (clip > t1 || clip < t0)) { inWin = false; if (minGap < 1e8f) debugf("[fkhit] st %d step %d clip %d MISS, closest gap %.1f\n", s.state, s.step, s.anim, (double)minGap); }
#endif
    if (clip < t0 || clip > t1) return;
    s.striking = true;
    if (s.hitDone) return;
    float base[3], tip[3];
    if (!sword_segment(base, tip)) return;
    const float *p = character.pos;
    const T3DVec3 *a = &character.capsuleCollider.localCapA, *c = &character.capsuleCollider.localCapB;
    float pa[3] = { p[0] + a->v[0], p[1] + a->v[1], p[2] + a->v[2] };
    float pb[3] = { p[0] + c->v[0], p[1] + c->v[1], p[2] + c->v[2] };
    bool hit = scu_capsule_vs_capsule_f(base, tip, 2.0f, pa, pb, character.capsuleCollider.radius);     // the blade's width
#ifdef HLE_FK_HITLOG
    {   float lo = 0.0f, hi = 120.0f;       // the gap between the blade's surface and the player's capsule
        for (int i = 0; i < 14; i++) { float m = (lo + hi) * 0.5f; if (scu_capsule_vs_capsule_f(base, tip, m, pa, pb, character.capsuleCollider.radius)) hi = m; else lo = m; }
        float gap = hi - 2.0f;
        if (!inWin) { inWin = true; minGap = 1e9f; }
        if (gap < minGap) minGap = gap;
        float fx = -fm_sinf(b->rot[1]), fz = fm_cosf(b->rot[1]), tx = tip[0] - b->pos[0], tz = tip[2] - b->pos[2], px = p[0] - b->pos[0], pz = p[2] - b->pos[2];
        if (hit) { inWin = false;
            debugf("[fkhit] st %d step %d clip %d HIT at %.2f: gap %.1f | blade base y %.0f tip y %.0f (boss y %.0f), tip fwd %.0f side %.0f | player fwd %.0f side %.0f y %.0f..%.0f r %.0f\n", s.state, s.step, s.anim, (double)clip, (double)gap,
                   (double)(base[1] - s.groundY), (double)(tip[1] - s.groundY), (double)(b->pos[1] - s.groundY), (double)(tx * fx + tz * fz), (double)(tx * fz - tz * fx), (double)(px * fx + pz * fz), (double)(px * fz - pz * fx),
                   (double)(pa[1] - s.groundY), (double)(pb[1] - s.groundY), (double)character.capsuleCollider.radius); }
    }
#endif
#ifdef HLE_LOG_HITVOL     // test aid: the old segment (640 along the hand's -X, 3 wide) against the drawn blade
    { const T3DMat4FP *B = &s.skel.boneMatricesFP[s.swordBone]; float m0[3], m1[3], o0[3], o1[3];
      mat4fp_mul_point_f32_row3_colbasis(B, (float[3]){ 0, 0, 0 }, m0); mat4fp_mul_point_f32_row3_colbasis(B, (float[3]){ -640.0f, 0, 0 }, m1);
      mat4fp_mul_point_f32_row3_colbasis(s.mat, m0, o0); mat4fp_mul_point_f32_row3_colbasis(s.mat, m1, o1);
      bool old = scu_capsule_vs_capsule_f(o0, o1, 3.0f, pa, pb, character.capsuleCollider.radius);
      static int n; if (old != hit && n++ < 40) debugf("[hitvol] fk swing clip %d: the old segment %s, the blade %s (old tip %.1f from the drawn tip)\n", s.anim, old ? "hits" : "misses", hit ? "hits" : "misses",
          (double)sqrtf((o1[0]-tip[0])*(o1[0]-tip[0]) + (o1[1]-tip[1])*(o1[1]-tip[1]) + (o1[2]-tip[2])*(o1[2]-tip[2]))); }
#endif
    if (!hit) return;
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
    if (clip >= pre && s.windK > 0.0f) swing_sfx();     // its whoosh then: it peaks ~0.09 s on, at the strike
    t3d_anim_set_speed(&s.anims[s.anim], clip < pre ? fminf(1.0f, (pre - from) / wind) : 1.0f);
    s.windK = clip < pre ? clamp01f((clip - from) / (pre - from)) : 0.0f;
}

#define FK_LOCK_K 0.7f      // a swing stops turning this far through its windup: it commits

/** True while a swing may still turn to the player (until lockK through its windup) */
static bool swing_tracks(float lockK)
{
    if (s.windK < lockK) return true;
    hle_commit();
    return false;
}

/** Holds the blade along (forward, side, up) in the boss's frame, eased in over ~0.25 s */
static void hold_blade(float f, float sd, float u)
{
    s.bladeTgt = 1.0f;
    s.bladeDir[0] = f; s.bladeDir[1] = sd; s.bladeDir[2] = u;
}

static void quat_rotate(const T3DQuat *q, const float v[3], float out[3])
{
    const float *x = q->v;      // v + 2w (q x v) + 2 q x (q x v)
    float t[3] = { 2.0f * (x[1] * v[2] - x[2] * v[1]), 2.0f * (x[2] * v[0] - x[0] * v[2]), 2.0f * (x[0] * v[1] - x[1] * v[0]) };
    out[0] = v[0] + x[3] * t[0] + (x[1] * t[2] - x[2] * t[1]);
    out[1] = v[1] + x[3] * t[1] + (x[2] * t[0] - x[0] * t[2]);
    out[2] = v[2] + x[3] * t[2] + (x[0] * t[1] - x[1] * t[0]);
}

/** Turns the sword hand (after the clip and its crossfade) so the blade points along bladeDir, by bladeW */
static void blade_pose(float dt)
{
    float rate = s.bladeTgt > s.bladeW ? 4.0f : 8.0f;      // in over 0.25 s, out over 0.12
    s.bladeW += fmaxf(-rate * dt, fminf(rate * dt, s.bladeTgt - s.bladeW));
    if (s.swordBone < 0) return;
    T3DBone *h = &s.skel.bones[s.swordBone];
    if (h->parentIdx == 0xFFFF) return;
    // a clip that has stopped no longer writes the hand: start from its own pose, not ours
    if (memcmp(&h->rotation, &s.handPosed, sizeof(T3DQuat)) == 0) { h->rotation = s.handBase; h->hasChanged = true; }
    if (s.bladeW <= 0.0f) return;
    s.handBase = h->rotation;
    const T3DMat4x3 *P = &s.skel.bones[h->parentIdx].matrix;   // last frame's: the arm isn't posed here
    float d[3] = { -s.bladeDir[0], s.bladeDir[2], s.bladeDir[1] };     // model space: forward is -X, side +Z
    float u[3], c[3], l = 0.0f;
    for (int i = 0; i < 3; i++) { u[i] = P->m[i][0] * d[0] + P->m[i][1] * d[1] + P->m[i][2] * d[2]; l += u[i] * u[i]; }
    if (l < 1e-8f) return;
    l = 1.0f / sqrtf(l);
    for (int i = 0; i < 3; i++) u[i] *= l;
    quat_rotate(&h->rotation, (float[3]){ -1.0f, 0.0f, 0.0f }, c);      // the blade now, in the arm's space
    float ax[3] = { c[1] * u[2] - c[2] * u[1], c[2] * u[0] - c[0] * u[2], c[0] * u[1] - c[1] * u[0] };
    float sn = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    if (sn < 1e-4f) return;
    for (int i = 0; i < 3; i++) ax[i] /= sn;
    float w = s.bladeW * s.bladeW * (3.0f - 2.0f * s.bladeW);
    T3DQuat r, q;
    t3d_quat_from_rotation(&r, ax, atan2f(sn, c[0] * u[0] + c[1] * u[1] + c[2] * u[2]) * w);
    t3d_quat_mul(&q, &r, &h->rotation);
    t3d_quat_normalize(&q);
    h->rotation = s.handPosed = q;
    h->hasChanged = true;
}

// ---- Lifetime ----

void fallen_knight_free(void)
{
    if (!s.ready) return;
    gpu_wait();
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
        s.headBone = t3d_skeleton_find_bone(&s.skel, "Head");
        s.ready = true;
#ifdef HLE_FK_POSESCAN  // test aid: the blade's direction (model space, y up) through every clip
        for (int i = 0; i < FKA_COUNT; i++) {
            T3DAnim *a = &s.anims[i];
            t3d_anim_attach(a, &s.skel); t3d_anim_set_playing(a, true);
            debugf("[pose] clip %d %s lasts %.2f s\n", i, ANIM_NAMES[i], (double)t3d_anim_get_length(a));
            for (float t = 0.0f; t <= t3d_anim_get_length(a) + 1e-3f; t += 0.05f) {
                t3d_anim_set_time(a, t); t3d_anim_update(a, 0.0f); t3d_skeleton_update(&s.skel);
                float b0[3], b1[3];
                mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[s.swordBone], (float[3]){ 0, 0, 0 }, b0);
                mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[s.swordBone], (float[3]){ -640.0f, 0, 0 }, b1);
                float d[3] = { b1[0] - b0[0], b1[1] - b0[1], b1[2] - b0[2] }, l = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]) + 1e-3f;
                debugf("[pose] %d %.2f dir %.2f %.2f %.2f hand %.0f %.0f %.0f\n", i, (double)t, (double)(d[0]/l), (double)(d[1]/l), (double)(d[2]/l), (double)b0[0], (double)b0[1], (double)b0[2]);
            }
            t3d_anim_set_playing(a, false);
        }
#endif
    }
    s.anim = -1;
    play(FKA_IDLE, true);

    boss->phaseIndex = FALLEN_KNIGHT_PHASE;
    boss->name = "Fallen Warrior";
    boss->health = boss->maxHealth;
    boss->mode = BOSS_MODE_CINEMATIC;       // not damageable until it lands
    boss->state = BOSS_STATE_INTRO;
    boss->poise = boss->poiseIdleT = boss->staggerT = 0.0f;
    boss->damageFlashTimer = 0.0f;
    boss->velX = boss->velZ = 0.0f;
    boss->handAttackColliderActive = false;
    boss->sphereAttackColliderActive = false;
    boss->visible = true;
    // Knight-sized capsule (1.6x the player's): its top at the helmet's (31; at 37 the sword met air over it)
    boss->capsuleCollider.localCapA = (T3DVec3){{ 0.0f, 6.0f, 0.0f }};
    boss->capsuleCollider.localCapB = (T3DVec3){{ 0.0f, 20.0f, 0.0f }};
    boss->capsuleCollider.radius = 11.0f;

    s.groundY = character.pos[1];
    boss->pos[1] = s.groundY + FK_DROP_HEIGHT;
    s.velY = 0.0f;
    s.cdCleave = 5.0f; s.cdHalfmoon = 12.0f; s.cdMoonblade = 9.0f; s.cdRoll = 0.0f; s.cdSplitter = 6.0f; s.cdUpdown = 3.0f; s.cdTri = 14.0f; s.cdHoly = 20.0f; s.cdPlunge = 8.0f;
    s.enraged = false;
    s.bladeW = 0.0f;
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
    if (s.state == FK_PLUNGE && s.step == 1) return false;
    b->pos[1] = s.groundY;
    enter(FK_STAGGER);
    play(FKA_KNOCKDOWN, true);
    t3d_anim_set_speed(&s.anims[FKA_KNOCKDOWN], t3d_anim_get_length(&s.anims[FKA_KNOCKDOWN]) / BOSS_STAGGER_S);   // down and up again in the stagger
    b->staggerT = BOSS_STAGGER_S;
    return true;
}

// ---- Skills (hostile versions of the player's) ----

static const float CLEAVE_SPREAD[5] = { 0.0f, -0.3f, 0.3f, -0.6f, 0.6f };

static void cleave_point(const Boss *b, int i, float o[3], float *dx, float *dz)    // where blade i rises
{
    float a = b->rot[1] + CLEAVE_SPREAD[i], dist = 22.0f + i * 10.0f;
    *dx = -fm_sinf(a); *dz = fm_cosf(a);
    o[0] = b->pos[0] + *dx * dist; o[1] = s.groundY; o[2] = b->pos[2] + *dz * dist;
}

static void cleave_update(Boss *b, float dt)
{
    // Stands and plants its sword point-down (sparks where it bites and where the blades
    // will rise), sinks into a crouch with it, then rips it up: five crimson blades rise one
    // by one, fanned along where it faced (it doesn't turn)
    const float PLANT = 0.3f, CROUCH = 0.5f, FROM = 0.25f, SINK = 1.5f, RELEASE = CROUCH + (0.85f - FROM) / SINK + 0.05f, GAP = 0.12f;
    if (s.anim == FKA_CLEAVE) t3d_anim_set_speed(&s.anims[s.anim], clip_time() < 0.85f ? SINK : 1.0f);
    if (s.t < RELEASE) {
        if (s.t < CROUCH) play(FKA_IDLE, false);
        else if (s.anim != FKA_CLEAVE) { play(FKA_CLEAVE, true); t3d_anim_set_time(&s.anims[s.anim], FROM); }
        if (s.t < RELEASE - 0.12f) hold_blade(0.3f, 0.0f, -0.95f);
        if (s.t >= PLANT - 0.07f && s.t - dt < PLANT - 0.07f) sfx(SCENE1_SFX_BOSS_LAND1, 0.8f);    // its onset on the bite
        if (s.t >= PLANT && s.t - dt < PLANT) {
            float base[3], tip[3];
            if (sword_segment(base, tip)) skill_fx_spawn_burst_hostile((float[3]){ tip[0], s.groundY, tip[2] }, 0.0f);
            for (int i = 0; i < 5; i += 2) { float o[3], dx, dz; cleave_point(b, i, o, &dx, &dz); skill_fx_spawn_burst_hostile(o, b->rot[1] + i); }
            screen_shake_set_shake_mag(2.0f);
        }
    }
    if (s.t >= RELEASE - 0.06f && s.t - dt < RELEASE - 0.06f) swing_sfx();     // peaking as it rips up
    if (s.step < 5 && s.t >= RELEASE + s.step * GAP) {
        float o[3], dx, dz;
        cleave_point(b, s.step, o, &dx, &dz);
        skill_fx_spawn_wave_hostile(SKILL_WAVE_BLADE, o, (float[3]){ dx * 6.0f, 0.0f, dz * 6.0f }, 5.0f, 2, 0.4f, 1.5f, false);
        if (s.step == 0) screen_shake_set_shake_mag(5.0f);
        s.step++;
    }
    if (s.t > RELEASE + 0.95f) enter(FK_RECOVER);
}

static void halfmoon_update(Boss *b, float dt)
{
    // Crouches with its sword held level and out to the side, settles its aim and holds it
    // (marks on the floor show the crescent's reach), throbbing crimson, a flash, then the
    // wide rolling slash where it faced: get round its side, or roll through it
    const float LOCK = 0.35f, CHARGE = 1.6f, HALF = 1.0f;      // the crescent reaches 57 degrees each side
    if (s.step == 0) {
        t3d_anim_set_speed(&s.anims[s.anim], 2.0f / CHARGE);   // the clip's sweep (~2.0 s in) as the crescent leaves
        if (s.t < LOCK) face_player(b, dt, 4.0f);
        else if (s.t - dt < LOCK) sfx(SCENE1_SFX_SPELL_CAST_BOSS, 0.8f);
        if (clip_time() > 0.3f) hold_blade(-0.45f, -0.89f, 0.0f);
        if (s.t >= LOCK && (s.t - dt < LOCK || (int)(s.t / 0.3f) != (int)((s.t - dt) / 0.3f))) {      // its edges and middle, then between
            bool odd = (int)(s.t / 0.3f) & 1;
            for (int k = odd ? -1 : -2; k <= 2; k += 2) {
                float a = b->rot[1] + k * HALF * 0.5f;
                skill_fx_spawn_burst_hostile((float[3]){ b->pos[0] - fm_sinf(a) * 34.0f, s.groundY, b->pos[2] + fm_cosf(a) * 34.0f }, a);
            }
        }
        if (s.t >= CHARGE) {
            skill_fx_next_arc_half(HALF);
            skill_fx_spawn_arc_hostile((float[3]){ b->pos[0], b->pos[1] + 12.0f, b->pos[2] }, b->rot[1], 90.0f, 30.0f);
            screen_shake_set_shake_mag(12.0f);
            sfx(SCENE1_SFX_BLADE_SLASH_BOSS, 1.0f);
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
        t3d_anim_set_time(&s.anims[s.anim], WIN[(s.step - 1) % 3][0] - 0.1f);     // the strike with the wave (the gap cut the clip short)
        t3d_anim_set_speed(&s.anims[s.anim], 1.2f);
        s.step++;
    }
    if (b->pos[1] <= s.groundY && s.velY < 0.0f) {
        b->pos[1] = s.groundY;
        skill_fx_spawn_burst_hostile((float[3]){ b->pos[0], s.groundY, b->pos[2] }, b->rot[1]);
        screen_shake_set_shake_mag(6.0f);
        sfx(SCENE1_SFX_BOSS_LAND1, 0.8f);
        enter(FK_RECOVER);
    }
}

static void splitter_update(Boss *b, float dt)
{
    // Moonlight Splitter: plants its feet, then three wound-up swings, each
    // loosing a crescent at the player (level, the other diagonal, upright): step aside
    static const int clips[3] = { FKA_ATTACK1, FKA_ATTACK2, FKA_ATTACK3 };
    static const float roll[3] = { 0.25f, -0.6f, 1.45f };
    if (s.step == 0) {      // a step back before it looses them
        face_player(b, dt, 4.0f);
        if (s.t < 0.45f) {
            play(FKA_BACKSTEP, false);
            move(b, fm_sinf(b->rot[1]) * 22.0f, -fm_cosf(b->rot[1]) * 22.0f, dt);
        } else play(FKA_IDLE, false);
        if (s.t <= dt) sfx(SCENE1_SFX_SPELL_WAVE_BOSS, 0.8f);
        s.windK = clamp01f(s.t / 0.5f);
        if (s.t >= 0.6f) { s.step = 1; play(clips[0], true); }
        return;
    }
    const float *w = WIN[s.step - 1];
    float clip = clip_time();
    swing_pace(0.0f, w[0], 0.55f);
    if (clip < w[0] && swing_tracks(0.35f)) face_player(b, dt, 3.0f);     // then the aim holds (early: it flies): sidestep it
    if (!s.hitDone && clip >= w[0]) {
        s.hitDone = true;
        float fx = -fm_sinf(b->rot[1]), fz = fm_cosf(b->rot[1]);
        float o[3] = { b->pos[0] + fx * 14.0f, s.groundY + 14.0f, b->pos[2] + fz * 14.0f };
        const float SPEED = 80.0f;
        skill_fx_spawn_crescent_hostile(o, (float[3]){ fx * SPEED, 0.0f, fz * SPEED }, roll[s.step - 1], 12.0f, 2.0f);
        screen_shake_set_shake_mag(3.0f);
    }
    if (s.hitDone && (clip >= w[1] || clip_done())) {
        if (s.step < 3) { s.step++; s.hitDone = false; play(clips[s.step - 1], true); }
        else enter(FK_RECOVER);
    }
}

static void updown_update(Boss *b, float dt)
{
    // Two wound-up slashes: rising from low in front, then held overhead and down
    // with a burst where it lands: back off or roll each; each commits late in its windup
    float clip = clip_time();
    const float *w = s.step == 0 ? WIN[1] : W_DN;
    swing_pace(s.step == 0 ? 0.0f : DN_FROM, w[0], 0.7f);
    if (clip < w[0] && swing_tracks(FK_LOCK_K)) face_player(b, dt, 3.0f);
    melee_check(b, w[0], w[1], s.step == 0 ? 14.0f : 18.0f);
    if (s.step == 0) {
        if (clip >= w[1] || clip_done()) { s.step = 1; s.hitDone = false; play(FKA_CHARGED, true);
                                             t3d_anim_set_time(&s.anims[s.anim], DN_FROM); t3d_anim_set_speed(&s.anims[s.anim], 0.1f); }
    } else {
        if (s.step == 1 && clip >= 1.95f && clip - dt < 1.95f) sfx(SCENE1_SFX_BOSS_LAND1, 0.7f);
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
    // Triangle slam: the sword raised, then driven into the floor; a triangle of
    // warnings closes round the player and blades erupt along it, striking all inside: get out
    const float ERUPT = 0.7f;
    float clip = clip_time();
    if (s.step == 0) {
        face_player(b, dt, 3.0f);
        swing_pace(DN_FROM, W_DN[0], 0.75f);
        if (clip >= 1.95f && clip - dt < 1.95f) sfx(SCENE1_SFX_BOSS_LAND1, 0.8f);     // its onset on the bite
        if (clip >= 2.02f) {    // the blade meets the floor: mark the triangle where the player stands
            float fx = -fm_sinf(b->rot[1]), fz = fm_cosf(b->rot[1]);
            skill_fx_spawn_burst_hostile((float[3]){ b->pos[0] + fx * 22.0f, s.groundY, b->pos[2] + fz * 22.0f }, b->rot[1]);
            screen_shake_set_shake_mag(8.0f);
            boss_ground_crush_spawn_varied(b->pos[0] + fx * 22.0f, s.groundY + 0.25f, b->pos[2] + fz * 22.0f, FK_CRACK, b->rot[1]);
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
            sfx(SCENE1_SFX_SLAM_BOSS, 1.0f);     // its thump as the blades erupt
            s.step = 2; s.t = 0.0f;
        }
    } else if (s.t >= 0.5f) enter(FK_RECOVER);
}

static void holyslam_update(Boss *b, float dt)
{
    // A swing, then the sword held high while the light gathers round
    // it, then slammed down: a blast all round it. Get far away, roll it or jump it
    float clip = clip_time();
    if (s.step == 0) {          // the opening swing (wound up like the combo's)
        swing_pace(0.0f, WIN[0][0], FK_WIND);
        if (clip < WIN[0][0] && swing_tracks(FK_LOCK_K)) face_player(b, dt, 2.5f);
        melee_check(b, WIN[0][0], WIN[0][1], FK_MELEE_DAMAGE);
        if (clip >= WIN[0][1] || clip_done()) {
            s.step = 1; s.t = 0.0f; s.hitDone = false;
            play(FKA_CHARGED, true); t3d_anim_set_time(&s.anims[s.anim], DN_FROM); t3d_anim_set_speed(&s.anims[s.anim], 0.05f);
            sfx(SCENE1_SFX_HOLY_BOSS, 1.0f);     // the light called down, swelling to the slam
        }
    } else if (s.step == 1) {   // the gathering
        face_player(b, dt, 1.5f);
        swing_pace(DN_FROM, W_DN[0], 1.5f);
        if (!s.hitDone && clip >= W_DN[0] - 0.08f) s.hitDone = true;    // as it comes down
        if ((int)(s.t * 5.0f) != (int)((s.t - dt) * 5.0f)) {    // the light drawn in toward it
            float a = s.t * 5.0f, r = 45.0f - 25.0f * clamp01f(s.t / 1.5f);
            skill_fx_spawn_burst_hostile((float[3]){ b->pos[0] + sinf(a) * r, s.groundY, b->pos[2] + cosf(a) * r }, a);
        }
        if (clip >= 2.02f) {
            float o[3] = { b->pos[0], s.groundY + 4.0f, b->pos[2] };
            skill_fx_spawn_arc_hostile(o, b->rot[1], 105.0f, 26.0f);
            skill_fx_spawn_arc_hostile(o, b->rot[1] + T3D_PI, 105.0f, 26.0f);
            skill_fx_spawn_pillar_hostile((float[3]){ b->pos[0], s.groundY, b->pos[2] });
            {   // where the blade bites
                float base[3], tip[3], cx = b->pos[0] - fm_sinf(b->rot[1]) * 22.0f, cz = b->pos[2] + fm_cosf(b->rot[1]) * 22.0f;
                if (sword_segment(base, tip)) { cx = tip[0]; cz = tip[2]; }
                boss_ground_crush_spawn_varied(cx, s.groundY + 0.25f, cz, FK_CRACK, b->rot[1]);
            }
            screen_shake_set_shake_mag(16.0f);
            sfx(SCENE1_SFX_SLAM_BOSS, 1.0f);     // its thump on this frame
            s.step = 2; s.t = 0.0f;
        }
    } else if (s.t >= 0.8f) enter(FK_RECOVER);     // spent a moment: the opening
}

static inline float smooth01(float x) { x = clamp01f(x); return x * x * (3.0f - 2.0f * x); }

/** Sets the clip's speed so it reaches `target` (clip seconds) this frame: a pose driven along a curve */
static void clip_to(float target, float dt)
{
    if (dt > 0.0f) t3d_anim_set_speed(&s.anims[s.anim], fmaxf(0.0f, (target - clip_time()) / dt));
}

#define DELAY_FROM 0.40f    // Attack3 rising, the blade going up behind its head
#define DELAY_HOLD 0.75f    // held there, still turning to the player, before it comes on

/** The delayed third: the overhead's rise eases to a near stop at the top and holds, then goes on */
static void delay_hold(float dt)
{
    if (clip_time() < DELAY_FROM || s.holdT >= DELAY_HOLD) return;
    s.holdT += dt;
    float k = s.holdT / DELAY_HOLD, e = smooth01(fminf(k, 1.0f - k) / 0.2f);
    t3d_anim_set_speed(&s.anims[s.anim], 1.0f - 0.94f * e);
}

/** How far ahead of the boss the player is (along its facing), or far if off to its side or behind */
static float dist_ahead(const Boss *b)
{
    float fx = -fm_sinf(b->rot[1]), fz = fm_cosf(b->rot[1]);
    float px = character.pos[0] - b->pos[0], pz = character.pos[2] - b->pos[2], ahead = px * fx + pz * fz;
    return (ahead > 0.0f && fabsf(px * fz - pz * fx) < 12.0f) ? ahead : 1e9f;
}

static void thrust_update(Boss *b, float dt)
{
    // The thrust: Attack1's draw (the hand back at its hip) held with the blade levelled at the
    // player, still turning, then it commits and drives the hand forward with a lunge, the blade
    // held level all the way: sidestep it
    const float COIL = 0.27f, HOLD = 0.6f, LUNGE = 95.0f;
    static const float W_TH[2] = { 0.48f, 0.76f };
    float clip = clip_time();
    if (clip < COIL) t3d_anim_set_speed(&s.anims[s.anim], 1.0f);
    else if (s.holdT < HOLD) {          // eased to a stop and away again
        s.holdT += dt;
        if (s.holdT >= HOLD) swing_sfx();      // the lunge's whoosh, peaking on the strike
        float k = s.holdT / HOLD;
        t3d_anim_set_speed(&s.anims[s.anim], 1.0f - 0.99f * smooth01(fminf(k, 1.0f - k) / 0.15f));
    } else t3d_anim_set_speed(&s.anims[s.anim], 1.15f);
    if (clip > 0.12f && clip < 0.86f) hold_blade(1.0f, -0.12f, -0.04f);
    if (s.holdT < HOLD * 0.5f) face_player(b, dt, 3.0f);
    else hle_commit();
    if (clip >= W_TH[0] - 0.06f && clip < W_TH[1]) {      // the lunge, along its committed aim, not through the player
        if (dist_ahead(b) > 18.0f) move(b, -fm_sinf(b->rot[1]) * LUNGE, fm_cosf(b->rot[1]) * LUNGE, dt);
    }
    melee_check(b, W_TH[0], W_TH[1], 13.0f);
    if (clip_done()) enter(FK_RECOVER);
}

static void rollcut_update(Boss *b, float dt)
{
    // Out of its roll it comes up into the rising cut, turning to the player on the way and
    // stepping in as it strikes (the roll's drift carried on); it commits late in the rise
    const float *w = WIN[1];
    float clip = clip_time();
    swing_pace(0.0f, w[0], 0.6f);
    if (clip < w[0] && swing_tracks(FK_LOCK_K)) face_player(b, dt, 5.0f);
    if (clip >= w[0] - 0.15f && clip < w[1] && dist_ahead(b) > 20.0f) move(b, -fm_sinf(b->rot[1]) * 60.0f, fm_cosf(b->rot[1]) * 60.0f, dt);
    melee_check(b, w[0], w[1], 14.0f);
    if (clip_done()) enter(FK_CHASE);       // a counter: it keeps the pressure on (no breather after it)
}

static void plunge_update(Boss *b, float dt)
{
    // Leaping plunge: AttackCharged's crouch (the sword drawn back low), a leap at where the player
    // stood as it left (the aim held: step aside), the sword raised overhead at the top, then
    // driven down as it lands: a ring of force where it bites, and a moment wrenching it free
    const float CROUCH = 0.7f, VY = 95.0f, G = 190.0f, FLIGHT = 2.0f * VY / G, DOWN = 0.16f;
    float clip = clip_time();
    if (s.step == 0) {
        face_player(b, dt, 4.0f);
        clip_to(0.3f + 0.95f * smooth01((s.t + dt) / CROUCH), dt);
        if (s.t >= CROUCH) {
            float dx, dz, d = to_player(b, &dx, &dz);
            float go = fmaxf(0.0f, d - 16.0f) / FLIGHT;     // its blade bites ~22 in front: it lands short of the player
            if (d > 1e-3f) { s.moveX = dx / d * go; s.moveZ = dz / d * go; } else s.moveX = s.moveZ = 0.0f;
            b->rot[1] = atan2f(-dx, dz);
            s.velY = VY; s.step = 1; s.t = 0.0f;
            hle_commit();
            sfx(SCENE1_SFX_SWING_BOSS, 0.8f);
        }
    } else if (s.step == 1) {
        s.striking = true;          // in the air: the stagger meter keeps (as mid-strike)
        move(b, s.moveX, s.moveZ, dt);
        s.velY -= G * dt;
        b->pos[1] = fmaxf(s.groundY, b->pos[1] + s.velY * dt);
        if (s.t >= FLIGHT - DOWN - 0.09f && s.t - dt < FLIGHT - DOWN - 0.09f) swing_sfx();     // the blade driven down
        float u = s.t + dt;     // where the clip should be at this frame's end
        clip_to(u < FLIGHT - DOWN ? 1.25f + 0.6f * smooth01(u / 0.6f) : 1.86f + (u - (FLIGHT - DOWN)), dt);
        melee_check(b, W_DN[0], W_DN[1], 18.0f);
        if (b->pos[1] <= s.groundY && s.velY < 0.0f) {
            float base[3], tip[3], o[3] = { b->pos[0], s.groundY + 4.0f, b->pos[2] };
            if (sword_segment(base, tip)) { o[0] = tip[0]; o[2] = tip[2]; }     // where the blade bites
            skill_fx_spawn_burst_hostile((float[3]){ o[0], s.groundY, o[2] }, b->rot[1]);
            boss_ground_crush_spawn_varied(o[0], s.groundY + 0.25f, o[2], FK_CRACK, b->rot[1]);
            float dmg = s.hitDone ? 0.0f : 10.0f;       // the blade itself already struck: the ring doesn't again
            skill_fx_spawn_arc_hostile(o, b->rot[1], 20.0f, dmg);
            skill_fx_spawn_arc_hostile(o, b->rot[1] + T3D_PI, 20.0f, dmg);
            screen_shake_set_shake_mag(10.0f);
            sfx(SCENE1_SFX_BOSS_LAND1, 1.0f);
            s.step = 2; s.t = 0.0f;
        }
    } else {
        clip_to(fminf(2.35f, clip + dt * (clip < 2.12f ? 0.8f : 0.45f)), dt);    // slow to pull it free
        if (s.t > 0.95f) enter(FK_RECOVER);
    }
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
    if (HLE_FK_FORCE == 2) { enter(FK_CLEAVE); debugf("[fk] cleave\n"); return; }
    if (HLE_FK_FORCE == 3) { enter(FK_SPLITTER); debugf("[fk] splitter\n"); return; }      // 3 = splitter, 4 = up-down, 5 = combo
    if (HLE_FK_FORCE == 7) { enter(FK_HOLYSLAM); play(FKA_ATTACK1, true); debugf("[fk] holyslam\n"); return; }
    if (HLE_FK_FORCE == 6) { enter(FK_TRISLAM); play(FKA_CHARGED, true); t3d_anim_set_time(&s.anims[s.anim], DN_FROM); t3d_anim_set_speed(&s.anims[s.anim], 0.1f); debugf("[fk] trislam\n"); return; }
    if (HLE_FK_FORCE == 8) { if (dist < 40.0f) return; s.cdPlunge = 0.0f; enter(FK_PLUNGE); play(FKA_CHARGED, true); t3d_anim_set_time(&s.anims[s.anim], 0.3f); debugf("[fk] plunge\n"); return; }
    if (HLE_FK_FORCE >= 4 && dist > FK_MELEE_RANGE - 4.0f) return;     // the melee ones: in reach first
    if (HLE_FK_FORCE == 4) { enter(FK_UPDOWN); play(FKA_ATTACK2, true); debugf("[fk] updown\n"); return; }
    if (HLE_FK_FORCE == 5) { enter(FK_COMBO); s.step = 1; play(FKA_ATTACK1, true); debugf("[fk] combo\n"); return; }
#endif
    if (s.cdHalfmoon <= 0.0f && dist < 70.0f) { s.cdHalfmoon = 16.0f; enter(FK_HALFMOON); play(FKA_HALFMOON, true); return; }
    if (s.cdMoonblade <= 0.0f && dist > 40.0f) { s.cdMoonblade = 12.0f; enter(FK_MOONBLADE); return; }
    if (s.enraged && s.cdHoly <= 0.0f && dist < 50.0f) { s.cdHoly = 20.0f; enter(FK_HOLYSLAM); play(FKA_ATTACK1, true); return; }
    if (s.cdTri <= 0.0f && dist > 40.0f && dist < 140.0f) {
        s.cdTri = 14.0f; enter(FK_TRISLAM); play(FKA_CHARGED, true); t3d_anim_set_time(&s.anims[s.anim], DN_FROM); t3d_anim_set_speed(&s.anims[s.anim], 0.1f); return;
    }
    if (s.cdPlunge <= 0.0f && dist > 50.0f && dist < 120.0f) {
        s.cdPlunge = 14.0f; enter(FK_PLUNGE); play(FKA_CHARGED, true); t3d_anim_set_time(&s.anims[s.anim], 0.3f); return;
    }
    if (s.cdSplitter <= 0.0f && dist > 35.0f && dist < 130.0f) { s.cdSplitter = 11.0f; enter(FK_SPLITTER); return; }
    if (s.cdCleave <= 0.0f && dist < 60.0f) { s.cdCleave = 9.0f; enter(FK_CLEAVE); return; }
    if (s.cdUpdown <= 0.0f && dist < FK_MELEE_RANGE + 8.0f) { s.cdUpdown = 7.0f; enter(FK_UPDOWN); play(FKA_ATTACK2, true); return; }
    if (dist < FK_MELEE_RANGE - 4.0f) { enter(FK_COMBO); s.step = 1; play(FKA_ATTACK1, true); return; }
}

void fallen_knight_update(Boss* b, float dt)
{
    if (!s.ready) return;
    if (b->damageFlashTimer > 0.0f) b->damageFlashTimer -= dt;
    s.t += dt;
    s.cdCleave -= dt; s.cdHalfmoon -= dt; s.cdMoonblade -= dt; s.cdRoll -= dt; s.cdSplitter -= dt; s.cdUpdown -= dt; s.cdTri -= dt; s.cdHoly -= dt; s.cdPlunge -= dt;
    s.bladeTgt = 0.0f;      // a move that holds the blade asks again each frame (hold_blade)

    float dx, dz, dist = to_player(b, &dx, &dz);

    if (b->state == BOSS_STATE_DEAD && s.state != FK_DEAD) {
        enter(FK_DEAD);
        play(FKA_DEATH, true);
        b->pos[1] = s.groundY;
    }


    // Half health: a war cry and a crimson burst (the fight's turn, like the others' breaks)
    if (!s.enraged && b->mode == BOSS_MODE_COMBAT && b->health > 0.0f && b->health <= b->maxHealth * 0.5f) {
        s.enraged = true;
        sfx(SCENE1_SFX_WAR_CRY, 1.0f);
        skill_fx_spawn_burst_hostile((float[3]){ b->pos[0], s.groundY, b->pos[2] }, b->rot[1]);
        screen_shake_set_shake_mag(6.0f);
    }

    b->striking = s.striking || s.state == FK_ROLL;     // mid-swing (melee_check) or rolling off
    s.striking = false;
#ifdef HLE_TEST_DUMMY
    { extern void hle_open_tick(bool, float); hle_open_tick((s.state == FK_CHASE || s.state == FK_RECOVER), dt); }   // test aid: measure the openings
#endif
    switch (s.state) {
    case FK_INTRO:
        // Falls from above in a crimson light, lands with a burst, then fights
        b->pos[1] = s.groundY + FK_DROP_HEIGHT * fmaxf(0.0f, 1.0f - s.t / (FK_INTRO_TIME * 0.6f));
        face_player(b, dt, 10.0f);
        if (s.t >= FK_INTRO_TIME * 0.6f - 0.07f && s.t - dt < FK_INTRO_TIME * 0.6f - 0.07f) sfx(SCENE1_SFX_BOSS_LAND1, 1.0f);   // onset on the landing
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
        if (ps == CHAR_STATE_ATTACKING && dist < 28.0f && s.cdRoll <= 0.0f) {
            s.cdRoll = 2.5f;
            if ((rand_custom_u32() % 100) < 40) {
                float side = (rand_custom_u32() & 1) ? 1.0f : -1.0f;
                s.counter = (rand_custom_u32() % 100) < 50;    // half the time it comes up cutting: then it rolls round, not away
#ifdef HLE_FK_COUNTER
                s.counter = true;
#endif
                float back = s.counter ? 0.15f : 0.6f;
                s.moveX = (-dx / dist * back + dz / dist * side) * 70.0f;
                s.moveZ = (-dz / dist * back - dx / dist * side) * 70.0f;
                enter(FK_ROLL);
                play(FKA_ROLL, true);
                break;
            }
        }
        if (s.t > 0.7f) choose(b, dist);
        break;
    }

    case FK_COMBO:
        if (s.step == 3 && s.finisher == FIN_THRUST) { thrust_update(b, dt); break; }
        {   // each swing wound up, tracking the player until it commits, just before the strike
            const float *w = WIN[s.anim == FKA_ATTACK2 ? 1 : s.anim == FKA_ATTACK3 ? 2 : 0];
            swing_pace(0.0f, w[0], FK_WIND);
            bool delay = s.step == 3 && s.finisher == FIN_DELAY;
            if (delay) delay_hold(dt);
            if (clip_time() < w[0] && ((s.holdT > 0.0f && s.holdT < DELAY_HOLD) || swing_tracks(FK_LOCK_K))) face_player(b, dt, 2.5f);
            melee_check(b, w[0], w[1], delay ? 13.0f : FK_MELEE_DAMAGE);
        }
        if (clip_done()) {
            if (s.step < 3 && dist < FK_MELEE_RANGE + 6.0f) {
                s.step++;
                s.hitDone = false;
                if (s.step == 3) {      // the third: as before, held at its top, or the thrust
                    uint32_t r = rand_custom_u32() % 100;
                    s.finisher = r < 35 ? FIN_DELAY : r < 70 ? FIN_THRUST : FIN_SLASH;
#ifdef HLE_FK_FIN
                    s.finisher = HLE_FK_FIN;
#endif
                    s.holdT = 0.0f;
#ifdef HLE_FK_LOG
                    debugf("[fk] combo finisher %d\n", s.finisher);
#endif
                }
                play(s.step == 2 ? FKA_ATTACK2 : s.finisher == FIN_THRUST ? FKA_ATTACK1 : FKA_ATTACK3, true);
            } else {
                enter(FK_RECOVER);
            }
        }
        break;

    case FK_ROLL:
        move(b, s.moveX, s.moveZ, dt);
        if (s.t > 0.55f) {
            if (s.counter) { enter(FK_ROLLCUT); play(FKA_ATTACK2, true); }
            else enter(FK_CHASE);
        }
        break;

    case FK_ROLLCUT:   rollcut_update(b, dt); break;
    case FK_PLUNGE:    plunge_update(b, dt);  break;

    case FK_CLEAVE:    cleave_update(b, dt);   break;
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
    blade_pose(dt);
    t3d_skeleton_update(&s.skel);

    float p[3] = { b->pos[0], b->pos[1], b->pos[2] };
    const float sc[3] = { FK_SCALE, FK_SCALE, FK_SCALE };
    const float rot[3] = { 0.0f, b->rot[1] + FK_YAW_OFFSET, 0.0f };
    t3d_mat4fp_from_srt_euler(s.mat, sc, rot, p);
    {   // The body capsule follows it down: feet to head while it lies staggered, so it's hit where it lies
        T3DVec3 top = {{ 0.0f, 20.0f, 0.0f }};
        if (s.state == FK_STAGGER && s.headBone >= 0) {
            float h[3], w[3];
            mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[s.headBone], (float[3]){ 0.0f, 0.0f, 0.0f }, h);
            mat4fp_mul_point_f32_row3_colbasis(s.mat, h, w);
            top = (T3DVec3){{ w[0] - b->pos[0], fmaxf(6.0f, w[1] - b->pos[1]), w[2] - b->pos[2] }};
        }
        b->capsuleCollider.localCapB = top;
    }
#ifdef HLE_FK_TRACE     // test aid: the blade tip through each swing, to find its strike
    {
        static float last[3]; float base[3], tip[3];
        if (s.state != FK_CHASE && s.state != FK_RECOVER && sword_segment(base, tip) && dt > 0.0f) {
            float v = sqrtf((tip[0]-last[0])*(tip[0]-last[0]) + (tip[1]-last[1])*(tip[1]-last[1]) + (tip[2]-last[2])*(tip[2]-last[2])) / dt;
            float fx = -fm_sinf(b->rot[1]), fz = fm_cosf(b->rot[1]), tx = tip[0] - b->pos[0], tz = tip[2] - b->pos[2];
            float px = character.pos[0] - b->pos[0], pz = character.pos[2] - b->pos[2];
            debugf("[fkt] st %d step %d clip %d t %.2f tipY %.0f v %.0f wind %.2f | tip fwd %.0f side %.0f, baseY %.0f | player fwd %.0f side %.0f\n", s.state, s.step, s.anim, (double)clip_time(), (double)(tip[1] - s.groundY), (double)v, (double)s.windK,
                   (double)(tx * fx + tz * fz), (double)(tx * fz - tz * fx), (double)(base[1] - s.groundY), (double)(px * fx + pz * fz), (double)(px * fz - pz * fx));
            float bx = base[0] - b->pos[0], bz = base[2] - b->pos[2], dl = sqrtf((tip[0]-base[0])*(tip[0]-base[0]) + (tip[1]-base[1])*(tip[1]-base[1]) + (tip[2]-base[2])*(tip[2]-base[2])) + 1e-3f;
            const float *mx = s.skel.bones[s.swordBone].matrix.m[0];
            debugf("[fkt2] blade fwd %.2f side %.2f up %.2f, base fwd %.0f side %.0f | model %.2f %.2f %.2f\n",
                   (double)(((tip[0]-base[0]) * fx + (tip[2]-base[2]) * fz) / dl), (double)(((tip[0]-base[0]) * fz - (tip[2]-base[2]) * fx) / dl), (double)((tip[1]-base[1]) / dl),
                   (double)(bx * fx + bz * fz), (double)(bx * fz - bz * fx), (double)-mx[0], (double)-mx[1], (double)-mx[2]);
        }
        if (sword_segment(base, tip)) memcpy(last, tip, sizeof(last));
    }
#endif
}

#ifdef HLE_LOG_HURTBOX
bool fallen_knight_bone(const char *name, float out[3])     // test aid: a bone's world position
{
    int i = t3d_skeleton_find_bone(&s.skel, name);
    if (i < 0 || !s.mat) return false;
    float h[3];
    mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[i], (float[3]){ 0.0f, 0.0f, 0.0f }, h);
    mat4fp_mul_point_f32_row3_colbasis(s.mat, h, out);
    return true;
}
#endif
#ifdef HLE_LOG_HITVOL
bool fallen_knight_hle_pose(const T3DSkeleton **sk, const T3DMat4FP **m)    // test aid: the pose, for its drawn extent
{
    *sk = &s.skel; *m = s.mat;
    return s.ready && s.mat;
}
#endif
void fallen_knight_draw(Boss* b)
{
    if (!s.ready || !b->visible) return;
    // Dark crimson armour, brighter while it flashes from a hit. Its moves are told apart by its
    // poses alone (no glows)
    bool flash = b->damageFlashTimer > 0.0f;
    t3d_light_set_ambient(flash ? (uint8_t[4]){ 255, 120, 120, 255 } : (uint8_t[4]){ 120, 45, 70, 255 });
    t3d_matrix_set(gpu_mat(s.mat), true);
    T3DMat4FP *bones = gpu_skeleton_begin(&s.skel);
    t3d_model_draw_skinned(characterModel, &s.skel);
    gpu_skeleton_end(&s.skel, bones);
    t3d_light_set_ambient((uint8_t[4]){ 255, 255, 255, 255 });
}
