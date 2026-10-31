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
#include "game_lighting.h"
#ifdef HLE_FW_CLOSECAM
float fw_closecam_head[3], fw_closecam_yaw;
#endif
#ifdef HLE_LOG_FWCLIP
#include "fw_clip_points.h"
#endif
#include "globals.h"
#include "fx/skill_fx.h"
#include "fx/screen_shake.h"
#include "../boss/environmental_mechanics/boss_ground_crush.h"
#include "controllers/audio_controller.h"
#include "controllers/camera_controller.h"
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
#define KNEEL_RISE_S      0.8f                     // StaggerKneel's last segment: up into the stance
#define FK_MELEE_RANGE    24.0f
#define FK_MELEE_DAMAGE   12.0f
#define FK_FELT(x)        ((x) / 1.125f)           // the damage the player takes -> its base (boss_damage_scale: 1.125)
#define FK_INTRO_TIME     2.4f
#define FK_DROP_HEIGHT    90.0f
#define FK_WIND           0.65f     // a basic swing's windup: as the Guardian's (0.8 s), a little quicker

enum {
    FKA_IDLE, FKA_WALK, FKA_RUN, FKA_ROLL, FKA_ATTACK1, FKA_ATTACK2, FKA_ATTACK3,
    FKA_CHARGED, FKA_CLEAVE, FKA_HALFMOON, FKA_DEATH, FKA_BACKFLIP, FKA_KNEEL, FKA_BACKSTEP, FKA_HOLYRAISE, FKA_PLUNGELEAP, FKA_HERODROP, FKA_LUNGE,
    FKA_STRAFE_L, FKA_STRAFE_R, FKA_COUNT
};
static const char *ANIM_NAMES[FKA_COUNT] = {
    "Idle", "Walk1", "Run", "Roll", "Attack1", "Attack2", "Attack3",
    "AttackCharged", "SkillCleave", "SkillHalfmoon", "Death", "SkillBackflip", "StaggerKneel", "WalkBackwards", "HolyRaise", "PlungeLeap", "HeroDrop", "Lunge",
    "StrafeWalkLeft", "StrafeWalkRight",
};
static const bool ANIM_LOOP[FKA_COUNT] = { true, true, true, [FKA_BACKSTEP] = true, [FKA_STRAFE_L] = true, [FKA_STRAFE_R] = true };

typedef enum {
    FK_INTRO, FK_CHASE, FK_COMBO, FK_ROLL, FK_CLEAVE, FK_HALFMOON, FK_MOONBLADE, FK_RECOVER, FK_STAGGER, FK_DEAD,
    FK_SPLITTER, FK_UPDOWN, FK_TRISLAM, FK_HOLYSLAM, FK_ROLLCUT, FK_PLUNGE, FK_LUNGE,
} FkState;

// The combo's third blow: the plain overhead, the same held at the top of its rise (a roll
// on its first motion is tracked and caught), or a lunging thrust
enum { FIN_SLASH, FIN_DELAY, FIN_THRUST };

// The fire lunge: the full one from far off (any distance), a quicker short one, or a leap back
// away from the player and then the full one
enum { LUNGE_FULL, LUNGE_SHORT, LUNGE_BACK };
#define FIRE_MAX   24       // points along one lunge's fire trail, at most (spaced to fit)
#define FIRE_LIFE  1.4f     // each stretch burns this long, then smoke: out in the order it was lit

// The strike of each swing clip (clip seconds), measured from the blade's speed: the
// windups before it can't hurt. Attack1-3 (Attack2 rises from low in front: the up-down's
// first), then the charged clip's overhead slam (played from DN_FROM: the slam, no more)
static const float WIN[3][2] = { { 0.55f, 0.85f }, { 0.42f, 0.75f }, { 0.9f, 1.2f } };
static const float W_DN[2] = { 1.88f, 2.08f };
#define DN_FROM 1.72f     // the sword already raised: held there, then down

static struct {
    bool ready;
    T3DSkeleton skel, skelBlend;
    float blendSet;         // the fade play() chose, to fit to the pose on its first frame (0: done, or set by hand)
    T3DAnim anims[FKA_COUNT];
    int anim;
    float blendT, blendDur; // crossfade timer (0 = done), and its length
    T3DMat4FP *mat;
    T3DModel *body;         // its body (tools/fw_knight_model.py): the knight's own mesh reshaped, on the knight's skeleton
    int swordBone;          // "Hand-Right": the blade runs along its -X
    int headBone;           // "Head": the body capsule's top while it kneels staggered

    FkState state;
    float t;                // time in state
    int step;               // events fired in the state / combo index
    bool hitDone;           // the current swing already connected
    bool striking;          // inside a swing's damage window this frame
    float groundY, velY;
    float moveX, moveZ;     // roll / jump drift
    float cdCleave, cdHalfmoon, cdMoonblade, cdRoll, cdSplitter, cdUpdown, cdTri, cdHoly, cdPlunge, cdLunge, cdBackLunge;
    int finisher;           // the combo's third blow (FIN_*)
    float holdT;            // time held in a finisher's pause
    bool counter;           // the roll comes up into a rising cut
    float tri[3][2];        // the triangle slam's corners on the floor
    float windK;            // 0..1 through the current swing's windup, 0 once it strikes
    bool enraged;           // the war cry at half health
    float bladeTgt, bladeW; // the held blade pose (blade_pose): wanted and current weight
    float bladeDir[3];      // its direction: forward, side, up (the boss's frame)
    float bladeCur[3], bladeTurn, bladeVel;   // the direction held now, turning toward bladeDir at up to bladeTurn rad/s (bladeVel: its speed now)
    T3DQuat handBase, handPosed;
    float recT;             // this recovery's length
    float stareT;           // from here on in it, a longer pause: it stares the player down (recT if none)
    float pickJit;          // this chase's offset to when it picks its next move (+-0.2 s: no metronome)
    int queued;             // a blow it has picked and is closing in to land (-1: none)
    float queuedAt;         // since when (kept off 2 s, it takes a move that reaches instead)
    float lastUsed[11];     // when each move last came (choose: the longer ago, the likelier)
    int lungeKind;          // LUNGE_*
    float lungeO[2], lungeLen, lungeGone, fireGap;     // the skim: from, how far, how far so far, between its flames
    int fireLit;            // fire trail points lit along this skim
    float fireHitT;         // until the trail can hurt again
    bool heavy;             // spent (after Holy Slam): hunched, sword down; else just breathing
    int bladeArc;           // its sword back up after that: 0 point-down, 1 out to its right, 2 onto the shoulder, 3 the clip's again
    float tiredW, breathPh; // the breathing pose's current weight, and its phase
    T3DBone spineKeep, headKeep, waistKeep, armKeep;    // the clip's own pose of the bones turned this frame (bone_turn)
    float spineRise;        // the breath's lift of the chest this frame (tired_pose)
    int spineBone;
    int armL[3];            // "UpperArm-Left", "LowerArm-Left", "Hand-Left": brought to the hilt (grip_pose)
    float gripW;            // how far its left hand is on the hilt: both hands on the sword in its big moves
    float gripFit;          // how well the hilt is placed for it (reach, ahead of the body), eased
    T3DBone gripKeep[3];    // the clip's own pose of those, put back after drawing
    // The sword's own bone (IK_Hand.R, unused by the mesh else: tools/fw_knight_model.py skins the
    // sword to it), on the hand each frame; in the lunge's thrust the sword turned in the grip
    int swordCarrier;
    T3DQuat carrierK; float carrierKP[3];   // its rest pose in the hand's (the sword's verts are baked in its rest space)
    T3DQuat seatQ;          // the sword's turn in the hand (about GRIP_P), identity but in the thrust
    float seatW;            // and how far it has moved over, GRIP_P to GRIP_T
    T3DBone handKeep;       // the clip's own pose of the hand, put back after drawing
    float beam2T, beam2O[3], beam2V[3], beam2Life;  // a Moon Blade cross's second beam, after the first (< 0: none)
    // Life (life_*): a layer of turns on the waist, spine, head and free arm over the clips
    int waistBone;
    float kv[16], kc[16], ko[16];   // the layer's channels (L_*) this frame, a cut's carry, as applied
    int kState, kStep;
    float chainW;           // the waist and spine's share of the layer: none in a swing (its blade is read from the pose)
    float leanW, runW;      // the chase's lean into its turns, and the run's forward lean: eased on and off (they cut out as a move began)
    float lookYaw, lookPitch, lookW;    // the head on the player, a beat late
    float flinch[3], flinchV[3];        // a blow taken: the head's pitch and yaw, the spine's pitch (sprung)
    int hitSeen;
    float fidgetT; int fidgetK;     // a fidget under way (time left, which)
    float yawPrev, yawRate;
    float cryT;             // the war cry's pose (time since; < 0 none)
    float dtLast;           // the last update's step (the cape steps with it)
    // Footwork between attacks: it circles the player, backs off or closes in, and turns on its feet
    float roamT;            // time left on this leg of it
    float roamF, roamS;     // its direction: toward the player, and to its side (-1..1 each)
    float stepT, stepSg;    // a turn on the spot: time left in it, and which way (its feet move)
} s;

// ---- Helpers ----

#ifdef HLE_TEST_DUMMY
int hle_fk_commit, hle_fk_swing;    // test aid: swings begun, and aims committed (the sidestep test reacts to them)
static bool committed;
static bool hle_counts(void)    // HLE_SIDESTEP_NEW: only the new moves (the combo's third, the roll's cut, the plunge)
{
#ifdef HLE_SIDESTEP_NEW
    return (s.state == FK_COMBO && s.step == 3) || s.state == FK_ROLLCUT || s.state == FK_PLUNGE || s.state == FK_LUNGE;
#else
    return true;
#endif
}
static void hle_commit(void) { if (!committed) { committed = true; if (hle_counts()) hle_fk_commit++; } }
#else
static inline void hle_commit(void) {}
#endif

#ifdef HLE_LOG_POP     // test aid: jolts in its motion, each laid to the clip or move change before it
static struct { int fa, ta, fs, ts; float t, peak, yawPk, body, at, lateB, lateS, turn; int pt, n; bool on; } pop;
static void pop_mark(int fa, int ta, int fs, int ts)
{
    if (pop.on && pop.t <= 0.0f) { if (ta >= 0) { pop.ta = ta; if (pop.fa < 0) pop.fa = fa; } if (ts >= 0) { pop.ts = ts; if (pop.fs < 0) pop.fs = fs; } return; }     // the same frame: one change
    if (pop.on) debugf("[pop] st %d>%d clip %d>%d peak %.0f pt %d yaw %.0f body %.0f at %.0f ms late %.0f %.0f turn %.2f fade %.2f\n", pop.fs, pop.ts, pop.fa, pop.ta, (double)pop.peak, pop.pt, (double)pop.yawPk, (double)pop.body, (double)(pop.at * 1000.0f), (double)pop.lateB, (double)pop.lateS, (double)pop.turn, (double)s.blendDur);
    pop.fa = ta >= 0 ? fa : -1; pop.ta = ta; pop.fs = ts >= 0 ? fs : -1; pop.ts = ts;
    pop.t = 0.0f; pop.peak = 0.0f; pop.yawPk = 0.0f; pop.body = 0.0f; pop.at = 0.0f; pop.lateB = pop.lateS = pop.turn = 0.0f; pop.pt = -1; pop.on = true;
}
#endif

// The pose a fade starts from carries on as it was moving, slowing to a stop (held still, a swing
// or a step stopped dead for a frame): each bone's turn and travel over the last frame, from the
// pose as the clips and fades made it (before the layers on top). No second clip played under it
#define FK_MAXB 48
#ifndef FK_FADE_TAU
#define FK_FADE_TAU 0.08f   // how quickly the carried motion dies away (s)
#endif
static T3DQuat poseQ[2][FK_MAXB]; static T3DVec3 poseP[2][FK_MAXB]; static float poseDt; static int poseN;
static float carryW[FK_MAXB][3], carryV[FK_MAXB][3];
static int8_t fadeSgn[FK_MAXB];     // each bone's way round, fixed as the fade starts
static T3DQuat fadeB[FK_MAXB];      // the clip's turn last frame, its sign kept continuous (a clip may give q or -q)

static int bone_count(void) { int n = s.skel.skeletonRef->boneCount; return n < FK_MAXB ? n : FK_MAXB; }

static void pose_record(float dt)
{
    int n = bone_count();
    memcpy(poseQ[1], poseQ[0], sizeof(poseQ[0])); memcpy(poseP[1], poseP[0], sizeof(poseP[0]));
    for (int i = 0; i < n; i++) { poseQ[0][i] = s.skel.bones[i].rotation; poseP[0][i] = s.skel.bones[i].position; }
    poseDt = dt; if (poseN < 2) poseN++;
}

static void fade_carry_begin(void)
{
    memset(carryW, 0, sizeof(carryW)); memset(carryV, 0, sizeof(carryV)); memset(fadeSgn, 0, sizeof(fadeSgn));
#ifndef HLE_FADE_FROZEN
    if (poseN < 2 || poseDt <= 1e-4f) return;
    for (int i = 0, n = bone_count(); i < n; i++) {
        const T3DQuat *a = &poseQ[0][i], *b = &poseQ[1][i];
        // the turn from last frame's to this: a * conj(b), the short way round
        float w = a->v[3] * b->v[3] + a->v[0] * b->v[0] + a->v[1] * b->v[1] + a->v[2] * b->v[2];
        float x = -a->v[3] * b->v[0] + a->v[0] * b->v[3] - a->v[1] * b->v[2] + a->v[2] * b->v[1];
        float y = -a->v[3] * b->v[1] + a->v[1] * b->v[3] - a->v[2] * b->v[0] + a->v[0] * b->v[2];
        float z = -a->v[3] * b->v[2] + a->v[2] * b->v[3] - a->v[0] * b->v[1] + a->v[1] * b->v[0];
        if (w < 0.0f) { w = -w; x = -x; y = -y; z = -z; }
        float sn = sqrtf(x * x + y * y + z * z);
        if (sn > 1e-6f) {
            float k = 2.0f * atan2f(sn, w) / sn / poseDt;
            carryW[i][0] = x * k; carryW[i][1] = y * k; carryW[i][2] = z * k;
        }
        for (int c = 0; c < 3; c++) carryV[i][c] = (poseP[0][i].v[c] - poseP[1][i].v[c]) / poseDt;
    }
#endif
}

static void fade_carry(float dt)
{
    float decay = expf(-dt / FK_FADE_TAU);
    for (int i = 0, n = bone_count(); i < n; i++) {
        float *w = carryW[i], l = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
        T3DBone *B = &s.skelBlend.bones[i];
        if (l * dt > 1e-5f) {
            T3DQuat r, q;
            t3d_quat_from_rotation(&r, (float[3]){ w[0] / l, w[1] / l, w[2] / l }, l * dt);
            t3d_quat_mul(&q, &r, &B->rotation); t3d_quat_normalize(&q);
            B->rotation = q;
        }
        for (int c = 0; c < 3; c++) { B->position.v[c] += carryV[i][c] * dt; carryV[i][c] *= decay; w[c] *= decay; }
    }
}

/** The pose faded from (skelBlend) into the clip's (skel), by k. As t3d_skeleton_blend, but each bone
 *  goes the way round it took on the fade's first frame: the shorter way, chosen afresh each frame,
 *  flipped side mid-fade where the clip turned a bone on through half a turn from where it came,
 *  and the bone (an arm) jumped. The clip's turn is followed frame to frame (q and -q are the same
 *  turn, and a clip may give either), the way round fixed against that */
static void fade_blend(float k)
{
    int n = s.skel.skeletonRef->boneCount;
    for (int i = 0; i < n; i++) {
        T3DBone *B = &s.skel.bones[i];
        const T3DBone *A = &s.skelBlend.bones[i];
        const float *qa = A->rotation.v;
        float qb[4] = { B->rotation.v[0], B->rotation.v[1], B->rotation.v[2], B->rotation.v[3] };
        float wa = 1.0f - k;
        if (i < FK_MAXB) {
            if (fadeSgn[i]) {
                const float *pb = fadeB[i].v;
                if (pb[0] * qb[0] + pb[1] * qb[1] + pb[2] * qb[2] + pb[3] * qb[3] < 0.0f) for (int c = 0; c < 4; c++) qb[c] = -qb[c];
            } else fadeSgn[i] = (qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3]) < 0.0f ? -1 : 1;
            memcpy(fadeB[i].v, qb, sizeof(qb));
            wa *= fadeSgn[i];
        } else if (qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3] < 0.0f) wa = -wa;
        for (int c = 0; c < 4; c++) B->rotation.v[c] = wa * qa[c] + k * qb[c];
        t3d_quat_normalize(&B->rotation);
        t3d_vec3_lerp(&B->position, &A->position, &B->position, k);
        t3d_vec3_lerp(&B->scale, &A->scale, &B->scale, k);
        B->hasChanged = true;
    }
}

