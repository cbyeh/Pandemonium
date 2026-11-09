#include <libdragon.h>
#include "gpu_frame.h"
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>
#include <t3d/t3dmodel.h>
#include <t3d/t3dskeleton.h>
#include <math.h>
#include <string.h>

#include "character_skin.h"
#include "game_math.h"
#include "game_lighting.h"
#include "globals.h"
#include "fx/proc_mesh.h"

// Knight rig facts (from knight.glb): it faces -X, Y up; bone rest rotations are
// identity, so bone-local offsets are model offsets. Offsets below are in glTF units.
#define GLTF 64.0f                     // t3d base scale: model units per glTF unit
// The cloth's lengths and the body's radii, in glTF units, times GU: tuned in the fights, where he's
// drawn at half MODEL_SCALE (2 world units per glTF unit there, GU 4). Scaled with him (bones_cache):
// fixed, the title's knight (1.5x) wore a cape a third his size and the armour went through it
static float GU = GLTF * MODEL_SCALE;

typedef struct { uint8_t r, g, b, set; } SkinColor;
#define C(r, g, b) { r, g, b, 1 }
static bool s_bladeGlow;     // the equipped sword's blade lit by itself (Lunar Edge, Lunar Requiem)
static SkinColor s_bladeEdge;     // the equipped sword's colour on the Dragonslayer's edges (Shadowslayer: its own, cold white)
static SkinColor s_bladeTint;     // the equipped sword's colour (character_skin_set_blade): over the skin's sword, or the Dragonslayer's edges

enum { CLOTH_NONE, CLOTH_CLOAK, CLOTH_HAIR, CLOTH_PLUME };

typedef struct {
    uint8_t kind;
    SkinColor color;
    bool tattered;
    float len, flare;                  // glTF units; bottom width over top width - 1
    float drop;                        // a cloak pinned this much below the shoulders
} ClothSpec;

// Accessories: procedural meshes riding a bone (see acc_build)
enum { ACC_WINGED_HELM = 1, ACC_BERSERK_HELM = 4, ACC_DRAGONSLAYER = 8,
       ACC_NIGHTFARER_HELM = 16, ACC_MANTLE = 32, ACC_CLAW_SHOT = 64 };

typedef struct {
    const char *name;
    SkinColor armor, helm, cloth, plume, chain, sword;
    bool hidePlume, hideSword;
    ClothSpec cl[2];
    bool hood;                         // a hood up over the helmet (hides the plume)
    uint8_t acc;
} SkinDef;

#define CLOAK(r, g, b) { CLOTH_CLOAK, C(r, g, b), false, 3.1f, 0.8f }   // to the ankles, standing

static const SkinDef SKINS[SKIN_COUNT] = {
    [SKIN_KNIGHT]           = { "Knight" },
    [SKIN_TEAL_CLOAK]       = { "Teal Cloak",    .cloth = C(20, 70, 70), .cl = { CLOAK(30, 120, 118) }, .hood = true },
    [SKIN_CRIMSON_CLOAK]    = { "Crimson Cloak", .cloth = C(90, 16, 18), .cl = { CLOAK(150, 26, 30) },  .hood = true },
    [SKIN_UMBER_CLOAK]      = { "Umber Cloak",   .cloth = C(62, 42, 26), .cl = { CLOAK(105, 70, 44) },  .hood = true },
    // Artorias: dark silver plate, bright mail, a torn royal-blue cloak, a long black plume
    [SKIN_WOLF_KNIGHT]      = { "Wolf Knight", .armor = C(112, 104, 100), .helm = C(146, 142, 146), .chain = C(165, 175, 196),
                                .cloth = C(30, 52, 108), .hidePlume = true,
                                .cl = { { CLOTH_CLOAK, C(42, 74, 152), true, 3.1f, 1.2f }, { CLOTH_PLUME, C(22, 22, 30), false, 3.6f, 0.4f } } },
    // Guts: blackened plate, a spined wolf helm with burning eyes, a ragged black cloak over the shoulders and chest, the Dragonslayer
    [SKIN_BLACK_SWORDSMAN]  = { "Black Swordsman", .armor = C(58, 57, 63), .helm = C(52, 51, 58), .cloth = C(24, 22, 22),
                                .chain = C(22, 22, 24), .hidePlume = true, .hideSword = true,
                                .cl = { { CLOTH_CLOAK, C(26, 24, 27), true, 3.2f, 1.5f } }, .acc = ACC_BERSERK_HELM | ACC_DRAGONSLAYER | ACC_MANTLE },
    // Malenia: brass plate, an olive pleated dress, a winged helm, a red mane from under it over a crimson cape (it tapers)
    [SKIN_SCARLET_VALKYRIE] = { "Scarlet Valkyrie", .armor = C(196, 156, 82), .helm = C(208, 168, 92), .cloth = C(98, 80, 50),
                                .chain = C(72, 60, 40), .hidePlume = true,
                                .cl = { { CLOTH_HAIR, C(200, 50, 28), true, 2.0f, 0.45f }, { CLOTH_CLOAK, C(86, 12, 16), false, 2.3f, 1.8f, 0.3f } },
                                .acc = ACC_WINGED_HELM },
    // Nightfarer (Nightreign): a winged silver helm, a royal-blue coat across the shoulders and chest, a tabard,
    // the claw shot on his left forearm
    [SKIN_NIGHTFARER]           = { "Nightfarer", .armor = C(92, 96, 112), .helm = C(176, 180, 194), .cloth = C(30, 56, 150),
                                .chain = C(58, 60, 72), .hidePlume = true,
                                .cl = { { CLOTH_CLOAK, C(38, 66, 170), false, 3.2f, 0.75f } },
                                .acc = ACC_NIGHTFARER_HELM | ACC_MANTLE | ACC_CLAW_SHOT },
};

static int  s_skin = SKIN_KNIGHT;
static const T3DModel *s_model;
static const T3DSkeleton *s_skel;
static rspq_block_t **s_block;       // character.c's dpl_model, re-recorded on change

const char* character_skin_name(int id) { return (id >= 0 && id < SKIN_COUNT) ? SKINS[id].name : SKINS[0].name; }
int  character_skin_get(void) { return s_skin; }

/* -----------------------------------------------------------------------------
 * Bones
 * -------------------------------------------------------------------------- */

enum { B_WAIST, B_SPINE, B_HEAD, B_ARM_L, B_ARM_R, B_FORE_L, B_FORE_R, B_HAND_L, B_HAND_R, B_LEG_L, B_LEG_R, B_SHIN_L, B_SHIN_R, B_FOOT_L, B_FOOT_R, B_COUNT };
static const char *BONE_NAMES[B_COUNT] = {
    "Waist", "Spine1", "Head", "UpperArm-Left", "UpperArm-Right", "LowerArm-Left", "LowerArm-Right", "Hand-Left", "Hand-Right", "UpperLeg-Left", "UpperLeg-Right",
    "LowerLeg-Left", "LowerLeg-Right", "Foot-Left", "Foot-Right",
};
static int s_bone[B_COUNT];

// Each bone's matrix times the model's, in floats, once a frame (bones_cache): a point through
// the two fixed-point matrices cost 24 conversions, and the cloth places ~45 points a frame
static float s_bw[B_COUNT][4][3];      // world = x*[0] + y*[1] + z*[2] + [3], x y z in glTF units

static void mat_f32(const T3DMat4FP *m, float out[4][3])
{
    for (int c = 0; c < 3; c++) {
        float col[3]; mat4fp_get_axis_colbasis_f32(m, c, col);
        for (int r = 0; r < 3; r++) out[r][c] = col[r];
    }
    mat4fp_get_translation_row3_f32(m, out[3]);
}

// Bones placed with an offset (the rest are only used at their origin: just [3] is worked out)
#define BONES_OFFSET ((1 << B_SPINE) | (1 << B_WAIST) | (1 << B_HEAD) | (1 << B_HAND_R))

static void bones_cache(const T3DMat4FP *modelMat)
{
    float m[4][3], bm[4][3];
    mat_f32(modelMat, m);
    for (int b = 0; b < B_COUNT; b++) {
        const T3DMat4FP *B = &s_skel->boneMatricesFP[s_bone[b]];
        int j0 = 3;
        if (BONES_OFFSET >> b & 1) { mat_f32(B, bm); j0 = 0; }
        else mat4fp_get_translation_row3_f32(B, bm[3]);
        for (int j = j0; j < 4; j++)
            for (int k = 0; k < 3; k++) {
                float s = bm[j][0] * m[0][k] + bm[j][1] * m[1][k] + bm[j][2] * m[2][k];
                s_bw[b][j][k] = j < 3 ? s * GLTF : s + m[3][k];
            }
    }
    const float *x = s_bw[B_SPINE][0];
    float gu = 2.0f * sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (gu > 1e-3f) GU = gu;     // 0 on the title's first frame, before the rig is posed
}

// bone-local glTF offset -> world (bones_cache first)
static void bone_world(int b, float x, float y, float z, float out[3])
{
    assertf((BONES_OFFSET >> b & 1) || (x == 0 && y == 0 && z == 0), "bone %d: add it to BONES_OFFSET", b);
    const float (*c)[3] = s_bw[b];
    for (int k = 0; k < 3; k++) out[k] = x * c[0][k] + y * c[1][k] + z * c[2][k] + c[3][k];
}

/* -----------------------------------------------------------------------------
 * Verlet cloth (world space)
 * -------------------------------------------------------------------------- */

#define CLOTH_COLS 7
#define CLOTH_ROWS 8
#define CLOTH_N (CLOTH_COLS * CLOTH_ROWS)
#define RING 4                          // frames the RSP may still be reading
// One step a frame, CLOTH_ITERS passes over the links, the body pushed out on the last: two
// steps of six (three of them colliding) cost 17 ms a frame on hardware, halving the frame rate.
// Two: the tethers (cloth_tether) hold its length, the passes only shape it
#define CLOTH_ITERS 2

