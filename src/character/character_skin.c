#include <libdragon.h>
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
#define GU   (GLTF * MODEL_SCALE)      // world units per glTF unit

typedef struct { uint8_t r, g, b, set; } SkinColor;
#define C(r, g, b) { r, g, b, 1 }

enum { CLOTH_NONE, CLOTH_CLOAK, CLOTH_HAIR, CLOTH_PLUME };

typedef struct {
    uint8_t kind;
    SkinColor color;
    bool tattered;
    float len, flare;                  // glTF units; bottom width over top width - 1
    float drop;                        // a cloak pinned this much below the shoulders
} ClothSpec;

// Accessories: procedural meshes riding a bone (see acc_build)
enum { ACC_WINGED_HELM = 1, ACC_FUR_COLLAR = 2, ACC_BERSERK_HELM = 4, ACC_DRAGONSLAYER = 8,
       ACC_WYLDER_HELM = 16, ACC_SCARF = 32, ACC_CLAW_SHOT = 64 };

typedef struct {
    const char *name;
    SkinColor armor, helm, cloth, plume, chain, sword;
    bool hidePlume, hideSword;
    ClothSpec cl[2];
    bool hood;                         // offers the Hood setting
    uint8_t acc;
} SkinDef;

#define CLOAK(r, g, b) { CLOTH_CLOAK, C(r, g, b), false, 5.4f, 0.4f }

static const SkinDef SKINS[SKIN_COUNT] = {
    [SKIN_KNIGHT]           = { "Knight" },
    [SKIN_TEAL_CLOAK]       = { "Teal Cloak",    .cloth = C(20, 70, 70), .cl = { CLOAK(30, 120, 118) }, .hood = true },
    [SKIN_CRIMSON_CLOAK]    = { "Crimson Cloak", .cloth = C(90, 16, 18), .cl = { CLOAK(150, 26, 30) },  .hood = true },
    [SKIN_UMBER_CLOAK]      = { "Umber Cloak",   .cloth = C(62, 42, 26), .cl = { CLOAK(105, 70, 44) },  .hood = true },
    // Artorias: dark silver plate, bright mail, a torn royal-blue cloak, a long black plume
    [SKIN_WOLF_KNIGHT]      = { "Wolf Knight", .armor = C(112, 104, 100), .helm = C(146, 142, 146), .chain = C(165, 175, 196),
                                .cloth = C(30, 52, 108), .hidePlume = true,
                                .cl = { { CLOTH_CLOAK, C(42, 74, 152), true, 4.6f, 0.5f }, { CLOTH_PLUME, C(22, 22, 30), false, 3.6f, 0.4f } } },
    // Guts: blackened plate, a spined wolf helm with burning eyes, a ragged black cloak, the Dragonslayer
    [SKIN_BLACK_SWORDSMAN]  = { "Black Swordsman", .armor = C(58, 57, 63), .helm = C(52, 51, 58), .cloth = C(24, 22, 22),
                                .chain = C(22, 22, 24), .hidePlume = true, .hideSword = true,
                                .cl = { { CLOTH_CLOAK, C(26, 24, 27), true, 6.2f, 0.55f } }, .acc = ACC_BERSERK_HELM | ACC_DRAGONSLAYER },
    // Malenia: brass plate, an olive pleated dress, a winged helm, a fur collar, red hair over a crimson cape
    [SKIN_SCARLET_VALKYRIE] = { "Scarlet Valkyrie", .armor = C(196, 156, 82), .helm = C(208, 168, 92), .cloth = C(98, 80, 50),
                                .chain = C(72, 60, 40), .hidePlume = true,
                                .cl = { { CLOTH_HAIR, C(196, 64, 30), true, 3.4f, 0.3f }, { CLOTH_CLOAK, C(86, 12, 16), false, 4.8f, 0.55f, 0.8f } },
                                .acc = ACC_WINGED_HELM | ACC_FUR_COLLAR },
    // Wylder (Nightreign): a winged silver helm, a dark scarf, a royal-blue tabard and long coat,
    // the claw shot on his left forearm
    [SKIN_WYLDER]           = { "Wylder", .armor = C(92, 96, 112), .helm = C(176, 180, 194), .cloth = C(30, 56, 150),
                                .chain = C(58, 60, 72), .hidePlume = true,
                                .cl = { { CLOTH_CLOAK, C(38, 66, 170), false, 6.0f, 0.45f } },
                                .acc = ACC_WYLDER_HELM | ACC_SCARF | ACC_CLAW_SHOT },
};

