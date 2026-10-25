#include <libdragon.h>
#include "gpu_frame.h"
#include <t3d/t3d.h>
#include <t3d/t3dskeleton.h>
#include <math.h>

#include "character.h"
#include "game/boss/boss.h"

#include "simple_collision_utility.h"
#include "debug_draw.h"
#include "dev.h"

#include "../scenes/guardian/guardian_scene_bounds.h"

#include "globals.h"

// ------------------------------------------------------------
// State (debug + collision endpoints)
// ------------------------------------------------------------

T3DVec3 charCapA;
T3DVec3 charCapB;
float charRadius = 1.0f;

T3DVec3 bossCapA;
T3DVec3 bossCapB;
float bossRadius = 1.0f;
bool bodyHitboxCollision = false;

T3DVec3 bossWeaponCapA;
T3DVec3 bossWeaponCapB;
float bossWeaponRadius = 1.0f;

bool bossWeaponCollision = false;

T3DVec3 charWeaponCapA;
T3DVec3 charWeaponCapB;
float charWeaponRadius = 2.0f;
bool charWeaponCollision = false;

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------

static inline bool circle_vs_circle_push_xz(
    float ax,
    float az,
    float ar,
    float bx,
    float bz,
    float br,
    float out_push[3],
    float out_n[3]
)
{
    float dx = ax - bx;
    float dz = az - bz;

    float r = ar + br;
    float d2 = dx * dx + dz * dz;

    if (d2 >= r * r) return false;

    float d = sqrtf(d2);

    float nx, nz;
    if (d > 1e-6f) {
        nx = dx / d;
        nz = dz / d;
    } else {
        nx = 1.0f;
        nz = 0.0f;
        d = 0.0f;
    }

    float pen = r - d;

    out_n[0] = nx;
    out_n[1] = 0.0f;
    out_n[2] = nz;

    out_push[0] = nx * pen;
    out_push[1] = 0.0f;
    out_push[2] = nz * pen;

    return true;
}

// The Guardian's body leans and strides well out of its upright capsule (its legs ~10 past it a
// tenth of the time, its bent-over torso and head 15-20): the sword also meets it along its
// spine and legs, from the bones (radii from the model's limbs)
#ifdef HLE_LOG_HITVOL
bool hle_limb_only;     // test aid: this frame's sword contact came from the limbs alone
#endif
static bool guardian_body_hit(const Boss *boss, const float a[3], const float b[3], float r)
{
    static const struct { const char *from, *to; float r; } LIMBS[] = {
        { "Waist", "Head", 7.0f },
        { "Waist", "LowerLeg-Left", 5.0f }, { "LowerLeg-Left", "Foot-Left", 4.0f },
        { "Waist", "LowerLeg-Right", 5.0f }, { "LowerLeg-Right", "Foot-Right", 4.0f },
    };
    enum { N = sizeof(LIMBS) / sizeof(LIMBS[0]) };
    static const void *skel;
    static int bone[N][2];
    if (boss->phaseIndex > 2 || boss->mode != BOSS_MODE_COMBAT || !boss->skeleton) return false;
    if (skel != boss->skeleton) {
        skel = boss->skeleton;
        for (int i = 0; i < N; i++) {
            bone[i][0] = t3d_skeleton_find_bone((T3DSkeleton*)boss->skeleton, LIMBS[i].from);
            bone[i][1] = t3d_skeleton_find_bone((T3DSkeleton*)boss->skeleton, LIMBS[i].to);
        }
    }
    for (int i = 0; i < N; i++) {
        T3DVec3 p, q;
        if (!boss_get_bone_world_pos(boss, bone[i][0], &p) || !boss_get_bone_world_pos(boss, bone[i][1], &q)) continue;
        if (scu_capsule_vs_capsule_f(a, b, r, p.v, q.v, LIMBS[i].r)) {
#ifdef HLE_LOG_HITVOL
            if (character_get_state() == CHAR_STATE_ATTACKING && !character.currentAttackHasHit) debugf("[hitvol] sword met the Guardian's %s-%s, outside its capsule\n", LIMBS[i].from, LIMBS[i].to);
#endif
            return true;
        }
    }
    return false;
}