// ab, il2 (1/|ab|^2), r2 and a bounding sphere (m, br2) are worked out once a frame (cloth_bodies)
typedef struct { float a[3], b[3], r, ab[3], il2, r2, m[3], br2; } Capsule;

typedef struct {
    int cols, rows;
    float p[CLOTH_N][3], q[CLOTH_N][3];
    float anchor[CLOTH_COLS][3], anchor2[CLOTH_COLS][3];   // anchor2: a plume's second, stiff row
    float hrest[CLOTH_ROWS], vrest[CLOTH_COLS];
    // each link's rest length and 1/rest^2 (link): across, down (to the next row), bend (two down)
    float hl[CLOTH_ROWS][2], vl[CLOTH_ROWS][CLOTH_COLS][2], bl[CLOTH_ROWS][CLOTH_COLS][2];
    float fall;                 // how fast its pins drop (units/s, 0 rising): the air lifts it
    float tl[CLOTH_ROWS][CLOTH_COLS];   // tethers: the farthest each point gets from its pin (its column's length to it)
    bool valid;
    float time;
    T3DVertPacked *vb[RING];
    rspq_block_t *tris[RING];   // loads its buffer and draws the grid: recorded once (per width)
    int trisGrid;               // the grid they were built for: cols x 16 + rows
    float acc;                  // time gathered for the next step (it steps every other frame)
    int built;                  // the ring slot holding the vertices last built (-1: none)
    bool stepped;               // stepped this frame: drawn halfway from the last state (q) to p
    int ring;
    uint32_t col[CLOTH_N];      // vertex colours, lit on step frames (between them it barely turns)
    uint32_t near[CLOTH_N];     // the capsules each particle was near at the last step (bits)
} Cloth;

static Cloth s_cloth[2];
static Capsule s_body[20];      // 20 < 32: a bit each in Cloth.near
static int s_bodyCount;
static uint32_t s_armBits;     // the sword and arms (they swing fast: cloth_draw always checks them)
static int s_blade;             // the blade's capsule; where it was at the last step, and its sweep since (swept)
static float s_bladeA0[3], s_bladeB0[3], s_swM[3], s_swR2;
static bool s_bladeOk, s_swing;

static const uint8_t TATTER[CLOTH_COLS] = { 100, 15, 60, 5, 80, 25, 90 };  // % of the last three segments

void character_skin_reset_cloth(void) { s_cloth[0].valid = s_cloth[1].valid = false; s_bladeOk = false; }

static inline float *PC(Cloth *cl, int c, int r) { return cl->p[r * cl->cols + c]; }
static inline bool strands(const ClothSpec *sp) { return sp->kind == CLOTH_HAIR || sp->kind == CLOTH_PLUME; }

static void cloth_anchors(Cloth *cl, const ClothSpec *sp)
{
    for (int c = 0; c < cl->cols; c++) {
        float u = (float)c / (cl->cols - 1) * 2.0f - 1.0f;   // -1..1 across
        if (sp->kind == CLOTH_CLOAK && sp->drop > 0.0f)   // across the back, under the hair
            bone_world(B_SPINE, 1.25f - 0.7f * u * u + ((c & 1) ? 0.06f : 0.0f), 2.62f - sp->drop, 0.8f * u, cl->anchor[c]);
        else if (sp->kind == CLOTH_CLOAK)   // from round the hood, onto the pauldrons at the sides, pleated (it flares out from there)
            bone_world(B_SPINE, 0.95f - 0.95f * u * u + ((c & 1) ? 0.08f : 0.0f), 2.62f, 1.1f * u, cl->anchor[c]);
        else if (sp->kind == CLOTH_HAIR)    // a mane from under the helm's back rim, at the nape
            bone_world(B_HEAD, 0.4f + 0.18f * (1.0f - u * u), 0.85f, 0.5f * u, cl->anchor[c]);
        else {                              // a crest on top of the helm that arches up and back
            bone_world(B_HEAD, -0.15f + 0.25f * (u + 1.0f), 2.0f, 0.1f * u, cl->anchor[c]);
            bone_world(B_HEAD, 0.35f + 0.25f * (u + 1.0f), 2.55f, 0.12f * u, cl->anchor2[c]);
        }
    }
}

static void cloth_bodies(void)
{
    Capsule *b = s_body; int n = 0;
    #define SPHERE(bone, x, y, z, rad) do { bone_world(bone, x, y, z, b[n].a); memcpy(b[n].b, b[n].a, sizeof b[n].a); b[n].r = (rad); n++; } while (0)
    #define CAPS(b0, b1, rad) do { bone_world(b0, 0, 0, 0, b[n].a); bone_world(b1, 0, 0, 0, b[n].b); b[n].r = (rad); n++; } while (0)
    // Radii (in glTF units, times GU below) are the mesh's extents plus a margin, as cloth triangles span between particles;
    // more on the elbows and forearms, whose spiked guards poked through between them
    const float m = 0.12f, ma = 0.24f, ml = 0.4f;   // ml: the hips and legs, whose spiked guards poked through at the mesh's size
    SPHERE(B_SPINE, -0.1f, 1.45f, 0, 1.0f + m);      // upper back
    SPHERE(B_SPINE, -0.1f, 0.35f, 0, 0.95f + m);
    SPHERE(B_WAIST, -0.05f, -0.2f, 0, 0.9f + ml);
    SPHERE(B_HEAD, -0.34f, 1.07f, 0, 0.9f);          // hair and plume roots sit on it
    const int arm0 = n;
    // sword hilt (the blade runs along the hand's -X): crossguard and pommel
    bone_world(B_HAND_R, -0.3f, 0, 0, b[n].a); bone_world(B_HAND_R, 1.25f, 0, 0, b[n].b); b[n].r = 0.7f + m; n++;
    // and the blade, pommel to tip (the stock sword's line; the Dragonslayer lies along it, broader)
    bone_world(B_HAND_R, 0.2f, 0.7f, 0.1f, b[n].a); bone_world(B_HAND_R, -8.0f, 1.05f, -1.0f, b[n].b);
    b[n].r = ((SKINS[s_skin].acc & ACC_DRAGONSLAYER) ? 0.75f : 0.4f) + m; n++;
    CAPS(B_ARM_L, B_FORE_L, 0.62f + ma);             // pauldrons and upper arms
    CAPS(B_ARM_R, B_FORE_R, 0.62f + ma);
    SPHERE(B_FORE_L, 0, 0, 0, 0.88f + ma);            // pointed elbow guards
    SPHERE(B_FORE_R, 0, 0, 0, 0.88f + ma);
    CAPS(B_FORE_L, B_HAND_L, 0.45f + ma);
    CAPS(B_FORE_R, B_HAND_R, 0.45f + ma);
    SPHERE(B_HAND_L, 0, 0, 0, 0.45f + ma);            // the left gauntlet (the right is in the hilt)
    s_armBits = ((1u << n) - 1) & ~((1u << arm0) - 1);
    s_blade = arm0 + 1;
    CAPS(B_LEG_L, B_SHIN_L, 0.5f + ml);
    CAPS(B_LEG_R, B_SHIN_R, 0.5f + ml);
    CAPS(B_SHIN_L, B_FOOT_L, 0.42f + ml);
    CAPS(B_SHIN_R, B_FOOT_R, 0.42f + ml);
    #undef SPHERE
    #undef CAPS
    s_bodyCount = n;
    for (int i = 0; i < n; i++) {
        Capsule *c = &b[i];
        float l2 = 0.0f;
        for (int k = 0; k < 3; k++) { c->ab[k] = c->b[k] - c->a[k]; l2 += c->ab[k] * c->ab[k]; c->m[k] = (c->a[k] + c->b[k]) * 0.5f; }
        c->il2 = l2 > 0.0f ? 1.0f / l2 : 0.0f;
        c->r *= 0.5f * GU;     // the mesh's own size (GU is twice it: the cloth's lengths were tuned so)
        c->r2 = c->r * c->r;
        float br = sqrtf(l2) * 0.5f + c->r;
        c->br2 = br * br;
    }
}

__attribute__((noinline, optimize("Os"))) static void cloth_reset(Cloth *cl, const ClothSpec *sp)   // rare (a first frame, a teleport): for size
{
    float len = sp->len * GU;
    float top = sqrtf(t3d_vec3_distance2(&(T3DVec3){{cl->anchor[0][0], cl->anchor[0][1], cl->anchor[0][2]}},
                                         &(T3DVec3){{cl->anchor[1][0], cl->anchor[1][1], cl->anchor[1][2]}}));
    for (int r = 0; r < cl->rows; r++) cl->hrest[r] = top * (1.0f + sp->flare * r / (cl->rows - 1));
    for (int c = 0; c < cl->cols; c++) cl->vrest[c] = len / (cl->rows - 1);
    // (0 when the pins meet, as on the title's first frame, before the rig is posed: then exact)
    #define REST(l, v) ((l)[0] = (v), (l)[1] = (v) > 1e-3f ? 1.0f / ((v) * (v)) : 0.0f)
    #define VREST(c, r) (cl->vrest[c] * ((sp->tattered && (r) >= cl->rows - 4) ? TATTER[c] / 100.0f : 1.0f))
    for (int r = 0; r < cl->rows; r++) {
        REST(cl->hl[r], cl->hrest[r]);
        for (int c = 0; c < cl->cols; c++) {
            if (r < cl->rows - 1) REST(cl->vl[r][c], VREST(c, r));
            if (r < cl->rows - 2) REST(cl->bl[r][c], VREST(c, r) + VREST(c, r + 1));
        }
    }
    #undef VREST
    #undef REST
    for (int c = 0; c < cl->cols; c++) {
        float t = 0.0f;
        // a plume's from its stiff second row (held to it: there, it's pinned)
        for (int r = 1; r < cl->rows; r++) cl->tl[r][c] = t += (r > 1 || sp->kind != CLOTH_PLUME) ? cl->vl[r - 1][c][0] : 0.0f;
    }
    for (int r = 0; r < cl->rows; r++)
        for (int c = 0; c < cl->cols; c++) {
            float *p = PC(cl, c, r);
            p[0] = cl->anchor[c][0]; p[1] = cl->anchor[c][1] - cl->vrest[c] * r; p[2] = cl->anchor[c][2];
            memcpy(cl->q[r * cl->cols + c], p, sizeof(float[3]));
        }
    memset(cl->near, 0xFF, sizeof cl->near);   // all of the body, until a step says what's near
    cl->valid = true;
}