static void play(int a, bool restart)
{
    if (a == s.anim && !restart) return;
#ifdef HLE_TEST_DUMMY
    committed = false;
    if (a >= FKA_ATTACK1 && a <= FKA_HALFMOON && hle_counts()) hle_fk_swing++;
#endif
#ifdef HLE_LOG_POP
    pop_mark(s.anim, a, s.state, -1);
#endif
    t3d_skeleton_blend(&s.skelBlend, &s.skel, &s.skel, 0.0f);    // snapshot the current pose
    fade_carry_begin();
    if (s.anim >= 0) t3d_anim_set_playing(&s.anims[s.anim], false);
    // swings cut in fast (the blade's hits are read from the pose); going down, dying and
    // moving about ease over longer
#ifndef FK_HALFMOON_FADE
#define FK_HALFMOON_FADE 0.4f   // down into its crouch (the slash is 1.6 s on: time to sink into it)
#endif
    s.blendDur = a == FKA_KNEEL ? 0.6f : a == FKA_HALFMOON ? FK_HALFMOON_FADE : a == FKA_LUNGE ? 0.3f : a == FKA_HOLYRAISE ? 0.4f : a == FKA_DEATH ? 0.35f : (a == FKA_IDLE || a == FKA_WALK || a == FKA_RUN || a == FKA_STRAFE_L || a == FKA_STRAFE_R || a == FKA_BACKSTEP || s.anim == FKA_ROLL) ? 0.3f : 0.15f;
    s.blendT = s.blendDur;
    s.blendSet = s.blendDur;
    // walk and run into each other in step: the new one at the same point of its stride
    float phase = -1.0f;
    if ((a == FKA_WALK || a == FKA_RUN) && (s.anim == FKA_WALK || s.anim == FKA_RUN) && a != s.anim)
        phase = fmodf(t3d_anim_get_time(&s.anims[s.anim]) / t3d_anim_get_length(&s.anims[s.anim]), 1.0f);
    s.anim = a;
    t3d_anim_attach(&s.anims[a], &s.skel);
    t3d_anim_set_time(&s.anims[a], phase >= 0.0f ? phase * t3d_anim_get_length(&s.anims[a]) : 0.0f);
    t3d_anim_set_speed(&s.anims[a], 1.0f);
    t3d_anim_set_playing(&s.anims[a], true);
}

static float to_player(const Boss *b, float *dx, float *dz)
{
    *dx = character.pos[0] - b->pos[0];
    *dz = character.pos[2] - b->pos[2];
    return sqrtf(*dx * *dx + *dz * *dz);
}

/** Turns it toward the player at `rate`; returns the turn it still owed this frame (radians, signed) */
static float face_player(Boss *b, float dt, float rate)
{
    float dx, dz;
    to_player(b, &dx, &dz);
    float target = atan2f(-dx, dz);
    float d = target - b->rot[1];
    while (d >  T3D_PI) d -= 2.0f * T3D_PI;
    while (d < -T3D_PI) d += 2.0f * T3D_PI;
    float m = rate * dt;
    b->rot[1] += (d > m) ? m : (d < -m) ? -m : d;
    return d;
}

static void move(Boss *b, float vx, float vz, float dt)
{
    float nx = b->pos[0] + vx * dt, nz = b->pos[2] + vz * dt;
    if (scene_bounds_check_room(nx, b->pos[1], nz)) { b->pos[0] = nx; b->pos[2] = nz; }
}

static inline float clamp01f(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

// ---- Life: keyframed turns over the clips ----
// The clips are the knight's own; what they lack is the boss's body language: a lean into its
// turns, a coil before a skill and a heave after it, the head on you, a flinch at a blow, fidgets
// in its breathers. Channels (radians; the waist's rise in glTF units), keyed like Shadowborne's.
// The waist and spine carry the sword arm, so their share (chainW) is nothing in a swing: its
// blade, read from the pose, lands exactly as before; the head and the free arm move any time
enum { L_WR, L_WP, L_SP, L_SY, L_SR, L_HP, L_HY, L_HR, L_AP, L_AR, L_N };
#define L_CHAIN(ch) ((ch) <= L_SR)

typedef struct { uint8_t ch, n; const float *k; } Track;
typedef struct { const Track *tr; uint8_t n; } Clip;
#define TR(ch, ...) { ch, sizeof((const float[]){ __VA_ARGS__ }) / sizeof(float) / 2, (const float[]){ __VA_ARGS__ } }
#define CLIP(name, ...) static const Track name##_tr[] = { __VA_ARGS__ }; static const Clip name = { name##_tr, sizeof(name##_tr) / sizeof(Track) }

static float key_slope(const float *k, int n, int i)
{
    if (i <= 0 || i >= n - 1) return 0.0f;
    float a = (k[2 * i + 1] - k[2 * i - 1]) / fmaxf(k[2 * i] - k[2 * i - 2], 1e-4f);
    float b = (k[2 * i + 3] - k[2 * i + 1]) / fmaxf(k[2 * i + 2] - k[2 * i], 1e-4f);
    return a * b <= 0.0f ? 0.0f : 0.5f * (a + b);      // a key at a peak holds there (clamped tangents)
}
static float key_eval(const Track *tr, float t)
{
    const float *k = tr->k;
    int n = tr->n;
    if (t <= k[0]) return k[1];
    if (t >= k[2 * n - 2]) return k[2 * n - 1];
    int i = 0;
    while (t >= k[2 * i + 2]) i++;
    float h = k[2 * i + 2] - k[2 * i], u = (t - k[2 * i]) / h, u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * k[2 * i + 1] + (u3 - 2 * u2 + u) * h * key_slope(k, n, i)
         + (-2 * u3 + 3 * u2) * k[2 * i + 3] + (u3 - u2) * h * key_slope(k, n, i + 1);
}
// Clip c at t (s), its keys stretched by scale, mirrored (side -1: the yaws and rolls), weighted by w
static void key_play(const Clip *c, float t, float scale, float side, float w)
{
    t /= fmaxf(scale, 1e-3f);
    for (int i = 0; i < c->n; i++) {
        int ch = c->tr[i].ch;
        float v = key_eval(&c->tr[i], t) * w;
        if (ch == L_SY || ch == L_SR || ch == L_HY || ch == L_HR) v *= side;
        s.kv[ch] += v;
    }
}
static void key_loop(const Clip *c, float u, float w) { key_play(c, u - floorf(u), 1.0f, 1.0f, w); }

// Turns a bone after the clip, for this frame's matrices only: pitch round its parent's side axis
// (+ leans it forward), yaw round its up, roll round its forward, and raised `rise` (its parent's
// units). `keep` saves the clip's own pose, put back by bone_unpose right after t3d_skeleton_update:
// left in the bone, it leaked into the next frame and the crossfades (the knight's clips don't
// write Spine1's forward offset: it crept 20 units)
static void bone_turn(int bone, float pitch, float yaw, float roll, float rise, T3DBone *keep)
{
    keep->parentIdx = 0xFFFF;       // nothing to put back
    if (bone < 0) return;
    T3DBone *h = &s.skel.bones[bone];
    if (h->parentIdx == 0xFFFF || (fabsf(pitch) + fabsf(yaw) + fabsf(roll) < 1e-4f && fabsf(rise) < 1e-4f)) return;
    *keep = *h;
    const T3DMat4x3 *P = &s.skel.bones[h->parentIdx].matrix;     // last frame's, as blade_pose
    const float ang[3] = { roll, yaw, pitch };      // round the parent's forward (x), up (y), side (z)
    float up[3], lu = 0.0f;
    for (int i = 0; i < 3; i++) { up[i] = P->m[i][1]; lu += up[i] * up[i]; }
    if (lu < 1e-8f) return;
    for (int a = 2; a >= 0; a--) {
        if (fabsf(ang[a]) < 1e-4f) continue;
        float ax[3], l = 0.0f;
        for (int i = 0; i < 3; i++) { ax[i] = P->m[i][a]; l += ax[i] * ax[i]; }
        if (l < 1e-8f) continue;
        l = 1.0f / sqrtf(l);
        for (int i = 0; i < 3; i++) ax[i] *= l;
        T3DQuat r, q;
        t3d_quat_from_rotation(&r, ax, ang[a]);
        t3d_quat_mul(&q, &r, &h->rotation);
        t3d_quat_normalize(&q);
        h->rotation = q;
    }
    lu = 1.0f / sqrtf(lu);
    for (int i = 0; i < 3; i++) h->position.v[i] += up[i] * lu * rise;
    h->hasChanged = true;
}


static void enter(FkState st)
{
#ifdef HLE_LOG_POP
    pop_mark(s.anim, -1, s.state, st);
#endif
#ifdef HLE_LOG_MOVES     // test aid: each move as it starts (the hits after it are its: [hit])
    debugf("[move] %d\n", st);
#endif
    s.state = st;
    s.t = 0.0f;
    if (st == FK_CHASE) s.pickJit = ((int)(rand_custom_u32() % 41) - 20) * 0.01f;
    s.step = 0;
    s.hitDone = false;
    s.windK = 0.0f;
    s.holdT = 0.0f;
#ifdef HLE_FK_LOG   // test aid: each move as it starts
    debugf("[fk] state %d\n", st);
#endif
}

// At the Guardian's level and with his falloff (boss_play_attack_sfx: the distance to the player);
// they played at a fixed 0.85 with none, as his at melee range but up to 3x his further off
static void sfx(int id, float vol)
{
    Boss *b = boss_get_instance();
    float dx = b ? character.pos[0] - b->pos[0] : 0.0f, dz = b ? character.pos[2] - b->pos[2] : 0.0f;
    audio_play_scene_sfx_dist(id, vol * BONUS_SFX_VOL, sqrtf(dx * dx + dz * dz));
}
// A ranged move's impact, heard from where it lands (from the boss, one by the player at range
// was faint or silent)
static void sfx_at(int id, float vol, float x, float z)
{
    float dx = character.pos[0] - x, dz = character.pos[2] - z;
    audio_play_scene_sfx_dist(id, vol * BONUS_SFX_VOL, sqrtf(dx * dx + dz * dz));
}
static void swing_sfx(void) { sfx(SCENE1_SFX_SWING_BOSS, 1.0f); }
// The intro's sounds, heard from the cutscene camera (the player stands 160 off, near silent)
static void sfx_cam(int id, float vol)
{
    Boss *b = boss_get_instance();
    float dx = customCamPos.v[0] - b->pos[0], dz = customCamPos.v[2] - b->pos[2];     // (the cutscene camera as it moves)
    audio_play_scene_sfx_dist(id, vol * BONUS_SFX_VOL, sqrtf(dx * dx + dz * dz));
}

static void quat_rotate(const T3DQuat *q, const float v[3], float out[3]);
static void bone_world(int i, T3DQuat *q, float p[3]);
static void quat_conj(T3DQuat *q);

// The knight's grip, in the hand's space (model units): the handle crosses the palm, so with the
// palm in and the blade along the arm the hand folds 90 degrees, fingers down. In the lunge's
// thrust the hand stays straight and the sword turns in it instead, about the grip, along the
// forearm (measured from fw_knight.glb's rest mesh)
static const float GRIP_P[3] = { 43.6f, 43.7f, 6.9f };          // where the blade's axis passes through the fist
static const float BLADE_H[3] = { -0.9902f, 0.0433f, -0.1324f }; // the blade's axis
static const float FORE_H[3] = { 0.0773f, 0.9450f, 0.3178f };    // the forearm's line (elbow to wrist), the wrist straight
static const float PALM_H[3] = { 0.137f, 0.286f, -0.937f };      // the palm's side
static const float GRIP_T[3] = { 7.96f, 0.96f, 1.5f };            // the handle's place in the straight hand: on the forearm's line a little under it, the guard at the fist's front (74 along)

/** A point of the sword in the hand's space, as it sits in the hand now (turned by seatQ about the
 *  grip, which moves over to GRIP_T) */
static void sword_seat(const float v[3], float out[3])
{
    float d[3] = { v[0] - GRIP_P[0], v[1] - GRIP_P[1], v[2] - GRIP_P[2] }, r[3];
    quat_rotate(&s.seatQ, d, r);
    for (int k = 0; k < 3; k++) out[k] = r[k] + GRIP_P[k] + (GRIP_T[k] - GRIP_P[k]) * s.seatW;
}

/** The drawn blade in world space, from the last pose: guard to tip, as the knight model has it
 *  in the hand's space. (The player's 640 along the hand's -X follows its sword trail; drawn
 *  without one, that reached 6 past this tip and ran 5 to the side of it) */
#ifdef HLE_LOG_POP
/** Each frame: how hard its hands, sword tip, head, hips and feet change speed (model space, glTF
 *  units/s^2: the clips' own motion, not its travel), and its turn (rad/s^2). For 0.4 s after a change
 *  the peak goes to that change's [pop] line; otherwise to the clip's own ([popclip]: what it does
 *  anyway, the bar a change is held to) */
static void pop_sample(float dt, float yaw)
{
    static const char *NAMES[] = { "Hand-Right", "Hand-Left", "Head", "Waist", "Foot-Left", "Foot-Right" };
    enum { NP = 7 };
    static int bone[6] = { -2 }; static float p0[NP][3], v0[NP][3], yaw0, yv0; static int warm;
    static float clipPk[FKA_COUNT], clipSum[FKA_COUNT]; static int clipN[FKA_COUNT]; static float logT;
    if (bone[0] == -2) for (int i = 0; i < 6; i++) bone[i] = t3d_skeleton_find_bone(&s.skel, NAMES[i]);
    if (dt <= 1e-4f) return;
    float p[NP][3];
    for (int i = 0; i < 6; i++) {
        if (bone[i] < 0) return;
        mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[bone[i]], (float[3]){ 0, 0, 0 }, p[i]);
    }
    { float t[3]; sword_seat((float[3]){ -522.0f, 69.0f, -68.0f }, t); mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[s.swordBone], t, p[6]); }
    float pk = 0.0f, bodyPk = 0.0f; int pt = -1;
    float trA[NP] = { 0 }; (void)trA;
    float yv = remainderf(yaw - yaw0, 2.0f * T3D_PI) / dt, ya = fabsf(yv - yv0) / dt;
    for (int i = 0; i < NP; i++) {
        float v[3], a = 0.0f;
        for (int c = 0; c < 3; c++) { v[c] = (p[i][c] - p0[i][c]) / 64.0f / dt; float d = v[c] - v0[i][c]; a += d * d; p0[i][c] = p[i][c]; v0[i][c] = v[c]; }
        a = sqrtf(a) / dt;
        trA[i] = a;
        if (warm >= 2 && a > pk) { pk = a; pt = i; }
        if (warm >= 2 && i < 6 && a > bodyPk) bodyPk = a;
    }
    yaw0 = yaw; yv0 = yv;
    if (warm < 2) { warm++; return; }
#ifdef HLE_POP_TRACE     // test aid: frame by frame through changes into clip HLE_POP_TRACE (each point's accel, and the layers' weights)
    if (pop.on && pop.ta == HLE_POP_TRACE) {
        debugf("[poptr] %3.0f ms fade %.2f blade %.2f grip %.2f seat %.2f chain %.2f fit %.2f t %.2f | hR %.0f hL %.0f hd %.0f hip %.0f fL %.0f fR %.0f tip %.0f\n", (double)(pop.t * 1000.0f), (double)s.blendT, (double)s.bladeW, (double)s.gripW, (double)s.seatW, (double)s.chainW, (double)s.gripFit, (double)s.t,
               (double)trA[0], (double)trA[1], (double)trA[2], (double)trA[3], (double)trA[4], (double)trA[5], (double)trA[6]);
    }
#endif
    if (pop.on) {
        pop.t += dt;
        if (pk > pop.peak) { pop.peak = pk; pop.pt = pt; pop.at = pop.t; }
        if (bodyPk > pop.body) pop.body = bodyPk;
        if (pop.t > 0.05f) { if (bodyPk > pop.lateB) pop.lateB = bodyPk; if (pk > pop.lateS) pop.lateS = pk; }     // past a hard cut's own jump
        if (ya > pop.yawPk) pop.yawPk = ya;
        if (pop.t > 0.4f) { debugf("[pop] st %d>%d clip %d>%d peak %.0f pt %d yaw %.0f body %.0f at %.0f ms late %.0f %.0f turn %.2f fade %.2f\n", pop.fs, pop.ts, pop.fa, pop.ta, (double)pop.peak, pop.pt, (double)pop.yawPk, (double)pop.body, (double)(pop.at * 1000.0f), (double)pop.lateB, (double)pop.lateS, (double)pop.turn, (double)s.blendDur); pop.on = false; }
    } else if (s.anim >= 0) {
        if (pk > clipPk[s.anim]) clipPk[s.anim] = pk;
        clipSum[s.anim] += pk; clipN[s.anim]++;
    }
    if ((logT += dt) > 30.0f) {
        logT = 0.0f;
        for (int a = 0; a < FKA_COUNT; a++) if (clipN[a]) debugf("[popclip] %d %s max %.0f mean %.0f n %d\n", a, ANIM_NAMES[a], (double)clipPk[a], (double)(clipSum[a] / clipN[a]), clipN[a]);
    }
}
#endif

