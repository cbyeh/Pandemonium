#include "weapon.h"
#include <libdragon.h>
#include <t3d/t3d.h>
#include <math.h>
#include <string.h>

#include "floor_fire.h"
#include "proc_mesh.h"
#include "gpu_frame.h"
#include "character.h"
#include "game/boss/boss.h"
#include "game/tower/tower.h"
#include "controllers/audio_controller.h"
#include "../scenes/guardian/guardian_scene_sfx.h"

// Shadowborne's ground flames (dragon_boss.c): an octahedron each in its fire's two colours, burning on
// the floor with its own tilt, turn and stretch, tumbling on; grown in, then shrinking as it dies (the hit
// shrinks with it). A blade's slam: one where it bites, four round it 9 out (its fireball's patch); a body's (the Sun's
// dive): eight round its edge
#ifndef FF_MAX
#define FF_MAX    10        // two patches (a third's puts out the oldest flames): 16 cost a Triangle Slam on a loop 57.2 fps in Ares, 10 58.4
#endif
#define FF_LIFE   3.0f      // a blade's patch burns 3 s (2 s cost the Fallen Warrior's Elite x1.38, under the 1.5-1.7 band)
#define FF_LIFE_RING 2.0f   // a body's ring 2 s (it shuts the whole opening round the boss: 3 s cost the Sun's x1.78, over it)
#define FF_SIZE   12.0f
#define FF_RING   9.0f      // a blade's patch
#define FF_TICK   0.5f      // a burn every 0.5 s (in it)
#define FF_DPS    6.0f      // felt: after the boss's own damage scale
#define PLAYER_R  6.0f      // the player's capsule axis (feet + 4 to + 16), 8 round less 2 of grace, as Shadowborne's hits

typedef struct { float p[3], age, life, size; uint8_t patch; } Flame;
static Flame s_fl[FF_MAX];
static int s_next;
static ProcMesh s_mesh;
static bool s_built;
static float s_hitCd;
static uint8_t s_patch, s_spent;    // (HLE_FIRE_ONCE: the patch last burnt; the metric counts a lingering hazard once)

static float flame_r(float size, float age, float life)     // as drawn (the octahedron, ~0.8 of its points' reach)
{
    float u = age / life;
    return 0.8f * size * (0.4f + 0.6f * fminf(1.0f, u * 2.0f)) * (1.0f - u * u);
}

int floor_fire_live(void) { int n = 0; for (int i = 0; i < FF_MAX; i++) n += s_fl[i].age < s_fl[i].life; return n; }
bool floor_fire_near(float x, float z, float r)
{
    for (int i = 0; i < FF_MAX; i++) {
        const Flame *f = &s_fl[i];
        if (f->age >= f->life) continue;
        float dx = f->p[0] - x, dz = f->p[2] - z, rr = r + flame_r(f->size, f->age, f->life);
        if (dx * dx + dz * dz < rr * rr) return true;
    }
    return false;
}

void floor_fire_slam(float x, float y, float z, float ring)
{
    if (!tower_affix(AFX_SMOULDERING)) return;
    if (!s_built) {     // (Shadowborne's glow: dragon_boss.c build_glow)
        static const float v[6][3] = { { 8, 0, 0 }, { -8, 0, 0 }, { 0, 8, 0 }, { 0, -8, 0 }, { 0, 0, 8 }, { 0, 0, -8 } };
        uint32_t a = proc_rgb(255, 120, 30), b = proc_rgb(255, 200, 70);
        proc_mesh_begin();
        for (int i = 0; i < 8; i++) proc_mesh_tri(v[(i & 1) ? 0 : 1], v[(i & 2) ? 2 : 3], v[(i & 4) ? 4 : 5], (i % 3) ? a : b, true);
        proc_mesh_end(&s_mesh);
        s_built = true;
    }
    bool body = ring > FF_RING;     // (round a body: no flame under it)
    s_patch = (uint8_t)(s_patch + 1) ? (uint8_t)(s_patch + 1) : 1;
    int n = body ? 8 : 5;
    for (int k = 0; k < n; k++) {
        float ang = (body ? k : k - 1) * (2.0f * T3D_PI / (body ? 8.0f : 4.0f)) + 0.4f * (float)(s_next & 3), rr = body || k ? ring : 0.0f;
        Flame *f = &s_fl[s_next];
        s_next = (s_next + 1) % FF_MAX;
        f->p[0] = x + fm_sinf(ang) * rr; f->p[1] = y + 2.0f; f->p[2] = z + fm_cosf(ang) * rr;
        f->age = 0.0f; f->life = body ? FF_LIFE_RING : FF_LIFE; f->size = FF_SIZE + 2.0f * (float)((s_next * 7) % 5) / 4.0f; f->patch = s_patch;
    }
    // it catches with Shadowborne's fire, heard from where it burns (the slam's own blow is its boss's)
    float dx = character.pos[0] - x, dz = character.pos[2] - z;
    audio_play_scene_sfx_dist(SCENE1_SFX_FIRE_DRAGON, 0.45f * BONUS_SFX_VOL, sqrtf(dx * dx + dz * dz));
#ifdef HLE_LOG_AFFIX
    debugf("[affix] Smouldering: a slam at %.0f,%.0f (%.0f from the player), %d flames burning\n", (double)x, (double)z, (double)sqrtf(dx * dx + dz * dz), floor_fire_live());
#endif
}

