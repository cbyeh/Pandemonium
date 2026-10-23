#include "boss_render.h"
#include "boss.h"
#include "boss_ui.h"

#include <libdragon.h>
#include "gpu_frame.h"
#include <t3d/t3d.h>
#include <t3d/t3dskeleton.h>
#include <t3d/t3dmodel.h>
#include <math.h>
#include <ctype.h>
#include <string.h>

#include "dev.h"
#include "dev/debug_draw.h"
#include "character.h"
#include "globals.h"
#include "general_utility.h"

// Shadow tuning (duplicated from boss.c for rendering alpha)
static const float BOSS_SHADOW_GROUND_Y = -1.0f;  // Match roomY floor level
static const float BOSS_JUMP_REF_HEIGHT = 120.0f;
static const float BOSS_SHADOW_BASE_ALPHA = 120.0f;

// Damage flash material tint cache: t3d sets prim color per material from the
// model asset, so an external rdpq_set_prim_color() gets clobbered. We mutate
// each material's primColor while flashing and restore on flash end.
static T3DMaterial** s_flashMats = NULL;
static color_t*      s_flashOrigPrim = NULL;
static uint8_t*      s_flashOrigFlags = NULL;
static int           s_flashMatCount = 0;
static const T3DModel* s_flashCachedModel = NULL;
static bool          s_flashTintApplied = false;

static void boss_flash_cache_init(const T3DModel* model)
{
    if (s_flashCachedModel == model && s_flashMats) return;

    // Free any previous cache (model changed).
    if (s_flashMats) {
        free(s_flashMats);
        s_flashMats = NULL;
    }

    if (s_flashOrigPrim) {
        free(s_flashOrigPrim);
        s_flashOrigPrim = NULL;
    }

    if (s_flashOrigFlags) {
        free(s_flashOrigFlags);
        s_flashOrigFlags = NULL;
    }

    s_flashMatCount = 0;
    s_flashCachedModel = NULL;
    s_flashTintApplied = false;

    if (!model) return;

    int count = 0;
    T3DModelIter it = t3d_model_iter_create(model, T3D_CHUNK_TYPE_MATERIAL);

    while (t3d_model_iter_next(&it)) {
        count++;
    }

    if (count <= 0) return;

    s_flashMats = (T3DMaterial**)malloc(sizeof(T3DMaterial*) * count);
    s_flashOrigPrim = (color_t*)malloc(sizeof(color_t) * count);
    s_flashOrigFlags = (uint8_t*)malloc(sizeof(uint8_t) * count);

    if (!s_flashMats || !s_flashOrigPrim || !s_flashOrigFlags) {
        free(s_flashMats);
        s_flashMats = NULL;

        free(s_flashOrigPrim);
        s_flashOrigPrim = NULL;

        free(s_flashOrigFlags);
        s_flashOrigFlags = NULL;

        return;
    }

    int i = 0;

    it = t3d_model_iter_create(model, T3D_CHUNK_TYPE_MATERIAL);

    while (t3d_model_iter_next(&it)) {
        T3DMaterial* m = it.material;

        s_flashMats[i] = m;
        s_flashOrigPrim[i] = m->primColor;
        s_flashOrigFlags[i] = m->setColorFlags;

        i++;
    }

    s_flashMatCount = i;
    s_flashCachedModel = model;
}

// tintAmount in [0..1]: 0 = fully original color, 1 = fully red.
// Each material lerps from its own original primColor toward red, so the
// fade returns smoothly to per-material colors (e.g. yellow swords, red cloth)
// instead of routing through a shared white and popping back at the end.
static void boss_flash_apply_tint(float tintAmount)
{
    if (!s_flashMats) return;

    if (tintAmount < 0.0f) tintAmount = 0.0f;
    if (tintAmount > 1.0f) tintAmount = 1.0f;

    const float ta = tintAmount;
    const float ka = 1.0f - tintAmount;

    // Muted dark red target so the flash reads as "hurt" without blowing out.
    const float tr = 100.0f;
    const float tg = 20.0f;
    const float tb = 20.0f;

    for (int i = 0; i < s_flashMatCount; i++) {
        T3DMaterial* m = s_flashMats[i];
        color_t o = s_flashOrigPrim[i];

        uint8_t r = (uint8_t)(o.r * ka + tr * ta);
        uint8_t g = (uint8_t)(o.g * ka + tg * ta);
        uint8_t b = (uint8_t)(o.b * ka + tb * ta);

        m->primColor = (color_t){r, g, b, o.a};
        m->setColorFlags = s_flashOrigFlags[i] | 0b001;
    }

    s_flashTintApplied = true;
}