static int  s_skin = SKIN_KNIGHT;
static bool s_hoodUp = true;
static const T3DModel *s_model;
static const T3DSkeleton *s_skel;
static rspq_block_t **s_block;       // character.c's dpl_model, re-recorded on change

const char* character_skin_name(int id) { return (id >= 0 && id < SKIN_COUNT) ? SKINS[id].name : SKINS[0].name; }
int  character_skin_get(void) { return s_skin; }
bool character_skin_hood_up(void) { return s_hoodUp; }
bool character_skin_has_hood(int id) { return id >= 0 && id < SKIN_COUNT && SKINS[id].hood; }

/* -----------------------------------------------------------------------------
 * Bones
 * -------------------------------------------------------------------------- */

enum { B_WAIST, B_SPINE, B_HEAD, B_ARM_L, B_ARM_R, B_FORE_L, B_FORE_R, B_HAND_L, B_HAND_R, B_LEG_L, B_LEG_R, B_SHIN_L, B_SHIN_R, B_FOOT_L, B_FOOT_R, B_COUNT };
static const char *BONE_NAMES[B_COUNT] = {
    "Waist", "Spine1", "Head", "UpperArm-Left", "UpperArm-Right", "LowerArm-Left", "LowerArm-Right", "Hand-Left", "Hand-Right", "UpperLeg-Left", "UpperLeg-Right",
    "LowerLeg-Left", "LowerLeg-Right", "Foot-Left", "Foot-Right",
};
static int s_bone[B_COUNT];

// bone-local glTF offset -> world
static void bone_world(const T3DMat4FP *modelMat, int b, float x, float y, float z, float out[3])
{
    const float local[3] = { x * GLTF, y * GLTF, z * GLTF };
    float model[3];
    mat4fp_mul_point_f32_row3_colbasis(&s_skel->boneMatricesFP[s_bone[b]], local, model);
    mat4fp_mul_point_f32_row3_colbasis(modelMat, model, out);
}

/* -----------------------------------------------------------------------------
 * Verlet cloth (world space)
 * -------------------------------------------------------------------------- */

#define CLOTH_COLS 7
#define CLOTH_ROWS 8
#define CLOTH_N (CLOTH_COLS * CLOTH_ROWS)
#define RING 4                          // frames the RSP may still be reading

typedef struct { float a[3], b[3], r; } Capsule;

typedef struct {
    int cols, rows;
    float p[CLOTH_N][3], q[CLOTH_N][3];
    float anchor[CLOTH_COLS][3], anchor2[CLOTH_COLS][3];   // anchor2: a plume's second, stiff row
    float hrest[CLOTH_ROWS], vrest[CLOTH_COLS];
    bool valid;
    float time;
    T3DVertPacked *vb[RING];
    T3DMat4FP *mat[RING];
    int ring;
} Cloth;

static Cloth s_cloth[2];
static Capsule s_body[20];
static int s_bodyCount;

static const uint8_t TATTER[CLOTH_COLS] = { 100, 15, 60, 5, 80, 25, 90 };  // % of the last three segments

void character_skin_reset_cloth(void) { s_cloth[0].valid = s_cloth[1].valid = false; }

static inline float *PC(Cloth *cl, int c, int r) { return cl->p[r * cl->cols + c]; }
static inline bool strands(const ClothSpec *sp) { return sp->kind == CLOTH_HAIR || sp->kind == CLOTH_PLUME; }

static void cloth_anchors(Cloth *cl, const ClothSpec *sp, const T3DMat4FP *M)
{
    for (int c = 0; c < cl->cols; c++) {
        float u = (float)c / (cl->cols - 1) * 2.0f - 1.0f;   // -1..1 across
        if (sp->kind == CLOTH_CLOAK && sp->drop > 0.0f)   // across the back, under the hair
            bone_world(M, B_SPINE, 1.25f - 0.7f * u * u + ((c & 1) ? 0.06f : 0.0f), 2.62f - sp->drop, 1.2f * u, cl->anchor[c]);
        else if (sp->kind == CLOTH_CLOAK)   // over the pauldrons, wrapping forward at the sides, pleated
            bone_world(M, B_SPINE, 0.95f - 0.95f * u * u + ((c & 1) ? 0.08f : 0.0f), 2.62f, 1.45f * u, cl->anchor[c]);
        else if (sp->kind == CLOTH_HAIR)    // from the back of the helm
            bone_world(M, B_HEAD, 0.6f - 0.15f * u * u, 1.3f, 0.62f * u, cl->anchor[c]);
        else {                              // a crest on top of the helm that arches up and back
            bone_world(M, B_HEAD, -0.15f + 0.25f * (u + 1.0f), 2.0f, 0.1f * u, cl->anchor[c]);
            bone_world(M, B_HEAD, 0.35f + 0.25f * (u + 1.0f), 2.55f, 0.12f * u, cl->anchor2[c]);
        }
    }
}