static inline void update_character_capsule_world(void)
{
    /*
     * Character capsule values are gameplay/world-unit local offsets.
     *
     * Do not multiply these by character.scale. character.scale is render/model
     * scale, and applying it here shrinks the gameplay collider dramatically
     * when the character is visually scaled by MODEL_SCALE.
     *
     * This also matches the boss collider path below, which treats capsule
     * offsets/radius as gameplay units.
     */
    charCapA = (T3DVec3){{
        character.pos[0] + character.capsuleCollider.localCapA.v[0],
        character.pos[1] + character.capsuleCollider.localCapA.v[1],
        character.pos[2] + character.capsuleCollider.localCapA.v[2],
    }};

    charCapB = (T3DVec3){{
        character.pos[0] + character.capsuleCollider.localCapB.v[0],
        character.pos[1] + character.capsuleCollider.localCapB.v[1],
        character.pos[2] + character.capsuleCollider.localCapB.v[2],
    }};

    charRadius = character.capsuleCollider.radius;
}

void collision_get_character_capsule_world(float outA[3], float outB[3], float *outR)
{
    update_character_capsule_world();

    if (outA) {
        outA[0] = charCapA.v[0];
        outA[1] = charCapA.v[1];
        outA[2] = charCapA.v[2];
    }

    if (outB) {
        outB[0] = charCapB.v[0];
        outB[1] = charCapB.v[1];
        outB[2] = charCapB.v[2];
    }

    if (outR) {
        *outR = charRadius;
    }
}

static inline bool update_boss_capsule_world(Boss **outBoss)
{
    Boss *boss = boss_get_instance();
    if (!boss) return false;

    bossCapA = (T3DVec3){{
        boss->pos[0] + boss->capsuleCollider.localCapA.v[0],
        boss->pos[1] + boss->capsuleCollider.localCapA.v[1],
        boss->pos[2] + boss->capsuleCollider.localCapA.v[2],
    }};

    bossCapB = (T3DVec3){{
        boss->pos[0] + boss->capsuleCollider.localCapB.v[0],
        boss->pos[1] + boss->capsuleCollider.localCapB.v[1],
        boss->pos[2] + boss->capsuleCollider.localCapB.v[2],
    }};

    bossRadius = boss->capsuleCollider.radius;

    *outBoss = boss;
    return true;
}

static void collision_resolve_character_scene_bounds(void)
{
    const SCU_OBB *roomOBBs = scene_bounds_get_room_obbs();
    int roomOBBCount = scene_bounds_get_room_obb_count();

    if (!roomOBBs || roomOBBCount <= 0) return;

    // More iterations means less corner tunneling / less elastic behavior.
    for (int iter = 0; iter < 8; iter++) {
        update_character_capsule_world();

        float vx, vz;
        character_get_velocity(&vx, &vz);

        bool any = false;

        for (int i = 0; i < roomOBBCount; i++) {
            float push[3];
            float n[3];

            if (scu_capsule_vs_obb_push_xz_f(
                    charCapA.v,
                    charCapB.v,
                    charRadius,
                    &roomOBBs[i],
                    push,
                    n
                ))
            {
                character.pos[0] += push[0];
                character.pos[2] += push[2];

                charCapA.v[0] += push[0];
                charCapA.v[2] += push[2];
                charCapB.v[0] += push[0];
                charCapB.v[2] += push[2];

                float vn = vx * n[0] + vz * n[2];
                if (vn < 0.0f) {
                    vx -= vn * n[0];
                    vz -= vn * n[2];
                }

                any = true;
            }
        }

        character_set_velocity_xz(vx, vz);

        if (!any) break;
    }

    update_character_capsule_world();
}

// ------------------------------------------------------------
// Lifecycle
// ------------------------------------------------------------

void collision_init(void)
{
    bodyHitboxCollision = false;
    bossWeaponCollision = false;
    charWeaponCollision = false;

    update_character_capsule_world();

    Boss *boss = NULL;
    if (update_boss_capsule_world(&boss)) {
        bossWeaponRadius = 5.0f;
    }

    charWeaponRadius = 2.0f;
}

// ------------------------------------------------------------
// Update
// ------------------------------------------------------------

