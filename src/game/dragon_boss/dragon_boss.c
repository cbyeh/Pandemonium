#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>
#include <math.h>
#include <string.h>

#include "dragon_boss.h"
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
#define HIT_R      17.0f
#define HIT_H      18.0f        // the hit capsule's height above its feet
#define DROP_H     160.0f       // flies down from this high

#define FL_MAX   40             // flames
#define ORB_MAX  8
#define BL_MAX   14             // the dark beam's delayed explosions
#define MAT_MAX  (40 + FL_MAX + ORB_MAX + 4)
#define MAT_RING 3              // frames the RSP may still be reading

#define COL_BACK   proc_rgb(46, 42, 54)
#define COL_BELLY  proc_rgb(82, 74, 78)
#define COL_BONE   proc_rgb(126, 118, 122)
#define COL_MEMB   proc_rgb(40, 32, 48)
#define COL_IVORY  proc_rgb(232, 226, 210)

typedef enum { DG_INTRO, DG_IDLE, DG_CLAW, DG_BITE, DG_TAIL, DG_BREATH, DG_CHARGE, DG_FLY,
               DG_ROAR, DG_NOVA, DG_ORBS, DG_BEAM, DG_TIRED, DG_DEAD } DgState;

typedef struct { float p[3], from[3], t; bool moving; } Foot;
typedef struct { float p[3], v[3], age, life, size; } Flame;
typedef struct { float p[3], v[3], age; bool on; } Orb;
typedef struct { float p[2], t; bool on; } Blast;

static struct {
    bool ready;
    ProcMesh seg, segDark, tseg, tsegDark, limb, mFoot, mHead, mHeadDark, mJaw, wing, wingDark, tip, tipDark, flame, mOrb;
    T3DMat4FP *mats;
    int matRing, mi;
    DgState state;
    float t, life;
    int step;
    float groundY, alt;
    bool dark;                  // the second half: the dark has taken it
    // The pose, eased each frame toward the targets the current move sets
    float neckPitch, neckYaw, neckReach, jaw, wingRaise, wingSpan, tail, crouch, claw[2], aimK;
    float tNeckPitch, tNeckYaw, tNeckReach, tJaw, tWingRaise, tWingSpan, tTail, tCrouch, tClaw[2], tAimK;
    float poseRate, flapRate, flapPh;
    float aim[3];               // where the head looks when aimK > 0
    // The rig, rebuilt each frame
    float F[3], R[3], chest[3], hips[3], neck[NNECK + 1][3], tl[NTAIL + 1][3];
    float head[3], hx[3], hy[3], hz[3], mouth[3], shoulder[4][3], foot[4][3];
    Foot feet[4];
    bool feetReady;
    // Moves
    float cdClaw, cdBite, cdTail, cdBreath, cdCharge, cdFly, cdNova, cdOrbs, cdBeam;
    float sweepFrom, sweepSpan, range, target[3], flyTo[2], emit, fireCd, glow;
    bool hitDone;
    Flame fl[FL_MAX];
    int flNext;
    Orb orb[ORB_MAX];
    int orbNext;
    Blast bl[BL_MAX];
    int blNext;
    float beamP[3];
    bool beamOn;
} s;

static inline float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
static inline float ease(float cur, float to, float rate, float dt) { return cur + (to - cur) * fminf(1.0f, rate * dt); }
static inline float frand(void) { return (rand_custom_u32() % 2001) / 1000.0f - 1.0f; }     // -1..1
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
    float x = b->pos[0] + dx, z = b->pos[2] + dz, fx = sinf(b->rot[1]), fz = cosf(b->rot[1]);
    if (!inside(x, z, 40.0f) || !inside(x + fx * 70.0f, z + fz * 70.0f, 5.0f) || !inside(x - fx * 80.0f, z - fz * 80.0f, 5.0f)) return false;
    b->pos[0] = x; b->pos[2] = z;
    return true;
}

static void enter(DgState st)
{
    s.state = st; s.t = 0.0f; s.step = 0; s.hitDone = false;
#ifdef HLE_AUTOPLAY
    debugf("[dragon] state %d\n", st);
#endif
}

static bool player_low(void) { return character.pos[1] - s.groundY < 10.0f; }
static void player_chest(float o[3]) { o[0] = character.pos[0]; o[1] = character.pos[1] + 12.0f; o[2] = character.pos[2]; }

static void sfx_boom(void) { audio_play_scene_sfx_dist(SCENE1_SFX_BOSS_SMASH2, 1.0f, 0.0f); }
static void sfx_swing(void) { audio_play_scene_sfx_dist(SCENE1_SFX_BOSS_SWING3, 1.0f, 0.0f); }
static void sfx_lunge(void) { audio_play_scene_sfx_dist(SCENE1_SFX_BOSS_LUNGE, 1.0f, 0.0f); }
static void sfx_roar(void)
{
#ifdef HLE_AUTOPLAY
    debugf("[dragon] roar\n");
#endif
    audio_play_scene_sfx_dist(SCENE1_SFX_DRAGON_ROAR, 1.0f, 0.0f);
}
static void sfx(int id, float vol) { audio_play_scene_sfx_dist(id, vol, 0.0f); }
static void sfx_step(void) { audio_play_scene_sfx_dist(SCENE1_SFX_BOSS_STEP1, 0.6f, 0.0f); }

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
        float y = yc + sinf(a) * h;
        if (y < yc - flatBelow) y = yc - flatBelow;
        out[i * 3 + 0] = cosf(a) * w; out[i * 3 + 1] = y; out[i * 3 + 2] = z;
    }
}

// Scale colours round a ring: the back in two shades (scales), the underside in plates
static void scale_cols(uint32_t *cols, int rings, int n, uint32_t back, uint32_t back2, uint32_t belly, int bellyQuads)
{
    for (int r = 0; r < rings; r++)
        for (int i = 0; i < n; i++) {
            // quad i spans angles i..i+1 from the right side, going up first; the belly is the last ones
            bool under = i >= n - bellyQuads - n / 4 && i < n - n / 4 + bellyQuads / 2 + 1 && sinf(2.0f * T3D_PI * (i + 0.5f) / n) < -0.35f;
            cols[r * n + i] = under ? ((r & 1) ? belly : (belly & 0xF0F0F0FFu)) : (((i + r) & 1) ? back : back2);
        }
}

// A body or neck segment along +z: a smooth ten-sided tube tapering to 0.82, scaled back,
// banded belly plates, crystals down the spine and a ridge of small ones on each flank
static void build_seg(ProcMesh *out, bool dark)
{
    enum { N = 10, R = 3 };
    float pts[R * N * 3];
    for (int r = 0; r < R; r++) {
        float k = 1.0f - 0.18f * r / (R - 1), pinch = (r == 1) ? 0.95f : 1.0f;   // a plate seam halfway
        ring(&pts[r * N * 3], N, MESH * r / (R - 1), MESH * k * pinch, MESH * k * pinch * 0.92f, 0.0f, 99.0f);
    }
    uint32_t cols[(R - 1) * N];
    scale_cols(cols, R - 1, N, COL_BACK, proc_rgb(42, 38, 50), COL_BELLY, 2);
    proc_mesh_begin();
    proc_mesh_loft(pts, R, N, cols, COL_BACK, false);
    uint32_t cr = crystal_col(dark);
    crystal((float[3]){ 0, MESH * 0.88f, 3.0f }, (float[3]){ 0, MESH * 1.95f, -5.0f }, 4.0f, cr);
    crystal((float[3]){ 0, MESH * 0.8f, 11.0f }, (float[3]){ 0, MESH * 1.55f, 6.0f }, 3.0f, cr);
    for (int sd = -1; sd <= 1; sd += 2)
        crystal((float[3]){ sd * MESH * 0.62f, MESH * 0.6f, 7.0f }, (float[3]){ sd * MESH * 1.15f, MESH * 1.02f, 2.0f }, 2.2f, cr);
    proc_mesh_end(out);
}