static void cloth_bodies(const T3DMat4FP *M)
{
    Capsule *b = s_body; int n = 0;
    #define SPHERE(bone, x, y, z, rad) do { bone_world(M, bone, x, y, z, b[n].a); memcpy(b[n].b, b[n].a, sizeof b[n].a); b[n].r = (rad) * GU; n++; } while (0)
    #define CAPS(b0, b1, rad) do { bone_world(M, b0, 0, 0, 0, b[n].a); bone_world(M, b1, 0, 0, 0, b[n].b); b[n].r = (rad) * GU; n++; } while (0)
    // Radii are the mesh's extents plus a margin, as cloth triangles span between particles
    const float m = 0.12f;
    SPHERE(B_SPINE, -0.1f, 1.45f, 0, 1.0f + m);      // upper back
    SPHERE(B_SPINE, -0.1f, 0.35f, 0, 0.95f + m);
    SPHERE(B_WAIST, -0.05f, -0.2f, 0, 0.9f + m);
    SPHERE(B_HEAD, -0.34f, 1.07f, 0, 0.9f);          // hair and plume roots sit on it
    // sword hilt (the blade runs along the hand's -X): crossguard and pommel
    bone_world(M, B_HAND_R, -0.3f, 0, 0, b[n].a); bone_world(M, B_HAND_R, 1.25f, 0, 0, b[n].b); b[n].r = (0.7f + m) * GU; n++;
    // and the blade, pommel to tip (the stock sword's line; the Dragonslayer lies along it, broader)
    bone_world(M, B_HAND_R, 0.2f, 0.7f, 0.1f, b[n].a); bone_world(M, B_HAND_R, -8.0f, 1.05f, -1.0f, b[n].b);
    b[n].r = ((SKINS[s_skin].acc & ACC_DRAGONSLAYER) ? 0.75f : 0.4f) * GU + m * GU; n++;
    CAPS(B_ARM_L, B_FORE_L, 0.62f + m);              // pauldrons and upper arms
    CAPS(B_ARM_R, B_FORE_R, 0.62f + m);
    SPHERE(B_FORE_L, 0, 0, 0, 0.88f + m);             // pointed elbow guards
    SPHERE(B_FORE_R, 0, 0, 0, 0.88f + m);
    CAPS(B_FORE_L, B_HAND_L, 0.45f + m);
    CAPS(B_FORE_R, B_HAND_R, 0.45f + m);
    CAPS(B_LEG_L, B_SHIN_L, 0.5f + m);
    CAPS(B_LEG_R, B_SHIN_R, 0.5f + m);
    CAPS(B_SHIN_L, B_FOOT_L, 0.42f + m);
    CAPS(B_SHIN_R, B_FOOT_R, 0.42f + m);
    #undef SPHERE
    #undef CAPS
    s_bodyCount = n;
}

static void cloth_reset(Cloth *cl, const ClothSpec *sp)
{
    float len = sp->len * GU;
    float top = sqrtf(t3d_vec3_distance2(&(T3DVec3){{cl->anchor[0][0], cl->anchor[0][1], cl->anchor[0][2]}},
                                         &(T3DVec3){{cl->anchor[1][0], cl->anchor[1][1], cl->anchor[1][2]}}));
    for (int r = 0; r < cl->rows; r++) cl->hrest[r] = top * (1.0f + sp->flare * r / (cl->rows - 1));
    for (int c = 0; c < cl->cols; c++) cl->vrest[c] = len / (cl->rows - 1);
    for (int r = 0; r < cl->rows; r++)
        for (int c = 0; c < cl->cols; c++) {
            float *p = PC(cl, c, r);
            p[0] = cl->anchor[c][0]; p[1] = cl->anchor[c][1] - cl->vrest[c] * r; p[2] = cl->anchor[c][2];
            memcpy(cl->q[r * cl->cols + c], p, sizeof(float[3]));
        }
    cl->valid = true;
}

