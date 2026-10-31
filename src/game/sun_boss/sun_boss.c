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
#include "../boss/environmental_mechanics/boss_ground_crush.h"
#include "simple_collision_utility.h"
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
#define FAR_DIST       70.0f           // past the chains' reach: the crosses are for a player who keeps away

typedef enum { SB_INTRO, SB_HOVER, SB_GLARE, SB_CORONA, SB_DIVE, SB_REST, SB_LASH, SB_SWEEP, SB_BREAK, SB_DEAD,
               SB_SPIRAL, SB_EMBER, SB_FLARE, SB_RAM, SB_ECLIPSE } SbState;

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

enum { FACE_UP, FACE_LO, FACE_BRAISE, FACE_BTILT, FACE_PW, FACE_PH, FACE_GX, FACE_GY, FACE_MOPEN, FACE_MCURVE, FACE_SNARL, FACE_N };
// lids open (rad up/down from the eye's level line: 0.16 wide, 0.085 heavy, 0.035 a squint, < 0
// shut), brows raised (rad) and tilted (+: angry, inner ends down), pupil width and height (1:
// round), gaze (rad: toward +x, up), mouth open (rad), its corners (+: a smile), the upper lip
// drawn up off the teeth. At rest: serene, heavy-lidded, the faintest smile
static const float FACE_REST[FACE_N] = { 0.085f, 0.08f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.05f, 0.0f };
// Unshackled, a fixed manic grin: eyes wide, pupils small, teeth showing
static const float FACE_REST2[FACE_N] = { 0.14f, 0.11f, 0.03f, 0.08f, 0.7f, 0.7f, 0.0f, 0.0f, 0.035f, 0.12f, 0.03f };
// Reactions to a blow (the faces it pulls; blended over whatever it was doing)
enum { RX_WINCE, RX_GRIMACE, RX_OH, RX_SCREAM, RX_SHOCK, RX_N };
static const float FACE_RX[RX_N][FACE_N] = {
    { 0.02f, 0.03f, 0.02f, -0.2f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, -0.12f, 0.0f },       // a wince: eyes squeezed, brows pinched, lips pressed
    { 0.05f, 0.06f, -0.02f, 0.25f, 0.8f, 0.8f, 0.05f, -0.02f, 0.01f, -0.12f, 0.03f },  // a grimace: teeth bared, a glance away
    { 0.15f, 0.12f, 0.05f, -0.1f, 0.8f, 0.8f, 0.0f, 0.0f, 0.05f, -0.05f, 0.0f },      // "oh": startled, a small round mouth
    { -0.015f, -0.015f, 0.04f, -0.35f, 1.0f, 1.0f, 0.0f, 0.0f, 0.1f, -0.15f, 0.02f },  // big: screwed shut, screaming
    { 0.17f, 0.13f, 0.07f, -0.15f, 0.35f, 0.35f, 0.0f, 0.0f, 0.09f, -0.08f, 0.0f },   // big: wide-eyed shock, pinprick pupils, agape
};

static struct {
    bool ready;
    ProcMesh ball[2], iris[2], lid[2], lipU[2], lipL[2], blush[2], rays[2][2];  // [unshackled]
    ProcMesh pupil, brow;
    float eyeF[3][3], mouthF[3][3];     // the right eye's and the mouth's frames (columns: their axes)
    sprite_t *texVeins;         // the ball's veins (tools/boss_textures.py)
    ProcMesh weight;
    T3DMat4FP *chainMat;           // MAT_RING * (1 + NCH): the links' origin, then the weights
    T3DVertPacked *linkVB[MAT_RING];
    rspq_block_t *linkBlock[MAT_RING];  // draws its buffer's links: the topology never changes
    int matRing;
    Chain ch[NCH];
    bool chainsReady, weights;
    int lashChain, lashLeft;
    float strikeDir[2], sweepFrom, sweepSpan;
    bool hitDone, chainHit;     // the strike's impact (its bursts) is done; the drawn chain has struck the player
    SbState state;
    float t, life;          // time in state, total time
    int step;
    float groundY, orbit;
    bool unshackled;
    float cdGlare, cdRain, cdCorona, cdDive, cdLash, cdSweep, cdSpiral, cdEmber;
    float aim;              // a volley's locked yaw (the crosses)
    float red;              // the ram's tell, eased: it glows blood red
    int hitSeen, rx;        // the blows it has reacted to, and the face it pulls for the last
    float rxT, rxLen, jolt; // that reaction's time and length (< 0: none), its recoil (drawn only)
    float saccadeT, saccade[2];   // unshackled, its eyes twitch: the next jump, the offset
    float face[FACE_N];     // the face, eased
    float blinkIn, blinkT;  // the next blink, and the one under way (< 0: none)
    float rainT[RAIN_MAX], rainPos[RAIN_MAX][2];
    float diveTarget[2];
    float raysSpin;
    float chainAcc;         // time gathered for the chains' next step (every other frame)
    float downT;            // knocked down by a full stagger: time since it hit the floor (< 0: not)
    bool chainStepped;      // stepped this frame: drawn halfway from the last state
    float cdFlare, cdRam, cdEclipse, hugT;
    bool combo;             // this lash runs on into a sweep of the same chain
    float inhale, dark;     // eased tells: it draws its corona in (the flare), it goes dark (the eclipse)
    float flareAge, flarePos[3], flareYaw;  // the flare's ring (< 0: none), timed as skill_fx draws it
    bool flareHit;
    float ramFrom[2], ramLen;   // the tackle's lane (along s.strikeDir)
    int ramLeft;
    float rainTell;         // the rain's tell: its chains rattle and it throbs twice (s)
    float ecPos[3][6][2];   // the eclipse's marks, by wave
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

static void enter(SbState st) {
#ifdef HLE_LOG_MOVES     // test aid: each move as it starts (the hits after it are its: [hit])
    debugf("[move] %d\n", st);
#endif
#ifdef HLE_SUN_TRACE
    debugf("[sun] %.1f s: state %d -> %d\n", (double)s.life, s.state, st);
#endif
    s.state = st; s.t = 0.0f; s.step = 0; }

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
static void sfx_boom(void) { sfx(SCENE1_SFX_BOSS_SMASH1, 1.0f); }
static void sfx_cast(void) { sfx(SCENE1_SFX_BOSS_SWING2, 1.0f); }
static void sfx_chain(int k) { sfx(k ? SCENE1_SFX_CHAIN2 : SCENE1_SFX_CHAIN1, 1.0f); }

static float speed_mult(void) { return s.unshackled ? 1.35f : 1.0f; }
static float hurt(float d) { return s.unshackled ? d * 1.2f : d; }     // unshackled, it hits 20% harder

// ---- Lifetime ----

void sun_boss_free(void)
{
    if (!s.ready) return;
    gpu_wait();
    for (int h = 0; h < 2; h++) {
        proc_mesh_free(&s.ball[h]); proc_mesh_free(&s.iris[h]); proc_mesh_free(&s.lid[h]);
        proc_mesh_free(&s.lipU[h]); proc_mesh_free(&s.lipL[h]); proc_mesh_free(&s.blush[h]);
        proc_mesh_free(&s.rays[h][0]); proc_mesh_free(&s.rays[h][1]);
    }
    proc_mesh_free(&s.pupil); proc_mesh_free(&s.brow);
    sprite_free(s.texVeins);
    for (int i = 0; i < MAT_RING; i++) { rspq_block_free(s.linkBlock[i]); free_uncached(s.linkVB[i]); }
    proc_mesh_free(&s.weight);
    free_uncached(s.chainMat);
    memset(&s, 0, sizeof(s));
}

void sun_boss_begin(Boss* boss)
{
    if (!s.ready) {
        for (int h = 0; h < 2; h++) {
            proc_mesh_begin(); proc_mesh_uv(192.0f, 96.0f); proc_mesh_sun_face(MESH_R, h); proc_mesh_end(&s.ball[h]);
            proc_mesh_begin(); proc_mesh_face_iris(MESH_R, h); proc_mesh_end(&s.iris[h]);
            proc_mesh_begin(); proc_mesh_face_lid(MESH_R, h); proc_mesh_end(&s.lid[h]);
            proc_mesh_begin(); proc_mesh_face_lip(MESH_R, true, h); proc_mesh_end(&s.lipU[h]);
            proc_mesh_begin(); proc_mesh_face_lip(MESH_R, false, h); proc_mesh_end(&s.lipL[h]);
            proc_mesh_begin(); proc_mesh_face_blush(MESH_R, h); proc_mesh_end(&s.blush[h]);
            for (int w = 0; w < 2; w++) { proc_mesh_begin(); proc_mesh_sun_rays(MESH_R, w, h); proc_mesh_end(&s.rays[h][w]); }
        }
        proc_mesh_begin(); proc_mesh_face_pupil(MESH_R); proc_mesh_end(&s.pupil);
        proc_mesh_begin(); proc_mesh_face_brow(MESH_R); proc_mesh_end(&s.brow);
        float F[3][3];
        proc_mesh_face_frame(SUN_FACE_EYE_YAW, SUN_FACE_EYE_PITCH, F);
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) s.eyeF[r][c] = F[c][r];
        proc_mesh_face_frame(0.0f, SUN_FACE_MOUTH_PITCH, F);
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) s.mouthF[r][c] = F[c][r];
        s.texVeins = sprite_load("rom:/shackled_sun/veins.rgba16.sprite");
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
        s.chainMat = malloc_uncached(sizeof(T3DMat4FP) * MAT_RING * (1 + NCH));
        s.ready = true;
    }
    boss->phaseIndex = SUN_BOSS_PHASE;
    boss->name = "The Shackled Sun";
    boss->health = boss->maxHealth;
    boss->mode = BOSS_MODE_CINEMATIC;       // not damageable until it has come down
    boss->state = BOSS_STATE_INTRO;
    boss->poise = boss->poiseIdleT = boss->staggerT = 0.0f;
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
    s.cdFlare = 3.0f; s.cdRam = 8.0f; s.cdEclipse = 0.0f; s.flareAge = -1.0f; s.hugT = 0.0f; s.inhale = s.dark = 0.0f;
    s.cdGlare = 2.0f; s.cdRain = 6.0f; s.cdCorona = 10.0f; s.cdDive = 14.0f; s.cdLash = 4.0f; s.cdSweep = 9.0f; s.cdSpiral = 7.0f; s.cdEmber = 5.0f;
    memcpy(s.face, FACE_REST, sizeof(s.face));
    s.red = 0.0f;
    s.hitSeen = boss->hitSeq; s.rxT = -1.0f; s.jolt = 0.0f; s.saccadeT = 0.0f; s.saccade[0] = s.saccade[1] = 0.0f;
    s.blinkIn = 2.0f; s.blinkT = -1.0f;
    s.chainsReady = false;
    s.weights = true;
    s.downT = -1.0f;
    for (int i = 0; i < RAIN_MAX; i++) s.rainT[i] = -1.0f;
    s.rainTell = 0.0f;
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