// A tail segment: the same, lighter (eight sides, one crystal)
static void build_tail_seg(ProcMesh *out, bool dark)
{
    enum { N = 8, R = 2 };
    float pts[R * N * 3];
    for (int r = 0; r < R; r++) ring(&pts[r * N * 3], N, MESH * r, MESH * (1.0f - 0.18f * r), MESH * (1.0f - 0.18f * r) * 0.9f, 0.0f, 99.0f);
    uint32_t cols[N];
    scale_cols(cols, 1, N, COL_BACK, proc_rgb(42, 38, 50), COL_BELLY, 2);
    proc_mesh_begin();
    proc_mesh_loft(pts, R, N, cols, COL_BACK, false);
    crystal((float[3]){ 0, MESH * 0.85f, 6.0f }, (float[3]){ 0, MESH * 1.8f, -2.0f }, 3.5f, crystal_col(dark));
    proc_mesh_end(out);
}

// A leg bone along +z: a smooth six-sided tube
static void build_limb(void)
{
    enum { N = 6, R = 2 };
    float pts[R * N * 3];
    for (int r = 0; r < R; r++) ring(&pts[r * N * 3], N, MESH * r, MESH * (1.0f - 0.2f * r), MESH * (1.0f - 0.2f * r), 0.0f, 99.0f);
    proc_mesh_begin();
    proc_mesh_loft(pts, R, N, NULL, COL_BACK, false);
    proc_mesh_end(&s.limb);
}

// A foot, +z forward, sole on y = 0: a pad and three hooked claws
static void build_foot(void)
{
    proc_mesh_begin();
    proc_mesh_box((float[3]){ 0, 1.5f, 1.5f }, (float[3]){ 3.4f, 1.6f, 3.6f }, COL_BACK);
    for (int k = -1; k <= 1; k++) {
        float x = k * 2.4f;
        proc_mesh_spike((float[3]){ x, 1.6f, 4.6f }, (float[3]){ x * 1.2f, 0.0f, 8.2f }, 1.0f, 3, COL_IVORY);
    }
    proc_mesh_spike((float[3]){ 0, 1.8f, -1.6f }, (float[3]){ 0, 0.2f, -4.0f }, 0.8f, 3, COL_IVORY);   // the dewclaw
    proc_mesh_end(&s.mFoot);
}

// The head, +z forward from the top of the neck: a lofted skull tapering to the snout, a flat
// jaw line, brows over glowing eyes, swept horns, cheek spikes, nostrils, the upper teeth
static void build_head(ProcMesh *out, bool dark)
{
    enum { N = 8, R = 8 };
    static const float Z[R] = { -2, 2, 7, 11, 15, 19, 23, 26.5f };
    static const float W[R] = { 5.6f, 6.8f, 6.7f, 5.7f, 4.7f, 4.0f, 3.4f, 1.9f };
    static const float H[R] = { 5.0f, 5.6f, 5.4f, 4.2f, 3.3f, 2.8f, 2.3f, 1.3f };
    static const float Y[R] = { 2.0f, 2.6f, 2.9f, 2.4f, 1.6f, 1.0f, 0.6f, 0.3f };
    float pts[R * N * 3];
    for (int r = 0; r < R; r++) ring(&pts[r * N * 3], N, Z[r], W[r], H[r], Y[r], H[r] * 0.45f);
    uint32_t cols[(R - 1) * N];
    for (int r = 0; r < R - 1; r++)
        for (int i = 0; i < N; i++) {
            float sn = sinf(2.0f * T3D_PI * (i + 0.5f) / N);
            cols[r * N + i] = sn < -0.3f ? proc_rgb(70, 30, 34) : (((i + r) & 1) ? COL_BACK : proc_rgb(42, 38, 50));   // the mouth line
        }
    uint32_t cr = crystal_col(dark), eye = dark ? proc_rgb(240, 90, 255) : proc_rgb(255, 130, 40);
    proc_mesh_begin();
    proc_mesh_loft(pts, R, N, cols, COL_BACK, true);
    for (int sd = -1; sd <= 1; sd += 2) {
        proc_mesh_box((float[3]){ sd * 4.1f, 6.0f, 11.0f }, (float[3]){ 1.6f, 0.9f, 3.2f }, COL_BACK);                     // brow
        crystal((float[3]){ sd * 4.9f, 4.4f, 13.5f }, (float[3]){ sd * 5.9f, 4.7f, 9.5f }, 1.1f, eye);                        // eye
        proc_mesh_spike((float[3]){ sd * 4.0f, 6.4f, 4.0f }, (float[3]){ sd * 8.5f, 13.5f, -15.0f }, 2.3f, 4, COL_BONE);   // horns
        proc_mesh_spike((float[3]){ sd * 6.0f, 3.0f, 1.0f }, (float[3]){ sd * 12.5f, 3.5f, -9.0f }, 1.6f, 4, COL_BONE);
        proc_mesh_spike((float[3]){ sd * 5.6f, 0.8f, 7.0f }, (float[3]){ sd * 9.0f, -0.5f, 2.0f }, 1.0f, 3, COL_BONE);     // cheek
        proc_mesh_box((float[3]){ sd * 1.4f, 1.4f, 25.6f }, (float[3]){ 0.5f, 0.4f, 0.6f }, proc_rgb(20, 14, 16));        // nostril
        for (int k = 0; k < 4; k++)
            crystal((float[3]){ sd * (3.6f - k * 0.4f), -0.4f, 12.0f + k * 3.6f }, (float[3]){ sd * (3.4f - k * 0.4f), -2.8f, 12.6f + k * 3.6f }, 0.7f, COL_IVORY);
    }
    crystal((float[3]){ 0, 7.4f, 4.0f }, (float[3]){ 0, 13.5f, -5.0f }, 2.6f, cr);                                           // crest
    crystal((float[3]){ 0, 6.8f, -1.0f }, (float[3]){ 0, 10.5f, -9.0f }, 2.0f, cr);
    crystal((float[3]){ 0, 3.0f, 21.0f }, (float[3]){ 0, 5.4f, 19.0f }, 0.9f, cr);                                            // nose horn
    proc_mesh_end(out);
}

// The lower jaw, +z forward from its hinge: lofted, the mouth inside on top, teeth up
static void build_jaw(void)
{
    enum { N = 6, R = 5 };
    static const float Z[R] = { 0, 6, 12, 18, 22.5f };
    static const float W[R] = { 4.6f, 4.4f, 3.8f, 3.0f, 1.6f };
    static const float H[R] = { 2.2f, 2.0f, 1.8f, 1.4f, 0.8f };
    float pts[R * N * 3];
    for (int r = 0; r < R; r++) ring(&pts[r * N * 3], N, Z[r], W[r], H[r], -1.6f, 99.0f);
    uint32_t cols[(R - 1) * N];
    for (int r = 0; r < R - 1; r++)
        for (int i = 0; i < N; i++) cols[r * N + i] = sinf(2.0f * T3D_PI * (i + 0.5f) / N) > 0.3f ? proc_rgb(110, 40, 44) : COL_BELLY;
    proc_mesh_begin();
    proc_mesh_loft(pts, R, N, cols, COL_BELLY, true);
    for (int sd = -1; sd <= 1; sd += 2)
        for (int k = 0; k < 4; k++)
            crystal((float[3]){ sd * (3.2f - k * 0.45f), 0.2f, 7.0f + k * 3.6f }, (float[3]){ sd * (3.0f - k * 0.45f), 2.6f, 7.6f + k * 3.6f }, 0.6f, COL_IVORY);
    proc_mesh_end(&s.mJaw);
}

