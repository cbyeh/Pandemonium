/**
 * Test-only input script (build with HLE_EXTRA_CFLAGS=-DHLE_AUTOPLAY): taps A to
 * walk through the menus so automated emulator runs reach gameplay.
 */
#ifdef HLE_AUTOPLAY
#include <libdragon.h>
#include <string.h>
#ifdef HLE_LOG_HITVOL
#include <t3d/t3dmodel.h>
#include <t3d/t3dskeleton.h>
#endif
#ifndef HLE_STRIKE_EVERY
#define HLE_STRIKE_EVERY 24     // frames between presses (fewer chains the combo)
#endif
#if defined(HLE_TEST_SKILLS) || defined(HLE_TEST_SKILL_DMG) || defined(HLE_TEST_TOGGLE) || defined(HLE_TEST_BOSS2) || defined(HLE_TEST_TWO_RUNS) || defined(HLE_TEST_BONUS) || defined(HLE_TEST_ENDING) || defined(HLE_TEST_HEAVY) || defined(HLE_TEST_DUMMY) || defined(HLE_TEST_COVER) || defined(HLE_TEST_ROLLCANCEL) || defined(HLE_TEST_SKIN) || defined(HLE_TEST_UNBOUND) || defined(HLE_TEST_CRUCIFIX) || defined(HLE_TEST_TOWER_RUN) || defined(HLE_TEST_TOWER_UI) || defined(HLE_TEST_SKIN_TITLE) || defined(HLE_TEST_SKIN_SAVE) || defined(HLE_TEST_SKIN_SLOT) || defined(HLE_TEST_SAVE_FLOW) || defined(HLE_TEST_SAVE_FRESH) || defined(HLE_BOOT_SKIN) || defined(HLE_TEST_LOADMENU) || defined(HLE_TEST_MARKS) || defined(HLE_TEST_BKEY) || defined(HLE_TEST_ANIMVIEW) || defined(HLE_TEST_SHRINE) || defined(HLE_TEST_CONTROLS)
#include "camera_controller.h"
#include "joypad_utility.h"
#include "character_skin.h"
#include <math.h>
#include "character.h"
#include "game/boss/boss.h"
#include "controllers/menu_controller.h"
#include "game/boss/environmental_mechanics/multi_sword_attacks.h"
#include "scenes/guardian/guardian_scene_bounds.h"
#endif
#if defined(HLE_TEST_AUDIO) || defined(HLE_TEST_SHRINE) || defined(HLE_TEST_LOADMENU) || defined(HLE_TEST_MARKS) || defined(HLE_TEST_QUIT) || defined(HLE_TEST_TOWER_RUN) || defined(HLE_TEST_TOWER_UI) || defined(HLE_TEST_SKIN_TITLE) || defined(HLE_TEST_SKIN_SAVE) || defined(HLE_TEST_SKIN_SLOT) || defined(HLE_TEST_SAVE_FLOW) || defined(HLE_TEST_SAVE_FRESH)
#include "utilities/save_controller.h"
#include "controllers/scene_controller.h"
#include "controllers/menu_controller.h"
#include "controllers/audio_controller.h"
#include "scenes/guardian/guardian_scene_sfx.h"
#include "game/boss/boss.h"
#endif

int hle_task_log;
float hle_dummy_damage;
int hle_dummy_hits;
// Openings (HLE_TEST_DUMMY): time a Bonus boss spends not attacking, and how many such gaps
float hle_open_time, hle_open_cur;
int hle_open_gaps;
bool hle_open_now;      // the boss is between attacks this frame (the bot playtest strikes then)
void hle_open_tick(bool open, float dt)
{
    hle_open_now = open;
    if (open) { hle_open_time += dt; hle_open_cur += dt; }
    else if (hle_open_cur > 0.0f) { hle_open_gaps++; hle_open_cur = 0.0f; }
}

void hle_autoplay(joypad_inputs_t *in, joypad_buttons_t *pressed)
{
    static int frame;
    frame++;
#ifdef HLE_PAD_LAYOUT       // test aid: play in this button layout (0 Classic, 1 A/B Swapped, 2 Skills on C)
    { extern void joypad_set_layout(int); joypad_set_layout(HLE_PAD_LAYOUT); }     // every frame: loading the save sets it too
#endif
#ifdef HLE_SKILLS_OFF   // the Skills setting off throughout (a loaded save can't turn it back on)
    { extern void character_set_skills_enabled(bool); character_set_skills_enabled(false); }
#endif
#ifdef HLE_TEST_T3D_EXACT
    if (frame % 300 == 0) { extern uint32_t hle_exact_calls, hle_exact_strips, hle_exact_bad; debugf("[exact] %lu vertex loads, %lu strips, %lu mismatched\n", (unsigned long)hle_exact_calls, (unsigned long)hle_exact_strips, (unsigned long)hle_exact_bad); }
#endif
#ifdef HLE_TEST_ARC_REACH   // test aid: when the boss goes out of Halfmoon's reach and back (its height, its state)
    {
        extern bool skill_arc_reaches(const struct Boss *b, float arcY); extern float deltaTime;
        Boss *bb = boss_get_instance();
        static int was = -1; static float outT, allT;
        if (bb && bb->mode == BOSS_MODE_COMBAT && bb->health > 0.0f) {
            int r = skill_arc_reaches(bb, character.pos[1] + 10.0f);
            float low = fminf(bb->capsuleCollider.localCapA.v[1], bb->capsuleCollider.localCapB.v[1]) + bb->pos[1] - bb->capsuleCollider.radius - character.pos[1];
            allT += deltaTime; if (!r) outT += deltaTime;
            if (r != was) { debugf("[reach] boss %d %s at state %d, lowest point %.0f above the floor\n", bb->phaseIndex, r ? "back in reach" : "OUT of reach", bb->state, (double)low); was = r; }
            if (frame % 900 == 0) debugf("[reach] out of reach %.0f%% of the fight\n", (double)(outT * 100.0f / fmaxf(allT, 0.01f)));
        }
    }
#endif
#ifdef HLE_LOG_HURTBOX     // test aid: the hit capsule against the model's bones (height span, and how far the body leans out of it)
    {
        Boss *bb = boss_get_instance();
        if (bb && bb->mode == BOSS_MODE_COMBAT && bb->phaseIndex <= 3 && frame % 10 == 0) {     // (Shadowborne logs its own: dragon_boss.c)
            static const char *names[] = { "Head", "Waist", "LowerLeg-Left", "LowerLeg-Right" };
            float lo = fminf(bb->capsuleCollider.localCapA.v[1], bb->capsuleCollider.localCapB.v[1]) - bb->capsuleCollider.radius;
            float hi = fmaxf(bb->capsuleCollider.localCapA.v[1], bb->capsuleCollider.localCapB.v[1]) + bb->capsuleCollider.radius;
            static float worstOut[4], worstH[4][2] = { { 1e9f, -1e9f }, { 1e9f, -1e9f }, { 1e9f, -1e9f }, { 1e9f, -1e9f } }; static int n;
            for (int i = 0; i < 4; i++) {
                float w[3]; bool ok = false;
                if (bb->phaseIndex <= 2) { T3DVec3 v; extern bool boss_get_bone_world_pos(const Boss*, int, T3DVec3*);
                    int idx = i == 0 ? bb->headBoneIndex : i == 1 ? bb->waistBoneIndex : i == 2 ? bb->lowerLegLeftBoneIndex : bb->lowerLegRightBoneIndex;
                    ok = boss_get_bone_world_pos(bb, idx, &v); w[0] = v.v[0]; w[1] = v.v[1]; w[2] = v.v[2]; }
                else if (bb->phaseIndex == 3) { extern bool fallen_knight_bone(const char*, float*); ok = fallen_knight_bone(names[i], w); }
                if (!ok) continue;
                float rel = w[1] - bb->pos[1], dx = w[0] - bb->pos[0], dz = w[2] - bb->pos[2];
                float out = sqrtf(dx * dx + dz * dz) - bb->capsuleCollider.radius;      // > 0: outside the capsule sideways
                if (out > worstOut[i]) worstOut[i] = out;
                worstH[i][0] = fminf(worstH[i][0], rel); worstH[i][1] = fmaxf(worstH[i][1], rel);
            }
            if (++n % 90 == 0) debugf("[hurtbox] boss %d capsule %.0f..%.0f r %.0f | head %.0f..%.0f out %.0f | waist %.0f..%.0f out %.0f | legs %.0f..%.0f out %.0f\n", bb->phaseIndex, (double)lo, (double)hi, (double)bb->capsuleCollider.radius,
                (double)worstH[0][0], (double)worstH[0][1], (double)worstOut[0], (double)worstH[1][0], (double)worstH[1][1], (double)worstOut[1], (double)fminf(worstH[2][0], worstH[3][0]), (double)fmaxf(worstH[2][1], worstH[3][1]), (double)fmaxf(worstOut[2], worstOut[3]));
        }
    }
#endif
#ifdef HLE_LOG_HITVOL     // test aid: the drawn bodies (every skinned vertex, swords left out) against the hit capsules
    if (frame % 30 == 0) debugf("[f] %d\n", frame);     // (to find a logged moment's frame, for a screenshot)
    if (frame % 6 == 0) {
        extern T3DModel *characterModel;
        extern bool hle_silhouette(const T3DModel *m, const T3DSkeleton *sk, const T3DMat4FP *M, const float o[3], float yaw, float h, float out[7]);
        enum { NS = 1200, NC = 7 };
        static float smp[2][NC][NS]; static int ns[2];
        static const char *cn[NC] = { "legs r", "torso r", "arms r", "head r", "top", "front", "side" };
        Boss *bb = boss_get_instance();
        CharacterState st = character_get_state();
        float o[NC];
        if (bb && bb->mode == BOSS_MODE_COMBAT && bb->health > 0.0f && st != CHAR_STATE_DEAD) {
            // the player at all heights; a boss below 32 above its feet (what a sword on the floor reaches)
            if (ns[0] < NS && hle_silhouette(characterModel, character.skeleton, character.modelMat, character.pos, character.rot[1], 1e9f, o)) {
                for (int c = 0; c < NC; c++) smp[0][c][ns[0]] = o[c];
                ns[0]++;
            }
            bool ok = false;
            if (bb->phaseIndex <= 2) ok = hle_silhouette(bb->model, bb->skeleton, bb->modelMat, bb->pos, bb->rot[1], 32.0f, o);
            else if (bb->phaseIndex == 3) { extern bool fallen_knight_hle_pose(const T3DSkeleton **, const T3DMat4FP **); const T3DSkeleton *sk; const T3DMat4FP *M;
                if (fallen_knight_hle_pose(&sk, &M)) ok = hle_silhouette(characterModel, sk, M, bb->pos, bb->rot[1], 32.0f, o); }
            if (ok && ns[1] < NS) { for (int c = 0; c < NC; c++) smp[1][c][ns[1]] = o[c]; ns[1]++; }
            if (ok && frame % 240 == 0) debugf("[hitvol] boss %d state %d: below 32 legs %.1f torso %.1f arms %.1f head %.1f | top %.1f | capsule r %.0f, %.0f..%.0f\n", bb->phaseIndex, bb->state,
                (double)o[0], (double)o[1], (double)o[2], (double)o[3], (double)o[4], (double)bb->capsuleCollider.radius,
                (double)(fminf(bb->capsuleCollider.localCapA.v[1], bb->capsuleCollider.localCapB.v[1]) - bb->capsuleCollider.radius),
                (double)(fmaxf(bb->capsuleCollider.localCapA.v[1], bb->capsuleCollider.localCapB.v[1]) + bb->capsuleCollider.radius));
        }
        if (frame % 1800 == 0)     // percentiles (50 / 90 / max) of each extent
            for (int w = 0; w < 2; w++) for (int c = 0; c < NC; c++) {
                int n = ns[w]; if (n < 10) continue;
                static float tmp[NS]; memcpy(tmp, smp[w][c], n * sizeof(float));
                for (int i = 1; i < n; i++) { float v = tmp[i]; int j = i - 1; while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; } tmp[j + 1] = v; }
                debugf("[hitvol] %s %s: p50 %.1f p90 %.1f p99 %.1f max %.1f (%d)\n", w ? "boss" : "player", cn[c], (double)tmp[n / 2], (double)tmp[n * 9 / 10], (double)tmp[n * 99 / 100], (double)tmp[n - 1], n);
            }
    }
