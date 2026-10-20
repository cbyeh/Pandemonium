#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>
#include <t3d/t3dmodel.h>
#include <math.h>
#include <string.h>

#include "guardian_ending.h"
#include "character.h"
#include "globals.h"
#include "camera_controller.h"
#include "audio_controller.h"
#include "general_utility.h"
#include "../../managers/cutscene_manager.h"
#include "../../fx/proc_mesh.h"
#include "../../fx/skill_fx.h"
#include "../../fx/screen_shake.h"

// ---- Staging (world units; the hall runs along +x) ----
#define FLOOR_Y      4.0f
#define THRONE_X     60.0f              // the King sits here, facing down the hall (-x)
#define WALK_FROM_X  (-320.0f)
#define WALK_TO_X    (-40.0f)
#define WALK_END_T   14.0f
#define VOID_Y       (-420.0f)          // the vortex hangs in the dark below the hall
#define END_T        60.0f              // the soundtrack's length

static const float KING_HEAD[3] = { THRONE_X + 2.0f, FLOOR_Y + 47.0f, 0.0f };
static const float ARM_PIVOT[3] = { THRONE_X + 2.0f, FLOOR_Y + 39.0f, 0.0f };

static bool s_ready;
static float s_t;
static int s_line;
static ProcMesh s_throne, s_king, s_arms, s_gold, s_sun;
static T3DModel *s_raysModel;
static T3DMat4FP *s_raysMat;
static ScrollParams s_raysScroll = { .xSpeed = 30.0f, .ySpeed = 0.0f, .scale = 64 };

static void camera_for(float t);

static inline float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
static inline float smooth(float t0, float t1, float t)
{
    float u = clamp01((t - t0) / (t1 - t0));
    return u * u * (3.0f - 2.0f * u);
}
static inline void lerp3(float out[3], const float a[3], const float b[3], float u)
{
    for (int i = 0; i < 3; i++) out[i] = a[i] + (b[i] - a[i]) * u;
}

// ---- The King, his throne and his gold (built once) ----

static uint32_t s_rng = 12345;
static float rnd(void) { s_rng = s_rng * 1103515245u + 12345u; return (float)((s_rng >> 8) & 0xFFFF) / 65535.0f; }

static void build_throne(void)
{
    const uint32_t stone = proc_rgb(48, 42, 52), dark = proc_rgb(26, 22, 30);
    proc_mesh_begin();
    proc_mesh_box((float[3]){ 0, 4, 0 }, (float[3]){ 22, 4, 24 }, stone);           // dais
    proc_mesh_box((float[3]){ 0, 13, 0 }, (float[3]){ 13, 3, 16 }, dark);           // seat
    proc_mesh_box((float[3]){ 12, 46, 0 }, (float[3]){ 3, 36, 19 }, dark);          // back
    proc_mesh_box((float[3]){ -1, 21, 17 }, (float[3]){ 12, 2, 2.5f }, stone);       // armrests
    proc_mesh_box((float[3]){ -1, 21, -17 }, (float[3]){ 12, 2, 2.5f }, stone);
    // A crown of blades along the top of the back, tallest in the middle
    for (int i = -3; i <= 3; i++) {
        float z = i * 6.0f, h = 30.0f - fabsf((float)i) * 6.0f;
        proc_mesh_spike((float[3]){ 12, 80, z }, (float[3]){ 13, 80 + h, z * 1.25f }, 3.2f, 4, dark);
    }
    proc_mesh_spike((float[3]){ 12, 62, 19 }, (float[3]){ 10, 80, 34 }, 3.0f, 4, dark);
    proc_mesh_spike((float[3]){ 12, 62, -19 }, (float[3]){ 10, 80, -34 }, 3.0f, 4, dark);
    proc_mesh_end(&s_throne);
}