void collision_update(void)
{
    update_character_capsule_world();

    // Static scene bounds should resolve before boss/body hitbox checks.
    collision_resolve_character_scene_bounds();

    Boss *boss = NULL;
    if (!update_boss_capsule_world(&boss)) {
        bodyHitboxCollision = false;
        bossWeaponCollision = false;
        charWeaponCollision = false;
        return;
    }

    // ------------------------------------------------------------
    // BODY vs BODY: resolve in XZ
    // ------------------------------------------------------------
    {
        float charX = 0.5f * (charCapA.v[0] + charCapB.v[0]);
        float charZ = 0.5f * (charCapA.v[2] + charCapB.v[2]);

        // The nearest point of the boss's axis (a lying body, like Shadowborne's, is long;
        // for an upright one this is its centre)
        float ux = bossCapB.v[0] - bossCapA.v[0], uz = bossCapB.v[2] - bossCapA.v[2], ul2 = ux * ux + uz * uz;
        float ut = ul2 > 1e-4f ? ((charX - bossCapA.v[0]) * ux + (charZ - bossCapA.v[2]) * uz) / ul2 : 0.5f;
        ut = ut < 0.0f ? 0.0f : ut > 1.0f ? 1.0f : ut;
        float bossX = bossCapA.v[0] + ux * ut;
        float bossZ = bossCapA.v[2] + uz * ut;

        float push[3];
        float n[3];

        bodyHitboxCollision = circle_vs_circle_push_xz(
            charX,
            charZ,
            charRadius,
            bossX,
            bossZ,
            bossRadius,
            push,
            n
        );

        if (bodyHitboxCollision) {
            character.pos[0] += push[0];
            character.pos[2] += push[2];

            charCapA.v[0] += push[0];
            charCapA.v[2] += push[2];
            charCapB.v[0] += push[0];
            charCapB.v[2] += push[2];

            float vx, vz;
            character_get_velocity(&vx, &vz);

            float vn = vx * n[0] + vz * n[2];
            if (vn < 0.0f) {
                vx -= vn * n[0];
                vz -= vn * n[2];
                character_set_velocity_xz(vx, vz);
            }
            // a big body (a charging dragon) must not shove the player through the walls
            collision_resolve_character_scene_bounds();
        }
    }

    // ------------------------------------------------------------
    // BOSS HAND WEAPON collider
    // ------------------------------------------------------------

    bossWeaponCollision = false;

    if (boss->handAttackColliderActive &&
        boss->skeleton &&
        boss->modelMat &&
        boss->handRightBoneIndex >= 0)
    {
        T3DSkeleton *sk = (T3DSkeleton*)boss->skeleton;

        const T3DMat4FP *B = &sk->boneMatricesFP[boss->handRightBoneIndex];
        const T3DMat4FP *M = (const T3DMat4FP*)boss->modelMat;

        const float p0_local[3] = { 0.0f, 0.0f, 0.0f };

        const float len = 640.0f;
        const float p1_local[3] = { -len, 0.0f, 0.0f };

        float p0_model[3];
        float p1_model[3];

        mat4fp_mul_point_f32_row3_colbasis(B, p0_local, p0_model);
        mat4fp_mul_point_f32_row3_colbasis(B, p1_local, p1_model);

        float p0_world[3];
        float p1_world[3];

        mat4fp_mul_point_f32_row3_colbasis(M, p0_model, p0_world);
        mat4fp_mul_point_f32_row3_colbasis(M, p1_model, p1_world);

        bossWeaponCapA = (T3DVec3){{ p0_world[0], p0_world[1], p0_world[2] }};
        bossWeaponCapB = (T3DVec3){{ p1_world[0], p1_world[1], p1_world[2] }};

        bossWeaponCollision = scu_capsule_vs_capsule_f(
            bossWeaponCapA.v,
            bossWeaponCapB.v,
            bossWeaponRadius,
            charCapA.v,
            charCapB.v,
            charRadius
        );
    }

    // ------------------------------------------------------------
    // CHARACTER HAND WEAPON collider
    // ------------------------------------------------------------

    charWeaponCollision = false;

    {
        static int s_charSwordBoneIndex = -1;

        if (character.skeleton && character.modelMat) {
            if (s_charSwordBoneIndex < 0) {
                s_charSwordBoneIndex = t3d_skeleton_find_bone(
                    (T3DSkeleton*)character.skeleton,
                    "Hand-Right"
                );
            }

            if (s_charSwordBoneIndex >= 0) {
                T3DSkeleton *sk = (T3DSkeleton*)character.skeleton;

                const T3DMat4FP *B = &sk->boneMatricesFP[s_charSwordBoneIndex];
                const T3DMat4FP *M = (const T3DMat4FP*)character.modelMat;

                const float p0_local[3] = { 0.0f, 0.0f, 0.0f };

                const float len = 640.0f;
                const float p1_local[3] = { -len, 0.0f, 0.0f };

                float p0_model[3];
                float p1_model[3];

                mat4fp_mul_point_f32_row3_colbasis(B, p0_local, p0_model);
                mat4fp_mul_point_f32_row3_colbasis(B, p1_local, p1_model);

                float p0_world[3];
                float p1_world[3];

                mat4fp_mul_point_f32_row3_colbasis(M, p0_model, p0_world);
                mat4fp_mul_point_f32_row3_colbasis(M, p1_model, p1_world);

                charWeaponCapA = (T3DVec3){{ p0_world[0], p0_world[1], p0_world[2] }};
                charWeaponCapB = (T3DVec3){{ p1_world[0], p1_world[1], p1_world[2] }};

                charWeaponRadius = 2.0f;

                charWeaponCollision = scu_capsule_vs_capsule_f(
                    charWeaponCapA.v,
                    charWeaponCapB.v,
                    charWeaponRadius,
                    bossCapA.v,
                    bossCapB.v,
                    bossRadius
                );
#ifdef HLE_LOG_HITVOL     // test aid: a swing that landed on a limb: would the capsule alone have met the boss later in it?
                { static bool pending, wasHit;
                  bool att = character_get_state() == CHAR_STATE_ATTACKING && character.currentAttackHasHit;
                  if (!pending && att && !wasHit && hle_limb_only) pending = true;
                  if (pending && att && charWeaponCollision) { debugf("[hitvol] limb hit: the capsule met it later in the swing\n"); pending = false; }
                  if (pending && !att) { debugf("[hitvol] limb hit: the capsule never met it that swing\n"); pending = false; }
                  wasHit = att; }
                hle_limb_only = false;
#endif
                if (!charWeaponCollision && character_get_state() == CHAR_STATE_ATTACKING) {     // (only read mid-swing)
                    charWeaponCollision = guardian_body_hit(boss, charWeaponCapA.v, charWeaponCapB.v, charWeaponRadius);
#ifdef HLE_LOG_HITVOL
                    hle_limb_only = charWeaponCollision;
#endif
                }
            }
        }
    }
}

