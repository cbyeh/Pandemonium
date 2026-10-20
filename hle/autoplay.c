/**
 * Test-only input script (build with HLE_EXTRA_CFLAGS=-DHLE_AUTOPLAY): taps A to
 * walk through the menus so automated emulator runs reach gameplay.
 */
#ifdef HLE_AUTOPLAY
#include <libdragon.h>

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
#ifdef HLE_TEST_SKILLS
    // A only (Start would pause), then each skill once: Halfmoon, lock on, Cleave, Moon Blade Dance
    if (frame > 200 && frame < 900 && frame % 60 == 0) { pressed->a = 1; in->btn.a = 1; }
    if (frame == 990) pressed->d_right = 1;
    if (frame == 1000) { pressed->z = 1; in->btn.z = 1; }
    if (frame == 1200) pressed->d_down = 1;
    if (frame > 1450 && frame < 1550) in->stick_x = -80;
    if (frame == 1460) pressed->d_up = 1;
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