static void build_king(void)
{
    const uint32_t robe = proc_rgb(20, 16, 24), mail = proc_rgb(34, 30, 38), crown = proc_rgb(150, 112, 36);
    proc_mesh_begin();
    // Seated: shins, lap, robe drape
    proc_mesh_box((float[3]){ -12, 9, 6 }, (float[3]){ 3, 8, 3.5f }, robe);
    proc_mesh_box((float[3]){ -12, 9, -6 }, (float[3]){ 3, 8, 3.5f }, robe);
    proc_mesh_box((float[3]){ -4, 18, 0 }, (float[3]){ 9, 3, 10 }, robe);
    proc_mesh_frustum((float[3]){ -3, 1, 0 }, 15, 10, 16, 7, robe);
    // Torso widening to the shoulders, head
    proc_mesh_frustum((float[3]){ 2, 19, 0 }, 9, 13, 22, 6, mail);
    proc_mesh_box((float[3]){ 2, 47, 0 }, (float[3]){ 4.5f, 5, 4.5f }, robe);
    // Glowing eyes on the face (-x side)
    for (int s = -1; s <= 1; s += 2) {
        float z = s * 1.8f, x = 2 - 4.6f;
        proc_mesh_quad((float[3]){ x, 47.5f, z - 0.9f }, (float[3]){ x, 47.5f, z + 0.9f },
                       (float[3]){ x, 48.4f, z + 0.9f }, (float[3]){ x, 48.4f, z - 0.9f }, proc_rgb(255, 196, 60), true);
    }
    // Crown
    for (int i = 0; i < 6; i++) {
        float a = 2.0f * T3D_PI * i / 6;
        float bx = 2 + cosf(a) * 4.0f, bz = sinf(a) * 4.0f;
        proc_mesh_spike((float[3]){ bx, 51.5f, bz }, (float[3]){ 2 + cosf(a) * 5.5f, 60.0f + (i & 1) * 3.0f, sinf(a) * 5.5f },
                        1.6f, 3, crown);
    }
    // Spiked pauldrons
    for (int s = -1; s <= 1; s += 2) {
        proc_mesh_box((float[3]){ 2, 41, s * 13.0f }, (float[3]){ 5, 3, 4 }, mail);
        proc_mesh_spike((float[3]){ 2, 43, s * 13.0f }, (float[3]){ 2, 56, s * 21.0f }, 3.0f, 4, robe);
        proc_mesh_spike((float[3]){ 4, 42, s * 15.0f }, (float[3]){ 9, 49, s * 25.0f }, 2.6f, 4, robe);
        proc_mesh_spike((float[3]){ -1, 42, s * 15.0f }, (float[3]){ -3, 51, s * 23.0f }, 2.4f, 4, robe);
    }
    proc_mesh_end(&s_king);

    // Arms, around the shoulder pivot: resting forward on the armrests
    proc_mesh_begin();
    for (int s = -1; s <= 1; s += 2) {
        proc_mesh_box((float[3]){ -1, -9, s * 15.0f }, (float[3]){ 3, 8, 3 }, mail);          // upper arm, down
        proc_mesh_box((float[3]){ -8, -17, s * 16.0f }, (float[3]){ 7, 2.5f, 2.6f }, mail);   // forearm on the rest
        for (int c = -1; c <= 1; c++)                                                       // claws
            proc_mesh_spike((float[3]){ -15, -17, s * 16.0f + c * 1.5f }, (float[3]){ -21, -19, s * 16.0f + c * 2.2f }, 0.9f, 3, robe);
    }
    proc_mesh_end(&s_arms);
}