// The right wing, spanning +x: arm, wrist claw and five fingers; the membrane scalloped
// between the fingertips; crystals along the leading edge
static void build_wing(ProcMesh *out, bool dark)
{
    const float root[3] = { 0, 0, 0 }, elbow[3] = { 30, 6, 4 }, wrist[3] = { 58, 10, -6 }, back[3] = { 2, -2, -32 };
    const float tip[5][3] = { { 88, 5, -14 }, { 80, 2, -32 }, { 66, 0, -46 }, { 48, -2, -54 }, { 28, -3, -48 } };
    proc_mesh_begin();
    proc_mesh_spike(root, elbow, 2.6f, 4, COL_BONE);
    proc_mesh_spike(elbow, wrist, 2.1f, 4, COL_BONE);
    proc_mesh_spike(wrist, (float[3]){ wrist[0] + 6, wrist[1] + 3, wrist[2] + 6 }, 1.0f, 3, COL_IVORY);    // wrist claw
    for (int i = 0; i < 5; i++) proc_mesh_spike(wrist, tip[i], 1.2f, 3, COL_BONE);
    const uint32_t memb = COL_MEMB, memb2 = proc_rgb(52, 36, 58);
    for (int i = 0; i < 4; i++) {
        // the edge between two fingertips sags in toward the wrist
        float mid[3];
        for (int k = 0; k < 3; k++) mid[k] = (tip[i][k] + tip[i + 1][k]) * 0.5f * 0.78f + wrist[k] * 0.22f;
        proc_mesh_tri(wrist, tip[i], mid, (i & 1) ? memb : memb2, false);
        proc_mesh_tri(wrist, mid, tip[i + 1], (i & 1) ? memb2 : memb, false);
    }
    float mid[3];
    for (int k = 0; k < 3; k++) mid[k] = (tip[4][k] + back[k]) * 0.5f * 0.8f + elbow[k] * 0.2f;
    proc_mesh_tri(wrist, tip[4], elbow, memb, false);
    proc_mesh_tri(elbow, tip[4], mid, memb2, false);
    proc_mesh_tri(elbow, mid, back, memb, false);
    proc_mesh_tri(elbow, back, root, memb2, false);
    uint32_t cr = crystal_col(dark);
    crystal(elbow, (float[3]){ elbow[0] + 1, elbow[1] + 8, elbow[2] + 4 }, 2.0f, cr);
    crystal(wrist, (float[3]){ wrist[0] + 5, wrist[1] + 9, wrist[2] + 2 }, 2.2f, cr);
    crystal((float[3]){ 44, 8, -1 }, (float[3]){ 46, 13, 2 }, 1.4f, cr);
    proc_mesh_end(out);
}

// The tail's tip: a crystal blade along +z
static void build_tail_tip(ProcMesh *out, bool dark)
{
    uint32_t cr = crystal_col(dark);
    proc_mesh_begin();
    crystal((float[3]){ 0, 0, 0 }, (float[3]){ 0, 0, 16.0f }, 3.4f, cr);
    for (int sd = -1; sd <= 1; sd += 2)
        crystal((float[3]){ 0, 0, 3.0f }, (float[3]){ sd * 6.0f, 1.0f, 8.0f }, 1.6f, cr);
    proc_mesh_end(out);
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

// ---- Lifetime ----

void dragon_boss_free(void)
{
    if (!s.ready) return;
    rspq_wait();
    ProcMesh *all[] = { &s.seg, &s.segDark, &s.tseg, &s.tsegDark, &s.limb, &s.mFoot, &s.mHead, &s.mHeadDark, &s.mJaw, &s.wing, &s.wingDark, &s.tip, &s.tipDark, &s.flame, &s.mOrb };
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) proc_mesh_free(all[i]);
    free_uncached(s.mats);
    memset(&s, 0, sizeof(s));
}

static void clear_hazards(void)
{
    for (int i = 0; i < FL_MAX; i++) s.fl[i].life = 0.0f;
    for (int i = 0; i < ORB_MAX; i++) s.orb[i].on = false;
    for (int i = 0; i < BL_MAX; i++) s.bl[i].on = false;
    s.beamOn = false;
    s.glow = 0.0f;
}

void dragon_boss_begin(Boss* boss)
{
    if (!s.ready) {
        build_seg(&s.seg, false); build_seg(&s.segDark, true);
        build_tail_seg(&s.tseg, false); build_tail_seg(&s.tsegDark, true);
        build_tail_tip(&s.tip, false); build_tail_tip(&s.tipDark, true);
        build_limb();
        build_foot();
        build_head(&s.mHead, false); build_head(&s.mHeadDark, true);
        build_jaw();
        build_wing(&s.wing, false); build_wing(&s.wingDark, true);
        build_glow(&s.flame, proc_rgb(255, 120, 30), proc_rgb(255, 200, 70));
        build_glow(&s.mOrb, proc_rgb(150, 50, 255), proc_rgb(230, 170, 255));
        s.mats = malloc_uncached(sizeof(T3DMat4FP) * MAT_RING * MAT_MAX);
        s.ready = true;
    }
    boss->phaseIndex = DRAGON_BOSS_PHASE;
    boss->name = "Shadowborn";
    boss->health = boss->maxHealth;
    boss->mode = BOSS_MODE_CINEMATIC;       // not damageable until it has landed
    boss->state = BOSS_STATE_INTRO;
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
    s.life = 0.0f;
    s.feetReady = false;
    s.neckReach = s.tNeckReach = 1.0f;
    s.wingSpan = 1.0f; s.wingRaise = 0.3f;
    s.cdClaw = 2.0f; s.cdBite = 4.0f; s.cdTail = 3.0f; s.cdBreath = 6.0f; s.cdCharge = 9.0f; s.cdFly = 16.0f;
    s.cdNova = 0.0f; s.cdOrbs = 4.0f; s.cdBeam = 9.0f;
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

static void flame_spawn(const float p[3], const float v[3], float life, float size)
{
    Flame *f = &s.fl[s.flNext];
    s.flNext = (s.flNext + 1) % FL_MAX;
    memcpy(f->p, p, sizeof(f->p)); memcpy(f->v, v, sizeof(f->v));
    f->age = 0.0f; f->life = life; f->size = size;
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
        float d[3] = { f->p[0] - pc[0], f->p[1] - pc[1], f->p[2] - pc[2] };
        float r = f->size * (0.4f + 0.6f * clamp01(f->age / (f->life * 0.5f)));
        if (s.fireCd <= 0.0f && len3(d) < r * 0.8f + 6.0f) { character_apply_damage(9.0f); s.fireCd = 0.4f; }
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
        float d[3] = { o->p[0] - pc[0], o->p[1] - pc[1], o->p[2] - pc[2] };
        bool hit = len3(d) < 8.0f;
        if (hit) character_apply_damage(12.0f);
        if (hit || o->p[1] < s.groundY + 1.0f || o->age > 7.0f || !room_ok(o->p[0], o->p[2])) {
            o->on = false;
            skill_fx_spawn_burst_hostile((float[3]){ o->p[0], fmaxf(o->p[1], s.groundY), o->p[2] }, 0.0f);
        }
    }
    for (int i = 0; i < BL_MAX; i++) {
        Blast *e = &s.bl[i];
        if (!e->on) continue;
        e->t -= dt;
        if (e->t > 0.0f) continue;
        e->on = false;
        float p[3] = { e->p[0], s.groundY, e->p[1] };
        skill_fx_spawn_wave_hostile(SKILL_WAVE_BLADE, p, (float[3]){ 0, 0, 0 }, 0.0f, 0, 1.0f, 0.8f, false);
        float dx = character.pos[0] - p[0], dz = character.pos[2] - p[2];
        if (dx * dx + dz * dz < 13.0f * 13.0f && player_low()) character_apply_damage(12.0f);
        screen_shake_set_shake_mag(3.0f);
        if (i & 1) sfx(SCENE1_SFX_ROCK_BLAST, 0.7f);     // every other one: they come five a second
    }
}

