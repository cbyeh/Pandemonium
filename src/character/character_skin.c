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

// Knight rig facts (from knight.glb): it faces -X, Y up; bone rest rotations are
// identity, so bone-local offsets are model offsets. Offsets below are in glTF units.
#define GLTF 64.0f                     // t3d base scale: model units per glTF unit
#define GU   (GLTF * MODEL_SCALE)      // world units per glTF unit

typedef struct { uint8_t r, g, b, set; } SkinColor;
#define C(r, g, b) { r, g, b, 1 }

enum { CLOTH_NONE, CLOTH_CLOAK, CLOTH_HAIR };

typedef struct {
    const char *name;
    SkinColor armor, helm, cloth, plume, chain;
    bool hidePlume;
    uint8_t clothKind;
    SkinColor clothColor;
    bool tattered;
    bool hood;                         // offers the Hood setting
} SkinDef;

static const SkinDef SKINS[SKIN_COUNT] = {
    [SKIN_KNIGHT]           = { "Knight" },
    [SKIN_TEAL_CLOAK]       = { "Teal Cloak",    .cloth = C(20, 70, 70),  .clothKind = CLOTH_CLOAK, .clothColor = C(30, 120, 118), .hood = true },
    [SKIN_CRIMSON_CLOAK]    = { "Crimson Cloak", .cloth = C(90, 16, 18),  .clothKind = CLOTH_CLOAK, .clothColor = C(150, 26, 30),  .hood = true },
    [SKIN_UMBER_CLOAK]      = { "Umber Cloak",   .cloth = C(62, 42, 26),  .clothKind = CLOTH_CLOAK, .clothColor = C(105, 70, 44),  .hood = true },
    [SKIN_WOLF_KNIGHT]      = { "Wolf Knight",   .armor = C(200, 208, 230), .helm = C(200, 208, 230), .cloth = C(26, 34, 84),
                                .plume = C(46, 64, 140), .clothKind = CLOTH_CLOAK, .clothColor = C(30, 40, 92), .tattered = true },
    [SKIN_BLACK_SWORDSMAN]  = { "Black Swordsman", .armor = C(78, 76, 84), .helm = C(70, 68, 76), .cloth = C(26, 22, 22),
                                .chain = C(20, 20, 22), .hidePlume = true, .clothKind = CLOTH_CLOAK, .clothColor = C(34, 30, 30), .tattered = true },
    [SKIN_SCARLET_VALKYRIE] = { "Scarlet Valkyrie", .armor = C(255, 205, 120), .helm = C(255, 205, 120), .cloth = C(150, 26, 30),
                                .hidePlume = true, .clothKind = CLOTH_HAIR, .clothColor = C(200, 58, 28) },
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

enum { B_WAIST, B_SPINE, B_HEAD, B_ARM_L, B_ARM_R, B_LEG_L, B_LEG_R, B_SHIN_L, B_SHIN_R, B_FOOT_L, B_FOOT_R, B_COUNT };
static const char *BONE_NAMES[B_COUNT] = {
    "Waist", "Spine1", "Head", "UpperArm-Left", "UpperArm-Right", "UpperLeg-Left", "UpperLeg-Right",
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

#define CLOTH_COLS 6
#define CLOTH_ROWS 8
#define CLOTH_N (CLOTH_COLS * CLOTH_ROWS)
#define RING 4                          // frames the RSP may still be reading

typedef struct { float a[3], b[3], r; } Capsule;

static struct {
    int cols, rows;
    float p[CLOTH_N][3], q[CLOTH_N][3];
    float anchor[CLOTH_COLS][3];
    float hrest[CLOTH_ROWS], vrest[CLOTH_COLS];
    bool valid;
    float time;
    Capsule body[8]; int bodyCount;
    T3DVertPacked *vb[RING];
    T3DMat4FP *mat[RING];
    int ring;
} cloth;

static const uint8_t TATTER[CLOTH_COLS] = { 100, 15, 60, 5, 80, 25 };  // % of the last three segments

void character_skin_reset_cloth(void) { cloth.valid = false; }

static inline float *P(int c, int r) { return cloth.p[r * cloth.cols + c]; }

static void cloth_anchors(const T3DMat4FP *M)
{
    const SkinDef *s = &SKINS[s_skin];
    for (int c = 0; c < cloth.cols; c++) {
        float u = (float)c / (cloth.cols - 1) * 2.0f - 1.0f;   // -1..1 across
        if (s->clothKind == CLOTH_CLOAK) {
            // Across the shoulders, wrapping forward at the sides
            bone_world(M, B_SPINE, 0.95f - 0.5f * u * u + ((c & 1) ? 0.1f : -0.05f), 2.2f, 0.95f * u, cloth.anchor[c]);   // pleated
        } else {
            bone_world(M, B_HEAD, 0.45f - 0.35f * u * u, 1.3f, 0.55f * u, cloth.anchor[c]);
        }
    }
}

static void cloth_bodies(const T3DMat4FP *M)
{
    Capsule *b = cloth.body; int n = 0;
    #define SPHERE(bone, x, y, z, rad) do { bone_world(M, bone, x, y, z, b[n].a); memcpy(b[n].b, b[n].a, sizeof b[n].a); b[n].r = (rad) * GU; n++; } while (0)
    #define CAPS(b0, b1, rad) do { bone_world(M, b0, 0, 0, 0, b[n].a); bone_world(M, b1, 0, 0, 0, b[n].b); b[n].r = (rad) * GU; n++; } while (0)
    SPHERE(B_SPINE, -0.05f, 1.05f, 0, 1.0f);
    SPHERE(B_WAIST, 0, 0, 0, 0.95f);
    SPHERE(B_HEAD, -0.34f, 1.07f, 0, 0.95f);
    SPHERE(B_ARM_L, 0, 0, 0, 0.5f);
    SPHERE(B_ARM_R, 0, 0, 0, 0.5f);
    CAPS(B_LEG_L, B_SHIN_L, 0.48f);
    CAPS(B_LEG_R, B_SHIN_R, 0.48f);
    CAPS(B_SHIN_L, B_FOOT_L, 0.4f);
    #undef SPHERE
    #undef CAPS
    cloth.bodyCount = n;
}

static void cloth_reset(void)
{
    const SkinDef *s = &SKINS[s_skin];
    float len = (s->clothKind == CLOTH_HAIR ? 4.0f : 5.4f) * GU;
    float flare = s->clothKind == CLOTH_HAIR ? 0.2f : 0.7f;
    float top = sqrtf(t3d_vec3_distance2(&(T3DVec3){{cloth.anchor[0][0], cloth.anchor[0][1], cloth.anchor[0][2]}},
                                         &(T3DVec3){{cloth.anchor[1][0], cloth.anchor[1][1], cloth.anchor[1][2]}}));
    for (int r = 0; r < cloth.rows; r++) cloth.hrest[r] = top * (1.0f + flare * r / (cloth.rows - 1));
    for (int c = 0; c < cloth.cols; c++) cloth.vrest[c] = len / (cloth.rows - 1);
    for (int r = 0; r < cloth.rows; r++)
        for (int c = 0; c < cloth.cols; c++) {
            float *p = P(c, r);
            p[0] = cloth.anchor[c][0]; p[1] = cloth.anchor[c][1] - cloth.vrest[c] * r; p[2] = cloth.anchor[c][2];
            memcpy(cloth.q[r * cloth.cols + c], p, sizeof(float[3]));
        }
    cloth.valid = true;
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

static void collide(float *p)
{
    for (int i = 0; i < cloth.bodyCount; i++) {
        const Capsule *c = &cloth.body[i];
        float ab[3] = { c->b[0] - c->a[0], c->b[1] - c->a[1], c->b[2] - c->a[2] };
        float ap[3] = { p[0] - c->a[0], p[1] - c->a[1], p[2] - c->a[2] };
        float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
        float h = l2 > 0 ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2 : 0;
        h = h < 0 ? 0 : h > 1 ? 1 : h;
        float d[3] = { ap[0] - ab[0] * h, ap[1] - ab[1] * h, ap[2] - ab[2] * h };
        float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dl < c->r && dl > 1e-4f) {
            float k = (c->r - dl) / dl;
            p[0] += d[0] * k; p[1] += d[1] * k; p[2] += d[2] * k;
        }
    }
}

static void cloth_step(float h, const float back[3])
{
    const float g = -220.0f * h * h;
    cloth.time += h;
    for (int r = 1; r < cloth.rows; r++)
        for (int c = 0; c < cloth.cols; c++) {
            float *p = P(c, r), *q = cloth.q[r * cloth.cols + c];
            float gust = (14.0f + 10.0f * sinf(cloth.time * 1.7f + c * 0.6f)) * h * h * r / cloth.rows;
            for (int i = 0; i < 3; i++) {
                float v = (p[i] - q[i]) * 0.97f;
                q[i] = p[i];
                p[i] += v + back[i] * gust;
            }
            p[1] += g;
        }
    for (int it = 0; it < 6; it++) {
        for (int c = 0; c < cloth.cols; c++) memcpy(P(c, 0), cloth.anchor[c], sizeof(float[3]));
        for (int r = 0; r < cloth.rows - 1; r++)
            for (int c = 0; c < cloth.cols; c++) {
                float rest = cloth.vrest[c];
                if (SKINS[s_skin].tattered && r >= cloth.rows - 4) rest *= TATTER[c] / 100.0f;
                link(P(c, r), P(c, r + 1), rest, r == 0);
            }
        int loose = SKINS[s_skin].tattered ? cloth.rows - 3 : cloth.rows;   // tatters hang as free strands
        for (int r = 1; r < loose; r++)
            for (int c = 0; c < cloth.cols - 1; c++) link(P(c, r), P(c + 1, r), cloth.hrest[r], false);
        for (int i = cloth.cols; i < cloth.cols * cloth.rows; i++) collide(cloth.p[i]);
    }
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

static void cloth_draw(const float origin[3])
{
    if (!cloth.vb[0]) {
        for (int i = 0; i < RING; i++) {
            cloth.vb[i] = malloc_uncached(sizeof(T3DVertPacked) * CLOTH_N / 2);
            cloth.mat[i] = malloc_uncached(sizeof(T3DMat4FP));
        }
    }
    T3DVertPacked *vb = cloth.vb[cloth.ring];
    T3DMat4FP *mat = cloth.mat[cloth.ring];
    cloth.ring = (cloth.ring + 1) % RING;

    // Positions relative to the character in 1/8 world units, for sub-unit precision
    const float SUB = 8.0f;
    t3d_mat4fp_from_srt_euler(mat, (float[3]){ 1 / SUB, 1 / SUB, 1 / SUB }, (float[3]){ 0, 0, 0 }, origin);
    SkinColor col = SKINS[s_skin].clothColor;
    for (int r = 0; r < cloth.rows; r++)
        for (int c = 0; c < cloth.cols; c++) {
            const float *l = P(c > 0 ? c - 1 : c, r), *rr = P(c < cloth.cols - 1 ? c + 1 : c, r);
            const float *u = P(c, r > 0 ? r - 1 : r), *d = P(c, r < cloth.rows - 1 ? r + 1 : r);
            float a[3] = { rr[0] - l[0], rr[1] - l[1], rr[2] - l[2] }, b[3] = { d[0] - u[0], d[1] - u[1], d[2] - u[2] };
            float n[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
            float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-6f;
            n[0] /= nl; n[1] /= nl; n[2] /= nl;
            const float *p = P(c, r);
            int16_t pos[3] = { (int16_t)((p[0] - origin[0]) * SUB), (int16_t)((p[1] - origin[1]) * SUB), (int16_t)((p[2] - origin[2]) * SUB) };
            put_vert(vb, r * cloth.cols + c, pos, shade(col, n, 1.0f - 0.25f * r / cloth.rows));
        }

    rdpq_sync_pipe();
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    rdpq_mode_blender(0);
    rdpq_mode_zbuf(true, true);
    rdpq_mode_alphacompare(0);
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT);
    t3d_matrix_set(mat, true);
    t3d_vert_load(vb, 0, cloth.cols * cloth.rows);
    for (int r = 0; r < cloth.rows - 1; r++)
        for (int c = 0; c < cloth.cols - 1; c++) {
            int i = r * cloth.cols + c;
            t3d_tri_draw(i, i + cloth.cols, i + 1);
            t3d_tri_draw(i + 1, i + cloth.cols, i + cloth.cols + 1);
        }
    t3d_tri_sync();
}

/* -----------------------------------------------------------------------------
 * Hood: static shells built once per skin, drawn on a bone
 * -------------------------------------------------------------------------- */

#define HOOD_MAX_VERTS 64
static T3DVertPacked *s_hoodVerts[2];
static rspq_block_t *s_hoodBlock;
static int s_hoodBone;

// Ellipsoid patch, theta around Y from +X (the back), phi up from the equator.
// peak > 0 drags the vertices nearest the back-top out into the hood's point.
static int shell(T3DVertPacked *vb, const float c[3], const float rad[3], float t0, float t1, int nt,
                 float p0, float p1, int np, float peak, SkinColor col)
{
    static const float BT[3] = { 0.64f, 0.77f, 0.0f };
    for (int j = 0; j < np; j++)
        for (int i = 0; i < nt; i++) {
            float th = t0 + (t1 - t0) * i / (nt - 1), ph = p0 + (p1 - p0) * j / (np - 1);
            float d[3] = { cosf(ph) * cosf(th), sinf(ph), cosf(ph) * sinf(th) };
            float k = d[0] * BT[0] + d[1] * BT[1];
            float pk = k > 0 ? peak * k * k * k * k : 0;
            int16_t pos[3] = {
                (int16_t)((c[0] + rad[0] * d[0] + BT[0] * pk) * GLTF),
                (int16_t)((c[1] + rad[1] * d[1] + BT[1] * pk) * GLTF),
                (int16_t)((c[2] + rad[2] * d[2]) * GLTF),
            };
            put_vert(vb, j * nt + i, pos, shade(col, d, 1.0f));
        }
    return nt * np;
}

static void shell_tris(int nt, int np)
{
    for (int j = 0; j < np - 1; j++)
        for (int i = 0; i < nt - 1; i++) {
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
    int nt = 9, np = 7, n0, n1 = 0, nt1 = 7, np1 = 7;
    if (s_hoodUp) {   // over the helmet, open at the face
        s_hoodBone = B_HEAD;
        n0 = shell(s_hoodVerts[0], (float[3]){ -0.34f, 1.07f, 0 }, (float[3]){ 1.08f, 1.08f, 0.84f },
                   -128 * R, 128 * R, nt, -42 * R, 90 * R, np, 0.45f, s->clothColor);
    } else {          // gathered round the neck, folds lying on the back
        s_hoodBone = B_SPINE;
        n0 = shell(s_hoodVerts[0], (float[3]){ -0.1f, 2.25f, 0 }, (float[3]){ 1.12f, 0.36f, 1.1f },
                   -135 * R, 135 * R, nt, -80 * R, 80 * R, np, 0, s->clothColor);
        n1 = shell(s_hoodVerts[1], (float[3]){ 0.9f, 1.75f, 0 }, (float[3]){ 0.28f, 0.62f, 0.66f },
                   -90 * R, 90 * R, nt1, -88 * R, 88 * R, np1, 0, s->clothColor);
    }
    rspq_block_begin();
    rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
    rdpq_mode_blender(0);
    rdpq_mode_zbuf(true, true);
    rdpq_mode_alphacompare(0);
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT);
    t3d_vert_load(s_hoodVerts[0], 0, n0);
    shell_tris(nt, np);
    t3d_tri_sync();
    if (n1) {
        t3d_vert_load(s_hoodVerts[1], 0, n1);
        shell_tris(nt1, np1);
        t3d_tri_sync();
    }
    s_hoodBlock = rspq_block_end();
}

/* -----------------------------------------------------------------------------
 * Recording / drawing
 * -------------------------------------------------------------------------- */

static bool filter_plume(void *user, const T3DObject *obj)
{
    (void)user;
    return !(obj->material && obj->material->name && strcmp(obj->material->name, "cloth2") == 0);
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
    static const char *MATS[] = { "knight", "head", "cloth", "cloth2", "chainmail" };
    const SkinColor cols[] = { s->armor, s->helm, s->cloth, s->plume, s->chain };
    color_t saved[5];
    for (int i = 0; i < 5; i++) tint(MATS[i], cols[i], &saved[i]);
    bool hidePlume = s->hidePlume || (s->hood && s_hoodUp);
    rspq_block_begin();
    t3d_model_draw_custom(model, (T3DModelDrawConf){
        .filterCb = hidePlume ? filter_plume : NULL,
        .matrices = skel->bufferCount == 1 ? skel->boneMatricesFP
                                           : (const T3DMat4FP*)t3d_segment_placeholder(T3D_SEGMENT_SKELETON),
    });
    rspq_block_t *block = rspq_block_end();
    for (int i = 0; i < 5; i++) {
        T3DMaterial *m = t3d_model_get_material(model, MATS[i]);
        if (m) m->primColor = saved[i];
    }
    hood_build();
    cloth.valid = false;
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
    if (s->clothKind == CLOTH_NONE) return;

    cloth.cols = s->clothKind == CLOTH_HAIR ? 3 : CLOTH_COLS;
    cloth.rows = CLOTH_ROWS;
    float prev0[3]; memcpy(prev0, cloth.anchor[0], sizeof prev0);
    cloth_anchors(modelMat);
    cloth_bodies(modelMat);
    float dx = cloth.anchor[0][0] - prev0[0], dy = cloth.anchor[0][1] - prev0[1], dz = cloth.anchor[0][2] - prev0[2];
    if (!cloth.valid || dx * dx + dy * dy + dz * dz > 40.0f * 40.0f) cloth_reset();   // first frame or teleport

    // "Back" is the model's +X axis in world space
    float o[3], x1[3], back[3];
    mat4fp_mul_point_f32_row3_colbasis(modelMat, (float[3]){ 0, 0, 0 }, o);
    mat4fp_mul_point_f32_row3_colbasis(modelMat, (float[3]){ GLTF, 0, 0 }, x1);
    for (int i = 0; i < 3; i++) back[i] = (x1[i] - o[i]) / GU;

    if (dt > 0.05f) dt = 0.05f;
    if (dt > 0.0f) { cloth_step(dt * 0.5f, back); cloth_step(dt * 0.5f, back); }
    cloth_draw(pos);
}