#endif
#ifdef HLE_LOG_JUMPS     // test aid: any frame the boss moves further than it could in one step (a teleport)
    {
        Boss *bb = boss_get_instance(); static float last[3]; static bool have;
        if (bb && bb->mode == BOSS_MODE_COMBAT) {
            float dx = bb->pos[0] - last[0], dy = bb->pos[1] - last[1], dz = bb->pos[2] - last[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (have && d > 8.0f) debugf("[jump] boss moved %.1f in one frame (to %.0f %.0f %.0f)\n", (double)d, (double)bb->pos[0], (double)bb->pos[1], (double)bb->pos[2]);
            last[0] = bb->pos[0]; last[1] = bb->pos[1]; last[2] = bb->pos[2]; have = true;
        } else have = false;
    }
#endif
#ifdef HLE_LOG_RELIC    // test aid: the save's relics after boot (does the Shrine's choice survive a reboot?)
    if (frame == 600) { extern int tower_relic_equipped(void); extern bool tower_relic_owned(int); debugf("[relic] after boot: equipped %d, owned%s%s%s%s%s\n", tower_relic_equipped(),
        tower_relic_owned(0) ? " 0" : "", tower_relic_owned(1) ? " 1" : "", tower_relic_owned(2) ? " 2" : "", tower_relic_owned(3) ? " 3" : "", tower_relic_owned(4) ? " 4" : ""); }
#endif
#ifdef HLE_LOG_FPS
    {   // game frames per second, every 2 s (for hardware-path runs in Ares)
        static uint32_t last; static int lastFrame;
        uint32_t now = get_ticks_ms();
        if (now - last >= 2000) { if (last) debugf("[fps] %.1f\n", (double)((frame - lastFrame) * 1000.0f / (now - last))); last = now; lastFrame = frame; }
    }
#endif
#ifdef HLE_TEST_TASKS
    {   // gfx tasks per game frame (a frame should be exactly one)
        extern uint32_t gsp_epoch;
        static uint32_t last;
        extern int hle_task_log;
        hle_task_log = (frame == 300 || frame == 301 || frame == 700 || frame == 701);
        if (hle_task_log) debugf("[tasks] frame %d\n", frame);
        if (frame % 30 == 0) { debugf("[tasks] f%d %.2f per frame\n", frame, (double)(gsp_epoch - last) / 30.0); last = gsp_epoch; }
    }
#endif
#ifdef HLE_TEST_PERF
    {
        extern uint32_t __hle_perf_ticks;
        static uint32_t last, start;
        uint32_t now = TICKS_READ();
        static int minq = 99; static uint32_t maxdt;
        int q = audio_get_queued_buffers();
        if (q < minq) minq = q;
        if (last && now - last > maxdt) maxdt = now - last;
        if (frame % 120 == 0) {
            debugf("[hle] perf: %lu frames in %lu ms, rsp-hle %lu%% of CPU time, audio min queued %d/%d, max frame %lu ms\n", 120UL,
                (unsigned long)TICKS_TO_MS(now - start), (unsigned long)(__hle_perf_ticks * 100ULL / (now - start)),
                minq, audio_get_num_buffers(), (unsigned long)TICKS_TO_MS(maxdt));
            minq = 99; maxdt = 0;
            extern void hle_perf_dump(uint32_t total);
            hle_perf_dump(now - start);
            __hle_perf_ticks = 0; start = now;
        }
        last = now; (void)last;
    }
#endif
#ifdef HLE_TEST_ROLLCANCEL
    // A during an attack, the Halfmoon charge and an airborne Moon Blade Dance
    // (states: 0 normal, 1 rolling, 2 attacking, 9 skill)
    {
        static int start = -1;
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) start = frame;
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
        if (t == 60)  { pressed->b = 1; in->btn.b = 1; }
        if (t == 200) pressed->d_right = 1;
        if (t == 400) pressed->d_up = 1;
        if (t == 70 || t == 230 || t == 440) { pressed->a = 1; in->btn.a = 1; }
        if (t == 69 || t == 72 || t == 229 || t == 232 || t == 439 || t == 442)
            debugf("[rc] t %d state %d\n", t, character_get_state());
        return;
    }
#endif
#ifndef HLE_SKIN_DIST       // the skin check's camera: distance, height, where it looks (README shots: closer)
#define HLE_SKIN_DIST 70.0f
#endif
#ifndef HLE_SKIN_CAM_Y
#define HLE_SKIN_CAM_Y 30.0f
#define HLE_SKIN_LOOK_Y 20.0f
#endif
#ifdef HLE_TEST_SKIN
    // Skin check: HLE_TEST_SKIN=<id>. Title, then the knight
    // runs a circle and stands while the camera orbits: back, side, front every 60 frames.
    {
        static int start = -1;
        if (frame == 100 || start == -1) character_skin_set(HLE_TEST_SKIN);
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) { start = frame; debugf("[skin] start %d\n", frame); }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
#ifdef HLE_SKIN_MENU
        // pause menu > Settings > Skin, then step the skin right twice (pause menu: Resume, Restart, Settings)
        {
            static const char seq[] = "SDDADDDDRR";
            if (t >= 30 && t % 15 == 0 && t / 15 - 2 < (int)sizeof(seq) - 1) {
                char c = seq[t / 15 - 2];
                if (c == 'S') { pressed->start = 1; in->btn.start = 1; }
                if (c == 'D') pressed->d_down = 1;
                if (c == 'A') { pressed->a = 1; in->btn.a = 1; }
                if (c == 'R') pressed->d_right = 1;
            }
            return;
        }
#endif
        cameraLockOnActive = false;
        if (t == 1) { character.pos[0] = boss->pos[0] - 400.0f; character.pos[2] = boss->pos[2]; character_skin_reset_cloth(); }
#ifndef HLE_SKIN_GATE_ROT
#define HLE_SKIN_GATE_ROT 5.06f   // three-quarter front
#endif
#ifdef HLE_SKIN_GATE      // README shots: on the fog gate's landing (x ~ -700), turned HLE_SKIN_GATE_ROT, the camera along +x
        {
            // the height only placed once: pinned every frame, he falls and is put back, and the cape is dragged down past his feet
            character.pos[0] = HLE_SKIN_GATE; character.pos[2] = 0.0f; character.rot[1] = HLE_SKIN_GATE_ROT;
#ifdef HLE_SKIN_GATE_Y
            if (t == 1) character.pos[1] = HLE_SKIN_GATE_Y;
#endif
            if (t == 60) character_skin_reset_cloth();
            if (t % 60 == 0) debugf("[skin] f%d knight y %.1f state %d\n", frame, (double)character.pos[1], (int)character_get_state());
            float *p = character.pos;
            customCamPos = (T3DVec3){{ p[0] + HLE_SKIN_DIST, p[1] + HLE_SKIN_CAM_Y, p[2] }};
            customCamTarget = (T3DVec3){{ p[0], p[1] + HLE_SKIN_LOOK_Y, p[2] }};
            camera_mode_smooth(CAMERA_CUSTOM, 0.01f);
            return;
        }
#endif
#ifdef HLE_SKIN_ACTION
        // run, a combo, two rolls; the camera steps round every 20 frames
        const int period = 20; const float dist = 50.0f;
        if (t < 120) { in->stick_x = (int)(75 * cosf(t * 0.05f)); in->stick_y = (int)(75 * sinf(t * 0.05f)); }
        if (t >= 120 && t < 260 && t % 30 == 0) { pressed->b = 1; in->btn.b = 1; }
        if (t == 280 || t == 330) { in->stick_y = 80; pressed->a = 1; in->btn.a = 1; }
#else
        const int period = 60; const float dist = HLE_SKIN_DIST;
        if (t < 180) { in->stick_x = (int)(75 * cosf(t * 0.05f)); in->stick_y = (int)(75 * sinf(t * 0.05f)); }
#endif
        float yaw = character.rot[1];
        float a = -yaw + (float)(t / period % 3) * 1.5708f;   // facing direction rotated by 0/90/180
        float *p = character.pos;
        customCamPos = (T3DVec3){{ p[0] + dist * sinf(a), p[1] + HLE_SKIN_CAM_Y, p[2] + dist * cosf(a) }};
        customCamTarget = (T3DVec3){{ p[0], p[1] + HLE_SKIN_LOOK_Y, p[2] }};
        camera_mode_smooth(CAMERA_CUSTOM, 0.01f);   // also ends the fight-intro transition
        if (t % period == 0) debugf("[skin] view %d at frame %d\n", t / period % 3, frame);
        return;
    }