// ------------------------------------------------------------
// Debug draw
// ------------------------------------------------------------

void collision_draw(T3DViewport *viewport)
{
    if (!DEV_MODE) return;
    if (!debugDraw) return;
    if (!viewport) return;

    gpu_wait();

    // Draw room OBBs, red if character capsule is currently overlapping.
    {
        const SCU_OBB *roomOBBs = scene_bounds_get_room_obbs();
        int roomOBBCount = scene_bounds_get_room_obb_count();

        for (int i = 0; i < roomOBBCount; i++) {
            float push[3];
            float n[3];

            bool hit = scu_capsule_vs_obb_push_xz_f(
                charCapA.v,
                charCapB.v,
                charRadius,
                &roomOBBs[i],
                push,
                n
            );

            scene_bounds_draw_obb_debug(
                viewport,
                &roomOBBs[i],
                0.0f,
                hit ? DEBUG_COLORS[0] : DEBUG_COLORS[2]
            );
        }
    }

    debug_draw_capsule(viewport, &charCapA, &charCapB, charRadius, DEBUG_COLORS[1]);
    debug_draw_capsule(viewport, &bossCapA, &bossCapB, bossRadius, DEBUG_COLORS[3]);

    if (bodyHitboxCollision) {
        T3DVec3 mid = {{
            0.5f * (charCapA.v[0] + charCapB.v[0]),
            0.5f * (charCapA.v[1] + charCapB.v[1]),
            0.5f * (charCapA.v[2] + charCapB.v[2]),
        }};

        debug_draw_cross(viewport, &mid, 5.0f, DEBUG_COLORS[0]);
    }

    Boss *boss = boss_get_instance();
    if (!boss) return;

    if (boss->handAttackColliderActive) {
        debug_draw_capsule(
            viewport,
            &bossWeaponCapA,
            &bossWeaponCapB,
            bossWeaponRadius,
            DEBUG_COLORS[5]
        );
    }

    if (boss->sphereAttackColliderActive) {
        const float OFFSET = 40.0f;
        float yaw = boss->rot[1];

        float fwdX = cosf(yaw);
        float fwdZ = sinf(yaw);

        T3DVec3 center = {{
            boss->pos[0] - fwdX * OFFSET,
            boss->pos[1],
            boss->pos[2] - fwdZ * OFFSET
        }};

        debug_draw_sphere(
            viewport,
            &center,
            20,
            DEBUG_COLORS[5]
        );
    }

    debug_draw_capsule(
        viewport,
        &charWeaponCapA,
        &charWeaponCapB,
        charWeaponRadius,
        DEBUG_COLORS[5]
    );

    if (charWeaponCollision) {
        T3DVec3 mid = {{
            0.5f * (charWeaponCapA.v[0] + charWeaponCapB.v[0]),
            0.5f * (charWeaponCapA.v[1] + charWeaponCapB.v[1]),
            0.5f * (charWeaponCapA.v[2] + charWeaponCapB.v[2]),
        }};

        debug_draw_cross(viewport, &mid, 6.0f, DEBUG_COLORS[0]);
    }
}