static void build_gold(void)
{
    proc_mesh_begin();
    s_rng = 12345;
    // Heaps of coins around the dais, spilling toward the hall
    for (int i = 0; i < 26; i++) {
        float a = T3D_PI * (0.35f + 1.3f * rnd()), r = 26.0f + rnd() * 30.0f;
        float x = cosf(a) * r * 0.8f, z = sinf(a) * r * 1.3f;
        if (x > 10.0f) x = 10.0f - rnd() * 6.0f;
        int g = 150 + (int)(rnd() * 60);
        proc_mesh_frustum((float[3]){ x, -1, z }, 8.0f + rnd() * 9.0f, 1.0f, 4.0f + rnd() * 5.0f, 7, proc_rgb(g + 45, g, 30));
    }
    for (int i = 0; i < 90; i++) {      // loose coins
        float x = -55.0f + rnd() * 60.0f, z = -50.0f + rnd() * 100.0f, y = 0.5f + rnd() * 3.0f, s = 1.4f;
        float tilt = rnd() * 1.2f;
        proc_mesh_tri((float[3]){ x - s, y, z }, (float[3]){ x + s, y + tilt, z - s }, (float[3]){ x, y + tilt * 0.5f, z + s },
                      proc_rgb(255, 214, 90), true);
    }
    proc_mesh_end(&s_gold);
}

#define SUN_R 16.0f      // built at this radius (proc meshes store 1/8 units)

// The Sun: a glowing ball whose eye (a black pupil with a red iris) looks down -y
static void build_sun(void)
{
    enum { LAT = 9, LON = 14 };
    proc_mesh_begin();
    for (int i = 0; i < LAT; i++) {
        float a0 = T3D_PI * i / LAT, a1 = T3D_PI * (i + 1) / LAT;   // 0 at the eye (-y)
        for (int j = 0; j < LON; j++) {
            float b0 = 2.0f * T3D_PI * j / LON, b1 = 2.0f * T3D_PI * (j + 1) / LON;
            float p[4][3] = {
                { sinf(a0) * cosf(b0), -cosf(a0), sinf(a0) * sinf(b0) }, { sinf(a0) * cosf(b1), -cosf(a0), sinf(a0) * sinf(b1) },
                { sinf(a1) * cosf(b1), -cosf(a1), sinf(a1) * sinf(b1) }, { sinf(a1) * cosf(b0), -cosf(a1), sinf(a1) * sinf(b0) },
            };
            float m = (a0 + a1) * 0.5f / T3D_PI;    // 0 eye .. 1 back
            uint32_t c = (i == 0) ? proc_rgb(8, 4, 4) : (i == 1) ? proc_rgb(200, 50, 20)
                       : proc_rgb(255, (int)(225 - 120 * m), (int)(110 - 90 * m));
            if (i > 1 && ((j * 7 + i * 3) % 11) == 0) c = proc_rgb(220, 70, 30);   // a few veins
            for (int k = 0; k < 4; k++) for (int q = 0; q < 3; q++) p[k][q] *= SUN_R;
            proc_mesh_quad(p[0], p[1], p[2], p[3], c, true);
        }
    }
    proc_mesh_end(&s_sun);
}

void ending_free(void)
{
    if (!s_ready) return;
    rspq_wait();
    proc_mesh_free(&s_throne);
    proc_mesh_free(&s_king);
    proc_mesh_free(&s_arms);
    proc_mesh_free(&s_gold);
    proc_mesh_free(&s_sun);
    if (s_raysModel) t3d_model_free(s_raysModel);
    if (s_raysMat) free_uncached(s_raysMat);
    s_raysModel = NULL;
    s_raysMat = NULL;
    s_ready = false;
}

void ending_begin(void)
{
    ending_free();
    build_throne();
    build_king();
    build_gold();
    build_sun();
    const float t[3] = { THRONE_X, FLOOR_Y, 0.0f };
    proc_mesh_set_transform(&s_throne, 1.0f, (float[3]){ 0, 0, 0 }, t);
    proc_mesh_set_transform(&s_king, 1.0f, (float[3]){ 0, 0, 0 }, t);
    proc_mesh_set_transform(&s_gold, 1.0f, (float[3]){ 0, 0, 0 }, t);
    s_raysModel = t3d_model_load("rom:/shackled_sun/shackled_sun_rays.t3dm");
    s_raysMat = malloc_uncached(sizeof(T3DMat4FP));
    s_ready = true;

    s_t = 0.0f;
    s_line = 0;
    character.pos[0] = WALK_FROM_X;
    character.pos[1] = FLOOR_Y;
    character.pos[2] = 0.0f;
    character_set_velocity_xz(0.0f, 0.0f);
    character_set_state(CHAR_STATE_FOG_WALK);

    cutscene_manager_set_camera_shot(NULL, (T3DVec3){{ WALK_FROM_X - 30.0f, 20.0f, 8.0f }},
                                     (T3DVec3){{ WALK_FROM_X - 30.0f, 20.0f, 8.0f }},
                                     (T3DVec3){{ WALK_FROM_X + 40.0f, 16.0f, 0.0f }});
    camera_for(0.0f);
    camera_mode_smooth(CAMERA_CUSTOM, 0.05f);   // also replaces any camera blend still running
    audio_stop_music();
    audio_play_music("rom:/video.wav64", false);
}

