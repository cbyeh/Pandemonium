#include "boss_ground_crush.h"
#include "gpu_frame.h"

#include <stdbool.h>
#include <string.h>

#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmodel.h>

#include "../../../utilities/globals.h"
#include "../../../utilities/general_utility.h"

typedef struct {
    bool active;

    float pos[3];
    float age;
    float life;
    float scale;
    float yaw;
    int variant;            // 0 = the model's own texture, else crackAlt[variant - 1]

    T3DMat4FP *mat;
} BossGroundCrush;

static BossGroundCrush groundCrush;

static T3DModel *groundCrushModel = NULL;
static sprite_t *crackAlt[3];       // the Fallen Warrior's other cracks (tools/crack_textures.py)

void boss_ground_crush_init(void)
{
    memset(&groundCrush, 0, sizeof(groundCrush));

    groundCrush.mat = malloc_uncached(sizeof(T3DMat4FP));
    groundCrush.scale = MODEL_SCALE;

    groundCrushModel = t3d_model_load("rom:/boss/ground_crush.t3dm");
    crackAlt[0] = sprite_load("rom:/boss/ground_crushed_b.ia8.sprite");
    crackAlt[1] = sprite_load("rom:/boss/ground_crushed_c.ia8.sprite");
    crackAlt[2] = sprite_load("rom:/boss/ground_crushed_d.ia8.sprite");
}

void boss_ground_crush_reset(void)
{
    groundCrush.active = false;

    groundCrush.pos[0] = 0.0f;
    groundCrush.pos[1] = 0.0f;
    groundCrush.pos[2] = 0.0f;

    groundCrush.age = 0.0f;
    groundCrush.life = 0.0f;
    groundCrush.scale = MODEL_SCALE;
}

void boss_ground_crush_spawn(float x, float y, float z)
{
    boss_ground_crush_spawn_sized(x, y, z, 1.0f);
}

void boss_ground_crush_spawn_sized(float x, float y, float z, float size)
{
    groundCrush.active = true;

    groundCrush.pos[0] = x;
    groundCrush.pos[1] = y;
    groundCrush.pos[2] = z;

    groundCrush.age = 0.0f;
    groundCrush.life = 3.0f;
    groundCrush.scale = MODEL_SCALE * size;
    groundCrush.yaw = 0.0f;
    groundCrush.variant = 0;
}

void boss_ground_crush_spawn_varied(float x, float y, float z, float size, float yaw)
{
    int last = groundCrush.variant;
    boss_ground_crush_spawn_sized(x, y, z, size);
    groundCrush.yaw = yaw;
    groundCrush.variant = (last + 1 + rand_custom_u32() % 3) % 4;     // never the same twice running
}

void boss_ground_crush_update(float dt)
{
    if (!groundCrush.active) return;

    groundCrush.age += dt;

    if (groundCrush.age > groundCrush.life) {
        groundCrush.active = false;
    }
}

void boss_ground_crush_draw(void)
{
    if (!groundCrush.active) return;
    if (!groundCrush.mat || !groundCrushModel) return;

    float alpha = 1.0f;

    const float fadeTime = 0.5f;
    const float fadeStart = groundCrush.life - fadeTime;

    if (groundCrush.age >= fadeStart) {
        alpha = (groundCrush.life - groundCrush.age) / fadeTime;

        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;
    }

    uint8_t a = (uint8_t)(alpha * 255.0f);

    t3d_mat4fp_from_srt_euler(
        groundCrush.mat,
        (float[3]){ groundCrush.scale, groundCrush.scale, groundCrush.scale },
        (float[3]){ 0.0f, groundCrush.yaw, 0.0f },
        (float[3]){ groundCrush.pos[0], groundCrush.pos[1], groundCrush.pos[2] }
    );

    t3d_matrix_set(gpu_mat(groundCrush.mat), true);

    rdpq_set_prim_color(RGBA32(255, 255, 255, a));

    // Another crack: its texture swapped in for this draw only
    T3DMaterial *m = NULL;
    sprite_t *own = NULL;
    if (groundCrush.variant > 0 && crackAlt[groundCrush.variant - 1]) {
        T3DModelIter it = t3d_model_iter_create(groundCrushModel, T3D_CHUNK_TYPE_OBJECT);
        if (t3d_model_iter_next(&it) && it.object->material) {
            m = it.object->material;
            own = m->textureA.texture;
            m->textureA.texture = crackAlt[groundCrush.variant - 1];
        }
    }
    t3d_model_draw(groundCrushModel);
    if (m) m->textureA.texture = own;
}

void boss_ground_crush_cleanup(void)
{
    if (groundCrush.mat) {
        free_uncached(groundCrush.mat);
        groundCrush.mat = NULL;
    }

    if (groundCrushModel) {
        t3d_model_free(groundCrushModel);
        groundCrushModel = NULL;
    }
    for (int i = 0; i < 3; i++) {
        if (crackAlt[i]) { sprite_free(crackAlt[i]); crackAlt[i] = NULL; }
    }

    memset(&groundCrush, 0, sizeof(groundCrush));
}