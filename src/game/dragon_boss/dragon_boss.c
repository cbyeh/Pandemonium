#include <libdragon.h>
#include "gpu_frame.h"
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>
#include <t3d/t3dmodel.h>
#include <t3d/t3dskeleton.h>
#include <math.h>
#include <string.h>

#include "dragon_boss.h"
#include "shadowborne_bind.h"     // tools/shadowborne_model.py
#include "character.h"
#include "globals.h"
#include "general_utility.h"
#include "game_time.h"
#include "fx/proc_mesh.h"
#include "fx/skill_fx.h"
#include "fx/screen_shake.h"
#include "../boss/environmental_mechanics/boss_ground_crush.h"
#include "controllers/audio_controller.h"
#include "../../scenes/guardian/guardian_scene_sfx.h"
#include "../../scenes/guardian/guardian_scene_bounds.h"

// ---- Body (world units; the player is about 30 tall) ----
#define BODY_H     30.0f        // spine height above the floor
#define CHEST_Z    26.0f        // the chest, ahead of the root (b->pos)
#define HIPS_Z    -32.0f
#define NNECK      4
#define NECK_SEG   15.0f
#define NTAIL      7
#define TAIL_SEG   17.0f
#define LEG_L      19.0f        // each leg bone
#define MESH       16.0f        // the segment and limb meshes are built 16 across, 16 long
#define SB_SCALE   64.0f        // the skinned body's model units a world unit (the importer's base scale)
#define WING_REST_FOLD 0.35f    // how far the wings fold held in (the old wings stayed open, only smaller)
#define WING_FOLDED 0.45f       // the wings' size folded, to their full size spread (they grow as they open)
#define WING_K     2.3f         // the great wings, this much bigger than the old pair (tools/shadowborne_rig.py)
#define HIT_R      20.0f        // the hit capsule: wide enough for its legs at sword height (17 missed their outer 4)
#define DROP_H     160.0f       // flies down from this high

#define FL_MAX   40             // flames
#define ORB_MAX  8
#define BL_MAX   14             // the dark beam's delayed explosions
#define BALL_MAX 4              // the triple breath's fireballs
#define MAT_MAX  (34 + FL_MAX + ORB_MAX + 2 * BALL_MAX + 6)
#define MAT_RING 3              // frames the RSP may still be reading

// A little brighter than they read: the scale and membrane textures multiply them (mean ~0.75)
#define COL_BACK   proc_rgb(58, 52, 68)
#define COL_BELLY  proc_rgb(104, 92, 98)
#define COL_BONE   proc_rgb(150, 140, 146)
#define COL_MEMB   proc_rgb(52, 42, 62)
#define COL_IVORY  proc_rgb(232, 226, 210)
#define MOUTH_TOP  proc_rgb(84, 36, 40)     // the mouth's flesh (untextured)
#define MOUTH_JAW  proc_rgb(130, 48, 52)

typedef enum { DG_INTRO, DG_IDLE, DG_CLAW, DG_BITE, DG_TAIL, DG_BREATH, DG_CHARGE, DG_FLY,
               DG_ROAR, DG_NOVA, DG_ORBS, DG_BEAM, DG_DEAD = 13, DG_WHIP, DG_DOWN,
               DG_TURN, DG_TRIPLE, DG_SNIPE, DG_SKYBEAM, DG_SWEEP, DG_CONE, DG_HOVER,
               DG_BACKSTEP, DG_SWIPE, DG_STOMP } DgState;     // (12 was the beam's spent state; the numbers kept for HLE_DRAGON_FORCE)

typedef struct { float p[3], from[3], t; bool moving; } Foot;
typedef struct { float p[3], v[3], age, life, size; bool harm, ember; } Flame;     // ember: a fireball's burning patch (a lighter tick)
typedef struct { float p[3], v[3], age; bool on; } Orb;
typedef struct { float p[2], t; bool on; } Blast;
typedef struct { float p[3], v[3]; bool on; } Ball;

static struct {
    bool ready;
    // Rigid parts on the skinned body: head, jaw, throat, feet
    ProcMesh mThroat, mFoot, mHead, mHeadDark, mJaw, flame, mOrb, mIce, mCone;      // (the crystals, the tail's blade: the skinned body's own)
    // The skinned body (tools/shadowborne_model.py): each bone's frame rebuilt from the rig every
    // frame. bindK: the bone's bind rotation in the frame the rig builds for it (F_bind^T R_bind)
    T3DModel *model;
    float bindK[SB_BONES][9];
    int boneT[SB_BONES];                                // each bind entry's bone in the model
    float knee[4][3];                                   // the leg joints (solved in the rig)
    float wj[2][13][3], wup[2][3];                      // each great wing: root, elbow, wrist, five knuckles, five fingertips; its upper side
    float flapOn;                                       // eased in and out of flapping (the wing's joints ripple with the beat)
    float wingG;                                        // the wings' size: they grow as they spread (WING_FOLDED folded)
    rspq_block_t *block, *blockUnlit;                   // the body's draw (lit), the membranes' and crystals' (unlit), recorded once: the bones' matrices set each frame
    float wingFold, tWingFold;                          // 1 folded along its back .. 0 spread
    sprite_t *texScales, *texMemb;      // tools/boss_textures.py
    T3DMat4FP *mats;
    int matRing, mi;
    DgState state;
    float t, life;
    int step;
    float groundY, alt;
    bool dark;                  // the second half: the dark has taken it
    // The pose, eased each frame toward the targets the current move sets
    float neckPitch, neckYaw, neckReach, jaw, wingRaise, wingSpan, tail, crouch, claw[2], aimK, rear;
    float tailUp, tTailUp;  // the tail curled up over its back (0: trailing; the Whip's tell)
    float tNeckPitch, tNeckYaw, tNeckReach, tJaw, tWingRaise, tWingSpan, tTail, tCrouch, tClaw[2], tAimK, tRear;
    float poseRate, flapRate, flapPh;
    // What makes it move like a living thing (rig): `roll` lifts its right side (rad), `shLift` a front
    // shoulder, `bend` curves the spine sideways, `lean` throws the chest forward; the wings `sweep`
    // back (rad), and `clawHi` lifts a raised paw higher (the heavy stomp)
    float roll, tRoll, shLift[2], tShLift[2], bend, tBend, lean, tLean, wingSweep, tWingSweep, clawHi, tClawHi;
    float yawPrev, yawRate, altPrev, altVel;    // how fast it turns and climbs: the tail, wings and spine answer it
    float tailLag, tailV, wingKick;             // the tail's sprung follow-through; the wings' flare on landing
    float swing[4];                             // each foot's swing (0 planted .. 1 at its height): the gait's bob and roll
    float flapAmt;                              // eased in and out of flapping (the beat no longer snaps on or off)
    int swipeK; float swipeAmt, tSwipeAmt, swipePh;     // the swipe charge's foreleg: cocked out wide, then raked across
    // The rig's results for drawing: the roll as posed (the gait's and a bank's added), the spine's middle, its up
    float rollNow, spine[2][3], bodyUp[3];
    float aim[3];               // where the head looks when aimK > 0
    // The rig, rebuilt each frame
    float F[3], R[3], chest[3], hips[3], neck[NNECK + 1][3], tl[NTAIL + 1][3];
    float head[3], hx[3], hy[3], hz[3], mouth[3], shoulder[4][3], foot[4][3];
    Foot feet[4];
    bool feetReady;
    // Moves
    float cdClaw, cdBite, cdTail, cdBreath, cdCharge, cdFly, cdNova, cdOrbs, cdBeam, cdWhip, cdTriple, cdSnipe, cdSweep, cdCone, cdBackstep, cdSwipe;
    float passT;                // just went past the player (charge, swipe, flight): the tail is ready for one behind it
    bool passTail;              // ...and (most of the time) it uses it rather than turning round
    float paceSg;               // which way it first paces in a pause
    float bsDist;               // the backstep's length: as far as the room behind it allows
    // Little life: a flinch at each blow taken, a shake-off after a landing or getting up, fidgets in
    // a pause (a tail thump, a paw scrape), a growl as it paces, the head following the player a beat late
    float prevHealth, shakeT, shakeNow, fidgetT, lookRel;
    int fidgetK;
    bool growled;
    bool followRanged;          // the backstep leads into a ranged attack
    int sweeps;                 // the ground beam's sweeps still to come
    bool heavyRest;             // the next pause a spent one (after a landing or three beam sweeps)
    float idleWait;
    float sweepFrom, sweepSpan, range, sg, target[3], beamFrom[3], flyTo[2], emit, fireCd, glow, beamW;
    bool hitDone;
    Flame fl[FL_MAX];
    int flNext;
    Orb orb[ORB_MAX];
    int orbNext;
    Blast bl[BL_MAX];
    int blNext;
    Ball ball[BALL_MAX];
    int ballNext;
    float beamP[3];
    bool beamOn;
    struct { float p[2], yaw, t; bool on, hit; } cone;     // a fire cone on the floor (DG_CONE)
} s;
static float tailPrev[NTAIL + 2][3];    // the tail's points and the blade's tip last frame (tail_sweep_hits)
static bool tailPrevOk;

static inline float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
static float smooth01(float u) { u = clamp01(u); return u * u * (3.0f - 2.0f * u); }
static inline float ease(float cur, float to, float rate, float dt) { return cur + (to - cur) * fminf(1.0f, rate * dt); }
// Its own random numbers (xorshift32, seeded from the clock), for every roll it makes: rand_custom_u32 is a
// 16-bit LCG the game steps in step with the frame, and its draws here came out far from uniform
// (the first candidates in a list, the backstep and the swipe charge, got 2 in 100 picks, not 1 in 4)
static uint32_t pick_rng(void)
{
    static uint32_t x;
    if (!x) x = (uint32_t)TICKS_READ() | 1u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}