// A heavier hem (each row HEM heavier than the one above): it lags and swings, the top gives.
// A link's correction is shared by weight: wa the part a takes (0: a is pinned)
#define HEM 0.15f
#define SH(ra, rb) ((1.0f + HEM * (rb)) / (2.0f + HEM * ((ra) + (rb))))
// row r with r + 1 (the bend links, r with r + 2, use it too: within 4%, and the ROM keeps its size)
static const float SHARE1[CLOTH_ROWS] = { SH(0, 1), SH(1, 2), SH(2, 3), SH(3, 4), SH(4, 5), SH(5, 6), SH(6, 7), 0 };
_Static_assert(CLOTH_ROWS == 8, "SHARE1: a weight per row");

// rest: its length and 1/length^2; stiff: the part of the stretch taken out (1: all of it).
// Near its length, (len - rest) / len is taken as (len^2 / rest^2 - 1) / 2: no root or divide
// (58 cycles), as most links are within 10% of their length
static void link(float *a, float *b, const float rest[2], float wa, float stiff)
{
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    float l2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    float k = 0.5f * (l2 * rest[1] - 1.0f);
    if (k > 0.1f || k < -0.1f) {
        if (l2 < 1e-8f) return;
        float len = sqrtf(l2);
        k = (len - rest[0]) / len;
    }
    k *= stiff;
    float wb = 1.0f - wa;
    for (int i = 0; i < 3; i++) { a[i] += d[i] * k * wa; b[i] -= d[i] * k * wb; }
}

#ifdef HLE_LOG_CLOTH
static uint32_t s_bodyT, s_stepT, s_subT, s_colT, s_ancT, s_vtxT, s_matT, s_wbT;
#endif
// Pushes a particle out of one capsule; false when it's beyond the capsule's bounding sphere
static inline bool collide1(float *p, const Capsule *c)
{
    float mx = p[0] - c->m[0], my = p[1] - c->m[1], mz = p[2] - c->m[2];
    if (mx * mx + my * my + mz * mz >= c->br2) return false;
    float ap[3] = { p[0] - c->a[0], p[1] - c->a[1], p[2] - c->a[2] };
    float h = (ap[0] * c->ab[0] + ap[1] * c->ab[1] + ap[2] * c->ab[2]) * c->il2;
    h = h < 0 ? 0 : h > 1 ? 1 : h;
    float d[3] = { ap[0] - c->ab[0] * h, ap[1] - c->ab[1] * h, ap[2] - c->ab[2] * h };
    float dl2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (dl2 < c->r2 && dl2 > 1e-8f) {
        float dl = sqrtf(dl2), k = (c->r - dl) / dl;
        p[0] += d[0] * k; p[1] += d[1] * k; p[2] += d[2] * k;
    }
    return true;
}

// Behind the plane (hair over a cape stays behind it: point, normal), if any
static inline void behind(float *p, const float *plane)
{
    if (!plane) return;
    float d = (p[0] - plane[0]) * plane[3] + (p[1] - plane[1]) * plane[4] + (p[2] - plane[2]) * plane[5];
    if (d < 0) for (int m = 0; m < 3; m++) p[m] -= d * plane[3 + m];
}

// Out of the capsules in bits (the hot loop of the cloth), then behind the plane; returns those
// it's near (cloth_draw checks only these, from a step's)
static uint32_t push_out(float *p, const float *plane, uint32_t only)
{
    uint32_t nb = 0;
    for (int i = 0; only; i++, only >>= 1) if ((only & 1) && collide1(p, &s_body[i])) nb |= 1u << i;
    behind(p, plane);
    return nb;
}

// The blade's sweep since the last step (a step is 1/30 s: in a big swing it passed right through
// the cloth): a point it went through, a third or two thirds of the way, is carried on with it
// (and so keeps some of its speed): the cloth sways off the swing instead of the blade cutting it
static void swept(float *p)
{
    float m[3] = { p[0] - s_swM[0], p[1] - s_swM[1], p[2] - s_swM[2] };
    if (m[0] * m[0] + m[1] * m[1] + m[2] * m[2] >= s_swR2) return;
    const Capsule *k = &s_body[s_blade];
    for (int j = 1; j <= 2; j++) {
        float t = j * (1.0f / 3.0f), a[3], ab[3];
        for (int i = 0; i < 3; i++) { a[i] = s_bladeA0[i] + (k->a[i] - s_bladeA0[i]) * t; ab[i] = s_bladeB0[i] + (k->b[i] - s_bladeB0[i]) * t - a[i]; }
        float ap[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
        float h = (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) * k->il2;     // (the blade keeps its length)
        h = h < 0 ? 0 : h > 1 ? 1 : h;
        float d[3] = { ap[0] - ab[0] * h, ap[1] - ab[1] * h, ap[2] - ab[2] * h };
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] >= 4.0f * k->r2) continue;     // twice its width: the air it drives moves cloth too
        for (int i = 0; i < 3; i++) p[i] += ((k->a[i] - s_bladeA0[i]) * (1.0f - h) + (k->b[i] - s_bladeB0[i]) * h) * (1.0f - t);
        return;
    }
}

// floor: it stays above; ceil: a cloak in the air rises to it (his head), never above (on the ground: below its pins)
static void cloth_step(Cloth *cl, const ClothSpec *sp, float h, const float back[3], const float *plane, float floor, float ceil)
{
    const float k = GU * (1.0f / (GLTF * MODEL_SCALE));   // gravity and wind scaled with him, as his cape is (1 in the fights)
    const float g = -220.0f * k * h * h;
    cl->time += h;
    const float damp = 1.0f - (strands(sp) ? 2.4f : 0.9f) * h;   // per second, not per step (0.92 / 0.97 at 30 steps)
    float wind[CLOTH_COLS];     // a gust down each column (sinf is a software call: once a column, not a particle)
    for (int c = 0; c < cl->cols; c++) wind[c] = (14.0f + 10.0f * sinf(cl->time * 1.7f + c * 0.6f)) * k * h * h / cl->rows;
    // falling, the air lifts it, evenly (the hem pushed most, it folded), never past 0.8 of its weight
    // (stronger, it shot up and heaped at his head as he came down), and partly back: it floats up behind him
    const float lift = fminf(cl->fall * 1.6f, 176.0f) * k * h * h;
    for (int r = 1; r < cl->rows; r++)
        for (int c = 0; c < cl->cols; c++) {
            float *p = PC(cl, c, r), *q = cl->q[r * cl->cols + c];
            float gust = wind[c] * r;
            for (int i = 0; i < 3; i++) {
                float v = (p[i] - q[i]) * damp;
                q[i] = p[i];
                p[i] += v + back[i] * (gust + lift);
            }
            p[1] += g + lift;
        }
    const bool plume = sp->kind == CLOTH_PLUME;
    float (*tpin)[3] = plume ? cl->anchor2 : cl->anchor;
    for (int it = 0; it < CLOTH_ITERS; it++) {
        for (int c = 0; c < cl->cols; c++) {
            memcpy(PC(cl, c, 0), cl->anchor[c], sizeof(float[3]));
            if (sp->kind == CLOTH_PLUME) memcpy(PC(cl, c, 1), cl->anchor2[c], sizeof(float[3]));
        }
        for (int r = 0; r < cl->rows - 1; r++)
            for (int c = 0; c < cl->cols; c++)
                link(PC(cl, c, r), PC(cl, c, r + 1), cl->vl[r][c], (r == 0 || (r == 1 && sp->kind == CLOTH_PLUME)) ? 0.0f : SHARE1[r], 1.0f);
        // bend links: hair can't fold back on itself; a cloak resists it softly, on the first
        // pass, so it drapes and swings instead of crumpling when he stops
        if (strands(sp) || it == 0)
            for (int r = 0; r < cl->rows - 2; r++)
                for (int c = 0; c < cl->cols; c++)
                    link(PC(cl, c, r), PC(cl, c, r + 2), cl->bl[r][c], r == 0 ? 0.0f : SHARE1[r], strands(sp) ? 1.0f : 0.35f);
        int loose = sp->tattered ? cl->rows - 3 : cl->rows;   // tatters hang as free strands
        for (int r = 1; r < loose; r++)
            for (int c = 0; c < cl->cols - 1; c++) link(PC(cl, c, r), PC(cl, c + 1, r), cl->hl[r], 0.5f, (sp->kind == CLOTH_HAIR && r > 1) ? 0.5f : 1.0f);
#ifdef HLE_LOG_CLOTH
        uint32_t tc = TICKS_READ();
#endif
        // the last pass: the tethers (no point farther from its pin than its column's length, however
        // fast he moves; the root only where one's pulled out past it), then out of the body
        if (it == CLOTH_ITERS - 1)
            for (int r = 1; r < cl->rows; r++)
                for (int c = 0; c < cl->cols; c++) {
                    float *p = PC(cl, c, r), *a = tpin[c];
#ifndef HLE_NO_SWEEP   // test aid: the blade only where it is at the step
                    if (s_swing) swept(p);
#endif
                    float d[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] }, l2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], t = cl->tl[r][c];
                    if (l2 > t * t) { float k = t / sqrtf(l2); for (int i = 0; i < 3; i++) p[i] = a[i] + d[i] * k; }
                    cl->near[r * cl->cols + c] = push_out(p, plane, (1u << s_bodyCount) - 1);
                }
#ifdef HLE_LOG_CLOTH
        s_colT += TICKS_READ() - tc;