// Breath: a stream of flames from the mouth toward `to`
static void breathe(const float to[3], float speed, float life, float dt)
{
    static float roarT;
    roarT -= dt;
    if (roarT <= 0.0f) { sfx(SCENE1_SFX_FIRE, 0.9f); roarT = 0.45f; }     // the torrent crackles on
    s.emit -= dt;
    while (s.emit <= 0.0f) {
        s.emit += 0.03f;
        float v[3] = { to[0] - s.mouth[0], to[1] - s.mouth[1], to[2] - s.mouth[2] };
        norm3(v);
        for (int m = 0; m < 3; m++) v[m] = v[m] * speed + frand() * 12.0f;
        flame_spawn(s.mouth, v, life, 16.0f + 4.0f * frand());
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
        if (s.alt > 4.0f) {                     // in the air the legs hang
            float hang[3];
            mad3(hang, s.shoulder[k], UP, -LEG_L * 1.6f);
            mad3(hang, hang, s.F, -8.0f);
            lerp3(f->p, f->p, hang, fminf(1.0f, dt * 6.0f));
            f->moving = false;
        } else if (f->moving) {
            f->t += dt / 0.3f;
            float u = clamp01(f->t);
            lerp3(f->p, f->from, rest, u);
            f->p[1] = s.groundY + sinf(u * T3D_PI) * 7.0f;
            if (u >= 1.0f) { f->moving = false; if (k < 2) sfx_step(); }
        } else {
            f->p[1] = fmaxf(s.groundY, f->p[1] - 90.0f * dt);
            float dx = f->p[0] - rest[0], dz = f->p[2] - rest[2];
            if (dx * dx + dz * dz > 16.0f * 16.0f && !s.feet[k ^ 1].moving) {
                f->moving = true; f->t = 0.0f;
                memcpy(f->from, f->p, sizeof(f->p));
            }
        }
        memcpy(s.foot[k], f->p, sizeof(f->p));
        if (k < 2 && s.claw[k] > 0.01f) {      // a front claw raised to strike
            float side = (k & 1) ? 1.0f : -1.0f, up[3];
            mad3(up, s.shoulder[k], s.F, 20.0f);
            mad3(up, up, s.R, -side * 4.0f);
            up[1] += 6.0f;
            lerp3(s.foot[k], f->p, up, s.claw[k]);
        }
    }
}