static inline float frand(void) { return (pick_rng() % 2001) / 1000.0f - 1.0f; }     // -1..1
static inline float len3(const float a[3]) { return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
static inline void mad3(float o[3], const float a[3], const float d[3], float k) { for (int i = 0; i < 3; i++) o[i] = a[i] + d[i] * k; }
static inline void lerp3(float o[3], const float a[3], const float b[3], float t) { for (int i = 0; i < 3; i++) o[i] = a[i] + (b[i] - a[i]) * t; }
static inline void norm3(float a[3]) { float l = len3(a); if (l > 1e-5f) for (int i = 0; i < 3; i++) a[i] /= l; }
static inline void cross3(float o[3], const float a[3], const float b[3])
{
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
static const float UP[3] = { 0.0f, 1.0f, 0.0f };

static float to_player(const Boss *b, float *dx, float *dz)
{
    *dx = character.pos[0] - b->pos[0];
    *dz = character.pos[2] - b->pos[2];
    return sqrtf(*dx * *dx + *dz * *dz);
}

static float turn_toward(Boss *b, float yaw, float rate, float dt)
{
    float d = remainderf(yaw - b->rot[1], 2.0f * T3D_PI);
    float m = rate * dt;
    b->rot[1] += (d > m) ? m : (d < -m) ? -m : d;
    return d;
}

static void face_player(Boss *b, float dt, float rate)
{
    float dx, dz;
    to_player(b, &dx, &dz);
    turn_toward(b, atan2f(dx, dz), rate, dt);
}

static bool room_ok(float x, float z) { return scene_bounds_check_room(x, s.groundY, z); }

// Inside the hall's walls by `m` (scene_bounds_check_room is a loose box that reaches past
// the west wall at -430, and this body is long)
static bool inside(float x, float z, float m)
{
    return room_ok(x, z) && x > -420.0f + m && x < 400.0f - m && fabsf(z) < 585.0f - m;
}

// Moves the root if the whole body (head end to tail end) stays in the hall
static bool move_by(Boss *b, float dx, float dz)
{
    float x = b->pos[0] + dx, z = b->pos[2] + dz, fx = fm_sinf(b->rot[1]), fz = fm_cosf(b->rot[1]);
    if (!inside(x, z, 40.0f) || !inside(x + fx * 70.0f, z + fz * 70.0f, 5.0f) || !inside(x - fx * 80.0f, z - fz * 80.0f, 5.0f)) return false;
    b->pos[0] = x; b->pos[2] = z;
    return true;
}

#ifdef HLE_LOG_HURTBOX
static void hb_move_end(void);
#endif
static void enter(DgState st)
{
#ifdef HLE_LOG_MOVES     // test aid: each move as it starts (the hits after it are its: [hit])
    debugf("[move] %d\n", st);
#endif
#ifdef HLE_LOG_HURTBOX
    hb_move_end();
#endif
    s.state = st; s.t = 0.0f; s.step = 0; s.hitDone = false; tailPrevOk = false;
    if (st != DG_IDLE) s.heavyRest = false;
    if (st == DG_TAIL) s.passT = 0.0f;     // one tail per pass
    // Between attacks a pause; now and then (35%) a longer breather, its head low: an opening
#ifndef HLE_DG_PAUSE
#define HLE_DG_PAUSE 0.5f   // the recovery added to every pause (a longer opening: about 70% cost, not ~80%)
#endif
    if (st == DG_IDLE) { s.paceSg = (pick_rng() & 1) ? 1.0f : -1.0f; s.growled = false; s.fidgetT = 0.0f; }
    if (st == DG_IDLE) s.idleWait = ((pick_rng() % 100 < 35) ? 3.3f + (pick_rng() % 100) / 100.0f : 1.8f) + HLE_DG_PAUSE + (s.dark ? 0.0f : 0.45f);     // (the light half pauses longer; the dark half presses on)
#ifdef HLE_AUTOPLAY
    debugf("[dragon] state %d\n", st);
#endif
}

// Gone past the player: for a moment the tail answers one behind it, most of the time (75%;
// always when they face the tail) before it would turn round
static void passed_player(void)
{
    s.passT = 3.0f;
    s.passTail = pick_rng() % 100 < 75;
}

bool dragon_boss_stagger(Boss* b)
{
    (void)b;
    // Not in the air, not already spent
    if (!s.ready || s.state == DG_INTRO || s.state == DG_FLY || s.state == DG_DEAD || s.alt > 1.0f) return false;
    s.beamOn = false; s.glow = 0.0f; s.cone.on = false;
    enter(DG_DOWN);     // its legs give: a lurch, then down on its belly, head lolling (dg_down)
    b->staggerT = BOSS_STAGGER_S;
    return true;
}

// The player behind it and facing its tail (looking along the tail toward the hips)
static bool player_faces_tail(void)
{
    float fx = -fm_sinf(character.rot[1]), fz = fm_cosf(character.rot[1]);     // the player's forward
    float tx = s.tl[3][0] - character.pos[0], tz = s.tl[3][2] - character.pos[2], l = sqrtf(tx * tx + tz * tz);
    return l > 1.0f && (fx * tx + fz * tz) / l > 0.7f;
}

static void player_chest(float o[3]) { o[0] = character.pos[0]; o[1] = character.pos[1] + 12.0f; o[2] = character.pos[2]; }

// ---- Contact with the player ----
// The player's body is its capsule: an axis from feet + 4 to feet + 16, 8 round. A blow lands when
// what is drawn comes within PLAYER_R of that axis (2 short of touching: a little grace)
#define PLAYER_R 6.0f
static float axis_d2(const float q[3])
{
    float y0 = character.pos[1] + 4.0f, y1 = character.pos[1] + 16.0f;
    float dx = q[0] - character.pos[0], dz = q[2] - character.pos[2], dy = q[1] < y0 ? y0 - q[1] : q[1] > y1 ? q[1] - y1 : 0.0f;
    return dx * dx + dy * dy + dz * dz;
}
// From the segment a-b to the player's axis (the distance is convex along a-b: a ternary search)
static float axis_dist_seg(const float a[3], const float b[3])
{
    float lo = 0.0f, hi = 1.0f, q[3];
    for (int i = 0; i < 16; i++) {
        float m1 = lo + (hi - lo) * (1.0f / 3.0f), m2 = hi - (hi - lo) * (1.0f / 3.0f), q2[3];
        lerp3(q, a, b, m1); lerp3(q2, a, b, m2);
        if (axis_d2(q) < axis_d2(q2)) hi = m2; else lo = m1;
    }
    lerp3(q, a, b, (lo + hi) * 0.5f);
    return sqrtf(axis_d2(q));
}
static float axis_dist_pt(const float p[3]) { return sqrtf(axis_d2(p)); }

// What is drawn, as a distance from the player's axis less the drawn shape's own radius
static float tail_r(int i) { return (14.5f - i * 1.8f) * 0.9f; }     // a tail link's tube (put_link r, tapering to 0.82)
static void tail_tip(float e[3])        // the end of the crystal blade (dragon_boss_draw)
{
    float d[3] = { s.tl[NTAIL][0] - s.tl[NTAIL - 1][0], s.tl[NTAIL][1] - s.tl[NTAIL - 1][1], s.tl[NTAIL][2] - s.tl[NTAIL - 1][2] };
    norm3(d);
    mad3(e, s.tl[NTAIL], d, 16.0f);
}
static float tail_gap(void)
{
    float g = 1e9f, e[3];
    for (int i = 0; i < NTAIL; i++) g = fminf(g, axis_dist_seg(s.tl[i], s.tl[i + 1]) - tail_r(i));
    tail_tip(e);
    return fminf(g, axis_dist_seg(s.tl[NTAIL], e) - 3.4f);
}
static float head_gap(void)             // the skull, about 6 round, 26 long
{
    float sn[3];
    mad3(sn, s.head, s.hz, 26.0f);
    return axis_dist_seg(s.head, sn) - 6.0f;
}
static float body_gap(void)             // the torso (18 round), the neck, the head and the legs
{
    float g = fminf(fminf(axis_dist_seg(s.chest, s.spine[0]), fminf(axis_dist_seg(s.spine[0], s.spine[1]), axis_dist_seg(s.spine[1], s.hips))) - 17.0f, head_gap());
    for (int i = 0; i < NNECK; i++) g = fminf(g, axis_dist_seg(s.neck[i], s.neck[i + 1]) - (11.0f - i * 1.2f) * 0.9f);
    for (int k = 0; k < 4; k++) g = fminf(g, axis_dist_seg(s.shoulder[k], s.foot[k]) - 6.0f);
    return g;
}
// A rushing body shoves the player out to its hit capsule (HIT_R round the line under the spine)
// before what is drawn can reach them: shoved is touched (the charge never landed)
#define SHOVE_R (PLAYER_R + 8.0f)     // (28 out from the capsule's axis is ~12.4 off the torso)

static float flame_r(float size, float age, float life)    // as dragon_boss_draw sizes it (the octahedron, ~0.8 of its points' reach)
{
    float u = age / life;
    return 0.8f * size * (0.4f + 0.6f * clamp01(u * 2.0f)) * (1.0f - u * u);
}

#ifdef HLE_LOG_HURTBOX    // test aid: each blow's gap between what is drawn and the player's body (8 round; < 0: touching)
enum { HB_BITE, HB_TAIL, HB_WHIP, HB_CHARGE, HB_LAND, HB_FLAME, HB_ORB, HB_BLAST, HB_BALL, HB_BALLBURST, HB_BEAM, HB_STREAM, HB_CONE, HB_N };
static const char *const hbName[HB_N] = { "bite", "tail", "whip", "charge", "land", "flame", "orb", "blast", "ball", "ballburst", "beam", "stream", "cone" };
static float hbMin[HB_N] = { [0 ... HB_N - 1] = 1e9f };
static int hbHits[HB_N];
static void hb_hit(int src, float gap) { debugf("[hb] hit %s gap %.1f state %d t %.2f\n", hbName[src], (double)gap, s.state, (double)s.t); hbHits[src]++; }
static void hb_touch(int src, float gap) { if (gap < hbMin[src]) hbMin[src] = gap; }     // a frame its blow could land
static void hb_move_end(void)     // the move's closest pass of each blow it could deal, and the hits
{
    for (int i = 0; i < HB_N; i++) {
        if (hbMin[i] < 1e8f) debugf("[hb] move %d %s closest %.1f hits %d%s\n", s.state, hbName[i], (double)hbMin[i], hbHits[i], hbMin[i] < PLAYER_R - 8.0f - 0.5f && !hbHits[i] ? " TOUCHED NOT HIT" : "");
        hbMin[i] = 1e9f; hbHits[i] = 0;
    }
}
#define HB_HIT(src, gap) hb_hit(src, gap)
#define HB_TOUCH(src, gap) hb_touch(src, gap)
#else
#define HB_HIT(src, gap) ((void)sizeof(gap))       // (not evaluated)
#define HB_TOUCH(src, gap) ((void)sizeof(gap))
#endif

// At the Guardian's level and with his falloff (boss_play_attack_sfx: the distance to the player);
// they played at a fixed 0.85 with none, as his at melee range but up to 3x his further off
static void sfx(int id, float vol)
{
    Boss *b = boss_get_instance();
    float dx = b ? character.pos[0] - b->pos[0] : 0.0f, dz = b ? character.pos[2] - b->pos[2] : 0.0f;
    audio_play_scene_sfx_dist(id, vol * BONUS_SFX_VOL, sqrtf(dx * dx + dz * dz));
}
static void sfx_boom(void) { sfx(SCENE1_SFX_BOSS_SMASH2, 1.0f); }
static void sfx_swing(void) { sfx(SCENE1_SFX_BOSS_SWING3, 1.0f); }
static void sfx_lunge(void) { sfx(SCENE1_SFX_BOSS_LUNGE, 1.0f); }
static void sfx_roar(void)
{
#ifdef HLE_AUTOPLAY
    debugf("[dragon] roar\n");
#endif
    sfx(SCENE1_SFX_DRAGON_ROAR, 1.0f);
}
static void sfx_step(void) { sfx(SCENE1_SFX_BOSS_STEP1, 1.0f); }      // the Guardian's footfall, at his level

// A fire cone (DG_CONE): from a point on the floor out along `yaw`, CONE_L long, CONE_HALF each side;
// it rolls out over CONE_GROW, burns CONE_HOLD, then dies down over CONE_FADE (harmless). Its height
// at a share `f` of its length and `a` round from its middle (the mesh is built from the same)
#define CONE_L 125.0f
#define CONE_HALF 0.5f
#define CONE_GROW 0.2f
#define CONE_HOLD 0.1f
#define CONE_FADE 0.22f
static const float CONE_F[4] = { 0.0f, 0.3f, 0.65f, 1.0f }, CONE_H[4] = { 3.0f, 12.0f, 18.0f, 10.0f };
static float cone_h(float f, float a)
{
    if (f < 0.0f || f > 1.0f) return 0.0f;
    int i = f < CONE_F[1] ? 0 : f < CONE_F[2] ? 1 : 2;
    float u = (f - CONE_F[i]) / (CONE_F[i + 1] - CONE_F[i]), q = a / CONE_HALF;
    return (CONE_H[i] + (CONE_H[i + 1] - CONE_H[i]) * u) * (1.0f - 0.6f * fminf(q * q, 1.0f));     // lower to its sides
}
// ---- Meshes ----

// A crystal: an unlit four-sided spike (it glows), faceted light and dark
static void crystal(const float base[3], const float tip[3], float r, uint32_t col)
{
    float d[3] = { tip[0] - base[0], tip[1] - base[1], tip[2] - base[2] };
    norm3(d);
    float u[3] = { 0, 1, 0 };
    if (fabsf(d[1]) > 0.9f) { u[0] = 1; u[1] = 0; }
    float a[3], c[3];
    cross3(a, d, u); norm3(a);
    cross3(c, d, a);
    float ring[4][3];
    for (int i = 0; i < 4; i++) {
        float ca = (i == 0) - (i == 2), sa = (i == 1) - (i == 3);
        for (int m = 0; m < 3; m++) ring[i][m] = base[m] + (a[m] * ca + c[m] * sa) * r;
    }
    uint32_t dim = ((((col >> 24) * 3 / 5) & 0xFF) << 24) | (((((col >> 16) & 0xFF) * 3 / 5) & 0xFF) << 16)
                 | (((((col >> 8) & 0xFF) * 3 / 5) & 0xFF) << 8) | 0xFF;
    for (int i = 0; i < 4; i++) proc_mesh_tri(ring[i], ring[(i + 1) & 3], tip, (i & 1) ? dim : col, true);
}

static uint32_t crystal_col(bool dark) { return dark ? proc_rgb(200, 96, 255) : proc_rgb(150, 178, 240); }

// An elliptical ring of `n` points across the z plane, its underside flattened (a jaw line)
static void ring(float *out, int n, float z, float w, float h, float yc, float flatBelow)
{
    for (int i = 0; i < n; i++) {
        float a = 2.0f * T3D_PI * i / n;
        float y = yc + fm_sinf(a) * h;
        if (y < yc - flatBelow) y = yc - flatBelow;
        out[i * 3 + 0] = fm_cosf(a) * w; out[i * 3 + 1] = y; out[i * 3 + 2] = z;
    }
}

// The scale map (tools/boss_textures.py) wraps once round a tube: 128 pixels round it (and
// repeats every 32 down it). The UV densities below keep its texels about square on each mesh
#define SCALES_AROUND 128.0f

// The throat: a smooth six-sided tube along +z, rounded and closed at both ends
static void build_limb(ProcMesh *out, uint32_t col)
{
    enum { N = 6, R = 6 };
    static const float Z[R] = { -0.3f, -0.17f, 0.0f, 1.0f, 1.15f, 1.27f }, K[R] = { 0.0f, 0.75f, 1.0f, 0.8f, 0.6f, 0.0f };
    float pts[R * N * 3];
    for (int r = 0; r < R; r++) ring(&pts[r * N * 3], N, MESH * Z[r], MESH * K[r], MESH * K[r], 0.0f, 99.0f);
    proc_mesh_begin();
    proc_mesh_uv(SCALES_AROUND, 3.2f);
    proc_mesh_loft(pts, R, N, NULL, col, false);
    proc_mesh_end(out);
}

// Horn, bone and claw (spikes) take the belly plates' band of the scale map, along their length:
// growth rings, not scales
static void bone_uv(float along) { proc_mesh_uv(8.0f, along); proc_mesh_uv_shift(92.0f, 0.0f); }

// A foot, +z forward, sole on y = 0: a pad and three hooked claws
static void build_foot(void)
{
    proc_mesh_begin();
    proc_mesh_uv(12.0f, 1.6f);
    proc_mesh_box((float[3]){ 0, 1.5f, 1.5f }, (float[3]){ 3.4f, 1.6f, 3.6f }, COL_BACK);
    bone_uv(2.4f);
    for (int k = -1; k <= 1; k++) {
        float x = k * 2.4f;
        proc_mesh_spike((float[3]){ x, 1.6f, 4.6f }, (float[3]){ x * 1.2f, 0.0f, 8.2f }, 1.0f, 3, COL_IVORY);
    }
    proc_mesh_spike((float[3]){ 0, 1.8f, -1.6f }, (float[3]){ 0, 0.2f, -4.0f }, 0.8f, 3, COL_IVORY);   // the dewclaw
    proc_mesh_end(&s.mFoot);
}

// The head, +z forward from the top of the neck: a lofted skull tapering to the snout, a flat
// jaw line, brows over glowing eyes, swept horns, cheek spikes, nostrils, the upper teeth. Closed
// behind (a point at its back): the neck doesn't fill it, and a head tipped toward the camera
// showed the hall through its open back
static void build_head(ProcMesh *out, bool dark)
{
    enum { N = 8, R = 9 };
    static const float Z[R] = { -3.6f, -2, 2, 7, 11, 15, 19, 23, 26.5f };
    static const float W[R] = { 0.0f, 5.6f, 6.8f, 6.7f, 5.7f, 4.7f, 4.0f, 3.4f, 1.9f };
    static const float H[R] = { 0.0f, 5.0f, 5.6f, 5.4f, 4.2f, 3.3f, 2.8f, 2.3f, 1.3f };
    static const float Y[R] = { 2.0f, 2.0f, 2.6f, 2.9f, 2.4f, 1.6f, 1.0f, 0.6f, 0.3f };
    float pts[R * N * 3];
    for (int r = 0; r < R; r++) ring(&pts[r * N * 3], N, Z[r], W[r], H[r], Y[r], H[r] * 0.45f);
    uint32_t cols[(R - 1) * N];
    for (int r = 0; r < R - 1; r++)
        for (int i = 0; i < N; i++) {
            float sn = fm_sinf(2.0f * T3D_PI * (i + 0.5f) / N);
            cols[r * N + i] = sn < -0.3f && r > 0 ? MOUTH_TOP : COL_BACK;   // the mouth line
        }
    uint32_t cr = crystal_col(dark), eye = dark ? proc_rgb(240, 90, 255) : proc_rgb(255, 130, 40);
    proc_mesh_begin();
    proc_mesh_uv(SCALES_AROUND, 3.4f);     // smaller scales than the body's; the mouth flesh, untextured
    proc_mesh_uv_flat_color(MOUTH_TOP);
    proc_mesh_loft(pts, R, N, cols, COL_BACK, true);
    for (int sd = -1; sd <= 1; sd += 2) {
        proc_mesh_box((float[3]){ sd * 4.1f, 6.0f, 11.0f }, (float[3]){ 1.6f, 0.9f, 3.2f }, COL_BACK);                     // brow
        crystal((float[3]){ sd * 4.9f, 4.4f, 13.5f }, (float[3]){ sd * 5.9f, 4.7f, 9.5f }, 1.1f, eye);                        // eye
        bone_uv(1.6f);
        proc_mesh_spike((float[3]){ sd * 4.0f, 6.4f, 4.0f }, (float[3]){ sd * 8.5f, 13.5f, -15.0f }, 2.3f, 4, COL_BONE);   // horns
        proc_mesh_spike((float[3]){ sd * 6.0f, 3.0f, 1.0f }, (float[3]){ sd * 12.5f, 3.5f, -9.0f }, 1.6f, 4, COL_BONE);
        proc_mesh_spike((float[3]){ sd * 5.6f, 0.8f, 7.0f }, (float[3]){ sd * 9.0f, -0.5f, 2.0f }, 1.0f, 3, COL_BONE);     // cheek
        proc_mesh_box((float[3]){ sd * 1.4f, 1.4f, 25.6f }, (float[3]){ 0.5f, 0.4f, 0.6f }, proc_rgb(20, 14, 16));        // nostril
        proc_mesh_uv(SCALES_AROUND, 3.4f); proc_mesh_uv_shift(0.0f, 0.0f);
        for (int k = 0; k < 4; k++)
            crystal((float[3]){ sd * (3.6f - k * 0.4f), -0.4f, 12.0f + k * 3.6f }, (float[3]){ sd * (3.4f - k * 0.4f), -2.8f, 12.6f + k * 3.6f }, 0.7f, COL_IVORY);
    }
    crystal((float[3]){ 0, 7.4f, 4.0f }, (float[3]){ 0, 13.5f, -5.0f }, 2.6f, cr);                                           // crest
    crystal((float[3]){ 0, 6.8f, -1.0f }, (float[3]){ 0, 10.5f, -9.0f }, 2.0f, cr);
    crystal((float[3]){ 0, 3.0f, 21.0f }, (float[3]){ 0, 5.4f, 19.0f }, 0.9f, cr);                                            // nose horn
    proc_mesh_end(out);
}

// The lower jaw, +z forward from its hinge: lofted, closed behind, the mouth inside on top, teeth up
static void build_jaw(void)
{
    enum { N = 6, R = 6 };
    static const float Z[R] = { -1.8f, 0, 6, 12, 18, 22.5f };
    static const float W[R] = { 0.0f, 4.6f, 4.4f, 3.8f, 3.0f, 1.6f };
    static const float H[R] = { 0.0f, 2.2f, 2.0f, 1.8f, 1.4f, 0.8f };
    float pts[R * N * 3];
    for (int r = 0; r < R; r++) ring(&pts[r * N * 3], N, Z[r], W[r], H[r], -1.6f, 99.0f);
    uint32_t cols[(R - 1) * N];
    for (int r = 0; r < R - 1; r++)
        for (int i = 0; i < N; i++) cols[r * N + i] = fm_sinf(2.0f * T3D_PI * (i + 0.5f) / N) > 0.3f && r > 0 ? MOUTH_JAW : COL_BELLY;
    proc_mesh_begin();
    proc_mesh_uv(SCALES_AROUND, 4.8f);
    proc_mesh_uv_flat_color(MOUTH_JAW);
    proc_mesh_loft(pts, R, N, cols, COL_BELLY, true);
    for (int sd = -1; sd <= 1; sd += 2)
        for (int k = 0; k < 4; k++)
            crystal((float[3]){ sd * (3.2f - k * 0.45f), 0.2f, 7.0f + k * 3.6f }, (float[3]){ sd * (3.0f - k * 0.45f), 2.6f, 7.6f + k * 3.6f }, 0.6f, COL_IVORY);
    proc_mesh_end(&s.mJaw);
}

// A glow: an unlit octahedron of radius 8, drawn additively (flames, dark orbs, the beam)
static void build_glow(ProcMesh *out, uint32_t a, uint32_t b)
{
    const float v[6][3] = { { 8, 0, 0 }, { -8, 0, 0 }, { 0, 8, 0 }, { 0, -8, 0 }, { 0, 0, 8 }, { 0, 0, -8 } };
    proc_mesh_begin();
    for (int i = 0; i < 8; i++)
        proc_mesh_tri(v[(i & 1) ? 0 : 1], v[(i & 2) ? 2 : 3], v[(i & 4) ? 4 : 5], (i % 3) ? a : b, true);
    proc_mesh_end(out);
}

// The fire cone at full size, its point at the origin, along +z (cone_h: its height); drawn
// scaled as it rolls out. Hot near its point, red at its rim, a low wall of fire along the rim
static void build_cone(void)
{
    enum { NA = 6 };
    float P[3][NA + 1][3], G[NA + 1][3];
    for (int r = 0; r < 3; r++)
        for (int j = 0; j <= NA; j++) {
            float a = CONE_HALF * (-1.0f + 2.0f * j / NA), d = CONE_L * CONE_F[r + 1];
            P[r][j][0] = fm_sinf(a) * d; P[r][j][1] = cone_h(CONE_F[r + 1], a); P[r][j][2] = fm_cosf(a) * d;
            if (r == 2) { memcpy(G[j], P[r][j], sizeof(G[j])); G[j][1] = 0.0f; }
        }
    const float apex[3] = { 0.0f, CONE_H[0], 0.0f };
    proc_mesh_begin();
    for (int j = 0; j < NA; j++) {
        bool alt = j & 1;
        proc_mesh_tri(apex, P[0][j], P[0][j + 1], proc_rgb(255, 214, 96), true);
        proc_mesh_quad(P[0][j], P[1][j], P[1][j + 1], P[0][j + 1], alt ? proc_rgb(255, 176, 60) : proc_rgb(255, 150, 46), true);
        proc_mesh_quad(P[1][j], P[2][j], P[2][j + 1], P[1][j + 1], alt ? proc_rgb(255, 104, 30) : proc_rgb(240, 84, 24), true);
        proc_mesh_quad(P[2][j], G[j], G[j + 1], P[2][j + 1], alt ? proc_rgb(230, 70, 22) : proc_rgb(255, 120, 34), true);
    }
    proc_mesh_end(&s.mCone);
}

// ---- The skinned body ----

// A bone's frame from its head, tail and reference: columns x, y (along the bone), z; row-major
static void sb_frame(const float h[3], const float t[3], const float ref[3], float M[9])
{
    float y[3] = { t[0] - h[0], t[1] - h[1], t[2] - h[2] }, x[3], z[3];
    norm3(y); cross3(x, ref, y); norm3(x); cross3(z, x, y);
    for (int r = 0; r < 3; r++) { M[r * 3 + 0] = x[r]; M[r * 3 + 1] = y[r]; M[r * 3 + 2] = z[r]; }
}

static bool sb_lit(void *u, const T3DObject *o) { (void)u; return o->material && !strcmp(o->material->name, "sb_body"); }
static bool sb_unlit(void *u, const T3DObject *o) { return !sb_lit(u, o); }

// Each bone's bind rotation as seen from the frame the rig will build for it: the rig's frame
// times this is the bone's rotation (whatever roll the model gave it)
static void sb_load(void)
{
    s.model = t3d_model_load("rom:/dragon_boss/shadowborne.t3dm");
    T3DSkeleton sk = t3d_skeleton_create(s.model);
    assertf(sk.skeletonRef->boneCount == SB_BONES, "shadowborne: %d bones, the bind pose has %d", sk.skeletonRef->boneCount, SB_BONES);
    t3d_skeleton_update(&sk);
    assertf(!strcmp(SB_BIND[16].name, "upper0") && !strcmp(SB_BIND[24].name, "armL") && !strcmp(SB_BIND[36].name, "armR"), "shadowborne: bone order");
    for (int i = 0; i < SB_BONES; i++) {
        int t = t3d_skeleton_find_bone(&sk, SB_BIND[i].name);
        assertf(t >= 0, "shadowborne: no bone %s", SB_BIND[i].name);
        s.boneT[i] = t;
        const T3DMat4x3 *m = &sk.bones[t].matrix;      // columns m[c]: its axes in the model
        float Fb[9];
        sb_frame(SB_BIND[i].head, SB_BIND[i].tail, SB_BIND[i].ref, Fb);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) s.bindK[i][r * 3 + c] = Fb[0 * 3 + r] * m->m[c][0] + Fb[1 * 3 + r] * m->m[c][1] + Fb[2 * 3 + r] * m->m[c][2];
    }
    t3d_skeleton_destroy(&sk);
    // (its matrices: the skeleton segment, set before each run). tiny3d lights every material: the
    // membranes and crystals, unlit like the old parts, are their own draw under a plain white ambient
    gpu_block_begin();
    t3d_model_draw_custom(s.model, (T3DModelDrawConf){ .matrices = gpu_skeleton_matrices(), .filterCb = sb_lit });
    s.block = gpu_block_end();
    gpu_block_begin();
    t3d_model_draw_custom(s.model, (T3DModelDrawConf){ .matrices = gpu_skeleton_matrices(), .filterCb = sb_unlit });
    s.blockUnlit = gpu_block_end();
}

static void tail_tip(float e[3]);
// Bone i's head, tail and frame reference from the rig as it stands (the order tools/shadowborne_model.py writes)
static void sb_points(const Boss *b, int i, float h[3], float t[3], const float **ref)
{
    static float up[3];
    *ref = UP;
    if (i == 0) { memcpy(h, b->pos, 12); mad3(t, b->pos, UP, 10.0f); *ref = s.F; return; }
    if (i <= 3) {
        const float *pts[4] = { s.chest, s.spine[0], s.spine[1], s.hips };
        memcpy(h, pts[i - 1], 12); memcpy(t, pts[i], 12); memcpy(up, s.bodyUp, 12); *ref = up; return;
    }
    if (i <= 7) { memcpy(h, s.neck[i - 4], 12); memcpy(t, s.neck[i - 3], 12); return; }
    if (i <= 15) { int j = i - 8; memcpy(h, s.tl[j], 12); if (j < NTAIL) memcpy(t, s.tl[j + 1], 12); else tail_tip(t); return; }
    if (i <= 23) {
        int k = (i - 16) >> 1;
        if (!((i - 16) & 1)) { memcpy(h, s.shoulder[k], 12); memcpy(t, s.knee[k], 12); }
        else { memcpy(h, s.knee[k], 12); memcpy(t, s.foot[k], 12); }
        *ref = s.F; return;
    }
    int w = (i - 24) / 12, j = (i - 24) % 12;      // arm, forearm, each finger's two joints
    if (j == 0) { memcpy(h, s.wj[w][0], 12); memcpy(t, s.wj[w][1], 12); }
    else if (j == 1) { memcpy(h, s.wj[w][1], 12); memcpy(t, s.wj[w][2], 12); }
    else if (j < 7) { memcpy(h, s.wj[w][2], 12); memcpy(t, s.wj[w][3 + j - 2], 12); }
    else { memcpy(h, s.wj[w][3 + j - 7], 12); memcpy(t, s.wj[w][8 + j - 7], 12); }
    *ref = s.wup[w];
}

// The body: every bone's matrix this frame (the rig's frame times its bind rotation, at the rig's
// point), the model at the root, scaled down from its model units; lit by the hall's light
static void sb_draw(Boss *b)
{
#ifdef HLE_LOG_FPS      // test aid: what the skinned body costs the CPU (its bones; the model's draw)
    static uint32_t tb, tm, n; uint32_t t0 = TICKS_READ();
#endif
    T3DMat4FP *bm = gpu_alloc(sizeof(T3DMat4FP) * SB_BONES);
    for (int i = 0; i < SB_BONES; i++) {
        float h[3], t[3], Ft[9], M[9];
        const float *ref;
        sb_points(b, i, h, t, &ref);
        sb_frame(h, t, ref, Ft);
        const float *K = s.bindK[i];
        float g = i >= 24 ? s.wingG : 1.0f;     // a wing bone at the wing's size (uniform: nothing squashed)
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) M[r * 3 + c] = (Ft[r * 3 + 0] * K[0 * 3 + c] + Ft[r * 3 + 1] * K[1 * 3 + c] + Ft[r * 3 + 2] * K[2 * 3 + c]) * g;
        T3DMat4 m = {{
            { M[0], M[3], M[6], 0 }, { M[1], M[4], M[7], 0 }, { M[2], M[5], M[8], 0 },
            { (h[0] - b->pos[0]) * SB_SCALE, (h[1] - b->pos[1]) * SB_SCALE, (h[2] - b->pos[2]) * SB_SCALE, 1 },
        }};
        t3d_mat4_to_fixed(&bm[s.boneT[i]], &m);
    }
    const float k = 1.0f / SB_SCALE;
    T3DMat4 mm = {{ { k, 0, 0, 0 }, { 0, k, 0, 0 }, { 0, 0, k, 0 }, { b->pos[0], b->pos[1], b->pos[2], 1 } }};
    T3DMat4FP mf;
    t3d_mat4_to_fixed(&mf, &mm);
    t3d_matrix_set(gpu_mat(&mf), true);
    // The rigid parts' baked light (proc_mesh: a key from above-front, 0.72, over 0.42 ambient) as a
    // real light: the body shades as it bends. The hall has no other lights: put back after
    static const T3DVec3 KEY = {{ -0.45f / 1.0136f, 0.8f / 1.0136f, 0.4f / 1.0136f }};
    t3d_light_set_ambient((uint8_t[4]){ 107, 107, 107, 255 });
    static const T3DVec3 FILL = {{ 0.45f / 1.0136f, -0.8f / 1.0136f, -0.4f / 1.0136f }};     // (and its faint bounce from the other side, 0.14)
    t3d_light_set_directional(0, (uint8_t[4]){ 184, 184, 184, 255 }, &KEY);
    t3d_light_set_directional(1, (uint8_t[4]){ 36, 36, 36, 255 }, &FILL);
    t3d_light_set_count(2);
#ifdef HLE_LOG_FPS
    uint32_t t1 = TICKS_READ();
#endif
    t3d_segment_set(T3D_SEGMENT_SKELETON, bm);
    rspq_block_run(s.block);
    t3d_light_set_count(0);
    t3d_light_set_ambient((uint8_t[4]){ 255, 255, 255, 255 });
    rspq_block_run(s.blockUnlit);
#ifdef HLE_LOG_FPS
    tb += t1 - t0; tm += TICKS_READ() - t1;
    if (++n == 120) { debugf("[sbcpu] bones %.2f model %.2f ms\n", TICKS_TO_US(tb / 120) / 1000.0, TICKS_TO_US(tm / 120) / 1000.0); tb = tm = n = 0; }
#endif
    t3d_light_set_count(0);
    t3d_light_set_ambient((uint8_t[4]){ 255, 255, 255, 255 });
}

// ---- Lifetime ----

void dragon_boss_free(void)
{
    if (!s.ready) return;
    gpu_wait();
    ProcMesh *all[] = { &s.mThroat, &s.mFoot, &s.mHead, &s.mHeadDark, &s.mJaw, &s.flame, &s.mOrb, &s.mIce, &s.mCone };
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) proc_mesh_free(all[i]);
    sprite_free(s.texScales); sprite_free(s.texMemb);
    rspq_block_free(s.block); rspq_block_free(s.blockUnlit);
    t3d_model_free(s.model);
    free_uncached(s.mats);
    memset(&s, 0, sizeof(s));
}

static void clear_hazards(void)
{
    for (int i = 0; i < FL_MAX; i++) s.fl[i].life = 0.0f;
    for (int i = 0; i < ORB_MAX; i++) s.orb[i].on = false;
    for (int i = 0; i < BL_MAX; i++) s.bl[i].on = false;
    for (int i = 0; i < BALL_MAX; i++) s.ball[i].on = false;
    s.beamOn = false;
    s.cone.on = false;
    s.glow = 0.0f;
}

void dragon_boss_begin(Boss* boss)
{
    if (!s.ready) {
        sb_load();
        build_limb(&s.mThroat, COL_BELLY);
        build_foot();
        build_head(&s.mHead, false); build_head(&s.mHeadDark, true);
        build_jaw();
        build_glow(&s.flame, proc_rgb(255, 120, 30), proc_rgb(255, 200, 70));
        build_glow(&s.mOrb, proc_rgb(150, 50, 255), proc_rgb(230, 170, 255));
        build_glow(&s.mIce, proc_rgb(110, 170, 255), proc_rgb(225, 240, 255));     // the crystal beam, before the dark
        build_cone();
        s.texScales = sprite_load("rom:/dragon_boss/scales.i8.sprite");
        s.texMemb = sprite_load("rom:/dragon_boss/membrane.i8.sprite");
        s.mats = malloc_uncached(sizeof(T3DMat4FP) * MAT_RING * MAT_MAX);
        s.ready = true;
    }
    boss->phaseIndex = DRAGON_BOSS_PHASE;
    boss->name = "Shadowborne";
    boss->health = boss->maxHealth;
    boss->mode = BOSS_MODE_CINEMATIC;       // not damageable until it has landed
    boss->state = BOSS_STATE_INTRO;
    boss->poise = boss->poiseIdleT = boss->staggerT = 0.0f;
    boss->damageFlashTimer = 0.0f;
    boss->velX = boss->velZ = 0.0f;
    boss->handAttackColliderActive = false;
    boss->sphereAttackColliderActive = false;
    boss->visible = true;
    boss->capsuleCollider.radius = HIT_R;
    float dx, dz;
    to_player(boss, &dx, &dz);
    boss->rot[0] = boss->rot[2] = 0.0f;
    boss->rot[1] = atan2f(dx, dz);

    s.groundY = character.pos[1];
    s.alt = DROP_H;
    s.dark = false;
#ifdef HLE_DRAGON_DARK      // test aid: its dark half from the start (looks, with HLE_TEST_FREEZE_BOSS)
    s.dark = true;
#endif
    s.life = 0.0f;
    s.feetReady = false;
    s.neckReach = s.tNeckReach = 1.0f;
    s.wingSpan = 1.0f; s.wingRaise = 0.3f; s.wingSweep = 0.0f;     // (flying in: as it always did)
    s.wingFold = s.tWingFold = 0.0f; s.wingG = 1.0f;
    s.yawPrev = boss->rot[1]; s.altPrev = DROP_H; s.tailLag = s.tailV = 0.0f; s.passT = 0.0f;
    s.cdBackstep = 6.0f; s.cdSwipe = 7.0f;
    s.prevHealth = boss->health; s.shakeT = 0.0f; s.lookRel = 0.0f;
    s.cdWhip = 8.0f;
    s.cdClaw = 2.0f; s.cdBite = 4.0f; s.cdTail = 3.0f; s.cdBreath = 6.0f; s.cdCharge = 9.0f; s.cdFly = 16.0f;
    s.cdNova = 0.0f; s.cdOrbs = 4.0f; s.cdBeam = 9.0f; s.cdTriple = 5.0f; s.cdSnipe = 7.0f; s.cdSweep = 10.0f; s.cdCone = 6.0f;
    clear_hazards();
    enter(DG_INTRO);
}

void dragon_boss_skip_intro(Boss* boss)
{
    if (!s.ready || s.state != DG_INTRO) return;
    s.alt = 0.0f;
    boss->mode = BOSS_MODE_COMBAT;
    boss->state = BOSS_STATE_NEUTRAL;
    enter(DG_IDLE);
}

// ---- Hazards: flames, dark orbs, the beam's explosions ----

static void flame_spawn(const float p[3], const float v[3], float life, float size, bool harm)
{
    Flame *f = &s.fl[s.flNext];
    s.flNext = (s.flNext + 1) % FL_MAX;
    memcpy(f->p, p, sizeof(f->p)); memcpy(f->v, v, sizeof(f->v));
    f->age = 0.0f; f->life = life; f->size = size; f->harm = harm; f->ember = false;
}

// A burst (skill_fx's crimson spikes, 14 long, ~11 high) as an explosion: it hits where its spikes
// are, ~12 out, to the feet, as it appears
#define BURST_R 12.0f
static bool burst_hits(const float p[3])
{
    float dx = character.pos[0] - p[0], dz = character.pos[2] - p[2];
    return dx * dx + dz * dz < (BURST_R + PLAYER_R) * (BURST_R + PLAYER_R) && character.pos[1] - s.groundY < 11.0f;
}

static void blast_spawn(const float p[3])
{
    Blast *e = &s.bl[s.blNext];
    s.blNext = (s.blNext + 1) % BL_MAX;
    e->p[0] = p[0]; e->p[1] = p[2]; e->t = 0.9f; e->on = true;
    skill_fx_spawn_burst_hostile((float[3]){ p[0], s.groundY, p[2] }, s.life);
}

