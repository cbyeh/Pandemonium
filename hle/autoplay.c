/**
 * Test-only input script (build with HLE_EXTRA_CFLAGS=-DHLE_AUTOPLAY): taps A to
 * walk through the menus so automated emulator runs reach gameplay.
 */
#ifdef HLE_AUTOPLAY
#include <libdragon.h>
#if defined(HLE_TEST_SKILLS) || defined(HLE_TEST_TOGGLE) || defined(HLE_TEST_BOSS2) || defined(HLE_TEST_TWO_RUNS) || defined(HLE_TEST_BONUS)
#include "character.h"
#include "game/boss/boss.h"
#include "controllers/menu_controller.h"
#include "game/boss/environmental_mechanics/multi_sword_attacks.h"
#endif

void hle_autoplay(joypad_inputs_t *in, joypad_buttons_t *pressed)
{
    static int frame;
    frame++;
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
#ifdef HLE_TEST_BONUS
    // Title -> Bonus -> Fallen Knight; watch the intro, stand still (it leaps at range),
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
        } else if (phase == 1) {        // Down, Down (Bonus), A, A (Fallen Knight)
            static const char seq[] = "DDAA";
            if (t >= 20 && step < 4) {
                if (seq[step] == 'D') pressed->d_down = 1; else { pressed->a = 1; in->btn.a = 1; }
                step++; t0 = frame;
            }
            if (step == 4 && boss) { phase = 2; t0 = frame; debugf("[bonus] scene at %d\n", frame); }
        } else if (phase == 2) {
            if (boss && boss->phaseIndex == 3 && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) {
                phase = 3; t0 = frame; debugf("[bonus] fight at %d\n", frame);
            }
        } else if (phase == 3) {
#ifdef HLE_TEST_FK_LEAP
            in->stick_x = 70;       // circle it (locked on) for a side view
#endif
            if (t == 900) { boss_apply_damage(boss, boss->maxHealth * 2.0f); debugf("[bonus] killed at %d\n", frame); phase = 4; t0 = frame; }
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
        // pause menu: Resume, Restart, Settings, Controls / settings: Audio, Video, Controls, Skills, Back
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