#endif
#ifdef HLE_TEST_TOWER
    // Self-check of the Tower rules (no fight): circle order, boss health, potions, skills
    if (frame == 120) {
        extern void tower_begin_run(uint32_t), tower_end_run(void), tower_next_floor(void);
        extern int tower_floor_boss(void), tower_mod_max_potions(void);
        extern float tower_boss_health_scale(int);
        extern bool tower_mod_skill_allowed(int);
        int fails = 0;
        for (uint32_t seed = 1; seed <= 20; seed++) {
            tower_begin_run(seed);
            int seen = 0;
            for (int f = 0; f < 5; f++) {
                int bss = tower_floor_boss();
                if ((f == 0 && bss != 0) || (f == 4 && bss != 1)) fails++;    // Guardian first, Unbound last
                seen |= 1 << bss;
                tower_next_floor();
            }
            if (seen != 0x1F) fails++;              // all five, once each
            if (fabsf(tower_boss_health_scale(1) - 1.0f) > 0.01f) fails++;                    // circle 2: 0.7 + 0.3
        }
        tower_begin_run(7);
        for (int f = 0; f < 25; f++) tower_next_floor();
        { float h = tower_boss_health_scale(1); if (fabsf(h - 2.3f) > 0.01f && fabsf(h - 2.875f) > 0.01f) fails++; }   // circle 6: 0.7 + 4 x 0.3 + 0.4 (x1.25 on an Iron Night)
        tower_begin_run(7);
        if (tower_mod_max_potions() != 3) fails++;
        if (fabsf(tower_boss_health_scale(1) - 0.7f) > 0.01f || fabsf(tower_boss_health_scale(5) - 0.385f) > 0.01f) fails++;   // circle 1, no skills (a Bonus boss: 0.7 x 0.55)
        { extern float tower_mod_damage_taken(void); if (fabsf(tower_mod_damage_taken() - 0.6f) > 0.01f) fails++; }
        if (tower_mod_skill_allowed(0)) fails++;
        tower_end_run();
        if (tower_mod_max_potions() != 3 || !tower_mod_skill_allowed(0) || tower_boss_health_scale(3) != 1.0f) fails++;
        debugf("[tower] self-check: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    }
    return;
#endif
#ifdef HLE_TEST_SKIN_SLOT
    // Run twice: the first run (booting on Save 1 with the Knight) loads Save 2 (back on the
    // title) and picks Wolf Knight; the second reports the slot and skin
    {
        static int phase = 0, t0 = 0, step = 0;
        int t = frame - t0;
        static const char *seqs[3] = { "DDDADA", "", "DADDDDADDDDA" };     // Load Game > Save 2 | (from Load Game) Settings > Skin: the list, Down x4 (Wolf Knight), A
        if (phase == 0) {
            if (menu_controller_is_active()) {
                debugf("[slot] booted on slot %d with skin %d (%s)\n", save_controller_get_active_slot(), character_skin_get(), character_skin_name(character_skin_get()));
                phase = (save_controller_get_active_slot() == 0 && character_skin_get() == 0) ? 1 : 9; t0 = frame;
            } else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1 || phase == 3) {
            const char *q = seqs[phase - 1];
            if (q[step] && t >= 20 + step * 15 && t < 21 + step * 15) {
                char c = q[step];
                if (c == 'D') pressed->d_down = 1; else if (c == 'R') pressed->d_right = 1; else { pressed->a = 1; in->btn.a = 1; }
                step++;
            }
            if (!q[step] && t > 20 + step * 15 + 60) {
                if (phase == 3) { debugf("[slot] set skin %d on slot %d\n", character_skin_get(), save_controller_get_active_slot()); phase = 9; }
                else { phase = 3; t0 = frame; step = 0; }
            }
        }
        return;
    }
#endif
#ifdef HLE_TEST_SAVE_FRESH
    // Run twice on a fresh EEPROM. Boot 1: Settings > Skin x3, Audio > Master volume down 3,
    // back out to the title, open Load Game (Save 1's card must show it used). Boot 2 should
    // come back with that skin and volume; it opens Load Game too
    {
        static int start = -1, step = 0;
        static const char *q = "";
        if (start < 0) {
            if (menu_controller_is_active()) {
                start = frame;
                const SaveData *d = save_controller_get_slot_data(0);
                debugf("[fresh] booted on slot %d, skin %d (%s), master %d, slot 1 stamp %lu\n", save_controller_get_active_slot(), character_skin_get(),
                       character_skin_name(character_skin_get()), audio_get_master_volume(), (unsigned long)(d ? d->last_played_timestamp : 0));
                q = (character_skin_get() == SKIN_KNIGHT) ? "DDDDADDDDRRRUUUUALLLBBUA" : "DDDDADDDD";    // boot 2: Settings, on the Skin row
            } else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
        if (q[step] && t == 20 + step * 15) {
            char c = q[step++];
            if (c == 'D') pressed->d_down = 1; else if (c == 'U') pressed->d_up = 1; else if (c == 'R') pressed->d_right = 1; else if (c == 'L') pressed->d_left = 1;
            else if (c == 'B') { pressed->b = 1; in->btn.b = 1; } else { pressed->a = 1; in->btn.a = 1; }
        }
        if (!q[step] && t == 20 + step * 15 + 120) {
            const SaveData *d = save_controller_get_slot_data(0);
            debugf("[fresh] done: skin %d (%s), master %d, slot 1 stamp %lu\n", character_skin_get(), character_skin_name(character_skin_get()),
                   audio_get_master_volume(), (unsigned long)(d ? d->last_played_timestamp : 0));
        }
        return;
    }
#endif
#ifdef HLE_TEST_SAVE_FLOW
    // Run three times on one EEPROM, fresh first. Boot 1: Black Swordsman on Save 1, then Load Game >
    // Save 2 (it must stay on the title) and the Umber cloak there. Boot 2 should open on Save 2
    // in Umber, then loads Save 1. Boot 3 should open on Save 1 in Black Swordsman.
    {
        static int start = -1, step = 0;
        static const char *q = "";
        if (start < 0) {
            if (menu_controller_is_active()) {
                start = frame;
                int sl = save_controller_get_active_slot(), k = character_skin_get();
                debugf("[flow] booted on slot %d with skin %d (%s)\n", sl, k, character_skin_name(k));
                if (sl == 0 && k == SKIN_KNIGHT) q = "DDDDADDDDRRRBUADADADDDDR";   // Settings > Skin x3, back, Load Game > Save 2, Settings > Skin x1
                else if (sl == 1) q = "DDDAA";                                  // Load Game > Save 1
            } else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
        if (q[step] && t == 20 + step * 15) {
            char c = q[step++];
            if (c == 'D') pressed->d_down = 1; else if (c == 'U') pressed->d_up = 1; else if (c == 'R') pressed->d_right = 1;
            else if (c == 'B') { pressed->b = 1; in->btn.b = 1; } else { pressed->a = 1; in->btn.a = 1; }
        }
        if (!q[step] && t == 20 + step * 15 + 120)
            debugf("[flow] done: slot %d, skin %d (%s), scene %d (1: title), menu %d\n", save_controller_get_active_slot(), character_skin_get(),
                   character_skin_name(character_skin_get()), scene_controller_get_active_scene(), menu_controller_is_active());
        return;
    }
#endif
#ifdef HLE_TEST_SKIN_SAVE
    // Run twice with the same ROM: the first run (booting with the Knight) picks Wolf Knight
    // in Title > Settings > Skin; the second reports what it booted with
    {
        static int start = -1, step = 0;
        if (start < 0) {
            if (menu_controller_is_active()) {
                start = frame;
                debugf("[save] booted with skin %d (%s), slot %d\n", character_skin_get(), character_skin_name(character_skin_get()), save_controller_get_active_slot());
            } else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        if (character_skin_get() != 0 && step == 0) return;    // second run: nothing to do
#ifdef HLE_SKIN_SAVE_UMBER
#define HLE_SKIN_SAVE_SEQ "DDDDADDDDADDDA"                       // the Umber Cloak (a hood)
#else
#define HLE_SKIN_SAVE_SEQ "DDDDADDDDADDDDA"                      // Settings > Skin: the list, Down x4 (Wolf Knight), A
#endif
        static const char seq[] = HLE_SKIN_SAVE_SEQ;
        int t = frame - start;
        if (seq[step] && t >= 20 + step * 15 && t < 21 + step * 15) {
            char c = seq[step];
            if (c == 'D') pressed->d_down = 1; else if (c == 'R') pressed->d_right = 1; else { pressed->a = 1; in->btn.a = 1; }
            step++;
        }
        if (!seq[step] && t == 400) debugf("[save] set skin %d (%s)\n", character_skin_get(), character_skin_name(character_skin_get()));
        return;
    }
#endif
#ifdef HLE_TEST_LOADMENU
    {   // test aid: open Title > Load Game and stay there (a look at the slot cards)
        static int start = -1, step = 0;
        if (start < 0) {
            if (menu_controller_is_active()) { start = frame; (void)save_controller_increment_run_count(); }   // a used slot
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
#ifndef HLE_MENU_ROW
#define HLE_MENU_ROW 3      // Load Game; 5: Credits
#endif
        if (step < HLE_MENU_ROW + 1 && t == 30 + step * 15) { if (step < HLE_MENU_ROW) pressed->d_down = 1; else { pressed->a = 1; in->btn.a = 1; } step++; }
        return;
    }
#endif
#ifdef HLE_TEST_BKEY
    {   // test aid: B on each title menu row must do nothing there (it only goes back)
        extern int scene_controller_get_active_scene(void);
        static int start = -1;
        if (start < 0) {
            if (menu_controller_is_active()) start = frame;
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start, row = t / 60;
        if (row < 6 && t % 60 == 20) { pressed->b = 1; in->btn.b = 1; debugf("[bkey] B on row %d\n", row); }
        if (row < 6 && t % 60 == 50) { debugf("[bkey] row %d: scene %d menu %d submenu %d\n", row, scene_controller_get_active_scene(), menu_controller_is_active(), menu_controller_is_title_submenu_active()); }
        if (row < 5 && t % 60 == 55) pressed->d_down = 1;
        return;
    }
#endif
#ifdef HLE_TEST_MARKS
    {   // test aid: a save with the story and two Bonus bosses beaten, then Title > Bonus
        static int start = -1, step = 0;
        if (start < 0) {
            if (menu_controller_is_active()) {
                start = frame; (void)save_controller_record_boss_clear_time_ms(754321);
                save_controller_mark_bonus_beaten(0); save_controller_mark_bonus_beaten(2);
            } else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
        if (step < 2 && t == 200 + step * 15) { if (step < 1) pressed->d_down = 1; else { pressed->a = 1; in->btn.a = 1; } step++; }
        return;
    }
#endif
#ifdef HLE_BOOT_SKIN
    if (frame == 2) character_skin_set(HLE_BOOT_SKIN);   // test aid: A/B the title with one skin
    return;
#endif
#ifdef HLE_TEST_SKIN_TITLE
    // Title > Settings (4 down) > Skin (4 down), then Right every 60 frames through the skins
    {
        static int start = -1, step = 0;
        if (start < 0) {
            if (menu_controller_is_active()) start = frame;
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        static const char seq[] = "DDDDADDDDA";      // Settings > Skin: the list
        int t = frame - start;
        if (step < 10 && t >= 20 + step * 15 && t < 20 + step * 15 + 1) { if (seq[step] == 'D') pressed->d_down = 1; else { pressed->a = 1; in->btn.a = 1; } step++; }
        {   // the longest frame after each switch (a long one jerks the fog gate's scroll)
            static uint32_t last; static int worst;
            uint32_t now = get_ticks_ms();
            int d = last ? (int)(now - last) : 0; last = now;
            if (step >= 10 && t > 200) { if (d > worst) worst = d; if ((t - 200) % 60 == 59) { debugf("[skin] worst frame %d ms\n", worst); worst = 0; } }
        }
        if (step >= 10 && t > 200 && (t - 200) % 60 == 0 && (t - 200) / 60 <= 8) {
            pressed->d_down = 1;
            debugf("[skin] down at %d\n", frame);
        }
        if (step >= 10 && t > 200 && (t - 200) % 60 == 8) { pressed->a = 1; in->btn.a = 1; }     // wear it (a locked one stays as it was)
        if (step >= 10 && t > 200 && (t - 200) % 60 == 30)
            debugf("[skin] now %d (%s) at %d | cam %.1f,%.1f,%.1f -> %.1f,%.1f,%.1f scale %.3f\n", character_skin_get(), character_skin_name(character_skin_get()), frame,
                   (double)camPos.v[0], (double)camPos.v[1], (double)camPos.v[2], (double)camTarget.v[0], (double)camTarget.v[1], (double)camTarget.v[2], (double)character.scale[0]);
        return;
    }
#endif
#ifdef HLE_TEST_CONTROLS
    // Title > Settings > Controls: Right (A/B Swapped), Right (Skills on C), Down, Right (camera 4);
    // the remap of each layout; then B back
    {
        extern void joypad_debug_remap(joypad_buttons_t *b);
        static int phase = 0, t0 = 0, step = 0;
        int t = frame - t0;
        if (phase == 0) {
            if (menu_controller_is_active()) { phase = 1; t0 = frame; }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {
            static const char seq[] = "DDDDADDA.R.R.DR";
            if (t >= 30 && seq[step]) {
                char c = seq[step];
                if (c == 'D') pressed->d_down = 1; else if (c == 'R') pressed->d_right = 1; else if (c == 'A') { pressed->a = 1; in->btn.a = 1; }
                if (c == '.') debugf("[ctl] layout %d (%s), camera %d\n", joypad_get_layout(), joypad_layout_name(joypad_get_layout()), camera_get_speed());
                step++; t0 = frame;
            }
            if (!seq[step] && t == 30) {
                debugf("[ctl] final: layout %d camera %d sens %.1f\n", joypad_get_layout(), camera_get_speed(), (double)cameraSensitivity);
                for (int l = 0; l < LAYOUT_COUNT; l++) {
                    joypad_set_layout(l);
                    joypad_buttons_t b = { 0 }; b.a = 1; b.c_up = 1;
                    joypad_debug_remap(&b);
                    debugf("[ctl] layout %d: physical A+C-up reads a%d b%d c_up%d d_up%d\n", l, b.a, b.b, b.c_up, b.d_up);
                }
                joypad_set_layout(LAYOUT_SKILLS_ON_C);
                phase = 2;
            }
        }
        return;
    }
#endif
#ifdef HLE_TEST_SHRINE
    // The Shrine's relic and Nightfarer rows: 200 Embers and two relics; Tower > Shrine, down to
    // Relic, right twice (the second relic), down, A (buy Nightfarer); then the checks
    {
        extern int tower_relic_equipped(void), tower_embers(void); extern bool tower_nightfarer_owned(void);
        static int phase = 0, t0 = 0, step = 0;
        int t = frame - t0;
        if (phase == 0) {
            if (menu_controller_is_active()) {
                TowerSave ts; save_controller_get_tower(&ts); debugf("[shrine] Embers at the menu %d\n", ts.embers); ts.embers = 500; ts.relics[0] = 0x12; ts.relics[1] = 0x8C; save_controller_set_tower(&ts);   // the second relic a pact (Brand of Wrath)
                phase = 1; t0 = frame;
            } else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {
#ifdef HLE_SHRINE_NONE
            static const char seq[] = "DDADADDDDDADDABDAUAUUAB";    // ... and back up to Relics, Up x2 (to None), A: no relic
#else
            static const char seq[] = "DDADADDDDDADDABDA";    // ... Relics: A, Down x2 (Saint's Chalice), A buys it, B; Nightfarer
#endif
            if (t >= 25 && seq[step]) {
                char c = seq[step];
#ifdef HLE_SHRINE_STICK     // the same, steered with the control stick
                if (c == 'D') in->stick_y = -80; else if (c == 'R') in->stick_x = 80;
                else if (c == 'U') in->stick_y = 80; else if (c == 'L') in->stick_x = -80; else if (c == 'B') { pressed->b = 1; in->btn.b = 1; } else { pressed->a = 1; in->btn.a = 1; }
#else
                if (c == 'D') pressed->d_down = 1; else if (c == 'R') pressed->d_right = 1;
                else if (c == 'U') pressed->d_up = 1; else if (c == 'L') pressed->d_left = 1; else if (c == 'B') { pressed->b = 1; in->btn.b = 1; } else { pressed->a = 1; in->btn.a = 1; }
#endif
                step++; t0 = frame;
            }
            if (!seq[step] && t == 40) {
                extern bool tower_relic_owned(int id);
                debugf("[shrine] owned:%s%s%s%s%s\n", tower_relic_owned(0) ? " 0" : "", tower_relic_owned(1) ? " 1" : "", tower_relic_owned(2) ? " 2" : "",
                       tower_relic_owned(3) ? " 3" : "", tower_relic_owned(4) ? " 4" : "");
                debugf("[shrine] relic equipped %d, Nightfarer owned %d, skin unlocked %d, Embers %d\n", tower_relic_equipped(),
                       tower_nightfarer_owned(), save_controller_skin_unlocked(SKIN_NIGHTFARER), tower_embers());
                pressed->d_up = 1; phase = 2;
            }
        }
        return;
    }
#endif
#ifdef HLE_TEST_TOWER_UI
    // Screens and the skill cards: 200 Embers; Title > Tower > Shrine: buy Honed Blade
    // (row 3); back, Climb; no skill works on floor 1; kill the boss: the draft, take its
    // first card (always a skill card): on floor 2 that one skill works; then pause >
    // Main Menu > Load Game.
    {
        extern bool tower_draft_is_open(void), tower_mod_skill_allowed(int);
        extern int tower_floor(void);
        extern int tower_upgrade_level(int), tower_embers(void);
        static int phase = 0, t0 = 0, step = 0;
        Boss *boss = boss_get_instance();
        int t = frame - t0;
        if (phase == 0) {
            if (menu_controller_is_active()) {
                TowerSave ts; save_controller_get_tower(&ts); ts.embers = 200; save_controller_set_tower(&ts);
                phase = 1; t0 = frame; debugf("[ui] title at %d\n", frame);
            } else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {
            // DDD A (Tower) . A? no: D (Shrine) A; D D A (Honed Blade); D D D D A (Halfmoon); B; U A (Climb)
            static const char seq[] = "DDA.DA.DDA.B.UA";
            if (t >= 25 && seq[step]) {
                char c = seq[step];
                if (c == 'D') pressed->d_down = 1; else if (c == 'U') pressed->d_up = 1;
                else if (c == 'A') { pressed->a = 1; in->btn.a = 1; } else if (c == 'B') { pressed->b = 1; in->btn.b = 1; }
                if (c == '.') debugf("[ui] step %d at %d\n", step, frame);
                step++; t0 = frame;
            }
            if (!seq[step]) {
                debugf("[ui] bought: Honed Blade %d; Embers left %d\n", tower_upgrade_level(2), tower_embers());
                phase = 2; t0 = frame;
            }
        } else if (phase == 2) {
            static bool fighting;
            if (!fighting) { if (!boss || boss->mode != BOSS_MODE_COMBAT) { t0 = frame; return; } fighting = true; t0 = frame; return; }
            if (t == 40) pressed->d_up = 1;
            if (t == 44) debugf("[ui] floor 1, D-up: state %d (%s)\n", character_get_state(), character_get_state() == CHAR_STATE_SKILL ? "CAST: not locked" : "locked");
            if (t == 120) { debugf("[ui] kill at %d\n", frame); boss_apply_damage(boss, boss->maxHealth * 2.0f); }
            if (t == 240) debugf("[ui] draft %s at %d\n", tower_draft_is_open() ? "open" : "CLOSED", frame);
            if (t == 300) { pressed->a = 1; in->btn.a = 1; }      // the first card
            if (t == 420) debugf("[ui] floor %d: skills usable %d %d %d\n", tower_floor(), tower_mod_skill_allowed(0), tower_mod_skill_allowed(1), tower_mod_skill_allowed(2));
            if (t == 430) { int k = tower_mod_skill_allowed(0) ? 0 : tower_mod_skill_allowed(1) ? 1 : 2;
                            if (k == 0) pressed->d_up = 1; else if (k == 1) pressed->d_right = 1; else pressed->d_down = 1; }
            if (t == 434) debugf("[ui] the learned skill: state %d (%s)\n", character_get_state(), character_get_state() == CHAR_STATE_SKILL ? "cast" : "NOT CAST");
            if (t == 520) { pressed->start = 1; in->btn.start = 1; }
            if (t == 540 || t == 550 || t == 560 || t == 570) pressed->d_down = 1;
            if (t == 590) { pressed->a = 1; in->btn.a = 1; }
            if (t == 700) { phase = 3; t0 = frame; }
        } else if (phase == 3) {
            if (t == 30 || t == 40 || t == 50) pressed->d_down = 1;   // Load Game
            if (t == 70) { pressed->a = 1; in->btn.a = 1; debugf("[ui] load game at %d\n", frame); }
        }
        return;
    }
#endif
#ifdef HLE_TEST_TOWER_RUN
    // A Tower run from the title: Down x3 (Tower), A, A (Climb). Each floor: check the
    // boss, a locked skill (D-up does nothing), kill it, take a boon (A). After floor 6,
    // die: the death screen's summary, then A back to the title.
    {
        extern int tower_floor(void), tower_floor_boss(void);
        extern bool tower_draft_is_open(void), tower_active(void); extern int tower_draft_selected(void);
        extern const char *tower_death_summary(void);
        static int phase = 0, t0 = 0, step = 0, lastFloor = 0;
        Boss *boss = boss_get_instance();
        int t = frame - t0;
        if (phase == 0) {
            if (menu_controller_is_active()) { phase = 1; t0 = frame; }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {
            static const char seq[] = "DDAA";
            if (t >= 20 && step < 5) { if (seq[step] == 'D') pressed->d_down = 1; else { pressed->a = 1; in->btn.a = 1; } step++; t0 = frame; }
            if (step == 5 && boss && tower_active()) { phase = 2; t0 = frame; }
        } else if (phase == 2) {
            if (!boss) return;
            if (tower_floor() != lastFloor && boss->mode == BOSS_MODE_COMBAT && !tower_draft_is_open()) {
                lastFloor = tower_floor(); t0 = frame;
#ifdef HLE_LOG_FPS
                { extern int hle_floor_mark; hle_floor_mark = 1; }   // test aid: count this floor's fight apart from its load
#endif
                debugf("[run] floor %d boss %d phase %d hp %.0f/%.0f | player hp %.0f/%.0f potions %d\n", lastFloor, tower_floor_boss(),
                       boss->phaseIndex, (double)boss->health, (double)boss->maxHealth, (double)character.health, (double)character.maxHealth,
                       character.healthPotions);
#ifdef HLE_RUN_CLIMB    // the long climb: the run's numbers each floor
                {
                    extern float tower_mod_damage_dealt(void), tower_mod_damage_taken(void), tower_mod_max_health(void);
                    extern int tower_run_embers(void), tower_circle(void);
                    heap_stats_t h; sys_get_heap_stats(&h);
                    debugf("[climb] floor %d circle %d boss %d hp %.0f | dealt x%.2f taken x%.2f maxhp %.0f potions %d | embers %d | heap %d KB free\n",
                           lastFloor, tower_circle(), tower_floor_boss(), (double)boss->maxHealth, (double)tower_mod_damage_dealt(),
                           (double)tower_mod_damage_taken(), (double)tower_mod_max_health(), character.healthPotions, tower_run_embers(),
                           (h.total - h.used) / 1024);
                }
#endif
            }
            if (t == 30) pressed->d_up = 1;
            if (t == 34) debugf("[run] D-up: state %d (%s)\n", character_get_state(), character_get_state() == CHAR_STATE_SKILL ? "cast: Moon Blade Dance learned" : "no skill");
#ifdef HLE_RUN_ROLLS       // roll until the stamina runs out (the wait before it refills)
            if (t == 40) character.stamina = 1.0f;          // a sliver left: this roll empties the bar
            if (t == 41) { pressed->a = 1; in->btn.a = 1; in->stick_y = 80; }
#endif
#ifdef HLE_RUN_STRIKE      // fight with the sword for 20 s first (the boons that hook into hits)
            if (t < 1200) {
                if (t == 1) pressed->z = 1;
                in->btn.z = 1;
                float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2];
                if (sqrtf(dx * dx + dz * dz) > 40.0f) in->stick_y = 70;
                if (t % 24 == 0) { pressed->b = 1; in->btn.b = 1; }
                if (t % 300 == 299) debugf("[run] boss hp %.0f/%.0f\n", (double)boss->health, (double)boss->maxHealth);
            }
            if (t == 1200) {
#ifdef HLE_LOG_FPS
                { extern int hle_floor_mark; hle_floor_mark = 2; }   // the fight's end (before the next load)
#endif
                boss_apply_damage(boss, boss->maxHealth * 2.0f);
            }
#else
            if (t == 90) boss_apply_damage(boss, boss->maxHealth * 2.0f);
#endif
            if (t >= 90 && t < 140) {   // stand facing the fallen boss, where its "A" prompt would show
                float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2], d = sqrtf(dx * dx + dz * dz);
                if (d > 60.0f) { character.pos[0] += dx / d * (d - 60.0f); character.pos[2] += dz / d * (d - 60.0f); }
            }
#if defined(HLE_RUN_SKIP) || defined(HLE_RUN_PICK) || defined(HLE_RUN_UPDOWN) || defined(HLE_RUN_CLIMB)   // a set card: HLE_RUN_PICK moves right that many, Skip is left of the first
            {
                static int k;
                if (!tower_draft_is_open()) k = 0;
                else {
                    k++;
#if defined(HLE_RUN_CLIMB)    // take a boon while any are offered (so they run out), else the first pact
                    static int want;
                    if (k == 1) {
                        extern int tower_draft_card(int i);
                        want = 0;
                        for (int i = 2; i >= 0; i--) if (tower_draft_card(i) >= 0) want = i;
                        debugf("[climb] cards %d %d %d, taking %d\n", tower_draft_card(0), tower_draft_card(1), tower_draft_card(2), want);
                    }
                    if (k >= 10 && k < 10 + 6 * want && (k - 10) % 6 == 0) { pressed->d_right = 1; in->btn.d_right = 1; }
#elif defined(HLE_RUN_SKIP)
                    if (k == 10) { pressed->d_down = 1; in->btn.d_down = 1; }     // Down to Skip
#elif defined(HLE_RUN_UPDOWN)   // Right to the second card, Down to Skip, Up back: the second card is taken
#ifdef HLE_RUN_STICK            // (the same with the control stick)
                    if (k >= 10 && k < 13) in->stick_x = 70;
                    if (k >= 20 && k < 23) in->stick_y = -70;
                    if (k >= 30 && k < 33) in->stick_y = 70;
#else
                    if (k == 10) { pressed->d_right = 1; in->btn.d_right = 1; }
                    if (k == 20) { pressed->d_down = 1; in->btn.d_down = 1; }
                    if (k == 30) { pressed->d_up = 1; in->btn.d_up = 1; }
#endif
                    if (k == 40) debugf("[updown] the draft reads: card %d selected\n", tower_draft_selected());
#else
                    if (k >= 10 && k < 10 + 6 * HLE_RUN_PICK && (k - 10) % 6 == 0) { pressed->d_right = 1; in->btn.d_right = 1; }
#endif
#ifndef HLE_DRAFT_HOLD
#define HLE_DRAFT_HOLD 60       // frames the draft stays up (longer: for a screenshot)
#endif
                    if (k == HLE_DRAFT_HOLD) { pressed->a = 1; in->btn.a = 1; }
                }
            }
#else
            if (tower_draft_is_open() && t % 40 == 0) { pressed->a = 1; in->btn.a = 1; }
#endif
#ifdef HLE_RUN_PAUSE        // on floor 2: pause, open Run (one down) and stay on the page
            if (lastFloor == 2) {
                if (t == 30) pressed->start = 1;
                if (t == 50) { pressed->d_down = 1; in->btn.d_down = 1; }
                if (t == 70) { pressed->a = 1; in->btn.a = 1; }
                if (t == 400 || t == 420) { pressed->b = 1; in->btn.b = 1; }     // back, then close the pause menu
                if (t >= 70 && t < 430) return;
            }
#endif
#ifndef HLE_RUN_FLOORS
#define HLE_RUN_FLOORS 7
#endif
            if (lastFloor == HLE_RUN_FLOORS && t == 60) { phase = 3; t0 = frame; character_apply_damage(9999.0f); }
        } else if (phase == 3) {
            if (t == 30) debugf("[run] died: state %d, \"%s\"\n", character_get_state(), tower_death_summary());
            if (t > 150 && t % 30 == 0) { pressed->a = 1; in->btn.a = 1; in->btn.a = 1; }
            if (t > 150 && !boss) { debugf("[run] back at the title, run %s\n", tower_active() ? "STILL ACTIVE" : "ended"); phase = 4; }
        }
        return;
    }
#endif
#ifdef HLE_TEST_QUIT
    // Pause mid-fight > Main Menu (4 down: Resume, Restart, Settings, Controls, Main Menu) > A:
    // back at the title, then into the fight again
    {
        static int start = -1, again = 0;
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT) { start = frame; debugf("[quit] fight at %d\n", frame); }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
        if (t == 60) { pressed->start = 1; in->btn.start = 1; }
        if (t == 80 || t == 90 || t == 100 || t == 110) pressed->d_down = 1;
        if (t == 125) { pressed->a = 1; in->btn.a = 1; }
        if (t == 160) debugf("[quit] after Main Menu: boss %s, title menu %s\n", boss ? "still there" : "gone", menu_controller_is_active() ? "open" : "closed");
        if (t > 200 && t < 1400 && t % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        if (!again && t > 200 && boss && boss->mode == BOSS_MODE_COMBAT) { again = 1; debugf("[quit] fight again at %d\n", frame); }
        return;
    }
#endif
#ifdef HLE_TEST_AUDIO
    // Every new sound and track loads and plays: in the fight, play each in turn and
    // check the mixer has a channel going a few frames later
    {
        static int start = -1, step = 0, fails = 0;
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT) start = frame;
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        static const char *const music[] = { "rom:/audio/music/shadowborn_pursuit-22k.wav64", "rom:/audio/music/shackled_sun_metal-22k.wav64" };
        const int nsfx = SCENE1_SFX_SWING_BOSS - SCENE1_SFX_DRAGON_ROAR + 1, total = nsfx + 2;
        int t = frame - start;
        // One at a time, 3.5 s apart. A sound counts as silent only if three tries in a row
        // start no channel (another sound ending in the same frame can hide one try)
        static int before, tries;
        if (step < total && t == 20 + step * 130 + tries * 30) {
            before = 0;
            for (int ch = SFX_CH_FIRST; ch <= SFX_CH_LAST; ch++) before += mixer_ch_playing(ch);
            if (step < nsfx) audio_play_scene_sfx_dist(SCENE1_SFX_DRAGON_ROAR + step, 1.0f, 0.0f);
            else audio_play_music(music[step - nsfx], true);
        }
        if (step < total && t == 20 + step * 130 + tries * 30 + 1) {
            bool on = false;
            if (step < nsfx) { int n = 0; for (int ch = SFX_CH_FIRST; ch <= SFX_CH_LAST; ch++) n += mixer_ch_playing(ch); on = n > before; }
            else on = audio_is_music_playing();
            if (!on && ++tries < 3) return;
            if (!on) fails++;
            debugf("[audio] %d %s\n", step, on ? "playing" : "SILENT");
            step++; tries = 0;
            if (step == total) debugf("[audio] self-check: %s (%d silent)\n", fails ? "FAIL" : "ok", fails);
        }
        return;
    }
#endif
#ifdef HLE_TEST_CRUCIFIX
#ifndef HLE_CX_DIST
#define HLE_CX_DIST 120.0f      // the camera's distance and height
#define HLE_CX_Y 20.0f
#endif
    // Guardian Unbound's hanging pose (forced on by boss_anim.c): reach phase 2, the player
    // stands off 300 units, the camera between them looking at the boss (front, then side)
    {
        static int phase = 0, t0 = 0;
        Boss *boss = boss_get_instance();
        int t = frame - t0;
        if (phase == 0) {
            if (boss && boss->mode == BOSS_MODE_COMBAT && boss->phaseIndex == 1) { phase = 1; t0 = frame; }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {
            if (t == 60) boss_apply_damage(boss, boss->maxHealth * 2.0f);
            if (t > 60 && boss->phaseIndex == 2 && boss->mode == BOSS_MODE_COMBAT) { phase = 2; t0 = frame; debugf("[cx] fight at %d\n", frame); }
            else if (t > 90 && frame % 20 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else {
            float dx = character.pos[0] - boss->pos[0], dz = character.pos[2] - boss->pos[2], d = sqrtf(dx * dx + dz * dz) + 1e-3f;
            dx /= d; dz /= d;
            if (d < 250.0f) { character.pos[0] = boss->pos[0] + dx * 300.0f; character.pos[2] = boss->pos[2] + dz * 300.0f; }
            static const float VA[3] = { 0.0f, 1.5708f, 0.7854f };     // front, side, three quarters: 1.5 s each
            int v = (t / 90) % 3;
            float ca = cosf(VA[v]), sa = sinf(VA[v]), cx = dx * ca - dz * sa, cz = dz * ca + dx * sa;
            const char *side = v == 0 ? "" : "side";
            cameraLockOnActive = false;
            customCamPos = (T3DVec3){{ boss->pos[0] + cx * HLE_CX_DIST, boss->pos[1] + HLE_CX_Y, boss->pos[2] + cz * HLE_CX_DIST }};
            customCamTarget = (T3DVec3){{ boss->pos[0], boss->pos[1] + HLE_CX_Y, boss->pos[2] }};
            camera_mode(CAMERA_CUSTOM);
            if (t % 90 == 0) debugf("[cx] t %d view %d %s\n", t, v, side);
        }
        return;
    }
#endif
#ifdef HLE_TEST_UNBOUND
    // Hazard check: reach Guardian Unbound, circle it for 150 s (every hit is logged with
    // its source, the player is topped up), kill it, then keep walking through where its
    // swords and walls were for 10 s: nothing may land after the death.
    {
        static int phase = 0, t0 = 0;
        Boss *boss = boss_get_instance();
        int t = frame - t0;
        if (phase == 0) {
            if (boss && boss->mode == BOSS_MODE_COMBAT && boss->phaseIndex == 1) { phase = 1; t0 = frame; }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {
            if (t == 60) boss_apply_damage(boss, boss->maxHealth * 2.0f);
            if (t > 60 && boss->phaseIndex == 2 && boss->mode == BOSS_MODE_COMBAT) { phase = 2; t0 = frame; debugf("[ub] fight at %d\n", frame); }
#ifndef HLE_UB_WATCH     // HLE_UB_WATCH: sit through the phase-2 cutscene (the clear time must leave it out)
            else if (t > 90 && frame % 20 == 0) { pressed->a = 1; in->btn.a = 1; }
#endif
        } else if (phase == 2) {
#ifdef HLE_UB_PAUSE     // the pause menu open for 300 frames mid-fight (left out of the clear time too)
            if (t == 600 || t == 900) { pressed->start = 1; in->btn.start = 1; debugf("[ub] start pressed at %d ms %lu\n", frame, (unsigned long)get_ticks_ms()); }
            if (t >= 600 && t < 905) return;
#endif
            in->stick_x = (int)(80 * cosf(t * 0.02f)); in->stick_y = (int)(80 * sinf(t * 0.02f));   // circle about
            if (t % 300 == 0) debugf("[ub] t %d hp %.0f boss state %d\n", t, (double)character_get_health(), boss->state);
            // the kill lands mid-sweep, with the walls up (the sweep's 3rd cycle, about 14 s in)
            static int sweeps, sweepAt, wasActive;
            int active = msa_ground_sweep_is_active();
            if (active && !wasActive && ++sweeps == 3) sweepAt = frame;
            wasActive = active;
            if (sweepAt && frame - sweepAt == 500) { boss_apply_damage(boss, boss->maxHealth * 2.0f); debugf("[ub] killed at %d ms %lu\n", frame, (unsigned long)get_ticks_ms()); phase = 3; t0 = frame; }
        }
        if (phase >= 2) {   // overhead shot of the player so the swords and walls are in frame
            cameraLockOnActive = false;
            float *p = character.pos;
            customCamPos = (T3DVec3){{ p[0] + 1.0f, p[1] + 260.0f, p[2] + 120.0f }};
            customCamTarget = (T3DVec3){{ p[0], p[1], p[2] }};
            camera_mode_smooth(CAMERA_CUSTOM, 0.01f);
        }
        if (phase == 3) {
            in->stick_x = (int)(80 * cosf(t * 0.03f)); in->stick_y = (int)(80 * sinf(t * 0.03f));
            if (t == 300) { debugf("[ub] after-death window over ms %lu\n", (unsigned long)get_ticks_ms()); phase = 4; }
        }
        return;
    }
#endif
#ifdef HLE_TEST_ANIMVIEW
    // A Guardian clip (HLE_TEST_ANIMVIEW = its index; with HLE_TEST_FREEZE_BOSS), posed at
    // 0, 0.4, 0.8 ... s, 60 frames each, from the side
    {
        static int start = -1;
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) start = frame;
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
        T3DAnim *a = ((T3DAnim**)boss->animations)[HLE_TEST_ANIMVIEW];
        if (t == 1) { t3d_anim_attach(a, (T3DSkeleton*)boss->skeleton); t3d_anim_set_playing(a, true); debugf("[anim] clip %d lasts %.2f s\n", HLE_TEST_ANIMVIEW, (double)a->animRef->duration); }
        float at = (t / 60) * 0.4f;
        if (t % 60 == 1) { t3d_anim_set_time(a, at); t3d_anim_update(a, 0.0f); t3d_skeleton_update((T3DSkeleton*)boss->skeleton); }
        if (t % 60 == 30) { T3DVec3 h = {0}; boss_get_head_world_pos(boss, &h); debugf("[anim] at %.1f s head y %.1f\n", (double)at, (double)h.v[1]); }
        cameraLockOnActive = false;
        character.pos[0] = boss->pos[0] + 120.0f; character.pos[2] = boss->pos[2];
        customCamPos = (T3DVec3){{ boss->pos[0] + 60.0f, boss->pos[1] + 30.0f, boss->pos[2] + 70.0f }};
        customCamTarget = (T3DVec3){{ boss->pos[0], boss->pos[1] + 22.0f, boss->pos[2] }};
        camera_mode_smooth(CAMERA_CUSTOM, 0.01f);
        return;
    }
#endif
#ifdef HLE_TEST_COVER
    // Cover art poses (with HLE_TEST_FREEZE_BOSS): the knight facing the Guardian;
    // camera shots switch every 60 frames (see the shot list in the log)
    {
        static int start = -1;
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) start = frame;
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
        cameraLockOnActive = false;
        character.pos[0] = boss->pos[0] - (t < 120 ? 600.0f : 70.0f); character.pos[2] = boss->pos[2] + 6.0f;   // out of the Guardian's shot
        character.rot[1] = T3D_PI * 1.5f;
        static const float shots[][6] = {
            { -80, 6, 50,    -5, 34, 0 },      // 0 (t < 120): the Guardian, from below
            { -40, 14, 26,   -70, 14, 6 },     // 1 (t >= 120): the knight, close, mid-swing
        };
        int k = t < 120 ? 0 : 1;
        customCamPos = (T3DVec3){{ shots[k][0], shots[k][1], shots[k][2] }};
        customCamTarget = (T3DVec3){{ shots[k][3], shots[k][4], shots[k][5] }};
        camera_mode(CAMERA_CUSTOM);
        if (t % 60 == 0) debugf("[cover] shot %d at frame %d\n", k, frame);
        if (t >= 125 && t < 129) { pressed->b = 1; in->btn.b = 1; }     // a swing, for an action pose
        return;
    }
#endif
#ifdef HLE_TEST_DUMMY
    // Threat measurement: a passive, invincible dummy at the start position; logs the
    // damage each boss lands over 120 s. HLE_DUMMY_BOSS: 1 Guardian, 2 Unbound,
    // 3 Fallen Warrior, 4 Shackled Sun, 5 Shadowborne.
    {
        extern float deltaTime;
        static int phase = 0, step = 0, t0 = 0;
        static float tm;
        Boss *boss = boss_get_instance();
        int t = frame - t0;
        if (phase == 0) {       // reach the title menu
            if (menu_controller_is_active()) { phase = 1; t0 = frame; }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {    // pick the fight
            const char *seq = HLE_DUMMY_BOSS == 3 ? "DAA" : HLE_DUMMY_BOSS == 4 ? "DADA" : HLE_DUMMY_BOSS == 5 ? "DADDA" : "A";
            int n = (int)strlen(seq);
            if (t >= 20 && step < n) { if (seq[step] == 'D') pressed->d_down = 1; else { pressed->a = 1; in->btn.a = 1; } step++; t0 = frame; }
            if (step == n) { phase = 2; t0 = frame; }
        } else if (phase == 2) {    // through the intro (A skips cutscenes)
            int want = HLE_DUMMY_BOSS <= 2 ? 1 : HLE_DUMMY_BOSS;
            if (boss && boss->mode == BOSS_MODE_COMBAT && boss->phaseIndex == want && character_get_state() == CHAR_STATE_NORMAL) {
                if (HLE_DUMMY_BOSS == 2) { boss_apply_damage(boss, boss->maxHealth * 2.0f); phase = 3; }
                else {
#ifdef HLE_DUMMY_HALF   // measure the second half: past the Shackled Sun's break (and its cutscene-free 2 s)
                    boss_apply_damage(boss, boss->maxHealth * 0.55f);
#endif
#ifdef HLE_DUMMY_SKIN   // the fight in this skin (e.g. a cloak, to time its cloth)
                    character_skin_set(HLE_DUMMY_SKIN);
#endif
                    phase = 4; tm = 0.0f; hle_dummy_damage = 0.0f; hle_dummy_hits = 0; debugf("[dummy] start\n"); }
            } else if (t > 60 && frame % 20 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 3) {    // Unbound: skip the transformation
            if (boss->phaseIndex == 2 && boss->mode == BOSS_MODE_COMBAT) {
#ifdef HLE_DUMMY_SKIN
                character_skin_set(HLE_DUMMY_SKIN);
#endif
                phase = 4; tm = 0.0f; hle_dummy_damage = 0.0f; hle_dummy_hits = 0; debugf("[dummy] start\n"); }
            else if (frame % 20 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 4) {
#ifdef HLE_DUMMY_REAL    // the bot playtest: it fights (HLE_DUMMY_STRIKE, HLE_DUMMY_ROLL), drinks below 40% and says how it went
            {
                static int potions0 = -1, done;
                if (potions0 < 0) potions0 = character.healthPotions;
                if (!done && character.health < character.maxHealth * 0.4f && character_get_state() == CHAR_STATE_NORMAL) {
                    extern bool character_try_use_health_potion(void);
                    if (character_try_use_health_potion()) debugf("[bot] t %.0f s: potion at %.0f health\n", (double)tm, (double)character.health);
                }
                bool won = boss && boss->state == BOSS_STATE_DEAD && (HLE_DUMMY_BOSS != 1 || boss->phaseIndex == 2);
                bool lost = character_get_state() == CHAR_STATE_DEAD;
                if (!done && (won || lost || tm > 300.0f)) {
                    done = 1;
                    debugf("[bot] boss %d %s at %.0f s: took %.0f in %d hits, %d potions used, %.0f health left, boss at %.0f/%.0f\n", HLE_DUMMY_BOSS,
                           won ? "beaten" : lost ? "player died" : "timed out", (double)tm, (double)hle_dummy_damage, hle_dummy_hits,
                           potions0 - character.healthPotions, (double)character.health, (double)(boss ? boss->health : 0), (double)(boss ? boss->maxHealth : 0));
                }
                if (done) return;
                if (HLE_DUMMY_BOSS == 1 && boss && boss->phaseIndex == 1 && boss->state == BOSS_STATE_DEAD) {   // the story: on to Unbound
                    if (frame % 20 == 0) { pressed->a = 1; in->btn.a = 1; }
                    tm += deltaTime;
                    return;
                }
                // Strike in the boss's openings, keep away and roll out while it attacks
                if (boss && boss->mode == BOSS_MODE_COMBAT) {
                    extern bool hle_open_now;
                    static int k, lastRoll;
                    k++;
                    if (k % 60 == 1) pressed->z = 1;       // (re)lock on
                    in->btn.z = 1;
                    float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2], d = sqrtf(dx * dx + dz * dz);
                    if (hle_open_now) {
                        if (d > 38.0f) in->stick_y = 70;                       // in to sword reach
                        else if (k % 20 == 0) { pressed->b = 1; in->btn.b = 1; }
                    } else {
                        in->stick_y = -70;                                     // back off
                        if (d < 70.0f && k - lastRoll > 45 && character.stamina > 30.0f) { pressed->a = 1; in->btn.a = 1; lastRoll = k; }
                    }
                }
                tm += deltaTime;
                return;
            }
#endif
#ifdef HLE_DUMMY_CAM      // the camera on the boss, from its front a little below (a pose check)
            if (boss) {
                cameraLockOnActive = false;
                float a = boss->rot[1];
                customCamPos = (T3DVec3){{ boss->pos[0] - sinf(a) * 120.0f, boss->pos[1] + 30.0f, boss->pos[2] + cosf(a) * 120.0f }};
                customCamTarget = (T3DVec3){{ boss->pos[0], boss->pos[1] + 35.0f, boss->pos[2] }};
                camera_mode_smooth(CAMERA_CUSTOM, 0.01f);
                if (boss->state == BOSS_STATE_AERIAL_SWORD_BARRAGE && frame % 20 == 0) debugf("[cam] f%d aerial, y %.0f\n", frame, (double)boss->pos[1]);
            }
#endif
#ifdef HLE_DUMMY_SIDECAM  // the camera off to the side of the boss and the player, both in view (attack videos)
            if (boss) {
                cameraLockOnActive = false;
                float mx = (boss->pos[0] + character.pos[0]) * 0.5f, mz = (boss->pos[2] + character.pos[2]) * 0.5f;
                float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2], d = sqrtf(dx * dx + dz * dz) + 1e-3f;
#ifndef HLE_SIDECAM_K       // how far out (2: twice as far and high)
#define HLE_SIDECAM_K 1.0f
#endif
                float back = fmaxf(110.0f, d * 1.05f) * HLE_SIDECAM_K;
                customCamPos = (T3DVec3){{ mx - dz / d * back - dx / d * 25.0f, character.pos[1] + 55.0f * HLE_SIDECAM_K, mz + dx / d * back - dz / d * 25.0f }};
                customCamTarget = (T3DVec3){{ mx, character.pos[1] + 18.0f, mz }};
                camera_mode_smooth(CAMERA_CUSTOM, 0.02f);
            }
#endif
#ifdef HLE_DUMMY_CAM_NEAR  // close on the boss from its front-left (past the player): its poses
            if (boss) {
                cameraLockOnActive = false;
                float a = boss->rot[1] + 0.9f;
                customCamPos = (T3DVec3){{ boss->pos[0] - sinf(a) * 80.0f, boss->pos[1] + 28.0f, boss->pos[2] + cosf(a) * 80.0f }};
                customCamTarget = (T3DVec3){{ boss->pos[0], boss->pos[1] + 18.0f, boss->pos[2] }};
                camera_mode_smooth(CAMERA_CUSTOM, 0.01f);
            }
#endif
#ifdef HLE_DUMMY_CAM_PORTRAIT  // the boss alone, centred, from its front (turned until the camera is inside the hall): README shots
            if (boss) {
                cameraLockOnActive = false;
#ifndef HLE_PORTRAIT_D
#define HLE_PORTRAIT_D 110.0f
#endif
#ifndef HLE_PORTRAIT_H
#define HLE_PORTRAIT_H 30.0f
#endif
#ifndef HLE_PORTRAIT_LOOK
#define HLE_PORTRAIT_LOOK 30.0f
#endif
#ifndef HLE_PORTRAIT_TURN
#define HLE_PORTRAIT_TURN 0.5f
#endif
                float a = boss->rot[1] + HLE_PORTRAIT_TURN, cx = 0, cz = 0;
                for (int i = 0; i < 16; i++, a += 0.39f) {
                    cx = boss->pos[0] - sinf(a) * HLE_PORTRAIT_D; cz = boss->pos[2] + cosf(a) * HLE_PORTRAIT_D;
                    float hx = (cx + boss->pos[0]) * 0.5f, hz = (cz + boss->pos[2]) * 0.5f;
                    if (scene_bounds_check_room(cx, character.pos[1], cz) && scene_bounds_check_room(hx, character.pos[1], hz)) break;
                }
                customCamPos = (T3DVec3){{ cx, boss->pos[1] + HLE_PORTRAIT_H, cz }};
                customCamTarget = (T3DVec3){{ boss->pos[0], boss->pos[1] + HLE_PORTRAIT_LOOK, boss->pos[2] }};
                camera_mode_smooth(CAMERA_CUSTOM, 0.01f);
                if (frame % 10 == 0) debugf("[portrait] f%d state %d y %.0f\n", frame, (int)boss->state, (double)boss->pos[1]);
            }
#endif
#ifdef HLE_DUMMY_CAM_HIGH  // a wide view from high up, fixed in the world, on the boss and the player (aim and turning)
            if (boss) {
                cameraLockOnActive = false;
                float mx = (boss->pos[0] + character.pos[0]) * 0.5f, mz = (boss->pos[2] + character.pos[2]) * 0.5f;
#ifndef HLE_CAM_HIGH_K     // how far out (2: twice as high and far back)
#define HLE_CAM_HIGH_K 1.0f
#endif
                customCamPos = (T3DVec3){{ mx, boss->pos[1] + 120.0f * HLE_CAM_HIGH_K, mz - 95.0f * HLE_CAM_HIGH_K }};
                customCamTarget = (T3DVec3){{ mx, boss->pos[1] + 10.0f, mz }};
                camera_mode_smooth(CAMERA_CUSTOM, 0.01f);
            }
#endif
#ifdef HLE_DUMMY_ORBIT   // run round the boss about 50 away (a sidestep the whole time)
            if (boss) {
                float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2], d = sqrtf(dx * dx + dz * dz) + 1e-3f;
                float k = fmaxf(-1.0f, fminf(1.0f, (d - 50.0f) * 0.08f));
                float wx = -dz / d + dx / d * k, wz = dx / d + dz / d * k;      // round it, and in or out to the radius
                float ya = cameraAngleX;     // the stick back from world (character.c, compute_desired_velocity)
                in->stick_y = (int)(70 * (wx * -fm_sinf(ya) + wz * -fm_cosf(ya)));
                in->stick_x = (int)(70 * (wx * fm_cosf(ya) + wz * -fm_sinf(ya)));
                if (frame % 15 == 0) debugf("[orbit] d %.0f player %.0f,%.0f boss %.0f,%.0f stick %d,%d cam %.2f\n", (double)d, (double)character.pos[0], (double)character.pos[2],
                                            (double)boss->pos[0], (double)boss->pos[2], in->stick_x, in->stick_y, (double)ya);
            }
#endif
#ifdef HLE_DUMMY_SIDESTEP   // the Fallen Warrior: step sideways for this many frames the moment a swing commits its aim
            if (boss) {          // (HLE_SIDESTEP_EARLY: as the swing begins, before it commits: it should turn and follow)
#ifdef HLE_SIDESTEP_EARLY
                extern int hle_fk_swing; int now = hle_fk_swing;
#else
                extern int hle_fk_commit; int now = hle_fk_commit;
#endif
                static int seen, left, side = 1;
                if (now != seen) { seen = now; left = HLE_DUMMY_SIDESTEP; side = -side; }
                if (left > 0) {
                    left--;
                    float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2], d = sqrtf(dx * dx + dz * dz) + 1e-3f;
                    float wx = -dz / d * side, wz = dx / d * side, ya = cameraAngleX;     // square to the boss (the stick from world as in HLE_DUMMY_ORBIT)
                    in->stick_y = (int)(80 * (wx * -fm_sinf(ya) + wz * -fm_cosf(ya)));
                    in->stick_x = (int)(80 * (wx * fm_cosf(ya) + wz * -fm_sinf(ya)));
                    if (cameraLockOnActive) { in->stick_x = 80 * side; in->stick_y = 0; }     // locked on, the stick is already square to it
                }
            }
#endif
#ifdef HLE_TEST_STAGGER   // the stagger meter filled in steps: a full stagger every other second, and again
            if (boss && (frame % 60) == 0 && boss->staggerT <= 0.0f) { boss->poise = fminf(1.0f, boss->poise + 0.55f); boss->poiseIdleT = 0.0f;
                debugf("[stagtest] f%d poise %.2f\n", frame, (double)boss->poise); }
#endif
#ifdef HLE_DUMMY_MOVE   // keep running in a wide circle (the cloth in motion)
            in->stick_x = (int)(70 * cosf(tm * 0.8f)); in->stick_y = (int)(70 * sinf(tm * 0.8f));
#endif
#ifdef HLE_DUMMY_CENTER   // held in the middle of the hall, facing the boss (moves that travel, away from the walls)
            if (boss) { character.pos[0] = 0.0f; character.pos[2] = 0.0f; }
#endif
#ifdef HLE_DUMMY_PIN      // held where it started: a blow's knockback can't shove it out past a wall (Shadowborne then stood stuck at the wall)
            { static float pin[2]; static bool set; if (!set) { pin[0] = character.pos[0]; pin[1] = character.pos[2]; set = true; }
              character.pos[0] = pin[0]; character.pos[2] = pin[1]; }
#endif
#ifdef HLE_DUMMY_KEEP     // locked on, keep about this far from the boss, strafing round it (its moves at range, from the side)
            if (boss) { static int k; if (k++ % 60 == 0) pressed->z = 1;
              in->btn.z = 1;
              float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2], d = sqrtf(dx * dx + dz * dz);
              in->stick_y = d < HLE_DUMMY_KEEP - 15.0f ? -70 : d > HLE_DUMMY_KEEP + 15.0f ? 70 : 0;
#ifndef HLE_DUMMY_KEEP_STILL    // (or only in and out while the boss isn't attacking: no strafe)
              in->stick_x = 45;
#else
              { extern bool hle_open_now; if (!hle_open_now) in->stick_y = 0; }
#endif
            }
#endif
#ifdef HLE_DUMMY_STRIKE   // and fight back: locked on, walk in, swing (the stagger meter, hit-stop)
            if (boss) { static int k; k++;     // (no boss once the run is back at the title)
              if (k == 1) pressed->z = 1;
              in->btn.z = 1;
              float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2];
              if (sqrtf(dx * dx + dz * dz) > 40.0f) in->stick_y = 70;
#ifdef HLE_STRIKE_OPEN     // swing only while the boss isn't attacking (a player who dodges its moves)
              extern bool hle_open_now;
              if (k % HLE_STRIKE_EVERY == 0 && hle_open_now) { pressed->b = 1; in->btn.b = 1; }
#else
              if (k % HLE_STRIKE_EVERY == 0) { pressed->b = 1; in->btn.b = 1; }
#endif
#ifdef HLE_STRIKE_STAMINA     // a player who manages stamina: whole combos
              character.stamina = character_get_max_stamina();
#endif
            }
#endif
#ifdef HLE_DUMMY_ROLL     // roll every 0.7 s: some catch a blow right at the start (a perfect dodge)
            { static int k; if (++k % 42 == 0) { pressed->a = 1; in->btn.a = 1; } }
#endif
            float prev = tm;
            tm += deltaTime;
            static int heapMin = 1 << 30;     // the lowest free heap, sampled each second
            if ((int)tm != (int)prev) { heap_stats_t h; sys_get_heap_stats(&h); if (h.total - h.used < heapMin) heapMin = h.total - h.used; }
            if ((int)(tm / 30.0f) != (int)(prev / 30.0f))
                debugf("[dummy] boss %d t %.0f s damage %.0f hits %d | open %.1f%% of the time, %d gaps of %.2f s | %.1f fps, heap min %d KB free\n", HLE_DUMMY_BOSS, (double)tm,
                       (double)hle_dummy_damage, hle_dummy_hits, (double)(100.0f * hle_open_time / (tm > 0 ? tm : 1)), hle_open_gaps,
                       (double)(hle_open_gaps ? hle_open_time / hle_open_gaps : 0.0f), (double)display_get_fps(), heapMin / 1024);
        }
        return;
    }
#endif
#ifdef HLE_TEST_HEAVY
    // Hold B for 0.8 s in the fight: must turn into the strong attack
    {
        static int start = -1;
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) start = frame;
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
        if (t >= 60 && t < 90) in->btn.b = 1;
        if (t == 60) pressed->b = 1;
        if (t < 60) {       // keep right in front of the boss, facing it
            float a = boss->rot[1];
            character.pos[0] = boss->pos[0] - fm_sinf(a) * 30.0f; character.pos[2] = boss->pos[2] + fm_cosf(a) * 30.0f;
            character.rot[1] = a + T3D_PI;
        }
        if (t == 140) { pressed->b = 1; in->btn.b = 1; }    // a light attack, for comparison
        if (t == 59) debugf("[heavy] frame %d knight %.0f,%.0f boss %.0f,%.0f\n", frame, (double)character.pos[0], (double)character.pos[2], (double)boss->pos[0], (double)boss->pos[2]);
        if (t >= 60 && t < 170 && t % 4 == 0) debugf("[heavy] t %d state %d boss hp %.1f stamina %.0f\n", t, character_get_state(), (double)boss->health, (double)character.stamina);
        return;
    }
#endif
#ifdef HLE_TEST_ENDING
    {
        static int start = -1;
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT) { start = frame; debugf("[end] fight at %d\n", frame); }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (frame == start + 30) {
            extern void scene_test_start_ending(void);
            scene_test_start_ending();
            debugf("[end] ending at %d\n", frame);
        } else if (frame % 150 == 0) {
            debugf("[end] f%d boss %p\n", frame, (void *)boss);
        }
        return;
    }
#endif
#ifdef HLE_TEST_BONUS
    // Title -> Bonus -> Fallen Warrior; watch the intro, stand still (it leaps at range),
    // kill it, watch the outro and the return to the title
    {
        static int phase = 0, t0 = 0, step = 0;
        Boss *boss = boss_get_instance();
        int t = frame - t0;
        static int heapMin = 1 << 30;     // the lowest free heap in the fight, sampled every 10 frames
        if (phase == 3 && frame % 10 == 0) { heap_stats_t h; sys_get_heap_stats(&h); if (h.total - h.used < heapMin) heapMin = h.total - h.used; }
        if (frame % 60 == 0) debugf("[bonus] f%d phase %d boss %d mode %d state %d hp %.0f heap min %d KB\n", frame, phase,
                                    boss ? boss->phaseIndex : -1, boss ? boss->mode : -1, boss ? boss->state : -1,
                                    boss ? (double)boss->health : 0.0, phase == 3 ? heapMin / 1024 : -1);
        if (phase == 0) {               // tap A until the title menu opens
            if (menu_controller_is_active()) { phase = 1; t0 = frame; }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {        // Down, Down (Bonus), A, A (Fallen Warrior)
#if defined(HLE_TEST_BONUS_SUN)
            static const char seq[] = "DADA";       // Bonus > The Shackled Sun
#elif defined(HLE_TEST_BONUS_DRAGON)
            static const char seq[] = "DADDA";      // Bonus > Shadowborne
#else
            static const char seq[] = "DAA";        // Bonus > Fallen Warrior
#endif
            int n = (int)sizeof(seq) - 1;
            if (t >= 20 && step < n) {
                if (seq[step] == 'D') pressed->d_down = 1; else { pressed->a = 1; in->btn.a = 1; }
                step++; t0 = frame;
            }
            if (step == n && boss) { phase = 2; t0 = frame; debugf("[bonus] scene at %d\n", frame); }
        } else if (phase == 2) {
            if (boss && boss->phaseIndex >= 3 && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) {
                phase = 3; t0 = frame; debugf("[bonus] fight at %d\n", frame);
            }
        } else if (phase == 3) {
#ifdef HLE_TEST_FK_LEAP
            in->stick_x = 70;       // circle it (locked on) for a side view
#endif
#ifdef HLE_TEST_BONUS_SUN
            if (t == 450) { boss_apply_damage(boss, boss->maxHealth * 0.55f); debugf("[bonus] half at %d\n", frame); }
#endif
#ifdef HLE_TEST_STRIKE
            // Can the sword land? Locked on, walk in and swing for 60 s (topped up): every
            // drop in the boss's health is a hit
            {
                static float lastHp; static int hits; static float lost;
                if (t == 1) { lastHp = boss->health; pressed->z = 1; }
                in->btn.z = 1;
                float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2];
                if (sqrtf(dx * dx + dz * dz) > 45.0f) in->stick_y = 70;
                if (t % HLE_STRIKE_EVERY == 0) { pressed->b = 1; in->btn.b = 1; }
                {   // the blade's closest approach to the boss's capsule this swing (surface gap)
                    static float gap = 1e9f, lowY, tipY;
                    extern T3DVec3 charWeaponCapA, charWeaponCapB;     // the blade, as the collision system has it
                    const float *sb = charWeaponCapA.v, *st = charWeaponCapB.v;
                    if (character_get_state() == CHAR_STATE_ATTACKING) {
                        const float *bp = boss->pos; const T3DVec3 *ca = &boss->capsuleCollider.localCapA, *cb = &boss->capsuleCollider.localCapB;
                        for (int i = 0; i <= 8; i++) {
                            float p[3]; for (int m = 0; m < 3; m++) p[m] = sb[m] + (st[m] - sb[m]) * i / 8.0f;
                            float a0[3] = { bp[0] + ca->v[0], bp[1] + ca->v[1], bp[2] + ca->v[2] }, u[3] = { cb->v[0] - ca->v[0], cb->v[1] - ca->v[1], cb->v[2] - ca->v[2] };
                            float l2 = u[0]*u[0] + u[1]*u[1] + u[2]*u[2], k = l2 > 0 ? ((p[0]-a0[0])*u[0] + (p[1]-a0[1])*u[1] + (p[2]-a0[2])*u[2]) / l2 : 0;
                            k = k < 0 ? 0 : k > 1 ? 1 : k;
                            float e[3] = { a0[0] + u[0]*k - p[0], a0[1] + u[1]*k - p[1], a0[2] + u[2]*k - p[2] };
                            float g = sqrtf(e[0]*e[0] + e[1]*e[1] + e[2]*e[2]) - boss->capsuleCollider.radius - 2.0f;
                            if (g < gap) { gap = g; lowY = p[1] - character.pos[1]; tipY = st[1] - character.pos[1]; }
                        }
                    }
                    if (t % 24 == 23) { if (gap < 1e8f) debugf("[strike] blade gap %.0f (closest point %.0f up, tip %.0f up)\n", (double)gap, (double)lowY, (double)tipY); gap = 1e9f; }
                }
                if (t % 24 == 12) debugf("[strike] swing: dist %.0f, boss %.0f above the player, %s\n", (double)sqrtf(dx * dx + dz * dz),
                                         (double)(boss->pos[1] - character.pos[1]), boss->health < lastHp - 0.01f ? "hit" : "-");
                if (boss->health < lastHp - 0.01f) { hits++; lost += lastHp - boss->health; }
                lastHp = boss->health;
                if (t == 2000) {
                    debugf("[strike] boss %d: %d hits, %.0f of %.0f health in 60 s\n", boss->phaseIndex, hits, (double)lost, (double)boss->maxHealth);
                    boss_apply_damage(boss, boss->maxHealth * 2.0f); phase = 4; t0 = frame;
                }
            }
#elif defined(HLE_DRAGON_FACE)
            // Its head up close: in front of the snout (level, below, above), at its side (the
            // jaw against the neck), from the front at 3/4 and from behind it; 40 frames a view
            {
                extern void dragon_boss_hle_head(float p[3], float z[3], float y[3]);
                cameraLockOnActive = false;
                if (t == 1) { character.pos[0] = boss->pos[0] - 260.0f; character.pos[2] = boss->pos[2] + 200.0f; }
                float hp[3], hz[3], hy[3];
                dragon_boss_hle_head(hp, hz, hy);
                float hx[3] = { hy[1] * hz[2] - hy[2] * hz[1], hy[2] * hz[0] - hy[0] * hz[2], hy[0] * hz[1] - hy[1] * hz[0] };
                static const float FZ[6] = { 60, 55, 55, 12, 45, -28 }, FY[6] = { 4, -22, 26, -2, 8, 30 }, FX[6] = { 0, 0, 0, 45, 32, 6 };
                int v = (t / 40) % 6;      // (5: from behind and above, onto the back of the skull)
                float c[3], g[3];
                for (int m = 0; m < 3; m++) { c[m] = hp[m] + hz[m] * FZ[v] + hy[m] * FY[v] + hx[m] * FX[v]; g[m] = hp[m] + hz[m] * (v == 3 ? 4.0f : 12.0f); }
                customCamPos = (T3DVec3){{ c[0], c[1], c[2] }};
                customCamTarget = (T3DVec3){{ g[0], g[1], g[2] }};
                camera_mode_smooth(CAMERA_CUSTOM, 0.01f);
                if (t == 1200) { boss_apply_damage(boss, boss->maxHealth * 2.0f); phase = 4; t0 = frame; }
            }
#elif defined(HLE_DRAGON_SHOWCASE)
            // Model check: the camera orbits Shadowborne (front, 3/4, side, back, then the head
            // up close), 50 frames a view, the player standing well clear
            {
                cameraLockOnActive = false;
                if (t == 1) { character.pos[0] = boss->pos[0] - 260.0f; character.pos[2] = boss->pos[2] + 200.0f; }
                static const float A[6] = { 0.0f, 0.8f, 1.57f, 2.4f, 3.14f, 0.35f }, D[6] = { 150, 150, 160, 150, 160, 70 }, H[6] = { 50, 60, 45, 70, 60, 45 };
                int v = (t / 50) % 6;
                float a = boss->rot[1] + A[v], *bp = boss->pos;
                customCamPos = (T3DVec3){{ bp[0] + sinf(a) * D[v], bp[1] + H[v], bp[2] + cosf(a) * D[v] }};
                customCamTarget = (T3DVec3){{ bp[0] + sinf(boss->rot[1]) * (v == 5 ? 70.0f : 0.0f), bp[1] + (v == 5 ? 55.0f : 30.0f), bp[2] + cosf(boss->rot[1]) * (v == 5 ? 70.0f : 0.0f) }};
                camera_mode_smooth(CAMERA_CUSTOM, 0.01f);
                if (t == 900) { boss_apply_damage(boss, boss->maxHealth * 0.55f); debugf("[bonus] half at %d\n", frame); }
                if (t == 1800) { boss_apply_damage(boss, boss->maxHealth * 2.0f); phase = 4; t0 = frame; }
            }
#elif defined(HLE_TEST_DRAGON_AIR)
            // Claw rings (HLE_DRAGON_FORCE=2): held 45 in front of it, in the air for the first
            // 1000 frames, then on the floor; every hit is logged
            {
                float a = boss->rot[1];
                character.pos[0] = boss->pos[0] + sinf(a) * 45.0f; character.pos[2] = boss->pos[2] + cosf(a) * 45.0f;
                character.pos[1] = boss->pos[1] + (t < 1000 ? 25.0f : 0.0f);
                cameraLockOnActive = false;     // a 3/4 view from its left front, to see the legs
                customCamPos = (T3DVec3){{ boss->pos[0] + sinf(a + 0.9f) * 150.0f, boss->pos[1] + 40.0f, boss->pos[2] + cosf(a + 0.9f) * 150.0f }};
                customCamTarget = (T3DVec3){{ boss->pos[0] + sinf(a) * 25.0f, boss->pos[1] + 20.0f, boss->pos[2] + cosf(a) * 25.0f }};
                camera_mode_smooth(CAMERA_CUSTOM, 0.01f);
                if (t == 1000) debugf("[air] now on the floor\n");
                if (t == 2000) { debugf("[air] done\n"); boss_apply_damage(boss, boss->maxHealth * 2.0f); phase = 4; t0 = frame; }
            }
#elif defined(HLE_TEST_DRAGON_WHIP)
            // The spin (HLE_DRAGON_FORCE=14): stand behind it; log its yaw through each spin
            {
                float a = boss->rot[1] + T3D_PI;
                if (t == 1) { character.pos[0] = boss->pos[0] + sinf(a) * 70.0f; character.pos[2] = boss->pos[2] + cosf(a) * 70.0f; }
                if (t % 4 == 0) debugf("[whip] f%d yaw %.2f\n", frame, (double)boss->rot[1]);
            }
#elif defined(HLE_TEST_BONUS_DRAGON)
            // a long fight, circling it (every hit is logged, the player is topped up): half at 60 s, dead at 120 s
            in->stick_x = (int)(70 * cosf(t * 0.015f)); in->stick_y = (int)(70 * sinf(t * 0.015f));
            if (t == 2000) { boss_apply_damage(boss, boss->maxHealth * 0.55f); debugf("[bonus] half at %d\n", frame); }
            if (t == 4000) { boss_apply_damage(boss, boss->maxHealth * 2.0f); debugf("[bonus] killed at %d\n", frame); phase = 4; t0 = frame; }
#else
            if (t == 900) { boss_apply_damage(boss, boss->maxHealth * 2.0f); debugf("[bonus] killed at %d\n", frame); phase = 4; t0 = frame; }
#endif
        } else if (phase == 4) {
            if (!boss) { debugf("[bonus] back at the title at %d\n", frame); phase = 5; }
        }
        return;
    }
#endif
#ifdef HLE_TEST_TWO_RUNS
    // Run 1: kill the Guardian, reach phase 2, die, back to the title. Run 2: kill the
    // Guardian again; phase 2 must start again.
    {
        static int run = 1, phase = 0, t0 = 0;
        Boss *boss = boss_get_instance();
        int t = frame - t0;
        if (frame % 60 == 0) debugf("[2r] f%d run %d phase %d boss %d mode %d hp %.0f\n", frame, run, phase,
                                    boss ? boss->phaseIndex : -1, boss ? boss->mode : -1, boss ? (double)boss->health : 0.0);
        if (phase == 0) {               // menus / intro: tap A until the phase-1 fight
            if (boss && boss->mode == BOSS_MODE_COMBAT && boss->phaseIndex == 1) { phase = 1; t0 = frame; }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {
            if (t == 60) boss_apply_damage(boss, boss->maxHealth * 2.0f);
            if (t > 60 && boss->phaseIndex == 2 && boss->mode == BOSS_MODE_COMBAT) {
                debugf("[2r] run %d: phase 2 fight OK\n", run);
                phase = 2; t0 = frame;
            } else if (t > 90 && frame % 20 == 0) { pressed->a = 1; in->btn.a = 1; }
            if (t > 3000 && t % 300 == 0) debugf("[2r] run %d: phase 2 NOT started\n", run);
        } else if (phase == 2 && run == 1) {
            if (character_get_state() != CHAR_STATE_DEAD) { if (t % 10 == 0) character_apply_damage(50.0f); }
            else { run = 2; phase = 3; t0 = frame; }
        } else if (phase == 3) {        // death screen -> title -> Continue
            if (!boss_get_instance() || boss->phaseIndex != 2) { phase = 0; t0 = frame; }
            else if (frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        }
        return;
    }
#endif
#ifdef HLE_TEST_BOSS2
    // Kill the Guardian, skip the transformation, fight Guardian Unbound, kill it too
    {
        static int phase = 0, t0 = 0;
        Boss *boss = boss_get_instance();
        int t = frame - t0;
        if (frame % 60 == 0 && boss)
            debugf("[b2] f%d phase %d boss phase %d mode %d state %d hp %.0f/%.0f msa %d\n", frame, phase,
                   boss->phaseIndex, boss->mode, boss->state, (double)boss->health, (double)boss->maxHealth,
                   msa_has_active_aerial_swords());
        if (phase == 0) {               // get to the fight
            if (boss && boss->mode == BOSS_MODE_COMBAT && boss->phaseIndex == 1) { phase = 1; t0 = frame; }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (!boss) {
            return;
        } else if (phase == 1) {        // first death
            if (t == 120) { boss_apply_damage(boss, boss->maxHealth * 2.0f); debugf("[b2] killed guardian\n"); }
            if (t > 120 && boss->phaseIndex == 2 && boss->mode == BOSS_MODE_COMBAT) { phase = 2; t0 = frame; debugf("[b2] unbound fight at %d\n", frame); }
#ifndef HLE_TEST_WATCH_CUTSCENE
            else if (t > 150 && frame % 20 == 0) { pressed->a = 1; in->btn.a = 1; }    // skip the cutscene
#endif
        } else if (phase == 2) {        // fight a while, then the final death
            if (t % 45 == 0) { pressed->b = 1; in->btn.b = 1; }
#ifdef HLE_TEST_BOSS2_DIE
            if (t % 60 == 0) debugf("[b2] player hp %.0f state %d\n", (double)character_get_health(), character_get_state());
            if (character_get_state() == CHAR_STATE_DEAD) { phase = 4; t0 = frame; debugf("[b2] player died at %d\n", frame); }
#else
            if (t == 900) { boss_apply_damage(boss, boss->maxHealth * 2.0f); debugf("[b2] killed unbound\n"); phase = 3; t0 = frame; }
#endif
        } else if (phase == 3) {        // the Fallen Knight descends
            if (boss->phaseIndex == 3 && boss->mode == BOSS_MODE_COMBAT) { phase = 5; t0 = frame; debugf("[b2] fallen knight fight at %d\n", frame); }
        } else if (phase == 5) {        // fight it, then the final death
            if (t % 40 == 0) { pressed->b = 1; in->btn.b = 1; }
            if (t % 60 == 0) debugf("[b2] FK hp %.0f state %d | player hp %.0f state %d\n", (double)boss->health, boss->state,
                                    (double)character_get_health(), character_get_state());
            if (t == 1500) { boss_apply_damage(boss, boss->maxHealth * 2.0f); debugf("[b2] killed fallen knight\n"); phase = 6; t0 = frame; }
        } else if (phase == 6) {
            if (t % 120 == 0) debugf("[b2] end: mode %d state %d hp %.0f\n", boss->mode, boss->state, (double)boss->health);
        } else if (phase == 4) {        // restart from the death screen: back to the first Guardian
            if (frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            if (t % 120 == 0) debugf("[b2] after restart: boss phase %d name %s mode %d hp %.0f\n", boss->phaseIndex, boss->name, boss->mode, (double)boss->health);
        }
        return;
    }
#endif
#ifdef HLE_TEST_TOGGLE
    // Skills OFF in the pause menu, die, restart, skills ON in the pause menu, try Cleave
    {
        enum { WAIT1, MENU_OFF, DIE, WAIT2, MENU_ON, TRY, DONE };
        static int phase = WAIT1, t0 = 0, step = 0;
        Boss *boss = boss_get_instance();
        bool combat = boss && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL;
        int t = frame - t0;
        // pause menu: Resume, Restart, Settings, Controls / settings: Audio, Video, Controls, Skills, Skin, Back
        // O = open the pause menu, C = close it (both wait until it took effect)
        static const char seq[] = "ODDADDDABBC";
        switch (phase) {
        case WAIT1: case WAIT2:
            if (!combat) {
                t0 = frame;
                if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            } else if (t > 60) {
                phase++; t0 = frame; step = 0;
                debugf("[tog] fight at %d\n", frame);
            }
            break;
        case MENU_OFF: case MENU_ON:
            if (t >= 12 && step < (int)sizeof(seq) - 1) {
                char c = seq[step];
                bool open = menu_controller_is_active();
                if (c == 'O' || c == 'C') {
                    if (open == (c == 'O')) step++;
                    else { pressed->start = 1; in->btn.start = 1; }
                } else {
                    if (c == 'D') pressed->d_down = 1;
                    if (c == 'A') { pressed->a = 1; in->btn.a = 1; }
                    if (c == 'B') { pressed->b = 1; in->btn.b = 1; }
                    step++;
                }
                t0 = frame;
            }
            if (step >= (int)sizeof(seq) - 1) {
                debugf("[tog] menu done: skills %d\n", character_skills_enabled());
                phase++; t0 = frame;
            }
            break;
        case DIE:
            if (character_get_state() == CHAR_STATE_DEAD) {
                if (frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            } else if (t > 300 && !combat) {
                phase = WAIT2; t0 = frame;
                debugf("[tog] restarted at %d\n", frame);
            }
            break;
        case TRY:
            if (t == 30 || t == 120) {
                pressed->d_down = 1;
                debugf("[tog] D-down: skills %d state %d\n", character_skills_enabled(), character_get_state());
            }
            if (t == 60 || t == 150) {
                debugf("[tog] after: state %d cooldown %.1f\n", character_get_state(),
                       (double)character_get_skill_cooldown(SKILL_CRESCENT_CLEAVE));
                if (t == 150) phase = DONE;
            }
            break;
        }
        return;
    }
#endif
#ifdef HLE_TEST_SKILL_DMG
    // Damage per use (with HLE_TEST_FREEZE_BOSS): every 200 frames the Guardian is healed and
    // the player set 30 in front of it, locked on, then uses HLE_TEST_SKILL_DMG once
    // (0: the three-swing combo, 1: Moon Blade Dance, 2: Halfmoon Slash, 3: Crescent Cleave)
    {
        extern float deltaTime;
        static int start = -1;
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) start = frame;
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = (frame - start) % 200, cyc = (frame - start) / 200;
        static float hp0; static int t0;
        if (t == 0) {
            boss->health = boss->maxHealth; hp0 = boss->health;
            float a = atan2f(character.pos[0] - boss->pos[0], character.pos[2] - boss->pos[2]);
            character.pos[0] = boss->pos[0] + sinf(a) * 30.0f; character.pos[2] = boss->pos[2] + cosf(a) * 30.0f;
            character.stamina = character.maxStamina;
#ifdef HLE_TEST_LOW_STAMINA     // a sliver of stamina: the first action still goes
            character.stamina = 1.0f;
#endif
            boss->poise = 0.0f; boss->staggerT = 0.0f;
        }
        if (t == 2 && cyc == 0) pressed->z = 1;
        in->btn.z = 1;
        if (t == 10) t0 = frame;
#if HLE_TEST_SKILL_DMG == 0
        if (t == 10 || t == 28 || t == 46) { pressed->b = 1; in->btn.b = 1; }
#elif HLE_TEST_SKILL_DMG == 1
        if (t == 10) pressed->d_up = 1;
#elif HLE_TEST_SKILL_DMG == 2
        if (t == 10) pressed->d_right = 1;
#else
        if (t == 10) pressed->d_down = 1;
#endif
#ifdef HLE_SDMG_RAISE     // the boss held this high in the air (its feet above the floor): out of the wave's reach?
        { static float y0; if (cyc == 0 && t == 0) y0 = boss->pos[1]; boss->pos[1] = y0 + (float)HLE_SDMG_RAISE; }
#endif
        static int busyEnd;
        if (t == 11) busyEnd = 0;
        if (t > 11 && !busyEnd && character_get_state() == CHAR_STATE_NORMAL) busyEnd = frame;
        if (t == 195) debugf("[sdmg] skill %d: %.1f damage, busy %.2f s, stagger meter %.2f%s\n", HLE_TEST_SKILL_DMG, (double)(hp0 - boss->health),
                             (double)((busyEnd - t0) * deltaTime), (double)boss->poise, boss->staggerT > 0.0f ? " (staggered)" : "");
        return;
    }
#endif
#ifdef HLE_TEST_SKILLS
    // A (not Start, it pauses) until the fight starts, then each skill once:
    // Halfmoon, lock on, Cleave, Moon Blade Dance jumping left
    static int start = -1;
    if (start < 0) {
        Boss *boss = boss_get_instance();
        if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) {
            start = frame;
            debugf("[skill] fight starts at frame %d\n", frame);
        } else if (frame > 200 && frame % 30 == 0) {
            pressed->a = 1; in->btn.a = 1;
        }
        return;
    }
    int t = frame - start;
#ifdef HLE_TEST_FREEZE_BOSS
    if (t < 40) in->btn.c_left = 1;     // orbit the camera to a side view
#endif
    if (t == 150) pressed->d_right = 1;
    if (t == 160) { pressed->z = 1; in->btn.z = 1; }
    if (t == 360) pressed->d_down = 1;
    if (t > 610 && t < 710) in->stick_x = -80;
    if (t == 620) pressed->d_up = 1;
    if (t == 150 || t == 360 || t == 620) debugf("[skill] press at frame %d\n", frame);
    return;
#endif
    if (frame > 200 && frame < 2000 && frame % 60 == 0) {
        pressed->a = 1; in->btn.a = 1;
        pressed->start = 1; in->btn.start = 1;
    }
    if (frame > 2400 && frame < 2600) in->stick_y = 80;   // walk forward a bit
    if (frame > 2600 && frame % 45 == 0) { pressed->b = 1; in->btn.b = 1; }   // attack
}
#endif
#ifdef HLE_LOG_HITVOL
#include <t3d/t3dmodel.h>
#include <t3d/t3dskeleton.h>
#include "utilities/game_math.h"
/** Every skinned vertex (swords and the summoned swords left out) in world space, from o: the
 *  widest (horizontal) below h above o of the legs, torso, arms and head; the top; the furthest
 *  in front of yaw (facing (-sin, cos)) and to its side */
bool hle_silhouette(const T3DModel *m, const T3DSkeleton *sk, const T3DMat4FP *M, const float o[3], float yaw, float h, float out[7])
{
    if (!m || !sk || !M) return false;
    float fx = -fm_sinf(yaw), fz = fm_cosf(yaw);
    for (int i = 0; i < 7; i++) out[i] = 0.0f;
    T3DModelIter it = t3d_model_iter_create(m, T3D_CHUNK_TYPE_OBJECT);
    while (t3d_model_iter_next(&it)) {
        const T3DObject *ob = it.object;
        const char *mn = ob->material && ob->material->name ? ob->material->name : "";
        static int said;
        if ((!strcmp(mn, "sword") || !strcmp(mn, "z_sword1")) && said < 2) {     // the blade's ends, in the hand's space (the hit segments are set from these)
            said++;
            for (int p = 0; p < ob->numParts; p++) {
                const T3DObjectPart *pt = &ob->parts[p];
                int lo = 0, hi = 0; const int16_t *L = NULL, *H = NULL;
                for (int v = 0; v < pt->vertLoadCount; v++) {
                    const int16_t *q = (v & 1) ? pt->vert[v >> 1].posB : pt->vert[v >> 1].posA;
                    if (!L || q[0] < lo) { lo = q[0]; L = q; }
                    if (!H || q[0] > hi) { hi = q[0]; H = q; }
                }
                if (L) debugf("[hitvol] %s part %d bone %d: x from %d,%d,%d to %d,%d,%d\n", mn, p, pt->matrixIdx, L[0], L[1], L[2], H[0], H[1], H[2]);
            }
        }
        if (strstr(mn, "sword") || strstr(mn, "a_black")) continue;
        for (int p = 0; p < ob->numParts; p++) {
            const T3DObjectPart *pt = &ob->parts[p];
            if (pt->matrixIdx >= sk->skeletonRef->boneCount) continue;
            const char *bn = sk->skeletonRef->bones[pt->matrixIdx].name;
            int c = (strstr(bn, "Leg") || strstr(bn, "Foot")) ? 0 : (strstr(bn, "Arm") || strstr(bn, "Hand") || strstr(bn, "Finger") || strstr(bn, "Thumb")) ? 2 : strstr(bn, "Head") ? 3 : 1;
            const T3DMat4FP *B = &sk->boneMatricesFP[pt->matrixIdx];
            for (int v = 0; v < pt->vertLoadCount; v++) {
                const T3DVertPacked *vp = &pt->vert[v >> 1];
                const int16_t *q = (v & 1) ? vp->posB : vp->posA;
                float l[3] = { q[0], q[1], q[2] }, mm[3], w[3];
                mat4fp_mul_point_f32_row3_colbasis(B, l, mm);
                mat4fp_mul_point_f32_row3_colbasis(M, mm, w);
                float dx = w[0] - o[0], dz = w[2] - o[2], dy = w[1] - o[1], r = sqrtf(dx * dx + dz * dz);
                if (dy < h) out[c] = fmaxf(out[c], r);
                out[4] = fmaxf(out[4], dy);
                out[5] = fmaxf(out[5], dx * fx + dz * fz); out[6] = fmaxf(out[6], fabsf(dx * fz - dz * fx));
            }
        }
    }
    return true;
}
#endif