static void hazards_update(float dt)
{
    float pc[3];
    player_chest(pc);
    for (int i = 0; i < FL_MAX; i++) {
        Flame *f = &s.fl[i];
        if (f->age >= f->life) continue;
        f->age += dt;
        mad3(f->p, f->p, f->v, dt);
        float k = fmaxf(0.0f, 1.0f - 1.5f * dt);
        for (int m = 0; m < 3; m++) f->v[m] *= k;
        if (f->p[1] < s.groundY + 2.0f) {       // on the floor it spreads and burns
            f->p[1] = s.groundY + 2.0f;
            f->v[1] = 0.0f;
        }
        // as drawn (it shrinks as it dies: the hit didn't); the young flames of the ground sweep are the
        // stream itself, the first to catch the player a heavier blow (once); the rest burn
        float gap = axis_dist_pt(f->p) - flame_r(f->size, f->age, f->life);
        if (!f->harm || s.fireCd > 0.0f || gap >= PLAYER_R) continue;
        bool stream = s.state == DG_SWEEP && s.step == 1 && !s.hitDone && f->age < 0.6f;
        HB_HIT(stream ? HB_STREAM : HB_FLAME, gap - 8.0f);
        // the rain from the air: a heavier hit, then a breath before the next (one standing under
        // a pass took ~60 at 9 every 0.4 s)
        bool rain = s.state == DG_FLY;
        character_apply_damage(stream ? 20.0f : rain ? 12.0f : f->ember ? 5.0f : 9.0f);
        s.fireCd = rain ? 1.2f : f->ember ? 0.5f : 0.4f;      // (a fireball's patch: 5 every 0.5 s)
        if (stream) s.hitDone = true;
    }
    for (int i = 0; i < ORB_MAX; i++) {
        Orb *o = &s.orb[i];
        if (!o->on) continue;
        o->age += dt;
        if (o->age < 0.8f) {
            o->p[1] += 8.0f * dt;               // gathers, then homes in
        } else {
            float want[3] = { pc[0] - o->p[0], pc[1] - o->p[1], pc[2] - o->p[2] };
            norm3(want);
            for (int m = 0; m < 3; m++) o->v[m] += (want[m] * 58.0f - o->v[m]) * fminf(1.0f, dt * 1.3f);
            mad3(o->p, o->p, o->v, dt);
        }
        bool hit = axis_dist_pt(o->p) < 4.5f + PLAYER_R;      // drawn 4.5 round (the chest point within 8 let some pass through)
        HB_TOUCH(HB_ORB, axis_dist_pt(o->p) - 4.5f - 8.0f);
        if (hit) { HB_HIT(HB_ORB, axis_dist_pt(o->p) - 4.5f - 8.0f); character_apply_damage(12.0f); }
        if (hit || o->p[1] < s.groundY + 1.0f || o->age > 7.0f || !room_ok(o->p[0], o->p[2])) {
            o->on = false;
            skill_fx_spawn_burst_hostile((float[3]){ o->p[0], fmaxf(o->p[1], s.groundY), o->p[2] }, 0.0f);
        }
    }
    for (int i = 0; i < BL_MAX; i++) {
        Blast *e = &s.bl[i];
        if (!e->on) continue;
        float was = e->t;
        e->t -= dt;
        if (was > 0.2f && e->t <= 0.2f && ((i & 1) || s.state == DG_SNIPE)) sfx(SCENE1_SFX_ROCK_BLAST, 0.7f);    // every other one: they come five a second
        if (e->t > 0.0f) continue;
        e->on = false;
        float p[3] = { e->p[0], s.groundY, e->p[1] };
        // a burst with the blade: the blade alone is 4 wide and the hit reached 13 (its warning
        // burst gone half a second before)
        skill_fx_spawn_wave_hostile(SKILL_WAVE_BLADE, p, (float[3]){ 0, 0, 0 }, 0.0f, 0, 1.0f, 0.8f, false);
        skill_fx_spawn_burst_hostile(p, s.life + i);
        float dx = character.pos[0] - p[0], dz = character.pos[2] - p[2];
        HB_TOUCH(HB_BLAST, sqrtf(dx * dx + dz * dz) - BURST_R - 8.0f);
        if (burst_hits(p)) { HB_HIT(HB_BLAST, sqrtf(dx * dx + dz * dz) - BURST_R - 8.0f); character_apply_damage(12.0f); }
        screen_shake_set_shake_mag(3.0f);
    }
    for (int i = 0; i < BALL_MAX; i++) {
        Ball *o = &s.ball[i];
        if (!o->on) continue;
        mad3(o->p, o->p, o->v, dt);
        bool direct = axis_dist_pt(o->p) < 5.5f + PLAYER_R;      // the ball is drawn 5.5 round
        if (!direct && o->p[1] > s.groundY + 2.0f && room_ok(o->p[0], o->p[2])) continue;
        o->on = false;      // bursts where it lands, and the floor burns there a moment
        float p[3] = { o->p[0], s.groundY, o->p[2] };
        skill_fx_spawn_burst_hostile(p, s.life);
        for (int k = 0; k < 6; k++) {
            float ang = k * (2.0f * T3D_PI / 6.0f) + frand() * 0.3f, rr = k ? 9.0f : 0.0f;
            flame_spawn((float[3]){ p[0] + fm_sinf(ang) * rr, s.groundY + 2.0f, p[2] + fm_cosf(ang) * rr }, (float[3]){ 0, 0, 0 }, 1.5f, 12.0f + 2.0f * frand(), true);
            s.fl[(s.flNext + FL_MAX - 1) % FL_MAX].ember = true;
        }
        float dx = character.pos[0] - p[0], dz = character.pos[2] - p[2];
        HB_TOUCH(HB_BALLBURST, sqrtf(dx * dx + dz * dz) - BURST_R - 8.0f);
        if (direct || burst_hits(p)) { HB_HIT(direct ? HB_BALL : HB_BALLBURST, direct ? axis_dist_pt(o->p) - 5.5f - 8.0f : sqrtf(dx * dx + dz * dz) - BURST_R - 8.0f); character_apply_damage(12.0f); }
        screen_shake_set_shake_mag(4.0f);
        sfx_boom();
    }
}

// A beam from the mouth to `to` this frame: hits the player where it is drawn (the beam `w` round,
// the glow where it meets the floor 9; once a move); a beam that reaches the floor (`blasts`)
// leaves explosions behind every 0.2 s. (It tested the player's chest point only, and some beams
// passing over the head or through the legs missed)
static void beam_at(const float to[3], float w, float dmg, bool blasts, float dt)
{
    memcpy(s.beamP, to, sizeof(s.beamP));
    memcpy(s.aim, to, sizeof(s.aim)); s.tAimK = 1.0f;
    s.beamOn = true; s.beamW = w; s.glow = 1.0f;
    float bg = fminf(axis_dist_seg(s.mouth, to) - w, axis_dist_pt(to) - 9.0f);
    HB_TOUCH(HB_BEAM, bg - 8.0f);
    if (!s.hitDone && bg < PLAYER_R) { HB_HIT(HB_BEAM, bg - 8.0f); character_apply_damage(dmg); s.hitDone = true; }
    if (!blasts) return;
    s.emit -= dt;
    if (s.emit <= 0.0f) { s.emit += 0.2f; blast_spawn(to); }
}

// Breath: a stream of flames from the mouth toward `to`
static void breathe(const float to[3], float speed, float life, float dt)
{
    static float roarT;
    roarT -= dt;
    if (roarT <= 0.0f || s.emit == 0.0f) { sfx(SCENE1_SFX_FIRE, 0.9f); roarT = 0.45f; }     // the torrent crackles on, from its first flames
    s.emit -= dt;
    while (s.emit <= 0.0f) {
        s.emit += 0.03f;
        float v[3] = { to[0] - s.mouth[0], to[1] - s.mouth[1], to[2] - s.mouth[2] };
        // at least fast enough to get there: a flame slows (hazards_update, 1.5 a second) and
        // goes no further than speed / 1.5 * (1 - e^(-1.5 life)); too slow, the fire stalled short
        // of what it was aimed at (the breath at range, the rain from the air)
        float need = 1.5f * len3(v) / (1.0f - expf(-1.5f * life)) * 1.05f;
        norm3(v);
        for (int m = 0; m < 3; m++) v[m] = v[m] * fmaxf(speed, need) + frand() * 12.0f;
        flame_spawn(s.mouth, v, life, 16.0f + 4.0f * frand(), true);
    }
}

// ---- The rig ----

static void foot_rest(int k, float out[3])
{
    float side = (k & 1) ? 1.0f : -1.0f;
    mad3(out, s.shoulder[k], s.F, k < 2 ? 6.0f : -2.0f);
    mad3(out, out, s.R, side * 5.0f);
    out[1] = s.groundY;
}

static void feet_update(float dt)
{
    if (!s.feetReady) {
        for (int k = 0; k < 4; k++) { foot_rest(k, s.feet[k].p); s.feet[k].moving = false; }
        s.feetReady = true;
    }
    for (int k = 0; k < 4; k++) {
        Foot *f = &s.feet[k];
        float rest[3];
        foot_rest(k, rest);
        if (s.alt > 4.0f) {
            // In the air: the front legs folded up under the chest, the hind ones trailing back,
            // each swaying on its own; reaching down for the floor as it comes in to land
            float hang[3], sway = fm_sinf(s.life * 2.3f + k * 1.7f);
            if (k < 2) {
                mad3(hang, s.shoulder[k], s.F, 9.0f + 2.0f * sway);
                mad3(hang, hang, UP, -LEG_L * 0.75f);
            } else {
                mad3(hang, s.shoulder[k], s.F, -LEG_L * 1.3f - 3.0f * sway);
                mad3(hang, hang, UP, -LEG_L * 0.85f);
            }
            float reach = clamp01((40.0f - s.alt) / 36.0f);     // 0 high up, 1 at the floor
            lerp3(hang, hang, rest, reach);
            if (reach > 0.0f) hang[1] = fmaxf(hang[1], s.groundY);
            lerp3(f->p, f->p, hang, fminf(1.0f, dt * 6.0f));
            f->moving = false;
            s.swing[k] = 0.0f;
        } else if (f->moving) {
            float was = f->t;
            f->t += dt / 0.3f;
            if (k < 2 && was < 0.75f && f->t >= 0.75f) sfx_step();     // loud 0.07 s on: as the paw lands
            float u = clamp01(f->t);
            lerp3(f->p, f->from, rest, u);
            s.swing[k] = fm_sinf(u * T3D_PI);
            f->p[1] = s.groundY + s.swing[k] * 7.0f;
            if (u >= 1.0f) f->moving = false;
        } else {
            s.swing[k] = 0.0f;
            f->p[1] = fmaxf(s.groundY, f->p[1] - 90.0f * dt);
            float dx = f->p[0] - rest[0], dz = f->p[2] - rest[2];
            bool fidget = s.state == DG_IDLE && k < 2 && (pick_rng() % 1000) < (uint32_t)(dt * 250.0f);   // ~1 in 4 s a paw
            if ((fidget || dx * dx + dz * dz > 16.0f * 16.0f) && !s.feet[k ^ 1].moving) {
                f->moving = true; f->t = 0.0f;
                memcpy(f->from, f->p, sizeof(f->p));
            }
        }
        memcpy(s.foot[k], f->p, sizeof(f->p));
        if (k < 2 && s.claw[k] > 0.01f) {      // a front claw raised to strike
            float side = (k & 1) ? 1.0f : -1.0f, up[3];
            mad3(up, s.shoulder[k], s.F, 20.0f);
            mad3(up, up, s.R, -side * 4.0f);
            up[1] += 6.0f + 14.0f * s.clawHi;      // the heavy stomp's paw ~1.5x the claw's height
            lerp3(s.foot[k], f->p, up, s.claw[k]);
        }
        if (k == s.swipeK && s.swipeAmt > 0.01f) {      // the swipe charge: the foreleg cocked out wide, then raked across in front
            float side = (k & 1) ? 1.0f : -1.0f, out[3], across[3], p[3];
            mad3(out, s.shoulder[k], s.R, side * 24.0f); mad3(out, out, s.F, 12.0f); out[1] = s.groundY + 12.0f;
            mad3(across, s.shoulder[k], s.R, -side * 14.0f); mad3(across, across, s.F, 26.0f); across[1] = s.groundY + 5.0f;
            lerp3(p, out, across, smooth01(s.swipePh));
            p[1] += 6.0f * fm_sinf(clamp01(s.swipePh) * T3D_PI);     // up over the arc
            lerp3(s.foot[k], s.foot[k], p, clamp01(s.swipeAmt));
        }
        // never further from its shoulder than the leg reaches (the lower bone was drawn stretched)
        float d[3] = { s.foot[k][0] - s.shoulder[k][0], s.foot[k][1] - s.shoulder[k][1], s.foot[k][2] - s.shoulder[k][2] }, l = len3(d);
        if (l > 2.0f * LEG_L - 0.5f) mad3(s.foot[k], s.shoulder[k], d, (2.0f * LEG_L - 0.5f) / l);
    }
}

static void knee(const float a[3], const float c[3], const float bend[3], float out[3]);

// The great wings' joints in the wing's own frame (x out along the arm, y its upper side, z
// forward), open; each bone keeps its length and turns toward its folded direction with the fold
// (tools/shadowborne_rig.py: the model is built from the same)
static const float W_ELBOW[3] = { 30, 6, 4 }, W_WRIST[3] = { 58, 10, -6 };
static const float W_TIPS[5][3] = { { 88, 5, -14 }, { 80, 2, -32 }, { 66, 0, -46 }, { 48, -2, -54 }, { 28, -3, -48 } };
static const float W_FOLD_ARM[3] = { 0.70f, 0.32f, -0.64f }, W_FOLD_FORE[3] = { 0.10f, -0.12f, -0.99f };
static const float W_FOLD_FING[5][3] = { { 0.26f, -0.40f, -0.88f }, { 0.25f, -0.46f, -0.85f }, { 0.24f, -0.52f, -0.82f }, { 0.23f, -0.58f, -0.78f }, { 0.22f, -0.64f, -0.74f } };
static void slerp_dir(const float a[3], const float b[3], float t, float o[3])
{
    float an[3] = { a[0], a[1], a[2] }, bn[3] = { b[0], b[1], b[2] };
    norm3(an); norm3(bn);
    float c = fminf(fmaxf(an[0] * bn[0] + an[1] * bn[1] + an[2] * bn[2], -1.0f), 1.0f), th = acosf(c);
    if (th < 1e-4f) { memcpy(o, an, 12); return; }
    float s0 = fm_sinf((1.0f - t) * th) / fm_sinf(th), s1 = fm_sinf(t * th) / fm_sinf(th);
    for (int m = 0; m < 3; m++) o[m] = an[m] * s0 + bn[m] * s1;
    norm3(o);
}
#define KNUCKLE 0.55f      // each finger's knuckle, this far from the wrist to its tip
// Each segment folds by its own amount (the arm, forearm and fingers apart: a beat runs down the
// wing as a wave) and droops (the forearm and fingers bent down under the stroke); `curl` bends a
// finger's outer joint on down
typedef struct { float foldArm, foldFore, foldFing, droopFore, droopFing, curl[5]; } WingPose;
static void droop(float d[3], float k) { d[1] -= k; norm3(d); }
static void wing_local(const WingPose *w, float L[12][3])     // elbow, wrist, five knuckles, five fingertips (the root at 0)
{
    float e0[3], w0[3], d[3], dir[3];
    for (int m = 0; m < 3; m++) { e0[m] = W_ELBOW[m] * WING_K; w0[m] = W_WRIST[m] * WING_K; d[m] = w0[m] - e0[m]; }
    slerp_dir(e0, W_FOLD_ARM, clamp01(w->foldArm), dir); mad3(L[0], (float[3]){ 0, 0, 0 }, dir, len3(e0));
    slerp_dir(d, W_FOLD_FORE, clamp01(w->foldFore), dir); droop(dir, w->droopFore); mad3(L[1], L[0], dir, len3(d));
    for (int i = 0; i < 5; i++) {
        float f[3] = { W_TIPS[i][0] * WING_K - w0[0], W_TIPS[i][1] * WING_K - w0[1], W_TIPS[i][2] * WING_K - w0[2] }, l = len3(f);
        slerp_dir(f, W_FOLD_FING[i], clamp01(w->foldFing), dir); droop(dir, w->droopFing * 0.6f); mad3(L[2 + i], L[1], dir, l * KNUCKLE);
        // the outer joint folds further, curled down under the wing
        float fb[3] = { W_FOLD_FING[i][0] * 0.9f, W_FOLD_FING[i][1] - 0.75f, W_FOLD_FING[i][2] * 0.8f };
        slerp_dir(f, fb, clamp01(w->foldFing), dir); droop(dir, w->droopFing);
        if (w->curl[i] > 0.001f) { float cd[3] = { dir[0], dir[1] - 0.8f, dir[2] }; slerp_dir(dir, cd, w->curl[i], dir); }
        mad3(L[7 + i], L[2 + i], dir, l * (1.0f - KNUCKLE));
    }
}

static bool flying(void);
// Each great wing: swept back about the upright (`wingSweep`), raised about the body's length, its
// leading edge twisted down on the downstroke, folded along the back (`wingFold`). They roll with
// the body, flare as it falls (the air under them) and on a landing, rise and fall a little with
// its breath
static void wings_rig(void)
{
    bool fl = flying();
    float air = fl ? 0.0f : fminf(fmaxf(-s.altVel * 0.006f, -0.2f), 0.45f), br = fl ? 0.0f : 0.04f * fm_sinf(s.life * 1.7f - 0.4f);
    // The joints ripple with a beat: the wing half folds on the upstroke and the fingers trail it,
    // the outer ones last; standing, the fingers flex a little, slowly, each on its own
    // A beat runs down the wing: the arm leads, the forearm folds and droops after it, the fingers
    // last (they fold most on the upstroke and are flung open on the down); standing, they flex
    float L[12][3], ph0 = s.flapPh, f = s.flapOn;
    WingPose wp = {
        .foldArm  = s.wingFold + 0.10f * f * (0.5f - 0.5f * fm_sinf(ph0 - 0.3f)),
        .foldFore = s.wingFold + 0.38f * f * (0.5f - 0.5f * fm_sinf(ph0 - 0.7f)),
        .foldFing = s.wingFold + 0.48f * f * (0.5f - 0.5f * fm_sinf(ph0 - 1.0f)),
        .droopFore = 0.30f * f * fm_sinf(ph0 - 0.8f),
        .droopFing = 0.50f * f * fm_sinf(ph0 - 1.3f),
    };
    for (int i = 0; i < 5; i++)
        wp.curl[i] = f * 0.35f * (0.5f + 0.5f * fm_sinf(ph0 - 1.2f - 0.15f * i)) + (1.0f - f) * 0.07f * (0.5f + 0.5f * fm_sinf(s.life * 1.3f + i * 0.7f));
    wing_local(&wp, L);
    s.wingG = 1.0f - (1.0f - WING_FOLDED) * clamp01(s.wingFold / WING_REST_FOLD);     // held in small, growing as they spread
    for (int j = 0; j < 12; j++) for (int m = 0; m < 3; m++) L[j][m] *= s.wingG;
    for (int w = 0; w < 2; w++) {
        float sd = w ? 1.0f : -1.0f, ph = s.flapPh;
        float flap = 0.25f + 0.6f * fm_sinf(ph - 0.35f * fm_cosf(ph));      // a quick downstroke, a slower recovery
        float th = s.wingRaise + (flap - s.wingRaise) * s.flapAmt + sd * s.rollNow + air + s.wingKick + br + 0.15f * s.shakeNow;
        float sw = s.wingSweep, tw = -0.25f * fm_cosf(ph) * s.flapAmt;
        float c = fm_cosf(th), sn = fm_sinf(th), cs = fm_cosf(sw), ss = fm_sinf(sw), ct = fm_cosf(tw), st = fm_sinf(tw), wx[3], wy[3], wz[3];
        for (int m = 0; m < 3; m++) {
            float x1 = sd * s.R[m] * cs - s.F[m] * ss, z1 = s.F[m] * cs + sd * s.R[m] * ss;
            float y2 = -x1 * sn + UP[m] * c;
            wx[m] = x1 * c + UP[m] * sn; wy[m] = y2 * ct - z1 * st; wz[m] = z1 * ct + y2 * st;
        }
        float *root = s.wj[w][0];
        mad3(root, s.chest, UP, 10.0f); mad3(root, root, s.R, sd * 9.0f); mad3(root, root, s.F, 2.0f);
        root[1] += sd * s.rollNow * 9.0f;
        for (int j = 0; j < 12; j++)
            for (int m = 0; m < 3; m++) s.wj[w][1 + j][m] = root[m] + wx[m] * L[j][0] + wy[m] * L[j][1] + wz[m] * L[j][2];
        // never into the floor (6 clear): a joint that would dip under rides up on it, its bone
        // turning up (the wings stay their size; a low stroke or a sprawl just lifts the tips)
        for (int j = 1; j < 13; j++) s.wj[w][j][1] = fmaxf(s.wj[w][j][1], s.groundY + 6.0f);
        memcpy(s.wup[w], wy, 12);
    }
}

// Flying (and its landing): its wings, tail and body move as they always did in the air
static bool flying(void) { return s.state == DG_FLY || s.state == DG_SKYBEAM || s.state == DG_HOVER || s.state == DG_INTRO; }