// ---- Timeline ----

static const struct { float t; const char *text; } LINES[] = {
    { 15.5f, "^You've come here...~ ^to kill\nme? And what will that\nachieve..." },
    { 21.0f, "^There is nothing left in this\nworld that is not already\nmine." },
    { 26.3f, "<If I die..." },
    { 29.6f, ">IT DIES WITH ME" },
    { 50.0f, "<A fitting image..." },
};
static const float LINE_CLEAR[] = { 20.8f, 26.0f, 29.4f, 33.5f, 56.0f };

static void knight_walk_x(float t, float *x) { *x = WALK_FROM_X + (WALK_TO_X - WALK_FROM_X) * clamp01(t / WALK_END_T); }

static void camera_for(float t)
{
    float p[3], q[3];
    float kx; knight_walk_x(t, &kx);
    if (t < 6.0f) {                 // behind the knight as it walks in
        float u = smooth(0.0f, 6.0f, t);
        p[0] = kx - 30.0f + 8.0f * u; p[1] = 18.0f; p[2] = 9.0f - 4.0f * u;
        q[0] = kx + 40.0f; q[1] = 15.0f; q[2] = 0.0f;
    } else if (t < 12.0f) {         // over its shoulder: the throne ahead
        p[0] = kx - 24.0f; p[1] = 24.0f; p[2] = -10.0f;
        lerp3(q, (float[3]){ kx + 40.0f, 15.0f, 0.0f }, KING_HEAD, smooth(6.0f, 9.0f, t));
    } else if (t < 15.0f) {         // high and wide: a small knight on a long carpet
        lerp3(p, (float[3]){ -150.0f, 80.0f, 120.0f }, (float[3]){ -125.0f, 76.0f, 108.0f }, smooth(12.0f, 15.0f, t));
        q[0] = kx + 30.0f; q[1] = 10.0f; q[2] = 0.0f;
    } else if (t < 21.0f) {         // the King, past the knight's shoulder
        lerp3(p, (float[3]){ WALK_TO_X - 34.0f, 26.0f, 14.0f }, (float[3]){ WALK_TO_X - 27.0f, 27.0f, 12.0f }, smooth(15.0f, 21.0f, t));
        memcpy(q, KING_HEAD, sizeof(q)); q[1] -= 6.0f;
    } else if (t < 26.0f) {         // his face
        lerp3(p, (float[3]){ THRONE_X - 42.0f, 49.0f, 7.0f }, (float[3]){ THRONE_X - 30.0f, 49.0f, 4.0f }, smooth(21.0f, 26.0f, t));
        memcpy(q, KING_HEAD, sizeof(q));
    } else if (t < 29.5f) {         // low, looking up at him over the gold
        lerp3(p, (float[3]){ THRONE_X - 62.0f, 8.0f, -34.0f }, (float[3]){ THRONE_X - 54.0f, 9.0f, -28.0f }, smooth(26.0f, 29.5f, t));
        memcpy(q, KING_HEAD, sizeof(q)); q[1] -= 6.0f;
    } else if (t < 37.0f) {         // he raises his arms; the camera climbs to the ceiling
        p[0] = THRONE_X - 85.0f; p[1] = 26.0f; p[2] = 22.0f;
        lerp3(q, (float[3]){ KING_HEAD[0], KING_HEAD[1] - 4.0f, 0.0f }, (float[3]){ THRONE_X - 40.0f, 300.0f, 0.0f },
              smooth(32.5f, 36.5f, t));
    } else if (t < 43.0f) {         // the Sun comes down
        lerp3(p, (float[3]){ THRONE_X - 85.0f, 26.0f, 22.0f }, (float[3]){ THRONE_X - 60.0f, 60.0f, 12.0f }, smooth(37.0f, 43.0f, t));
        q[0] = THRONE_X - 40.0f; q[1] = 250.0f - 60.0f * smooth(37.0f, 43.0f, t); q[2] = 0.0f;
    } else if (t < 54.0f) {         // the vortex, from above, following the fall
        float y = VOID_Y - 20.0f - 150.0f * smooth(47.5f, 54.0f, t);
        p[0] = 8.0f; p[1] = y + 50.0f - 10.0f * smooth(44.0f, 47.5f, t); p[2] = 0.0f;
        q[0] = 0.0f; q[1] = y - 60.0f; q[2] = 0.0f;
    } else {                        // back in the hall, looking down at what is left
        float u = smooth(55.0f, 60.0f, t);
        p[0] = WALK_TO_X + 6.0f; p[1] = 70.0f + 60.0f * u; p[2] = 5.0f;
        q[0] = WALK_TO_X; q[1] = FLOOR_Y; q[2] = 0.0f;
    }
    customCamPos = (T3DVec3){{ p[0], p[1], p[2] }};
    customCamTarget = (T3DVec3){{ q[0], q[1], q[2] }};
}