static bool sword_segment(float base[3], float tip[3])
{
    if (s.swordBone < 0) return false;
    const T3DMat4FP *B = &s.skel.boneMatricesFP[s.swordBone];
    float m0[3], m1[3], g[3], t[3];
    sword_seat((float[3]){ -29.0f, 46.0f, -3.0f }, g);
    sword_seat((float[3]){ -522.0f, 69.0f, -68.0f }, t);
    mat4fp_mul_point_f32_row3_colbasis(B, g, m0);
    mat4fp_mul_point_f32_row3_colbasis(B, t, m1);
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

#ifndef FK_QUICK_REC
#define FK_QUICK_REC 1.2f   // the breather after a quick move (the combo, up-down, splitter, a stagger)
#endif
#ifndef FK_PAUSE
#define FK_PAUSE 0.35f      // added to every breather
#endif
#ifndef FK_REACH_PICK
#define FK_REACH_PICK 0.5f  // in reach, it picks its next move this soon (0.7 s further off)
#endif

/** The breather after a move, as long as the move was big; `heavy`: spent, not just breathing */
static void recover(float t, bool heavy)
{
    // Now and then (20% each) a quick move runs straight into the next (don't get greedy), or a
    // breather runs on a second, it staring you down
    uint32_t r = rand_custom_u32() % 100;
#ifdef HLE_FK_PACE      // test aid: 1 always the chain, 2 always the stare
    r = HLE_FK_PACE == 1 ? 0 : 99;
#endif
    if (!heavy && t <= FK_QUICK_REC && r < 20) { enter(FK_CHASE); return; }
    enter(FK_RECOVER);
    t += FK_PAUSE;      // (every move now comes round: the breathers a little longer for them)
    s.recT = s.stareT = t;
    s.heavy = heavy; s.bladeArc = 0;
    if (!heavy && r >= 80) s.recT += 1.0f;
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

static void hold_blade_turn(float f, float sd, float u, float turn)
{
    float l = sqrtf(f * f + sd * sd + u * u);
    s.bladeTgt = 1.0f; s.bladeTurn = turn;
    s.bladeDir[0] = f / l; s.bladeDir[1] = sd / l; s.bladeDir[2] = u / l;
}

static void hold_blade(float f, float sd, float u)
{
    hold_blade_turn(f, sd, u, 1e9f);    // straight to it
}

static float vnorm(float v[3]) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-6f) for (int k = 0; k < 3; k++) v[k] /= l; return l; }

static void quat_rotate(const T3DQuat *q, const float v[3], float out[3])
{
    const float *x = q->v;      // v + 2w (q x v) + 2 q x (q x v)
    float t[3] = { 2.0f * (x[1] * v[2] - x[2] * v[1]), 2.0f * (x[2] * v[0] - x[0] * v[2]), 2.0f * (x[0] * v[1] - x[1] * v[0]) };
    out[0] = v[0] + x[3] * t[0] + (x[1] * t[2] - x[2] * t[1]);
    out[1] = v[1] + x[3] * t[1] + (x[2] * t[0] - x[0] * t[2]);
    out[2] = v[2] + x[3] * t[2] + (x[0] * t[1] - x[1] * t[0]);
}


static void bone_unpose(int bone, const T3DBone *keep)
{
    if (bone < 0 || keep->parentIdx == 0xFFFF) return;
    T3DBone *h = &s.skel.bones[bone];
    h->rotation = keep->rotation; h->position = keep->position;
    h->hasChanged = true;       // next frame's matrix from the clip's own pose
}

#ifndef FK_BREATH_RISE
#define FK_BREATH_RISE 0.12f    // the chest's rise with a breath, a share of the spine's height above the waist
#endif

/** Its breathers: the chest rising and falling, a nod; spent (after Holy Slam), hunched over and heaving
 *  faster, head hung. Eased in and out over ~0.3 s, so it goes back to fighting smoothly */
static void tired_pose(float dt)
{
    float want = (s.state == FK_RECOVER && s.t < s.recT - 0.3f) ? 1.0f : 0.0f;
    float rate = want > s.tiredW ? 3.0f : 3.5f;
    s.tiredW += fmaxf(-rate * dt, fminf(rate * dt, want - s.tiredW));
    s.spineRise = 0.0f;
    if (s.tiredW <= 0.0f) return;
    s.breathPh += dt * 2.0f * T3D_PI * (s.heavy ? 1.3f : 0.55f);      // spent ~80 breaths a minute, else ~33
    float w = s.tiredW * s.tiredW * (3.0f - 2.0f * s.tiredW), br = 0.5f + 0.5f * sinf(s.breathPh), hv = s.heavy ? 1.0f : 0.0f;
    float h = s.spineBone >= 0 ? t3d_vec3_len(&s.skel.bones[s.spineBone].position) : 0.0f;     // the spine's height above the waist
    s.kv[L_SP] += w * hv * (0.42f + 0.1f * br); s.spineRise = w * h * FK_BREATH_RISE * br;
    s.kv[L_HP] += w * (hv * 0.2f - 0.05f * br);
}