static void eye_front(const Boss *b, float a, float side, float out[3])
{
    out[0] = b->pos[0] + sinf(a) * (SUN_R + 4.0f) + cosf(a) * side;
    out[1] = b->pos[1];
    out[2] = b->pos[2] + cosf(a) * (SUN_R + 4.0f) - sinf(a) * side;
}

// Sunbrand Cross (only at range, FAR_DIST): the eye squints and turns on the player, then
// locks: three crosses swell in front of it, the middle one big, and the lane it will fly down
// flares on the floor; 0.55 s later they fly, straight along the lock. Step wide or roll.
// Unshackled, a second volley, locked afresh.
#define CROSS_LOCK 0.55f
static void glare_update(Boss *b, float dt)
{
    const float WIND = 1.1f / speed_mult(), SPEED = 80.0f, GAP = 0.55f;
    int volleys = s.unshackled ? 2 : 1;
    float lockAt = WIND - CROSS_LOCK + s.step * (CROSS_LOCK + GAP);
    if (s.step < volleys && s.t < lockAt) face_player(b, dt, 3.0f);
    else {      // locked: turn (eased) onto the aim and hold it
        float d = remainderf(s.aim - b->rot[1], 2.0f * T3D_PI);
        b->rot[1] += d * fminf(1.0f, dt * 12.0f);
    }
    // it rises a little to shoot down the lane
    b->pos[1] += (s.groundY + HOVER_H + 6.0f - b->pos[1]) * fminf(1.0f, dt * 3.0f);
    if (s.step < volleys && s.t >= lockAt) {
        float dx, dz, d = to_player(b, &dx, &dz);
        {   // aimed at a random point between the player and where they're heading (0-100% of
            // the way they'd go before the crosses reach them): a strafe isn't a sure dodge
            float vx, vz, lead = (rand() % 1001) / 1000.0f * (CROSS_LOCK + d / SPEED);
            character_get_velocity(&vx, &vz);
            dx += vx * lead; dz += vz * lead;
        }
        s.aim = atan2f(dx, dz);
        float dy = (character.pos[1] + 10.0f) - b->pos[1];
        float h = sqrtf(d * d + dy * dy);
        for (int k = -1; k <= 1; k++) {
            float a = s.aim + k * 0.3f;
            float vy = h > 1e-3f ? dy / h * SPEED : 0.0f, vh = h > 1e-3f ? d / h * SPEED : SPEED;
            float o[3]; eye_front(b, s.aim, k * 9.0f, o);
            skill_fx_next_shape(k ? 1.35f : 1.9f, CROSS_LOCK + fabsf((float)k) * 0.06f);
            skill_fx_spawn_wave_hostile(SKILL_WAVE_X, o, (float[3]){ sinf(a) * vh, vy, cosf(a) * vh },
                                        hurt(k ? 13.0f : 18.0f), 1, 0.0f, fmaxf(2.2f, (h + 40.0f) / SPEED), false);   // far enough to reach
        }
        sfx(SCENE1_SFX_DARK_BEAM, 0.8f);
#ifdef HLE_AUTOPLAY
        debugf("[sun] cross volley %d at %.0f\n", s.step + 1, (double)d);
#endif
        s.step++;
    }
    // loosed: they fly on their own when their charge runs out
    float rel = WIND + (s.step - 1) * (CROSS_LOCK + GAP);
    if (s.step > 0 && s.t >= rel && s.t - dt < rel) { sfx_cast(); screen_shake_set_shake_mag(5.0f); }
    if (s.t > WIND + (volleys - 1) * (CROSS_LOCK + GAP) + 0.7f) enter(SB_HOVER);
}

// Solar Spiral: the sun climbs, its corona spins up, then two arms of slow embers (three unshackled) wheel out of it, wide
// enough to fill the arena: roll through an arm, or weave between them
static void spiral_update(Boss *b, float dt)
{
    const float WIND = 1.0f / speed_mult(), EMIT = 0.15f, TURN = 0.5f;     // embers ~18 apart where the player stands: gaps to slip through
    int arms = s.unshackled ? 3 : 2, shots = 10;
    b->pos[1] += (s.groundY + HOVER_H + 12.0f - b->pos[1]) * fminf(1.0f, dt * 2.5f);
    s.raysSpin = 4.0f + 4.0f * clamp01(s.t / WIND);
    if (s.t < WIND) {
        face_player(b, dt, 2.0f);
        if (s.t - dt <= 0.0f) sfx(SCENE1_SFX_FIRE_SUN, 0.9f);
        s.aim = b->rot[1];
        return;
    }
    if (s.step < shots && s.t >= WIND + s.step * EMIT) {
        float spin = TURN * s.step;
        for (int k = 0; k < arms; k++) {
            float a = s.aim + spin + k * 2.0f * T3D_PI / arms;
            float o[3] = { b->pos[0] + sinf(a) * SUN_R, s.groundY + 9.0f, b->pos[2] + cosf(a) * SUN_R };
            skill_fx_spawn_orb_hostile(o, (float[3]){ sinf(a) * 34.0f, 0.0f, cosf(a) * 34.0f }, hurt(8.0f), 3.2f, 0.0f);
        }
        if (s.step == 0) { sfx_cast(); screen_shake_set_shake_mag(4.0f); }
        s.step++;
    }
    // the sun turns with its arms, easing out
    b->rot[1] += TURN / EMIT * dt * (1.0f - clamp01((s.t - WIND - shots * EMIT) / 0.4f));
    if (s.t > WIND + shots * EMIT + 0.6f) { enter(SB_HOVER); s.t = -1.0f; }     // spent: a longer opening after it
}

// Sun Ember (at range): the sun draws back and an ember swells in front of the eye, then it
// is loosed and hunts the player, turning after it, until it comes close: from there it flies
// straight, so a late sidestep or roll slips it. Unshackled, a second from the other side.
static void ember_update(Boss *b, float dt)
{
    const float CHARGE = 0.85f / speed_mult();
    int n = s.unshackled ? 2 : 1;
    face_player(b, dt, 3.0f);
    float dx, dz, d = to_player(b, &dx, &dz);
    if (d > 1e-3f && s.t < CHARGE)      // draws back a little while it charges
        move_toward(b, b->pos[0] - dx / d * 10.0f, b->pos[2] - dz / d * 10.0f, 14.0f, dt);
    b->pos[1] += (s.groundY + HOVER_H + 4.0f - b->pos[1]) * fminf(1.0f, dt * 3.0f);
    if (s.step < n && s.t >= s.step * 0.5f) {
        float a = atan2f(dx, dz), side = (s.step ? -1.0f : 1.0f) * 10.0f;
        float o[3]; eye_front(b, a, side, o);
        o[1] += 4.0f;
        // it leaves sideways and up, then curls round onto the player
        float v[3] = { sinf(a) * 38.0f + cosf(a) * side * 1.5f, 20.0f, cosf(a) * 38.0f - sinf(a) * side * 1.5f };
        skill_fx_next_shape(1.9f, CHARGE);
        skill_fx_spawn_orb_hostile(o, v, hurt(20.0f), fmaxf(3.6f, d / 40.0f + 1.5f), 2.4f);
        sfx(SCENE1_SFX_FIRE_SUN, 0.8f);
#ifdef HLE_AUTOPLAY
        debugf("[sun] ember %d at %.0f\n", s.step + 1, (double)d);
#endif
        s.step++;
    }
    for (int i = 0; i < n; i++)     // each ember as it's loosed
        if (s.t >= CHARGE + i * 0.5f && s.t - dt < CHARGE + i * 0.5f) sfx_cast();
    if (s.t > CHARGE + (n - 1) * 0.5f + 0.6f) enter(SB_HOVER);
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
        skill_fx_spawn_burst_warn((float[3]){ s.rainPos[i][0], s.groundY, s.rainPos[i][1] }, 0.0f);
    }
}

static void rain_tick(float dt)
{
    for (int i = 0; i < RAIN_MAX; i++) {
        if (s.rainT[i] < 0.0f) continue;
        float was = s.rainT[i];
        s.rainT[i] -= dt;
        if (!(i & 1) && was >= 0.2f && s.rainT[i] < 0.2f) sfx_at(SCENE1_SFX_ROCK_BLAST, 0.7f, s.rainPos[i][0], s.rainPos[i][1]);     // loud 0.2 s on, as it erupts (every other: they come 0.12 s apart)
        if (s.rainT[i] < 0.0f) {
            float p[3] = { s.rainPos[i][0], s.groundY, s.rainPos[i][1] };
            skill_fx_spawn_wave_hostile(SKILL_WAVE_BLADE, p, (float[3]){ 0, 0, 0 }, hurt(14.0f), 1, 0.0f, 0.9f, false);
            if (i == 0) screen_shake_set_shake_mag(4.0f);
        }
    }
}

static void corona_update(Boss *b, float dt)
{
    // A long wind-up (the rays spin up, the eye screwed shut), then a ring of fire all around: roll
    // through it or be far away
    if (s.step == 0) {
        s.raysSpin = 6.0f;
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
                           skill_fx_spawn_burst_warn((float[3]){ s.diveTarget[0], s.groundY, s.diveTarget[1] }, 0.0f); }
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
            boss_ground_crush_spawn(b->pos[0], s.groundY + 0.25f, b->pos[2]);
            screen_shake_set_shake_mag(14.0f);
            sfx_boom();
            enter(SB_REST);
        }
    }
}