static void link(float *a, float *b, float rest, bool pinA)
{
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1e-4f) return;
    float k = (len - rest) / len;
    float wa = pinA ? 0.0f : 0.5f, wb = pinA ? 1.0f : 0.5f;
    for (int i = 0; i < 3; i++) { a[i] += d[i] * k * wa; b[i] -= d[i] * k * wb; }
}

// pad: hair over a cape rides a little further out, so it lies on the cape
static void collide(float *p, float pad)
{
    for (int i = 0; i < s_bodyCount; i++) {
        const Capsule *c = &s_body[i];
        float ab[3] = { c->b[0] - c->a[0], c->b[1] - c->a[1], c->b[2] - c->a[2] };
        float ap[3] = { p[0] - c->a[0], p[1] - c->a[1], p[2] - c->a[2] };
        float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
        float h = l2 > 0 ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2 : 0;
        h = h < 0 ? 0 : h > 1 ? 1 : h;
        float d[3] = { ap[0] - ab[0] * h, ap[1] - ab[1] * h, ap[2] - ab[2] * h };
        float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        float r = c->r + (i == 3 ? 0.0f : pad);   // not round the head: that is where it is pinned
        if (dl < r && dl > 1e-4f) {
            float k = (r - dl) / dl;
            p[0] += d[0] * k; p[1] += d[1] * k; p[2] += d[2] * k;
        }
    }
}