static void rig(Boss *b, float dt)
{
    float yaw = b->rot[1];
    s.F[0] = fm_sinf(yaw); s.F[1] = 0.0f; s.F[2] = fm_cosf(yaw);
    s.R[0] = fm_cosf(yaw); s.R[1] = 0.0f; s.R[2] = -fm_sinf(yaw);
    // in the air each downstroke heaves the body up (lags the beat)
    float base = s.groundY + s.alt + (s.alt > 4.0f ? 3.0f * s.flapOn * fm_sinf(s.flapPh - 2.2f) : 0.0f), breath = fm_sinf(s.life * 1.7f);
    b->pos[1] = base;

    // The gait (each foot's swing, last frame): the chest and hips ride up a little over each
    // swinging leg and the body rolls off the lifted side; a turn banks it (more in the air)
    float gaitF = fmaxf(s.swing[0], s.swing[1]), gaitH = fmaxf(s.swing[2], s.swing[3]);
    bool fl = flying();
    float bank = fl ? 0.0f : fminf(fmaxf(-s.yawRate * 0.08f, -0.35f), 0.35f);
    s.shakeNow = s.shakeT > 0.0f && s.shakeT < 0.6f && s.alt < 1.0f ? fm_sinf(T3D_PI * s.shakeT / 0.6f) * fm_sinf(s.life * 32.0f) : 0.0f;
    s.rollNow = s.roll + bank + 0.05f * (s.swing[0] + s.swing[2] - s.swing[1] - s.swing[3]) + 0.12f * s.shakeNow;
    for (int m = 0; m < 3; m++) s.bodyUp[m] = UP[m] * fm_cosf(s.rollNow) - s.R[m] * fm_sinf(s.rollNow);

    mad3(s.chest, b->pos, s.F, CHEST_Z + s.lean);
    s.chest[1] = base + BODY_H + 2.0f - s.crouch + breath + s.rear + 0.35f * (s.shLift[0] + s.shLift[1]) + 1.2f * gaitF;     // `rear`: up on its hind legs
    mad3(s.hips, b->pos, s.F, HIPS_Z);
    s.hips[1] = base + BODY_H - s.crouch * 0.6f + 0.3f * breath + gaitH;
    // The spine bows sideways between them (`bend`; a turn bows it out behind the head)
    float bend = s.bend + (fl ? 0.0f : fminf(fmaxf(-s.yawRate * 6.0f, -6.0f), 6.0f));
    for (int j = 0; j < 2; j++) {
        lerp3(s.spine[j], s.chest, s.hips, (j + 1) / 3.0f);
        mad3(s.spine[j], s.spine[j], s.R, bend * 0.9f);
    }

    // The neck curves up from the chest and over, toward its yaw. Never quite still: it looks
    // about a little and nods with its breath (less as it aims at something)
    float idle = fl ? 0.0f : 1.0f - clamp01(s.aimK);
    float nyaw = s.neckYaw + idle * (0.06f * fm_sinf(s.life * 0.83f) + 0.03f * fm_sinf(s.life * 2.1f + 1.0f)) + 0.25f * s.shakeNow;
    float p0 = 0.6f + s.neckPitch + idle * 0.03f * fm_sinf(s.life * 1.7f - 0.6f), dir[3] = { 0 };
    memcpy(s.neck[0], s.chest, sizeof(s.chest));
    for (int i = 0; i < NNECK; i++) {
        float yi = yaw + nyaw * (i + 1) / NNECK, pi = p0 - 0.22f * i;
        dir[0] = fm_sinf(yi) * fm_cosf(pi); dir[1] = fm_sinf(pi); dir[2] = fm_cosf(yi) * fm_cosf(pi);
        mad3(s.neck[i + 1], s.neck[i], dir, NECK_SEG * s.neckReach);
        if (s.neck[i + 1][1] < s.groundY + 6.0f) s.neck[i + 1][1] = s.groundY + 6.0f;
    }
    memcpy(s.head, s.neck[NNECK], sizeof(s.head));
    float q = p0 - 0.22f * NNECK - 0.1f + idle * 0.04f * fm_sinf(s.life * 1.3f + 2.0f), yh = yaw + nyaw;
    s.hz[0] = fm_sinf(yh) * fm_cosf(q); s.hz[1] = fm_sinf(q); s.hz[2] = fm_cosf(yh) * fm_cosf(q);
    if (s.aimK > 0.01f) {
        float to[3] = { s.aim[0] - s.head[0], s.aim[1] - s.head[1], s.aim[2] - s.head[2] };
        norm3(to);
        lerp3(s.hz, s.hz, to, clamp01(s.aimK));
        norm3(s.hz);
    }
    cross3(s.hx, UP, s.hz); norm3(s.hx);
    cross3(s.hy, s.hz, s.hx);
    mad3(s.mouth, s.head, s.hz, 20.0f);
    mad3(s.mouth, s.mouth, s.hy, -1.0f);

    // The tail trails back and down, swaying, a wave running down it to the tip; `tail` swings it
    // to a side, `tailLag` (sprung) is its follow-through as the body turns
    memcpy(s.tl[0], s.hips, sizeof(s.hips));
    for (int i = 0; i < NTAIL; i++) {
        float k = (float)(i + 1) / NTAIL;
        float yi = yaw + T3D_PI + (fm_sinf(s.life * 1.4f - i * 0.7f) * (0.07f + 0.06f * k) + s.tail + s.tailLag * k) * k;
        float pi = -0.12f - 0.05f * i + s.tailUp * (0.35f + 0.12f * i) + 0.05f * fm_sinf(s.life * 0.9f - i * 0.5f) * k;     // curled up, more toward the tip
        dir[0] = fm_sinf(yi) * fm_cosf(pi); dir[1] = fm_sinf(pi); dir[2] = fm_cosf(yi) * fm_cosf(pi);
        mad3(s.tl[i + 1], s.tl[i], dir, TAIL_SEG);
        if (s.tl[i + 1][1] < s.groundY + 3.0f) s.tl[i + 1][1] = s.groundY + 3.0f;
    }

    // Shoulders and hips: front left, front right, hind left, hind right. They roll with the body,
    // a front one rides up as its paw is raised (`shLift`, the claw), each a little over its swinging leg
    for (int k = 0; k < 4; k++) {
        float side = (k & 1) ? 1.0f : -1.0f;
        mad3(s.shoulder[k], k < 2 ? s.chest : s.hips, s.R, side * (k < 2 ? 12.0f : 11.0f));
        s.shoulder[k][1] += (k < 2 ? -6.0f : -4.0f) + side * s.rollNow * (k < 2 ? 12.0f : 11.0f) + 2.0f * s.swing[k]
                          + (k < 2 ? s.shLift[k] : 0.0f);
        // never out of the torso's side (18 round): a raised shoulder came out of it, the leg off the body
        const float *c = k < 2 ? s.chest : s.hips;
        float o[3] = { s.shoulder[k][0] - c[0], s.shoulder[k][1] - c[1], s.shoulder[k][2] - c[2] }, l = len3(o);
        if (l > 14.0f) mad3(s.shoulder[k], c, o, 14.0f / l);
    }
    feet_update(dt);
    for (int k = 0; k < 4; k++) {       // elbows point back and out, knees forward and out
        float side = (k & 1) ? 1.0f : -1.0f, bend[3];
        for (int m = 0; m < 3; m++) bend[m] = s.F[m] * (k < 2 ? -1.0f : 0.8f) + s.R[m] * side * 0.5f + UP[m] * 0.2f;
        knee(s.shoulder[k], s.foot[k], bend, s.knee[k]);
    }
    wings_rig();

    // The hit capsule runs the length of the body, from the base of the neck (or the head,
    // when it is down) to the hips, at the height a sword swings (the spine is above it:
    // a capsule on the spine was out of reach). Its bottom stays at its lowest foot (in the air
    // it hung 8-14 below them), its front end rises with the chest when it rears, and it stops
    // where the neck's base and forefeet do (it reached 7 past them)
    float a[3], c[3], low = fminf(fminf(fminf(s.foot[0][1], s.foot[1][1]), fminf(s.foot[2][1], s.foot[3][1])), fminf(s.chest[1] - 18.0f, s.hips[1] - 16.0f));
    float axisY = low + HIT_R - 1.0f;
    bool headDown = s.head[1] - s.groundY < 30.0f;     // in sword reach (at 16 a head lowered 16-30 up was out of it)
    mad3(a, s.neck[1], s.F, -10.0f);
    if (headDown) memcpy(a, s.head, sizeof(a));
    else a[1] = fmaxf(fminf(s.chest[1] - 13.0f, fminf(fminf(s.foot[0][1], s.foot[1][1]), s.chest[1] - 18.0f) + HIT_R - 1.0f), axisY);
    memcpy(c, s.hips, sizeof(c));
    c[1] = axisY;
    for (int m = 0; m < 3; m++) {
        b->capsuleCollider.localCapA.v[m] = a[m] - b->pos[m];
        b->capsuleCollider.localCapB.v[m] = c[m] - b->pos[m];
    }
#ifdef HLE_LOG_HURTBOX   // the hit capsule against the drawn body: heights over the floor, reach along its facing, width at sword height
    {
        static int n;
        if (++n % 15 == 0) {
#define DOTF(p) (((p)[0] - b->pos[0]) * s.F[0] + ((p)[2] - b->pos[2]) * s.F[2])
#define DOTR(p) fabsf(((p)[0] - b->pos[0]) * s.R[0] + ((p)[2] - b->pos[2]) * s.R[2])
            float r = b->capsuleCollider.radius, gy = s.groundY;
            float lowF = fminf(s.chest[1] - 18.0f, fminf(s.foot[0][1], s.foot[1][1])) - gy, lowR = fminf(s.hips[1] - 16.0f, fminf(s.foot[2][1], s.foot[3][1])) - gy;
            float front = fmaxf(fmaxf(DOTF(s.neck[1]) + 10.0f, DOTF(s.chest) + 7.0f), fmaxf(DOTF(s.foot[0]), DOTF(s.foot[1])) + 8.0f), sn[3];
            mad3(sn, s.head, s.hz, 26.0f);
            if (s.head[1] - gy < 30.0f) front = fmaxf(front, fmaxf(DOTF(s.head), DOTF(sn)) + 6.0f);    // the head, when it is in reach
            float side = 0.0f;
            for (int k = 0; k < 4; k++) side = fmaxf(side, fmaxf(DOTR(s.foot[k]) + 4.0f, DOTR(s.shoulder[k]) + 8.0f) * (s.foot[k][1] - gy < 26.0f));
            if (fabsf(s.chest[1] - gy - 15.0f) < 18.0f) side = fmaxf(side, sqrtf(18.0f * 18.0f - (s.chest[1] - gy - 15.0f) * (s.chest[1] - gy - 15.0f)));
            debugf("[hurtbox] st %d alt %.0f | front end %.0f..%.0f over the floor (drawn lowest %.0f), rear %.0f..%.0f (drawn lowest %.0f) | reach %.0f drawn %.0f | half-width %.0f drawn %.0f\n",
                   s.state, (double)s.alt, (double)(a[1] - r - gy), (double)(a[1] + r - gy), (double)lowF, (double)(c[1] - r - gy), (double)(c[1] + r - gy), (double)lowR,
                   (double)(DOTF(a) + r), (double)front, (double)r, (double)side);
#undef DOTF
#undef DOTR
        }
    }
#endif
}

// ---- Blows checked on the pose as drawn (after the rig) ----

static float cone_grow(void) { float u = clamp01(s.cone.t / CONE_GROW); return 1.0f - (1.0f - u) * (1.0f - u); }    // its reach, eased out

static bool cone_hits(void)
{
    // the player within the drawn cone (its reach so far, its sides, its height there), body and all
    float k = cone_grow(), dx = character.pos[0] - s.cone.p[0], dz = character.pos[2] - s.cone.p[1];
    float fx = fm_sinf(s.cone.yaw), fz = fm_cosf(s.cone.yaw), along = dx * fx + dz * fz, across = dx * fz - dz * fx, d = sqrtf(dx * dx + dz * dz);
    if (d - PLAYER_R > CONE_L * k || along < -PLAYER_R) return false;
    float a = atan2f(fabsf(across), fmaxf(along, 0.0f)), m = d > PLAYER_R ? asinf(PLAYER_R / d) : T3D_PI;
    if (a > CONE_HALF + m) return false;
    float f = fminf(d / (CONE_L * k), 1.0f);
    return character.pos[1] - s.groundY < cone_h(f, fminf(a, CONE_HALF)) * k + 1.0f;
}

// The tail as drawn, now and where it passed since the last frame (a fast swing moves the blade
// ~20-50 a frame, more than the tail is thick: three in-between poses, flat, the low tail cleared
// by a jump as before)
static float seg_dist_xz(const float a[3], const float b[3], float px, float pz)
{
    float ux = b[0] - a[0], uz = b[2] - a[2], l2 = ux * ux + uz * uz;
    float t = l2 > 1e-4f ? clamp01(((px - a[0]) * ux + (pz - a[2]) * uz) / l2) : 0.0f;
    float ex = a[0] + ux * t - px, ez = a[2] + uz * t - pz;
    return sqrtf(ex * ex + ez * ez);
}
static float tail_sweep_gap(void)     // < PLAYER_R: a hit
{
    float cur[NTAIL + 2][3], g = tail_gap();
    memcpy(cur, s.tl, sizeof(s.tl));
    tail_tip(cur[NTAIL + 1]);
    if (tailPrevOk && g >= PLAYER_R && character.pos[1] - s.groundY < 10.0f)
        for (int k = 1; k <= 3; k++)
            for (int i = 0; i <= NTAIL; i++) {
                float a[3], c[3];
                lerp3(a, tailPrev[i], cur[i], k * 0.25f); lerp3(c, tailPrev[i + 1], cur[i + 1], k * 0.25f);
                g = fminf(g, seg_dist_xz(a, c, character.pos[0], character.pos[2]) - (i < NTAIL ? tail_r(i) : 3.4f));
            }
    memcpy(tailPrev, cur, sizeof(cur)); tailPrevOk = true;
    return g;
}

static void hits_after_rig(Boss *b, float dt)
{
    if (s.state == DG_TAIL && s.step == 1) {
        float g = tail_sweep_gap();
        HB_TOUCH(HB_TAIL, g - 8.0f);
        if (g < PLAYER_R && !s.hitDone) { s.hitDone = true; HB_HIT(HB_TAIL, g - 8.0f); character_apply_damage(28.0f); }
    } else if (s.state == DG_WHIP && s.step == 1) {
        float g = fminf(tail_sweep_gap(), body_gap());      // the head and body spin round too
        HB_TOUCH(HB_WHIP, g - 8.0f);
        if (g < PLAYER_R && !s.hitDone) { s.hitDone = true; HB_HIT(HB_WHIP, g - 8.0f); character_apply_damage(22.0f); }
    } else if (s.state == DG_CHARGE && s.step == 1) {
        // the body as drawn (it was a flat 25 round the head-hips line, wider than the neck and head)
        HB_TOUCH(HB_CHARGE, body_gap() - 8.0f);
        if (!s.hitDone && body_gap() < SHOVE_R) { s.hitDone = true; HB_HIT(HB_CHARGE, body_gap() - 8.0f); character_apply_damage(28.0f); }
    } else tailPrevOk = false;
    if (s.cone.on) {
        s.cone.t += dt;
        if (s.cone.t < CONE_GROW + CONE_HOLD) {
#ifdef HLE_LOG_HURTBOX
            {   // the gap to the drawn cone: outside its reach or sides (as a distance), or above it
                float k = cone_grow(), dx = character.pos[0] - s.cone.p[0], dz = character.pos[2] - s.cone.p[1], d = sqrtf(dx * dx + dz * dz);
                float fx = fm_sinf(s.cone.yaw), fz = fm_cosf(s.cone.yaw), along = dx * fx + dz * fz, across = dx * fz - dz * fx;
                float side = fabsf(across) * fm_cosf(CONE_HALF) - fmaxf(along, 0.0f) * fm_sinf(CONE_HALF);   // past its side edge
                HB_TOUCH(HB_CONE, fmaxf(fmaxf(d - CONE_L * k, side), -along) - 8.0f);
            }
#endif
            if (!s.cone.hit && cone_hits()) { s.cone.hit = true; HB_HIT(HB_CONE, 0.0f); character_apply_damage(18.0f); }
        } else if (s.cone.t >= CONE_GROW + CONE_HOLD + CONE_FADE) s.cone.on = false;
    }
}

// ---- Moves ----

static void claw_strike(Boss *b, int k)
{
    float c[3];
    mad3(c, s.chest, s.F, 18.0f);
    c[1] = s.groundY + 6.0f;
    float yaw = b->rot[1] + (k ? 0.35f : -0.35f);
    skill_fx_spawn_arc_hostile(c, -yaw, 46.0f, 16.0f);      // the arc faces (-sin, cos) of its yaw
    screen_shake_set_shake_mag(5.0f);
}

static void claw_update(Boss *b, float dt)
{
    // Two swipes, left then right: step out of reach, or roll through
    face_player(b, dt, 1.6f);
    int k = s.step < 2 ? 0 : 1;
    float side = k ? 1.0f : -1.0f, up = (s.step == 0 || s.step == 2) && s.t < 0.6f ? 1.0f : 0.0f;
    s.tCrouch = 3.0f; s.tNeckPitch = -0.15f;
    // the raised side heaves up, the head and spine follow the paw, the tail swings against it
    s.tRoll = side * 0.05f * up; s.tShLift[k] = 0.0f; s.tLean = up > 0.0f ? -2.0f : 3.0f; s.tBend = side * 2.5f;
    s.tNeckYaw = side * 0.15f; s.tTail = -side * 0.15f; s.tWingRaise = 0.3f + 0.15f * up;
    float wind = (k ? 0.48f : 0.66f) / (s.dark ? 1.2f : 1.0f);     // the raised leg's windup (20% longer: easier to read)
    if (s.step == 0 || s.step == 2) {
        s.tClaw[k] = s.t < wind - 0.07f ? 1.0f : 0.0f;     // raised, then slammed down onto the strike
        // Both swipes: the Guardian's unused swing1 (at his level), peaking 0.36 s in, on the strike
        float lead = wind - 0.34f;
        if (s.t >= lead && s.t - dt < lead) sfx(SCENE1_SFX_BOSS_SWING1, 1.0f);
        if (s.t >= wind) { claw_strike(b, k); s.step++; s.t = 0.0f; }
    } else if (s.step == 1) {
        s.tClaw[0] = 0.0f;
        if (s.t >= 0.3f) { s.step = 2; s.t = 0.0f; }
    } else if (s.t >= 0.7f) enter(DG_IDLE);
}

static void bite_update(Boss *b, float dt)
{
    // Rears back, then lunges low and snaps: the deadliest hit, so keep out of its face
    float dx, dz;
    to_player(b, &dx, &dz);
    if (s.step == 0) {
        face_player(b, dt, 2.0f);
        s.tNeckReach = 0.8f; s.tNeckPitch = 0.35f; s.tJaw = 0.7f; s.poseRate = 6.0f;
        s.tCrouch = 3.0f; s.tRear = 3.0f; s.tLean = -3.0f; s.tWingRaise = 0.5f; s.tTailUp = 0.1f;    // drawn back on its haunches
        s.target[0] = character.pos[0]; s.target[1] = s.groundY + 8.0f; s.target[2] = character.pos[2];
        if (s.t >= 0.6f / (s.dark ? 1.2f : 1.0f)) {
            // Reach to land the mouth on the target: the lowered neck spans about 52 per unit of
            // reach in front of the chest, the jaws about 12 more
            float hx = s.target[0] - s.chest[0], hz = s.target[2] - s.chest[2];
            // Past what the neck reaches unstretched, the whole body lunges the rest (the neck was
            // drawn up to 1.7 times its length)
            float want = fminf(fmaxf((sqrtf(hx * hx + hz * hz) - 12.0f) / 52.0f, 0.8f), 1.7f);
            s.range = fminf(want, 1.15f); s.sg = (want - s.range) * 52.0f;
            s.step = 1; s.t = 0.0f; sfx_lunge();
        }
    } else if (s.step == 1) {
        s.tNeckReach = s.range; s.tNeckPitch = -0.75f; s.tJaw = 0.9f; s.poseRate = 16.0f;
        s.tCrouch = 5.0f; s.tWingRaise = 0.6f; s.tWingSweep = 0.7f; s.tTailUp = 0.2f;     // thrown in behind its head, wings flung back
        memcpy(s.aim, s.target, sizeof(s.aim)); s.tAimK = 1.0f;
        if (s.sg > 0.0f && s.t < 0.15f) move_by(b, s.F[0] * s.sg * dt / 0.15f, s.F[2] * s.sg * dt / 0.15f);
        if (s.t >= 0.1f) {     // the head as drawn, as it comes down (it hit anyone within 22 of the mouth, flat)
            HB_TOUCH(HB_BITE, head_gap() - 8.0f);
            if (!s.hitDone && head_gap() < PLAYER_R) { s.hitDone = true; HB_HIT(HB_BITE, head_gap() - 8.0f); character_apply_damage(38.0f); }
        }
        if (s.t >= 0.2f) {
            s.tJaw = 0.0f;
            screen_shake_set_shake_mag(6.0f);
            sfx_boom();
            s.step = 2; s.t = 0.0f;
        }
    } else {
        s.tNeckReach = s.range; s.tNeckPitch = -0.7f; s.tCrouch = 4.0f; s.tLean = 2.0f; s.poseRate = 4.0f;    // its head lies low: hit it
        if (s.t >= 0.9f) enter(DG_IDLE);
    }
}


static void tail_update(Boss *b, float dt)
{
    // The answer to standing behind it: it looks back over its shoulder at you, cocks the tail
    // out beside the hind leg on your side (its crystal blade glowing), the hips and spine
    // coiling, then scythes it in a wide fan across behind it to beside the other hind leg, its
    // hips twisting into the blow: one swipe, from the side away from you toward you. Out of the tail's reach, it backs toward you as it coils. No spin: jump it, roll through,
    // or get out in front
    const float SWING = 0.45f, FAN = 1.95f, REACH = 105.0f;     // the fan: FAN each side of straight back
    float dx, dz;
    to_player(b, &dx, &dz);
    float rel = remainderf(atan2f(dx, dz) - b->rot[1], 2.0f * T3D_PI), sd = s.sweepSpan > 0.0f ? 1.0f : -1.0f;
    if (s.step == 0) {
        if (s.t < dt * 1.5f) {
            // from the side away from the player, right across toward and through them
            float pr = remainderf(atan2f(dx, dz) - b->rot[1] - T3D_PI, 2.0f * T3D_PI);   // the player from the tail's line
            float sg = pr > 0.0f ? -1.0f : 1.0f;
            s.sweepFrom = sg * FAN; s.sweepSpan = -2.0f * sg * FAN;
            s.sweeps = 0;
            sd = s.sweepSpan > 0.0f ? 1.0f : -1.0f;
        }
        s.tNeckYaw = rel > 0.0f ? 1.1f : -1.1f; s.tNeckPitch = 0.2f;    // a look back at you
        s.tTail = s.sweepFrom; s.tCrouch = 4.0f; s.poseRate = 4.0f;
        s.tBend = sd * 5.0f; s.tRoll = sd * 0.08f; s.tTailUp = 0.15f; s.tWingRaise = 0.45f;     // coiled against the swing
        {   // backs in until the player is in reach of the tail's root
            float hx = character.pos[0] - s.hips[0], hz = character.pos[2] - s.hips[2], dh = sqrtf(hx * hx + hz * hz);
            if (dh > REACH) move_by(b, -s.F[0] * fminf(dh - REACH, 140.0f * dt), -s.F[2] * fminf(dh - REACH, 140.0f * dt));
        }
        if (s.t >= 0.5f && s.t - dt < 0.5f) sfx_swing();
        if (s.t >= 0.65f) { s.step = 1; s.t = 0.0f; }
    } else if (s.step == 1) {
        s.tTail = s.sweepFrom + s.sweepSpan; s.poseRate = 13.0f;
        s.tBend = -sd * 6.0f; s.tRoll = -sd * 0.12f; s.tCrouch = 5.0f; s.tTailUp = 0.0f;
        b->rot[1] += sd * 0.7f * dt * (1.0f - s.t / SWING) * 2.0f;   // the hips twist with it (~0.25 rad)
        // (the hit: hits_after_rig, on the tail as drawn)
        if (s.t >= SWING) {
            screen_shake_set_shake_mag(6.0f);
            s.step = 2; s.t = 0.0f;     // one swipe only
        }
    } else {
        s.poseRate = 3.0f;
        if (s.t >= 0.6f) enter(DG_IDLE);
    }
}

static void whip_update(Boss *b, float dt)
{
    // Spins a full turn with its tail out, a scything whip all round it: jump it, roll
    // through, or be out of the tail's reach. Told by its crouch, head dipped, and its tail curled up
    // over its back like a scorpion's (its resting pose has the tail trailing low)
    const float WIND = 0.8f / (s.dark ? 1.2f : 1.0f), SPIN = 0.6f;
    if (s.step == 0) {      // it doesn't turn to face you: the spin reaches all round
        s.tCrouch = 7.0f; s.tNeckPitch = -0.4f; s.tTail = 0.0f; s.tTailUp = 1.0f; s.poseRate = 5.0f;
        s.tBend = 5.0f; s.tRoll = -0.08f; s.tWingRaise = 0.45f; s.tWingSweep = 0.3f;     // coiled to spin
        if (s.t >= WIND) { s.step = 1; s.t = 0.0f; sfx_swing(); }
    } else if (s.step == 1) {
        b->rot[1] = remainderf(b->rot[1] + 2.0f * T3D_PI * dt / SPIN, 2.0f * T3D_PI);     // counter-clockwise on screen
        s.tCrouch = 5.0f; s.tTail = 0.0f; s.poseRate = 12.0f;
        s.tWingRaise = 0.15f; s.tWingSweep = 0.0f; s.tWingSpan = 1.0f;      // flung out by the spin
        // the whole warned circle, as the tail and body sweep it (hits_after_rig: it hit everyone
        // in the circle 0.18 s in, wherever the tail was)
        if (s.t >= SPIN) {
            skill_fx_spawn_burst_hostile((float[3]){ s.tl[NTAIL][0], s.groundY, s.tl[NTAIL][2] }, 0.0f);
            screen_shake_set_shake_mag(7.0f);
            sfx_boom();
            s.step = 2; s.t = 0.0f;
        }
    } else {
        s.poseRate = 3.0f;
        if (s.t >= 0.8f) enter(DG_IDLE);       // dizzy a moment: the opening
    }
}

// A point on the floor at `dist` from the root, at world yaw `a`
static void floor_point(const Boss *b, float a, float dist, float out[3])
{
    out[0] = b->pos[0] + fm_sinf(a) * dist; out[1] = s.groundY; out[2] = b->pos[2] + fm_cosf(a) * dist;
}

static void breath_update(Boss *b, float dt)
{
    // Breathes in, then sweeps a torrent of fire across the floor in front of it
    float dx, dz, dist = to_player(b, &dx, &dz);
    if (s.step == 0) {
        face_player(b, dt, 1.5f);
        s.tNeckPitch = 0.45f; s.tNeckReach = 0.9f; s.tJaw = 0.5f;
        s.tRear = 4.0f * smooth01(s.t / 0.7f); s.tLean = -3.0f; s.tWingRaise = 0.55f; s.tWingSweep = 0.3f; s.tTailUp = 0.1f;   // the chest swells: it breathes in
        s.glow = 0.5f + 0.5f * clamp01(s.t / 0.9f);
        if (s.t < dt * 1.5f) {
            float sg = (pick_rng() & 1) ? 1.0f : -1.0f;
            s.sweepFrom = atan2f(dx, dz) - 0.75f * sg; s.sweepSpan = 1.5f * sg;
            s.range = fminf(fmaxf(dist, 50.0f), 190.0f);     // out to where the player is (it's chosen to 200)
        }
        if (s.t >= 0.9f / (s.dark ? 1.2f : 1.0f)) { s.step = 1; s.t = 0.0f; s.emit = 0.0f; sfx_lunge(); }
    } else if (s.step == 1) {
        float a = s.sweepFrom + s.sweepSpan * clamp01(s.t / 1.6f);
        float to[3];
        floor_point(b, a, s.range, to);
        memcpy(s.aim, to, sizeof(s.aim)); s.tAimK = 1.0f;
        s.tJaw = 0.9f; s.tNeckPitch = -0.15f; s.glow = 1.0f;
        s.tCrouch = 3.0f; s.tLean = 3.0f; s.tWingRaise = 0.3f; s.tBend = -(s.sweepSpan > 0.0f ? 1.0f : -1.0f) * 3.0f;     // and leans into the torrent
        turn_toward(b, a, 0.8f, dt);
        breathe(to, 120.0f, 0.8f, dt);
        if (s.t >= 1.6f) { s.step = 2; s.t = 0.0f; s.glow = 0.0f; }
    } else if (s.t >= 0.6f) enter(DG_IDLE);
}