// Solar Flare (the punish for standing under it): the sun sinks and draws its corona in, the
// eye shrinking, while sparks run round the floor at the flare's reach; then a ring of fire
// bursts out from beneath it, over the floor. Roll through the ring's edge or be outside it
#define FLARE_R    40.0f
#define FLARE_BAND 8.0f        // the ring's bright outer edge (the ribbon is 16 wide at most, fading inward)
#define FLARE_LIVE 0.4f        // it hurts while it grows (0.35 s) and is bright; it fades out by 0.55
static void flare_update(Boss *b, float dt)
{
    const float WIND = 0.8f / speed_mult();
    if (s.step == 0) {
        face_player(b, dt, 2.0f);
        b->pos[1] += (s.groundY + HOVER_H - 4.0f - b->pos[1]) * fminf(1.0f, dt * 4.0f);
        s.raysSpin = 3.0f + 7.0f * clamp01(s.t / WIND);
        int k = (int)floorf(s.t * 8.0f / WIND);
        if (k != (int)floorf((s.t - dt) * 8.0f / WIND) && k < 8) {     // round the reach, in turn
            float a = b->rot[1] + k * (T3D_PI / 4.0f);
            skill_fx_spawn_burst_warn((float[3]){ b->pos[0] + sinf(a) * FLARE_R, s.groundY, b->pos[2] + cosf(a) * FLARE_R }, a);
        }
        if (s.t <= dt) sfx(SCENE1_SFX_FIRE_SUN, 1.0f);
        if (s.t >= WIND) {
            s.flarePos[0] = b->pos[0]; s.flarePos[1] = s.groundY + 4.0f; s.flarePos[2] = b->pos[2];
            s.flareYaw = b->rot[1]; s.flareAge = 0.0f; s.flareHit = false;
            // the ring is drawn (skill_fx), its hit is flare_tick's: the drawn band only
            skill_fx_next_arc_half(T3D_PI);
            skill_fx_spawn_arc_hostile(s.flarePos, s.flareYaw, FLARE_R, 0.0f);
            skill_fx_spawn_pillar_hostile((float[3]){ b->pos[0], s.groundY, b->pos[2] });
            screen_shake_set_shake_mag(9.0f);
            sfx_boom();
            s.step = 1; s.t = 0.0f;
        }
    } else {
        b->pos[1] += (s.groundY + HOVER_H - b->pos[1]) * fminf(1.0f, dt * 3.0f);
        if (s.t > 0.7f) enter(SB_HOVER);
    }
}

// The ring, as draw_arc has it: its outer edge grows from a fifth to FLARE_R in 0.35 s; the
// ribbon is 16 * taper wide (thinner round the back), bright at its outer edge
static void flare_tick(float dt)
{
    if (s.flareAge < 0.0f) return;
    s.flareAge += dt;
    float R = FLARE_R * (0.2f + 0.8f * fminf(1.0f, s.flareAge / 0.35f));
    float dx = character.pos[0] - s.flarePos[0], dz = character.pos[2] - s.flarePos[2], d = sqrtf(dx * dx + dz * dz);
    // (an arc faces (-sin yaw, cos yaw): skill_fx's convention, mirrored from the boss's)
    float th = d > 1e-3f ? acosf(fmaxf(-1.0f, fminf(1.0f, (-dx * sinf(s.flareYaw) + dz * cosf(s.flareYaw)) / d))) : 0.0f;
    float band = fminf(FLARE_BAND, 16.0f * (0.35f + 0.65f * cosf(th * 0.5f))), rr = character.capsuleCollider.radius;
    bool low = character.pos[1] <= s.flarePos[1] + 2.0f;    // as the arcs: one in the air clears it
    bool hit = !s.flareHit && s.flareAge <= FLARE_LIVE && low && d - rr <= R && d + rr >= R - band;
#ifdef HLE_LOG_HITVOL
    if (hit || (!s.flareHit && s.flareAge <= FLARE_LIVE && s.flareAge + dt > FLARE_LIVE))
        debugf("[hitvol] flare age %.2f: ring drawn %.1f-%.1f, the player's body %.1f-%.1f from its centre, %.0f up: %s\n", (double)s.flareAge,
               (double)(R - band), (double)R, (double)(d - rr), (double)(d + rr), (double)(character.pos[1] - s.groundY), hit ? "HIT" : "missed");
#endif
    if (hit) { s.flareHit = true; character_apply_damage(hurt(16.0f)); }
    if (s.flareAge > 0.55f) s.flareAge = -1.0f;
}

// Solar Ram: it pulls back and rears, scowling, turning on the player; dips and locks, then hurls itself down the lane and skids to a stop. Unshackled, it turns and
// rams again. The eye itself is what hits (its drawn radius), swept along the frame's path
static void ram_start(Boss *b, int count)
{
    s.ramLeft = count;
    enter(SB_RAM);
    (void)b;
#ifdef HLE_AUTOPLAY
    debugf("[sun] ram x%d\n", count);
#endif
}

static void ram_update(Boss *b, float dt)
{
    // (the second ram, unshackled, was 0.6: 0.44 s to read a 24 hit; now 0.7 s)
    const float sm = speed_mult(), WIND = (s.ramLeft > 1 || !s.unshackled ? 0.9f : 0.95f) / sm, LOCK = 0.3f, SPEED = 125.0f;
    float dx, dz, d = to_player(b, &dx, &dz);
    if (s.step == 0) {
        if (s.t - dt <= 0.0f) { s.ramFrom[0] = b->pos[0]; s.ramFrom[1] = b->pos[2]; }   // where the pull-back starts
        if (s.t < WIND - LOCK) {
            // pulls back for leverage: a quick 26 away from the player, easing into a coiled hold
            // (its flares and novas never back off first, so the tackle reads apart from them)
            face_player(b, dt, 4.0f);
            float k = clamp01(s.t / ((WIND - LOCK) * 0.7f)), back = 26.0f * (1.0f - (1.0f - k) * (1.0f - k));
            if (d > 1e-3f) move_toward(b, s.ramFrom[0] - dx / d * back, s.ramFrom[1] - dz / d * back, 200.0f, dt);
        } else {
            if (s.t - dt < WIND - LOCK) {   // locks its lane, out past the player
                s.aim = atan2f(dx, dz);
                s.strikeDir[0] = sinf(s.aim); s.strikeDir[1] = cosf(s.aim);
                s.ramLen = fminf(130.0f, fmaxf(50.0f, d + 30.0f));
                sfx_chain(0);
            }
            b->rot[1] += remainderf(s.aim - b->rot[1], 2.0f * T3D_PI) * fminf(1.0f, dt * 12.0f);
        }
        float hy = s.t < WIND - LOCK ? HOVER_H + 4.0f : HOVER_H - 6.0f;    // rears as it pulls back, then dips: the eye at the player's height
        b->pos[1] += (s.groundY + hy - b->pos[1]) * fminf(1.0f, dt * 4.0f);
        if (s.t >= WIND) { s.step = 1; s.t = 0.0f; s.hitDone = false; s.ramFrom[0] = b->pos[0]; s.ramFrom[1] = b->pos[2];
                           sfx_cast(); screen_shake_set_shake_mag(5.0f);
                           // charging, its body shoves the player where the eye meets it (not 4 out
                           // from it, the sword's margin): no shove without the hit
                           b->capsuleCollider.radius = SUN_R; }
    } else if (s.step == 1) {
        // out of the gate at full speed, easing to a stop at the lane's end
        float T = s.ramLen / SPEED * 1.4f, k = clamp01(s.t / T), u = s.ramLen * sinf(k * T3D_PI * 0.5f);
        float was[3] = { b->pos[0], b->pos[1], b->pos[2] };
        float tx = s.ramFrom[0] + s.strikeDir[0] * u, tz = s.ramFrom[1] + s.strikeDir[1] * u;
        move_toward(b, tx, tz, 1e4f, dt);
        bool blocked = fabsf(b->pos[0] - tx) + fabsf(b->pos[2] - tz) > 0.5f;    // a wall: it stops there
        float pa[3], pb[3];
        for (int m = 0; m < 3; m++) { pa[m] = character.pos[m] + character.capsuleCollider.localCapA.v[m]; pb[m] = character.pos[m] + character.capsuleCollider.localCapB.v[m]; }
        bool hit = !s.hitDone && scu_capsule_vs_capsule_f(was, b->pos, SUN_R, pa, pb, character.capsuleCollider.radius);
#ifdef HLE_LOG_HITVOL
        {   // the eye's centre against the body's axis (closest approach over the frame's path)
            float best = 1e9f;
            for (int j = 0; j <= 8; j++) {
                float c[3], q[3], ab[3], l2 = 0.0f, tt = 0.0f;
                for (int m = 0; m < 3; m++) { c[m] = was[m] + (b->pos[m] - was[m]) * j / 8.0f; ab[m] = pb[m] - pa[m]; l2 += ab[m] * ab[m]; }
                for (int m = 0; m < 3; m++) tt += (c[m] - pa[m]) * ab[m];
                tt = l2 > 0.0f ? clamp01(tt / l2) : 0.0f;
                float e2 = 0.0f; for (int m = 0; m < 3; m++) { q[m] = pa[m] + ab[m] * tt - c[m]; e2 += q[m] * q[m]; }
                best = fminf(best, sqrtf(e2));
            }
            float drawn = SUN_R * (1.0f + 0.04f * sinf(s.life * 5.0f));
            if (!s.hitDone && best < SUN_R + character.capsuleCollider.radius + 8.0f)
                debugf("[hitvol] ram t %.2f: the eye's centre %.1f from the body's axis (eye drawn %.1f + body %.0f = %.1f): %s\n", (double)s.t, (double)best,
                       (double)drawn, (double)character.capsuleCollider.radius, (double)(drawn + character.capsuleCollider.radius), hit ? "HIT" : "miss");
        }
#endif
        if (hit) { s.hitDone = true; character_apply_damage(hurt(20.0f)); }
        if (k >= 1.0f || blocked) { s.step = 2; s.t = 0.0f; b->capsuleCollider.radius = SUN_R + 4.0f;
                                    screen_shake_set_shake_mag(blocked ? 10.0f : 4.0f); if (blocked) sfx_boom(); }
    } else {
        face_player(b, dt, 1.5f);
        b->pos[1] += (s.groundY + HOVER_H - b->pos[1]) * fminf(1.0f, dt * 3.0f);
        if (s.t > (s.ramLeft > 1 ? 0.35f : 0.6f)) {
            if (--s.ramLeft > 0) { s.step = 0; s.t = 0.0f; }
            else enter(SB_HOVER);
        }
    }
}