#endif
    }
    // Never above the pins (a cloak) or above the head (a plume streams back, it can't stand up)
    for (int r = 1; r < cl->rows; r++)
        for (int c = 0; c < cl->cols; c++) {
            float top = sp->kind == CLOTH_PLUME ? cl->anchor2[c][1] : cl->anchor[c][1];
            float maxY = top - (sp->kind == CLOTH_PLUME ? 0.02f : 0.15f) * r * cl->vrest[c];
            float *y = &PC(cl, c, r)[1];
            if (sp->kind == CLOTH_CLOAK) {   // in the air, up to his head (hard); landed, eased back under its pins (snapped, it crumpled)
                if (ceil > maxY) { if (*y > ceil) *y = ceil; }
                else if (*y > maxY) *y += (maxY - *y) * 0.35f;
            } else if (*y > maxY) *y = maxY;
            if (*y < floor) *y = floor;     // and never through the floor (a roll, a knockdown)
        }
    if (sp->kind == CLOTH_PLUME)   // the links above may have nudged its stiff root
        for (int c = 0; c < cl->cols; c++) memcpy(PC(cl, c, 1), cl->anchor2[c], sizeof(float[3]));
}

static inline void put_vert(T3DVertPacked *vb, int i, const int16_t pos[3], uint32_t rgba)
{
    T3DVertPacked *v = &vb[i >> 1];
    if (i & 1) { memcpy(v->posB, pos, 6); v->normB = 0; v->rgbaB = rgba; }
    else       { memcpy(v->posA, pos, 6); v->normA = 0; v->rgbaA = rgba; }
}

// shade() with the colour times the ambient light already worked out (tint, 0..255)
static inline uint32_t shade_tinted(const float tint[3], const float n[3], float bias)
{
    static const float L[3] = { 0.33f, 0.85f, 0.41f };
    float s = (0.3f + 0.7f * fabsf(n[0] * L[0] + n[1] * L[1] + n[2] * L[2])) * bias;
    int r = tint[0] * s, g = tint[1] * s, b = tint[2] * s;
    r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFF;
}

static uint32_t shade(SkinColor c, const float n[3], float bias)
{
    static const float L[3] = { 0.33f, 0.85f, 0.41f };
    float d = fabsf(n[0] * L[0] + n[1] * L[1] + n[2] * L[2]);
    float s = (0.3f + 0.7f * d) * bias;
    // t3d lights the knight's vertex colours by the ambient colour alone
    const float K = 1.0f / 255.0f;      // a multiply: a divide is 29 cycles, three a vertex
    float amb[3] = { colorAmbient[0] * K, colorAmbient[1] * K, colorAmbient[2] * K };
    int r = c.r * s * amb[0], g = c.g * s * amb[1], b = c.b * s * amb[2];
    r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFF;
}

#ifdef HLE_LOG_CLIP
static int s_clipN, s_frontN;
static float s_frontPl[6];      // the cloak's wall (cloth_draw counts points in front of it)
#endif
// The vertices are in the knight's model space, drawn with its matrix, built every frame (the
// cloth steps every other frame: see halfway below). o is the model's origin in the world, ax its unit axes there (scaled), s2 their |ax|^2.
// A cloak is drawn twice, culled: its outside, and its lining (darker) wound the other way
static void cloth_draw(Cloth *cl, const ClothSpec *sp, const T3DMat4FP *modelMat, const float o[3], const float ax[3][3], float s2, bool rebuild, const float *plane)
{
    const bool lined = !strands(sp);
    const int n = cl->cols * cl->rows;
    if (!cl->vb[0]) {
        for (int i = 0; i < RING; i++) {
            cl->vb[i] = malloc_uncached(sizeof(T3DVertPacked) * CLOTH_N);   // the outside, then the lining
        }
    }
    if (cl->trisGrid != cl->cols * 16 + cl->rows) {   // the grid's triangles never change: built once, not each frame
        gpu_wait();
        for (int k = 0; k < RING; k++) {
            if (cl->tris[k]) rspq_block_free(cl->tris[k]);
            gpu_block_begin();
            for (int side = 0; side < (lined ? 2 : 1); side++) {
                t3d_vert_load(cl->vb[k] + side * n / 2, 0, n);
                for (int r = 0; r < cl->rows - 1; r++)
                    for (int c = 0; c < cl->cols - 1; c++) {
                        int i = r * cl->cols + c;
                        if (side) { t3d_tri_draw(i, i + 1, i + cl->cols); t3d_tri_draw(i + 1, i + cl->cols + 1, i + cl->cols); }
                        else      { t3d_tri_draw(i, i + cl->cols, i + 1); t3d_tri_draw(i + 1, i + cl->cols, i + cl->cols + 1); }
                    }
                t3d_tri_sync();
            }
            cl->tris[k] = gpu_block_end();
        }
        cl->trisGrid = cl->cols * 16 + cl->rows;
    }
    // Built every frame, the cloth stepping every other one (a step costs 1.5 ms): a step frame
    // shows halfway from the last step (Verlet keeps it in q), the next frame the step itself.
    // The cape moves on evenly at 60, a frame behind its pins (they follow the body every frame).
    // Shown as stepped it moved at 30 against the body's 60: it juddered
    bool halfway = rebuild && cl->built >= 0;
    bool light = rebuild || cl->built < 0;     // lit on step frames (and the first)
    cl->ring = (cl->ring + 1) % RING;
    cl->built = cl->ring;
    // Written through the cache, then flushed for the RSP's DMA: uncached stores cost the
    // vertices 0.8 ms a frame
    T3DVertPacked *vb = CachedAddr(cl->vb[cl->ring]);

#ifdef HLE_LOG_CLOTH
    uint32_t tb = TICKS_READ();
#endif
    // What's drawn is pushed out of this frame's body: the body moves every frame, the cloth
    // every other one, and a point halfway between two outside it can be inside it. Only out of
    // the capsules it was near at the step: a frame or two on, nothing else has reached it
    float shown[CLOTH_N][3];
    for (int r = 0; r < cl->rows; r++)
        for (int c = 0; c < cl->cols; c++) {
            const float *p = PC(cl, c, r), *q = cl->q[r * cl->cols + c];
            float *v = shown[r * cl->cols + c];
            bool pin = r == 0 || (r == 1 && sp->kind == CLOTH_PLUME);
            for (int m = 0; m < 3; m++) v[m] = (halfway && !pin) ? 0.5f * (p[m] + q[m]) : p[m];
            // and out of the sword and arms, wherever they are: a swing reaches cloth they weren't near at the step
#ifdef HLE_ARMS_NEAR   // test aid: as before, only what was near at the step
            if (!pin) push_out(v, plane, cl->near[r * cl->cols + c]);
#else
            if (!pin) push_out(v, plane, cl->near[r * cl->cols + c] | s_armBits);
#endif
#ifdef HLE_LOG_CLIP    // test aid: drawn points still inside the sword or an arm (cloth through them)
            if (!pin) for (int b = 0; b < s_bodyCount; b++) if (s_armBits >> b & 1) {
                const Capsule *k = &s_body[b];
                float ap[3] = { v[0] - k->a[0], v[1] - k->a[1], v[2] - k->a[2] };
                float hh = (ap[0] * k->ab[0] + ap[1] * k->ab[1] + ap[2] * k->ab[2]) * k->il2; hh = hh < 0 ? 0 : hh > 1 ? 1 : hh;
                float d[3] = { ap[0] - k->ab[0] * hh, ap[1] - k->ab[1] * hh, ap[2] - k->ab[2] * hh };
                if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 0.81f * k->r2) { s_clipN++; break; }
            }
            if (!pin && sp->kind == CLOTH_CLOAK && (v[0] - s_frontPl[0]) * s_frontPl[3] + (v[2] - s_frontPl[2]) * s_frontPl[5] < -0.3f * GU) s_frontN++;
#endif
        }
    // Per vertex: no divides but the normal's (a divide is 29 cycles), the colour's tint once
    const float inv = 1.0f / s2, K = 1.0f / 255.0f;
    const float tint[3] = { sp->color.r * colorAmbient[0] * K, sp->color.g * colorAmbient[1] * K, sp->color.b * colorAmbient[2] * K };
    #define DC(c, r) shown[(r) * cl->cols + (c)]
    for (int r = 0; r < cl->rows; r++)
        for (int c = 0; c < cl->cols; c++) {
            int i = r * cl->cols + c;
            if (light) {
                const float *l = DC(c > 0 ? c - 1 : c, r), *rr = DC(c < cl->cols - 1 ? c + 1 : c, r);
                const float *u = DC(c, r > 0 ? r - 1 : r), *d = DC(c, r < cl->rows - 1 ? r + 1 : r);
                float a[3] = { rr[0] - l[0], rr[1] - l[1], rr[2] - l[2] }, b[3] = { d[0] - u[0], d[1] - u[1], d[2] - u[2] };
                float nv[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
                float nl = 1.0f / (sqrtf(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]) + 1e-6f);
                nv[0] *= nl; nv[1] *= nl; nv[2] *= nl;
                // strands get a sheen down the middle, cloth darkens toward the hem and in its pleats (the
                // odd columns, pinned further out: hanging smooth, the folds still read)
                float bias = sp->kind == CLOTH_HAIR ? ((c & 1) ? 0.72f : 1.05f) : strands(sp) ? 0.85f + 0.3f * (c == cl->cols / 2) : (1.0f - 0.25f * r / cl->rows) * ((c & 1) ? 0.72f : 1.0f);
                cl->col[i] = shade_tinted(tint, nv, bias);
            }
            const float *p = DC(c, r);
            float w[3] = { p[0] - o[0], p[1] - o[1], p[2] - o[2] };     // world -> model units
            int16_t pos[3];
            for (int m = 0; m < 3; m++) pos[m] = (int16_t)((w[0] * ax[m][0] + w[1] * ax[m][1] + w[2] * ax[m][2]) * inv);
            put_vert(vb, i, pos, cl->col[i]);
            // the lining: 7/8 of the outside, each channel (no carries across: the low bits masked);
            // at 5/8 a fold or a lift showed it as black shards, not the same cloth
            if (lined) put_vert(vb, n + i, pos, (cl->col[i] - (cl->col[i] >> 3 & 0x1F1F1F00)) | 0xFF);
        }
    #undef DC