static void charge_update(Boss *b, float dt)
{
    // Roars, then rushes headlong: roll aside
    if (s.step == 0) {
        face_player(b, dt, 2.5f);
        s.tNeckPitch = 0.7f; s.tJaw = 1.0f; s.tCrouch = 4.0f;
        s.tRear = 5.0f; s.tWingRaise = 0.8f; s.tWingSweep = 0.15f; s.tWingSpan = 1.0f; s.tTailUp = 0.2f;     // up, wings spread, roaring
        if (s.t < dt * 1.5f) sfx_roar();
        if (s.t >= 0.28f && s.t - dt < 0.28f) screen_shake_set_shake_mag(4.0f);     // as the roar comes in
        if (s.t >= 0.9f / (s.dark ? 1.2f : 1.0f)) { s.step = 1; s.t = 0.0f; sfx_lunge(); }
    } else if (s.step == 1) {
        s.tNeckPitch = -0.2f; s.tNeckReach = 1.15f; s.tCrouch = 6.0f + 2.5f * fm_sinf(s.t * 16.0f); s.tJaw = 0.3f;    // a bounding gallop
        s.tLean = 4.0f; s.tWingRaise = 0.35f; s.tWingSweep = 1.0f; s.tWingSpan = 0.8f; s.poseRate = 12.0f;
        bool moved = move_by(b, s.F[0] * 240.0f * dt, s.F[2] * 240.0f * dt);     // (the hit: hits_after_rig)
        if (!moved || s.t >= 0.6f) { s.step = 2; s.t = 0.0f; screen_shake_set_shake_mag(6.0f); sfx_step(); s.crouch += 3.0f; }
    } else if (s.t >= 0.6f) { passed_player(); enter(DG_IDLE); }
}

// Touching down: bursts at its feet and a quake, harmless (only its moves hit)
static void land_slam(float shake)
{
    s.heavyRest = true;
    passed_player();                    // (a flight often ends past the player)
    s.shakeT = 0.9f;                    // a shake-off once it has its feet
    for (int i = 0; i < 4; i++) skill_fx_spawn_burst_hostile((float[3]){ s.foot[i][0], s.groundY, s.foot[i][2] }, i * 1.5f);
    screen_shake_set_shake_mag(shake);
    sfx_boom();
}

static void fly_update(Boss *b, float dt)
{
    // Takes off, strafes the hall breathing fire down onto the floor, lands with a quake
    s.tWingSpan = 1.0f;
    if (s.step == 0) {
        s.flapRate = 9.0f;
        s.alt = ease(s.alt, 95.0f, 2.2f, dt);
        face_player(b, dt, 2.0f);
        {   // close, it backs off as it climbs: the strafe starts with its jaws short of the player
            // (taking off near them, its mouth was already past them: the rain fell behind them)
            float dx, dz, d = to_player(b, &dx, &dz);
            if (d < 150.0f) move_by(b, -s.F[0] * 60.0f * dt, -s.F[2] * 60.0f * dt);
        }
        if (s.t >= 1.1f) {
            // Over the player and on past it, as far as the hall allows
            float dx, dz, d = to_player(b, &dx, &dz);
            d = fmaxf(d, 1.0f);
            float len = 170.0f;
            do {
                s.flyTo[0] = character.pos[0] + dx / d * len; s.flyTo[1] = character.pos[2] + dz / d * len;
                len -= 20.0f;
            } while (len > -60.0f && !inside(s.flyTo[0], s.flyTo[1], 90.0f));
            s.step = 1; s.t = 0.0f; s.emit = 0.0f;
        }
    } else if (s.step == 1) {
        s.flapRate = 9.0f;
        float dx = s.flyTo[0] - b->pos[0], dz = s.flyTo[1] - b->pos[2], d = sqrtf(dx * dx + dz * dz);
        turn_toward(b, atan2f(dx, dz), 3.0f, dt);
        float st = fminf(d, 150.0f * dt);
        float px = character.pos[0] - s.mouth[0], pz = character.pos[2] - s.mouth[2];
        bool over = px * px + pz * pz < 95.0f * 95.0f;     // the mouth near the player (the root trailed it by ~90)
        if (d > 1e-3f && inside(b->pos[0] + dx / d * st, b->pos[2] + dz / d * st, 40.0f)) { b->pos[0] += dx / d * st; b->pos[2] += dz / d * st; }
        // the wall: over the player it hangs there a moment, still breathing, then lands (it landed at
        // once: by a wall, where the player often is, it took off and came straight down, no fire)
        else { s.t = over ? fmaxf(s.t, 2.3f) : 9.0f; s.hitDone = true; }     // (hitDone: hanging there)
        if (over) {     // over the player: fire rains down
            // down under its jaws, a little ahead: the rain sweeps across the floor under its path
            // (aimed 25 ahead of the mouth, it landed past a player it was over)
            // Hanging at a wall, short of them, it breathes at the player instead
            float to[3];
            mad3(to, s.mouth, s.F, 10.0f);
            if (s.hitDone) { to[0] = character.pos[0]; to[2] = character.pos[2]; }
            to[1] = s.groundY;
            memcpy(s.aim, to, sizeof(s.aim)); s.tAimK = 0.8f; s.tJaw = 0.9f;
            // fast enough to reach the floor (at 110 they stalled ~60 up, over the knight's head:
            // the rain never hit) and long-lived enough to burn there a moment along its path
            breathe(to, 300.0f, 1.2f, dt);
        }
        if (d < 6.0f || s.t > 3.5f) { s.step = 2; s.t = 0.0f; }
    } else if (s.step == 2) {
        s.flapRate = 6.0f;
        s.alt = ease(s.alt, 0.0f, 3.0f, dt);
        if (s.alt < 1.0f) {
            s.alt = 0.0f;
            land_slam(12.0f);
            s.step = 3; s.t = 0.0f;
        }
    } else if (s.t >= 0.6f) enter(DG_IDLE);
}

static void roar_update(Boss *b, float dt)
{
    // Half health: the dark takes it. A long roar; its crystals turn violet
    s.alt = ease(s.alt, 0.0f, 3.0f, dt);
    s.tNeckPitch = 0.8f; s.tJaw = 1.0f; s.tWingSpan = 1.0f; s.tWingRaise = 0.7f; s.tCrouch = 2.0f;
    s.tRear = 7.0f; s.tWingSweep = 0.0f; s.tTailUp = 0.3f; s.tNeckYaw = 0.08f * fm_sinf(s.life * 9.0f);     // reared, shaking with it
    if (s.t <= dt) sfx_roar();      // loud 0.28 s in: the bursts from then
    if (s.step < 8 && s.t >= 0.25f + 0.3f * s.step) {
        float a = s.step * 0.8f;
        skill_fx_spawn_burst_hostile((float[3]){ b->pos[0] + fm_sinf(a) * 45.0f, s.groundY, b->pos[2] + fm_cosf(a) * 45.0f }, a);
        screen_shake_set_shake_mag(8.0f);
        s.step++;
    }
    if (s.t >= 2.4f) { s.alt = 0.0f; b->mode = BOSS_MODE_COMBAT; s.cdNova = 1.0f; enter(DG_IDLE); }
}

static void nova_update(Boss *b, float dt)
{
    // Gathers the dark (bursts at its feet), then it bursts outward: get far away or roll
    if (s.step == 0) {
        s.tCrouch = 6.0f; s.tWingRaise = 0.9f; s.tWingSpan = 1.0f; s.tNeckPitch = 0.5f; s.tJaw = 0.6f;
        s.tWingSweep = -0.15f; s.tTailUp = 0.3f; s.tRoll = 0.03f * fm_sinf(s.life * 23.0f);      // wings cupped forward round it, trembling
        s.glow = clamp01(s.t / 1.7f);
        if (s.t >= 1.42f && s.t - dt < 1.42f) sfx(SCENE1_SFX_DARK_NOVA, 1.0f);    // loud 0.28 s on: the burst
        if (s.t >= 1.7f) {
            float c[3] = { b->pos[0], s.groundY + 6.0f, b->pos[2] };
            skill_fx_spawn_arc_hostile(c, b->rot[1], 120.0f, 24.0f);
            skill_fx_spawn_arc_hostile(c, b->rot[1] + T3D_PI, 120.0f, 24.0f);
            screen_shake_set_shake_mag(14.0f);
            sfx_boom();
            s.glow = 0.0f; s.wingKick = 0.4f;
            s.step = 1; s.t = 0.0f;
        }
    } else {
        s.tWingRaise = 0.2f; s.tWingSweep = 0.05f; s.tWingSpan = 1.0f; s.tRear = 3.0f;     // flung open by the burst
        if (s.t >= 1.0f) enter(DG_IDLE);
    }
}

static void orbs_update(Boss *b, float dt)
{
    // Looses dark orbs that hang, then home in on the player: lead them into the floor
    face_player(b, dt, 1.2f);
    s.tNeckPitch = 0.6f; s.tJaw = 0.6f;
    s.tRear = 3.0f; s.tWingRaise = 0.8f; s.tWingSweep = 0.05f; s.tWingSpan = 1.0f;
    int n = 4;
    if (s.step < n && s.t >= 0.15f * s.step) {
        Orb *o = &s.orb[s.orbNext];
        s.orbNext = (s.orbNext + 1) % ORB_MAX;
        float side = (s.step & 1) ? 1.0f : -1.0f;
        mad3(o->p, s.head, s.R, side * (12.0f + 6.0f * (s.step >> 1)));
        o->p[1] += 10.0f + 6.0f * (s.step >> 1);
        o->v[0] = o->v[1] = o->v[2] = 0.0f;
        o->age = -0.1f * s.step;
        o->on = true;
        sfx(SCENE1_SFX_SPELL_CAST_BOSS, 0.7f);
        s.step++;
    }
    if (s.t >= 1.6f) enter(DG_IDLE);
}

// How far from the root to aim the beam at the floor so that it crosses chest height (12 up) at
// `dist` (the player's distance from the root): from the mouth's height now, by similar triangles.
// (It was aimed 40 past the player: from a mouth ~60 up that passed over the head at mid range)
static float beam_reach(const Boss *b, float dist)
{
    float mo = (s.mouth[0] - b->pos[0]) * s.F[0] + (s.mouth[2] - b->pos[2]) * s.F[2];     // the mouth, ahead of the root
    float h = s.mouth[1] - s.groundY, dp = fmaxf(dist - mo, 10.0f);
    return fminf(mo + dp * h / fmaxf(h - 12.0f, 4.0f), 280.0f);
}

static void beam_update(Boss *b, float dt)
{
    // Midir's laser: it rears up and gathers the dark (wings high, the light in its jaws
    // swelling, a burst where the sweep will start), then rakes a beam across the hall from
    // one side to the other, the floor exploding behind it; one to three times (at random), the
    // first from either side, each after back the other way after a short gathering (the light
    // swelling again, a burst where it will start). Roll through each or get under its chin.
    // A short, heavy recovery after, its head low (no longer spent on the floor)
    float dx, dz, dist = to_player(b, &dx, &dz);
    const float CHARGE = 1.4f, SWEEP = 2.0f, RECHARGE = 0.6f;
    if (s.step == 0) {
        face_player(b, dt, 2.0f);
        s.tNeckPitch = 0.9f; s.tNeckReach = 0.85f; s.tJaw = 0.8f; s.tCrouch = 4.0f; s.tWingRaise = 0.75f; s.tWingSpan = 1.0f; s.poseRate = 4.0f;
        s.tRear = 5.0f; s.tWingSweep = 0.1f; s.tTailUp = 0.2f;
        s.glow = clamp01(s.t / CHARGE);
        if (s.t < dt * 1.5f) {
            float sg = (pick_rng() & 1) ? 1.0f : -1.0f;
            s.sweeps = 1 + (int)(pick_rng() % 3);
#ifdef HLE_DG_SWEEPS    // test aid: always this many sweeps
            s.sweeps = HLE_DG_SWEEPS;
#endif
            s.heavyRest = s.sweeps == 3;
#ifdef HLE_AUTOPLAY
            debugf("[dragon] beam: %d sweeps, the first %s\n", s.sweeps, sg > 0.0f ? "left to right" : "right to left");
#endif
            s.sweepFrom = atan2f(dx, dz) - 1.2f * sg; s.sweepSpan = 2.4f * sg;
            s.range = dist;     // where the player is: beam_reach aims it through their chest
            sfx(SCENE1_SFX_SPELL_CAST_BOSS, 0.8f);
        }
        if (s.t >= CHARGE * 0.55f && s.t - dt < CHARGE * 0.55f) {
            float p[3];
            floor_point(b, s.sweepFrom, beam_reach(b, s.range), p);
            skill_fx_spawn_burst_hostile(p, 0.0f);     // where it will begin
        }
        if (s.t >= CHARGE) { s.step = 1; s.t = 0.0f; s.emit = 0.0f; sfx(SCENE1_SFX_DARK_BEAM, 1.0f); screen_shake_set_shake_mag(6.0f); }
    } else if (s.step == 1) {
        float u = clamp01(s.t / SWEEP), a = s.sweepFrom + s.sweepSpan * u * u * (3.0f - 2.0f * u);   // eased in and out
        float to[3];
        floor_point(b, a, beam_reach(b, s.range), to);
        s.tJaw = 1.0f; s.tNeckPitch = 0.15f; s.tCrouch = 3.0f + 0.6f * fm_sinf(s.life * 31.0f); s.tWingRaise = 0.5f; s.poseRate = 6.0f;
        s.tLean = -2.0f; s.tBend = -(s.sweepSpan > 0.0f ? 1.0f : -1.0f) * 3.0f;      // braced against it, shuddering
        turn_toward(b, a, 1.1f, dt);       // the body swings round behind its head
        beam_at(to, 3.0f, 21.0f, true, dt);
        screen_shake_set_shake_mag(2.5f);
        if (s.t >= SWEEP) {
            s.beamOn = false; s.t = 0.0f;
            if (--s.sweeps > 0) {       // again, from where this one ended, back across where you are now
                float sg = s.sweepSpan > 0.0f ? 1.0f : -1.0f, p[3];
                s.sweepFrom = atan2f(dx, dz) + 1.2f * sg; s.sweepSpan = -2.4f * sg;
                s.range = dist;
                floor_point(b, s.sweepFrom, beam_reach(b, s.range), p);
                skill_fx_spawn_burst_hostile(p, 0.0f);
                sfx(SCENE1_SFX_SPELL_CAST_BOSS, 0.7f);
                s.glow = 0.45f; s.step = 2;
            } else { s.glow = 0.0f; s.step = 3; }
        }
    } else if (s.step == 2) {
        // Gathers again: the head swings to the new start, the light swelling and flickering
        float p[3];
        floor_point(b, s.sweepFrom, beam_reach(b, s.range), p);
        lerp3(s.aim, s.beamP, p, smooth01(s.t / (RECHARGE * 0.8f))); s.tAimK = 1.0f;    // from where the last ended
        s.tJaw = 0.7f; s.tNeckPitch = 0.5f; s.tCrouch = 4.0f; s.tWingRaise = 0.65f; s.poseRate = 5.0f;
        turn_toward(b, s.sweepFrom, 0.8f, dt);
        s.glow = 0.45f + 0.55f * smooth01(s.t / RECHARGE) + 0.12f * fm_sinf(s.life * 26.0f);
        if (s.t >= RECHARGE) { s.step = 1; s.t = 0.0f; s.emit = 0.0f; s.hitDone = false; sfx(SCENE1_SFX_DARK_BEAM, 1.0f); screen_shake_set_shake_mag(6.0f); }
    } else {
        s.tCrouch = 6.0f; s.tNeckPitch = -0.5f; s.tJaw = 0.4f; s.poseRate = 3.0f;    // it settles, head low...
        if (s.t >= 0.5f) { enter(DG_IDLE); s.idleWait = 3.0f; }      // ...into a breather, panting: the opening it pays for the beam
    }
}


static void skybeam_update(Boss *b, float dt)
{
    // Bayle's flying breath: it rises, sliding out to one side of you, the dark swelling in its
    // jaws; banks into a line across your front (a burst marks where the beam will touch down),
    // then glides along it, raking the beam down onto the floor in a line beside its flight path
    // that runs through where you stand, explosions in its wake; it glides to a stop and lands.
    // Read its heading: step off the line, roll through the beam, or get in under it
    const float RISE = 1.2f, BANK = 0.7f, RAKE = 1.9f, GLIDE = 0.7f, LAND = 1.1f, ALT = 75.0f, DIP = 15.0f;
    s.tWingSpan = 1.0f;
    float *P0 = s.beamFrom, *D = s.target, *off = s.flyTo;      // the line's start, its heading, the beam's reach to the side
    if (s.step == 0) {
        if (s.t < dt * 1.5f) {
            // A line across the player's front, through its own spot: the side and length that fit the hall
            float dx, dz, d = fmaxf(to_player(b, &dx, &dz), 1.0f), nx = dx / d, nz = dz / d, in = d - 100.0f;    // the line 100 out from the player
            float sg = (pick_rng() & 1) ? 1.0f : -1.0f;
            bool ok = false;
            for (int i = 0; i < 12 && !ok; i++) {
                float sd = (i & 1) ? -sg : sg, L = 240.0f - 30.0f * (i >> 1);
                D[0] = nz * sd; D[1] = 0.0f; D[2] = -nx * sd;
                float sx = b->pos[0] + nx * in - D[0] * L * 0.5f, sz = b->pos[2] + nz * in - D[2] * L * 0.5f;
                ok = inside(sx, sz, 70.0f) && inside(sx + D[0] * L * 0.5f, sz + D[2] * L * 0.5f, 70.0f) && inside(sx + D[0] * L, sz + D[2] * L, 70.0f);
                if (ok) { s.range = L; off[0] = sx; off[1] = sz; }
            }
            if (!ok) { enter(DG_FLY); return; }      // no room for a line here: the fire strafe instead
            P0[0] = b->pos[0]; P0[2] = b->pos[2];
            sfx(SCENE1_SFX_SPELL_CAST_BOSS, 0.8f);
        }
        s.flapRate = 9.0f;
        float u = smooth01(s.t / RISE);         // up, sliding out to the line's start
        s.alt = ALT * u;
        float x = P0[0] + (off[0] - P0[0]) * u, z = P0[2] + (off[1] - P0[2]) * u;
        if (inside(x, z, 40.0f)) { b->pos[0] = x; b->pos[2] = z; }
        face_player(b, dt, 1.5f);
        s.tNeckPitch = -0.2f; s.tJaw = 0.5f;
        s.glow = 0.6f * clamp01(s.t / RISE);
        if (s.t >= RISE) {
            P0[0] = b->pos[0]; P0[2] = b->pos[2];
            float px = character.pos[0] - P0[0], pz = character.pos[2] - P0[2];
            float nx = -D[2], nz = D[0];            // across the line, toward the player
            if (nx * px + nz * pz < 0.0f) { nx = -nx; nz = -nz; }
            float w = fminf(fmaxf(nx * px + nz * pz, 45.0f), 150.0f);
            off[0] = nx * w; off[1] = nz * w;
            s.sweepFrom = b->rot[1]; s.sweepSpan = remainderf(atan2f(D[0], D[2]) - b->rot[1], 2.0f * T3D_PI);
            skill_fx_spawn_burst_hostile((float[3]){ P0[0] + off[0], s.groundY, P0[2] + off[1] }, 0.0f);    // where the beam touches down
            s.step = 1; s.t = 0.0f;
        }
    } else if (s.step == 1) {
        // Banks into the line: the body swings to its heading, the head stays on the touch-down point
        s.flapRate = 6.0f;
        b->rot[1] = s.sweepFrom + s.sweepSpan * smooth01(s.t / BANK);
        float to[3] = { P0[0] + off[0], s.groundY, P0[2] + off[1] };
        memcpy(s.aim, to, sizeof(s.aim)); s.tAimK = 1.0f;
        s.tNeckPitch = -0.5f; s.tJaw = 0.8f; s.poseRate = 4.0f;
        s.tNeckYaw = fminf(fmaxf(remainderf(atan2f(off[0], off[1]) - b->rot[1], 2.0f * T3D_PI), -1.3f), 1.3f);
        s.glow = 0.6f + 0.4f * clamp01(s.t / BANK) + 0.15f * fm_sinf(s.life * 24.0f);
        if (s.t >= BANK) { s.step = 2; s.t = 0.0f; s.emit = 0.0f; sfx(SCENE1_SFX_DARK_BEAM, 1.0f); screen_shake_set_shake_mag(5.0f); }
    } else if (s.step <= 3) {
        // The glide: eased along the line over the rake and the glide after it, sinking as it goes
        float tt = s.t + (s.step == 3 ? RAKE : 0.0f), k = smooth01(tt / (RAKE + GLIDE));
        float x = P0[0] + D[0] * s.range * k, z = P0[2] + D[2] * s.range * k;
        if (inside(x, z, 40.0f)) { b->pos[0] = x; b->pos[2] = z; }
        s.flapRate = 0.0f; s.tWingRaise = 0.45f;        // wings held out, gliding
        if (s.step == 2) {
            s.alt = ALT - DIP * smooth01(s.t / RAKE);
            float to[3] = { b->pos[0] + off[0] + D[0] * 10.0f, s.groundY, b->pos[2] + off[1] + D[2] * 10.0f };
            s.tNeckYaw = fminf(fmaxf(remainderf(atan2f(to[0] - b->pos[0], to[2] - b->pos[2]) - b->rot[1], 2.0f * T3D_PI), -1.3f), 1.3f);
            s.tNeckPitch = -0.5f; s.tJaw = 1.0f; s.poseRate = 4.0f;
            beam_at(to, 3.0f, 22.0f, true, dt);
            screen_shake_set_shake_mag(2.0f);
            if (s.t >= RAKE) { s.beamOn = false; s.glow = 0.0f; s.step = 3; s.t = 0.0f; }
        } else {
            if (s.t > GLIDE * 0.5f) s.flapRate = 6.0f;     // a few beats to settle
            s.alt = (ALT - DIP) * (1.0f - smooth01(s.t / LAND));
            if (s.t >= LAND) {
                s.alt = 0.0f;
                land_slam(9.0f);
                s.step = 4; s.t = 0.0f;
            }
        }
    } else if (s.t >= 0.6f) enter(DG_IDLE);
}

static void snipe_update(Boss *b, float dt)
{
    // At a distant player: its head drops level and tracks you while light gathers in its jaws;
    // then the head stops (the aim is fixed, a line of bursts on the floor shows it) and a
    // thin, fast beam cracks along that line. Step or roll off the line once it stops tracking
    float sm = s.dark ? 1.2f : 1.0f;
    if (s.step == 0) {
        face_player(b, dt, 2.2f);
        s.tNeckPitch = 0.1f; s.tNeckReach = 1.15f; s.tJaw = 0.5f; s.tCrouch = 3.0f;
        s.tLean = 2.0f; s.tWingSweep = 0.75f; s.tWingRaise = 0.3f;     // low and still, wings tucked: taking aim
        player_chest(s.aim); s.tAimK = 1.0f;
        s.glow = clamp01(s.t / 1.1f);
        if (s.t < dt * 1.5f) sfx(SCENE1_SFX_SPELL_CAST_BOSS, 0.8f);
        if (s.t >= 1.1f / sm) {
            // The line from its mouth through the player's chest, on to the floor behind them
            float c[3], drop = s.mouth[1] - s.groundY;
            player_chest(c);
            float k = c[1] < s.mouth[1] - 1.0f ? fminf(drop / (s.mouth[1] - c[1]), 2.5f) : 2.5f;
            for (int m = 0; m < 3; m++) s.target[m] = s.mouth[m] + (c[m] - s.mouth[m]) * k;
            s.target[1] = fmaxf(s.target[1], s.groundY);
            for (int i = 1; i <= 3; i++) {
                float p[3];
                lerp3(p, s.mouth, s.target, 0.25f + 0.25f * i);
                p[1] = s.groundY;
                skill_fx_spawn_burst_hostile(p, i * 1.3f);
            }
            s.step = 1; s.t = 0.0f;
        }
    } else if (s.step == 1) {
        // Locked: the head holds still, the glow flares
        memcpy(s.aim, s.target, sizeof(s.aim)); s.tAimK = 1.0f;
        s.tNeckPitch = 0.05f; s.tNeckReach = 1.15f; s.tJaw = 0.9f; s.tCrouch = 3.0f;
        s.glow = 1.0f + 0.3f * fm_sinf(s.life * 30.0f);
        if (s.t >= 0.45f) { s.step = 2; s.t = 0.0f; sfx(SCENE1_SFX_DARK_BEAM, 1.0f); screen_shake_set_shake_mag(5.0f); blast_spawn(s.target); }
    } else if (s.step == 2) {
        s.tNeckReach = 1.15f; s.tJaw = 1.0f; s.tCrouch = 3.0f; s.tNeckPitch = 0.1f;
        beam_at(s.target, 2.5f, 22.0f, false, dt);
        if (s.t >= 0.45f) { s.beamOn = false; s.glow = 0.0f; s.step = 3; s.t = 0.0f; }
    } else {
        s.tNeckPitch = -0.2f; s.poseRate = 3.0f;
        if (s.t >= 0.6f) enter(DG_IDLE);
    }
}