// Solar Eclipse (unshackled, its signature): it climbs high and goes dark, only its corona
// burning, then looses flares that fall on marked spots: a ring close round it, a wider ring,
// then on and around the player. It sinks to the floor after, spent (a long opening)
#define EC_RISE  1.3f
#define EC_GAP   0.7f
#define EC_FLY   0.9f
static const int EC_N[3] = { 4, 6, 4 };
static const float EC_RING[3] = { 26.0f, 54.0f, 26.0f };    // round the sun, round the sun, round the player
static void eclipse_update(Boss *b, float dt)
{
    face_player(b, dt, 1.5f);
    float spent = EC_RISE + 2 * EC_GAP + EC_FLY + 0.3f;
    float hy = s.t < spent ? s.groundY + 70.0f : s.groundY + REST_H;
    b->pos[1] += (hy - b->pos[1]) * fminf(1.0f, dt * (s.t < spent ? 2.5f : 4.0f));
    s.raysSpin = fmaxf(s.raysSpin, s.t < spent ? 7.0f : 1.0f);
    if (s.t <= dt) { sfx(SCENE1_SFX_DARK_BEAM, 1.0f); screen_shake_set_shake_mag(4.0f); }
    if (s.step < 3 && s.t >= EC_RISE + s.step * EC_GAP) {
        int w = s.step, n = EC_N[w];
        float cx = w == 2 ? character.pos[0] : b->pos[0], cz = w == 2 ? character.pos[2] : b->pos[2];
        float a0 = (rand_custom_u32() % 628) / 100.0f;
        for (int i = 0; i < n; i++) {
            float a = a0 + i * 2.0f * T3D_PI / n, r = (w == 2 && i == 0) ? 0.0f : EC_RING[w];
            if (w == 2 && i > 0) a = a0 + (i - 1) * 2.0f * T3D_PI / (n - 1);
            float m[3] = { cx + sinf(a) * r, s.groundY + 1.0f, cz + cosf(a) * r };
            if (!scene_bounds_check_room(m[0], s.groundY, m[2])) { m[0] = cx; m[2] = cz; }
            s.ecPos[w][i][0] = m[0]; s.ecPos[w][i][1] = m[2];
            skill_fx_spawn_burst_warn(m, a);
            // falls from the sun's underside onto the mark, landing as the mark's last spark fades
            float o[3] = { b->pos[0], b->pos[1] - SUN_R, b->pos[2] };
            skill_fx_next_shape(1.5f, 0.0f);
            skill_fx_spawn_orb_hostile(o, (float[3]){ (m[0] - o[0]) / EC_FLY, (m[1] - o[1]) / EC_FLY, (m[2] - o[2]) / EC_FLY }, hurt(15.0f), EC_FLY, 0.0f);
        }
        sfx_cast();
#ifdef HLE_AUTOPLAY
        debugf("[sun] eclipse wave %d: %d flares\n", w + 1, n);
#endif
        s.step++;
    }
    for (int w = 0; w < s.step; w++) {      // the marks spark again halfway down
        float tl = EC_RISE + w * EC_GAP + EC_FLY;
        if (s.t >= tl && s.t - dt < tl) {   // the flares landing: heard from the nearest
            int k = 0;
            for (int i = 1; i < EC_N[w]; i++) {
                float ax = character.pos[0] - s.ecPos[w][i][0], az = character.pos[2] - s.ecPos[w][i][1];
                float bx = character.pos[0] - s.ecPos[w][k][0], bz = character.pos[2] - s.ecPos[w][k][1];
                if (ax * ax + az * az < bx * bx + bz * bz) k = i;
            }
            sfx_at(SCENE1_SFX_BOSS_SMASH1, 1.0f, s.ecPos[w][k][0], s.ecPos[w][k][1]);     // (sfx_boom's)
        }
        float tw = EC_RISE + w * EC_GAP + EC_FLY * 0.5f;
        if (s.t >= tw && s.t - dt < tw)
            for (int i = 0; i < EC_N[w]; i++)
                skill_fx_spawn_burst_warn((float[3]){ s.ecPos[w][i][0], s.groundY, s.ecPos[w][i][1] }, (float)i);
    }
    if (s.t > spent + 0.5f) {       // down on the floor, dim: open
        screen_shake_set_shake_mag(7.0f); sfx_boom();
        enter(SB_REST);
        s.t = 3.0f / speed_mult() - 2.6f;
    }
}