#ifdef HLE_LOG_CLOTH
    uint32_t tw = TICKS_READ();
#endif
    data_cache_hit_writeback(vb, sizeof(T3DVertPacked) * (lined ? n : n / 2));
#ifdef HLE_LOG_CLOTH
    s_wbT += TICKS_READ() - tw;
    s_vtxT += TICKS_READ() - tb;
#endif

#ifdef HLE_LOG_CLOTH
    uint32_t tv = TICKS_READ();
#endif
    rdpq_sync_pipe();
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    rdpq_mode_blender(0);
    rdpq_mode_zbuf(true, true);
    rdpq_mode_alphacompare(0);
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT | (lined ? T3D_FLAG_CULL_FRONT : 0));
    t3d_matrix_set(modelMat, true);
    rspq_block_run(cl->tris[cl->built]);
#ifdef HLE_LOG_CLOTH
    s_subT += TICKS_READ() - tv;
#endif
}

/* -----------------------------------------------------------------------------
 * Hood: static shells built once per skin, drawn on a bone
 * -------------------------------------------------------------------------- */

#define HOOD_MAX_VERTS 70
static T3DVertPacked *s_hoodVerts;
static rspq_block_t *s_hoodBlock;

// Ellipsoid patch, theta around Y from +X (the back), phi up from the equator.
// peak > 0 drags the vertices nearest the back-top out into the hood's point;
// lip > 0 pushes the front vertices at the brow out and down into an overhang.
__attribute__((noinline, optimize("Os"))) static int shell(T3DVertPacked *vb, const float c[3], const float rad[3], float t0, float t1, int nt,
                 float p0, float p1, int np, float peak, float lip, SkinColor col)
{
    static const float BT[3] = { 0.64f, 0.77f, 0.0f };
    for (int j = 0; j < np; j++)
        for (int i = 0; i < nt; i++) {
            float th = t0 + (t1 - t0) * i / (nt - 1), ph = p0 + (p1 - p0) * j / (np - 1);
            float d[3] = { cosf(ph) * cosf(th), sinf(ph), cosf(ph) * sinf(th) };
            float k = d[0] * BT[0] + d[1] * BT[1];
            float pk = k > 0 ? peak * k * k * k * k : 0;
            float lp = (lip > 0 && d[0] < -0.5f && ph > 0.0f && ph < 0.35f) ? lip : 0;
            float fl = (lip > 0 && d[0] > 0 && d[1] < 0) ? 0.9f * -d[1] * d[0] : 0;   // hood back drapes onto the cloak
            int16_t pos[3] = {
                (int16_t)((c[0] + rad[0] * d[0] + BT[0] * pk + d[0] * lp + fl) * GLTF),
                (int16_t)((c[1] + rad[1] * d[1] + BT[1] * pk - lp * 0.5f) * GLTF),
                (int16_t)((c[2] + rad[2] * d[2] + d[2] * lp) * GLTF),
            };
            put_vert(vb, j * nt + i, pos, shade(col, d, lp > 0 ? 0.8f : 1.0f));
        }
    return nt * np;
}

// Quads facing the front (-X) below phiOpen are left out: the face opening
static void shell_tris(int nt, int np, float t0, float t1, float p0, float p1, float phiOpen)
{
    for (int j = 0; j < np - 1; j++)
        for (int i = 0; i < nt - 1; i++) {
            float th = t0 + (t1 - t0) * (i + 0.5f) / (nt - 1), ph = p0 + (p1 - p0) * (j + 0.5f) / (np - 1);
            if (cosf(th) < -0.55f && ph < phiOpen) continue;
            int a = j * nt + i;
            t3d_tri_draw(a, a + nt, a + 1);
            t3d_tri_draw(a + 1, a + nt, a + nt + 1);
        }
}

static uint8_t s_hoodAmb[3];    // the ambient the hood's colours were baked with

static void hood_build(void)
{
    memcpy(s_hoodAmb, colorAmbient, sizeof s_hoodAmb);
    if (s_hoodBlock) { gpu_wait(); rspq_block_free(s_hoodBlock); s_hoodBlock = NULL; }   // may still be queued
    const SkinDef *s = &SKINS[s_skin];
    if (!s->hood) return;
    if (!s_hoodVerts) s_hoodVerts = malloc_uncached(sizeof(T3DVertPacked) * HOOD_MAX_VERTS / 2);

    // over the helmet and forehead, open at the face
    const float R = (float)M_PI / 180.0f;
    const float t0 = -180 * R, t1 = 180 * R, p0 = -52 * R, p1 = 90 * R, open = 12 * R;
    const int nt = 11, np = 6;
    int n = shell(s_hoodVerts, (float[3]){ -0.34f, 1.07f, 0 }, (float[3]){ 1.1f, 1.1f, 0.86f },
                  t0, t1, nt, p0, p1, np, 0.45f, 0.14f, s->cl[0].color);
    gpu_block_begin();
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    rdpq_mode_blender(0);
    rdpq_mode_zbuf(true, true);
    rdpq_mode_alphacompare(0);
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT);
    t3d_vert_load(s_hoodVerts, 0, n);
    shell_tris(nt, np, t0, t1, p0, p1, open);
    t3d_tri_sync();
    s_hoodBlock = gpu_block_end();
}

/* -----------------------------------------------------------------------------
 * Accessories: proc meshes in a bone's space (model units, glTF x 64)
 * -------------------------------------------------------------------------- */

#define ACC_MAX 4
static struct { ProcMesh m; int bone; } s_acc[ACC_MAX];
static int s_accCount;

static void G(float out[3], float x, float y, float z) { out[0] = x * GLTF; out[1] = y * GLTF; out[2] = z * GLTF; }
static inline uint32_t rgb(SkinColor c) { return proc_rgb(c.r, c.g, c.b); }

// A flat shape given as a strip of point pairs (both faces lit)
static void strip(const float (*l)[3], const float (*r)[3], int n, uint32_t col)
{
    for (int i = 0; i < n - 1; i++) proc_mesh_quad(l[i], r[i], r[i + 1], l[i + 1], col, false);
}

__attribute__((noinline, optimize("Os"))) static void acc_winged_helm(SkinColor gold)
{
    // Malenia's crest: a tall wing of feathers rising off the helm and swept back,
    // in two leaves splayed a little so it reads from behind as well
    for (int side = -1; side <= 1; side += 2)
        for (int k = 0; k < 7; k++) {
            float a = (96.0f - k * 9.0f) * (float)M_PI / 180.0f, len = 1.3f - 0.075f * k, w = 0.22f - 0.01f * k;
            float bx = -0.72f + 0.13f * k, ox = bx + 0.34f, by = 1.07f + sqrtf(0.81f - ox * ox) - 0.3f;   // rooted in the dome
            float dx = cosf(a), dy = sinf(a), sz = side * 0.08f;     // a slight V, to read from behind
            float l[4][3], r[4][3];
            for (int i = 0; i < 4; i++) {
                float t = i / 3.0f, ww = w * (1.0f - 0.7f * t), bend = 0.28f * t * t;   // tips curl back
                float px = bx + dx * len * t + bend, py = by + dy * len * t, pz = side * 0.05f + sz * t * len;
                G(l[i], px - dy * ww, py + dx * ww, pz); G(r[i], px + dy * ww, py - dx * ww, pz);
            }
            SkinColor c = gold; float f = 0.85f + 0.05f * k;
            c.r = (uint8_t)fminf(255, c.r * f); c.g = (uint8_t)fminf(255, c.g * f); c.b = (uint8_t)fminf(255, c.b * f);
            strip(l, r, 4, rgb(c));
        }
}

__attribute__((noinline, optimize("Os"))) static void acc_nightfarer_helm(SkinColor silver)
{
    // Wings on both temples, swept up and back like a Norse helm's: five feathers a side,
    // flaring out from the helm a little
    for (int side = -1; side <= 1; side += 2)
        for (int k = 0; k < 5; k++) {
            float a = (62.0f - k * 12.0f) * (float)M_PI / 180.0f, len = 1.3f - 0.11f * k, w = 0.26f - 0.02f * k;
            float bx = -0.15f + 0.12f * k, by = 1.15f + 0.05f * k, bz = side * 0.82f;
            float dx = cosf(a), dy = sinf(a);
            // each feather's face tilted 45 degrees outward, so the wing reads from behind too
            const float ci = 0.7071f, so = 0.7071f;
            float l[4][3], r[4][3];
            for (int i = 0; i < 4; i++) {
                float t = i / 3.0f, ww = w * (1.0f - 0.6f * t), bend = 0.35f * t * t;
                float px = bx + dx * len * t + bend, py = by + dy * len * t, pz = bz + side * 0.6f * t;
                float wx = -dy * ww * ci, wy = dx * ww * ci, wz = side * ww * so;
                G(l[i], px + wx, py + wy, pz + wz); G(r[i], px - wx, py - wy, pz - wz);
            }
            SkinColor c = silver; float f = 0.8f + 0.06f * k;
            c.r = (uint8_t)fminf(255, c.r * f); c.g = (uint8_t)fminf(255, c.g * f); c.b = (uint8_t)fminf(255, c.b * f);
            strip(l, r, 4, rgb(c));
        }
}