// plane: hair over a cape stays behind it (point, normal), or NULL
static void cloth_step(Cloth *cl, const ClothSpec *sp, float h, const float back[3], const float *plane)
{
    const float g = -220.0f * h * h;
    cl->time += h;
    const float damp = strands(sp) ? 0.92f : 0.97f;
    for (int r = 1; r < cl->rows; r++)
        for (int c = 0; c < cl->cols; c++) {
            float *p = PC(cl, c, r), *q = cl->q[r * cl->cols + c];
            float gust = (14.0f + 10.0f * sinf(cl->time * 1.7f + c * 0.6f)) * h * h * r / cl->rows;
            for (int i = 0; i < 3; i++) {
                float v = (p[i] - q[i]) * damp;
                q[i] = p[i];
                p[i] += v + back[i] * gust;
            }
            p[1] += g;
        }
    for (int it = 0; it < 6; it++) {
        for (int c = 0; c < cl->cols; c++) {
            memcpy(PC(cl, c, 0), cl->anchor[c], sizeof(float[3]));
            if (sp->kind == CLOTH_PLUME) memcpy(PC(cl, c, 1), cl->anchor2[c], sizeof(float[3]));
        }
        for (int r = 0; r < cl->rows - 1; r++)
            for (int c = 0; c < cl->cols; c++) {
                float rest = cl->vrest[c];
                if (sp->tattered && r >= cl->rows - 4) rest *= TATTER[c] / 100.0f;
                link(PC(cl, c, r), PC(cl, c, r + 1), rest, r == 0 || (r == 1 && sp->kind == CLOTH_PLUME));
            }
        if (strands(sp))   // bend links: hair can't fold back on itself
            for (int r = 0; r < cl->rows - 2; r++)
                for (int c = 0; c < cl->cols; c++) link(PC(cl, c, r), PC(cl, c, r + 2), 2 * cl->vrest[c], r == 0);
        int loose = sp->tattered ? cl->rows - 3 : cl->rows;   // tatters hang as free strands
        for (int r = 1; r < loose; r++)
            for (int c = 0; c < cl->cols - 1; c++) link(PC(cl, c, r), PC(cl, c + 1, r), cl->hrest[r], false);
        if (it >= 3) for (int i = cl->cols; i < cl->cols * cl->rows; i++) {
            float *p = cl->p[i];
            collide(p, 0.0f);
            if (plane) {
                float d = (p[0] - plane[0]) * plane[3] + (p[1] - plane[1]) * plane[4] + (p[2] - plane[2]) * plane[5];
                if (d < 0) for (int m = 0; m < 3; m++) p[m] -= d * plane[3 + m];
            }
        }
        // Never above the pins (a cloak) or above the head (a plume streams back, it can't stand up)
        for (int r = 1; r < cl->rows; r++)
            for (int c = 0; c < cl->cols; c++) {
                float top = sp->kind == CLOTH_PLUME ? cl->anchor2[c][1] : cl->anchor[c][1];
                float maxY = top - (sp->kind == CLOTH_PLUME ? 0.02f : 0.15f) * r * cl->vrest[c];
                if (PC(cl, c, r)[1] > maxY) PC(cl, c, r)[1] = maxY;
            }
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

static uint32_t shade(SkinColor c, const float n[3], float bias)
{
    static const float L[3] = { 0.33f, 0.85f, 0.41f };
    float d = fabsf(n[0] * L[0] + n[1] * L[1] + n[2] * L[2]);
    float s = (0.3f + 0.7f * d) * bias;
    // t3d lights the knight's vertex colours by the ambient colour alone
    float amb[3] = { colorAmbient[0] / 255.0f, colorAmbient[1] / 255.0f, colorAmbient[2] / 255.0f };
    int r = c.r * s * amb[0], g = c.g * s * amb[1], b = c.b * s * amb[2];
    r = r > 255 ? 255 : r; g = g > 255 ? 255 : g; b = b > 255 ? 255 : b;
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFF;
}

static void cloth_draw(Cloth *cl, const ClothSpec *sp, const float origin[3])
{
    if (!cl->vb[0]) {
        for (int i = 0; i < RING; i++) {
            cl->vb[i] = malloc_uncached(sizeof(T3DVertPacked) * CLOTH_N / 2);
            cl->mat[i] = malloc_uncached(sizeof(T3DMat4FP));
        }
    }
    T3DVertPacked *vb = cl->vb[cl->ring];
    T3DMat4FP *mat = cl->mat[cl->ring];
    cl->ring = (cl->ring + 1) % RING;

    // Positions relative to the character in 1/8 world units, for sub-unit precision
    const float SUB = 8.0f;
    t3d_mat4fp_from_srt_euler(mat, (float[3]){ 1 / SUB, 1 / SUB, 1 / SUB }, (float[3]){ 0, 0, 0 }, origin);
    for (int r = 0; r < cl->rows; r++)
        for (int c = 0; c < cl->cols; c++) {
            const float *l = PC(cl, c > 0 ? c - 1 : c, r), *rr = PC(cl, c < cl->cols - 1 ? c + 1 : c, r);
            const float *u = PC(cl, c, r > 0 ? r - 1 : r), *d = PC(cl, c, r < cl->rows - 1 ? r + 1 : r);
            float a[3] = { rr[0] - l[0], rr[1] - l[1], rr[2] - l[2] }, b[3] = { d[0] - u[0], d[1] - u[1], d[2] - u[2] };
            float n[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
            float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-6f;
            n[0] /= nl; n[1] /= nl; n[2] /= nl;
            const float *p = PC(cl, c, r);
            int16_t pos[3] = { (int16_t)((p[0] - origin[0]) * SUB), (int16_t)((p[1] - origin[1]) * SUB), (int16_t)((p[2] - origin[2]) * SUB) };
            // strands get a sheen down the middle, cloth darkens toward the hem
            float bias = strands(sp) ? 0.85f + 0.3f * (c == cl->cols / 2) : 1.0f - 0.25f * r / cl->rows;
            put_vert(vb, r * cl->cols + c, pos, shade(sp->color, n, bias));
        }

    rdpq_sync_pipe();
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    rdpq_mode_blender(0);
    rdpq_mode_zbuf(true, true);
    rdpq_mode_alphacompare(0);
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT);
    t3d_matrix_set(mat, true);
    t3d_vert_load(vb, 0, cl->cols * cl->rows);
    for (int r = 0; r < cl->rows - 1; r++)
        for (int c = 0; c < cl->cols - 1; c++) {
            int i = r * cl->cols + c;
            t3d_tri_draw(i, i + cl->cols, i + 1);
            t3d_tri_draw(i + 1, i + cl->cols, i + cl->cols + 1);
        }
    t3d_tri_sync();
}

/* -----------------------------------------------------------------------------
 * Hood: static shells built once per skin, drawn on a bone
 * -------------------------------------------------------------------------- */

#define HOOD_MAX_VERTS 70
static T3DVertPacked *s_hoodVerts[2];
static rspq_block_t *s_hoodBlock;
static int s_hoodBone;

// Ellipsoid patch, theta around Y from +X (the back), phi up from the equator.
// peak > 0 drags the vertices nearest the back-top out into the hood's point;
// lip > 0 pushes the front vertices at the brow out and down into an overhang.
static int shell(T3DVertPacked *vb, const float c[3], const float rad[3], float t0, float t1, int nt,
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

static void hood_build(void)
{
    if (s_hoodBlock) { rspq_wait(); rspq_block_free(s_hoodBlock); s_hoodBlock = NULL; }   // may still be queued
    const SkinDef *s = &SKINS[s_skin];
    if (!s->hood) return;
    for (int i = 0; i < 2; i++)
        if (!s_hoodVerts[i]) s_hoodVerts[i] = malloc_uncached(sizeof(T3DVertPacked) * HOOD_MAX_VERTS / 2);

    const float R = (float)M_PI / 180.0f;
    // shell 0: the hood (or the collar), shell 1: the folded hood on the back
    float t0 = -180 * R, t1 = 180 * R, p0 = -52 * R, p1 = 90 * R, open = -10;   // -10: no opening
    int nt = 11, np = 6, n0, n1 = 0, nt1 = 7, np1 = 7;
    if (s_hoodUp) {   // over the helmet and forehead, open at the face
        s_hoodBone = B_HEAD;
        open = 12 * R;
        n0 = shell(s_hoodVerts[0], (float[3]){ -0.34f, 1.07f, 0 }, (float[3]){ 1.1f, 1.1f, 0.86f },
                   t0, t1, nt, p0, p1, np, 0.45f, 0.14f, s->cl[0].color);
    } else {          // gathered round the neck, folds lying on the back
        s_hoodBone = B_SPINE;
        t0 = -135 * R; t1 = 135 * R; p0 = -80 * R; p1 = 80 * R; nt = 9; np = 7;
        n0 = shell(s_hoodVerts[0], (float[3]){ -0.1f, 2.25f, 0 }, (float[3]){ 1.12f, 0.36f, 1.1f },
                   t0, t1, nt, p0, p1, np, 0, 0, s->cl[0].color);
        // outside the cloak (it hangs from x 1.25 at the shoulders; at 0.9 the fold was buried in it),
        // a little darker so it reads against the cloak
        SkinColor c = s->cl[0].color;
        c.r = c.r * 4 / 5; c.g = c.g * 4 / 5; c.b = c.b * 4 / 5;
        n1 = shell(s_hoodVerts[1], (float[3]){ 1.4f, 2.2f, 0 }, (float[3]){ 0.38f, 0.58f, 1.0f },
                   -90 * R, 90 * R, nt1, -88 * R, 88 * R, np1, 0, 0, c);
    }
    rspq_block_begin();
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    rdpq_mode_blender(0);
    rdpq_mode_zbuf(true, true);
    rdpq_mode_alphacompare(0);
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT);
    t3d_vert_load(s_hoodVerts[0], 0, n0);
    shell_tris(nt, np, t0, t1, p0, p1, open);
    t3d_tri_sync();
    if (n1) {
        t3d_vert_load(s_hoodVerts[1], 0, n1);
        shell_tris(nt1, np1, 0, 0, 0, 0, -10);
        t3d_tri_sync();
    }
    s_hoodBlock = rspq_block_end();
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

static void acc_winged_helm(SkinColor gold)
{
    // Malenia's crest: a tall wing of feathers rising off the helm and swept back,
    // in two leaves splayed a little so it reads from behind as well
    for (int side = -1; side <= 1; side += 2)
        for (int k = 0; k < 7; k++) {
            float a = (96.0f - k * 9.0f) * (float)M_PI / 180.0f, len = 2.1f - 0.12f * k, w = 0.26f - 0.012f * k;
            float bx = -0.72f + 0.13f * k, by = 1.84f + 0.03f * k;
            float dx = cosf(a), dy = sinf(a), sz = side * 0.08f;     // a slight V, to read from behind
            float l[4][3], r[4][3];
            for (int i = 0; i < 4; i++) {
                float t = i / 3.0f, ww = w * (1.0f - 0.7f * t), bend = 0.45f * t * t;   // tips curl back
                float px = bx + dx * len * t + bend, py = by + dy * len * t, pz = side * 0.05f + sz * t * len;
                G(l[i], px - dy * ww, py + dx * ww, pz); G(r[i], px + dy * ww, py - dx * ww, pz);
            }
            SkinColor c = gold; float f = 0.85f + 0.05f * k;
            c.r = (uint8_t)fminf(255, c.r * f); c.g = (uint8_t)fminf(255, c.g * f); c.b = (uint8_t)fminf(255, c.b * f);
            strip(l, r, 4, rgb(c));
        }
}

static void acc_wylder_helm(SkinColor silver)
{
    // Wings on both temples, swept up and back like a Norse helm's: five feathers a side,
    // flaring out from the helm a little
    for (int side = -1; side <= 1; side += 2)
        for (int k = 0; k < 5; k++) {
            float a = (80.0f - k * 12.0f) * (float)M_PI / 180.0f, len = 1.7f - 0.14f * k, w = 0.28f - 0.02f * k;
            float bx = -0.2f + 0.12f * k, by = 1.55f + 0.05f * k, bz = side * 0.62f;
            float dx = cosf(a), dy = sinf(a);
            // each feather's face tilted 45 degrees outward, so the wing reads from behind too
            const float ci = 0.7071f, so = 0.7071f;
            float l[4][3], r[4][3];
            for (int i = 0; i < 4; i++) {
                float t = i / 3.0f, ww = w * (1.0f - 0.6f * t), bend = 0.35f * t * t;
                float px = bx + dx * len * t + bend, py = by + dy * len * t, pz = bz + side * 0.4f * t;
                float wx = -dy * ww * ci, wy = dx * ww * ci, wz = side * ww * so;
                G(l[i], px + wx, py + wy, pz + wz); G(r[i], px - wx, py - wy, pz - wz);
            }
            SkinColor c = silver; float f = 0.8f + 0.06f * k;
            c.r = (uint8_t)fminf(255, c.r * f); c.g = (uint8_t)fminf(255, c.g * f); c.b = (uint8_t)fminf(255, c.b * f);
            strip(l, r, 4, rgb(c));
        }
}

static void acc_claw_shot(void)
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

static void ring(float cx, float cy, float rx, float ry, float rz, float th0, float th1, int nt, int np, uint32_t col, float fluff)
{
    for (int j = 0; j < np; j++)
        for (int i = 0; i < nt; i++) {
            float t[2] = { th0 + (th1 - th0) * i / nt, th0 + (th1 - th0) * (i + 1) / nt };
            float f[2] = { -1.4f + 2.8f * j / np, -1.4f + 2.8f * (j + 1) / np };
            float q[4][3];
            for (int k = 0; k < 4; k++) {
                float th = t[(k == 1 || k == 2)], ph = f[k >= 2];
                float fl = 1.0f + fluff * sinf(th * 13.0f + ph * 5.0f);
                G(q[k], cx + rx * cosf(ph) * cosf(th) * fl, cy + ry * sinf(ph) * fl, rz * cosf(ph) * sinf(th) * fl);
            }
            proc_mesh_quad(q[0], q[1], q[2], q[3], col, false);
        }
}

static void acc_berserk_helm(SkinColor steel)
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

static void acc_dragonslayer(SkinColor iron)
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
            proc_mesh_quad(a0, a1, b1, b0, steel, false);
            BP(c0, L, sd * HW, f * ET); BP(tip, P, 0, 0); BP(ch, L + 0.75f, 0, f * TH);
            BP(c1, L, sd * CW, f * TH);
            proc_mesh_quad(c0, tip, ch, c1, steel, false);     // the point's bevel
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
        proc_mesh_quad(a0, a1, b1, b0, steel, false);
        BP(tip, P, 0, 0);
        proc_mesh_tri(a1, tip, b1, steel, false);
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
    ACC(ACC_FUR_COLLAR, B_SPINE, ring(-0.1f, 2.62f, 1.32f, 0.38f, 1.42f, -2.5f, 2.5f, 16, 3, proc_rgb(156, 132, 100), 0.08f));
    ACC(ACC_BERSERK_HELM, B_HEAD, acc_berserk_helm(s->helm));
    ACC(ACC_DRAGONSLAYER, B_HAND_R, acc_dragonslayer((SkinColor)C(70, 68, 70)));
    ACC(ACC_WYLDER_HELM, B_HEAD, acc_wylder_helm(s->helm));
    ACC(ACC_SCARF, B_SPINE, ring(-0.1f, 2.66f, 1.2f, 0.36f, 1.28f, -2.6f, 2.6f, 16, 3, proc_rgb(48, 50, 60), 0.03f));
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
    const SkinColor cols[] = { s->armor, s->helm, s->cloth, s->plume, s->chain, s->sword };
    enum { NM = sizeof MATS / sizeof MATS[0] };
    color_t saved[NM];
    for (int i = 0; i < NM; i++) tint(MATS[i], cols[i], &saved[i]);
    uintptr_t hide = ((s->hidePlume || (s->hood && s_hoodUp)) ? 1 : 0) | (s->hideSword ? 2 : 0);
    rspq_block_begin();
    t3d_model_draw_custom(model, (T3DModelDrawConf){
        .userData = (void*)hide,
        .filterCb = hide ? filter_parts : NULL,
        .matrices = skel->bufferCount == 1 ? skel->boneMatricesFP
                                           : (const T3DMat4FP*)t3d_segment_placeholder(T3D_SEGMENT_SKELETON),
    });
    rspq_block_t *block = rspq_block_end();
    for (int i = 0; i < NM; i++) {
        T3DMaterial *m = t3d_model_get_material(model, MATS[i]);
        if (m) m->primColor = saved[i];
    }
    hood_build();
    acc_build();
    character_skin_reset_cloth();
    return block;
}

void character_skin_bind_block(rspq_block_t **block) { s_block = block; }

static void rerecord(void)
{
    if (!s_block || !*s_block || !s_model) return;
    rspq_wait();   // the old block may still be queued for this frame
    rspq_block_free(*s_block);
    *s_block = character_skin_record(s_model, s_skel);
}

void character_skin_set(int id)
{
    if (id < 0 || id >= SKIN_COUNT) id = SKIN_KNIGHT;
    if (id == s_skin) return;
    s_skin = id;
    rerecord();
}

void character_skin_set_hood_up(bool up)
{
    if (up == s_hoodUp) return;
    s_hoodUp = up;
    rerecord();
}

void character_skin_draw_extras(const T3DSkeleton *skel, const T3DMat4FP *modelMat, const float pos[3], float dt)
{
    const SkinDef *s = &SKINS[s_skin];
    if (!s_model || skel != s_skel) return;

    if (s_hoodBlock) {
        t3d_matrix_set(modelMat, true);
        t3d_matrix_push(&skel->boneMatricesFP[s_bone[s_hoodBone]]);
        rspq_block_run(s_hoodBlock);
        t3d_matrix_pop(1);
    }
    acc_draw(skel, modelMat);
    if (s->cl[0].kind == CLOTH_NONE) return;

    cloth_bodies(modelMat);
    // "Back" is the model's +X axis in world space
    float o[3], x1[3], back[3];
    mat4fp_mul_point_f32_row3_colbasis(modelMat, (float[3]){ 0, 0, 0 }, o);
    mat4fp_mul_point_f32_row3_colbasis(modelMat, (float[3]){ GLTF, 0, 0 }, x1);
    for (int i = 0; i < 3; i++) back[i] = (x1[i] - o[i]) / GU;
    if (dt > 0.05f) dt = 0.05f;

    bool cape = s->cl[0].kind == CLOTH_CLOAK || s->cl[1].kind == CLOTH_CLOAK;
    for (int k = 0; k < 2; k++) {
        const ClothSpec *sp = &s->cl[k];
        Cloth *cl = &s_cloth[k];
        if (sp->kind == CLOTH_NONE) continue;
        cl->cols = sp->kind == CLOTH_CLOAK ? CLOTH_COLS : sp->kind == CLOTH_HAIR ? 3 : 2;
        cl->rows = CLOTH_ROWS;
        float prev[CLOTH_COLS][3]; memcpy(prev, cl->anchor, sizeof prev);
        float *prev0 = prev[0];
        cloth_anchors(cl, sp, modelMat);
        float dx = cl->anchor[0][0] - prev0[0], dy = cl->anchor[0][1] - prev0[1], dz = cl->anchor[0][2] - prev0[2];
        if (!cl->valid || dx * dx + dy * dy + dz * dz > 40.0f * 40.0f) cloth_reset(cl, sp);   // first frame or teleport
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
        // hair over a cape: kept behind a plane just outside the cape's top edge
        float plane[6], *pl = NULL;
        if (strands(sp) && cape) {
            bone_world(modelMat, B_SPINE, 1.12f, 2.62f, 0.0f, plane);
            float hl = sqrtf(back[0] * back[0] + back[2] * back[2]) + 1e-6f;
            plane[3] = back[0] / hl; plane[4] = 0.0f; plane[5] = back[2] / hl;
            pl = plane;
        }
        if (dt > 0.0f) { cloth_step(cl, sp, dt * 0.5f, back, pl); cloth_step(cl, sp, dt * 0.5f, back, pl); }
        if (pl) t3d_state_set_depth_offset(-0x40);   // where it lies on the cape, it wins
        cloth_draw(cl, sp, pos);
        if (pl) t3d_state_set_depth_offset(0);
    }
}