static void rig(Boss *b, float dt)
{
    float yaw = b->rot[1];
    s.F[0] = sinf(yaw); s.F[1] = 0.0f; s.F[2] = cosf(yaw);
    s.R[0] = cosf(yaw); s.R[1] = 0.0f; s.R[2] = -sinf(yaw);
    float base = s.groundY + s.alt, breath = sinf(s.life * 1.7f) * 0.8f;
    b->pos[1] = base;

    mad3(s.chest, b->pos, s.F, CHEST_Z);
    s.chest[1] = base + BODY_H + 2.0f - s.crouch + breath;
    mad3(s.hips, b->pos, s.F, HIPS_Z);
    s.hips[1] = base + BODY_H - s.crouch * 0.6f;

    // The neck curves up from the chest and over, toward its yaw
    float p0 = 0.6f + s.neckPitch, dir[3] = { 0 };
    memcpy(s.neck[0], s.chest, sizeof(s.chest));
    for (int i = 0; i < NNECK; i++) {
        float yi = yaw + s.neckYaw * (i + 1) / NNECK, pi = p0 - 0.22f * i;
        dir[0] = sinf(yi) * cosf(pi); dir[1] = sinf(pi); dir[2] = cosf(yi) * cosf(pi);
        mad3(s.neck[i + 1], s.neck[i], dir, NECK_SEG * s.neckReach);
        if (s.neck[i + 1][1] < s.groundY + 6.0f) s.neck[i + 1][1] = s.groundY + 6.0f;
    }
    memcpy(s.head, s.neck[NNECK], sizeof(s.head));
    float q = p0 - 0.22f * NNECK - 0.1f, yh = yaw + s.neckYaw;
    s.hz[0] = sinf(yh) * cosf(q); s.hz[1] = sinf(q); s.hz[2] = cosf(yh) * cosf(q);
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

    // The tail trails back and down, swaying; `tail` swings it to a side
    memcpy(s.tl[0], s.hips, sizeof(s.hips));
    for (int i = 0; i < NTAIL; i++) {
        float k = (float)(i + 1) / NTAIL;
        float yi = yaw + T3D_PI + (sinf(s.life * 1.4f - i * 0.7f) * 0.1f + s.tail) * k, pi = -0.12f - 0.05f * i;
        dir[0] = sinf(yi) * cosf(pi); dir[1] = sinf(pi); dir[2] = cosf(yi) * cosf(pi);
        mad3(s.tl[i + 1], s.tl[i], dir, TAIL_SEG);
        if (s.tl[i + 1][1] < s.groundY + 3.0f) s.tl[i + 1][1] = s.groundY + 3.0f;
    }

    // Shoulders and hips: front left, front right, hind left, hind right
    for (int k = 0; k < 4; k++) {
        float side = (k & 1) ? 1.0f : -1.0f;
        mad3(s.shoulder[k], k < 2 ? s.chest : s.hips, s.R, side * (k < 2 ? 12.0f : 11.0f));
        s.shoulder[k][1] -= k < 2 ? 6.0f : 4.0f;
    }
    feet_update(dt);

    // The hit capsule runs the length of the body, from the base of the neck (or the head,
    // when it is down) to the hips, at the height a sword swings (the spine is above it:
    // a capsule on the spine was out of reach)
    float a[3], c[3];
    bool headDown = s.head[1] - s.groundY < 16.0f;
    lerp3(a, s.neck[1], s.head, headDown ? 1.0f : 0.0f);
    memcpy(c, s.hips, sizeof(c));
    if (!headDown) a[1] = base + HIT_H;
    c[1] = base + HIT_H;
    for (int m = 0; m < 3; m++) {
        b->capsuleCollider.localCapA.v[m] = a[m] - b->pos[m];
        b->capsuleCollider.localCapB.v[m] = c[m] - b->pos[m];
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
    sfx_swing();
}

static void claw_update(Boss *b, float dt)
{
    // Two swipes, left then right: step out of reach, or roll through
    face_player(b, dt, 1.6f);
    int k = s.step < 2 ? 0 : 1;
    s.tCrouch = 3.0f; s.tNeckPitch = -0.15f;
    float wind = (k ? 0.4f : 0.55f) / (s.dark ? 1.2f : 1.0f);
    if (s.step == 0 || s.step == 2) {
        s.tClaw[k] = 1.0f;
        if (s.t >= wind) { claw_strike(b, k); s.step++; s.t = 0.0f; }
    } else if (s.step == 1) {
        s.tClaw[0] = 0.0f;
        if (s.t >= 0.25f) { s.step = 2; s.t = 0.0f; }
    } else if (s.t >= 0.7f) enter(DG_IDLE);
}

static void bite_update(Boss *b, float dt)
{
    // Rears back, then lunges low and snaps: the deadliest hit, so keep out of its face
    float dx, dz;
    to_player(b, &dx, &dz);
    if (s.step == 0) {
        face_player(b, dt, 2.0f);
        s.tNeckReach = 0.75f; s.tNeckPitch = 0.35f; s.tJaw = 0.7f; s.poseRate = 6.0f;
        s.target[0] = character.pos[0]; s.target[1] = s.groundY + 8.0f; s.target[2] = character.pos[2];
        if (s.t >= 0.6f / (s.dark ? 1.2f : 1.0f)) {
            // Reach to land the mouth on the target: the lowered neck spans about 52 per unit of
            // reach in front of the chest, the jaws about 12 more
            float hx = s.target[0] - s.chest[0], hz = s.target[2] - s.chest[2];
            s.range = fminf(fmaxf((sqrtf(hx * hx + hz * hz) - 12.0f) / 52.0f, 0.6f), 1.7f);
            s.step = 1; s.t = 0.0f; sfx_lunge();
        }
    } else if (s.step == 1) {
        s.tNeckReach = s.range; s.tNeckPitch = -0.75f; s.tJaw = 0.9f; s.poseRate = 16.0f;
        memcpy(s.aim, s.target, sizeof(s.aim)); s.tAimK = 1.0f;
        if (s.t >= 0.2f) {
            float mx = character.pos[0] - s.mouth[0], mz = character.pos[2] - s.mouth[2];
            if (mx * mx + mz * mz < 22.0f * 22.0f && player_low()) character_apply_damage(38.0f);
            s.tJaw = 0.0f;
            screen_shake_set_shake_mag(6.0f);
            sfx_boom();
            s.step = 2; s.t = 0.0f;
        }
    } else {
        s.tNeckReach = s.range; s.tNeckPitch = -0.7f; s.poseRate = 4.0f;    // its head lies low: hit it
        if (s.t >= 0.9f) enter(DG_IDLE);
    }
}

static float seg_dist_xz(const float a[3], const float b[3], float px, float pz)
{
    float ux = b[0] - a[0], uz = b[2] - a[2], l2 = ux * ux + uz * uz;
    float t = l2 > 1e-4f ? clamp01(((px - a[0]) * ux + (pz - a[2]) * uz) / l2) : 0.0f;
    float ex = a[0] + ux * t - px, ez = a[2] + uz * t - pz;
    return sqrtf(ex * ex + ez * ez);
}

static void tail_update(Boss *b, float dt)
{
    // Whips the tail across behind it: jump it, roll through, or stand in front
    (void)b;
    if (s.step == 0) {
        if (s.t < dt * 1.5f) {
            float dx, dz;
            to_player(b, &dx, &dz);
            float pr = remainderf(atan2f(dx, dz) - b->rot[1] - T3D_PI, 2.0f * T3D_PI);   // from the tail's line
            s.sweepFrom = pr > 0.0f ? -1.3f : 1.3f;
            s.sweepSpan = -2.9f * (pr > 0.0f ? -1.0f : 1.0f);
        }
        s.tTail = s.sweepFrom; s.poseRate = 5.0f;
        if (s.t >= 0.55f) { s.step = 1; s.t = 0.0f; sfx_swing(); }
    } else if (s.step == 1) {
        s.tTail = s.sweepFrom + s.sweepSpan; s.poseRate = 11.0f;
        if (!s.hitDone && player_low())
            for (int i = 1; i < NTAIL; i++)
                if (seg_dist_xz(s.tl[i], s.tl[i + 1], character.pos[0], character.pos[2]) < 10.0f) {
                    s.hitDone = true;
                    character_apply_damage(20.0f);
                    break;
                }
        if (s.t >= 0.45f) {
            skill_fx_spawn_burst_hostile((float[3]){ s.tl[NTAIL][0], s.groundY, s.tl[NTAIL][2] }, 0.0f);
            screen_shake_set_shake_mag(5.0f);
            s.step = 2; s.t = 0.0f;
        }
    } else {
        s.poseRate = 3.0f;
        if (s.t >= 0.6f) enter(DG_IDLE);
    }
}

// A point on the floor at `dist` from the root, at world yaw `a`
static void floor_point(const Boss *b, float a, float dist, float out[3])
{
    out[0] = b->pos[0] + sinf(a) * dist; out[1] = s.groundY; out[2] = b->pos[2] + cosf(a) * dist;
}

static void breath_update(Boss *b, float dt)
{
    // Breathes in, then sweeps a torrent of fire across the floor in front of it
    float dx, dz, dist = to_player(b, &dx, &dz);
    if (s.step == 0) {
        face_player(b, dt, 1.5f);
        s.tNeckPitch = 0.45f; s.tNeckReach = 0.9f; s.tJaw = 0.5f;
        s.glow = 0.5f + 0.5f * clamp01(s.t / 0.9f);
        if (s.t < dt * 1.5f) {
            float sg = (rand_custom_u32() & 1) ? 1.0f : -1.0f;
            s.sweepFrom = atan2f(dx, dz) - 0.75f * sg; s.sweepSpan = 1.5f * sg;
            s.range = fminf(fmaxf(dist, 50.0f), 130.0f);
        }
        if (s.t >= 0.9f / (s.dark ? 1.2f : 1.0f)) { s.step = 1; s.t = 0.0f; s.emit = 0.0f; sfx_lunge(); }
    } else if (s.step == 1) {
        float a = s.sweepFrom + s.sweepSpan * clamp01(s.t / 1.6f);
        float to[3];
        floor_point(b, a, s.range, to);
        memcpy(s.aim, to, sizeof(s.aim)); s.tAimK = 1.0f;
        s.tJaw = 0.9f; s.tNeckPitch = -0.15f; s.glow = 1.0f;
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
        if (s.t < dt * 1.5f) { screen_shake_set_shake_mag(4.0f); sfx_roar(); }
        if (s.t >= 0.9f / (s.dark ? 1.2f : 1.0f)) { s.step = 1; s.t = 0.0f; sfx_lunge(); }
    } else if (s.step == 1) {
        s.tNeckPitch = -0.2f; s.tNeckReach = 1.2f; s.tCrouch = 6.0f; s.tJaw = 0.3f;
        bool moved = move_by(b, s.F[0] * 240.0f * dt, s.F[2] * 240.0f * dt);
        if (!s.hitDone && player_low() && seg_dist_xz(s.head, s.hips, character.pos[0], character.pos[2]) < HIT_R + 8.0f) {
            s.hitDone = true;
            character_apply_damage(28.0f);
        }
        if (!moved || s.t >= 0.6f) { s.step = 2; s.t = 0.0f; screen_shake_set_shake_mag(6.0f); }
    } else if (s.t >= 0.6f) enter(DG_IDLE);
}

static void fly_update(Boss *b, float dt)
{
    // Takes off, strafes the hall breathing fire down onto the floor, lands with a quake
    s.tWingSpan = 1.0f;
    if (s.step == 0) {
        s.flapRate = 9.0f;
        s.alt = ease(s.alt, 95.0f, 2.2f, dt);
        face_player(b, dt, 2.0f);
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
        if (d > 1e-3f && inside(b->pos[0] + dx / d * st, b->pos[2] + dz / d * st, 40.0f)) { b->pos[0] += dx / d * st; b->pos[2] += dz / d * st; }
        else s.t = 9.0f;     // the wall: land here
        float px = character.pos[0] - b->pos[0], pz = character.pos[2] - b->pos[2];
        if (px * px + pz * pz < 95.0f * 95.0f) {   // over the player: fire rains down
            float to[3];
            mad3(to, s.mouth, s.F, 25.0f);
            to[1] = s.groundY;
            memcpy(s.aim, to, sizeof(s.aim)); s.tAimK = 0.8f; s.tJaw = 0.9f;
            breathe(to, 110.0f, 1.5f, dt);
        }
        if (d < 6.0f || s.t > 3.5f) { s.step = 2; s.t = 0.0f; }
    } else if (s.step == 2) {
        s.flapRate = 6.0f;
        s.alt = ease(s.alt, 0.0f, 3.0f, dt);
        if (s.alt < 1.0f) {
            s.alt = 0.0f;
            float dx, dz;
            if (to_player(b, &dx, &dz) < 40.0f && player_low()) character_apply_damage(20.0f);
            for (int i = 0; i < 4; i++)
                skill_fx_spawn_burst_hostile((float[3]){ s.foot[i][0], s.groundY, s.foot[i][2] }, i * 1.5f);
            screen_shake_set_shake_mag(12.0f);
            sfx_boom();
            s.step = 3; s.t = 0.0f;
        }
    } else if (s.t >= 0.6f) enter(DG_IDLE);
}

static void roar_update(Boss *b, float dt)
{
    // Half health: the dark takes it. A long roar; its crystals turn violet
    s.alt = ease(s.alt, 0.0f, 3.0f, dt);
    s.tNeckPitch = 0.8f; s.tJaw = 1.0f; s.tWingSpan = 1.0f; s.tWingRaise = 0.7f; s.tCrouch = 2.0f;
    if (s.step < 8 && s.t >= 0.3f * s.step) {
        float a = s.step * 0.8f;
        skill_fx_spawn_burst_hostile((float[3]){ b->pos[0] + sinf(a) * 45.0f, s.groundY, b->pos[2] + cosf(a) * 45.0f }, a);
        screen_shake_set_shake_mag(8.0f);
        if (s.step == 0) sfx_roar();
        s.step++;
    }
    if (s.t >= 2.4f) { s.alt = 0.0f; b->mode = BOSS_MODE_COMBAT; s.cdNova = 1.0f; enter(DG_IDLE); }
}

static void nova_update(Boss *b, float dt)
{
    // Gathers the dark (bursts at its feet), then it bursts outward: get far away or roll
    if (s.step == 0) {
        s.tCrouch = 6.0f; s.tWingRaise = 0.9f; s.tWingSpan = 1.0f; s.tNeckPitch = 0.5f; s.tJaw = 0.6f;
        s.glow = clamp01(s.t / 1.7f);
        if ((int)(s.t * 4.0f) != (int)((s.t - dt) * 4.0f)) {
            float a = s.life * 2.3f;
            skill_fx_spawn_burst_hostile((float[3]){ b->pos[0] + sinf(a) * 30.0f, s.groundY, b->pos[2] + cosf(a) * 30.0f }, a);
        }
        if (s.t >= 1.7f) {
            float c[3] = { b->pos[0], s.groundY + 6.0f, b->pos[2] };
            skill_fx_spawn_arc_hostile(c, b->rot[1], 120.0f, 24.0f);
            skill_fx_spawn_arc_hostile(c, b->rot[1] + T3D_PI, 120.0f, 24.0f);
            screen_shake_set_shake_mag(14.0f);
            sfx_boom();
            sfx(SCENE1_SFX_DARK_NOVA, 1.0f);
            s.glow = 0.0f;
            s.step = 1; s.t = 0.0f;
        }
    } else if (s.t >= 1.0f) enter(DG_IDLE);
}

static void orbs_update(Boss *b, float dt)
{
    // Looses dark orbs that hang, then home in on the player: lead them into the floor
    face_player(b, dt, 1.2f);
    s.tNeckPitch = 0.6f; s.tJaw = 0.6f;
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
        sfx(SCENE1_SFX_SPELL_CAST, 0.7f);
        s.step++;
    }
    if (s.t >= 1.6f) enter(DG_IDLE);
}