/** Turns the sword hand (after the clip and its crossfade) so the blade points along bladeDir, by bladeW */
static void blade_pose(float dt)
{
    // in over 0.25 s, out over 0.12 (in a swing); between moves back to the shoulder over 0.4 s
    bool calm = s.state == FK_RECOVER || s.state == FK_STAGGER || s.state == FK_CHASE;
    float rate = s.bladeTgt > s.bladeW ? 4.0f : calm ? 2.5f : 8.0f;
    bool starting = s.bladeW <= 0.0f;
    s.bladeW += fmaxf(-rate * dt, fminf(rate * dt, s.bladeTgt - s.bladeW));
    if (s.swordBone < 0) return;
    T3DBone *h = &s.skel.bones[s.swordBone];
    if (h->parentIdx == 0xFFFF) return;
    // a clip that has stopped no longer writes the hand: start from its own pose, not ours
    if (memcmp(&h->rotation, &s.handPosed, sizeof(T3DQuat)) == 0) { h->rotation = s.handBase; h->hasChanged = true; }
    if (s.bladeW <= 0.0f) return;
    s.handBase = h->rotation;
    // the arm as it is this frame (its matrix is last frame's: in a fade the arm moves fast, and
    // the blade, aimed off a frame-old arm, shook)
    T3DQuat pq, pqi; float pp[3];
    bone_world(h->parentIdx, &pq, pp);
    pqi = pq; quat_conj(&pqi);
    float clipDir[3];   // where the clip has the blade
    {
        float cp[3], cm[3];
        quat_rotate(&h->rotation, (float[3]){ -1.0f, 0.0f, 0.0f }, cp);
        quat_rotate(&pq, cp, cm);
#ifdef HLE_LOG_BLADEP   // test aid: this frame's arm against last frame's matrix (dot of the two blade directions; ~1 when still)
        { const T3DMat4x3 *P = &s.skel.bones[h->parentIdx].matrix; float om[3] = { 0 };
          for (int j = 0; j < 3; j++) for (int i = 0; i < 3; i++) om[j] += cp[i] * P->m[i][j];
          vnorm(om); float nm[3] = { cm[0], cm[1], cm[2] }; vnorm(nm);
          static int k; if (++k % 15 == 0) debugf("[bladep] dot %.4f fade %.2f\n", (double)(om[0] * nm[0] + om[1] * nm[1] + om[2] * nm[2]), (double)s.blendT); }
#endif
        clipDir[0] = -cm[0]; clipDir[1] = cm[2]; clipDir[2] = cm[1]; vnorm(clipDir);
    }
    if (starting) {     // a slow turn starts from where the clip has the blade
        memcpy(s.bladeCur, clipDir, sizeof(clipDir));
        s.bladeVel = 0.0f;
        if (s.bladeTurn < 1e8f) s.bladeW = 1.0f;    // it takes over where the blade is: no fade, nothing to jump
    }
    if (s.bladeArc == 2 && s.state == FK_RECOVER) {      // back onto the clip's blade, then let go of it there: nothing to jump
        memcpy(s.bladeDir, clipDir, sizeof(clipDir));
        if (s.bladeCur[0] * clipDir[0] + s.bladeCur[1] * clipDir[1] + s.bladeCur[2] * clipDir[2] > 0.995f) { s.bladeArc = 3; s.bladeW = 0.0f; return; }
    }
    {   // turn toward the asked direction, at most bladeTurn rad/s, along the great circle; a slow turn
        // eases up to speed and down onto it (at a constant speed it started and stopped dead, and the
        // other hand on the hilt jerked with it)
        float *c = s.bladeCur, *t = s.bladeDir, dot = fmaxf(-1.0f, fminf(1.0f, c[0] * t[0] + c[1] * t[1] + c[2] * t[2]));
        float ang = acosf(dot), step = s.bladeTurn * dt;
        if (s.bladeTurn < 1e8f) {
            float acc = s.bladeTurn * 8.0f;     // full speed in 1/8 s
            s.bladeVel = fminf(fminf(s.bladeTurn, sqrtf(2.0f * acc * ang)), s.bladeVel + acc * dt);
            step = s.bladeVel * dt;
        }
        if (ang <= step || ang < 1e-4f) memcpy(c, t, sizeof(float[3]));
        else {
            float k = sinf(step) / sinf(ang), k0 = cosf(step) - dot * k;
            for (int i = 0; i < 3; i++) c[i] = c[i] * k0 + t[i] * k;
            vnorm(c);
        }
    }
    float d[3] = { -s.bladeCur[0], s.bladeCur[2], s.bladeCur[1] };     // model space: forward is -X, side +Z
    float u[3], c[3], l = 0.0f;
    quat_rotate(&pqi, d, u);
    for (int i = 0; i < 3; i++) l += u[i] * u[i];
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

#define FK_GRIP 1.0f     // the left hand's place on the hilt: this far behind the right, toward the pommel

/** A bone's model-space rotation and origin, from this frame's local pose (bones scale 1) */
static void bone_world(int i, T3DQuat *q, float p[3])
{
    const T3DBone *b = &s.skel.bones[i];
    if (b->parentIdx == 0xFFFF) { *q = b->rotation; memcpy(p, b->position.v, sizeof(float[3])); return; }
    T3DQuat pq; float pp[3], r[3];
    bone_world(b->parentIdx, &pq, pp);
    quat_rotate(&pq, b->position.v, r);
    for (int k = 0; k < 3; k++) p[k] = pp[k] + r[k];
    T3DQuat lr = b->rotation;
    t3d_quat_mul(q, &pq, &lr);
}

static void quat_conj(T3DQuat *q) { q->v[0] = -q->v[0]; q->v[1] = -q->v[1]; q->v[2] = -q->v[2]; }

/** The shortest turn taking direction a onto b (both unit) */
static void quat_between(T3DQuat *q, const float a[3], const float b[3])
{
    float ax[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    float sn = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    if (sn < 1e-5f) { *q = (T3DQuat){{ 0.0f, 0.0f, 0.0f, 1.0f }}; return; }
    for (int k = 0; k < 3; k++) ax[k] /= sn;
    t3d_quat_from_rotation(q, ax, atan2f(sn, a[0] * b[0] + a[1] * b[1] + a[2] * b[2]));
}

/** Its big moves two-handed: the left arm reaches (two-bone IK, the elbow bent as the clip has
 *  it) for the hilt behind the right hand, after every other pose this frame (the hand keeps
 *  the clip's turn at the wrist: matched to the right one it could twist the wrist inside out) */
static void grip_pose(float dt)
{
    bool want = s.state == FK_HALFMOON || s.state == FK_UPDOWN || s.state == FK_TRISLAM || s.state == FK_PLUNGE
             || (s.state == FK_HOLYSLAM && s.step == 1);     // the Holy Slam: the blade raised high and brought down
    // not while the fade into the move still swings the hilt round (the hand chased it and jerked): once it is most of the way
    if (want && s.gripW <= 0.0f && s.blendT > 0.3f * s.blendDur) want = false;
    float rate = want ? 7.0f : 2.0f;    // onto the hilt in ~0.15 s, off it again over ~0.5 s
    s.gripW += fmaxf(-rate * dt, fminf(rate * dt, (want ? 1.0f : 0.0f) - s.gripW));
    for (int k = 0; k < 3; k++) s.gripKeep[k].parentIdx = 0xFFFF;
    if (s.gripW <= 0.0f) s.gripFit = 0.0f;
    if (s.gripW <= 0.0f || s.swordBone < 0 || s.armL[0] < 0 || s.armL[1] < 0 || s.armL[2] < 0) return;
    T3DBone *U = &s.skel.bones[s.armL[0]], *L = &s.skel.bones[s.armL[1]], *H = &s.skel.bones[s.armL[2]];
    T3DQuat qR, qP, qU, qL, r, c;
    float pR[3], pP[3], bl[3], T[3], S[3], E[3], d[3], v[3];
    bone_world(s.swordBone, &qR, pR);
    quat_rotate(&qR, (float[3]){ -1.0f, 0.0f, 0.0f }, bl);
    for (int k = 0; k < 3; k++) T[k] = pR[k] - bl[k] * FK_GRIP;
    bone_world(U->parentIdx, &qP, pP);
    quat_rotate(&qP, U->position.v, S); for (int k = 0; k < 3; k++) S[k] += pP[k];
    t3d_quat_mul(&qU, &qP, &U->rotation);
    float a = t3d_vec3_len(&L->position), bb = t3d_vec3_len(&H->position);
    for (int k = 0; k < 3; k++) d[k] = T[k] - S[k];
    float dl = vnorm(d);
    // only where the hands can meet: in reach, and ahead of the shoulder (model space: forward is
    // -X, its left +Z), not across or behind the body, where the arm would pass through it
    float reach = fminf(1.0f, fmaxf(0.0f, ((a + bb) * 0.92f - dl) / 0.4f));     // never locked straight
    float ahead = fminf(1.0f, fmaxf(0.0f, (-(T[0] - S[0]) - 0.2f) / 0.6f));
    float across = fminf(1.0f, fmaxf(0.0f, ((T[2] - S[2]) + 1.6f) / 0.6f));
    float fit = reach * ahead * across;
    s.gripFit += fmaxf(-4.0f * dt, fminf(4.0f * dt, fit - s.gripFit));    // eased, so the arm never snaps
    fit = s.gripFit;
    if (fit <= 0.0f) return;
    dl = fmaxf(fabsf(a - bb) + 0.01f, fminf((a + bb) * 0.97f, dl));
    float along = (a * a - bb * bb + dl * dl) / (2.0f * dl), h = sqrtf(fmaxf(0.0f, a * a - along * along));
    for (int k = 0; k < 3; k++) v[k] = (float[3]){ 0.0f, -1.0f, 0.6f }[k];     // the elbow bends down and out
    float vd = v[0] * d[0] + v[1] * d[1] + v[2] * d[2];
    for (int k = 0; k < 3; k++) v[k] -= d[k] * vd;
    if (vnorm(v) < 1e-4f) { v[0] = 0.0f; v[1] = -1.0f; v[2] = 0.0f; }
    for (int k = 0; k < 3; k++) { E[k] = S[k] + d[k] * along + v[k] * h; T[k] = S[k] + d[k] * dl; }
    // the upper arm onto the elbow, the forearm onto the hilt, the hand as the right one
    float cu[3], wu[3] = { E[0] - S[0], E[1] - S[1], E[2] - S[2] };
    quat_rotate(&qU, L->position.v, cu); vnorm(cu); vnorm(wu);
    quat_between(&r, cu, wu); t3d_quat_mul(&c, &r, &qU); qU = c; t3d_quat_normalize(&qU);
    t3d_quat_mul(&qL, &qU, &L->rotation);
    float cl[3], wl[3] = { T[0] - E[0], T[1] - E[1], T[2] - E[2] };
    quat_rotate(&qL, H->position.v, cl); vnorm(cl); vnorm(wl);
    quat_between(&r, cl, wl); t3d_quat_mul(&c, &r, &qL); qL = c; t3d_quat_normalize(&qL);
    T3DQuat lu, ll, inv;
    inv = qP; quat_conj(&inv); t3d_quat_mul(&lu, &inv, &qU);
    inv = qU; quat_conj(&inv); t3d_quat_mul(&ll, &inv, &qL);
    float w = s.gripW * s.gripW * (3.0f - 2.0f * s.gripW) * fit;
    T3DBone *B[2] = { U, L }; T3DQuat *Q[2] = { &lu, &ll };
    for (int k = 0; k < 2; k++) {
        s.gripKeep[k] = *B[k];
        t3d_quat_nlerp(&B[k]->rotation, &s.gripKeep[k].rotation, Q[k], w);
        B[k]->hasChanged = true;
    }
}

static void quat_from_basis(T3DQuat *q, const float x[3], const float y[3], const float z[3])     // columns x, y, z
{
    float m00 = x[0], m11 = y[1], m22 = z[2], tr = m00 + m11 + m22;
    if (tr > 0.0f) { float k = sqrtf(tr + 1.0f) * 2.0f; *q = (T3DQuat){{ (y[2] - z[1]) / k, (z[0] - x[2]) / k, (x[1] - y[0]) / k, 0.25f * k }}; }
    else if (m00 > m11 && m00 > m22) { float k = sqrtf(1.0f + m00 - m11 - m22) * 2.0f; *q = (T3DQuat){{ 0.25f * k, (y[0] + x[1]) / k, (z[0] + x[2]) / k, (y[2] - z[1]) / k }}; }
    else if (m11 > m22) { float k = sqrtf(1.0f + m11 - m00 - m22) * 2.0f; *q = (T3DQuat){{ (y[0] + x[1]) / k, 0.25f * k, (z[1] + y[2]) / k, (z[0] - x[2]) / k }}; }
    else { float k = sqrtf(1.0f + m22 - m00 - m11) * 2.0f; *q = (T3DQuat){{ (z[0] + x[2]) / k, (z[1] + y[2]) / k, 0.25f * k, (x[1] - y[0]) / k }}; }
    t3d_quat_normalize(q);
}

/** How far into the lunge's thrust the clip is (0..1): out with the thrust, back in with its recovery */
static float thrust_w(void)
{
    if (s.state != FK_LUNGE || s.anim != FKA_LUNGE) return 0.0f;
    float c = clip_time();
    return c < 0.95f ? clamp01f((c - 0.55f) / 0.12f) : 1.0f - clamp01f((c - 0.95f) / 0.3f);
}

/** The sword on its own bone: on the hand (as the mesh has it, at rest), turned in the grip in the
 *  thrust. There the hand is put straight on the forearm too, its palm in (left), the fingers along
 *  the blade, the arm 5 degrees down (the clip's: tools/poses/lunge) */
static void sword_carry(void)
{
    s.handKeep.parentIdx = 0xFFFF;
    if (s.swordCarrier < 0 || s.swordBone < 0) return;
    float w = thrust_w();
    w = w * w * (3.0f - 2.0f * w);
    s.seatQ = (T3DQuat){{ 0.0f, 0.0f, 0.0f, 1.0f }};
    s.seatW = w;
    if (w > 0.0f) {
        T3DBone *h = &s.skel.bones[s.swordBone];
        // the hand: its forearm line along the arm's, its palm to the left (the model: forward -x, up +y, left +z)
        static const float F[3] = { -0.9962f, -0.0872f, 0.0f }, L[3] = { 0.0f, 0.0f, 1.0f };
        float a0[3], a1[3], a2[3], pd;
        memcpy(a0, FORE_H, sizeof(a0)); vnorm(a0);
        pd = PALM_H[0] * a0[0] + PALM_H[1] * a0[1] + PALM_H[2] * a0[2];
        for (int k = 0; k < 3; k++) a1[k] = PALM_H[k] - a0[k] * pd;
        vnorm(a1);
        a2[0] = a0[1] * a1[2] - a0[2] * a1[1]; a2[1] = a0[2] * a1[0] - a0[0] * a1[2]; a2[2] = a0[0] * a1[1] - a0[1] * a1[0];
        T3DQuat qa, qb, qr, qp, ql;
        quat_from_basis(&qa, a0, a1, a2);       // the hand's frame (forearm, palm, their cross) ...
        float b2[3] = { F[1] * L[2] - F[2] * L[1], F[2] * L[0] - F[0] * L[2], F[0] * L[1] - F[1] * L[0] };
        quat_from_basis(&qb, F, L, b2);         // ... onto the arm's (along, left, their cross)
        quat_conj(&qa); t3d_quat_mul(&qr, &qb, &qa);    // the hand's rotation in the model
        float pp[3];
        bone_world(h->parentIdx, &qp, pp);
        quat_conj(&qp); t3d_quat_mul(&ql, &qp, &qr); t3d_quat_normalize(&ql);
        s.handKeep = *h;
        t3d_quat_nlerp(&h->rotation, &s.handKeep.rotation, &ql, w);
        h->hasChanged = true;
        // the sword turned in it: its blade onto the forearm's line, about the grip
        T3DQuat full, id = {{ 0.0f, 0.0f, 0.0f, 1.0f }};
        quat_between(&full, BLADE_H, a0);
        t3d_quat_nlerp(&s.seatQ, &id, &full, w);
    }
    // its bone: the hand's pose x the turn in the grip x its rest offset from the hand
    T3DQuat qh, qt, tmp, qw, ql2; float ph[3], pw[3], v[3], r[3], pt[3];
    bone_world(s.swordBone, &qh, ph);
    sword_seat(s.carrierKP, r);
    quat_rotate(&qh, r, pt);
    for (int k = 0; k < 3; k++) pt[k] += ph[k];
    t3d_quat_mul(&tmp, &qh, &s.seatQ); t3d_quat_mul(&qt, &tmp, &s.carrierK);
    T3DBone *c = &s.skel.bones[s.swordCarrier];
    bone_world(c->parentIdx, &qw, pw);
    quat_conj(&qw);
    t3d_quat_mul(&ql2, &qw, &qt); t3d_quat_normalize(&ql2);
    for (int k = 0; k < 3; k++) v[k] = pt[k] - pw[k];
    quat_rotate(&qw, v, r);
    c->rotation = ql2;
    memcpy(c->position.v, r, sizeof(r));
    c->hasChanged = true;
}

/** The clip's own pose of the left arm back, for the next frame's (a held clip doesn't rewrite it) */
static void grip_unpose(void)
{
    for (int k = 0; k < 3; k++) {
        if (s.gripKeep[k].parentIdx == 0xFFFF || s.armL[k] < 0) continue;
        T3DBone *b = &s.skel.bones[s.armL[k]];
        b->rotation = s.gripKeep[k].rotation; b->hasChanged = true;
    }
}

// ---- Life: the clips ----

// A breather: its weight shifting from foot to foot, the head settling (a loop over 4.2 s)
CLIP(BREATHER,
    TR(L_SR, 0, 0, 0.25f, 0.04f, 0.5f, 0, 0.75f, -0.04f, 1, 0),
    TR(L_WR, 0, 0, 0.3f, 0.02f, 0.6f, -0.015f, 1, 0),
    TR(L_HR, 0, 0, 0.4f, 0.04f, 0.8f, -0.03f, 1, 0));
// Halfmoon: coiled ever tighter round the sword out at its side, sunk low, eyes up on you; the
// slash unwinds it past straight and it settles (the crescent leaves at 1.6 s)
CLIP(HALFMOON,
    TR(L_SY, 0, 0, 0.4f, -0.15f, 1.4f, -0.3f, 1.55f, -0.35f, 1.68f, 0.25f, 1.9f, 0.15f, 2.6f, 0),
    TR(L_WR, 0, 0, 0.4f, -0.12f, 1.5f, -0.2f, 1.68f, -0.05f, 2.2f, 0),
    TR(L_SP, 0, 0, 0.4f, 0.1f, 1.5f, 0.18f, 1.68f, 0.05f, 2.4f, 0),
    TR(L_HP, 0, 0, 1.5f, -0.12f, 1.7f, 0, 2.4f, 0),
    TR(L_SR, 0, 0, 1.6f, 0, 1.7f, 0.12f, 2.2f, 0),
    TR(L_AR, 0, 0, 0.4f, 0.3f, 1.55f, 0.35f, 1.7f, 0.1f, 2.4f, 0));
// Cleave: bowed over the sword as it plants and sinks, then flung up and back as it rips it free
CLIP(CLEAVE,
    TR(L_SP, 0, 0, 0.3f, 0.15f, 0.85f, 0.3f, 0.95f, -0.15f, 1.3f, -0.05f, 1.9f, 0),
    TR(L_WR, 0, 0, 0.3f, -0.05f, 0.85f, -0.15f, 0.95f, 0.05f, 1.4f, 0),
    TR(L_HP, 0, 0, 0.8f, -0.15f, 0.95f, -0.25f, 1.5f, 0),
    TR(L_AP, 0, 0, 0.85f, -0.2f, 1.0f, 0.3f, 1.6f, 0));
// The splitter: a hop back, landing heavy; each of its three swings thrown from the hips (clip
// time, stretched to each swing's strike)
CLIP(SPLIT_BACK,
    TR(L_WR, 0, 0, 0.2f, -0.08f, 0.45f, 0.03f, 0.6f, 0),
    TR(L_SP, 0, 0, 0.25f, -0.1f, 0.45f, 0.08f, 0.6f, 0),
    TR(L_HP, 0, 0, 0.3f, 0.08f, 0.6f, 0));
CLIP(SPLIT_CUT,
    TR(L_SY, 0, 0, 0.4f, 0.12f, 0.55f, -0.18f, 0.8f, -0.05f),
    TR(L_SP, 0, 0, 0.45f, -0.06f, 0.6f, 0.1f, 0.85f, 0),
    TR(L_WR, 0, 0, 0.5f, 0, 0.58f, -0.06f, 0.85f, 0));
// The overhead slam (clip time, AttackCharged from its raise): arched back as the blade goes up,
// then everything behind the blow, the knees giving as it bites the floor
CLIP(SLAM,
    TR(L_SP, 1.72f, 0, 1.85f, -0.2f, 1.95f, -0.25f, 2.02f, 0.25f, 2.2f, 0.15f, 2.35f, 0.05f),
    TR(L_WR, 1.72f, 0, 1.9f, 0.05f, 2.02f, -0.2f, 2.2f, -0.1f, 2.35f, 0),
    TR(L_HP, 1.72f, 0, 1.9f, -0.2f, 2.02f, 0.1f, 2.3f, 0));
// The Holy Slam's gathering: drawn up and back under the raised blade, head thrown back, held,
// then the slam (from the gathering's start)
CLIP(HOLY,
    TR(L_SP, 0, 0, 0.3f, -0.15f, 1.0f, -0.22f, 1.05f, -0.2f, 1.25f, 0.3f, 1.5f, 0.2f),
    TR(L_WR, 0, 0, 1.0f, 0.06f, 1.25f, -0.25f, 1.5f, -0.15f),
    TR(L_HP, 0, 0, 0.5f, -0.1f, 1.0f, -0.12f, 1.25f, 0.1f, 1.6f, 0.05f));     // (thrown back further, its horns met the pauldron)
// A landing from a leap: the weight taken through its knees and back (from the touch)
CLIP(LAND,
    TR(L_WR, 0, 0, 0.08f, -0.25f, 0.35f, -0.05f, 0.6f, 0),
    TR(L_SP, 0, 0, 0.08f, 0.25f, 0.4f, 0.05f, 0.7f, 0),
    TR(L_HP, 0, 0, 0.1f, 0.15f, 0.5f, 0),
    TR(L_AR, 0, 0, 0.1f, 0.35f, 0.5f, 0));
// The war cry at half health: a hunch, then reared back roaring, the free arm flung out (from it)
CLIP(WARCRY,
    TR(L_SP, 0, 0, 0.15f, 0.12f, 0.4f, -0.3f, 1.3f, -0.28f, 1.7f, 0),
    TR(L_HP, 0, 0, 0.15f, 0.15f, 0.4f, -0.35f, 1.3f, -0.3f, 1.7f, 0),
    TR(L_AR, 0, 0, 0.4f, 0.6f, 1.3f, 0.5f, 1.7f, 0),
    TR(L_AP, 0, 0, 0.4f, -0.3f, 1.3f, -0.3f, 1.7f, 0),
    TR(L_WR, 0, 0, 0.15f, -0.08f, 0.4f, 0.04f, 1.7f, 0),
    TR(L_HY, 0.4f, 0, 0.5f, 0.05f, 0.6f, -0.05f, 0.7f, 0.04f, 0.8f, -0.04f, 0.9f, 0));
// Fidgets in a breather: the neck cracked one way then the other; the shoulders rolled; the off
// hand flexed
CLIP(FID_NECK,
    TR(L_HR, 0, 0, 0.15f, 0.3f, 0.3f, 0.28f, 0.42f, -0.05f, 0.55f, -0.3f, 0.7f, -0.28f, 0.85f, 0),
    TR(L_HP, 0, 0, 0.2f, 0.06f, 0.6f, 0.06f, 0.85f, 0));
CLIP(FID_SHOULDER,
    TR(L_SR, 0, 0, 0.3f, 0.1f, 0.6f, -0.08f, 0.9f, 0),
    TR(L_SY, 0, 0, 0.3f, -0.08f, 0.6f, 0.06f, 0.9f, 0),
    TR(L_SP, 0, 0, 0.3f, -0.06f, 0.9f, 0));
CLIP(FID_HAND,
    TR(L_AP, 0, 0, 0.25f, -0.4f, 0.6f, -0.35f, 0.9f, 0),
    TR(L_AR, 0, 0, 0.25f, 0.2f, 0.6f, 0.15f, 0.9f, 0),
    TR(L_HY, 0, 0, 0.3f, 0.25f, 0.6f, 0.2f, 0.9f, 0));

static bool melee_state(void)       // its blade is read from the pose: the waist and spine stay as the clip has them
{
    return s.state == FK_COMBO || s.state == FK_UPDOWN || s.state == FK_ROLLCUT || s.state == FK_PLUNGE || s.state == FK_LUNGE || (s.state == FK_HOLYSLAM && s.step == 0);
}

static float landT = -1.0f;         // since its last landing (LAND)
static uint32_t life_rng(void)      // its fidgets' own rolls (the fight's stay as they were)
{
    static uint32_t x;
    if (!x) x = (uint32_t)TICKS_READ() | 1u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

/** This frame's layer: the move's clip, the head on the player, a flinch, a fidget, the lean into a turn */
static void life_update(Boss *b, float dt)
{
    // a cut (a new move or the next step) keeps what was showing and fades it (no snap)
    bool cut = s.state != s.kState || s.step != s.kStep;
    s.kState = s.state; s.kStep = s.step;
    if (dt > 0.0f) {
        float yr = remainderf(b->rot[1] - s.yawPrev, 2.0f * T3D_PI) / dt;
        s.yawRate += (yr - s.yawRate) * fminf(1.0f, 8.0f * dt);
    }
    s.yawPrev = b->rot[1];
    float want = melee_state() ? 0.0f : 1.0f;
    s.chainW += fmaxf(-8.0f * dt, fminf(8.0f * dt, want - s.chainW));

    // the move's own
    float clip = s.anim >= 0 ? clip_time() : 0.0f;
    switch (s.state) {
    case FK_HALFMOON: key_play(&HALFMOON, s.t, 1.0f, 1.0f, 1.0f); break;
    case FK_CLEAVE:   key_play(&CLEAVE, s.t, 1.0f, 1.0f, 1.0f); break;
    case FK_SPLITTER:
        if (s.step == 0) key_play(&SPLIT_BACK, s.t, 1.0f, 1.0f, 1.0f);
        else key_play(&SPLIT_CUT, clip, WIN[s.step - 1][0] / 0.55f, (s.step & 1) ? 1.0f : -1.0f, 1.0f);
        break;
    case FK_TRISLAM:  if (s.step == 0) key_play(&SLAM, clip, 1.0f, 1.0f, 1.0f); break;
#ifdef HLE_FK_NOHOLYKEYS    // test aid: the Holy Slam as its clips have it
    case FK_HOLYSLAM: break;
#else
    case FK_HOLYSLAM: if (s.step == 1) key_play(&HOLY, s.t, 1.0f, 1.0f, 1.0f); break;
#endif
    case FK_RECOVER:  key_loop(&BREATHER, s.t / 4.2f, clamp01f(s.t / 0.5f)); break;
    case FK_STAGGER: {      // heaving while it kneels; it looks up at you before it rises
        float br = sinf(s.t * 2.0f * T3D_PI * 0.9f);
        s.kv[L_SP] += 0.06f * br; s.kv[L_HP] += -0.04f * br; s.kv[L_WR] += 0.02f * br;
        break;
    }
    default: break;
    }
    if (landT >= 0.0f) { key_play(&LAND, landT, 1.0f, 1.0f, 1.0f); landT += dt; if (landT > 0.8f) landT = -1.0f; }
    if (s.cryT >= 0.0f) { key_play(&WARCRY, s.cryT, 1.0f, 1.0f, 1.0f); s.cryT += dt; if (s.cryT > 1.8f) s.cryT = -1.0f; }

    // the chase: leaning into its turns, the head leading them; running, it leans on
    {
        float lw = s.state == FK_CHASE ? 1.0f : 0.0f, rw = (s.state == FK_CHASE && s.anim == FKA_RUN) ? 1.0f : 0.0f;
        s.leanW += fmaxf(-5.0f * dt, fminf(5.0f * dt, lw - s.leanW));       // ~0.2 s either way
        s.runW += fmaxf(-4.0f * dt, fminf(4.0f * dt, rw - s.runW));
        float l = s.leanW * s.leanW * (3.0f - 2.0f * s.leanW), r = s.runW * s.runW * (3.0f - 2.0f * s.runW);
        s.kv[L_SR] += l * fmaxf(-0.15f, fminf(0.15f, -s.yawRate * 0.06f));
        s.kv[L_HY] -= l * fmaxf(-0.3f, fminf(0.3f, s.yawRate * 0.12f));
        s.kv[L_SP] += r * 0.08f;
    }

    // a fidget now and then in a long breather (about one in 3 s): one of three
    if (s.state == FK_RECOVER && !s.heavy && s.fidgetT <= 0.0f && s.t > 0.6f && s.t < s.recT - 1.2f && (life_rng() % 1000) < (uint32_t)(dt * 330.0f)) {
        s.fidgetT = 0.9f; s.fidgetK = life_rng() % 3;
    }
    if (s.fidgetT > 0.0f) {
        if (s.state != FK_RECOVER) s.fidgetT = 0.0f;
        else {
            const Clip *f = s.fidgetK == 0 ? &FID_NECK : s.fidgetK == 1 ? &FID_SHOULDER : &FID_HAND;
            key_play(f, 0.9f - s.fidgetT, 1.0f, 1.0f, 1.0f);
            s.fidgetT -= dt;
        }
    }

    // the head on the player, a beat late (most in a breather or the chase, a little in a swing)
    float dx, dz;
    to_player(b, &dx, &dz);
    float rel = remainderf(atan2f(-dx, dz) - b->rot[1], 2.0f * T3D_PI);
    float lw = s.state == FK_ROLL || s.state == FK_DEAD ? 0.0f : s.state == FK_STAGGER ? (b->staggerT < 1.2f ? 0.8f : 0.3f)
             : (s.state == FK_CHASE || s.state == FK_RECOVER || s.state == FK_INTRO) ? 1.0f : 0.5f;
    s.lookW += (lw - s.lookW) * fminf(1.0f, 4.0f * dt);
    s.lookYaw += (fmaxf(-1.0f, fminf(1.0f, rel)) - s.lookYaw) * fminf(1.0f, 3.0f * dt);
#ifdef HLE_FK_NOLOOK    // test aid: the head as the clips have it
    s.lookW = 0.0f;
#endif
    s.kv[L_HY] -= s.lookYaw * 0.85f * s.lookW;     // (+ yaw turns it away from + rel)
    s.kv[L_SY] -= s.lookYaw * 0.25f * s.lookW;
    if (s.state == FK_STAGGER) s.kv[L_HP] -= 0.25f * s.lookW;     // up at you from its knee

    // a blow taken: the head snapped back and away, the chest recoiling, sprung back
    Boss *bb = b;
    if (bb->hitSeq != s.hitSeen) {
        if (s.state != FK_DEAD && s.state != FK_INTRO) {
            float k = fmaxf(0.35f, fminf(1.0f, bb->hitWeight / 2.5f)), sd = rel > 0.0f ? 1.0f : -1.0f;
            s.flinchV[0] -= 7.0f * k; s.flinchV[1] += sd * 5.0f * k; s.flinchV[2] -= 5.0f * k;
#ifdef HLE_LOG_MOVES
            debugf("[flinch] fk k %.2f state %d\n", (double)k, s.state);
#endif
        }
        s.hitSeen = bb->hitSeq;
    }
    float h = fminf(dt, 0.05f);
    for (int i = 0; i < 3; i++) {
        s.flinchV[i] += (-140.0f * s.flinch[i] - 15.0f * s.flinchV[i]) * h;
        s.flinch[i] += s.flinchV[i] * h;
    }
    s.kv[L_HP] += s.flinch[0]; s.kv[L_HY] += s.flinch[1]; s.kv[L_SP] += s.flinch[2];

    float fade = expf(-14.0f * dt);
    for (int ch = 0; ch < L_N; ch++) {
        if (cut) s.kc[ch] = s.ko[ch] - s.kv[ch];
        else s.kc[ch] *= fade;
        s.ko[ch] = s.kv[ch] + s.kc[ch];
        s.kv[ch] = 0.0f;
    }
}

/** The layer onto the bones (after the clip's crossfade, before the blade and the grip) */
static void life_apply(void)
{
    float c = s.chainW * s.chainW * (3.0f - 2.0f * s.chainW);
    bone_turn(s.waistBone, s.ko[L_WP] * c, 0.0f, 0.0f, s.ko[L_WR] * c * 64.0f, &s.waistKeep);
    bone_turn(s.spineBone, s.ko[L_SP] * c, s.ko[L_SY] * c, s.ko[L_SR] * c, s.spineRise, &s.spineKeep);
    bone_turn(s.headBone, s.ko[L_HP], s.ko[L_HY], s.ko[L_HR], 0.0f, &s.headKeep);
    bone_turn(s.armL[0], s.ko[L_AP], 0.0f, s.ko[L_AR], 0.0f, &s.armKeep);
}

static void life_unpose(void)
{
    bone_unpose(s.armL[0], &s.armKeep);
    bone_unpose(s.headBone, &s.headKeep);
    bone_unpose(s.spineBone, &s.spineKeep);
    bone_unpose(s.waistBone, &s.waistKeep);
}

// ---- Lifetime ----

void fallen_knight_free(void)
{
    if (!s.ready) return;
    gpu_wait();
    for (int i = 0; i < FKA_COUNT; i++) t3d_anim_destroy(&s.anims[i]);
    t3d_model_free(s.body);
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
        s.body = t3d_model_load("rom:/fallen_knight/fw_knight.t3dm");
        s.mat = malloc_uncached(sizeof(T3DMat4FP));
        memset(s.mat, 0, sizeof(T3DMat4FP));    // nothing drawn until the first update sets it (was whatever the heap held)
        s.swordBone = t3d_skeleton_find_bone(&s.skel, "Hand-Right");
        s.headBone = t3d_skeleton_find_bone(&s.skel, "Head");
        s.spineBone = t3d_skeleton_find_bone(&s.skel, "Spine1");
        s.waistBone = t3d_skeleton_find_bone(&s.skel, "Waist");
        s.armL[0] = t3d_skeleton_find_bone(&s.skel, "UpperArm-Left");
        s.armL[1] = t3d_skeleton_find_bone(&s.skel, "LowerArm-Left");
        s.armL[2] = t3d_skeleton_find_bone(&s.skel, "Hand-Left");
        s.swordCarrier = t3d_skeleton_find_bone(&s.skel, "IK_Hand.R");
        s.seatQ = (T3DQuat){{ 0.0f, 0.0f, 0.0f, 1.0f }}; s.seatW = 0.0f;
        if (s.swordCarrier >= 0 && s.swordBone >= 0) {     // (the fresh skeleton is at rest)
            T3DQuat qh, qc; float ph[3], pc[3], d[3];
            bone_world(s.swordBone, &qh, ph); bone_world(s.swordCarrier, &qc, pc);
            quat_conj(&qh);
            t3d_quat_mul(&s.carrierK, &qh, &qc);
            for (int k = 0; k < 3; k++) d[k] = pc[k] - ph[k];
            quat_rotate(&qh, d, s.carrierKP);
        }
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
    s.anim = -1; poseN = 0;
    play(FKA_HERODROP, true);     // the entrance: the fall, the superhero landing, the rise
    s.blendT = 0.0f;

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
    s.cdCleave = 5.0f; s.cdHalfmoon = 12.0f; s.cdMoonblade = 9.0f; s.cdRoll = 0.0f; s.cdSplitter = 6.0f; s.cdUpdown = 3.0f; s.cdTri = 14.0f; s.cdHoly = 20.0f; s.cdPlunge = 8.0f; s.cdLunge = 6.0f; s.cdBackLunge = 10.0f;
    s.queued = -1; s.fireHitT = 0.0f;
    for (int i = 0; i < 11; i++) s.lastUsed[i] = get_ticks_ms() * 0.001f;     // (all equally fresh at the start)
    s.enraged = false;
    s.bladeW = 0.0f;
    s.beam2T = -1.0f;
    s.cryT = -1.0f; s.chainW = 1.0f; s.roamT = 0.0f; s.stepT = 0.0f; s.hitSeen = boss->hitSeq; s.lookYaw = s.lookW = 0.0f;
    memset(s.ko, 0, sizeof(s.ko)); memset(s.kc, 0, sizeof(s.kc)); memset(s.flinch, 0, sizeof(s.flinch)); memset(s.flinchV, 0, sizeof(s.flinchV));
    enter(FK_INTRO);
}

void fallen_knight_skip_intro(Boss* boss)
{
    if (!s.ready || s.state != FK_INTRO) return;
    boss->pos[1] = s.groundY;
    if (s.anim == FKA_HERODROP) {     // a skip stands up over the idle's blend; a Tower floor (no intro) starts standing
        play(FKA_IDLE, true);
        if (s.t < 0.05f) s.blendT = 0.0f;
    }
    boss->mode = BOSS_MODE_COMBAT;
    boss->state = BOSS_STATE_NEUTRAL;
    enter(FK_CHASE);
}

bool fallen_knight_stagger(Boss* b)
{
    // Knocked down where it stands; not in the air (the leap) or already down
    if (!s.ready || s.state == FK_INTRO || s.state == FK_MOONBLADE || s.state == FK_STAGGER || s.state == FK_DEAD) return false;
    if (s.state == FK_PLUNGE && s.step == 1) return false;
    if (s.state == FK_LUNGE && (s.step == 0 || s.step == 2)) return false;     // off its feet: the leap back, the skim
    b->pos[1] = s.groundY;
    enter(FK_STAGGER);
    play(FKA_KNEEL, true);
    sfx(SCENE1_SFX_FW_HURT, 1.0f);
    b->staggerT = boss_stagger_s();
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
    // Stands and plants its sword point-down, sinks into a crouch with it, then rips it up: five crimson blades rise one
    // by one, fanned along where it faced (it doesn't turn)
    const float PLANT = 0.3f, CROUCH = 0.5f, FROM = 0.25f, SINK = 1.5f, RELEASE = CROUCH + (0.85f - FROM) / SINK + 0.05f, GAP = 0.12f;
    if (s.anim == FKA_CLEAVE) t3d_anim_set_speed(&s.anims[s.anim], clip_time() < 0.85f ? SINK : 1.0f);
    if (s.t < RELEASE) {
        if (s.t < CROUCH) play(FKA_IDLE, false);
        else if (s.anim != FKA_CLEAVE) { play(FKA_CLEAVE, true); t3d_anim_set_time(&s.anims[s.anim], FROM); }
        if (s.t < RELEASE - 0.12f) hold_blade(0.3f, 0.0f, -0.95f);
        if (s.t >= PLANT - 0.07f && s.t - dt < PLANT - 0.07f) sfx(SCENE1_SFX_BOSS_LAND1, 0.9f);    // its onset on the bite
        if (s.t >= PLANT && s.t - dt < PLANT) {
            screen_shake_set_shake_mag(2.0f);
        }
    }
    if (s.t >= RELEASE - 0.06f && s.t - dt < RELEASE - 0.06f) swing_sfx();     // peaking as it rips up
    if (s.step < 5 && s.t >= RELEASE + s.step * GAP) {
        float o[3], dx, dz;
        cleave_point(b, s.step, o, &dx, &dz);
        skill_fx_spawn_wave_hostile(SKILL_WAVE_BLADE, o, (float[3]){ dx * 6.0f, 0.0f, dz * 6.0f }, FK_FELT(7.0f), 2, 0.4f, 1.5f, false);
        if (s.step == 0) screen_shake_set_shake_mag(5.0f);
        s.step++;
    }
    if (s.t > RELEASE + 0.95f) recover(1.3f, false);
}

static void halfmoon_update(Boss *b, float dt)
{
    // Crouches with its sword held level and out to the side, settles its aim and holds it,
    // then the wide rolling slash where it faced: get round its side, or roll through it
    const float LOCK = 0.35f, CHARGE = 1.6f, HALF = 1.0f;      // the crescent reaches 57 degrees each side
    if (s.step == 0) {
        t3d_anim_set_speed(&s.anims[s.anim], 2.0f / CHARGE);   // the clip's sweep (~2.0 s in) as the crescent leaves
        if (s.t < LOCK) face_player(b, dt, 4.0f);
        else if (s.t - dt < LOCK) sfx(SCENE1_SFX_SPELL_CAST_BOSS, 0.8f);
        if (clip_time() > 0.1f) hold_blade_turn(-0.45f, -0.89f, 0.0f, 5.0f);    // turned there smoothly (it whipped round)
        if (s.t >= CHARGE) {
            skill_fx_next_arc_half(HALF);
            skill_fx_spawn_arc_hostile((float[3]){ b->pos[0], b->pos[1] + 12.0f, b->pos[2] }, b->rot[1], 90.0f, FK_FELT(55.0f));
            screen_shake_set_shake_mag(12.0f);
            sfx(SCENE1_SFX_BLADE_SLASH_BOSS, 1.0f);
            s.step = 1;
        }
    }
    if (s.t > 2.9f) recover(1.6f, false);
}

#define BEAM_GAP 0.25f      // between a Moon Blade cross's two beams (58 a second: ~15 apart along their path, told apart)
#define BEAM_TILT 1.2217f   // each beam 70 degrees off level, the two across each other: a tall, narrow X (10 to each side) to step out of
// A cut for each beam of a cross, as it leaves: Attack1's diagonal for the first, Attack2's rising
// cut, the other diagonal, for the second (Attack3's overhead is vertical: it matched neither);
// begun just before the strike so the blade meets the beam, sped up to fit the 0.25 s between them
static void beam_swing(int k)
{
    play(k ? FKA_ATTACK2 : FKA_ATTACK1, true);
    t3d_anim_set_time(&s.anims[s.anim], WIN[k][0] - 0.05f);
    t3d_anim_set_speed(&s.anims[s.anim], 1.2f);
}
static void moonblade_update(Boss *b, float dt)
{
    // leap high and rain five crimson crosses on the player
    if (s.step == 0) {
        s.velY = 120.0f;
        play(FKA_BACKFLIP, true);
        skill_fx_spawn_crown_hostile((float[3]){ b->pos[0], s.groundY, b->pos[2] });     // its take-off, as the player's
        s.step = 1;
    }
    s.velY -= 120.0f * ((s.step >= 1 && s.step <= 5 && s.velY < 15.0f) ? 0.15f : 1.0f) * dt;
    if (s.velY < -60.0f) s.velY = -60.0f;
    b->pos[1] += s.velY * dt;
    face_player(b, dt, 6.0f);
    // the crosses come slower and further apart than the player's own: readable, dodgeable
    if (s.step >= 1 && s.step <= 5 && s.t >= 0.55f + (s.step - 1) * 0.55f) {
        float o[3] = { b->pos[0], b->pos[1] + 20.0f, b->pos[2] };
        float side = (s.step - 3) * 4.0f;     // walking across the player (narrow X: no wider, or it passes a still one)
        float fx = -fm_sinf(b->rot[1]), fz = fm_cosf(b->rot[1]);
        float tgt[3] = { character.pos[0] + fz * side, s.groundY + 4.0f, character.pos[2] - fx * side };
        float d[3] = { tgt[0] - o[0], tgt[1] - o[1], tgt[2] - o[2] };
        float len = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        if (len < 1.0f) len = 1.0f;
        const float SPEED = 58.0f;
        if (s.step == 5) {      // the last a little ahead of the player (half its way there): strafing alone won't clear it
            float vx, vz, lead = 0.5f * len / SPEED;
            character_get_velocity(&vx, &vz);
            tgt[0] += vx * lead; tgt[2] += vz * lead;
            d[0] = tgt[0] - o[0]; d[2] = tgt[2] - o[2];
            len = fmaxf(1.0f, sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]));
        }
        // Each cross is two beams, tilted across each other, the second loosed BEAM_GAP after the
        // first along the same path: they make the criss-cross, a gap between them (they never
        // touch), 8 each (the cross's 16)
        float v[3] = { d[0] / len * SPEED, d[1] / len * SPEED, d[2] / len * SPEED };
        skill_fx_spawn_slash_hostile(o, v, BEAM_TILT, FK_FELT(8.0f), len / SPEED);
        memcpy(s.beam2O, o, sizeof(o)); memcpy(s.beam2V, v, sizeof(v)); s.beam2Life = len / SPEED; s.beam2T = BEAM_GAP;
        sfx(SCENE1_SFX_FW_SLICE, 1.0f);
        beam_swing(0);      // its first beam's cut (the second's is played as that beam leaves)
        s.step++;
    }
    if (b->pos[1] <= s.groundY && s.velY < 0.0f) {
        b->pos[1] = s.groundY;
        landT = 0.0f;
        skill_fx_spawn_crown_hostile((float[3]){ b->pos[0], s.groundY, b->pos[2] });
        screen_shake_set_shake_mag(6.0f);
        sfx(SCENE1_SFX_BOSS_LAND1, 0.9f);
        recover(1.3f, false);
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
        else recover(FK_QUICK_REC, false);
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
        { static float was; if (s.step == 1 && clip >= 1.95f && was < 1.95f) sfx(SCENE1_SFX_BOSS_LAND1, 0.9f); was = clip; }    // (the clip runs at its own speed here: clip - dt missed it)
        if (s.step == 1 && clip >= 2.02f) {    // the blade meets the floor
            screen_shake_set_shake_mag(6.0f);
            s.step = 2;
        }
        if (clip >= 2.35f || clip_done()) recover(FK_QUICK_REC, false);
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
        // The Up-Down's overhead slam (the same clip, moved as it is), told apart by a grinding of
        // stone as it raises the blade and held a moment longer
        face_player(b, dt, 3.0f);
        swing_pace(DN_FROM, W_DN[0], 1.0f);
        if (s.t <= dt) sfx(SCENE1_SFX_ROCK_BLAST, 0.7f);
        { static float was; if (clip >= 1.95f && was < 1.95f) sfx(SCENE1_SFX_BOSS_LAND1, 0.9f); was = clip; }     // its onset on the bite (clip - dt missed it when the clip ran fast)
        if (clip >= 2.02f) {    // the blade meets the floor: mark the triangle where the player stands
            float fx = -fm_sinf(b->rot[1]), fz = fm_cosf(b->rot[1]);
            screen_shake_set_shake_mag(8.0f);
            boss_ground_crush_spawn_varied(b->pos[0] + fx * 22.0f, s.groundY + 0.25f, b->pos[2] + fz * 22.0f, FK_CRACK, b->rot[1]);
            float a0 = (rand_custom_u32() % 628) / 100.0f;
            for (int i = 0; i < 3; i++) {
                float a = a0 + i * 2.0944f;
                s.tri[i][0] = character.pos[0] + sinf(a) * 40.0f; s.tri[i][1] = character.pos[2] + cosf(a) * 40.0f;
            }
            for (int i = 0; i < 3; i++)
                for (int k = 0; k < 2; k++) { float p[3]; tri_point(i, k * 0.5f, p); skill_fx_spawn_burst_warn(p, (float)(i * 2 + k)); }
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
            sfx_at(SCENE1_SFX_SLAM_BOSS, 1.0f, (s.tri[0][0] + s.tri[1][0] + s.tri[2][0]) / 3.0f,
                   (s.tri[0][1] + s.tri[1][1] + s.tri[2][1]) / 3.0f);     // its thump as the blades erupt, from the triangle
            s.step = 2; s.t = 0.0f;
        }
    } else if (s.t >= 0.5f) recover(1.6f, false);
}

static void holyslam_update(Boss *b, float dt)
{
    // A swing, then the sword held high while the light gathers (its sound; no floor marks),
    // then slammed down: a blast all round it. Get far away, roll it or jump it
    float clip = clip_time();
    if (s.step == 0) {          // the opening swing (wound up like the combo's)
        swing_pace(0.0f, WIN[0][0], FK_WIND);
        if (clip < WIN[0][0] && swing_tracks(FK_LOCK_K)) face_player(b, dt, 2.5f);
        melee_check(b, WIN[0][0], WIN[0][1], FK_FELT(15.0f));
        if (clip >= WIN[0][1] || clip_done()) {
            s.step = 1; s.t = 0.0f; s.hitDone = false;
            play(FKA_HOLYRAISE, true);    // its tell: both hands high behind the head, elbows out, the blade down its back
            sfx(SCENE1_SFX_HOLY_BOSS, 1.0f);     // the light called down, swelling to the slam
        }
    } else if (s.step == 1) {   // the gathering
        face_player(b, dt, 1.5f);
        if (s.anim == FKA_HOLYRAISE) {      // held 1 s, then its slam
            if (s.t >= 1.0f) { play(FKA_CHARGED, true); t3d_anim_set_time(&s.anims[s.anim], W_DN[0] - 0.08f); s.blendT = s.blendDur = 0.1f; s.windK = 1.0f; }
        } else swing_pace(DN_FROM, W_DN[0], 1.5f);
        if (!s.hitDone && clip >= W_DN[0] - 0.08f) s.hitDone = true;    // as it comes down
        if (clip >= 2.02f) {
            float o[3] = { b->pos[0], s.groundY + 4.0f, b->pos[2] };
            skill_fx_spawn_arc_hostile(o, b->rot[1], 105.0f, FK_FELT(45.0f));
            skill_fx_spawn_arc_hostile(o, b->rot[1] + T3D_PI, 105.0f, FK_FELT(45.0f));
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
    } else {    // spent a moment, low over the blow, then up on its feet with the sword kept point-down (the
                // charged clip's tail swung it up round its head onto the shoulder, and the breather's
                // point-down turn then cut it through the body), then the longest breather, spent
        if (s.t >= 0.05f) hold_blade(0.3f, 0.25f, -0.92f);
        if (s.t < 0.25f) t3d_anim_set_speed(&s.anims[s.anim], 1.0f - s.t / 0.25f);   // the blow's follow-through slowing to a halt, still low
        else if (s.anim != FKA_IDLE) { play(FKA_IDLE, false); s.blendT = s.blendDur = 0.55f; }
        if (s.t >= 0.8f) recover(2.2f, true);
    }
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
    if (clip_done()) recover(FK_QUICK_REC, false);
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
        if (u < FLIGHT - DOWN) {    // in the air: the charged clip's sword raised, the legs tucked into the leap
            if (s.anim != FKA_PLUNGELEAP) play(FKA_PLUNGELEAP, true);
            clip_to(u < 0.6f ? 0.6f * smooth01(u / 0.6f) : u, dt);
        } else {                    // driven down: the charged clip's strike, as it was
            if (s.anim != FKA_CHARGED) { play(FKA_CHARGED, true); t3d_anim_set_time(&s.anims[s.anim], 1.86f + (s.t - (FLIGHT - DOWN))); s.blendT = s.blendDur = 0.08f; }
            clip_to(1.86f + (u - (FLIGHT - DOWN)), dt);
        }
        melee_check(b, W_DN[0], W_DN[1], 18.0f);
        if (b->pos[1] <= s.groundY && s.velY < 0.0f) {
            float base[3], tip[3], o[3] = { b->pos[0], s.groundY + 4.0f, b->pos[2] };
            if (sword_segment(base, tip)) { o[0] = tip[0]; o[2] = tip[2]; }     // where the blade bites
            boss_ground_crush_spawn_varied(o[0], s.groundY + 0.25f, o[2], FK_CRACK, b->rot[1]);
            float dmg = s.hitDone ? 0.0f : 10.0f;       // the blade itself already struck: the ring doesn't again
            skill_fx_spawn_arc_hostile(o, b->rot[1], 20.0f, dmg);
            skill_fx_spawn_arc_hostile(o, b->rot[1] + T3D_PI, 20.0f, dmg);
            screen_shake_set_shake_mag(10.0f);
            sfx(SCENE1_SFX_BOSS_LAND1, 0.9f);
            s.step = 2; s.t = 0.0f;
        }
    } else {
        clip_to(fminf(2.35f, clip + dt * (clip < 2.12f ? 0.8f : 0.45f)), dt);    // slow to pull it free
        if (s.t > 0.95f) recover(1.4f, false);
    }
}

/** A point of its fire trail (skill_fx draws it and says where it burns) */
static void fire_light(const Boss *b, float x, float z)
{
    skill_fx_fire_trail_add((float[3]){ x, s.groundY, z }, b->rot[1], FIRE_LIFE);
}

/** One standing in the fire (on the floor: jump it) is burnt, then again each second it stays */
static void fire_update(float dt)
{
    if (s.fireHitT > 0.0f) { s.fireHitT -= dt; return; }
    if (character.pos[1] - s.groundY > 6.0f || !skill_fx_fire_trail_touches(character.pos, character.capsuleCollider.radius)) return;
    character_apply_damage(FK_FELT(8.0f));
    s.fireHitT = 1.0f;
    sfx_at(SCENE1_SFX_FIRE, 0.6f, character.pos[0], character.pos[2]);
}

static void lunge_update(Boss *b, float dt)
{
    // The fire lunge (the Lunge clip, keyed): down into its lunge, the sword hand
    // drawn back to its hip with the blade level at you, its left leg leading, turning on you,
    // held as it commits; then it skims at you barely off the floor, the arm and blade thrust out
    // in one line as it goes, stepping through to land on its right leg as far as you were, fire
    // springing up along its path; it stops short and draws the sword back in. Step out of its
    // line (and clear of the fire). Never from close: the short one (quicker tell, at most 80)
    // needs 70 between them, and one closed in on in its tell leaps back first: away, a low
    // backflip, then the lunge from there
    const float DRAW_AT = 0.55f, LUNGE_END = 0.95f;     // the clip: drawn back by 0.55 s, then the thrust and its stride
    const float TELL = s.lungeKind == LUNGE_SHORT ? 0.5f : s.lungeKind == LUNGE_BACK ? 0.6f : 0.85f, LOCK = TELL * 0.65f;
    const float SPEED = s.lungeKind == LUNGE_SHORT ? 170.0f : 240.0f, HOP = 3.5f;
    float fx = -fm_sinf(b->rot[1]), fz = fm_cosf(b->rot[1]);
    if (s.step == 0) {          // the leap back
        const float VY = 70.0f, G = 190.0f, BACK = 150.0f;
        if (s.t <= dt) {
            float dx, dz, d = to_player(b, &dx, &dz);
            if (d > 1e-3f) { s.moveX = -dx / d * BACK; s.moveZ = -dz / d * BACK; } else { s.moveX = -fx * BACK; s.moveZ = -fz * BACK; }
            play(FKA_BACKFLIP, true);
            t3d_anim_set_speed(&s.anims[s.anim], 0.6f / (2.0f * VY / G));     // the flip done as it lands
            s.velY = VY;
            sfx(SCENE1_SFX_SWING_BOSS, 0.6f);
        }
        face_player(b, dt, 4.0f);
        move(b, s.moveX, s.moveZ, dt);
        s.velY -= G * dt;
        b->pos[1] = fmaxf(s.groundY, b->pos[1] + s.velY * dt);
        if (b->pos[1] <= s.groundY && s.velY < 0.0f) {
            landT = 0.0f;
            sfx(SCENE1_SFX_BOSS_LAND1, 0.7f);
            s.step = 1; s.t = 0.0f;
            play(FKA_LUNGE, true);
        }
        return;
    }
    if (s.step == 1) {          // the tell: into the lunge, turning on you, then held
        if (s.anim != FKA_LUNGE) play(FKA_LUNGE, true);
        clip_to(DRAW_AT * fminf(1.0f, (s.t + dt) / TELL), dt);     // drawn back over the tell, then held
        if (s.t < LOCK) face_player(b, dt, 5.0f);
        else hle_commit();
        if (s.t >= TELL) {
            float dx, dz, d = to_player(b, &dx, &dz), ahead = dx * fx + dz * fz;
            if (d < 50.0f && s.lungeKind != LUNGE_BACK) {      // closed in on: no stab from here, it leaps back first
                s.lungeKind = LUNGE_BACK; s.step = 0; s.t = 0.0f;
                return;
            }
            s.lungeLen = fmaxf(8.0f, fminf(d, ahead > 0.0f ? ahead : d) - 18.0f);   // short of where you were: the blade reaches on
            if (s.lungeKind == LUNGE_SHORT) s.lungeLen = fminf(s.lungeLen, 80.0f);
            s.lungeO[0] = b->pos[0]; s.lungeO[1] = b->pos[2]; s.lungeGone = 0.0f; s.fireLit = 0;
            s.fireGap = fmaxf(8.0f, s.lungeLen / (FIRE_MAX - 1));
            s.step = 2; s.t = 0.0f;
            swing_sfx();
            sfx(SCENE1_SFX_FIRE, 0.8f);     // its launch (the boss's)
            skill_fx_fire_trail_begin();
            fire_light(b, b->pos[0], b->pos[2]);
#ifdef HLE_LOG_LUNGE    // test aid: each lunge's launch, and where it stops
            debugf("[lunge] kind %d go %.0f (player %.0f away) gap %.1f at %d ms\n", s.lungeKind, (double)s.lungeLen, (double)d, (double)s.fireGap, (int)get_ticks_ms());
#ifdef HLE_LOG_MOVE_MS
            { extern uint32_t dragon_vi_frame(void); debugf("[movems] lunge launch VI %d\n", (int)dragon_vi_frame()); }
#endif
#endif
        }
        return;
    }
    if (s.step == 2) {          // the skim
        s.striking = true;
        float want = fminf(SPEED * dt, s.lungeLen - s.lungeGone), x0 = b->pos[0], z0 = b->pos[2];
        if (dt > 0.0f) move(b, fx * want / dt, fz * want / dt, dt);
        float went = sqrtf((b->pos[0] - x0) * (b->pos[0] - x0) + (b->pos[2] - z0) * (b->pos[2] - z0));
        if (went < want * 0.5f) s.lungeLen = s.lungeGone;      // a wall: it stops there
        s.lungeGone += went;
        b->pos[1] = s.groundY + HOP * sinf(T3D_PI * clamp01f(s.lungeGone / fmaxf(1.0f, s.lungeLen)));
        clip_to(DRAW_AT + (LUNGE_END - DRAW_AT) * clamp01f(s.lungeGone / fmaxf(1.0f, s.lungeLen)), dt);    // the thrust, the legs stepping through, as far as it has gone
        for (int lit = (int)(s.lungeGone / s.fireGap); s.fireLit < lit; ) {     // a flame each fireGap along its path
            s.fireLit++;
            fire_light(b, s.lungeO[0] + fx * s.fireLit * s.fireGap, s.lungeO[1] + fz * s.fireLit * s.fireGap);
        }
        melee_check(b, DRAW_AT + 0.06f, 1e9f, FK_FELT(33.0f));     // once the blade is on its way out: 33 taken
        if (s.lungeGone >= s.lungeLen - 1e-3f) {
            if (s.lungeGone - s.fireLit * s.fireGap > 2.0f) fire_light(b, b->pos[0], b->pos[2]);     // the trail right up to where it stops
            b->pos[1] = s.groundY;
            landT = 0.0f;
            sfx(SCENE1_SFX_BOSS_LAND1, 0.6f);
            s.step = 3; s.t = 0.0f;
            t3d_anim_set_speed(&s.anims[s.anim], 1.0f);     // stopped: the clip's recovery, the sword drawn in and onto the shoulder
#ifdef HLE_LOG_LUNGE
            { float dx, dz; debugf("[lunge] stop: went %.0f, %d fire points, player %.0f away, blade hit %d at %d ms\n", (double)s.lungeGone, s.fireLit, (double)to_player(b, &dx, &dz), s.hitDone, (int)get_ticks_ms()); }
#endif
        }
        return;
    }
    // planted, recovering (the clip, 0.95-1.55 s); then now and then straight into its combo
    if (s.anim == FKA_LUNGE && !clip_done()) t3d_anim_set_speed(&s.anims[s.anim], 1.0f);
    if (s.t >= 0.6f) {
        float dx, dz;
        uint32_t r = rand_custom_u32() % 100;
#ifdef HLE_FK_PACE
        r = HLE_FK_PACE == 1 ? 0 : 99;
#endif
        if (r < 35 && to_player(b, &dx, &dz) < FK_MELEE_RANGE + 6.0f) { enter(FK_COMBO); s.step = 1; play(FKA_ATTACK1, true); }
        else recover(1.3f, false);
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
    if (HLE_FK_FORCE == 10) { if (dist < 70.0f) return; s.lungeKind = dist >= 110.0f ? LUNGE_FULL : LUNGE_SHORT; enter(FK_LUNGE); s.step = 1; play(FKA_LUNGE, true); debugf("[fk] lunge %d dist %.0f\n", s.lungeKind, (double)dist); return; }
    if (HLE_FK_FORCE == 11) { s.lungeKind = LUNGE_BACK; enter(FK_LUNGE); debugf("[fk] lunge back dist %.0f\n", (double)dist); return; }   // 10 = the lunge (by range), 11 = the leap back and lunge
    if (HLE_FK_FORCE >= 4 && dist > FK_MELEE_RANGE - 4.0f) return;     // the melee ones: in reach first
    if (HLE_FK_FORCE == 9) { enter(FK_MOONBLADE); debugf("[fk] moonblade\n"); return; }   // 9 = the Moon Blade (the leap and its slashes), at any range
    if (HLE_FK_FORCE == 4) { enter(FK_UPDOWN); play(FKA_ATTACK2, true); debugf("[fk] updown\n"); return; }
    if (HLE_FK_FORCE == 5) { enter(FK_COMBO); s.step = 1; play(FKA_ATTACK1, true); debugf("[fk] combo\n"); return; }
#endif
    // Every move off its cooldown is a candidate at any range, the likelier the longer since it last
    // came (1 + the seconds since, to 20): all of them come round, none twice running. One that must
    // reach (the combo, up-down, cleave, halfmoon, holy slam) picked out of reach: it closes in first.
    // The fire lunge is its gap-closer: off 70, the likelier the further (twice past 110), and again
    // twice when the player has kept away; the leap back and lunge from close in
    enum { P_HALFMOON, P_MOONBLADE, P_HOLY, P_TRI, P_PLUNGE, P_SPLITTER, P_CLEAVE, P_UPDOWN, P_LUNGE, P_BACKLUNGE, P_COMBO, P_N };
    static const float REACH[P_N] = { 70.0f, 1e9f, 50.0f, 1e9f, 1e9f, 1e9f, 60.0f, FK_MELEE_RANGE + 8.0f, 1e9f, 1e9f, FK_MELEE_RANGE - 4.0f };
    float now = get_ticks_ms() * 0.001f;
    int m = s.queued;
    bool farOnly = m >= 0 && now - s.queuedAt > 2.0f;     // kept off it 2 s (the player backing away): a move that reaches
    if (farOnly) m = -1;
    if (m < 0) {
        float *cd[P_N] = { &s.cdHalfmoon, &s.cdMoonblade, &s.cdHoly, &s.cdTri, &s.cdPlunge, &s.cdSplitter, &s.cdCleave, &s.cdUpdown, &s.cdLunge, &s.cdBackLunge, NULL };
        float w[P_N], tot = 0.0f;
        for (int i = 0; i < P_N; i++) {
            bool ok = (!cd[i] || *cd[i] <= 0.0f) && (i != P_HOLY || s.enraged) && (i != P_PLUNGE || dist >= 40.0f)    // (the plunge's leap needs room)
                   && (i != P_LUNGE || dist >= 70.0f) && (i != P_BACKLUNGE || dist < 60.0f)
                   && (!farOnly || REACH[i] > 1e8f);
            w[i] = ok ? 1.0f + fminf(now - s.lastUsed[i], 19.0f) : 0.0f;
            if (i == P_LUNGE) w[i] *= (dist >= 110.0f ? 2.0f : 1.0f) * (farOnly ? 2.0f : 1.0f);
            tot += w[i];
        }
#ifdef HLE_LOG_CHOOSE    // test aid: the distance and each candidate's weight at a pick
        debugf("[choose] dist %.0f w %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f\n", (double)dist, (double)w[0], (double)w[1], (double)w[2], (double)w[3], (double)w[4], (double)w[5], (double)w[6], (double)w[7], (double)w[8], (double)w[9], (double)w[10]);
#endif
        static uint32_t x;              // its own rolls (xorshift32, seeded from the clock), as Shadowborne's
        if (!x) x = (uint32_t)TICKS_READ() | 1u;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        if (tot <= 0.0f) { s.queuedAt = now; return; }     // (none off cooldown that reaches: on after it)
        float r = (x % 10000) / 10000.0f * tot;
        m = 0;
        while (m < P_N - 1 && (w[m] <= 0.0f || r >= w[m])) { r -= w[m]; m++; }
        s.queued = m; s.queuedAt = now;
    }
    if (dist > REACH[m]) return;    // closing in on it
    s.queued = -1; s.lastUsed[m] = now;
    switch (m) {
    case P_HALFMOON:  s.cdHalfmoon = 16.0f; enter(FK_HALFMOON); play(FKA_HALFMOON, true); break;
    case P_MOONBLADE: s.cdMoonblade = 12.0f; enter(FK_MOONBLADE); break;
    case P_HOLY:      s.cdHoly = 20.0f; enter(FK_HOLYSLAM); play(FKA_ATTACK1, true); break;
    case P_TRI:       s.cdTri = 14.0f; enter(FK_TRISLAM); play(FKA_CHARGED, true); t3d_anim_set_time(&s.anims[s.anim], DN_FROM); t3d_anim_set_speed(&s.anims[s.anim], 0.1f); break;
    case P_PLUNGE:    s.cdPlunge = 14.0f; enter(FK_PLUNGE); play(FKA_CHARGED, true); t3d_anim_set_time(&s.anims[s.anim], 0.3f); break;
    case P_SPLITTER:  s.cdSplitter = 11.0f; enter(FK_SPLITTER); break;
    case P_CLEAVE:    s.cdCleave = 9.0f; enter(FK_CLEAVE); break;
    case P_UPDOWN:    s.cdUpdown = 7.0f; enter(FK_UPDOWN); play(FKA_ATTACK2, true); break;
    case P_LUNGE:     s.cdLunge = 10.0f; s.cdBackLunge = fmaxf(s.cdBackLunge, 5.0f); s.lungeKind = dist >= 110.0f ? LUNGE_FULL : LUNGE_SHORT; enter(FK_LUNGE); s.step = 1; play(FKA_LUNGE, true); break;
    case P_BACKLUNGE: s.cdBackLunge = 16.0f; s.cdLunge = fmaxf(s.cdLunge, 5.0f); s.lungeKind = LUNGE_BACK; enter(FK_LUNGE); break;
    default:          enter(FK_COMBO); s.step = 1; play(FKA_ATTACK1, true); break;
    }
}

/** Footwork between its attacks: it picks a leg every 0.6-1.4 s and walks it, facing the player all
 *  the while (any direction: round them either way, in, or back off), on the knight's own walk,
 *  strafe and backward clips. Turning on the spot it steps round rather than spinning on planted
 *  feet (its walk, played while the body comes round). Its moves and their timing are untouched */
// Footfalls at the knight's pace (its steps, deeper)
static void footfalls(float dt, bool run)
{
    static float stepT; static int stepN;
    if ((stepT -= dt) <= 0.0f) {
        sfx(SCENE1_SFX_FW_STEP1 + (stepN++ & 3), 0.8f);
        stepT = run ? 0.40f : 0.70f;
    }
}

static void footwork(Boss *b, float dist, float dx, float dz, float dt, bool chase)
{
    if ((s.roamT -= dt) <= 0.0f) {
        uint32_t r = life_rng();
        s.roamT = 0.6f + (r % 80) / 100.0f;
        r >>= 7;
        int k = r % 100;
        // in reach it mostly circles, further out it mostly closes in; now and then it backs off
        if (dist < FK_MELEE_RANGE)         { s.roamF = k < 25 ? -0.7f : 0.0f; s.roamS = (k & 1) ? 1.0f : -1.0f; }        // in reach: it circles (and sometimes gives ground)
        else if (dist > 70.0f)             { s.roamF = 1.0f; s.roamS = ((r >> 7) % 100 < 45) ? ((k & 1) ? 0.5f : -0.5f) : 0.0f; }
        else                               { s.roamF = k < 15 ? -0.6f : 0.85f; s.roamS = (k & 1) ? 0.7f : -0.7f; }
        if (!chase) { s.roamF *= 0.5f; s.roamS *= 0.8f; }      // in a breather it only shifts about
    }
    float turn = face_player(b, dt, chase ? FK_TURN_RATE : 2.0f);
    if (fabsf(turn) > 0.9f && s.stepT <= 0.0f) { s.stepT = 0.45f; s.stepSg = turn > 0.0f ? 1.0f : -1.0f; }    // a big turn: it steps round
    s.stepT -= dt;
    float f = s.roamF, sd = s.roamS;
    // out of reach it always closes (circling at the edge, it never came on): the chase comes first
    if (dist > FK_MELEE_RANGE - 4.0f && f < 0.6f && !(s.roamF < -0.2f && dist < FK_MELEE_RANGE + 14.0f)) f = 0.85f;
    if (dist < FK_MELEE_RANGE - 6.0f && f > 0.0f) f = 0.0f;                  // well in reach: no closer
    if (dist > 150.0f) { f = 1.0f; sd *= 0.3f; }                             // too far: in
    float sp = (f > 0.6f && dist > 45.0f) ? FK_RUN_SPEED : FK_WALK_SPEED;
    float vx = 0.0f, vz = 0.0f;
    if (dist > 1e-3f) {
        float nx = dx / dist, nz = dz / dist;
        vx = (nx * f + nz * sd) * sp; vz = (nz * f - nx * sd) * sp;
    }
    if (s.stepT > 0.0f) {       // the turn's own steps: it walks the arc it is turning through
        play(FKA_WALK, false);
        if (dist > 1e-3f) move(b, (dz / dist) * s.stepSg * FK_WALK_SPEED * 0.7f, (-dx / dist) * s.stepSg * FK_WALK_SPEED * 0.7f, dt);
        footfalls(dt, false);
        return;
    }
    // the clip that matches where it is going (its own: forward, back, or a strafe)
    bool moving = fabsf(f) > 0.05f || fabsf(sd) > 0.05f;
    if (!moving) play(FKA_IDLE, false);
    else if (f < -0.2f) play(FKA_BACKSTEP, false);
    else if (fabsf(sd) > fabsf(f) * 1.2f) play(sd > 0.0f ? FKA_STRAFE_L : FKA_STRAFE_R, false);
    else play(sp >= FK_RUN_SPEED ? FKA_RUN : FKA_WALK, false);
    if (moving) { move(b, vx, vz, dt); footfalls(dt, sp >= FK_RUN_SPEED); }
}

void fallen_knight_update(Boss* b, float dt)
{
    if (!s.ready) return;
    if (b->damageFlashTimer > 0.0f) b->damageFlashTimer -= dt;
    s.t += dt;
    s.dtLast = dt;
    s.cdCleave -= dt; s.cdHalfmoon -= dt; s.cdMoonblade -= dt; s.cdRoll -= dt; s.cdSplitter -= dt; s.cdUpdown -= dt; s.cdTri -= dt; s.cdHoly -= dt; s.cdPlunge -= dt; s.cdLunge -= dt; s.cdBackLunge -= dt;
    s.bladeTgt = 0.0f;      // a move that holds the blade asks again each frame (hold_blade)

    float dx, dz, dist = to_player(b, &dx, &dz);

#ifdef HLE_FK_STAGGERME   // test aid: staggered every 8 s (its stagger and the sound with it)
    { static float t; if ((t += dt) >= 8.0f && fallen_knight_stagger(b)) { t = 0.0f; debugf("[fk] staggered\n"); } }
#endif
    if (b->state == BOSS_STATE_DEAD && s.state != FK_DEAD) {
        enter(FK_DEAD);
        play(FKA_DEATH, true);
        b->pos[1] = s.groundY;
    }


    // Half health: a war cry and a crimson burst (the fight's turn, like the others' breaks)
    if (!s.enraged && b->mode == BOSS_MODE_COMBAT && b->health > 0.0f && b->health <= b->maxHealth * 0.5f) {
        s.enraged = true;
        s.cryT = 0.0f;
        sfx(SCENE1_SFX_WAR_CRY, 1.0f);
        screen_shake_set_shake_mag(6.0f);
    }

    b->striking = s.striking || s.state == FK_ROLL;     // mid-swing (melee_check) or rolling off
    s.striking = false;
#ifdef HLE_TEST_DUMMY
    { extern void hle_open_tick(bool, float); hle_open_tick((s.state == FK_CHASE || s.state == FK_RECOVER), dt); }   // test aid: measure the openings
#endif
    // a Moon Blade cross's second beam, BEAM_GAP after its first (even once the leap has landed)
    if (s.beam2T >= 0.0f && (s.beam2T -= dt) < 0.0f) {
        skill_fx_spawn_slash_hostile(s.beam2O, s.beam2V, -BEAM_TILT, FK_FELT(8.0f), s.beam2Life);
        if (s.state == FK_MOONBLADE) { beam_swing(1); sfx(SCENE1_SFX_FW_SLICE, 1.0f); }      // its cut (not once it has landed)
    }
    if (s.state != FK_DEAD) fire_update(dt);
    switch (s.state) {
    case FK_INTRO:
        // Falls from above, the blade down its back, into a superhero landing that cracks
        // the floor; rises (HeroDrop), then fights
        b->pos[1] = s.groundY + FK_DROP_HEIGHT * fmaxf(0.0f, 1.0f - s.t / (FK_INTRO_TIME * 0.6f));
        face_player(b, dt, 10.0f);
        if (s.t >= 0.05f && s.t - dt < 0.05f) sfx_cam(SCENE1_SFX_BOSS_SWING4, 1.0f);    // the fall: its swell as it passes the camera
        // the Guardian's slam (the sound that goes with its floor crack), its onset 0.05 s in on the landing
        if (s.t >= FK_INTRO_TIME * 0.6f - 0.05f && s.t - dt < FK_INTRO_TIME * 0.6f - 0.05f) sfx_cam(SCENE1_SFX_BOSS_SMASH1, 1.0f);
        if (s.step == 0 && b->pos[1] <= s.groundY) {
            float p[3] = { b->pos[0], s.groundY, b->pos[2] };
            boss_ground_crush_spawn_sized(p[0], s.groundY + 0.25f, p[2], 1.0f);
            screen_shake_set_shake_mag(10.0f);
            s.step = 1; landT = 0.0f;
        }
        if (s.anim == FKA_HERODROP && clip_done()) { play(FKA_IDLE, true); s.blendT = 0.0f; }    // HeroDrop ends on Idle's first frame
        break;      // waits, landed, until the intro cutscene ends (fallen_knight_skip_intro)

    case FK_CHASE: {
        footwork(b, dist, dx, dz, dt, true);
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
        if (s.t > (dist < FK_MELEE_RANGE + 8.0f ? FK_REACH_PICK : 0.7f) + s.pickJit) choose(b, dist);     // in reach, it comes on sooner
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
                recover(FK_QUICK_REC, false);
            }
        }
        break;

    case FK_ROLL:
        move(b, s.moveX, s.moveZ, dt);
        if (s.t - dt < 0.39f && s.t >= 0.39f) sfx(SCENE1_SFX_FW_ROLL, 0.9f);     // as it comes down onto the floor (the knight's Roll clip)
        if (s.t > 0.55f) {
            if (s.counter) { enter(FK_ROLLCUT); play(FKA_ATTACK2, true); }
            else enter(FK_CHASE);
        }
        break;

    case FK_ROLLCUT:   rollcut_update(b, dt); break;
    case FK_PLUNGE:    plunge_update(b, dt);  break;
    case FK_LUNGE:     lunge_update(b, dt);   break;

    case FK_CLEAVE:    cleave_update(b, dt);   break;
    case FK_HALFMOON:  halfmoon_update(b, dt); break;
    case FK_MOONBLADE: moonblade_update(b, dt); break;
    case FK_SPLITTER:  splitter_update(b, dt); break;
    case FK_UPDOWN:    updown_update(b, dt);   break;
    case FK_TRISLAM:   trislam_update(b, dt);  break;
    case FK_HOLYSLAM:  holyslam_update(b, dt); break;

    case FK_STAGGER:
        // Down from the stagger: it gets up when the clip ends and the stagger is over (the kneel
        // keeps the stance's sword arm: the blade rides on the shoulder all the way down and up)
        // The clip is 3.5 s (BOSS_STAGGER_S): a longer stagger holds the kneel until the 0.8 s rise is due
        if (b->staggerT > KNEEL_RISE_S && clip_time() > t3d_anim_get_length(&s.anims[FKA_KNEEL]) - KNEEL_RISE_S - 0.1f)
            t3d_anim_set_time(&s.anims[FKA_KNEEL], t3d_anim_get_length(&s.anims[FKA_KNEEL]) - KNEEL_RISE_S - 0.1f);
        if (b->staggerT <= 0.0f && clip_done()) {
#ifdef HLE_AUTOPLAY
            { extern float gameTime; debugf("[stagger] fk up at %.1f s\n", (double)gameTime); }
#endif
            recover(FK_QUICK_REC, false);
        }
        break;

    case FK_RECOVER:
        // A breather after every move: the opening to hit back
        if (s.t <= s.stareT) play(FKA_IDLE, false);
        // spent: its sword point-down, then swung up round its right side onto the shoulder as it
        // comes on (let go straight, the hand's turn cut the blade through its body)
        if (s.heavy && s.t < s.recT - 0.8f) hold_blade(0.3f, 0.25f, -0.92f);
        else if (s.heavy && s.bladeArc < 3) {
            if (s.bladeArc == 0) s.bladeArc = 1;
            if (s.bladeArc == 1 && s.bladeCur[1] < -0.9f) s.bladeArc = 2;
            if (s.bladeArc == 1) hold_blade_turn(0.1f, -1.0f, 0.0f, 6.0f);
            else { s.bladeTgt = 1.0f; s.bladeTurn = 6.0f; }     // blade_pose aims it at the clip's own
        }
        if (s.t > s.stareT) footwork(b, dist, dx, dz, dt, false);      // it shifts its feet and squares up to you
        if (s.t > s.recT) enter(FK_CHASE);
        break;

    case FK_DEAD:
        break;
    }

    // Animate (with a short crossfade after clip changes)
    t3d_anim_update(&s.anims[s.anim], dt);
#ifdef HLE_POP_REF      // test aid: no crossfades (the clips' own motion, for HLE_LOG_POP to hold the changes to)
    s.blendT = 0.0f;
#endif
#ifndef FK_FIT_K
#define FK_FIT_K 0.06f      // the fade's length per radian its chain (waist to the hand that turns most) has to turn
#endif
#ifndef FK_FIT_MAX
#define FK_FIT_MAX 0.45f
#endif
    // A fade as long as the way it has to go: a big change of pose (the sword off its shoulder and
    // low before it) eased over longer, so it never lurches there and stops; one that flows on
    // (the combo's blows into each other) stays as quick as it was. Chosen on its first frame, the
    // new clip at its real start (some are started part way in); one set by hand is kept
    if (s.blendSet > 0.0f) {
        if (s.blendDur == s.blendSet && s.blendT == s.blendSet) {
            // (the sword arm's chain, or the other's: it comes onto the hilt in the two-handed moves)
            static int chain[8] = { -2 };
            static const char *CH[8] = { "Waist", "Spine1", "UpperArm-Right", "LowerArm-Right", "Hand-Right", "UpperArm-Left", "LowerArm-Left", "Hand-Left" };
            if (chain[0] == -2) for (int i = 0; i < 8; i++) chain[i] = t3d_skeleton_find_bone(&s.skel, CH[i]);
            float ang[8];
            for (int i = 0; i < 8; i++) {
                ang[i] = 0.0f;
                if (chain[i] < 0) continue;
                const T3DQuat *qa = &s.skelBlend.bones[chain[i]].rotation, *qb = &s.skel.bones[chain[i]].rotation;
                float d = fabsf(qa->v[0] * qb->v[0] + qa->v[1] * qb->v[1] + qa->v[2] * qb->v[2] + qa->v[3] * qb->v[3]);
                ang[i] = 2.0f * acosf(fminf(1.0f, d));
            }
            float turn = ang[0] + ang[1] + fmaxf(ang[2] + ang[3] + ang[4], ang[5] + ang[6] + ang[7]);
            float fit = fminf(FK_FIT_MAX, FK_FIT_K * turn);
#ifdef HLE_LOG_POP
            pop.turn = turn;
#endif
            if (fit > s.blendDur) s.blendT = s.blendDur = fit;
        }
        s.blendSet = 0.0f;
    }
    if (s.blendT > 0.0f) {
        fade_carry(dt);
        s.blendT -= dt;
        float k = 1.0f - fmaxf(0.0f, s.blendT) / s.blendDur;
        fade_blend(k * k * (3.0f - 2.0f * k));     // eased in and out
    }
    pose_record(dt);
    tired_pose(dt);
    life_update(b, dt);
    life_apply();
    blade_pose(dt);
    grip_pose(dt);
    sword_carry();
    t3d_skeleton_update(&s.skel);
#ifdef HLE_LOG_POP
    pop_sample(dt, b->rot[1]);
#endif
    grip_unpose();
    if (s.handKeep.parentIdx != 0xFFFF) { s.skel.bones[s.swordBone].rotation = s.handKeep.rotation; s.skel.bones[s.swordBone].hasChanged = true; }
    life_unpose();

#ifdef HLE_LOG_FWCLIP   // test aid: the closest the horns come to the pauldron and to the blade's line, posed (sample points; pauldron < ~0.05 glTF units: touching)
    if (s.headBone >= 0 && s.spineBone >= 0) {
        static int armBone = -2;
        if (armBone == -2) armBone = t3d_skeleton_find_bone(&s.skel, "UpperArm-Left");
        static float worst = 1e9f, worstS = 1e9f, wsu, wg[3]; static int n, wst, wi, wj, wsst, wsi;
        float h[FW_CLIP_NH][3], q[3], g[3], t[3], b0[3], b1[3];
        for (int i = 0; i < FW_CLIP_NH; i++) mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[s.headBone], (float[3]){ FW_CLIP_H[i][0] * 64.0f, FW_CLIP_H[i][1] * 64.0f, FW_CLIP_H[i][2] * 64.0f }, h[i]);
        for (int j = 0; j < FW_CLIP_NP; j++) {
            if (armBone < 0) break;
            mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[armBone], (float[3]){ FW_CLIP_P[j][0] * 64.0f, FW_CLIP_P[j][1] * 64.0f, FW_CLIP_P[j][2] * 64.0f }, q);
            for (int i = 0; i < FW_CLIP_NH; i++) {
                float d = sqrtf((q[0] - h[i][0]) * (q[0] - h[i][0]) + (q[1] - h[i][1]) * (q[1] - h[i][1]) + (q[2] - h[i][2]) * (q[2] - h[i][2])) / 64.0f;
                if (d < worst) { worst = d; wst = s.state; wi = i; wj = j; }
            }
        }
        if (s.swordBone >= 0) {     // the blade, guard to tip, in the same (model) space
            sword_seat((float[3]){ -29.0f, 46.0f, -3.0f }, g); sword_seat((float[3]){ -522.0f, 69.0f, -68.0f }, t);
            mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[s.swordBone], g, b0);
            mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[s.swordBone], t, b1);
            float e[3] = { b1[0] - b0[0], b1[1] - b0[1], b1[2] - b0[2] }, ee = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
            float o[3], ax[3][3], gl[3];     // the guard in the head bone's own axes (glTF units)
            mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[s.headBone], (float[3]){ 0, 0, 0 }, o);
            for (int a = 0; a < 3; a++) { float u[3] = { 0, 0, 0 }; u[a] = 64.0f; mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[s.headBone], u, ax[a]); for (int k = 0; k < 3; k++) ax[a][k] -= o[k]; }
            for (int a = 0; a < 3; a++) gl[a] = ((b0[0] - o[0]) * ax[a][0] + (b0[1] - o[1]) * ax[a][1] + (b0[2] - o[2]) * ax[a][2]) / (64.0f * 64.0f);
            for (int i = 0; i < FW_CLIP_NH; i++) {
                float u = fmaxf(0.0f, fminf(1.0f, ((h[i][0] - b0[0]) * e[0] + (h[i][1] - b0[1]) * e[1] + (h[i][2] - b0[2]) * e[2]) / ee)), d2 = 0.0f;
                for (int k = 0; k < 3; k++) { float c = h[i][k] - b0[k] - e[k] * u; d2 += c * c; }
                float d = sqrtf(d2) / 64.0f;
                if (d < worstS) { worstS = d; wsst = s.state; wsi = i; wsu = u; memcpy(wg, gl, sizeof wg); }
            }
        }