static void triple_update(Boss *b, float dt)
{
    // Three fireballs, each aimed as it leaves at where you are then, a little off at random (to
    // a side, short or long): slow enough to step out of. It rears its head back (the fire glowing
    // in its jaws) and snaps it forward with each one. Each leaves the floor burning a moment
    float wind = (s.step == 0 ? 1.2f : 0.7f) / (s.dark ? 1.15f : 1.0f);     // a long rear before the first (was 0.8)
    if (s.step == 0) {
        face_player(b, dt, 1.8f);
        if (s.t < dt * 1.5f) s.sg = (pick_rng() & 1) ? 1.0f : -1.0f;
    }
    if (s.step < 3) {
        float u = clamp01(s.t / wind);
        bool rear = u < 0.75f;      // rear back, then snap forward to spit
        s.tNeckPitch = rear ? 0.7f : -0.1f; s.tNeckReach = rear ? 0.8f : 1.1f;
        s.tJaw = rear ? 0.3f : 0.9f; s.poseRate = rear ? 6.0f : 14.0f;
        // its own tell (the Breath's head-up is alike): wings flared high and the neck drawn far
        // back as it rears for each spit
        s.tWingRaise = rear ? 0.9f : 0.4f; if (rear) s.tNeckReach = 0.8f;
        s.tLean = rear ? -3.0f : 3.0f; s.tRear = rear ? 3.0f : 0.0f; s.tWingSweep = rear ? 0.2f : 0.5f;
        s.glow = 0.4f + 0.6f * u;
        if (s.t < dt * 1.5f) {      // this shot's aim: where the player is now, a random spread off it
            s.sg = 0.24f * frand(); s.range = 0.85f + 0.3f * (frand() * 0.5f + 0.5f);
        }
        float ax = character.pos[0] - s.mouth[0], az = character.pos[2] - s.mouth[2], a = atan2f(ax, az) + s.sg, r = sqrtf(ax * ax + az * az) * s.range;
        float to[3] = { s.mouth[0] + fm_sinf(a) * r, s.groundY + 2.0f, s.mouth[2] + fm_cosf(a) * r };     // this shot's spot on the floor
        if (s.step > 0) { memcpy(s.aim, to, sizeof(s.aim)); s.tAimK = 0.6f; face_player(b, dt, 1.2f); }
        if (s.t >= wind) {
            Ball *o = &s.ball[s.ballNext];
            s.ballNext = (s.ballNext + 1) % BALL_MAX;
            memcpy(o->p, s.mouth, sizeof(o->p));
            float v[3] = { to[0] - s.mouth[0], to[1] - s.mouth[1], to[2] - s.mouth[2] };
            norm3(v);
            for (int m = 0; m < 3; m++) o->v[m] = v[m] * 115.0f;     // (was 190: slow enough to read and step out of)
            o->on = true;
            sfx(SCENE1_SFX_FIRE, 1.0f);
            screen_shake_set_shake_mag(3.0f);
            s.glow = 0.3f;
            s.step++; s.t = 0.0f;
        }
    } else {
        s.tNeckPitch = -0.15f; s.poseRate = 3.0f;
        if (s.t >= 0.7f) enter(DG_IDLE);
    }
}

static void turn_update(Boss *b, float dt)
{
    // A heavy turn to face a player at its side or behind it: its head swings round first, then
    // the body follows on its planted feet, slow to get going and slow to stop (about 2 s for a
    // half turn). It sinks a little through the middle, the tail dragging behind
    float dx, dz;
    to_player(b, &dx, &dz);
    if (s.t < dt * 1.5f) {
        s.sweepFrom = b->rot[1];
        s.sweepSpan = remainderf(atan2f(dx, dz) - b->rot[1], 2.0f * T3D_PI);
        s.range = 0.7f + 1.3f * fabsf(s.sweepSpan) / T3D_PI;
        sfx_step();
    }
    float u = clamp01(s.t / s.range), w = fm_sinf(u * T3D_PI);
    b->rot[1] = s.sweepFrom + s.sweepSpan * u * u * (3.0f - 2.0f * u);
    float rel = remainderf(atan2f(dx, dz) - b->rot[1], 2.0f * T3D_PI);
    s.tNeckYaw = fminf(fmaxf(rel * 0.9f, -1.0f), 1.0f);     // the head leads
    s.tCrouch = 3.5f * w; s.tTail = -s.sweepSpan * 0.3f * w; s.tNeckPitch = -0.1f * w; s.poseRate = 4.0f;
    s.tBend = (s.sweepSpan > 0.0f ? 1.0f : -1.0f) * 5.0f * w;      // the spine curls into the turn
    if (u >= 1.0f) {
        screen_shake_set_shake_mag(3.0f);
        sfx_step();
        enter(DG_IDLE);
        s.idleWait = 0.6f;      // then it attacks soon
    }
}

static void sweep_update(Boss *b, float dt)
{
    // Kalameet's ground fire: it plants low, head down by the floor and turned out to one side,
    // the fire swelling in its jaws (a burst on the floor marks where the sweep starts); then a
    // stream of fire along the floor in front of it, swept slowly across to the other side, the
    // ground left burning a moment. Roll through the stream, or get beside or behind it
    const float WIND = 1.2f, SWEEP = 2.3f, HALF = 1.2f, REACH = 200.0f;    // the arc: 1.2 rad each side of its nose
    float sm = s.dark ? 1.15f : 1.0f, to[3];
    s.tCrouch = 8.0f; s.tNeckReach = 1.1f;
    if (s.step == 0) {
        if (s.t < dt * 1.5f) { s.sg = (pick_rng() & 1) ? 1.0f : -1.0f; sfx(SCENE1_SFX_SPELL_CAST_BOSS, 0.6f); }
        face_player(b, dt, 1.5f);
        float u = smooth01(s.t / (WIND * 0.6f));
        s.tNeckPitch = -0.6f; s.tNeckYaw = -s.sg * HALF * 0.7f * u; s.tJaw = 0.35f; s.poseRate = 4.0f;
        floor_point(b, b->rot[1] - s.sg * HALF * u, REACH, to);
        if (s.t >= WIND * 0.5f && s.t - dt < WIND * 0.5f) skill_fx_spawn_burst_hostile(to, 0.0f);     // where it starts
        to[1] += 6.0f;      // the head looks along the floor, the jaws just off it
        memcpy(s.aim, to, sizeof(s.aim)); s.tAimK = u;
        s.glow = clamp01(s.t / (WIND / sm)) * (0.85f + 0.15f * fm_sinf(s.life * 20.0f));
        if (s.t >= WIND / sm) { s.sweepFrom = b->rot[1]; s.step = 1; s.t = 0.0f; s.emit = 0.0f; sfx_lunge(); }
    } else if (s.step == 1) {
        // Planted: the neck carries the sweep (eased in and out), the body only leans into it
        float u = smooth01(s.t / SWEEP), off = -s.sg * HALF + 2.0f * s.sg * HALF * u;
        b->rot[1] = s.sweepFrom + off * 0.15f * clamp01(s.t / 0.4f);
        floor_point(b, s.sweepFrom + off, REACH, to);
        breathe(to, 190.0f, 1.4f, dt);      // the flames spread on the floor and burn on
        // Caught in the stream: its young flames (hazards_update), once; the burning floor after
        // is the flames' own. (A line to the aim point hit out to 200 at once: the flames only
        // ever reach ~125 and take a second to)
#ifdef HLE_LOG_HURTBOX
        float sg2 = 1e9f;
        for (int i = 0; i < FL_MAX; i++) if (s.fl[i].age < s.fl[i].life) sg2 = fminf(sg2, axis_dist_pt(s.fl[i].p) - flame_r(s.fl[i].size, s.fl[i].age, s.fl[i].life) - 8.0f);
        HB_TOUCH(HB_STREAM, sg2);
#endif
        to[1] += 6.0f;
        memcpy(s.aim, to, sizeof(s.aim)); s.tAimK = 1.0f;
        s.tNeckPitch = -0.6f; s.tNeckYaw = off * 0.7f; s.tJaw = 0.9f; s.poseRate = 6.0f;
        s.tBend = off * 4.0f; s.tRoll = -off * 0.08f; s.tTail = -off * 0.25f; s.tWingRaise = 0.35f; s.tWingSweep = 0.45f;
        s.glow = 1.0f;
        if (s.t >= SWEEP) { s.step = 2; s.t = 0.0f; s.glow = 0.0f; }
    } else {
        s.tNeckPitch = -0.35f; s.tNeckYaw = s.sg * HALF * 0.5f; s.tJaw = 0.4f; s.poseRate = 3.0f;    // head low, spent: the opening
        if (s.t >= 1.0f) enter(DG_IDLE);
    }
}

#define CONE_AT 62.0f      // the cone's point: on the floor this far ahead of the root (under the head, its mouth at ~85)
static void cone_fire(Boss *b)
{
    s.cone.p[0] = b->pos[0] + s.F[0] * CONE_AT; s.cone.p[1] = b->pos[2] + s.F[2] * CONE_AT;
    s.cone.yaw = b->rot[1]; s.cone.t = 0.0f; s.cone.on = true; s.cone.hit = false;
    for (int i = 0; i < 5; i++) {       // harmless flames rolling out over it (they die inside it)
        float a = s.cone.yaw + 0.4f * frand(), dd = CONE_L * (0.35f + 0.45f * (i / 4.0f));
        float p[3] = { s.cone.p[0], s.groundY + 2.0f, s.cone.p[1] }, v[3] = { fm_sinf(a) * dd / 0.33f, 0.0f, fm_cosf(a) * dd / 0.33f };
        flame_spawn(p, v, 0.45f, 13.0f + 3.0f * frand(), false);
    }
    sfx(SCENE1_SFX_FIRE, 1.0f);
    sfx_lunge();
    screen_shake_set_shake_mag(4.0f);
}

static void cone_update(Boss *b, float dt)
{
    // Kalameet's fire: it rears up on its hind legs, the fire swelling in its jaws, and comes down
    // on its forefeet with a quake; then three bursts of fire, each a cone rolling out over the
    // floor in front of it. Before each it draws its head back, the jaws glowing, then thrusts it
    // down (0.12 s) and the cone spreads as the head comes down (it burns 0.3 s); 0.83 s from one
    // burning to the next. Roll through each burst, or get beside it, behind it or in under its chest
    const float REAR = 0.8f, PLANT = 0.45f, TELL = 0.75f, LEAD = 0.08f, BURST = CONE_GROW + CONE_HOLD, REST = 1.0f;
    float sm = s.dark ? 1.15f : 1.0f, flick = 0.12f * fm_sinf(s.life * 26.0f);
    float aim[3] = { b->pos[0] + s.F[0] * (CONE_AT + 35.0f), s.groundY + 4.0f, b->pos[2] + s.F[2] * (CONE_AT + 35.0f) };
    s.tNeckYaw = 0.0f; s.tWingSpan = 1.0f;
    if (s.step == 0) {          // rears up
        if (s.t < dt * 1.5f) sfx_roar();
        face_player(b, dt, 1.8f);
        // (lower than it was, 16 and 0.8: from the player's camera up close its head was off the top)
        s.tRear = 9.0f; s.tClaw[0] = s.tClaw[1] = 0.8f; s.tNeckPitch = 0.45f; s.tNeckReach = 0.8f; s.tJaw = 0.4f; s.tWingRaise = 0.75f; s.poseRate = 4.0f;
        s.glow = 0.5f * smooth01(s.t / (REAR / sm));
        if (s.t >= REAR / sm) { s.step = 1; s.t = 0.0f; }
    } else if (s.step == 1) {   // down on its forefeet, the head still drawn back: the first tell
        face_player(b, dt, 1.2f);
        s.tNeckPitch = 0.45f; s.tNeckReach = 0.8f; s.tJaw = 0.35f; s.tCrouch = 3.0f; s.tWingRaise = 0.6f; s.poseRate = 6.0f;
        if (s.t >= 0.15f && s.t - dt < 0.15f) { screen_shake_set_shake_mag(7.0f); sfx_boom(); }
        s.glow = 0.5f + 0.5f * smooth01(s.t / (PLANT / sm)) + flick;
        if (s.t >= PLANT / sm) { s.step = 2; s.t = 0.0f; }
    } else if (s.step <= 6 && !(s.step & 1)) {     // 2, 4, 6: a burst, the head thrust down along it
        float u = smooth01(s.t / 0.12f);
        memcpy(s.aim, aim, sizeof(s.aim)); s.tAimK = u;
        s.tNeckPitch = 0.45f - 0.9f * u; s.tNeckReach = 0.8f + 0.15f * u; s.tJaw = 0.35f + 0.65f * u; s.tCrouch = 4.0f; s.poseRate = 12.0f;
        if (s.t >= LEAD && s.t - dt < LEAD) cone_fire(b);
        s.glow = 1.0f - 0.6f * clamp01((s.t - LEAD) / BURST);
        if (s.t >= LEAD + BURST) { s.step++; s.t = 0.0f; }
    } else if (s.step <= 5) {   // 3, 5: the next tell: the head drawn back up, the jaws glowing
        face_player(b, dt, 1.2f);
        float u = smooth01(s.t / (TELL / sm)), back = smooth01(s.t / 0.3f);
        s.tNeckPitch = -0.45f + 0.9f * back; s.tNeckReach = 0.95f - 0.15f * back; s.tJaw = 1.0f - 0.7f * back; s.tCrouch = 3.0f; s.poseRate = 8.0f;
        s.glow = 0.4f + 0.6f * u + flick * u;
        if (s.t >= TELL / sm) { s.step++; s.t = 0.0f; }
    } else {                    // spent: head low a moment, the opening
        s.tNeckPitch = -0.5f; s.tCrouch = 5.0f; s.tJaw = 0.4f; s.poseRate = 3.0f;
        s.glow = 0.4f * (1.0f - clamp01(s.t / 0.4f));
        if (s.t >= REST) enter(DG_IDLE);
    }
}

static void hover_update(Boss *b, float dt)
{
    // The hovering beam: it rises where it stands and hangs there, wings beating, the dark
    // gathering in its jaws (a burst marks its left, where the beam will touch down); rakes the
    // beam over the floor from its left to its right through where you stand; gathers again a
    // moment (a burst on its right) and rakes it back from right to left; then sinks and lands.
    // Roll each sweep, or get in under it
    const float RISE = 1.3f, CHARGE = 0.8f, SWEEP = 1.7f, RECHARGE = 0.7f, LAND = 1.2f, ALT = 70.0f, HALF = 1.1f;
    s.tWingSpan = 1.0f;
    float bob = 3.0f * fm_sinf(s.life * 2.2f), to[3];
    if (s.step == 0) {          // up, then the charge
        // its own tell (the Sky Beam also rises casting): a roar as it heaves itself straight up,
        // its wings pounding
        if (s.t < dt * 1.5f) { sfx(SCENE1_SFX_SPELL_CAST_BOSS, 0.8f); sfx_roar(); }
        float u = smooth01(s.t / RISE);
        s.alt = (ALT + bob) * u;
        s.flapRate = s.t < RISE ? 13.0f : 7.0f;
        s.tNeckPitch = -0.3f; s.tJaw = 0.5f; s.poseRate = 4.0f;
        s.glow = 0.9f * clamp01(s.t / (RISE + CHARGE)) + 0.1f * fm_sinf(s.life * 24.0f) * clamp01((s.t - RISE) / CHARGE);
        if (s.t < RISE) face_player(b, dt, 1.6f);
        else {
            if (s.t - dt < RISE) {      // the line fixed: from its left to its right, over where you stand
                float dx, dz;       // (from up here a beam aimed past you passes over your head)
                s.range = fminf(fmaxf(to_player(b, &dx, &dz), 50.0f), 230.0f);
                s.sweepFrom = b->rot[1] - HALF; s.sweepSpan = 2.0f * HALF;
                floor_point(b, s.sweepFrom, s.range, to);
                skill_fx_spawn_burst_hostile(to, 0.0f);
            }
            floor_point(b, s.sweepFrom, s.range, to);
            memcpy(s.aim, to, sizeof(s.aim)); s.tAimK = smooth01((s.t - RISE) / (CHARGE * 0.7f));
        }
        if (s.t >= RISE + CHARGE) { s.step = 1; s.t = 0.0f; s.emit = 0.0f; s.hitDone = false; sfx(SCENE1_SFX_DARK_BEAM, 1.0f); screen_shake_set_shake_mag(5.0f); }
    } else if (s.step == 1 || s.step == 3) {   // a sweep: left to right, then back
        s.alt = ALT + bob; s.flapRate = 7.0f;
        float u = smooth01(s.t / SWEEP), a = s.sweepFrom + s.sweepSpan * (s.step == 1 ? u : 1.0f - u);
        floor_point(b, a, s.range, to);
        turn_toward(b, a, 0.5f, dt);        // the body leans round with it a little
        s.tNeckPitch = -0.4f; s.tJaw = 1.0f; s.poseRate = 6.0f;
        beam_at(to, 3.0f, 22.0f, true, dt);
        screen_shake_set_shake_mag(2.0f);
        if (s.t >= SWEEP) {
            s.beamOn = false; s.glow = 0.5f; s.step++; s.t = 0.0f;
            if (s.step == 2) {      // the way back over where you are now
                float dx, dz;
                s.range = fminf(fmaxf(to_player(b, &dx, &dz), 50.0f), 230.0f);
                floor_point(b, s.sweepFrom + s.sweepSpan, s.range, to);
                skill_fx_spawn_burst_hostile(to, 0.0f);
                sfx(SCENE1_SFX_SPELL_CAST_BOSS, 0.7f);
            } else s.sg = s.alt;    // the height it sinks from
        }
    } else if (s.step == 2) {   // gathers again
        s.alt = ALT + bob; s.flapRate = 7.0f;
        floor_point(b, s.sweepFrom + s.sweepSpan, s.range, to);
        lerp3(s.aim, s.beamP, to, smooth01(s.t / (RECHARGE * 0.8f))); s.tAimK = 1.0f;
        s.tNeckPitch = -0.3f; s.tJaw = 0.6f; s.poseRate = 5.0f;
        s.glow = 0.5f + 0.5f * smooth01(s.t / RECHARGE) + 0.12f * fm_sinf(s.life * 26.0f);
        if (s.t >= RECHARGE) { s.step = 3; s.t = 0.0f; s.emit = 0.0f; s.hitDone = false; sfx(SCENE1_SFX_DARK_BEAM, 1.0f); screen_shake_set_shake_mag(5.0f); }
    } else if (s.step == 4) {   // sinks and lands
        s.glow = 0.5f * (1.0f - clamp01(s.t / 0.4f));
        s.flapRate = 6.0f;
        s.alt = s.sg * (1.0f - smooth01(s.t / LAND));
        if (s.t >= LAND) { s.alt = 0.0f; land_slam(9.0f); s.step = 5; s.t = 0.0f; }
    } else if (s.t >= 0.7f) enter(DG_IDLE);
}

// Room to rush this far ahead (move_by's test at the end of it): by a wall the charge roared
// and went nowhere
static bool room_ahead(const Boss *b, float d)
{
    float x = b->pos[0] + s.F[0] * d, z = b->pos[2] + s.F[2] * d;
    return inside(x, z, 40.0f) && inside(x + s.F[0] * 70.0f, z + s.F[2] * 70.0f, 5.0f);
}

// Room to step this far straight back (the body's whole length stays in the hall)
static bool room_behind(const Boss *b, float d)
{
    float x = b->pos[0] - s.F[0] * d, z = b->pos[2] - s.F[2] * d;
    return inside(x, z, 40.0f) && inside(x - s.F[0] * 80.0f, z - s.F[2] * 80.0f, 5.0f);
}

// A ranged attack straight after the backstep (no pause): one that suits where the player now is
static void enter_ranged(Boss *b, float dist, float rel)
{
    (void)b;
    DgState pick[6]; int n = 0;
    if (fabsf(rel) < 0.7f) {
        if (dist > 110.0f && dist < 260.0f) pick[n++] = DG_TRIPLE;
        if (dist > 70.0f && dist < 200.0f) pick[n++] = DG_BREATH;
        if (dist > 170.0f) pick[n++] = DG_SNIPE;
        if (dist > 55.0f && dist < 160.0f && s.cdSweep <= 0.0f) pick[n++] = DG_SWEEP;
        if (s.dark && dist > 110.0f && s.cdBeam <= 0.0f) pick[n++] = DG_BEAM;
    }
    if (s.dark) pick[n++] = DG_ORBS;
    if (!n) { enter(DG_IDLE); s.idleWait = 0.8f; return; }
    DgState st = pick[pick_rng() % n];
    if (st == DG_TRIPLE) s.cdTriple = 13.0f; else if (st == DG_BREATH) s.cdBreath = 9.0f; else if (st == DG_SNIPE) s.cdSnipe = 10.0f;
    else if (st == DG_SWEEP) s.cdSweep = 15.0f; else if (st == DG_BEAM) s.cdBeam = 18.0f; else s.cdOrbs = 13.0f;
    enter(st);
}

// How far it can spring back here: the longest hop (to 150) that keeps it in the hall
static float backstep_room(const Boss *b)
{
    float d = 150.0f;
    while (d >= 50.0f && !room_behind(b, d + 20.0f)) d -= 20.0f;
    return d;
}

static void backstep_update(Boss *b, float dt)
{
    if (s.t < dt * 1.5f && s.step == 0) {      // most often as far as it can, sometimes (40%) a short hop
        float room = backstep_room(b);
        s.bsDist = fmaxf(pick_rng() % 100 < 40 && room >= 55.0f ? fminf(room, 55.0f + pick_rng() % 25) : room, 30.0f);
    }
    // Harmless, and quick: it sinks onto its haunches, wings flaring, and springs back off its
    // forelegs with one great beat of the wings, head high and eyes on you, the tail swung up to
    // balance; it lands hind feet first, skids, and the forefeet come down. Mostly a ranged attack
    // follows straight away: keep after it
    const float SINK = 0.24f, AIR = 0.35f + s.bsDist / 600.0f, LAND = 0.45f, HOP = 8.0f + s.bsDist * 0.08f;     // a longer hop, longer and higher
    const float DIST = s.bsDist;
    face_player(b, dt, 2.5f);
    if (s.step == 0) {
        if (s.t < dt * 1.5f) { s.followRanged = pick_rng() % 100 < 75; sfx(SCENE1_SFX_BOSS_STEP1, 0.8f); }
        s.tCrouch = 8.0f; s.tLean = -4.0f; s.tNeckPitch = 0.3f; s.tNeckReach = 0.85f; s.tJaw = 0.2f;
        s.tWingRaise = 0.85f; s.tWingSpan = 0.95f; s.tWingSweep = 0.1f; s.tTailUp = 0.25f; s.poseRate = 12.0f;
        if (s.t >= SINK) {
            s.step = 1; s.t = 0.0f; s.range = 0.0f;
            s.flapPh = T3D_PI * 0.5f;       // one beat: from the top, down through the jump, back up
            sfx(SCENE1_SFX_BOSS_SWING3, 0.8f);
        }
    } else if (s.step == 1) {
        float u = clamp01(s.t / AIR), k = 1.0f - (1.0f - u) * (1.0f - u);     // fast off the ground, slowing
        if (move_by(b, -s.F[0] * DIST * (k - s.range), -s.F[2] * DIST * (k - s.range))) s.range = k;
        s.alt = HOP * fm_sinf(u * T3D_PI);
        s.flapRate = 2.0f * T3D_PI / AIR;
        s.tCrouch = -2.0f; s.tRear = 7.0f; s.tLean = -2.0f; s.tNeckPitch = 0.5f; s.tNeckReach = 0.9f; s.tJaw = 0.35f;
        s.tWingSpan = 1.0f; s.tWingSweep = 0.0f; s.tTailUp = 0.5f; s.poseRate = 9.0f;
        if (u >= 1.0f) {
            s.alt = 0.0f; s.step = 2; s.t = 0.0f;
            sfx_step(); screen_shake_set_shake_mag(3.0f);
            s.crouch += 4.0f; s.wingKick = 0.35f;
        }
    } else {
        float u = clamp01(s.t / LAND);
        move_by(b, -s.F[0] * 70.0f * (1.0f - u) * dt, -s.F[2] * 70.0f * (1.0f - u) * dt);     // the skid
        s.tRear = u < 0.35f ? 6.0f : 0.0f;      // the forefeet come down after the hind
        if (s.t >= LAND * 0.35f && s.t - dt < LAND * 0.35f) sfx_step();
        s.tCrouch = 4.0f; s.tNeckPitch = 0.2f; s.tWingRaise = 0.4f; s.tWingSweep = 0.25f; s.poseRate = 8.0f;
        if (s.t >= LAND) {
            float dx, dz, dist = to_player(b, &dx, &dz), rel = remainderf(atan2f(dx, dz) - b->rot[1], 2.0f * T3D_PI);
            if (s.followRanged) enter_ranged(b, dist, rel);
            else { enter(DG_IDLE); s.idleWait = 1.0f; }
        }
    }
}