__attribute__((noinline, optimize("Os"))) static void acc_claw_shot(void)
{
    // A grapnel on the forearm, halfway to the wrist: a steel housing and three claws at its
    // wrist end (the forearm's direction comes from the rig: the hand's rest offset)
    const T3DChunkBone *hand = &s_skel->skeletonRef->bones[s_bone[B_HAND_L]];
    float v[3] = { hand->position.v[0] / GLTF, hand->position.v[1] / GLTF, hand->position.v[2] / GLTF };
    int ax = fabsf(v[0]) > fabsf(v[1]) ? (fabsf(v[0]) > fabsf(v[2]) ? 0 : 2) : (fabsf(v[1]) > fabsf(v[2]) ? 1 : 2);
    float c[3] = { v[0] * 0.5f * GLTF, v[1] * 0.5f * GLTF, v[2] * 0.5f * GLTF };
    float h[3] = { 0.24f * GLTF, 0.24f * GLTF, 0.24f * GLTF };
    h[ax] = fabsf(v[ax]) * 0.28f * GLTF;
    c[(ax + 1) % 3] += 0.28f * GLTF;        // on the outside of the arm
    proc_mesh_box(c, h, proc_rgb(110, 114, 126));
    for (int k = -1; k <= 1; k++) {         // the claws, pointing toward the wrist
        float base[3] = { c[0], c[1], c[2] }, tip[3];
        base[ax] += (v[ax] > 0 ? 1 : -1) * h[ax];
        base[(ax + 2) % 3] += k * 0.14f * GLTF;
        memcpy(tip, base, sizeof tip);
        tip[ax] += (v[ax] > 0 ? 1 : -1) * 0.32f * GLTF;
        tip[(ax + 1) % 3] += 0.08f * GLTF;
        proc_mesh_spike(base, tip, 0.07f * GLTF, 4, proc_rgb(150, 154, 166));
    }
}

// A mantle in the cloak's colour: from close round the neck, down over the shoulders and across
// the chest, its back over the cloak's pins (one piece with it). A hem dipping in front; ragged if
// the cloak is. Static, in the spine's space: no cloth cost
__attribute__((noinline, optimize("Os"))) static void acc_mantle(const ClothSpec *cloak)
{
    enum { NT = 16 };
    float top[NT + 1][3], mid[NT + 1][3], hem[NT + 1][3];
    for (int i = 0; i <= NT; i++) {
        float th = -(float)M_PI + 2.0f * (float)M_PI * i / NT, c = cosf(th), sn = sinf(th);   // th 0: the back (+X)
        float front = c < 0 ? c * c : 0.0f;                    // 1 at the chest
        float rag = (cloak->tattered && (i & 1)) ? 0.14f : 0.0f;
        G(top[i], -0.12f + 0.58f * c, 2.78f - 0.12f * front, 0.64f * sn);    // below the visor
        G(mid[i], -0.08f + 0.9f * c, 2.6f - 0.1f * front, 1.06f * sn);     // over the shoulders, where the cloak pins
        G(hem[i], -0.06f + 0.84f * c, 2.2f - 0.26f * front - rag, 1.0f * sn);   // tucked in onto the body: flush
    }
    SkinColor k = cloak->color;   // as dark as the lit cloak (shaded, it's darker than its colour)
    k.r = k.r * 4 / 5; k.g = k.g * 4 / 5; k.b = k.b * 4 / 5;
    uint32_t col = rgb(k);
    k.r = k.r * 3 / 4; k.g = k.g * 3 / 4; k.b = k.b * 3 / 4;   // the neck's fold a shade darker
    uint32_t fold = rgb(k);
    for (int i = 0; i < NT; i++) {
        proc_mesh_quad(top[i], top[i + 1], mid[i + 1], mid[i], fold, false);
        proc_mesh_quad(mid[i], mid[i + 1], hem[i + 1], hem[i], col, false);
    }
}

__attribute__((noinline, optimize("Os"))) static void acc_berserk_helm(SkinColor steel)
{
    // Spined fins swept back off the helm, and the burning eyes of the Berserker armour
    uint32_t c = rgb(steel);
    static const float FINS[][6] = {
        { -0.75f, 1.85f, 0.0f,  1.35f, 2.0f, 0.0f },
        { -0.35f, 1.78f, 0.26f, 1.35f, 1.72f, 0.4f },  { -0.35f, 1.78f, -0.26f, 1.35f, 1.72f, -0.4f },
        { -0.05f, 1.45f, 0.5f,  1.25f, 1.32f, 0.6f },  { -0.05f, 1.45f, -0.5f, 1.25f, 1.32f, -0.6f },
    };
    for (unsigned i = 0; i < sizeof FINS / sizeof FINS[0]; i++) {
        float b[3], t[3];
        G(b, FINS[i][0], FINS[i][1], FINS[i][2]); G(t, FINS[i][3], FINS[i][4], FINS[i][5]);
        proc_mesh_spike(b, t, (i == 0 ? 0.22f : 0.16f) * GLTF, 4, c);
    }
    for (int side = -1; side <= 1; side += 2) {     // slanted slits, a hair in front of the visor
        float q[4][3];
        G(q[0], -1.33f, 0.79f, side * 0.07f); G(q[1], -1.33f, 0.86f, side * 0.07f);
        G(q[2], -1.31f, 0.9f, side * 0.36f);  G(q[3], -1.31f, 0.83f, side * 0.36f);
        proc_mesh_quad(q[0], q[1], q[2], q[3], proc_rgb(255, 196, 40), true);
    }
}

__attribute__((noinline, optimize("Os"))) static void acc_dragonslayer(SkinColor iron)
{
    // The Dragonslayer: a broad straight slab, silver bevelled edges round a black core on
    // both faces, a chisel point (the black ends in a chevron short of it), a crescent guard,
    // a wrapped grip and a pommel cap. The stock sword runs from the pommel (1.36, 0.65, 0.2)
    // to the tip (-8.15, 1.05, -1.0) in the hand's space, its flat across y; this one ends
    // within the sword's hit length (the same for every skin).
    float o[3] = { 0.2f, 0.66f, 0.15f }, u[3] = { -9.5f, 0.4f, -1.2f };
    float ul = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (int i = 0; i < 3; i++) u[i] /= ul;
    float w[3] = { -u[0] * u[1], 1.0f - u[1] * u[1], -u[2] * u[1] };   // y without its u part
    float wl = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    for (int i = 0; i < 3; i++) w[i] /= wl;
    float t[3] = { u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0] };
    // a point at (along, across, thickness) in the blade's frame
    #define BP(out, s0, s1, s2) for (int m_ = 0; m_ < 3; m_++) (out)[m_] = (o[m_] + u[m_] * (s0) + w[m_] * (s1) + t[m_] * (s2)) * GLTF
    // oriented box: along u from a to b, half widths hw (w) and ht (t)
    #define OBOX(a, b, hw, ht, col) do { \
        float v[8][3]; \
        for (int k = 0; k < 8; k++) { BP(v[k], (k & 1) ? (b) : (a), (k & 2) ? (hw) : -(hw), (k & 4) ? (ht) : -(ht)); } \
        static const int F[6][4] = { {0,1,3,2}, {4,6,7,5}, {0,4,5,1}, {2,3,7,6}, {0,2,6,4}, {1,5,7,3} }; \
        for (int f = 0; f < 6; f++) proc_mesh_quad(v[F[f][0]], v[F[f][1]], v[F[f][2]], v[F[f][3]], col, false); } while (0)
    const uint32_t steel = proc_rgb(178, 178, 176), core = proc_rgb(22, 22, 24), dark = proc_rgb(40, 30, 26);
    const uint32_t edge = s_bladeEdge.set ? proc_rgb(s_bladeEdge.r, s_bladeEdge.g, s_bladeEdge.b) : steel;     // the equipped sword's colour on its silver (weapon.c)
    (void)iron;
    OBOX(-1.15f, -0.8f, 0.2f, 0.2f, steel);                    // pommel cap
    OBOX(-0.8f, 0.7f, 0.14f, 0.14f, dark);                     // wrapped grip
    OBOX(0.7f, 1.0f, 0.62f, 0.2f, steel);                      // crescent guard: a broad bar
    OBOX(0.72f, 0.98f, 0.42f, 0.24f, steel);                   //   swelling at the middle

    // The blade: L long to the shoulder of the point, P to its tip; HW wide, TH thick at the
    // spine, the edges bevelled down to a third of it
    const float S0 = 1.0f, L = 7.6f, P = 8.9f, HW = 0.52f, CW = 0.30f, TH = 0.11f, ET = TH * 0.35f;
    for (int f = -1; f <= 1; f += 2) {          // each face
        float a0[3], a1[3], b0[3], b1[3], c0[3], c1[3], tip[3], ch[3];
        for (int sd = -1; sd <= 1; sd += 2) {   // the silver bevels, either side of the core
            BP(a0, S0, sd * HW, f * ET); BP(a1, L, sd * HW, f * ET);
            BP(b0, S0, sd * CW, f * TH); BP(b1, L, sd * CW, f * TH);
            proc_mesh_quad(a0, a1, b1, b0, edge, false);
            BP(c0, L, sd * HW, f * ET); BP(tip, P, 0, 0); BP(ch, L + 0.75f, 0, f * TH);
            BP(c1, L, sd * CW, f * TH);
            proc_mesh_quad(c0, tip, ch, c1, edge, false);      // the point's bevel
        }
        // the black core, a hair proud of the face, ending in a chevron
        BP(b0, S0, -CW, f * (TH + 0.01f)); BP(b1, L, -CW, f * (TH + 0.01f));
        BP(c0, L, CW, f * (TH + 0.01f));   BP(c1, S0, CW, f * (TH + 0.01f));
        proc_mesh_quad(b0, b1, c0, c1, core, false);
        BP(ch, L + 0.75f, 0, f * (TH + 0.01f));
        proc_mesh_tri(b1, ch, c0, core, false);
    }
    for (int sd = -1; sd <= 1; sd += 2) {       // the edges themselves, and up to the point
        float a0[3], a1[3], b0[3], b1[3], tip[3];
        BP(a0, S0, sd * HW, ET); BP(a1, L, sd * HW, ET); BP(b0, S0, sd * HW, -ET); BP(b1, L, sd * HW, -ET);
        proc_mesh_quad(a0, a1, b1, b0, edge, false);
        BP(tip, P, 0, 0);
        proc_mesh_tri(a1, tip, b1, edge, false);
    }
    #undef OBOX
    #undef BP
}