static void beam_update(Boss *b, float dt)
{
    // Charges, then sweeps a dark beam over the floor that leaves explosions behind;
    // spent, it lies with its head down (the opening)
    float dx, dz, dist = to_player(b, &dx, &dz);
    if (s.step == 0) {
        face_player(b, dt, 2.0f);
        s.tNeckPitch = 0.5f; s.tNeckReach = 0.85f; s.tJaw = 0.8f;
        s.glow = clamp01(s.t / 1.5f);
        if (s.t < dt * 1.5f) {
            float sg = (rand_custom_u32() & 1) ? 1.0f : -1.0f;
            s.sweepFrom = atan2f(dx, dz) - 0.9f * sg; s.sweepSpan = 1.8f * sg;
            s.range = fminf(fmaxf(dist, 50.0f), 150.0f);
        }
        if (s.t >= 1.5f) { s.step = 1; s.t = 0.0f; s.emit = 0.0f; sfx(SCENE1_SFX_DARK_BEAM, 1.0f); }
    } else if (s.step == 1) {
        float a = s.sweepFrom + s.sweepSpan * clamp01(s.t / 2.2f);
        floor_point(b, a, s.range, s.beamP);
        memcpy(s.aim, s.beamP, sizeof(s.aim)); s.tAimK = 1.0f;
        s.tJaw = 1.0f; s.tNeckPitch = 0.1f; s.beamOn = true; s.glow = 1.0f;
        turn_toward(b, a, 0.7f, dt);
        // The beam: from the mouth to the floor
        float pc[3], u[3] = { s.beamP[0] - s.mouth[0], s.beamP[1] - s.mouth[1], s.beamP[2] - s.mouth[2] };
        player_chest(pc);
        float l2 = u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
        float k = l2 > 1e-3f ? clamp01(((pc[0] - s.mouth[0]) * u[0] + (pc[1] - s.mouth[1]) * u[1] + (pc[2] - s.mouth[2]) * u[2]) / l2) : 0.0f;
        float e[3] = { s.mouth[0] + u[0] * k - pc[0], s.mouth[1] + u[1] * k - pc[1], s.mouth[2] + u[2] * k - pc[2] };
        if (!s.hitDone && len3(e) < 9.0f) { character_apply_damage(18.0f); s.hitDone = true; }   // once a sweep
        s.emit -= dt;
        if (s.emit <= 0.0f) { s.emit += 0.2f; blast_spawn(s.beamP); }
        screen_shake_set_shake_mag(2.0f);
        if (s.t >= 2.2f) { s.beamOn = false; s.glow = 0.0f; enter(DG_TIRED); }
    }
}