static void boss_flash_restore(void)
{
    if (!s_flashMats || !s_flashTintApplied) return;

    for (int i = 0; i < s_flashMatCount; i++) {
        T3DMaterial* m = s_flashMats[i];

        m->primColor = s_flashOrigPrim[i];
        m->setColorFlags = s_flashOrigFlags[i];
    }

    s_flashTintApplied = false;
}

ScrollDyn bossScrollDyn = {
    .xSpeed = 0.0f,
    .ySpeed = 30.0f,
    .scale  = 64.0f,
    .spr = NULL,
};

typedef struct {
    ScrollDyn* scroll;
    Boss* boss;
} BossDrawUserData;

static void boss_scroll_dyn_cb_wrapper(
    void* userData,
    const T3DMaterial* material,
    rdpq_texparms_t* tp,
    rdpq_tile_t tile
) {
    BossDrawUserData* ud = (BossDrawUserData*)userData;
    ScrollDyn* scroll = ud ? ud->scroll : NULL;

    scroll_dyn_cb(scroll, material, tp, tile);
}

void boss_draw_init(void)
{
    // This texture is only needed for the scrolling/fog material. If it is not
    // available yet, keep it NULL and fall back to regular drawing.
    if (!bossScrollDyn.spr) {
        bossScrollDyn.spr = sprite_load("rom:/boss_room/fog.i8.sprite");
    }
}

// The model is recorded into blocks once (re-emitting it cost the CPU 4.6 ms a frame, the
// Guardian fight's largest stage): the main body at four flash tints, the temporary swords
// apart (hidden once it is dead). Only the parts with a scrolling (dynamic) texture are drawn
// live each frame. The bones are read from memory when a block runs, so it still animates.
#define FLASH_LEVELS 4
static const T3DModel *s_recModel;
static rspq_block_t *s_recMain[FLASH_LEVELS], *s_recSwords;

static bool is_temp_sword(const T3DObject *obj)
{
    const char *m = (obj && obj->material && obj->material->name) ? obj->material->name : NULL;
    return m && (strcmp(m, "z_temp_swords") == 0 || strcmp(m, "z_temp_swords_decal") == 0);
}
static bool is_dynamic(const T3DObject *obj)
{
    return obj && obj->material && (obj->material->textureA.texReference || obj->material->textureB.texReference);
}
static bool filter_main(void *u, const T3DObject *obj) { (void)u; return !is_dynamic(obj) && !is_temp_sword(obj); }
static bool filter_swords(void *u, const T3DObject *obj) { (void)u; return !is_dynamic(obj) && is_temp_sword(obj); }

static void boss_record_blocks(Boss *boss)
{
    T3DSkeleton* skel = (T3DSkeleton*)boss->skeleton;
    (void)skel;
    const T3DMat4FP *mats = gpu_skeleton_matrices();     // this frame's copy (boss_render_draw)
    if (s_recModel) {
        gpu_wait();
        for (int k = 0; k < FLASH_LEVELS; k++) rspq_block_free(s_recMain[k]);
        rspq_block_free(s_recSwords);
    }
    for (int k = 0; k < FLASH_LEVELS; k++) {
        if (k) boss_flash_apply_tint(0.35f * k / (FLASH_LEVELS - 1)); else boss_flash_restore();
        gpu_block_begin();
        t3d_model_draw_custom(boss->model, (T3DModelDrawConf){ .filterCb = filter_main, .matrices = mats });
        s_recMain[k] = gpu_block_end();
    }
    boss_flash_restore();
    gpu_block_begin();
    t3d_model_draw_custom(boss->model, (T3DModelDrawConf){ .filterCb = filter_swords, .matrices = mats });
    s_recSwords = gpu_block_end();
    s_recModel = boss->model;
}

static bool filter_dynamic(void *u, const T3DObject *obj) { (void)u; return is_dynamic(obj); }

/** Frees the recorded blocks (the model is about to go: a later one may reuse its address). */
void boss_render_forget_model(void)
{
    if (!s_recModel) return;
    gpu_wait();
    for (int k = 0; k < FLASH_LEVELS; k++) rspq_block_free(s_recMain[k]);
    rspq_block_free(s_recSwords);
    s_recModel = NULL;
}