static void acc_build(void)
{
    for (int i = 0; i < s_accCount; i++) proc_mesh_free(&s_acc[i].m);   // waits for the RSP
    s_accCount = 0;
    const SkinDef *s = &SKINS[s_skin];
    #define ACC(flag, bn, ...) if ((s->acc & (flag)) && s_accCount < ACC_MAX) { \
        proc_mesh_begin(); __VA_ARGS__; proc_mesh_end(&s_acc[s_accCount].m); \
        proc_mesh_set_transform(&s_acc[s_accCount].m, 1.0f, (float[3]){ 0, 0, 0 }, (float[3]){ 0, 0, 0 }); \
        s_acc[s_accCount++].bone = bn; }
    ACC(ACC_WINGED_HELM, B_HEAD, acc_winged_helm(s->helm));
    ACC(ACC_BERSERK_HELM, B_HEAD, acc_berserk_helm(s->helm));
    ACC(ACC_DRAGONSLAYER, B_HAND_R, acc_dragonslayer((SkinColor)C(70, 68, 70)));
    ACC(ACC_NIGHTFARER_HELM, B_HEAD, acc_nightfarer_helm(s->helm));
    ACC(ACC_MANTLE, B_SPINE, acc_mantle(&s->cl[0]));
    ACC(ACC_CLAW_SHOT, B_FORE_L, acc_claw_shot());
    #undef ACC
}

static void acc_draw(const T3DSkeleton *skel, const T3DMat4FP *modelMat)
{
    if (!s_accCount) return;
    rdpq_sync_pipe();
    // Save the scene's render mode and restore it after: a reset mode here turned the
    // title's fog off for whatever drew next (the fog gate dimmed with these skins)
    rdpq_mode_push();
    rdpq_set_mode_standard();
    rdpq_mode_zbuf(true, true);
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT);
    for (int i = 0; i < s_accCount; i++) {
        t3d_matrix_set(modelMat, true);
        t3d_matrix_push(&skel->boneMatricesFP[s_bone[s_acc[i].bone]]);
        t3d_matrix_push(s_acc[i].m.mat);     // proc_mesh's 1/8 units
        rspq_block_run(s_acc[i].m.block);
        t3d_matrix_pop(2);
    }
    rdpq_mode_pop();
}

/* -----------------------------------------------------------------------------
 * Recording / drawing
 * -------------------------------------------------------------------------- */

// userData: bit 0 hides the plume (cloth2), bit 1 the sword
static bool filter_parts(void *user, const T3DObject *obj)
{
    uintptr_t hide = (uintptr_t)user;
    const char *n = obj->material ? obj->material->name : NULL;
    if (!n) return true;
    if ((hide & 1) && strcmp(n, "cloth2") == 0) return false;
    if ((hide & 2) && strcmp(n, "sword") == 0) return false;
    return true;
}

static void tint(const char *matName, SkinColor c, color_t *saved)
{
    T3DMaterial *m = t3d_model_get_material(s_model, matName);
    if (!m) return;
    *saved = m->primColor;
    if (c.set) m->primColor = RGBA32(c.r, c.g, c.b, 255);
}

rspq_block_t* character_skin_record(const T3DModel *model, const T3DSkeleton *skel)
{
    s_model = model; s_skel = skel;
    for (int i = 0; i < B_COUNT; i++) s_bone[i] = t3d_skeleton_find_bone((T3DSkeleton*)skel, BONE_NAMES[i]);
    const SkinDef *s = &SKINS[s_skin];

    // Tint, record, then restore: the Fallen Knight draws this model live with stock colours
    static const char *MATS[] = { "knight", "head", "cloth", "cloth2", "chainmail", "sword" };
    const SkinColor cols[] = { s->armor, s->helm, s->cloth, s->plume, s->chain, s_bladeTint.set ? s_bladeTint : s->sword };
    enum { NM = sizeof MATS / sizeof MATS[0] };
    color_t saved[NM];
    for (int i = 0; i < NM; i++) tint(MATS[i], cols[i], &saved[i]);
    // The equipped sword's blade (weapon.c) lit by itself, its texture times its colour: it glows in the dark
    // halls without a new glow (the Lunar Edge's moonlit blue); restored after the record, as the tints
    T3DMaterial *swm = s_bladeTint.set && s_bladeGlow ? t3d_model_get_material(model, "sword") : NULL;
    uint64_t swCombiner = swm ? swm->colorCombiner : 0;
#ifdef HLE_AUTOPLAY
    if (swm) debugf("[blade] sword material: texture %s, combiner %016llx\n", swm->textureA.texPath ? swm->textureA.texPath : "(none)", (unsigned long long)swCombiner);
#endif
    if (swm && swm->textureA.texPath) swm->colorCombiner = RDPQ_COMBINER1((TEX0, 0, PRIM, 0), (0, 0, 0, 1));
    uintptr_t hide = ((s->hidePlume || s->hood) ? 1 : 0) | (s->hideSword ? 2 : 0);
    gpu_block_begin();
    t3d_model_draw_custom(model, (T3DModelDrawConf){
        .userData = (void*)hide,
        .filterCb = hide ? filter_parts : NULL,
        .matrices = gpu_skeleton_matrices(),     // this frame's copy (character_draw)
    });
    rspq_block_t *block = gpu_block_end();
    for (int i = 0; i < NM; i++) {
        T3DMaterial *m = t3d_model_get_material(model, MATS[i]);
        if (m) m->primColor = saved[i];
    }
    if (swm) swm->colorCombiner = swCombiner;
    hood_build();
    acc_build();
    character_skin_reset_cloth();
    return block;
}

void character_skin_bind_block(rspq_block_t **block) { s_block = block; }

static void rerecord(void)
{
    if (!s_block || !*s_block || !s_model) return;
    gpu_wait();   // the old block may still be queued for this frame
    rspq_block_free(*s_block);
    *s_block = character_skin_record(s_model, s_skel);
}

void character_skin_set_blade(bool set, bool glow, uint8_t r, uint8_t g, uint8_t b, uint8_t er, uint8_t eg, uint8_t eb)
{
    SkinColor c = { r, g, b, set }, e = { er, eg, eb, set };
    if (!memcmp(&c, &s_bladeTint, sizeof c) && !memcmp(&e, &s_bladeEdge, sizeof e) && glow == s_bladeGlow) return;
    s_bladeTint = c; s_bladeEdge = e; s_bladeGlow = glow;
    rerecord();
}

void character_skin_set(int id)
{
    if (id < 0 || id >= SKIN_COUNT) id = SKIN_KNIGHT;
    if (id == s_skin) return;
    s_skin = id;
    rerecord();
}

