/**
 * Test-only input script (build with HLE_EXTRA_CFLAGS=-DHLE_AUTOPLAY): taps A to
 * walk through the menus so automated emulator runs reach gameplay.
 */
#ifdef HLE_AUTOPLAY
#include <libdragon.h>
#include <string.h>
#if defined(HLE_TEST_SKILLS) || defined(HLE_TEST_TOGGLE) || defined(HLE_TEST_BOSS2) || defined(HLE_TEST_TWO_RUNS) || defined(HLE_TEST_BONUS) || defined(HLE_TEST_ENDING) || defined(HLE_TEST_HEAVY) || defined(HLE_TEST_DUMMY) || defined(HLE_TEST_COVER) || defined(HLE_TEST_ROLLCANCEL) || defined(HLE_TEST_SKIN) || defined(HLE_TEST_UNBOUND)
#include "camera_controller.h"
#include "character_skin.h"
#include <math.h>
#include "character.h"
#include "game/boss/boss.h"
#include "controllers/menu_controller.h"
#include "game/boss/environmental_mechanics/multi_sword_attacks.h"
#endif

int hle_task_log;
float hle_dummy_damage;
int hle_dummy_hits;

void hle_autoplay(joypad_inputs_t *in, joypad_buttons_t *pressed)
{
    static int frame;
    frame++;
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
#ifdef HLE_TEST_VIDEO
    extern void video_player_request(const char *rom_video_path);
    if (frame == 30) video_player_request("rom:/video.h264");
    return;
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
#ifdef HLE_TEST_SKIN
    // Skin check: HLE_TEST_SKIN=<id> (HLE_SKIN_HOOD=0 for hood down). Title, then the knight
    // runs a circle and stands while the camera orbits: back, side, front every 60 frames.
    {
        static int start = -1;
#ifndef HLE_SKIN_HOOD
#define HLE_SKIN_HOOD 1
#endif
        if (frame == 100 || start == -1) { character_skin_set_hood_up(HLE_SKIN_HOOD); character_skin_set(HLE_TEST_SKIN); }
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
#ifdef HLE_SKIN_ACTION
        // run, a combo, two rolls; the camera steps round every 20 frames
        const int period = 20; const float dist = 50.0f;
        if (t < 120) { in->stick_x = (int)(75 * cosf(t * 0.05f)); in->stick_y = (int)(75 * sinf(t * 0.05f)); }
        if (t >= 120 && t < 260 && t % 30 == 0) { pressed->b = 1; in->btn.b = 1; }
        if (t == 280 || t == 330) { in->stick_y = 80; pressed->a = 1; in->btn.a = 1; }
#else
        const int period = 60; const float dist = 70.0f;
        if (t < 180) { in->stick_x = (int)(75 * cosf(t * 0.05f)); in->stick_y = (int)(75 * sinf(t * 0.05f)); }
#endif
        float yaw = character.rot[1];
        float a = -yaw + (float)(t / period % 3) * 1.5708f;   // facing direction rotated by 0/90/180
        float *p = character.pos;
        customCamPos = (T3DVec3){{ p[0] + dist * sinf(a), p[1] + 30.0f, p[2] + dist * cosf(a) }};
        customCamTarget = (T3DVec3){{ p[0], p[1] + 20.0f, p[2] }};
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
                if ((f == 0 && bss != 0) || (f == 1 && bss != 1)) fails++;
                seen |= 1 << bss;
                tower_next_floor();
            }
            if (seen != 0x1F) fails++;              // all five, once each
            if (tower_boss_health_scale(1) < 1.29f || tower_boss_health_scale(1) > 1.31f) fails++;   // circle 2
        }
        tower_begin_run(7);
        if (tower_mod_max_potions() != 3) fails++;
        if (tower_boss_health_scale(1) != 1.0f || tower_boss_health_scale(5) != 0.5f) fails++;   // no skills unlocked
        if (tower_mod_skill_allowed(0)) fails++;
        tower_end_run();
        if (tower_mod_max_potions() != 7 || !tower_mod_skill_allowed(0) || tower_boss_health_scale(3) != 1.0f) fails++;
        debugf("[tower] self-check: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    }
    return;
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
            else if (t > 90 && frame % 20 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 2) {
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
    // 3 Fallen Warrior, 4 Shackled Sun, 5 Shadowborn.
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
            const char *seq = HLE_DUMMY_BOSS == 3 ? "DDAA" : HLE_DUMMY_BOSS == 4 ? "DDADA" : HLE_DUMMY_BOSS == 5 ? "DDADDA" : "A";
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
                    phase = 4; tm = 0.0f; hle_dummy_damage = 0.0f; hle_dummy_hits = 0; debugf("[dummy] start\n"); }
            } else if (t > 60 && frame % 20 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 3) {    // Unbound: skip the transformation
            if (boss->phaseIndex == 2 && boss->mode == BOSS_MODE_COMBAT) { phase = 4; tm = 0.0f; hle_dummy_damage = 0.0f; hle_dummy_hits = 0; debugf("[dummy] start\n"); }
            else if (frame % 20 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 4) {
            float prev = tm;
            tm += deltaTime;
            if ((int)(tm / 30.0f) != (int)(prev / 30.0f))
                debugf("[dummy] boss %d t %.0f s damage %.0f hits %d\n", HLE_DUMMY_BOSS, (double)tm, (double)hle_dummy_damage, hle_dummy_hits);
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
        if (frame % 60 == 0) debugf("[bonus] f%d phase %d boss %d mode %d state %d hp %.0f\n", frame, phase,
                                    boss ? boss->phaseIndex : -1, boss ? boss->mode : -1, boss ? boss->state : -1,
                                    boss ? (double)boss->health : 0.0);
        if (phase == 0) {               // tap A until the title menu opens
            if (menu_controller_is_active()) { phase = 1; t0 = frame; }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {        // Down, Down (Bonus), A, A (Fallen Warrior)
#if defined(HLE_TEST_BONUS_SUN)
            static const char seq[] = "DDADA";      // Bonus > The Shackled Sun
#elif defined(HLE_TEST_BONUS_DRAGON)
            static const char seq[] = "DDADDA";     // Bonus > Shadowborn
#else
            static const char seq[] = "DDAA";       // Bonus > Fallen Warrior
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
#ifdef HLE_TEST_BONUS_DRAGON
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
        // pause menu: Resume, Restart, Settings, Controls / settings: Audio, Video, Controls, Skills, Skin, Hood, Back
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