static float sun_y(float t) { return 330.0f - 150.0f * smooth(37.0f, 43.0f, t); }

bool ending_update(float dt)
{
    if (!s_ready) return false;
    s_t += dt;
    float t = s_t;
#ifdef HLE_TEST_ENDING
    { static int n; if (++n % 30 == 0) debugf("[end] t %.2f dt %.3f\n", (double)t, (double)dt); }
#endif

    // Dialog
    if (s_line < (int)(sizeof(LINES) / sizeof(LINES[0])) && t >= LINES[s_line].t) {
        cutscene_manager_begin_dialog(LINES[s_line].text, 0.0f);
        s_line++;
    }
    if (s_line > 0 && t >= LINE_CLEAR[s_line - 1] && cutscene_manager_is_dialog_active()) cutscene_manager_clear_dialog();
    cutscene_manager_update_dialog();

    // The knight: walks in, stops, falls through the vortex shrinking, lands tiny
    float kx; knight_walk_x(t, &kx);
    if (t < WALK_END_T) {
        character.pos[0] = kx;
        character.rot[1] = T3D_PI * 0.5f;      // facing down the hall (+x)
    } else if (t < 47.5f) {
        if (character_get_state() == CHAR_STATE_FOG_WALK) character_set_state(CHAR_STATE_NORMAL);
        character.pos[0] = WALK_TO_X;
    } else if (t < 54.0f) {
        if (character_get_state() != CHAR_STATE_KNOCKDOWN) character_set_state(CHAR_STATE_KNOCKDOWN);   // not DEAD: that is the death screen
        float u = smooth(47.5f, 54.0f, t);
        character.pos[0] = 0.0f; character.pos[1] = VOID_Y - 20.0f - 150.0f * u; character.pos[2] = 0.0f;
        character.rot[1] += dt * 3.0f;
        float sc = MODEL_SCALE * (0.5f - 0.42f * u);
        character.scale[0] = character.scale[1] = character.scale[2] = sc;
    } else {
        if (character_get_state() == CHAR_STATE_KNOCKDOWN) character_set_state(CHAR_STATE_NORMAL);
        float u = clamp01((t - 54.0f) / 1.2f);
        character.pos[0] = WALK_TO_X; character.pos[1] = FLOOR_Y + 60.0f * (1.0f - u * u); character.pos[2] = 0.0f;
        float sc = MODEL_SCALE * 0.12f;
        character.scale[0] = character.scale[1] = character.scale[2] = sc;
    }
    character_set_velocity_xz(0.0f, 0.0f);
    character_update_cinematic();

    // The King's arms rise with "IT DIES WITH ME"
    float raise = smooth(29.6f, 31.0f, t);
    proc_mesh_set_transform(&s_arms, 1.0f, (float[3]){ 0, 0, -1.9f * raise }, ARM_PIVOT);

    if (t > 37.0f && t < 43.0f) screen_shake_set_shake_mag(1.5f);
    else screen_shake_set_shake_mag(0.0f);

    camera_for(t);
    return t < END_T;
}