static void choose(Boss *b, float dist, float rel)
{
#ifdef HLE_DRAGON_FORCE   // test aid: one move over and over (a DgState)
    enter((DgState)HLE_DRAGON_FORCE);
    return;
#endif
    bool front = fabsf(rel) < 0.7f, behind = fabsf(rel) > 2.0f;
    if (behind && dist < 150.0f && s.cdTail <= 0.0f) { s.cdTail = 3.5f; enter(DG_TAIL); return; }
    if (s.dark) {
        if (s.cdNova <= 0.0f && dist < 110.0f) { s.cdNova = 16.0f; enter(DG_NOVA); return; }
        if (s.cdBeam <= 0.0f && front && dist > 60.0f) { s.cdBeam = 18.0f; enter(DG_BEAM); return; }
        if (s.cdOrbs <= 0.0f) { s.cdOrbs = 13.0f; enter(DG_ORBS); return; }
    }
    if (s.cdFly <= 0.0f && dist > 70.0f) { s.cdFly = 22.0f; enter(DG_FLY); return; }
    if (front && dist < 85.0f && s.cdClaw <= 0.0f) { s.cdClaw = 2.5f; enter(DG_CLAW); return; }
    if (front && dist < 100.0f && s.cdBite <= 0.0f) { s.cdBite = 5.0f; enter(DG_BITE); return; }
    if (front && dist > 70.0f && dist < 200.0f && s.cdBreath <= 0.0f) { s.cdBreath = 9.0f; enter(DG_BREATH); return; }
    if (front && dist > 140.0f && s.cdCharge <= 0.0f) { s.cdCharge = 8.0f; enter(DG_CHARGE); return; }
}

void dragon_boss_update(Boss* b, float dt)
{
    if (!s.ready) return;
    if (b->damageFlashTimer > 0.0f) b->damageFlashTimer -= dt;
    s.t += dt;
    s.life += dt;
    float sm = s.dark ? 1.2f : 1.0f;
    s.cdClaw -= dt * sm; s.cdBite -= dt * sm; s.cdTail -= dt * sm; s.cdBreath -= dt * sm; s.cdCharge -= dt * sm;
    s.cdFly -= dt * sm; s.cdNova -= dt * sm; s.cdOrbs -= dt * sm; s.cdBeam -= dt * sm;
    s.fireCd -= dt;

    float dx, dz, dist = to_player(b, &dx, &dz);
    float rel = remainderf(atan2f(dx, dz) - b->rot[1], 2.0f * T3D_PI);

    // The resting pose, which each move overrides: it watches the player
    s.tNeckPitch = 0.0f; s.tNeckYaw = fminf(fmaxf(rel * 0.6f, -0.6f), 0.6f); s.tNeckReach = 1.0f;
    s.tJaw = 0.05f; s.tWingRaise = 0.25f; s.tWingSpan = 0.5f; s.tTail = 0.0f; s.tCrouch = 0.0f;
    s.tClaw[0] = s.tClaw[1] = 0.0f; s.tAimK = 0.0f; s.poseRate = 6.0f; s.flapRate = 0.0f;

    if (b->state == BOSS_STATE_DEAD && s.state != DG_DEAD) { enter(DG_DEAD); clear_hazards(); }
    if (!s.dark && s.state != DG_DEAD && s.state != DG_INTRO && b->health <= b->maxHealth * 0.5f) {
        s.dark = true;
        b->mode = BOSS_MODE_CINEMATIC;
        s.glow = 0.0f; s.beamOn = false;
        enter(DG_ROAR);
    }


#ifdef HLE_TEST_DUMMY
    { extern void hle_open_tick(bool, float); hle_open_tick((s.state == DG_IDLE || s.state == DG_TIRED), dt); }   // test aid: measure the openings
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
            if (s.step == 1 && s.t > 0.4f) { s.step = 2; sfx_roar(); screen_shake_set_shake_mag(5.0f); }
        }
        face_player(b, dt, 2.0f);
        break;

    case DG_IDLE: {
        // A heavy, slow turn: get beside it and behind it
        float d = turn_toward(b, atan2f(dx, dz), 1.1f * sm, dt);
        if (dist > 75.0f && fabsf(d) < 0.8f) move_by(b, s.F[0] * 30.0f * sm * dt, s.F[2] * 30.0f * sm * dt);
        else if (dist < 50.0f && fabsf(d) < 1.0f) move_by(b, -s.F[0] * 18.0f * dt, -s.F[2] * 18.0f * dt);
        if (s.t > 1.8f / sm) choose(b, dist, rel);     // a pause between attacks: the opening
        break;
    }

    case DG_CLAW:   claw_update(b, dt); break;
    case DG_BITE:   bite_update(b, dt); break;
    case DG_TAIL:   tail_update(b, dt); break;
    case DG_BREATH: breath_update(b, dt); break;
    case DG_CHARGE: charge_update(b, dt); break;
    case DG_FLY:    fly_update(b, dt); break;
    case DG_ROAR:   roar_update(b, dt); break;
    case DG_NOVA:   nova_update(b, dt); break;
    case DG_ORBS:   orbs_update(b, dt); break;
    case DG_BEAM:   beam_update(b, dt); break;

    case DG_TIRED:
        // Spent: its head on the floor, the best chance to hit it
        s.tCrouch = 10.0f; s.tNeckPitch = -0.95f; s.tNeckReach = 1.1f; s.tJaw = 0.3f; s.tWingRaise = -0.1f; s.poseRate = 3.0f;
        if (s.t >= 3.5f / sm) enter(DG_IDLE);
        break;

    case DG_DEAD:
        // Falls and lies on the floor
        s.alt = ease(s.alt, 0.0f, 4.0f, dt);
        s.tCrouch = BODY_H - 9.0f; s.tNeckPitch = -1.0f; s.tNeckYaw = 0.4f; s.tWingRaise = -0.25f; s.tWingSpan = 0.7f;
        s.tJaw = 0.4f; s.tTail = 0.3f; s.poseRate = 1.5f;
        break;
    }
    b->rot[0] = b->rot[2] = 0.0f;

    // Ease the pose toward the move's targets
    float pr = s.poseRate;
    s.neckPitch = ease(s.neckPitch, s.tNeckPitch, pr, dt);
    s.neckYaw = ease(s.neckYaw, s.tNeckYaw, pr * 0.6f, dt);
    s.neckReach = ease(s.neckReach, s.tNeckReach, pr, dt);
    s.jaw = ease(s.jaw, s.tJaw, 10.0f, dt);
    s.wingSpan = ease(s.wingSpan, s.tWingSpan, 3.0f, dt);
    s.tail = ease(s.tail, s.tTail, pr, dt);
    s.crouch = ease(s.crouch, s.tCrouch, 5.0f, dt);
    s.aimK = ease(s.aimK, s.tAimK, 8.0f, dt);
    for (int k = 0; k < 2; k++) s.claw[k] = ease(s.claw[k], s.tClaw[k], 9.0f, dt);
    if (s.flapRate > 0.0f) {
        s.flapPh += dt * s.flapRate;
        s.wingRaise = 0.25f + 0.6f * sinf(s.flapPh);
    } else {
        s.wingRaise = ease(s.wingRaise, s.tWingRaise, 3.0f, dt);
    }

    rig(b, dt);
    hazards_update(dt);
}