#ifdef HLE_LOG_FWPOSE   // the blade (guard, tip), the chest's and the upper arm's frames, in the head bone's space: horn shapes scored offline
        if (s.swordBone >= 0 && armBone >= 0 && (n & 3) == 0) {
            float o[3], ax[3][3], r[2][3], f[2][4][3];
            mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[s.headBone], (float[3]){ 0, 0, 0 }, o);
            for (int a = 0; a < 3; a++) { float u[3] = { 0, 0, 0 }; u[a] = 64.0f; mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[s.headBone], u, ax[a]); for (int k = 0; k < 3; k++) ax[a][k] -= o[k]; }
            #define TOHEAD(src, dst) for (int a_ = 0; a_ < 3; a_++) dst[a_] = ((src[0] - o[0]) * ax[a_][0] + (src[1] - o[1]) * ax[a_][1] + (src[2] - o[2]) * ax[a_][2]) / (64.0f * 64.0f)
            TOHEAD(b0, r[0]); TOHEAD(b1, r[1]);
            int bones[2] = { s.spineBone, armBone };
            for (int bi = 0; bi < 2; bi++)
                for (int a = 0; a < 4; a++) {     // the bone's origin, then where its unit axes land
                    float u[3] = { 0, 0, 0 }, w[3]; if (a) u[a - 1] = 64.0f;
                    mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[bones[bi]], u, w);
                    TOHEAD(w, f[bi][a]);
                }
            #undef TOHEAD
            char line[400]; int len = snprintf(line, sizeof line, "[fwpose] %d %.3f %.3f %.3f %.3f %.3f %.3f", s.state, (double)r[0][0], (double)r[0][1], (double)r[0][2], (double)r[1][0], (double)r[1][1], (double)r[1][2]);
            for (int bi = 0; bi < 2; bi++) for (int a = 0; a < 4; a++) len += snprintf(line + len, sizeof line - len, " %.3f %.3f %.3f", (double)f[bi][a][0], (double)f[bi][a][1], (double)f[bi][a][2]);
            debugf("%s\n", line);
        }