void character_skin_draw_extras(const T3DSkeleton *skel, const T3DMat4FP *modelMat, const float pos[3], float ground, float dt)
{
    const SkinDef *s = &SKINS[s_skin];
    if (!s_model || skel != s_skel) return;

#ifdef HLE_PROF_SERIAL   // test aid: the model, hood, accessories and cloth, each run alone
    static uint32_t s_kp[4], s_kpN;
    s_kp[0] += gpu_prof_split();
    if (++s_kpN == 120) { debugf("[knightgpu] model %.2f hood %.2f accessories %.2f cloth %.2f\n", TICKS_TO_US(s_kp[0] / 120) / 1000.0, TICKS_TO_US(s_kp[1] / 120) / 1000.0, TICKS_TO_US(s_kp[2] / 120) / 1000.0, TICKS_TO_US(s_kp[3] / 120) / 1000.0); memset(s_kp, 0, sizeof s_kp); s_kpN = 0; }
#endif
    // Baked under another light (the boot records the skin before any scene sets one: a black hood)
    if (s_hoodBlock && memcmp(s_hoodAmb, colorAmbient, sizeof s_hoodAmb)) hood_build();
    if (s_hoodBlock) {
        t3d_matrix_set(modelMat, true);
        t3d_matrix_push(&skel->boneMatricesFP[s_bone[B_HEAD]]);
        rspq_block_run(s_hoodBlock);
        t3d_matrix_pop(1);
    }
#ifdef HLE_PROF_SERIAL
    s_kp[1] += gpu_prof_split();
#endif
    acc_draw(skel, modelMat);
#ifdef HLE_PROF_SERIAL
    s_kp[2] += gpu_prof_split();
#endif
    if (s->cl[0].kind == CLOTH_NONE) return;

#ifdef HLE_LOG_CLOTH   // test aid: the cloth's CPU time a frame (CPU cycles: on an interpreter, its cost too)
    uint32_t t0 = TICKS_READ();
#endif
    static bool odd;        // the cloth steps every other frame (both pieces together)
    odd = !odd;
    bool step = odd && dt > 0.0f;
    bones_cache(modelMat);
#ifdef HLE_LOG_CLOTH
    s_matT += TICKS_READ() - t0;
#endif
    cloth_bodies();     // every frame: what's drawn is pushed out of it too (cloth_draw)
#ifdef HLE_LOG_CLOTH
    s_bodyT += TICKS_READ() - t0;
#endif
    // "Back" is the model's +X axis in world space
    float o[3], x1[3], back[3];
    mat4fp_mul_point_f32_row3_colbasis(modelMat, (float[3]){ 0, 0, 0 }, o);
    mat4fp_mul_point_f32_row3_colbasis(modelMat, (float[3]){ GLTF, 0, 0 }, x1);
    for (int i = 0; i < 3; i++) back[i] = (x1[i] - o[i]) / GU;
    float ax[3][3];     // the model's unit axes in the world, for world -> model units (uniform scale)
    for (int a = 0; a < 3; a++) {
        mat4fp_mul_point_f32_row3_colbasis(modelMat, (float[3]){ a == 0, a == 1, a == 2 }, ax[a]);
        for (int m = 0; m < 3; m++) ax[a][m] -= o[m];
    }
    float s2 = ax[0][0] * ax[0][0] + ax[0][1] * ax[0][1] + ax[0][2] * ax[0][2];
    if (dt > 0.05f) dt = 0.05f;

    bool cape = s->cl[0].kind == CLOTH_CLOAK || s->cl[1].kind == CLOTH_CLOAK;
    const Capsule *bk = &s_body[s_blade];
    if (step) {     // the blade's sweep since the last step: its bounding sphere (swept), none after a teleport
        float sw = 0.0f;
        for (int i = 0; i < 3; i++) {
            s_swM[i] = (s_bladeA0[i] + s_bladeB0[i] + bk->a[i] + bk->b[i]) * 0.25f;
            float e = bk->b[i] - s_bladeB0[i]; sw += e * e;
        }
        float r = 0.0f;
        const float *pts[4] = { s_bladeA0, s_bladeB0, bk->a, bk->b };
        for (int j = 0; j < 4; j++) { float e2 = 0; for (int i = 0; i < 3; i++) e2 += (pts[j][i] - s_swM[i]) * (pts[j][i] - s_swM[i]); r = fmaxf(r, e2); }
        r = sqrtf(r) + 2.0f * bk->r;
        s_swR2 = r * r;
        s_swing = s_bladeOk && sw < 60.0f * 60.0f;
    }
    float ceil = -1e9f;     // in the air (a jump, Moon Blade Dance): a cloak may rise to the top of his head
    if (pos[1] > ground + 1.0f) { float hd[3]; bone_world(B_HEAD, -0.34f, 1.97f, 0.0f, hd); ceil = hd[1]; }
    for (int k = 0; k < 2; k++) {
        const ClothSpec *sp = &s->cl[k];
        Cloth *cl = &s_cloth[k];
        if (sp->kind == CLOTH_NONE) continue;
        cl->cols = sp->kind == CLOTH_CLOAK ? CLOTH_COLS : sp->kind == CLOTH_HAIR ? 5 : 2;     // a mane: five locks; plumes: two strands
        cl->rows = sp->kind == CLOTH_HAIR ? 6 : CLOTH_ROWS;     // the mane is short
#ifdef HLE_LOG_CLOTH
        uint32_t ta = TICKS_READ();
#endif
        float prev[CLOTH_COLS][3]; memcpy(prev, cl->anchor, sizeof prev);
        float *prev0 = prev[0];
        cloth_anchors(cl, sp);
        float dx = cl->anchor[0][0] - prev0[0], dy = cl->anchor[0][1] - prev0[1], dz = cl->anchor[0][2] - prev0[2];
        cl->fall = dt > 0.0f && dy < 0.0f ? fminf(-dy / dt, 200.0f) : 0.0f;   // (a teleport: not a fall)
        if (!cl->valid || dx * dx + dy * dy + dz * dz > 40.0f * 40.0f) { cloth_reset(cl, sp); cl->acc = 0.0f; cl->built = -1; }   // first frame or teleport
        else {      // carried partly with its pins (the run and the turns): it streams back at a slant, not flat out
            const float follow = sp->kind == CLOTH_CLOAK ? 0.55f : 0.35f;
            for (int c = 0; c < cl->cols; c++) {
                float mx = (cl->anchor[c][0] - prev[c][0]) * follow, mz = (cl->anchor[c][2] - prev[c][2]) * follow;
                for (int r = 1; r < cl->rows; r++) {
                    float *p = PC(cl, c, r), *q = cl->q[r * cl->cols + c];
                    p[0] += mx; p[2] += mz; q[0] += mx; q[2] += mz;
                }
            }
        }
#ifdef HLE_LOG_CLOTH
        s_ancT += TICKS_READ() - ta;
#endif
        // hair over a cape: kept behind a plane just outside the cape's top edge
        float plane[6], *pl = NULL;
        if (strands(sp) && cape) {
            bone_world(B_SPINE, 1.12f, 2.62f, 0.0f, plane);
            float hl = sqrtf(back[0] * back[0] + back[2] * back[2]) + 1e-6f;
            plane[3] = back[0] / hl; plane[4] = 0.0f; plane[5] = back[2] / hl;
            pl = plane;
        }
        const bool hair = pl != NULL;
        // a cloak: kept behind him, or at most at his sides (it swung forward through him in attacks):
        // behind a wall through his spine, square to it (it turns with his torso)
        if (sp->kind == CLOTH_CLOAK) {
            const float *sx = s_bw[B_SPINE][0];
            float hl = sqrtf(sx[0] * sx[0] + sx[2] * sx[2]) + 1e-6f;
            memcpy(plane, s_bw[B_SPINE][3], sizeof(float[3]));
            plane[3] = sx[0] / hl; plane[4] = 0.0f; plane[5] = sx[2] / hl;
#ifdef HLE_LOG_CLIP
            memcpy(s_frontPl, plane, sizeof s_frontPl);
#endif
#ifndef HLE_CAPE_FRONT   // test aid: as before, free to swing forward
            pl = plane;
#endif
        }
#ifdef HLE_LOG_CLOTH
        uint32_t ts = TICKS_READ();
#endif
        // A step every other frame, of both frames' time
        cl->acc += dt;
        cl->stepped = step;
        if (cl->stepped) { cloth_step(cl, sp, fminf(cl->acc, 0.07f), back, pl, ground + 0.1f * GU, ceil); cl->acc = 0.0f; }
        for (int c = 0; c < cl->cols; c++) {     // the pins follow the body every frame
            memcpy(PC(cl, c, 0), cl->anchor[c], sizeof(float[3]));
            if (sp->kind == CLOTH_PLUME) memcpy(PC(cl, c, 1), cl->anchor2[c], sizeof(float[3]));
        }
#ifdef HLE_LOG_CLOTH
        s_stepT += TICKS_READ() - ts;
#endif
        if (hair) t3d_state_set_depth_offset(-0x40);   // where it lies on the cape, it wins
        cloth_draw(cl, sp, modelMat, o, ax, s2, cl->stepped, pl);
        if (hair) t3d_state_set_depth_offset(0);
    }
    if (step) { memcpy(s_bladeA0, bk->a, sizeof s_bladeA0); memcpy(s_bladeB0, bk->b, sizeof s_bladeB0); s_bladeOk = true; }
#ifdef HLE_PROF_SERIAL
    s_kp[3] += gpu_prof_split();
#endif
#ifdef HLE_LOG_CLOTH
    { static uint32_t sum; static int n; sum += TICKS_READ() - t0;
      static float st, stMax; Cloth *cl = &s_cloth[s->cl[0].kind == CLOTH_CLOAK ? 0 : 1];   // the cloak's stretch, hem to pins / rest
      float hang = 0, rest = 0;
      for (int c = 0; c < cl->cols; c++)
          for (int r = 0; r < cl->rows - 1; r++) { hang += sqrtf(t3d_vec3_distance2((T3DVec3*)PC(cl, c, r), (T3DVec3*)PC(cl, c, r + 1))); rest += cl->vl[r][c][0]; }
      st += hang / rest; stMax = fmaxf(stMax, hang / rest);
      if (n == 119) { debugf("[cloth] stretch %.3f avg, %.3f max\n", (double)(st / 120), (double)stMax); st = stMax = 0; }
      if (++n == 120) { debugf("[cloth] %lu us a frame: bodies %lu, simulation %lu, draw submit %lu (collision %lu) anchors %lu vertices %lu (matrix %lu, flush %lu)\n", (unsigned long)(TICKS_TO_US(sum) / n),
                               (unsigned long)(TICKS_TO_US(s_bodyT) / n), (unsigned long)(TICKS_TO_US(s_stepT) / n), (unsigned long)(TICKS_TO_US(s_subT) / n), (unsigned long)(TICKS_TO_US(s_colT) / n), (unsigned long)(TICKS_TO_US(s_ancT) / n), (unsigned long)(TICKS_TO_US(s_vtxT) / n), (unsigned long)(TICKS_TO_US(s_matT) / n), (unsigned long)(TICKS_TO_US(s_wbT) / n)); sum = s_bodyT = s_stepT = s_subT = s_colT = s_ancT = s_vtxT = s_matT = s_wbT = 0; n = 0; } }
#endif
#ifdef HLE_LOG_CLIP
    { static int n, sum, worst; sum += s_clipN; worst = s_clipN > worst ? s_clipN : worst; s_clipN = 0;
      static int fs; fs += s_frontN; s_frontN = 0;
      if (++n == 60) { debugf("[clip] %d points in the arms over 60 frames (worst frame %d), %d in front of him\n", sum, worst, fs); n = sum = worst = fs = 0; } }
#endif
#ifdef HLE_LOG_HEM   // test aid: the cloak's hem against the legs, above the ankle in GU (the units of ClothSpec.len)
    { static int n; if (++n % 60 == 0) {
      const Cloth *cl = &s_cloth[s->cl[0].kind == CLOTH_CLOAK ? 0 : 1];
      float foot = 0.5f * (s_bw[B_FOOT_L][3][1] + s_bw[B_FOOT_R][3][1]), mean = 0, lo = 1e9f, hi = -1e9f;
      for (int c = 0; c < cl->cols; c++) { float y = cl->p[(cl->rows - 1) * cl->cols + c][1]; mean += y; lo = fminf(lo, y); hi = fmaxf(hi, y); }
      mean /= cl->cols;
      #define UP(y) (double)(((y) - foot) / GU)
      debugf("[hem] skin %d hem mean %.2f low %.2f high %.2f | knee %.2f hip %.2f waist %.2f pin %.2f\n", s_skin, UP(mean), UP(lo), UP(hi),
             UP(0.5f * (s_bw[B_SHIN_L][3][1] + s_bw[B_SHIN_R][3][1])), UP(0.5f * (s_bw[B_LEG_L][3][1] + s_bw[B_LEG_R][3][1])), UP(s_bw[B_WAIST][3][1]), UP(cl->anchor[3][1]));
      #undef UP
    } }
#endif
}