static void boss_draw_scrolling(Boss* boss, const T3DMat4FP *modelMat)
{
    BossDrawUserData ud = {
        .scroll = &bossScrollDyn,
        .boss = boss,
    };

    T3DSkeleton* skel = (T3DSkeleton*)boss->skeleton;

    // If the scrolling texture was not loaded, avoid the custom dyn texture path.
    // This prevents startup crashes if the sprite is not present in the current ROM.
    if (!bossScrollDyn.spr) {
        t3d_matrix_set(modelMat, true);

        t3d_model_draw_custom(boss->model, (T3DModelDrawConf){
            .userData = &ud,
            .tileCb = NULL,
            .filterCb = filter_dynamic,
            .dynTextureCb = NULL,
            .matrices = (skel && skel->bufferCount == 1)
                ? skel->boneMatricesFP
                : (const T3DMat4FP*)t3d_segment_placeholder(T3D_SEGMENT_SKELETON),
        });

        return;
    }

    t3d_matrix_set(modelMat, true);

    t3d_model_draw_custom(boss->model, (T3DModelDrawConf){
        .userData = &ud,
        .tileCb = NULL,
        .filterCb = filter_dynamic,
        .dynTextureCb = boss_scroll_dyn_cb_wrapper,
        .matrices = (skel && skel->bufferCount == 1)
            ? skel->boneMatricesFP
            : (const T3DMat4FP*)t3d_segment_placeholder(T3D_SEGMENT_SKELETON),
    });
}

// Draw only the shadow - should be called in a batched shadow pass with zbuf(false, false)
void boss_draw_shadow(Boss* boss)
{
    if (!boss || !boss->visible) return;
    if (!boss->dpl_shadow || !boss->shadowMat) return;

    // Compute alpha like character: fade with height.
    float h = boss->pos[1] - BOSS_SHADOW_GROUND_Y;
    if (h < 0.0f) h = 0.0f;

    float t = (BOSS_JUMP_REF_HEIGHT > 0.0f) ? (h / BOSS_JUMP_REF_HEIGHT) : 0.0f;
    if (t > 1.0f) t = 1.0f;

    float fade = 1.0f - t;
    fade *= fade;

    uint8_t a = (uint8_t)(BOSS_SHADOW_BASE_ALPHA * fade);

    if (a > 0) {
        rdpq_set_prim_color(RGBA32(0, 0, 0, a));
        t3d_matrix_set(gpu_mat((T3DMat4FP*)boss->shadowMat), true);
        rspq_block_run((rspq_block_t*)boss->dpl_shadow);
    }
}

void boss_render_draw(Boss* boss)
{
    if (!boss || !boss->visible) return;

    // Be defensive: render might be called before init is fully complete.
    if (!boss->model || !boss->modelMat) return;

    // Shadow is now drawn separately via boss_draw_shadow() in a batched pass.
    // This avoids expensive mode changes per boss.

    boss_flash_cache_init((const T3DModel*)boss->model);
    if (s_recModel != boss->model) boss_record_blocks(boss);

    float tint = 0.0f;
    if (boss->damageFlashTimer > 0.0f) {
        // 0.25s window: hold peak tint for the first half, then fade out.
        // Peak is intentionally well below 1.0 so the boss reads as "hurt"
        // without being repainted solid red.
        const float kMaxTint = 0.35f;

        float f = boss->damageFlashTimer / 0.25f;

        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;

        tint = (f >= 0.5f)
            ? kMaxTint
            : (f * 2.0f * kMaxTint);

        boss_flash_apply_tint(tint);
    } else if (s_flashTintApplied) {
        boss_flash_restore();
    }

    // The recorded body at the nearest flash tint, the swords while it lives, the scrolling parts live
    int level = (int)(tint / 0.35f * (FLASH_LEVELS - 1) + 0.5f);
    if (level < 0) level = 0; else if (level > FLASH_LEVELS - 1) level = FLASH_LEVELS - 1;
    const T3DMat4FP *modelMat = gpu_mat(boss->modelMat);
    T3DSkeleton *skel = (T3DSkeleton*)boss->skeleton;
    T3DMat4FP *bones = skel ? gpu_skeleton_begin(skel) : NULL;
    t3d_matrix_set(modelMat, true);
    rspq_block_run(s_recMain[level]);
    if (boss->state != BOSS_STATE_DEAD) rspq_block_run(s_recSwords);
    boss_draw_scrolling(boss, modelMat);
    if (skel) gpu_skeleton_end(skel, bones);

    // Draw sword attached to Hand-Right bone.
    if (boss->handRightBoneIndex >= 0 && boss->swordDpl && boss->swordMatFP) {
        T3DSkeleton* skel = (T3DSkeleton*)boss->skeleton;

        if (skel) {
            // Push bone matrix, then sword's local transform matrix.
            // t3d_matrix_push(&skel->boneMatricesFP[boss->handRightBoneIndex]);
            //     t3d_matrix_push((T3DMat4FP*)boss->swordMatFP);
            //     rspq_block_run((rspq_block_t*)boss->swordDpl);
            // t3d_matrix_pop(2);
        }
    }
}