#endif
        if (++n % 120 == 0) { debugf("[fwclip] closest %.3f (state %d) horn %d pauldron %d sword %.3f (state %d) horn %d at %.2f along the blade, guard at %.2f %.2f %.2f (head)\n", (double)worst, wst, wi, wj, (double)worstS, wsst, wsi, (double)wsu, (double)wg[0], (double)wg[1], (double)wg[2]); worst = worstS = 1e9f; }
    }
#endif
#ifdef HLE_LOG_GRIP     // test aid: the wrist: the angle between the forearm (elbow to wrist) and the blade, per move
    {   // (a straight-armed grip, the handle along the forearm: ~13 degrees). Its range over each move, and while held (blade_pose)
        static int fore = -2, st = -1, stp = -1; static float lo, hi, hiHeld, atHi;
        if (fore == -2) fore = t3d_skeleton_find_bone(&s.skel, "LowerArm-Right");
        float e[3], w[3], base[3], tip[3];
        if (fore >= 0 && s.swordBone >= 0 && sword_segment(base, tip)) {
            float m0[3], m1[3];
            mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[fore], (float[3]){ 0, 0, 0 }, m0);
            mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[s.swordBone], (float[3]){ 0, 0, 0 }, m1);
            mat4fp_mul_point_f32_row3_colbasis(s.mat, m0, e); mat4fp_mul_point_f32_row3_colbasis(s.mat, m1, w);
            float a[3] = { w[0] - e[0], w[1] - e[1], w[2] - e[2] }, c[3] = { tip[0] - base[0], tip[1] - base[1], tip[2] - base[2] };
            vnorm(a); vnorm(c);
            float ang = acosf(fmaxf(-1.0f, fminf(1.0f, a[0] * c[0] + a[1] * c[1] + a[2] * c[2]))) * 57.3f;
            if (s.state != st || s.step != stp) {
                if (st >= 0) debugf("[grip] state %d step %d: wrist %.0f..%.0f deg (worst at clip %.2f), held %.0f\n", st, stp, (double)lo, (double)hi, (double)atHi, (double)hiHeld);
                st = s.state; stp = s.step; lo = 1e9f; hi = -1.0f; hiHeld = -1.0f;
            }
            if (ang < lo) lo = ang;
            if (ang > hi) { hi = ang; atHi = clip_time(); }
            if (s.bladeW > 0.5f && ang > hiHeld) hiHeld = ang;
        }
    }