static float swipe_leg_gap(void)        // the swiping foreleg as drawn: its bones (~6 round) and paw
{
    int k = s.swipeK;
    return fminf(axis_dist_seg(s.shoulder[k], s.foot[k]) - 6.0f, axis_dist_pt(s.foot[k]) - 7.0f);
}

static void swipe_update(Boss *b, float dt)
{
    // The swipe charge: it turns a shoulder to you, that foreleg cocked out wide to the side and
    // the body rolled up over it, head low; then it bursts forward, raking the paw across in front
    // of it as it runs at you and on past you. Roll through it, or away from the cocked paw;
    // once it is past, you are behind it: mind the tail
    float sm = s.dark ? 1.2f : 1.0f, side = (s.swipeK & 1) ? 1.0f : -1.0f;
    const float RAKE = 0.32f, SPEED = 250.0f;
    if (s.step == 0) {
        float dx, dz, dist = to_player(b, &dx, &dz);
        if (s.t < dt * 1.5f) {
            float rel = remainderf(atan2f(dx, dz) - b->rot[1], 2.0f * T3D_PI);
            s.swipeK = rel > 0.0f ? 1 : 0;      // the paw on the player's side
            side = (s.swipeK & 1) ? 1.0f : -1.0f;
            sfx_step();
        }
        face_player(b, dt, 2.4f);
        float u = smooth01(s.t / 0.5f);
        s.tSwipeAmt = 1.0f; s.swipePh = 0.0f;
        s.tRoll = side * 0.22f * u; s.tShLift[s.swipeK] = 8.0f; s.tCrouch = 6.0f; s.tLean = -3.0f;
        s.tNeckPitch = -0.35f; s.tNeckYaw = -side * 0.3f; s.tNeckReach = 0.9f; s.tJaw = 0.5f;
        s.tWingRaise = 0.55f; s.tWingSweep = 0.6f; s.tBend = -side * 5.0f; s.tTail = side * 0.25f; s.poseRate = 6.0f;
        float wind = 0.85f / sm;
        if (s.t >= wind - 0.3f && s.t - dt < wind - 0.3f) sfx(SCENE1_SFX_BOSS_SWING1, 1.0f);     // peaks 0.36 s in: on the rake
        if (s.t >= wind) {
            // Through the player and on past: as far as the hall lets it go
            float len = fminf(dist + 90.0f, 260.0f);
            while (len > 60.0f && !room_ahead(b, len)) len -= 20.0f;
            s.range = len; s.sg = 0.0f;
            s.step = 1; s.t = 0.0f; sfx_lunge();
        }
    } else if (s.step == 1) {
        s.tSwipeAmt = 1.0f; s.swipePh = clamp01(s.t / RAKE);
        s.tRoll = side * 0.22f * (1.0f - s.swipePh) - side * 0.08f * s.swipePh; s.tShLift[s.swipeK] = 8.0f * (1.0f - s.swipePh);
        s.tCrouch = 7.0f; s.tLean = 4.0f; s.tNeckPitch = -0.3f; s.tNeckReach = 1.15f; s.tJaw = 0.4f;
        s.tWingRaise = 0.4f; s.tWingSweep = 1.0f; s.tWingSpan = 0.8f; s.tBend = side * 4.0f * s.swipePh; s.tTail = -side * 0.2f; s.poseRate = 12.0f;
        float st = SPEED * dt;
        bool moved = s.sg < s.range && move_by(b, s.F[0] * st, s.F[2] * st);
        if (moved) s.sg += st;
        if (s.t >= 0.12f && !s.hitDone) {
            float g = fminf(s.swipePh < 1.0f ? swipe_leg_gap() : 1e9f, body_gap() - (SHOVE_R - PLAYER_R));
            HB_TOUCH(HB_CHARGE, g - 8.0f);
            if (g < PLAYER_R) { s.hitDone = true; HB_HIT(HB_CHARGE, g - 8.0f); character_apply_damage(24.0f); screen_shake_set_shake_mag(5.0f); }
        }
        if (s.t >= RAKE && s.t - dt < RAKE) screen_shake_set_shake_mag(3.0f);
        if ((!moved && s.t > RAKE) || s.t > 1.4f) { s.step = 2; s.t = 0.0f; sfx_step(); s.tailV += side * 3.0f; }
    } else {
        // The skid: it digs in and slews to a stop, looking back over its shoulder for you
        float u = clamp01(s.t / 0.35f), dx, dz;
        move_by(b, s.F[0] * 110.0f * (1.0f - u) * dt, s.F[2] * 110.0f * (1.0f - u) * dt);
        to_player(b, &dx, &dz);
        float rel = remainderf(atan2f(dx, dz) - b->rot[1], 2.0f * T3D_PI);
        s.tNeckYaw = fminf(fmaxf(rel * 0.8f, -1.1f), 1.1f); s.tNeckPitch = 0.1f;
        s.tCrouch = 5.0f * (1.0f - u) + 2.0f; s.tRoll = -side * 0.1f * (1.0f - u); s.tWingRaise = 0.5f; s.tWingSweep = 0.5f; s.poseRate = 6.0f;
        if (s.t >= 0.9f) { passed_player(); enter(DG_IDLE); s.idleWait = 0.5f; }
    }
}

static void stomp_update(Boss *b, float dt)
{
    // The heavy stomp: the claw's big brother, on one side only. It heaves that whole side up,
    // shoulder and all, the body rolled over and the paw lifted high, the wing on that side
    // flung up and the tail swung out to balance; then brings it down with all its weight.
    // A wide slam round the paw, then it stays planted a moment: the opening
    float sm = s.dark ? 1.2f : 1.0f, wind = 1.0f / sm;
    int k = s.swipeK;
    float side = (k & 1) ? 1.0f : -1.0f;
    if (s.step == 0) {
        if (s.t < dt * 1.5f) {
            float dx, dz;
            to_player(b, &dx, &dz);
            s.swipeK = k = remainderf(atan2f(dx, dz) - b->rot[1], 2.0f * T3D_PI) > 0.0f ? 1 : 0;    // the side nearer the player
            side = (k & 1) ? 1.0f : -1.0f;
            sfx(SCENE1_SFX_BOSS_STEP1, 0.8f);
        }
        face_player(b, dt, 1.2f);
        float u = smooth01(s.t / (wind * 0.75f));
        s.tClaw[k] = s.t < wind - 0.07f ? 1.0f : 0.0f; s.tClawHi = s.t < wind - 0.07f ? 1.0f : 0.0f;
        s.tRoll = side * 0.2f * u; s.tShLift[k] = 0.0f; s.tCrouch = 2.0f; s.tLean = -3.0f * u;
        s.tNeckPitch = -0.1f; s.tNeckYaw = side * 0.25f; s.tJaw = 0.45f;
        s.tWingRaise = 0.45f; s.tWingSweep = 0.15f; s.tTail = -side * 0.35f * u; s.tBend = side * 3.0f * u; s.poseRate = 5.0f;
        float lead = wind - 0.34f;
        if (s.t >= lead && s.t - dt < lead) sfx(SCENE1_SFX_BOSS_SWING1, 1.0f);
        if (s.t >= wind) {
            float c[3];
            mad3(c, s.foot[k], s.F, 8.0f);
            c[1] = s.groundY + 6.0f;
            skill_fx_spawn_arc_hostile(c, -b->rot[1], 64.0f, 30.0f);
            screen_shake_set_shake_mag(11.0f);
            sfx_boom(); sfx_step();
            s.crouch += 5.0f; s.wingKick = 0.25f;
            s.step = 1; s.t = 0.0f;
        }
    } else {
        // planted on that paw, the body settling back level, head low
        s.tRoll = -side * 0.06f * (1.0f - clamp01(s.t / 0.5f)); s.tCrouch = 6.0f; s.tLean = 3.0f;
        s.tNeckPitch = -0.45f; s.tNeckYaw = side * 0.2f; s.tJaw = 0.3f; s.tWingRaise = 0.15f; s.tTail = side * 0.1f; s.poseRate = 3.0f;
        if (s.t >= 0.9f) enter(DG_IDLE);
    }
}

static void choose(Boss *b, float dist, float rel)
{
#ifdef HLE_DRAGON_CYCLE   // test aid: every move in turn (a gallery)
    {
        static const DgState L[] = { DG_BACKSTEP, DG_SWIPE, DG_STOMP, DG_CLAW, DG_BITE, DG_TAIL, DG_WHIP, DG_BREATH, DG_CHARGE, DG_FLY,
                                     DG_TRIPLE, DG_SNIPE, DG_SWEEP, DG_CONE, DG_SKYBEAM, DG_HOVER, DG_BEAM, DG_NOVA, DG_ORBS, DG_TURN };
        static int i;
        DgState st = L[i++ % (int)(sizeof(L) / sizeof(L[0]))];
        debugf("[cycle] %d\n", st);
        enter(st);
        return;
    }
#endif
#ifdef HLE_DRAGON_FORCE   // test aid: one move over and over (a DgState)
    enter((DgState)HLE_DRAGON_FORCE);
    return;
#endif
    bool front = fabsf(rel) < 0.7f, behind = fabsf(rel) > 2.0f;
    if (behind && dist < 150.0f && s.cdTail <= 0.0f) { s.cdTail = 3.5f; enter(DG_TAIL); return; }     // the answer to standing behind it
    // Every move that suits where the player is (and is off its cooldown) is a candidate, picked
    // at random by weight: the chase (the backstep, the swipe charge) weighs most, so the player
    // runs after it about half the time, and every other attack still gets its turns (a fixed
    // order starved the ones lower down: the bite and the charge never came)
    struct { DgState st; float *cd, cdv, w; } c[20];
    int n = 0;
#define CAND(cond, ST, CD, CDV, W) do { if (cond) { c[n].st = ST; c[n].cd = &s.CD; c[n].cdv = CDV; c[n].w = W; n++; } } while (0)
    const float CHASE = 7.0f;
    CAND(front && dist < 110.0f && s.cdBackstep <= 0.0f && backstep_room(b) >= 70.0f, DG_BACKSTEP, cdBackstep, 3.5f, CHASE);
    CAND(front && dist > 60.0f && dist < 200.0f && s.cdSwipe <= 0.0f && room_ahead(b, dist + 20.0f), DG_SWIPE, cdSwipe, 3.5f, CHASE);
    CAND(front && dist < 85.0f && s.cdClaw <= 0.0f, DG_CLAW, cdClaw, 3.0f, 1.0f);
    CAND(front && dist < 85.0f && s.cdClaw <= 0.0f, DG_STOMP, cdClaw, 3.5f, 0.8f);
    CAND(front && dist < 100.0f && s.cdBite <= 0.0f, DG_BITE, cdBite, 5.0f, 1.0f);
    CAND(dist > 30.0f && dist < 115.0f && s.cdWhip <= 0.0f, DG_WHIP, cdWhip, 11.0f, 1.0f);    // any side
    if (s.dark) {
        CAND(s.cdNova <= 0.0f && dist < 110.0f, DG_NOVA, cdNova, 16.0f, 1.5f);
        // (closer, the player is under its chin, where the beam can't reach: it raked over them)
        CAND(s.cdBeam <= 0.0f && front && dist > 110.0f, DG_BEAM, cdBeam, 18.0f, 1.5f);
        CAND(s.cdOrbs <= 0.0f, DG_ORBS, cdOrbs, 13.0f, 1.0f);
    }
    CAND(s.cdFly <= 0.0f && dist > 70.0f, DG_FLY, cdFly, 22.0f, 1.0f);       // (in the dark half: often a sky beam or the hover instead)
    CAND(front && dist > 170.0f && s.cdSnipe <= 0.0f, DG_SNIPE, cdSnipe, 10.0f, 1.0f);
    CAND(front && dist > 55.0f && dist < 160.0f && s.cdSweep <= 0.0f, DG_SWEEP, cdSweep, 15.0f, 1.0f);
    CAND(front && dist > 40.0f && dist < 140.0f && s.cdCone <= 0.0f, DG_CONE, cdCone, 14.0f, 1.0f);
    CAND(front && dist > 110.0f && dist < 260.0f && s.cdTriple <= 0.0f, DG_TRIPLE, cdTriple, 13.0f, 1.0f);
    CAND(front && dist > 70.0f && dist < 200.0f && s.cdBreath <= 0.0f, DG_BREATH, cdBreath, 9.0f, 1.0f);
    CAND(front && dist > 140.0f && s.cdCharge <= 0.0f && room_ahead(b, 100.0f), DG_CHARGE, cdCharge, 8.0f, 1.0f);
#undef CAND
#ifdef HLE_LOG_CHOOSE    // test aid: what it saw when it chose, and its candidates
    { char buf[96]; int o = 0; for (int i = 0; i < n && o < 80; i++) o += snprintf(buf + o, sizeof buf - o, "%d ", c[i].st); buf[o] = 0;
      debugf("[choose] dist %.0f rel %.2f | %s\n", (double)dist, (double)rel, buf); }
#endif
    if (!n) return;     // nothing suits: it keeps walking in (the idle) and tries again
    float tot = 0.0f;
    for (int i = 0; i < n; i++) tot += c[i].w;
    float r = (pick_rng() % 10000) / 10000.0f * tot;
    int k = 0;
    while (k < n - 1 && r >= c[k].w) { r -= c[k].w; k++; }
    *c[k].cd = c[k].cdv;
#ifdef HLE_LOG_CHOOSE
    debugf("[choose] picked %d (r %.2f of %.2f)\n", c[k].st, (double)r, (double)tot);
#endif
    DgState st = c[k].st;
    if (st == DG_FLY && s.dark) { uint32_t q = pick_rng() % 4; st = q < 2 ? DG_FLY : q == 2 ? DG_SKYBEAM : DG_HOVER; }
    enter(st);
}