// ---- Drawing ----

static void put(const ProcMesh *m, const float p[3], const float x[3], const float y[3], const float z[3],
                float sx, float sy, float sz)
{
    if (!m->block || s.mi >= MAT_MAX) return;
    T3DMat4FP *o = &s.mats[s.matRing * MAT_MAX + s.mi++];
    sx /= 8.0f; sy /= 8.0f; sz /= 8.0f;     // proc_mesh stores 1/8 units
    T3DMat4 M = {{
        { x[0] * sx, x[1] * sx, x[2] * sx, 0 }, { y[0] * sy, y[1] * sy, y[2] * sy, 0 },
        { z[0] * sz, z[1] * sz, z[2] * sz, 0 }, { p[0], p[1], p[2], 1 },
    }};
    t3d_mat4_to_fixed(o, &M);
    t3d_matrix_set(o, true);
    rspq_block_run(m->block);
}

// A tube mesh (built along +z, MESH across) from `a` to `b`, `r` thick, `over` times as long
static void put_link(const ProcMesh *m, const float a[3], const float b[3], float r, float over)
{
    float z[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    float l = len3(z);
    if (l < 1e-3f) return;
    for (int i = 0; i < 3; i++) z[i] /= l;
    const float *up = fabsf(z[1]) > 0.95f ? s.F : UP;
    float x[3], y[3];
    cross3(x, up, z); norm3(x);
    cross3(y, z, x);
    put(m, a, x, y, z, r / MESH, r / MESH, l * over / MESH);
}

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

void dragon_boss_draw(Boss* b)
{
    if (!s.ready || !b->visible) return;
    s.matRing = (s.matRing + 1) % MAT_RING;
    s.mi = 0;
    rdpq_sync_pipe();
    rdpq_set_mode_standard();
    rdpq_mode_zbuf(true, true);
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT);

    const ProcMesh *seg = s.dark ? &s.segDark : &s.seg;
    float m1[3], m2[3];
    lerp3(m1, s.chest, s.hips, 1.0f / 3.0f);
    lerp3(m2, s.chest, s.hips, 2.0f / 3.0f);
    put_link(seg, s.chest, m1, 18.0f, 1.2f);
    put_link(seg, m1, m2, 17.5f, 1.2f);
    put_link(seg, m2, s.hips, 16.0f, 1.25f);
    for (int i = 0; i < NNECK; i++) put_link(seg, s.neck[i], s.neck[i + 1], 11.0f - i * 1.2f, 1.25f);
    const ProcMesh *tseg = s.dark ? &s.tsegDark : &s.tseg;
    for (int i = 0; i < NTAIL; i++) put_link(i < 2 ? seg : tseg, s.tl[i], s.tl[i + 1], 14.5f - i * 1.8f, 1.2f);
    {   // the crystal blade at its tip, along the last segment
        float d[3] = { s.tl[NTAIL][0] - s.tl[NTAIL - 1][0], s.tl[NTAIL][1] - s.tl[NTAIL - 1][1], s.tl[NTAIL][2] - s.tl[NTAIL - 1][2] }, e[3];
        norm3(d);
        mad3(e, s.tl[NTAIL], d, 16.0f);
        put_link(s.dark ? &s.tipDark : &s.tip, s.tl[NTAIL], e, MESH, 1.0f);
    }

    for (int k = 0; k < 4; k++) {
        float side = (k & 1) ? 1.0f : -1.0f, bend[3], kn[3];
        // elbows point back and out, knees forward and out
        for (int m = 0; m < 3; m++) bend[m] = s.F[m] * (k < 2 ? -1.0f : 0.8f) + s.R[m] * side * 0.5f + UP[m] * 0.2f;
        knee(s.shoulder[k], s.foot[k], bend, kn);
        put_link(&s.limb, s.shoulder[k], kn, k < 2 ? 7.5f : 8.5f, 1.1f);   // thick upper legs, the hind ones thicker
        put_link(&s.limb, kn, s.foot[k], 5.5f, 1.05f);
        static const float Y[3] = { 0, 1, 0 };
        float fx[3]; cross3(fx, Y, s.F);
        put(&s.mFoot, s.foot[k], fx, Y, s.F, 1.0f, 1.0f, 1.0f);     // the foot, flat, facing forward
    }

    put(s.dark ? &s.mHeadDark : &s.mHead, s.head, s.hx, s.hy, s.hz, 1.0f, 1.0f, 1.0f);
    float hinge[3], jy[3], jz[3], ca = cosf(s.jaw * 0.7f), sa = sinf(s.jaw * 0.7f);
    mad3(hinge, s.head, s.hz, 6.0f);
    mad3(hinge, hinge, s.hy, -2.5f);
    for (int m = 0; m < 3; m++) { jz[m] = s.hz[m] * ca - s.hy[m] * sa; jy[m] = s.hy[m] * ca + s.hz[m] * sa; }
    put(&s.mJaw, hinge, s.hx, jy, jz, 1.0f, 1.0f, 1.0f);

    // Four wings: the great pair at the shoulders, a smaller pair behind
    const ProcMesh *wing = s.dark ? &s.wingDark : &s.wing;
    for (int pair = 0; pair < 2; pair++)
        for (int sd = -1; sd <= 1; sd += 2) {
            float th = s.wingRaise + (pair ? -0.2f : 0.0f), c = cosf(th), sn = sinf(th), x[3], y[3], root[3];
            for (int m = 0; m < 3; m++) { x[m] = sd * s.R[m] * c + UP[m] * sn; y[m] = -sd * s.R[m] * sn + UP[m] * c; }
            mad3(root, s.chest, UP, pair ? 6.0f : 10.0f);
            mad3(root, root, s.R, sd * (pair ? 7.0f : 9.0f));
            mad3(root, root, s.F, pair ? -16.0f : 2.0f);
            float k = pair ? 0.6f : 1.0f;
            put(wing, root, x, y, s.F, k * s.wingSpan, k, k);
        }

    // Glows: fire, dark orbs, the beam, the light in its mouth. Alpha-blended, not
    // additive: the real RDP wraps an overflowing sum (overlapping flames turned rainbow)
    rdpq_sync_pipe();
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
        if (s.orb[i].on) put_glow(&s.mOrb, s.orb[i].p, 4.5f + 0.8f * sinf(s.life * 9.0f + i));
    if (s.beamOn) {
        float mid[3], z[3] = { s.beamP[0] - s.mouth[0], s.beamP[1] - s.mouth[1], s.beamP[2] - s.mouth[2] };
        float l = len3(z), x[3], y[3];
        if (l > 1e-3f) {
            for (int i = 0; i < 3; i++) z[i] /= l;
            cross3(x, UP, z); norm3(x);
            cross3(y, z, x);
            lerp3(mid, s.mouth, s.beamP, 0.5f);
            put(&s.mOrb, mid, x, y, z, 3.0f / 8.0f, 3.0f / 8.0f, l * 0.5f / 8.0f);
            put_glow(&s.mOrb, s.beamP, 9.0f + 2.0f * sinf(s.life * 20.0f));
        }
    }
    if (s.glow > 0.01f) put_glow(s.dark || s.state == DG_BEAM || s.state == DG_NOVA ? &s.mOrb : &s.flame, s.mouth, 2.0f + 5.0f * s.glow);
}