#endif
#ifdef HLE_LOG_LOOK     // test aid: how far the head and the body each turn from the player (rad)
    if (s.headBone >= 0 && s.mat) {
        static int n;
        if (++n % 20 == 0) {
            float h0[3], h1[3], w0[3], w1[3];
            mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[s.headBone], (float[3]){ 0, 0, 0 }, h0);
            mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[s.headBone], (float[3]){ -64.0f, 0, 0 }, h1);     // the model faces -X
            mat4fp_mul_point_f32_row3_colbasis(s.mat, h0, w0); mat4fp_mul_point_f32_row3_colbasis(s.mat, h1, w1);
            float hx = w1[0] - w0[0], hz = w1[2] - w0[2], px = character.pos[0] - w0[0], pz = character.pos[2] - w0[2];
            float head = remainderf(atan2f(-hx, hz) - atan2f(-px, pz), 2.0f * T3D_PI), body = remainderf(b->rot[1] - atan2f(-px, pz), 2.0f * T3D_PI);
            debugf("[look] st %d head off %.2f body off %.2f lookYaw %.2f w %.2f\n", s.state, (double)head, (double)body, (double)s.lookYaw, (double)s.lookW);
        }
    }
#endif
#ifdef HLE_LOG_FWPATH   // test aid: where it stands relative to the player (its bearing round them, and how far)
    { static int n; if (++n % 10 == 0) { float px = b->pos[0] - character.pos[0], pz = b->pos[2] - character.pos[2];
        debugf("[fwpath] %.3f %.1f st %d clip %d\n", (double)atan2f(px, pz), (double)sqrtf(px * px + pz * pz), s.state, s.anim); } }