static const char* boss_render_get_state_name(BossState state)
{
    static const char* stateNames[] = {
        "Intro",
        "Neutral",
        "Chase",
        "Strafe",
        "Recover",
        "Stagger",
        "Dead",
        "Lunge",
        "Power Jump",
        "Combo",
        "Combo Starter",
        "Tracking Slam",
        "Flip Attack",
        "Lunge Starter",
        "Smash",
        "Quick Attack",
        "Aerial Sword Barrage",
        "Ground Sweep",
    };

    if (state < 0 || state >= BOSS_STATE_GROUND_SWEEP + 1) {
        return "Unknown";
    }

    return stateNames[state];
}

static const char* boss_render_get_mode_name(BossMode mode)
{
    switch (mode) {
        case BOSS_MODE_INACTIVE:
            return "Inactive";

        case BOSS_MODE_CINEMATIC:
            return "Cinematic";

        case BOSS_MODE_COMBAT:
            return "Combat";

        case BOSS_MODE_POST_DEFEAT:
            return "Post Defeat";

        default:
            return "Unknown";
    }
}

void boss_render_debug(Boss* boss, void* viewport)
{
    if (!boss || !viewport) return;

    T3DViewport* vp = (T3DViewport*)viewport;

    // Boss combat UI/debug should only draw during combat.
    // Cutscenes and scene state are intentionally not referenced here.
    if (!boss_is_combat_active(boss)) {
        return;
    }

    if (boss->health <= 0.0f || !boss->visible) {
        return;
    }

    // Boss health bar.
    float ratio = boss->maxHealth > 0.0f
        ? fmaxf(0.0f, fminf(1.0f, boss->health / boss->maxHealth))
        : 0.0f;

    float flash = 0.0f;

    if (boss->damageFlashTimer > 0.0f) {
        flash = fminf(1.0f, boss->damageFlashTimer / 0.25f);
    }

    boss_ui_draw_health_bar(boss->name, ratio, flash);
    { extern float gameTime; boss_ui_draw_poise_bar(boss->poise, boss->staggerT > 0.0f, gameTime); }

    if (!DEV_MODE || !debugDraw) {
        return;
    }

    // Display debug info.
    float dx = character.pos[0] - boss->pos[0];
    float dy = character.pos[1] - boss->pos[1];
    float dz = character.pos[2] - boss->pos[2];

    float dist = sqrtf(dx * dx + dy * dy + dz * dz);

    rdpq_set_prim_color(RGBA32(255, 255, 255, 255));

    int y = 48;
    int listSpacing = 12;

    rdpq_text_printf(
        NULL,
        FONT_UNBALANCED,
        20,
        y,
        "Boss Mode: %s",
        boss_render_get_mode_name(boss_get_mode(boss))
    );
    y += listSpacing;

    rdpq_text_printf(
        NULL,
        FONT_UNBALANCED,
        20,
        y,
        "Boss State: %s",
        boss_render_get_state_name(boss->state)
    );
    y += listSpacing;

    rdpq_text_printf(NULL, FONT_UNBALANCED, 20, y, "Boss Dist: %.1f", dist);
    y += listSpacing;

    if (boss->attackNameDisplayTimer > 0.0f && boss->currentAttackName) {
        rdpq_text_printf(NULL, FONT_UNBALANCED, 20, y, "Attack: %s", boss->currentAttackName);
        y += listSpacing;
    }

    // Animation blending stats.
    if (boss->isBlending) {
        y += listSpacing;

        rdpq_set_prim_color(RGBA32(0x39, 0xBF, 0x1F, 0xFF));
        rdpq_text_printf(NULL, FONT_UNBALANCED, 20, y, "Blending: ON");
        y += listSpacing;

        rdpq_set_prim_color(RGBA32(255, 255, 255, 255));

        rdpq_text_printf(
            NULL,
            FONT_UNBALANCED,
            20,
            y,
            "Blend Factor: %.2f (%.0f%%)",
            boss->blendFactor,
            boss->blendFactor * 100.0f
        );
        y += listSpacing;

        rdpq_text_printf(
            NULL,
            FONT_UNBALANCED,
            20,
            y,
            "Blend Timer: %.3fs / %.3fs",
            boss->blendTimer,
            boss->blendDuration
        );
    } else {
        y += listSpacing;

        rdpq_set_prim_color(RGBA32(0x66, 0x66, 0x66, 0xFF));
        rdpq_text_printf(NULL, FONT_UNBALANCED, 20, y, "Blending: OFF");

        rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
    }

    // Draw boss targeting debug visualization.
    T3DVec3 targetPos = {{
        boss->debugTargetingPos[0],
        boss->debugTargetingPos[1],
        boss->debugTargetingPos[2],
    }};

    debug_draw_sphere(vp, &targetPos, 4.0f, DEBUG_COLORS[5]);
    debug_draw_cross(vp, &targetPos, 4.0f, DEBUG_COLORS[5]);
}