bool ending_room_visible(void)
{
    return s_t < 43.0f || s_t >= 54.0f;
}

// `lookUp`: the eye faces up (seen from above) instead of down
static void draw_sun(const float pos[3], float scale, bool lookUp)
{
    const float sc = MODEL_SCALE * scale * 1.3f;
    // The rays card faces +x: turn it to face along y, spinning slowly
    t3d_mat4fp_from_srt_euler(s_raysMat, (float[3]){ sc, sc, sc },
                              (float[3]){ s_t * 0.6f, 0.0f, lookUp ? T3D_PI * 0.5f : -T3D_PI * 0.5f }, pos);
    proc_mesh_set_transform(&s_sun, scale * 7.0f / SUN_R, (float[3]){ lookUp ? T3D_PI : 0.0f, 0.0f, 0.0f }, pos);
    rdpq_sync_pipe();
    rdpq_mode_zbuf(false, false);
    t3d_matrix_push_pos(1);
        t3d_matrix_set(s_raysMat, true);
        t3d_model_draw_custom(s_raysModel, (T3DModelDrawConf){ .userData = &s_raysScroll, .tileCb = tile_scroll });
        proc_mesh_draw(&s_sun);
    t3d_matrix_pop(1);
}

void ending_draw_world(void)
{
    if (!s_ready) return;
    float t = s_t;
    if (ending_room_visible()) {
        t3d_matrix_push_pos(1);
            proc_mesh_draw(&s_gold);
            proc_mesh_draw(&s_throne);
            proc_mesh_draw(&s_king);
            proc_mesh_draw(&s_arms);
        t3d_matrix_pop(1);
        if (t > 33.0f && t < 43.0f) draw_sun((float[3]){ THRONE_X - 40.0f, sun_y(t), 0.0f }, 6.0f, false);
    } else if (t >= 44.0f) {
        // The void: the Sun's eye at the bottom of a swirling funnel, the knight falling in
        draw_sun((float[3]){ 0.0f, VOID_Y - 230.0f, 0.0f }, 7.0f, true);
        skill_fx_draw_vortex((float[3]){ 0.0f, VOID_Y, 0.0f }, 220.0f, 110.0f, t);
        if (t >= 47.5f) {
            rdpq_sync_pipe();
            rdpq_set_mode_standard();
            rdpq_mode_zbuf(true, true);
            t3d_matrix_push_pos(1);
                character_draw();
            t3d_matrix_pop(1);
        }
    }
}

static void draw_black(float a)
{
    if (a <= 0.0f) return;
    rdpq_sync_pipe();
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_set_prim_color(RGBA32(0, 0, 0, (uint8_t)(clamp01(a) * 255.0f)));
    rdpq_fill_rectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
}

void ending_draw_overlay(void)
{
    if (!s_ready) return;
    float t = s_t;
    cutscene_manager_draw_dialog();
    // Fade in from the portal's black; a blackout between the Sun and the void; fade out
    float a = 1.0f - clamp01(t / 1.2f);
    a = fmaxf(a, smooth(42.2f, 43.0f, t) * (1.0f - smooth(44.0f, 44.8f, t)));
    a = fmaxf(a, smooth(57.5f, 59.5f, t));
    draw_black(a);
}