#endif
    float p[3] = { b->pos[0], b->pos[1], b->pos[2] };
    const float sc[3] = { FK_SCALE, FK_SCALE, FK_SCALE };
    const float rot[3] = { 0.0f, b->rot[1] + FK_YAW_OFFSET, 0.0f };
    t3d_mat4fp_from_srt_euler(s.mat, sc, rot, p);
#ifdef HLE_FW_CLOSECAM   // test aid: the camera close round its head and shoulders (camera_controller asks)
    if (s.headBone >= 0) {
        float h[3];
        mat4fp_mul_point_f32_row3_colbasis(&s.skel.boneMatricesFP[s.headBone], (float[3]){ 0.0f, 0.0f, 0.0f }, h);
        mat4fp_mul_point_f32_row3_colbasis(s.mat, h, fw_closecam_head);
        float o[3], f[3];       // its facing (the knight faces -x in its model): HLE_FW_CLOSECAM_A 0 is straight in front
        mat4fp_mul_point_f32_row3_colbasis(s.mat, (float[3]){ 0.0f, 0.0f, 0.0f }, o);
        mat4fp_mul_point_f32_row3_colbasis(s.mat, (float[3]){ -64.0f, 0.0f, 0.0f }, f);
        fw_closecam_yaw = atan2f(f[0] - o[0], f[2] - o[2]);
    }
#endif
    {   // The body capsule follows it down: feet to head while it kneels staggered, so it's hit where it is
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
static bool fk_parts(void *user, const T3DObject *obj)     // the plume off: its horns stand in for it
{
    (void)user;
    return !(obj->material && obj->material->name && !strcmp(obj->material->name, "cloth2"));
}

void fallen_knight_draw(Boss* b)
{
    if (!s.ready || !b->visible) return;
    // A fallen champion: the knight reshaped (a horned, beaked helm, broader), in dark crimson with
    // a dark blade, under the hall's light like the player (a hit flashes it red). Its moves are
    // told apart by its poses alone (no glows)
    bool flash = b->damageFlashTimer > 0.0f;
    uint8_t amb[4] = { colorAmbient[0], colorAmbient[1], colorAmbient[2], 255 };
    t3d_light_set_ambient(flash ? (uint8_t[4]){ 255, 120, 120, 255 } : amb);
    static const char *MATS[] = { "knight", "head", "chainmail", "cloth", "straps", "sword", "black", "lames", "dome" };
    static const color_t COLS[] = { RGBA32(100, 27, 34, 255), RGBA32(100, 27, 34, 255), RGBA32(128, 40, 48, 255), RGBA32(56, 18, 22, 255),
                                    RGBA32(78, 22, 27, 255), RGBA32(84, 80, 82, 255), RGBA32(20, 16, 18, 255),
                                    RGBA32(84, 40, 32, 255), RGBA32(70, 16, 20, 255) };     // (the pauldron: its lames dark brownish red, its dome dark red)
    static bool tinted;     // (its own model: its colours set once)
    if (!tinted) {
        for (int i = 0; i < (int)(sizeof MATS / sizeof MATS[0]); i++) { T3DMaterial *m = t3d_model_get_material(s.body, MATS[i]); if (m) m->primColor = COLS[i]; }
        tinted = true;
    }
    t3d_matrix_set(gpu_mat(s.mat), true);
    T3DMat4FP *bones = gpu_skeleton_begin(&s.skel);
    t3d_model_draw_custom(s.body, (T3DModelDrawConf){ .matrices = gpu_skeleton_matrices(), .filterCb = fk_parts });
    gpu_skeleton_end(&s.skel, bones);
    t3d_light_set_ambient((uint8_t[4]){ 255, 255, 255, 255 });
}