void dragon_boss_update(Boss* b, float dt)
{
    if (!s.ready) return;
    if (b->damageFlashTimer > 0.0f) b->damageFlashTimer -= dt;
    s.t += dt;
    s.life += dt;
    float sm = s.dark ? 1.2f : 1.0f;
    s.cdWhip -= dt * sm;
    s.cdClaw -= dt * sm; s.cdBite -= dt * sm; s.cdTail -= dt * sm; s.cdBreath -= dt * sm; s.cdCharge -= dt * sm;
    s.cdFly -= dt * sm; s.cdNova -= dt * sm; s.cdOrbs -= dt * sm; s.cdBeam -= dt * sm; s.cdTriple -= dt * sm; s.cdSnipe -= dt * sm; s.cdSweep -= dt * sm; s.cdCone -= dt * sm;
    s.fireCd -= dt;
    s.cdBackstep -= dt * sm; s.cdSwipe -= dt * sm; s.passT -= dt;

    float dx, dz, dist = to_player(b, &dx, &dz);
    float rel = remainderf(atan2f(dx, dz) - b->rot[1], 2.0f * T3D_PI);

    // The resting pose, which each move overrides: it watches the player
    s.lookRel = ease(s.lookRel, rel, 2.5f, dt);     // the head follows the player a beat late (a roll past it)
    s.tNeckPitch = 0.0f; s.tNeckYaw = fminf(fmaxf(s.lookRel * 0.75f, -0.85f), 0.85f); s.tNeckReach = 1.0f;
    s.tJaw = 0.05f; s.tWingRaise = 0.25f; s.tWingSpan = 0.7f; s.tTail = 0.0f; s.tCrouch = 0.0f;
    s.tClaw[0] = s.tClaw[1] = 0.0f; s.tAimK = 0.0f; s.tRear = 0.0f; s.tTailUp = 0.0f; s.poseRate = 6.0f; s.flapRate = 0.0f;
    s.tRoll = 0.0f; s.tShLift[0] = s.tShLift[1] = 0.0f; s.tBend = 0.0f; s.tLean = 0.0f; s.tWingSweep = 0.45f; s.tClawHi = 0.0f; s.tSwipeAmt = 0.0f;

    if (b->state == BOSS_STATE_DEAD && s.state != DG_DEAD) { enter(DG_DEAD); clear_hazards(); }
    if (!s.dark && s.state != DG_DEAD && s.state != DG_INTRO && b->health <= b->maxHealth * 0.5f) {
        s.dark = true;
        b->mode = BOSS_MODE_CINEMATIC;
        s.glow = 0.0f; s.beamOn = false; s.cone.on = false;
        enter(DG_ROAR);
    }


    // Past a move's windup, a tail swing, or up in the air: the strike itself
    b->striking = s.state == DG_FLY || s.state == DG_TAIL || (s.state == DG_CLAW && s.step != 0 && s.step != 2) ||
                  ((s.state == DG_BITE || s.state == DG_BREATH || s.state == DG_CHARGE || s.state == DG_WHIP || s.state == DG_NOVA || s.state == DG_BEAM ||
                    s.state == DG_TRIPLE || s.state == DG_SNIPE || s.state == DG_SKYBEAM || s.state == DG_SWEEP || s.state == DG_CONE || s.state == DG_HOVER || s.state == DG_SWIPE || s.state == DG_STOMP) && s.step > 0);
#ifdef HLE_TEST_DUMMY
    { extern void hle_open_tick(bool, float); hle_open_tick((s.state == DG_IDLE || s.state == DG_TURN || s.state == DG_DOWN), dt); }   // test aid: measure the openings
#endif
    switch (s.state) {
    case DG_INTRO:
        // Flies down into the hall and lands, then waits for the intro to end
        s.tWingSpan = 1.0f;
        if (s.alt > 0.0f) {
            s.flapRate = 8.0f;
            s.alt = fmaxf(0.0f, s.alt - fmaxf(s.alt * 1.1f, 25.0f) * dt);
            if (s.alt <= 0.0f) {
                s.step = 1; s.t = 0.0f;     // landed: the roar follows
                for (int i = 0; i < 4; i++)
                    skill_fx_spawn_burst_hostile((float[3]){ s.foot[i][0], s.groundY, s.foot[i][2] }, i * 1.5f);
                screen_shake_set_shake_mag(12.0f);
                sfx_boom();
            }
        } else {
            s.tNeckPitch = 0.7f; s.tJaw = 0.8f; s.tWingRaise = 0.6f;    // a roar at the player
            if (s.step == 1 && s.t > 0.12f) { s.step = 2; sfx_roar(); }      // loud 0.28 s on
            if (s.step == 2 && s.t >= 0.4f && s.t - dt < 0.4f) screen_shake_set_shake_mag(5.0f);
        }
        face_player(b, dt, 2.0f);
        break;

    case DG_IDLE: {
        // It only creeps round to follow a player in front of it; one at its side or behind gets
        // the tail if it's ready, else a slow, heavy turn (DG_TURN): get beside it and behind it
        // just past the player and they face its tail: the tail at once, its cooldown or not
        // (past a player at range the tail backs in to them: tail_update)
        bool passed = s.passT > 0.0f && (s.passTail || player_faces_tail());
        if (passed && s.t > 0.12f && fabsf(rel) > 2.0f && dist < 240.0f) { s.cdTail = 3.5f; enter(DG_TAIL); break; }
        if (s.t > 0.3f && fabsf(rel) > 2.0f && dist < 150.0f && s.cdTail <= 0.0f) { s.cdTail = 3.5f; enter(DG_TAIL); break; }
        // In a long pause at range it prowls: paces side to side across your front like a big cat,
        // its head on you (closing in when far, easing off when near), and turns back to face you
        // before it strikes
        float wait = s.idleWait / sm;
        bool paces = !s.heavyRest && wait >= 1.8f && dist > 70.0f && dist < 220.0f, pace = paces && s.t > 0.4f && s.t < wait - 0.8f;
        if (s.t > 0.3f && fabsf(rel) > (paces ? 1.7f : 1.0f)) { enter(DG_TURN); break; }     // (pacing, it turns back by itself)
        if (pace) {
            float sgn = ((int)((s.t - 0.4f) / 2.8f) & 1) ? -s.paceSg : s.paceSg;
            float off = sgn * (1.15f - fminf(fmaxf((dist - 120.0f) / 150.0f, -0.35f), 0.35f));
            float d = turn_toward(b, atan2f(dx, dz) + off, 1.6f * sm, dt);
            if (fabsf(d) < 0.6f) move_by(b, s.F[0] * 24.0f * sm * dt, s.F[2] * 24.0f * sm * dt);
            s.tNeckYaw = fminf(fmaxf(s.lookRel * 0.85f, -1.1f), 1.1f);
            if (!s.growled) { s.growled = true; if (pick_rng() % 100 < 60) sfx(SCENE1_SFX_DRAGON_GROWL, 0.8f); }     // a low growl as it starts to prowl
        } else {
            float d = turn_toward(b, atan2f(dx, dz), (wait >= 1.8f && s.t >= wait - 0.8f ? 2.2f : 0.7f) * sm, dt);    // (back round from its pacing)
            if (dist > 75.0f && fabsf(d) < 0.8f) move_by(b, s.F[0] * 30.0f * sm * dt, s.F[2] * 30.0f * sm * dt);
            else if (dist < 50.0f && fabsf(d) < 1.0f) move_by(b, -s.F[0] * 18.0f * dt, -s.F[2] * 18.0f * dt);
        }
        if (s.t > 0.3f && s.t < s.idleWait / sm - 0.3f) {   // every pause it breathes, its body rising and sinking
            if (s.heavyRest) {      // spent (after landing, or three beam sweeps): heaving, head low, panting, wings sagging
                float br = fm_sinf(s.life * 3.3f);
                s.tCrouch = 6.0f + 2.5f * br; s.tNeckPitch = -0.55f + 0.12f * br; s.tJaw = 0.35f + 0.15f * br;
                s.tWingRaise = -0.15f; s.tWingSweep = 0.3f; s.tWingSpan = 0.85f; s.poseRate = 3.0f;
            } else {
                float br = fm_sinf(s.life * 2.0f);
                s.tCrouch = 1.5f + 1.5f * br; s.tNeckPitch = 0.04f * br; s.poseRate = 3.0f;
                // Standing in a pause, now and then (about once in 3 s) a fidget: the tail lifted and
                // slapped down on the floor, or a forepaw lifted and scraped back across it
                if (!pace && s.fidgetT <= 0.0f && s.t > 0.6f && s.t < wait - 1.2f && (pick_rng() % 1000) < (uint32_t)(dt * 330.0f)) {
                    s.fidgetT = 1.0f; s.fidgetK = pick_rng() % 3;
                }
                if (s.fidgetT > 0.0f) {
                    float u = 1.0f - s.fidgetT, was = u - dt;
                    if (s.fidgetK == 0) {
                        s.tTailUp = u < 0.4f ? 0.55f : 0.0f; s.poseRate = u < 0.4f ? 5.0f : 10.0f;
                        if (u >= 0.6f && was < 0.6f) { sfx(SCENE1_SFX_BOSS_STEP1, 0.45f); screen_shake_set_shake_mag(1.5f); }
                    } else {
                        int k = s.fidgetK - 1;
                        s.tClaw[k] = u < 0.3f ? 0.3f : u < 0.7f ? 0.1f : 0.0f; s.tNeckPitch = -0.15f; s.tNeckYaw = k ? 0.2f : -0.2f;
                        if (u >= 0.32f && was < 0.32f) sfx(SCENE1_SFX_BOSS_STEP1, 0.3f);
                    }
                    s.fidgetT -= dt;
                }
            }
        }
        if (s.t > s.idleWait / sm) choose(b, dist, rel);     // a pause between attacks: the opening
        break;
    }

    case DG_CLAW:   claw_update(b, dt); break;
    case DG_BITE:   bite_update(b, dt); break;
    case DG_TAIL:   tail_update(b, dt); break;
    case DG_WHIP:   whip_update(b, dt); break;
    case DG_BREATH: breath_update(b, dt); break;
    case DG_CHARGE: charge_update(b, dt); break;
    case DG_FLY:    fly_update(b, dt); break;
    case DG_ROAR:   roar_update(b, dt); break;
    case DG_NOVA:   nova_update(b, dt); break;
    case DG_ORBS:   orbs_update(b, dt); break;
    case DG_BEAM:   beam_update(b, dt); break;
    case DG_SKYBEAM: skybeam_update(b, dt); break;
    case DG_SNIPE:  snipe_update(b, dt); break;
    case DG_TRIPLE: triple_update(b, dt); break;
    case DG_TURN:   turn_update(b, dt); break;
    case DG_SWEEP:  sweep_update(b, dt); break;
    case DG_CONE:   cone_update(b, dt); break;
    case DG_HOVER:  hover_update(b, dt); break;
    case DG_BACKSTEP: backstep_update(b, dt); break;
    case DG_SWIPE:  swipe_update(b, dt); break;
    case DG_STOMP:  stomp_update(b, dt); break;

    case DG_DOWN: {
        // A full stagger: a lurch, the legs buckle and it drops onto its belly, head lolling,
        // wings sprawled, tail limp; then it heaves itself up
        float t = s.t, up = clamp01((t - (BOSS_STAGGER_S - 0.8f)) / 0.8f);     // it heaves itself up over the last 0.8 s
        if (t < 0.25f) { s.tNeckPitch = 0.8f; s.tCrouch = -3.0f; s.tJaw = 0.9f; s.tWingRaise = 0.8f; s.poseRate = 9.0f; }
        else {
            if (t >= 0.47f && t - dt < 0.47f) { screen_shake_set_shake_mag(10.0f); sfx_boom();     // the belly on the floor
                for (int i = 0; i < 2; i++) skill_fx_spawn_burst_hostile((float[3]){ s.chest[0] + s.R[0] * (i ? 14.0f : -14.0f), s.groundY, s.chest[2] + s.R[2] * (i ? 14.0f : -14.0f) }, i * 1.5f); }
            s.tCrouch = (BODY_H - 12.0f) * (1.0f - up); s.tNeckPitch = -0.95f + 0.6f * up; s.tNeckReach = 1.15f;
            s.tNeckYaw = 0.45f * fm_sinf(t * 2.4f) * (1.0f - up); s.tJaw = 0.5f * (1.0f - up);
            s.tWingRaise = -0.35f + 0.5f * up; s.tWingSpan = 0.95f; s.tTail = 0.15f * fm_sinf(t * 1.3f);
            s.tWingSweep = 0.2f; s.tRoll = 0.12f * fm_sinf(t * 1.9f) * (1.0f - up);      // sprawled, rocking as it tries to rise
            s.poseRate = t < 0.6f ? 6.0f : 3.0f;
        }
        if (t >= BOSS_STAGGER_S) { enter(DG_IDLE); s.shakeT = 0.6f; }     // up, it shakes itself off
        break;
    }

    case DG_DEAD:
        // Falls and lies on the floor
        s.alt = ease(s.alt, 0.0f, 4.0f, dt);
        s.tCrouch = BODY_H - 9.0f; s.tNeckPitch = -1.0f; s.tNeckYaw = 0.4f; s.tWingRaise = -0.25f; s.tWingSpan = 0.7f;
        s.tJaw = 0.4f; s.tTail = 0.3f; s.poseRate = 1.5f;
        s.tRoll = 0.3f; s.tWingSweep = 0.1f;        // slumped over on its side
        break;
    }
    b->rot[0] = b->rot[2] = 0.0f;

#ifdef HLE_DRAGON_HITME   // test aid: a sword hit every 1.5 s, a finisher's every 6 s (the flinch)
    { static float ht; ht += dt; if (ht >= 1.5f) { ht = 0.0f; static int n; boss_apply_damage(b, (++n % 4) ? 4.0f : 10.0f); } }
#endif
    // A blow taken: a flinch (the head jerked up and away, the body recoiling, wings twitching),
    // kicked onto the pose and eased back: a finisher the whole of it. No change to what it is doing
    {
        float hit = s.prevHealth - b->health;
        if (hit > 0.5f && s.state != DG_DEAD && s.state != DG_INTRO && s.state != DG_ROAR) {
            float k = fminf(fmaxf(hit / 10.0f, 0.35f), 1.0f), sd = rel > 0.0f ? 1.0f : -1.0f;
            s.neckPitch += 0.3f * k; s.neckYaw -= sd * 0.25f * k; s.lean -= 4.0f * k; s.jaw += 0.4f * k;
            s.roll += sd * 0.08f * k; s.wingKick += 0.15f * k; s.tailV += sd * 1.5f * k;
#ifdef HLE_LOG_MOVES
            debugf("[flinch] %.1f k %.2f state %d\n", (double)hit, (double)k, s.state);
#endif
        }
        s.prevHealth = b->health;
        s.shakeT -= dt;
    }

    // Ease the pose toward the move's targets
    float pr = s.poseRate;
    s.neckPitch = ease(s.neckPitch, s.tNeckPitch, pr, dt);
    s.neckYaw = ease(s.neckYaw, s.tNeckYaw, pr * 0.6f, dt);
    s.neckReach = ease(s.neckReach, s.tNeckReach, pr, dt);
    s.jaw = ease(s.jaw, s.tJaw, 10.0f, dt);
    s.wingSpan = ease(s.wingSpan, s.tWingSpan, 3.0f, dt);
    s.tail = ease(s.tail, s.tTail, pr, dt);
    s.tailUp = ease(s.tailUp, s.tTailUp, pr, dt);
    s.crouch = ease(s.crouch, s.tCrouch, s.state == DG_DOWN ? 7.0f : 5.0f, dt);     // it drops when its legs give
    s.aimK = ease(s.aimK, s.tAimK, 8.0f, dt);
    s.rear = ease(s.rear, s.tRear, 4.0f, dt);
    bool slam = s.state == DG_CLAW || s.state == DG_STOMP;      // a paw slammed down fast
    for (int k = 0; k < 2; k++) s.claw[k] = ease(s.claw[k], s.tClaw[k], slam && s.tClaw[k] < s.claw[k] ? 30.0f : 9.0f, dt);
    s.roll = ease(s.roll, s.tRoll, slam && fabsf(s.tRoll) < fabsf(s.roll) ? 14.0f : pr * 0.8f, dt);
    for (int k = 0; k < 2; k++) s.shLift[k] = ease(s.shLift[k], s.tShLift[k], slam && s.tShLift[k] < s.shLift[k] ? 18.0f : pr * 0.8f, dt);
    s.bend = ease(s.bend, s.tBend, pr * 0.7f, dt);
    s.lean = ease(s.lean, s.tLean, pr, dt);
    s.wingSweep = ease(s.wingSweep, s.tWingSweep, 3.0f, dt);
    // the great wings fold along its back when the moves hold them in (their old span), spread when out
    // (as the old wings: held in small and swept back, mostly open; spread to their full size)
    s.tWingFold = clamp01((0.95f - s.tWingSpan) / 0.25f) * WING_REST_FOLD;
    s.wingFold = ease(s.wingFold, s.tWingFold, 3.0f, dt);
    s.clawHi = ease(s.clawHi, s.tClawHi, 6.0f, dt);
    s.swipeAmt = ease(s.swipeAmt, s.tSwipeAmt, 10.0f, dt);
    // Flapping fades in and out; the raise it beats about stays eased
    s.flapPh += dt * s.flapRate;
    s.flapAmt = ease(s.flapAmt, s.flapRate > 0.0f ? 1.0f : 0.0f, 5.0f, dt);
    s.flapOn = ease(s.flapOn, s.flapRate > 0.0f ? 1.0f : 0.0f, 5.0f, dt);
    s.wingRaise = ease(s.wingRaise, s.tWingRaise, 3.0f, dt);
    if (flying()) {     // the flight's beat as it was: straight onto it, from it eased back
        s.tWingSweep = 0.0f; s.flapAmt = 0.0f;
        if (s.flapRate > 0.0f) s.wingRaise = 0.25f + 0.6f * fm_sinf(s.flapPh);
    }
    s.wingKick = ease(s.wingKick, 0.0f, 2.5f, dt);
    // How fast it turns and climbs; the tail's follow-through, a spring (it overshoots and settles)
    if (dt > 0.0f) {
        s.yawRate = ease(s.yawRate, remainderf(b->rot[1] - s.yawPrev, 2.0f * T3D_PI) / dt, 8.0f, dt);
        s.altVel = ease(s.altVel, (s.alt - s.altPrev) / dt, 8.0f, dt);
    }
    s.yawPrev = b->rot[1]; s.altPrev = s.alt;
    s.tailV += (40.0f * (flying() ? 0.0f : fminf(fmaxf(-s.yawRate * 0.35f, -0.7f), 0.7f) - s.tailLag) - 7.0f * s.tailV) * fminf(dt, 0.05f);
    s.tailLag = fminf(fmaxf(s.tailLag + s.tailV * dt, -0.9f), 0.9f);

    rig(b, dt);
#ifdef HLE_LOG_RIG      // test aid: per move, the worst stretch of what is drawn (1: as built) and the head's nearest pass of its own body
    {
        static int st = -1; static float leg, neckLo = 9, neckHi, torsoLo = 9, torsoHi, headIn = 1e9f, footIn = 1e9f;
        if (s.state != st) {
            if (st >= 0) debugf("[rig] move %d leg %.2f neck %.2f..%.2f torso %.2f..%.2f head-body %.1f foot-foot %.1f\n", st, (double)leg, (double)neckLo, (double)neckHi, (double)torsoLo, (double)torsoHi, (double)headIn, (double)footIn);
            st = s.state; leg = 0; neckLo = torsoLo = 9; neckHi = torsoHi = 0; headIn = footIn = 1e9f;
        }
        for (int k = 0; k < 4; k++) { float d[3] = { s.foot[k][0] - s.shoulder[k][0], s.foot[k][1] - s.shoulder[k][1], s.foot[k][2] - s.shoulder[k][2] }; leg = fmaxf(leg, len3(d) / (2.0f * LEG_L)); }
        neckLo = fminf(neckLo, s.neckReach); neckHi = fmaxf(neckHi, s.neckReach);
        float t[3] = { s.hips[0] - s.chest[0], s.hips[1] - s.chest[1], s.hips[2] - s.chest[2] }, tl = len3(t) / (CHEST_Z - HIPS_Z);
        torsoLo = fminf(torsoLo, tl); torsoHi = fmaxf(torsoHi, tl);
        // the skull (6 round) against the torso (17) and the front legs (6): < 0 is inside
        float sn[3]; mad3(sn, s.head, s.hz, 20.0f);
        for (int j = 0; j <= 2; j++) { float p[3]; lerp3(p, s.head, sn, j * 0.5f);
            float dd[3] = { p[0] - s.chest[0], p[1] - s.chest[1], p[2] - s.chest[2] }; headIn = fminf(headIn, len3(dd) - 17.0f - 6.0f);
            for (int k = 0; k < 2; k++) { float e[3] = { p[0] - s.foot[k][0], p[1] - s.foot[k][1], p[2] - s.foot[k][2] }; headIn = fminf(headIn, len3(e) - 6.0f - 4.0f); } }
        float ff[3] = { s.foot[0][0] - s.foot[1][0], 0, s.foot[0][2] - s.foot[1][2] }; footIn = fminf(footIn, len3(ff));
    }
#endif
    hits_after_rig(b, dt);
    hazards_update(dt);
#ifdef HLE_DRAGON_TRACE   // test aid: the pose every frame (snaps), the player and the beam
    extern float hle_dummy_damage;
    debugf("[dtrace] %.3f st %d step %d t %.2f pos %.1f %.1f alt %.1f yaw %.3f nyaw %.3f npitch %.3f mouth %.1f %.1f %.1f beam %d %.1f %.1f pl %.1f %.1f dmg %.0f\n",
           (double)s.life, s.state, s.step, (double)s.t, (double)b->pos[0], (double)b->pos[2], (double)s.alt, (double)b->rot[1], (double)s.neckYaw, (double)s.neckPitch,
           (double)s.mouth[0], (double)s.mouth[1], (double)s.mouth[2], s.beamOn, (double)s.beamP[0], (double)s.beamP[2], (double)character.pos[0], (double)character.pos[2], (double)hle_dummy_damage);
#endif
}

// ---- Drawing ----

static void put(const ProcMesh *m, const float p[3], const float x[3], const float y[3], const float z[3],
                float sx, float sy, float sz)
{
    if (!m->block || s.mi >= MAT_MAX) return;
    sx /= 8.0f; sy /= 8.0f; sz /= 8.0f;     // proc_mesh stores 1/8 units
    if (g_viewFrustum) {    // out of view: not drawn (the RSP would transform it all to clip it away)
        T3DVec3 c;
        for (int k = 0; k < 3; k++) c.v[k] = p[k] + x[k] * sx * m->bc[0] + y[k] * sy * m->bc[1] + z[k] * sz * m->bc[2];
        if (!t3d_frustum_vs_sphere(g_viewFrustum, &c, m->br * fmaxf(fabsf(sx), fmaxf(fabsf(sy), fabsf(sz))))) return;
    }
    T3DMat4FP *o = &s.mats[s.matRing * MAT_MAX + s.mi++];
    T3DMat4 M = {{
        { x[0] * sx, x[1] * sx, x[2] * sx, 0 }, { y[0] * sy, y[1] * sy, y[2] * sy, 0 },
        { z[0] * sz, z[1] * sz, z[2] * sz, 0 }, { p[0], p[1], p[2], 1 },
    }};
    t3d_mat4_to_fixed(o, &M);
    t3d_matrix_set(o, true);
    rspq_block_run(m->block);
}

// A tube mesh (built along +z, MESH across) from `a` to `b`, `r` thick, `over` times as long,
// its top turned toward `upv` (the torso rolls with the body)
static void put_link_up(const ProcMesh *m, const float a[3], const float b[3], float r, float over, const float upv[3])
{
    float z[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    float l = len3(z);
    if (l < 1e-3f) return;
    for (int i = 0; i < 3; i++) z[i] /= l;
    const float *up = fabsf(z[1]) > 0.95f ? s.F : upv;
    float x[3], y[3];
    cross3(x, up, z); norm3(x);
    cross3(y, z, x);
    put(m, a, x, y, z, r / MESH, r / MESH, l * over / MESH);
}
static void put_link(const ProcMesh *m, const float a[3], const float b[3], float r, float over) { put_link_up(m, a, b, r, over, UP); }

static void put_glow(const ProcMesh *m, const float p[3], float size)
{
    static const float X[3] = { 1, 0, 0 }, Y[3] = { 0, 1, 0 }, Z[3] = { 0, 0, 1 };
    put(m, p, X, Y, Z, size / 8.0f, size / 8.0f, size / 8.0f);
}

static void knee(const float a[3], const float c[3], const float bend[3], float out[3])
{
    float d[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    float l = len3(d);
    if (l < 1e-3f) { memcpy(out, a, sizeof(float) * 3); return; }
    for (int i = 0; i < 3; i++) d[i] /= l;
    float lc = fminf(l, 2.0f * LEG_L - 0.01f), h = sqrtf(fmaxf(0.0f, LEG_L * LEG_L - lc * lc * 0.25f));
    float bd = bend[0] * d[0] + bend[1] * d[1] + bend[2] * d[2];
    float bp[3] = { bend[0] - d[0] * bd, bend[1] - d[1] * bd, bend[2] - d[2] * bd };
    norm3(bp);
    for (int i = 0; i < 3; i++) out[i] = a[i] + d[i] * lc * 0.5f + bp[i] * h;
}

#ifndef HLE_DRAGON_CULL      // test aid: the face culled on the closed parts (0: none)
#define HLE_DRAGON_CULL T3D_FLAG_CULL_BACK
#endif
#define DRAW_OPEN   (T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT | T3D_FLAG_TEXTURED)
#define DRAW_CLOSED (DRAW_OPEN | HLE_DRAGON_CULL)
#ifdef HLE_PROF_DRAGON   // test aid: RSP+RDP time of each part (body, legs, head, wings, glows), each run alone
static uint32_t s_dp[5], s_dpN;
#define DPROF(i) { s_dp[i] += gpu_prof_end(); if (i < 4) gpu_prof_begin(); }
#endif
#ifdef HLE_DRAGON_FACE      // test aid: where the head is and which way it looks (the camera aid)
void dragon_boss_hle_head(float p[3], float z[3], float y[3]) { memcpy(p, s.head, 12); memcpy(z, s.hz, 12); memcpy(y, s.hy, 12); }
#endif
void dragon_boss_draw(Boss* b)
{
#ifdef HLE_PROF_DRAGON
    gpu_prof_begin();
    if (++s_dpN == 120) { debugf("[dragonprof] body %.2f legs %.2f head %.2f wings %.2f glows %.2f\n", TICKS_TO_US(s_dp[0] / 120) / 1000.0, TICKS_TO_US(s_dp[1] / 120) / 1000.0,
        TICKS_TO_US(s_dp[2] / 120) / 1000.0, TICKS_TO_US(s_dp[3] / 120) / 1000.0, TICKS_TO_US(s_dp[4] / 120) / 1000.0); memset(s_dp, 0, sizeof s_dp); s_dpN = 0; }
#endif
    if (!s.ready || !b->visible) return;
#ifdef HLE_LOG_FPS   // test aid: what it was doing when a refresh was missed
    { extern int deltaFrames; if (deltaFrames > 1) { int nf = 0, no = 0; for (int i = 0; i < FL_MAX; i++) nf += s.fl[i].age < s.fl[i].life; for (int i = 0; i < ORB_MAX; i++) no += s.orb[i].on;
      debugf("[dragonmiss] state %d alt %.0f flames %d orbs %d beam %d\n", s.state, (double)s.alt, nf, no, s.beamOn); } }
#endif
    s.matRing = (s.matRing + 1) % MAT_RING;
    s.mi = 0;
    rdpq_sync_pipe();
    rdpq_mode_push();       // restored at the end: the caller's fog and modes stay
    rdpq_set_mode_standard();
    rdpq_mode_zbuf(true, true);
    proc_mesh_tex_begin(s.texScales);
    // Its body is closed meshes wound outward: their far sides culled (the RSP's triangle setup is
    // most of this boss's cost). The wings stay two-sided
    t3d_state_set_drawflags(DRAW_CLOSED);

    {   // its crystals' tint (the body's crystal material is unlit: its facets' shade times this)
        uint32_t c = crystal_col(s.dark);
        rdpq_set_prim_color(RGBA32(c >> 24, (c >> 16) & 0xFF, (c >> 8) & 0xFF, 255));
    }
    sb_draw(b);         // the skinned body (its own texture, combiner and light)
    proc_mesh_tex_begin(s.texScales);
    t3d_state_set_drawflags(DRAW_CLOSED);

#ifdef HLE_PROF_DRAGON
    DPROF(0);
#endif
    for (int k = 0; k < 4; k++) {
        // The front leg about to slam glows red, pulsing (the claw's windup): its paw
        bool hot = s.state == DG_CLAW && (s.step == 0 || s.step == 2) && k == (s.step < 2 ? 0 : 1);
        if (hot) {
            float w = (0.6f + 0.4f * fm_sinf(s.life * 18.0f)) * clamp01(s.t / 0.3f);
            // blends toward red by w (a tint would only darken its near-black scales)
            rdpq_mode_combiner(RDPQ_COMBINER1((PRIM, SHADE, PRIM_ALPHA, SHADE), (0, 0, 0, SHADE)));
            rdpq_set_prim_color(RGBA32(235, 30, 20, (int)(200 * w)));
        }
        static const float Y[3] = { 0, 1, 0 };
        float fx[3], fs = k < 2 ? 1.6f : 1.25f;     // the forefeet big, clawed (the old feet, larger)
        cross3(fx, Y, s.F);
        put(&s.mFoot, s.foot[k], fx, Y, s.F, fs, fs, fs);
        if (hot) rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
    }

#ifdef HLE_PROF_DRAGON
    DPROF(1);
#endif
    put(s.dark ? &s.mHeadDark : &s.mHead, s.head, s.hx, s.hy, s.hz, 1.0f, 1.0f, 1.0f);
    float hinge[3], jy[3], jz[3], ca = fm_cosf(s.jaw * 0.7f), sa = fm_sinf(s.jaw * 0.7f);
    mad3(hinge, s.head, s.hz, 6.0f);
    mad3(hinge, hinge, s.hy, -2.5f);
    for (int m = 0; m < 3; m++) { jz[m] = s.hz[m] * ca - s.hy[m] * sa; jy[m] = s.hy[m] * ca + s.hz[m] * sa; }
    put(&s.mJaw, hinge, s.hx, jy, jz, 1.0f, 1.0f, 1.0f);
    {   // the throat: from under the back of the skull to the jaw's underside, stretched as it opens
        // (open, the hall showed between the jaw and the neck)
        float ta[3], tb[3];
        mad3(ta, s.head, s.hy, -3.0f); mad3(ta, ta, s.hz, -1.0f);
        mad3(tb, hinge, jz, 7.0f); mad3(tb, tb, jy, -2.8f);     // under the tongue, not through it
        put_link(&s.mThroat, ta, tb, 3.0f, 1.0f);
    }

#ifdef HLE_PROF_DRAGON
    DPROF(2);
#endif
#ifdef HLE_PROF_DRAGON
    DPROF(3);
#endif
    // Glows: fire, dark orbs, the beam, the light in its mouth. Alpha-blended, not
    // additive: the real RDP wraps an overflowing sum (overlapping flames turned rainbow)
    rdpq_sync_pipe();
    t3d_state_set_drawflags(DRAW_OPEN & ~T3D_FLAG_TEXTURED);
    rdpq_mode_zbuf(true, false);
    rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, SHADE), (0, 0, 0, PRIM)));
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_set_prim_color(RGBA32(255, 255, 255, 170));
    for (int i = 0; i < FL_MAX; i++) {
        const Flame *f = &s.fl[i];
        if (f->age >= f->life) continue;
        float u = f->age / f->life;
        put_glow(&s.flame, f->p, f->size * (0.4f + 0.6f * clamp01(u * 2.0f)) * (1.0f - u * u));
    }
    for (int i = 0; i < ORB_MAX; i++)
        if (s.orb[i].on) put_glow(&s.mOrb, s.orb[i].p, 4.5f + 0.8f * fm_sinf(s.life * 9.0f + i));
    for (int i = 0; i < BALL_MAX; i++)
        if (s.ball[i].on) {     // the fireball and a fainter one trailing it
            float tr[3];
            mad3(tr, s.ball[i].p, s.ball[i].v, -0.05f);
            put_glow(&s.flame, s.ball[i].p, 5.5f + 1.0f * fm_sinf(s.life * 14.0f + i));
            put_glow(&s.flame, tr, 3.5f);
        }
    if (s.cone.on) {        // the fire cone: rolls out, flickers, then sinks and fades (harmless by then)
        float k = fmaxf(cone_grow(), 0.04f), v = clamp01((s.cone.t - CONE_GROW - CONE_HOLD) / CONE_FADE);
        float fz[3] = { fm_sinf(s.cone.yaw), 0.0f, fm_cosf(s.cone.yaw) }, fx[3] = { fz[2], 0.0f, -fz[0] }, p[3] = { s.cone.p[0], s.groundY, s.cone.p[1] };
        float flick = 1.0f + 0.12f * fm_sinf(s.life * 31.0f), mir = fm_sinf(s.life * 17.0f) > 0.0f ? 1.0f : -1.0f;     // mirrored now and then: its tongues shift
        rdpq_set_prim_color(RGBA32(255, 255, 255, (int)(170 * (1.0f - v))));
        put(&s.mCone, p, fx, UP, fz, k * mir, k * flick * (1.0f - 0.7f * v), k);
        rdpq_set_prim_color(RGBA32(255, 255, 255, 170));
    }
    const ProcMesh *light = s.dark ? &s.mOrb : &s.mIce;     // the beams: crystal light, then the dark
    if (s.beamOn) {
        float mid[3], z[3] = { s.beamP[0] - s.mouth[0], s.beamP[1] - s.mouth[1], s.beamP[2] - s.mouth[2] };
        float l = len3(z), x[3], y[3];
        if (l > 1e-3f) {
            for (int i = 0; i < 3; i++) z[i] /= l;
            cross3(x, UP, z); norm3(x);
            cross3(y, z, x);
            lerp3(mid, s.mouth, s.beamP, 0.5f);
            float w = s.beamW * (1.0f + 0.15f * fm_sinf(s.life * 40.0f));
            put(light, mid, x, y, z, w / 8.0f, w / 8.0f, l * 0.5f / 8.0f);
            put_glow(light, s.beamP, 9.0f + 2.0f * fm_sinf(s.life * 20.0f));
        }
    }
    if (s.state == DG_TAIL && s.step == 0)      // the tail's crystal blade glows before it swings
        put_glow(light, s.tl[NTAIL], 3.0f + 2.5f * clamp01(s.t / 0.5f) + fm_sinf(s.life * 16.0f));
    bool beamish = s.state == DG_BEAM || s.state == DG_NOVA || s.state == DG_SNIPE || s.state == DG_SKYBEAM || s.state == DG_HOVER;
    if (s.glow > 0.01f) put_glow(beamish || s.dark ? light : &s.flame, s.mouth, 2.0f + 5.0f * s.glow);
#ifdef HLE_PROF_DRAGON
    DPROF(4);
#endif
    rdpq_mode_pop();
}