bool sun_boss_stagger(Boss* b)
{
    // Knocked out of the air onto the floor, dimmed (as after a dive); not mid-dive or breaking free
    if (!s.ready || s.state == SB_INTRO || s.state == SB_DIVE || s.state == SB_REST || s.state == SB_BREAK || s.state == SB_DEAD) return false;
    for (int k = 0; k < NCH; k++) s.ch[k].kin = s.ch[k].hot = false;
    enter(SB_REST);
    b->staggerT = boss_stagger_s();
    s.t = 3.0f / speed_mult() - b->staggerT;     // on the floor for the stagger
    s.downT = 0.0f;         // it falls from where it was: a bounce and a rocking on the floor
    return true;
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

// ---- The chains' hits: against the chain as drawn (a link is 3.4 across its half), not a
// line out of the sun's middle: a chain hangs from a root on the sun's side, ~11 off that line
#define LINK_HALF_W 3.4f

static void chain_drawn(const Boss *b, const Chain *c, float P[NN][3])     // as chains_draw has it
{
    chain_root(b, c, P[0]);
    const float f = s.chainStepped ? 0.5f : 1.0f;
    for (int i = 1; i < NN; i++)
        for (int m = 0; m < 3; m++) P[i][m] = c->q[i][m] + (c->p[i][m] - c->q[i][m]) * f;
}

static float seg_dist_xz(const float a[3], const float b[3], float px, float pz)
{
    float ex = b[0] - a[0], ez = b[2] - a[2], l2 = ex * ex + ez * ez;
    float t = l2 > 1e-6f ? ((px - a[0]) * ex + (pz - a[2]) * ez) / l2 : 0.0f;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    float dx = px - a[0] - ex * t, dz = pz - a[2] - ez * t;
    return sqrtf(dx * dx + dz * dz);
}

static bool in_tri_xz(const float a[3], const float b[3], const float c[3], float px, float pz)
{
    float d1 = (px - b[0]) * (a[2] - b[2]) - (a[0] - b[0]) * (pz - b[2]);
    float d2 = (px - c[0]) * (b[2] - c[2]) - (b[0] - c[0]) * (pz - c[2]);
    float d3 = (px - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (pz - a[2]);
    return !(((d1 < 0) || (d2 < 0) || (d3 < 0)) && ((d1 > 0) || (d2 > 0) || (d3 > 0)));
}

/** The player's body within reach of the chain where it is now, or swept over since `was` (a
 *  fast sweep moves its tip ~10 a frame), seen from above: the links below `maxH` over the floor */
static bool chain_hits_player(const float now[NN][3], const float was[NN][3], float maxH)
{
    float px = character.pos[0], pz = character.pos[2], R = LINK_HALF_W + character.capsuleCollider.radius;
    for (int i = 0; i < NN - 1; i++) {
        if (fminf(now[i][1], now[i + 1][1]) - s.groundY > maxH) continue;
        if (seg_dist_xz(now[i], now[i + 1], px, pz) < R) return true;
        if (was && (in_tri_xz(was[i], was[i + 1], now[i + 1], px, pz) || in_tri_xz(was[i], now[i + 1], now[i], px, pz))) return true;
    }
    return was && now[NN - 1][1] - s.groundY <= maxH && seg_dist_xz(was[NN - 1], now[NN - 1], px, pz) < R;     // grazed by the tip on its way
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

// Shackle Lash: a chain glows and the eye glances up at it, as it rears up behind the sun, then slams down along a line
// at the player (step aside). Unshackled, two in a row.
static void lash_start(Boss *b, int count)
{
    float dx, dz;
    to_player(b, &dx, &dz);
    s.lashChain = nearest_chain(b, dx, dz);
    s.lashLeft = count;
    s.hitDone = s.chainHit = false;
    s.ch[s.lashChain].hot = true;
    // Shackle Flail: the (last) lash's chain, lying where it struck, is dragged round in a sweep
    s.combo = (int)(rand_custom_u32() % 100) < (s.unshackled ? 50 : 35);
#if defined(HLE_SUN_FORCE)
    if (HLE_SUN_FORCE == 11) s.combo = true;
#endif
    enter(SB_LASH);
#ifdef HLE_AUTOPLAY
    debugf("[sun] lash x%d%s\n", count, s.combo ? " into a sweep" : "");
#endif
}

static void lash_update(Boss *b, float dt)
{
    Chain *c = &s.ch[s.lashChain];
    float wind = (s.unshackled ? 0.75f : 0.95f) * (s.lashLeft == 1 && s.unshackled ? 0.7f : 1.0f);
    face_player(b, dt, 2.0f);
    float root[3];
    chain_root(b, c, root);
#ifdef HLE_LOG_HITVOL     // test aid: the drawn chain against its pose, from the strike on
    if (s.step >= 1) { float P[NN][3]; chain_drawn(b, c, P);
        debugf("[hitvol] lash step %d t %.2f: node 4 %.0f off its pose at height %.0f, tip %.0f off at height %.0f\n", s.step, (double)s.t,
               (double)sqrtf((P[4][0]-c->kt[4][0])*(P[4][0]-c->kt[4][0]) + (P[4][2]-c->kt[4][2])*(P[4][2]-c->kt[4][2])), (double)(P[4][1] - s.groundY),
               (double)sqrtf((P[NN-1][0]-c->kt[NN-1][0])*(P[NN-1][0]-c->kt[NN-1][0]) + (P[NN-1][2]-c->kt[NN-1][2])*(P[NN-1][2]-c->kt[NN-1][2])), (double)(P[NN-1][1] - s.groundY)); }
#endif
    // The strike: the drawn chain hurts wherever it comes down below head height (it whips: its
    // pose is reached a few frames on, the tip ~19 short of it), from the swing over the top
    // until it lies on the floor
    static float was[NN][3];
    if (s.step >= 1 && !s.chainHit && (s.step == 1 || s.t < 0.3f)) {
        float now[NN][3];
        chain_drawn(b, c, now);
        if (s.step == 1 && s.t <= dt) memcpy(was, now, sizeof(was));
        if (character.pos[1] - s.groundY < 8.0f && chain_hits_player((const float (*)[3])now, (const float (*)[3])was, 20.0f)) {
            s.chainHit = true;
            character_apply_damage(hurt(16.0f));
        }
        memcpy(was, now, sizeof(was));
#ifdef HLE_LOG_HITVOL
        if (s.chainHit || (s.step == 2 && s.t + dt >= 0.3f)) {
            float px = character.pos[0] - b->pos[0], pz = character.pos[2] - b->pos[2];
            float along = px * s.strikeDir[0] + pz * s.strikeDir[1], side = fabsf(px * s.strikeDir[1] - pz * s.strikeDir[0]);
            debugf("[hitvol] lash: player %.0f along the sun's line, %.0f beside it (the old band %s), the drawn chain %s at step %d t %.2f\n", (double)along, (double)side,
                   along > 0.0f && along < CHAIN_LEN + SUN_R + 4.0f && side < 9.0f ? "hit" : "missed", s.chainHit ? "hits" : "missed", s.step, (double)s.t);
        }
#endif
    }
    if (s.step == 0) {          // wind-up: track the player, the chain rises behind the sun
        // aimed from its root (on the sun's side), so the chain itself comes down on the player
        float dx = character.pos[0] - root[0], dz = character.pos[2] - root[2], d = sqrtf(dx * dx + dz * dz);
        if (s.t < wind - 0.2f && d > 1e-3f) { s.strikeDir[0] = dx / d; s.strikeDir[1] = dz / d; }
        float k = clamp01(s.t / wind);
        c->kin = true; c->kk = 0.18f;
        chain_pose(b, c, s.strikeDir[0], s.strikeDir[1], T3D_PI * (1.0f + 0.82f * k), false);
        if (s.t >= wind) { s.step = 1; s.t = 0.0f; sfx_cast(); sfx_chain(0); }
    } else if (s.step == 1) {   // over the top and down onto the floor
        float k = clamp01(s.t / 0.2f);
        c->kk = 0.6f;
        chain_pose(b, c, s.strikeDir[0], s.strikeDir[1], T3D_PI * (1.82f + 0.72f * k * k), true);
        if (k >= 1.0f && !s.hitDone) {
            s.hitDone = true;
#ifdef HLE_SUN_FORCE
            { float px = character.pos[0] - b->pos[0], pz = character.pos[2] - b->pos[2];
              float along = px * s.strikeDir[0] + pz * s.strikeDir[1], side = fabsf(px * s.strikeDir[1] - pz * s.strikeDir[0]);
              float r[3]; chain_root(b, c, r); const float *tp = c->p[NN - 1];
              debugf("[sun] strike along %.0f side %.0f | tip from root %.0f,%.0f,%.0f kt %.0f,%.0f,%.0f\n", (double)along, (double)side,
                     (double)(tp[0]-r[0]), (double)(tp[1]-r[1]), (double)(tp[2]-r[2]),
                     (double)(c->kt[NN-1][0]-r[0]), (double)(c->kt[NN-1][1]-r[1]), (double)(c->kt[NN-1][2]-r[2])); }
#endif
            boss_ground_crush_spawn(root[0] + s.strikeDir[0] * CHAIN_LEN * 0.6f, s.groundY + 0.25f,
                                    root[2] + s.strikeDir[1] * CHAIN_LEN * 0.6f);
            screen_shake_set_shake_mag(10.0f);
            sfx_boom();
            sfx_chain(1);
            s.step = 2; s.t = 0.0f;
        }
    } else {                    // let it lie, then let go
        bool flail = s.combo && s.lashLeft == 1;    // held on the floor, hot, and dragged round
        if (s.t > 0.15f && !flail) c->kin = false;
        if (s.t > 0.4f && !flail) c->hot = false;
        if (flail && s.t > 0.25f) { s.sweepSpan = 0.0f; enter(SB_SWEEP); return; }
        if (s.t > (s.lashLeft > 1 ? 0.35f : 0.7f)) {
            if (--s.lashLeft > 0) {
                float dx, dz;
                to_player(b, &dx, &dz);
                int prev = s.lashChain;
                s.lashChain = nearest_chain(b, dx, dz);
                if (s.lashChain == prev) s.lashChain = (prev + 1) % NCH;
                s.ch[prev].hot = s.ch[prev].kin = false;    // it went on to the next before this one cooled (stayed orange)
                s.ch[s.lashChain].hot = true; s.hitDone = s.chainHit = false;
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
    // a flail (after a lash) drags the chain from where it lies: a shorter wind-up, its own tell
    // (the chain kept glowing on the floor, a rattle, the sun twisting back against the swing, its eye squinting down along it)
    const float WIND = s.combo ? 0.6f / speed_mult() : 1.0f, SWING = 0.7f;
    float dx, dz;
    to_player(b, &dx, &dz);
    if (s.step == 0) {
        if (s.t == 0.0f || s.t < dt * 1.5f) {
            float pa = atan2f(dx, dz);
            if (s.combo) {      // from the lash's line, round through the player's side
                float r[3]; chain_root(b, c, r);
                s.sweepFrom = atan2f(s.strikeDir[0], s.strikeDir[1]);
                float side = remainderf(atan2f(character.pos[0] - r[0], character.pos[2] - r[2]) - s.sweepFrom, 2.0f * T3D_PI);
                s.sweepSpan = side >= 0.0f ? 3.4f : -3.4f;
                sfx_chain(0);
            } else {
                s.sweepSpan = (rand_custom_u32() & 1) ? 3.6f : -3.6f;
                s.sweepFrom = pa - s.sweepSpan * 0.5f;
            }
            s.aim = b->rot[1];
        }
        // twists back against the swing, eased (the chains' roots go with it)
        float k = clamp01(s.t / WIND);
        b->rot[1] = s.aim - (s.sweepSpan > 0.0f ? 1.0f : -1.0f) * 0.3f * k * k * (3.0f - 2.0f * k);
        c->kin = true; c->kk = 0.15f; c->hot = true;
        chain_pose(b, c, sinf(s.sweepFrom), cosf(s.sweepFrom), T3D_PI * 0.5f, true);
        if (s.t >= WIND) { s.step = 1; s.t = 0.0f; s.hitDone = false; sfx_cast(); sfx_chain(1); }
    } else if (s.step == 1) {
        float k = clamp01(s.t / SWING), a0 = s.sweepFrom + s.sweepSpan * clamp01((s.t - dt) / SWING), a = s.sweepFrom + s.sweepSpan * k;
        c->kk = 0.5f;
        b->rot[1] = s.aim + (s.sweepSpan > 0.0f ? 1.0f : -1.0f) * (-0.3f + 0.9f * k * (2.0f - k));   // and turns into it
        chain_pose(b, c, sinf(a), cosf(a), T3D_PI * 0.5f, true);
        // the chain as drawn swept over the player since the last frame (it trails its pose, the
        // tip most: hit by what is seen, not by the angle round the sun's middle out to 73)
        static float was[NN][3];
        float now[NN][3];
        chain_drawn(b, c, now);
        if (s.t <= dt) memcpy(was, now, sizeof(was));
        bool hit = !s.hitDone && character.pos[1] - s.groundY < 6.0f && chain_hits_player((const float (*)[3])now, (const float (*)[3])was, 1e9f);
#ifdef HLE_LOG_HITVOL
        { float pa = atan2f(dx, dz), dist = sqrtf(dx * dx + dz * dz);
          float r0 = remainderf(pa - a0, 2.0f * T3D_PI), r1 = remainderf(pa - a, 2.0f * T3D_PI);
          bool old = (r0 > 0) != (r1 > 0) && fabsf(r0) < 1.0f && dist < SUN_R + CHAIN_LEN + 4.0f && character.pos[1] - s.groundY < 6.0f;
          if ((old || hit) && !s.hitDone) debugf("[hitvol] sweep: player %.0f from the sun, the old angle test %s, the chain %s\n", (double)dist, old ? "hit" : "missed", hit ? "hits" : "misses"); }
#else
        (void)a0;
#endif
        memcpy(was, now, sizeof(was));
        if (hit) {
            s.hitDone = true;
            character_apply_damage(hurt(14.0f));
        }
        if (k >= 1.0f) { s.step = 2; s.t = 0.0f; screen_shake_set_shake_mag(6.0f); }
    } else {
        if (s.t > 0.1f) c->kin = false;
        if (s.t > 0.3f) c->hot = false;
        if (s.t > 0.6f) { s.combo = false; enter(SB_HOVER); }
    }
}

static void choose(Boss *b, float dist)
{
#ifdef HLE_SUN_FORCE   // test aid: 1 = lash, 2 = sweep, 3 = dive, 4 = corona, 5 = crosses, 6 = spiral, 7 = ember, back to back;
                       // 10 = flare, 11 = lash into a sweep, 12 = ram, 13 = eclipse (unshackled: HLE_DUMMY_HALF)
    if (HLE_SUN_FORCE == 10) {  // from 8 to 56 away in turn: inside, on and outside the ring
        static int n; float dx, dz, d = to_player(b, &dx, &dz), want = 8.0f + (n++ % 7) * 8.0f;
        if (d > 1e-3f) { b->pos[0] = character.pos[0] - dx / d * want; b->pos[2] = character.pos[2] - dz / d * want; }
        debugf("[sun] flare at %.0f\n", (double)want); enter(SB_FLARE); return;
    }
    if (HLE_SUN_FORCE == 12) { ram_start(b, s.unshackled ? 2 : 1); return; }
    if (HLE_SUN_FORCE == 13) { enter(SB_ECLIPSE); debugf("[sun] eclipse\n"); return; }
    if (HLE_SUN_FORCE == 14) { if (s.cdRain > 0.0f) return; s.cdRain = 3.0f; rain_spawn(); sfx_cast(); s.t = -0.3f; s.rainTell = 0.9f; sfx_chain(0); sfx_chain(1); debugf("[sun] rain\n"); return; }   // 14 = the rain alone (no tell of its own before 2026-10-28)
    if (HLE_SUN_FORCE == 11) { lash_start(b, s.unshackled ? 2 : 1); return; }
    if (HLE_SUN_FORCE == 5 || HLE_SUN_FORCE == 7) { if (dist < FAR_DIST + 5.0f) return; }    // the ranged ones: it drifts out to range first
    else if (HLE_SUN_FORCE != 9) { float dx, dz, d = to_player(b, &dx, &dz);   // within reach of the player
      if (d > 50.0f) { b->pos[0] = character.pos[0] - dx / d * 50.0f; b->pos[2] = character.pos[2] - dz / d * 50.0f; } }
    if (HLE_SUN_FORCE == 9) return;     // 9 = never attacks: it only hovers (can a sword reach it?)
    if (HLE_SUN_FORCE == 5) { enter(SB_GLARE); return; }
    if (HLE_SUN_FORCE == 6) { enter(SB_SPIRAL); debugf("[sun] spiral\n"); return; }
    if (HLE_SUN_FORCE == 7) { enter(SB_EMBER); return; }
    if (HLE_SUN_FORCE == 1) { lash_start(b, s.unshackled ? 2 : 1); return; }
    if (HLE_SUN_FORCE == 3) { enter(SB_DIVE); debugf("[sun] dive\n"); return; }
    if (HLE_SUN_FORCE == 4) { enter(SB_CORONA); debugf("[sun] corona\n"); return; }
    if (HLE_SUN_FORCE == 2) { float dx, dz; to_player(b, &dx, &dz); s.lashChain = nearest_chain(b, dx, dz); enter(SB_SWEEP); debugf("[sun] sweep\n"); return; }
#endif
    // Unshackled, the eclipse comes round on its own
    if (s.unshackled && s.cdEclipse <= 0.0f) { s.cdEclipse = 40.0f; enter(SB_ECLIPSE);
#ifdef HLE_AUTOPLAY
        debugf("[sun] eclipse\n");
#endif
        return; }
    // Kept close under it (5 s against its body since the last): the flare, a punish for camping there
    if (s.cdFlare <= 0.0f && s.hugT >= 5.0f && dist < 30.0f) { s.cdFlare = s.unshackled ? 16.0f : 18.0f; s.hugT = 0.0f; enter(SB_FLARE);
#ifdef HLE_AUTOPLAY
        debugf("[sun] flare at %.0f\n", (double)dist);
#endif
        return; }
    // Otherwise the move in reach that has waited longest past its cooldown: by priority alone
    // the dive, corona and spiral took every turn up close
    enum { M_DIVE, M_CORONA, M_SPIRAL, M_RAM, M_LASH, M_SWEEP, M_GLARE, M_EMBER, M_RAIN, M_N };
    float *cd[M_N] = { &s.cdDive, &s.cdCorona, &s.cdSpiral, &s.cdRam, &s.cdLash, &s.cdSweep, &s.cdGlare, &s.cdEmber, &s.cdRain };
    bool ok[M_N] = { dist < 90.0f, dist < 60.0f, true, dist < 110.0f, dist < CHAIN_LEN + SUN_R, s.unshackled && dist < CHAIN_LEN + SUN_R,
                     dist > FAR_DIST, dist > 50.0f, true };
    int m = -1;
    for (int i = 0; i < M_N; i++) if (ok[i] && *cd[i] <= 0.0f && (m < 0 || *cd[i] < *cd[m])) m = i;
    switch (m) {
    case M_DIVE:   s.cdDive = 13.0f; enter(SB_DIVE); break;
    case M_CORONA: s.cdCorona = 11.0f; enter(SB_CORONA); break;
    case M_SPIRAL: s.cdSpiral = s.unshackled ? 10.0f : 13.0f; enter(SB_SPIRAL);
#ifdef HLE_AUTOPLAY
        debugf("[sun] spiral\n");
#endif
        break;
    case M_RAM:    s.cdRam = s.unshackled ? 9.0f : 11.0f; ram_start(b, s.unshackled ? 2 : 1); break;
    case M_LASH:   s.cdLash = s.unshackled ? 4.5f : 6.0f; lash_start(b, s.unshackled ? 2 : 1); break;
    case M_SWEEP: {
        float dx, dz; to_player(b, &dx, &dz);
        s.cdSweep = 10.0f; s.lashChain = nearest_chain(b, dx, dz); enter(SB_SWEEP);
#ifdef HLE_AUTOPLAY
        debugf("[sun] sweep\n");
#endif
        break; }
    // At range: the crosses and the hunting ember (standing off is not safe)
    case M_GLARE:  s.cdGlare = s.unshackled ? 3.5f : 5.0f; enter(SB_GLARE); break;
    case M_EMBER:  s.cdEmber = s.unshackled ? 5.0f : 7.0f; enter(SB_EMBER); break;
    case M_RAIN:   s.cdRain = s.unshackled ? 6.0f : 8.0f; rain_spawn(); sfx_cast(); s.t = -0.3f;
                   s.rainTell = 0.9f; sfx_chain(0); sfx_chain(1); break;     // it shakes in its chains as the light falls
    default: break;
    }
}

// ---- The face: a human face on the ball, its two eyes (lids, iris, pupil), brows and lips
// moving, and every move its own look ----
static void face_update(const Boss *b, float dt)
{
    float f[FACE_N];
    memcpy(f, s.unshackled ? FACE_REST2 : FACE_REST, sizeof(f));
    float rate = 12.0f, sm = speed_mult();
    bool blinks = false;
    // Its eyes follow the player (and lead its turns: the body comes round after them), unless a move looks elsewhere
    {
        float dx, dz, d = to_player(b, &dx, &dz);
        float rel = remainderf(atan2f(dx, dz) - b->rot[1], 2.0f * T3D_PI), up = atan2f(character.pos[1] + 12.0f - b->pos[1], fmaxf(d, 1.0f));
        f[FACE_GX] = fmaxf(-0.09f, fminf(0.09f, rel * 0.8f));
        f[FACE_GY] = fmaxf(-0.08f, fminf(0.06f, up * 0.6f));
        if (s.unshackled) {     // and twitch, unshackled: a darting saccade now and then
            if ((s.saccadeT -= dt) <= 0.0f) {
                s.saccadeT = 0.25f + (rand_custom_u32() % 60) / 100.0f;
                s.saccade[0] = ((int)(rand_custom_u32() % 5) - 2) * 0.012f; s.saccade[1] = ((int)(rand_custom_u32() % 3) - 1) * 0.01f;
            }
            f[FACE_GX] += s.saccade[0]; f[FACE_GY] += s.saccade[1];
        }
    }
    switch (s.state) {
    case SB_GLARE:      // the crosses: a level, narrow-eyed aim down the lane, the pupils slits, lips pressed thin
        f[FACE_UP] = 0.035f; f[FACE_LO] = 0.045f; f[FACE_BRAISE] = -0.03f; f[FACE_PW] = s.step ? 0.3f : 0.45f; f[FACE_PH] = 1.15f; f[FACE_MCURVE] = -0.05f;
        break;
    case SB_SPIRAL:     // its eyes roll round, brows high, mouth a round O, as the rays spin up
        if (s.step == 0) { float w = s.t * 9.0f; f[FACE_GX] = 0.07f * cosf(w); f[FACE_GY] = 0.07f * sinf(w);
                           f[FACE_UP] = 0.14f; f[FACE_LO] = 0.11f; f[FACE_BRAISE] = 0.05f; f[FACE_MOPEN] = 0.07f; f[FACE_MCURVE] = -0.08f; }
        break;
    case SB_EMBER:      // pupils blown wide, a hungry grin
        if (s.t < 0.85f / sm + 0.2f) { f[FACE_PW] = f[FACE_PH] = 2.1f; f[FACE_UP] = 0.13f; f[FACE_LO] = 0.1f; f[FACE_MCURVE] = 0.22f; f[FACE_MOPEN] = 0.02f; f[FACE_SNARL] = 0.015f; }
        break;
    case SB_CORONA:     // screwed shut, brows crushed down, teeth clenched; then wide, screaming, as it bursts
        if (s.step == 0) { f[FACE_UP] = f[FACE_LO] = -0.015f; f[FACE_BTILT] = 0.35f; f[FACE_BRAISE] = -0.05f; f[FACE_SNARL] = 0.03f; f[FACE_MCURVE] = -0.12f; }
        else if (s.t < 1.8f) { f[FACE_UP] = 0.17f; f[FACE_LO] = 0.13f; f[FACE_BRAISE] = 0.06f; f[FACE_PW] = f[FACE_PH] = 0.6f; f[FACE_MOPEN] = 0.1f; f[FACE_SNARL] = 0.01f; }
        break;
    case SB_DIVE:       // wide-eyed, staring down at where it will land, mouth agape
        f[FACE_UP] = 0.16f; f[FACE_LO] = 0.12f; f[FACE_GY] = -0.08f; f[FACE_PW] = f[FACE_PH] = 0.65f; f[FACE_BRAISE] = 0.04f; f[FACE_MOPEN] = 0.04f;
        break;
    case SB_RAM:        // red with rage: brows down in a V, eyes narrowed, teeth bared
        if (s.step < 2) { f[FACE_BTILT] = 0.45f; f[FACE_BRAISE] = -0.04f; f[FACE_UP] = 0.05f; f[FACE_LO] = 0.06f; f[FACE_SNARL] = 0.035f; f[FACE_MOPEN] = 0.015f; f[FACE_MCURVE] = -0.15f; }
        break;
    case SB_FLARE:      // alarm: eyes wide, pupils pinpricks, brows up and in, a gasp
        if (s.step == 0) { f[FACE_UP] = 0.17f; f[FACE_LO] = 0.13f; f[FACE_PW] = f[FACE_PH] = 0.35f; f[FACE_BRAISE] = 0.06f; f[FACE_BTILT] = -0.25f; f[FACE_MOPEN] = 0.08f; f[FACE_GY] = -0.03f; }
        break;
    case SB_ECLIPSE:    // its eyes slowly close as it darkens, a serene smile; then wide, grinning, on the falling flares
        if (s.t < EC_RISE) { f[FACE_UP] = f[FACE_LO] = -0.015f; f[FACE_MCURVE] = 0.2f; rate = 2.5f; }
        else if (s.t < EC_RISE + 2 * EC_GAP + EC_FLY + 0.3f) { f[FACE_UP] = 0.15f; f[FACE_LO] = 0.11f; f[FACE_MCURVE] = 0.25f; f[FACE_MOPEN] = 0.03f; f[FACE_SNARL] = 0.02f; }
        break;
    case SB_LASH:       // a sly glance up at the chain about to rear, one brow's worth of menace, a smirk
        if (s.step == 0) { f[FACE_GX] = sinf(s.ch[s.lashChain].az) > 0.0f ? 0.08f : -0.08f; f[FACE_GY] = 0.06f; f[FACE_BTILT] = 0.18f; f[FACE_MCURVE] = 0.12f; }
        break;
    case SB_SWEEP:      // squinting down along the chain on the floor, a scowl
        if (s.step == 0) { f[FACE_GX] = sinf(s.sweepFrom - b->rot[1]) > 0.0f ? 0.07f : -0.07f; f[FACE_GY] = -0.07f; f[FACE_UP] = 0.045f; f[FACE_LO] = 0.04f; f[FACE_BTILT] = 0.2f; f[FACE_MCURVE] = -0.15f; }
        break;
    case SB_REST:       // knocked down: dazed, its eyes wandering, mouth slack; else spent, drowsy
        if (s.downT >= 0.0f) { f[FACE_UP] = 0.05f; f[FACE_LO] = 0.06f; f[FACE_GX] = 0.05f * sinf(s.t * 3.0f); f[FACE_GY] = 0.03f * cosf(s.t * 2.2f);
                               f[FACE_BRAISE] = 0.03f; f[FACE_BTILT] = -0.2f; f[FACE_MOPEN] = 0.035f; f[FACE_MCURVE] = -0.08f; }
        else { f[FACE_UP] = 0.05f; f[FACE_LO] = 0.065f; f[FACE_MCURVE] = 0.0f; blinks = true; }
        break;
    case SB_BREAK:      // raging free: eyes wide, brows down, a roar
        f[FACE_UP] = 0.16f; f[FACE_LO] = 0.12f; f[FACE_BTILT] = 0.4f; f[FACE_PW] = f[FACE_PH] = 0.55f; f[FACE_MOPEN] = 0.1f; f[FACE_SNARL] = 0.02f; f[FACE_MCURVE] = -0.1f;
        break;
    case SB_DEAD:
        f[FACE_UP] = f[FACE_LO] = -0.015f; f[FACE_MOPEN] = 0.04f; f[FACE_MCURVE] = -0.05f; rate = 2.0f;
        break;
    default:            // drifting: serene, watching the player, blinking now and then
        blinks = true;
        break;
    }
    if (s.rainTell > 0.0f) { f[FACE_GY] = 0.09f; f[FACE_UP] = 0.16f; f[FACE_LO] = 0.1f; f[FACE_BRAISE] = 0.07f; f[FACE_MOPEN] = 0.05f; }  // the rain: it looks up to the sky
    // A blow taken: a face of its own, a small one (a plain hit) or a big one (a combo's finisher,
    // a skill), and a recoil. Mid-move only its brows and mouth react, so the move's tell (its eyes) stays read
    if (b->hitSeq != s.hitSeen) {
        s.hitSeen = b->hitSeq;
        bool big = b->hitWeight >= 2.0f;
        if (s.state != SB_DEAD && s.state != SB_INTRO) {
            int pick = rand_custom_u32() % (big ? 2 : 3);
            if (big ? s.rx - RX_SCREAM == pick : s.rx == pick) pick = (pick + 1) % (big ? 2 : 3);    // not the same face twice running
            s.rx = big ? RX_SCREAM + pick : pick;
            s.rxT = 0.0f; s.rxLen = big ? 0.75f : 0.4f;
            s.jolt = fmaxf(s.jolt, big ? 1.0f : 0.45f);
#ifdef HLE_LOG_MOVES
            debugf("[sunhit] weight %.1f: face %d\n", (double)b->hitWeight, s.rx);
#endif
        }
    }
    if (s.rxT >= 0.0f) {
        s.rxT += dt;
        // in fast (0.05 s), held, then let go over the last 40%
        float k = clamp01(s.rxT / 0.05f) * clamp01((s.rxLen - s.rxT) / (0.4f * s.rxLen));
        bool tell = !(s.state == SB_HOVER || s.state == SB_REST || s.state == SB_BREAK);
        for (int i = 0; i < FACE_N; i++) {
            bool eyes = i <= FACE_LO || (i >= FACE_PW && i <= FACE_GY);
            if (!(tell && eyes)) f[i] += (FACE_RX[s.rx][i] - f[i]) * k;
        }
        rate = fmaxf(rate, 25.0f);       // a flinch is quick
        if (s.rxT >= s.rxLen) s.rxT = -1.0f;
    }
    s.jolt = fmaxf(0.0f, s.jolt - dt * 2.5f);
    for (int i = 0; i < FACE_N; i++) s.face[i] += (f[i] - s.face[i]) * fminf(1.0f, dt * rate);
    // A blink: shut in 0.07 s, open in 0.13 (on top of the eased lids, never mid-tell)
    if (s.blinkT >= 0.0f) { s.blinkT += dt; if (s.blinkT > 0.2f) s.blinkT = -1.0f; }
    else if (blinks && (s.blinkIn -= dt) <= 0.0f) { s.blinkT = 0.0f; s.blinkIn = 1.8f + (rand_custom_u32() % 300) / 100.0f; }
    // The ram: blood red, pulsing, as it rears and charges
    float red = s.state == SB_RAM && s.step < 2 ? (s.step ? 1.0f : clamp01(s.t / (0.6f / sm))) : 0.0f;
    s.red += (red - s.red) * fminf(1.0f, dt * 10.0f);
}

void sun_boss_update(Boss* b, float dt)
{
    if (!s.ready) return;
    if (b->damageFlashTimer > 0.0f) b->damageFlashTimer -= dt;
    s.t += dt;
    s.life += dt;
#ifdef HLE_SUN_HITME    // test aid: a sword hit every 1.2 s, every third one a finisher (the passive dummy never hits it)
    { static float ht; static int n; if (b->mode == BOSS_MODE_COMBAT && (ht += dt) >= 1.2f) { ht = 0.0f; n++; boss_apply_hit(b, 0.01f, (n % 3 ? 1.0f : 2.5f) * 4.0f / POISE_DAMAGE); } }
#endif
    float sm = speed_mult();
    s.cdGlare -= dt * sm; s.cdRain -= dt * sm; s.cdCorona -= dt * sm; s.cdDive -= dt * sm; s.cdLash -= dt * sm; s.cdSweep -= dt * sm;
    s.cdSpiral -= dt * sm; s.cdEmber -= dt * sm; s.cdFlare -= dt * sm; s.cdRam -= dt * sm; s.cdEclipse -= dt * sm;
    s.raysSpin += (1.0f - s.raysSpin) * fminf(1.0f, dt * 2.0f);
    rain_tick(dt);
    flare_tick(dt);
    if (!s.chainsReady) chains_reset(b);

    float dx, dz, dist = to_player(b, &dx, &dz);
    if (dist < 30.0f) s.hugT += dt;

    if (b->state == BOSS_STATE_DEAD && s.state != SB_DEAD) {
        enter(SB_DEAD);
        for (int k = 0; k < NCH; k++) s.ch[k].kin = s.ch[k].hot = false;
        for (int i = 0; i < RAIN_MAX; i++) s.rainT[i] = -1.0f;
        s.flareAge = -1.0f;
    }
    // Half health: the shackles break
    if (!s.unshackled && s.state != SB_DEAD && b->health <= b->maxHealth * 0.5f) {
        s.unshackled = true;
        for (int k = 0; k < NCH; k++) s.ch[k].kin = s.ch[k].hot = false;
        b->mode = BOSS_MODE_CINEMATIC;
        enter(SB_BREAK);
    }


    if (!(s.state == SB_RAM && s.step == 1)) b->capsuleCollider.radius = SUN_R + 4.0f;     // (a stagger mid-charge)
    // Past a move's windup (step 0), or diving: the strike itself
    b->striking = s.state == SB_DIVE || ((s.state == SB_GLARE || s.state == SB_CORONA || s.state == SB_LASH || s.state == SB_SWEEP
                                          || s.state == SB_SPIRAL || s.state == SB_EMBER || s.state == SB_FLARE || s.state == SB_RAM
                                          || s.state == SB_ECLIPSE) && s.step > 0);
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
        s.combo = false;
        face_player(b, dt, 3.0f);
        s.orbit += dt * 0.35f * sm;
        float away = atan2f(-dx, -dz) + s.orbit * 0.3f;
        float keep = KEEP_DIST;
#ifdef HLE_SUN_FORCE
        if (HLE_SUN_FORCE == 5 || HLE_SUN_FORCE == 7) keep = 85.0f;     // test aid: hovers at range (the ranged moves)
#endif
        move_toward(b, character.pos[0] + sinf(away) * keep, character.pos[2] + cosf(away) * keep, DRIFT_SPEED * sm, dt);
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
    case SB_SPIRAL: spiral_update(b, dt); break;
    case SB_EMBER:  ember_update(b, dt); break;
    case SB_FLARE:  flare_update(b, dt); break;
    case SB_RAM:    ram_update(b, dt); break;
    case SB_ECLIPSE: eclipse_update(b, dt); break;

    case SB_REST:
        // Dimmed on the floor: the opening (a stagger drops it there from the air)
        if (s.downT >= 0.0f) {      // knocked down: it falls, bounces twice, rocks, then gets up
            s.downT += dt;
            float fall = clamp01(s.downT / 0.25f), bt = s.downT - 0.25f;
            float bounce = bt > 0.0f ? fabsf(sinf(bt * 9.0f)) * 9.0f * expf(-bt * 4.0f) : 0.0f;
            float h = s.groundY + REST_H + bounce;
            b->pos[1] += (h - b->pos[1]) * (fall < 1.0f ? fminf(1.0f, dt * 14.0f) : 1.0f);
            if (bt > 0.0f && bt - dt <= 0.0f) { screen_shake_set_shake_mag(9.0f); sfx_boom(); }
        } else
        b->pos[1] += (s.groundY + REST_H - b->pos[1]) * fminf(1.0f, dt * 8.0f);
#ifdef HLE_AUTOPLAY
        if (s.t > 3.0f / sm && s.downT >= 0.0f) { extern float gameTime; debugf("[stagger] sun up at %.1f s\n", (double)gameTime); }
#endif
        if (s.t > 3.0f / sm) s.downT = -1.0f;
        if (s.t > 3.0f / sm) enter(SB_HOVER);
        break;

    case SB_BREAK:
        // The chains snap, then it burns hotter
        b->pos[1] += (s.groundY + HOVER_H + 10.0f - b->pos[1]) * fminf(1.0f, dt * 3.0f);
        s.raysSpin = 8.0f;
        if (s.weights && s.t >= 0.5f) {   // the weights burst and the chains whip free
            s.weights = false;
            sfx_chain(0); sfx_chain(1);
            for (int k = 0; k < NCH; k++)
                for (int i = 1; i < NN; i++) s.ch[k].q[i][1] -= 0.6f * i;
            screen_shake_set_shake_mag(10.0f);
        }
        if (s.step < 6 && s.t >= s.step * 0.25f) {
            screen_shake_set_shake_mag(8.0f);
            if (s.step == 0) sfx_boom();
            s.step++;
        }
        if (s.t > 2.0f) { b->mode = BOSS_MODE_COMBAT; s.cdCorona = 0.0f; s.cdEclipse = 6.0f; enter(SB_HOVER); }
        break;

    case SB_DEAD:
        // Gutters out and sinks to the floor
        b->pos[1] += (s.groundY + 6.0f - b->pos[1]) * fminf(1.0f, dt * 1.5f);
        break;
    }
    b->rot[0] = b->rot[2] = 0.0f;

    // Its tells are its body's and its face's (face_update), each move its own; no glow
    float inhale = s.state == SB_FLARE && s.step == 0 ? clamp01(s.t / (0.8f / sm)) : 0.0f;
    float dark = s.state == SB_ECLIPSE && s.t < EC_RISE + 2 * EC_GAP + EC_FLY + 0.3f ? 1.0f : 0.0f;
    s.inhale += (inhale - s.inhale) * fminf(1.0f, dt * (inhale > s.inhale ? 8.0f : 12.0f));
    s.dark += (dark - s.dark) * fminf(1.0f, dt * 2.5f);
    face_update(b, dt);
    // Attacking, its corona and loops of flame whirl up (harder as it strikes), easing back as it drifts
    bool attacking = !(s.state == SB_INTRO || s.state == SB_HOVER || s.state == SB_REST || s.state == SB_BREAK || s.state == SB_DEAD) || s.rainTell > 0.0f;
    float whirl = attacking ? (b->striking ? 7.0f : 4.5f) : 0.0f;
    if (s.raysSpin < whirl) s.raysSpin += (whirl - s.raysSpin) * fminf(1.0f, dt * 6.0f);
    if (s.rainTell > 0.0f) s.rainTell -= dt;

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

// m's matrix: the eye's (base) times a turn about its middle, L (rows: the images' x, y, z)
static void face_mat(ProcMesh *m, const T3DMat4 *base, const float L[3][3])
{
    T3DMat4 loc, w;
    t3d_mat4_identity(&loc);
    for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) loc.m[c][r] = L[r][c];
    t3d_mat4_mul(&w, base, &loc);
    t3d_mat4_to_fixed_3x4(m->mat, &w);
}

static void m3_mul(float out[3][3], const float a[3][3], const float b[3][3])
{
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
}

// Turns about x (+: the eye's front down toward -y), y (+: toward +x) and z (+: x toward y)
static void rot_x(float m[3][3], float a) { float c = fm_cosf(a), n = fm_sinf(a); float r[3][3] = { { 1, 0, 0 }, { 0, c, -n }, { 0, n, c } }; memcpy(m, r, sizeof(r)); }
static void rot_y(float m[3][3], float a) { float c = fm_cosf(a), n = fm_sinf(a); float r[3][3] = { { c, 0, n }, { 0, 1, 0 }, { -n, 0, c } }; memcpy(m, r, sizeof(r)); }
static void rot_z(float m[3][3], float a) { float c = fm_cosf(a), n = fm_sinf(a); float r[3][3] = { { c, -n, 0 }, { n, c, 0 }, { 0, 0, 1 } }; memcpy(m, r, sizeof(r)); }

// A part built in frame F (the right side's; mirrored in x for the left), turned by L in it,
// drawn in the current batch
static void face_part(ProcMesh *m, const T3DMat4 *base, const float F[3][3], bool left, const float L[3][3])
{
    float t[3][3];
    m3_mul(t, F, L);
    if (left) for (int c = 0; c < 3; c++) t[0][c] = -t[0][c];
    face_mat(m, base, t);
    proc_mesh_batch_draw(m);
}

// The moving parts: the opaque ones in one batch, then the soft-edged lids (and the blush) in another
static void face_draw(const T3DMat4 *base, int h, color_t tint)
{
    const float *f = s.face;
    float gx[3][3], gy[3][3], gaze[3][3], m[3][3], a[3][3], b[3][3];
    proc_mesh_batch_begin(false, tint, NULL);
    for (int side = 0; side < 2; side++) {
        bool left = side;
        // the eyes look the same way: the left one's gaze is mirrored back
        rot_y(gx, left ? -f[FACE_GX] : f[FACE_GX]); rot_x(gy, -f[FACE_GY]); m3_mul(gaze, gx, gy);
        face_part(&s.iris[h], base, s.eyeF, left, gaze);
        float sc[3][3] = { { f[FACE_PW], 0, 0 }, { 0, f[FACE_PH], 0 }, { 0, 0, 1 } };
        m3_mul(m, gaze, sc);
        face_part(&s.pupil, base, s.eyeF, left, m);
        rot_x(a, -f[FACE_BRAISE]); rot_z(b, f[FACE_BTILT]); m3_mul(m, b, a);
        face_part(&s.brow, base, s.eyeF, left, m);
        // lips: parted (the upper one drawn up off the teeth by the snarl), their corners up or down
        rot_z(b, f[FACE_MCURVE]);
        rot_x(a, -(0.4f * f[FACE_MOPEN] + f[FACE_SNARL])); m3_mul(m, b, a);
        face_part(&s.lipU[h], base, s.mouthF, left, m);
        rot_x(a, 0.6f * f[FACE_MOPEN]); m3_mul(m, b, a);
        face_part(&s.lipL[h], base, s.mouthF, left, m);
    }
    proc_mesh_batch_end();
    // a blink shuts the lids on top of it all
    float shut = s.blinkT < 0.0f ? 0.0f : s.blinkT < 0.07f ? s.blinkT / 0.07f : 1.0f - (s.blinkT - 0.07f) / 0.13f;
    proc_mesh_batch_begin(true, tint, NULL);
    proc_mesh_batch_draw(&s.blush[h]);
    for (int side = 0; side < 2; side++) {
        bool left = side;
        // lids: the upper one turned up off the eye's level line (tilted with the brow), the lower one its mirror turned down
        rot_x(a, -(f[FACE_UP] * (1.0f - shut) - 0.015f * shut)); rot_z(b, 0.5f * f[FACE_BTILT]); m3_mul(m, b, a);
        face_part(&s.lid[h], base, s.eyeF, left, m);
        rot_x(a, -(f[FACE_LO] * (1.0f - shut) - 0.015f * shut));
        float mir[3][3] = { { 0.997f, 0, 0 }, { 0, -0.997f, 0 }, { 0, 0, 0.997f } };
        m3_mul(m, mir, a);
        face_part(&s.lid[h], base, s.eyeF, left, m);
    }
    proc_mesh_batch_end();
}

void sun_boss_draw(Boss* b)
{
    if (!s.ready || !b->visible) return;
    float dying = (s.state == SB_DEAD) ? clamp01(s.t / 3.0f) : 0.0f;
    float pulse = 1.0f + 0.04f * sinf(s.life * 5.0f) + (s.rainTell > 0.0f ? 0.25f * fabsf(sinf(s.rainTell * 7.0f)) : 0.0f)   // the rain: two throbs (drawn only: the lock-on stays put)
                + (b->damageFlashTimer > 0.0f ? 0.08f : 0.0f);
    float scale = (SUN_R / MESH_R) * pulse * (1.0f - 0.5f * dying);
    static float restK;     // shrinks a little on the floor, easing (a stagger drops it there)
    restK += ((s.state == SB_REST ? 1.0f : 0.0f) - restK) * fminf(1.0f, deltaTime * 6.0f);
    scale *= (1.0f - 0.1f * restK) * (1.0f - 0.12f * s.inhale);     // the flare: the eye draws in

    float yaw = -b->rot[1];     // Tiny3D's yaw turns +z the other way round
    // Knocked down: it rocks side to side on the floor, settling (and squashes as it lands)
    float rock = s.state == SB_REST && s.downT > 0.25f ? 0.45f * sinf((s.downT - 0.25f) * 7.0f) * expf(-(s.downT - 0.25f) * 1.2f) : 0.0f;
    rock -= 0.22f * s.jolt * s.jolt;      // a blow taken: it reels back (drawn only: hitboxes and lock-on stay put)
    int h = s.unshackled;
    T3DMat4 base;
    t3d_mat4_from_srt_euler(&base, (float[3]){ scale / 8.0f, scale / 8.0f, scale / 8.0f }, (float[3]){ rock, yaw, 0.0f }, b->pos);
    proc_mesh_set_transform(&s.ball[h], scale, (float[3]){ rock, yaw, 0.0f }, b->pos);

    // Its rays: the spikes (and ring) and the wavy flames between them, turning opposite ways
    static float spinAngle;
    spinAngle += deltaTime * 0.6f * s.raysSpin;
    float fury = clamp01((s.raysSpin - 1.0f) / 6.0f);     // whirling: the rays lash in and out
    float cs = scale * (1.0f + 0.05f * sinf(s.life * 7.0f) + fury * 0.08f * sinf(s.life * 23.0f)) * (1.0f - dying) * (1.0f + 0.45f * s.inhale);    // and flare
    proc_mesh_set_transform(&s.rays[h][0], cs, (float[3]){ rock, yaw, spinAngle }, b->pos);
    proc_mesh_set_transform(&s.rays[h][1], cs, (float[3]){ rock, yaw, -spinAngle * 1.3f }, b->pos);

    // The ram: blood red, pulsing; eclipsed, a dark violet, its rays still burning round it
    float warn = s.red * (0.7f + 0.3f * sinf(s.life * 18.0f));
    color_t tint = RGBA32(255, (int)(255 - 225 * warn), (int)(255 - 235 * warn), 255);
    color_t eyeTint = RGBA32((int)(tint.r * (1.0f - 0.78f * s.dark)), (int)(tint.g * (1.0f - 0.85f * s.dark)), (int)(tint.b * (1.0f - 0.6f * s.dark)), 255);

#ifdef HLE_LOG_CLOTH
    uint32_t t0 = TICKS_READ();
#endif
    if (s.chainsReady) chains_draw(b);
#ifdef HLE_LOG_CLOTH
    { static uint32_t sum; static int n; sum += TICKS_READ() - t0; if (++n == 120) { debugf("[chains] draw %lu us a frame\n", (unsigned long)(TICKS_TO_US(sum) / n)); sum = 0; n = 0; } }
#endif
#ifdef HLE_PROF_DRAGON   // test aid: RSP+RDP time of each part (its timers), each run alone
    static uint32_t sp[3], spN;
#define SPROF(i) { sp[i] += gpu_prof_end(); gpu_prof_begin(); }
    gpu_prof_begin();
#else
#define SPROF(i)
#endif
    if (dying < 1.0f) {
        proc_mesh_batch_begin(false, tint, NULL);
        for (int w = 0; w < 2; w++) proc_mesh_batch_draw(&s.rays[h][w]);
        proc_mesh_batch_end();
    }
    SPROF(0);
    proc_mesh_draw_textured(&s.ball[h], s.texVeins, eyeTint);
    SPROF(1);
    // Fog off for the soft edges (the lids, the blush): tiny3d writes the fog into the vertices' alpha (as skill_fx)
    t3d_fog_set_enabled(false);
    proc_mesh_set_transform(&s.blush[h], scale, (float[3]){ rock, yaw, 0.0f }, b->pos);
    face_draw(&base, h, eyeTint);
    SPROF(2);
    t3d_fog_set_enabled(true);
#ifdef HLE_PROF_DRAGON
    gpu_prof_end();
    if (++spN == 120) { debugf("[sunprof] rays %.2f ball %.2f face %.2f ms\n", TICKS_TO_US(sp[0] / 120) / 1000.0, TICKS_TO_US(sp[1] / 120) / 1000.0,
        TICKS_TO_US(sp[2] / 120) / 1000.0); memset(sp, 0, sizeof sp); spN = 0; }
#endif
#undef SPROF
}