void floor_fire_update(float dt)
{
    if (s_hitCd > 0.0f) s_hitCd -= dt;
    Boss *b = boss_get_instance();
    bool live = b && b->mode == BOSS_MODE_COMBAT && b->health > 0.0f;
    for (int i = 0; i < FF_MAX; i++) {
        Flame *f = &s_fl[i];
        if (f->age >= f->life) continue;
        f->age += dt;
        if (!live) { f->age = f->life; continue; }      // (its fall puts them out, as the bosses' hazards)
        if (s_hitCd > 0.0f) continue;
#ifdef HLE_FIRE_ONCE    // test aid (the metric): a patch burns the player once (it steps out)
        if (f->patch == s_spent) continue;
#endif
        // the player's axis, feet + 4 to + 16, against the flame as drawn
        float dx = character.pos[0] - f->p[0], dz = character.pos[2] - f->p[2], y0 = character.pos[1] + 4.0f, y1 = character.pos[1] + 16.0f;
        float dy = f->p[1] < y0 ? y0 - f->p[1] : f->p[1] > y1 ? f->p[1] - y1 : 0.0f;
        float gap = sqrtf(dx * dx + dy * dy + dz * dz) - flame_r(f->size, f->age, f->life);
        if (gap >= PLAYER_R) continue;
#ifdef HLE_HUMAN_BOT
        { extern int hb_src; hb_src = 1; }   // test aid: Smouldering's floor fire
#endif
        character_apply_damage_kind(FF_DPS * FF_TICK / boss_damage_scale(), DMG_FIRE);
        s_hitCd = FF_TICK;
        s_spent = f->patch;
        audio_play_scene_sfx_dist(SCENE1_SFX_FIRE, 0.6f * BONUS_SFX_VOL, 0.0f);    // (as the Fallen Warrior's fire: at the player)
#ifdef HLE_LOG_AFFIX
        debugf("[affix] Smouldering: burnt, %.1f off the flame's edge (as drawn %.1f round)\n", (double)(gap - PLAYER_R), (double)flame_r(f->size, f->age, f->life));
#endif
    }
}

void floor_fire_draw(void)
{
    if (!s_built || !s_mesh.block) return;
    bool any = false;
    for (int i = 0; i < FF_MAX && !any; i++) any = s_fl[i].age < s_fl[i].life;
    if (!any) return;
    // Alpha-blended, not additive (the real RDP wraps an overflowing sum), as Shadowborne's
    rdpq_sync_pipe();
    rdpq_mode_push();
    rdpq_set_mode_standard();
    t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT);
    rdpq_mode_zbuf(true, false);
    rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, SHADE), (0, 0, 0, PRIM)));
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_set_prim_color(RGBA32(255, 255, 255, 170));
    for (int i = 0; i < FF_MAX; i++) {
        const Flame *f = &s_fl[i];
        if (f->age >= f->life) continue;
        float u = f->age / f->life, sz = f->size * (0.4f + 0.6f * fminf(1.0f, u * 2.0f)) * (1.0f - u * u);
        // its own tilt and stretch (a slot's), turning on (dragon_boss_draw's ground flames)
        float tb = i * 1.7f + 0.6f, cb = fm_cosf(tb), sb = fm_sinf(tb);
        float k0 = (0.85f + 0.3f * ((i * 37) % 7) / 6.0f) / 8.0f, k1 = (0.85f + 0.3f * ((i * 53) % 5) / 4.0f) / 8.0f, k2 = (0.8f + 0.4f * ((i * 71) % 9) / 8.0f) / 8.0f;
        float a = i * 2.39996f + f->age * (2.0f + (i & 3)), ca = fm_cosf(a), sa = fm_sinf(a);
        float x[3] = { ca, sa * sb, sa * cb }, y[3] = { 0.0f, cb, -sb }, z[3] = { -sa, ca * sb, ca * cb };
        float sx = sz * k0 / 8.0f, sy = sz * k1 / 8.0f, sz2 = sz * k2 / 8.0f;     // (proc_mesh stores 1/8 units)
        if (g_viewFrustum) {
            T3DVec3 c = {{ f->p[0], f->p[1], f->p[2] }};
            if (!t3d_frustum_vs_sphere(g_viewFrustum, &c, s_mesh.br * fmaxf(sx, fmaxf(sy, sz2)) + 2.0f)) continue;
        }
        T3DMat4FP *m = gpu_alloc(sizeof(T3DMat4FP));
        T3DMat4 M = {{
            { x[0] * sx, x[1] * sx, x[2] * sx, 0 }, { y[0] * sy, y[1] * sy, y[2] * sy, 0 },
            { z[0] * sz2, z[1] * sz2, z[2] * sz2, 0 }, { f->p[0], f->p[1], f->p[2], 1 },
        }};
        t3d_mat4_to_fixed(m, &M);
        t3d_matrix_set(m, true);
        rspq_block_run(s_mesh.block);
    }
    rdpq_mode_pop();
}

void floor_fire_free(void)
{
    for (int i = 0; i < FF_MAX; i++) s_fl[i].age = s_fl[i].life;
    if (s_built) { gpu_wait(); proc_mesh_free(&s_mesh); s_built = false; }
}
