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
        if (frame % 120 == 0) {
            debugf("[hle] perf: %lu frames in %lu ms, rsp-hle %lu%% of CPU time\n", 120UL,
                (unsigned long)TICKS_TO_MS(now - start), (unsigned long)(__hle_perf_ticks * 100ULL / (now - start)));
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
    if (frame > 200 && frame < 2000 && frame % 60 == 0) {
        pressed->a = 1; in->btn.a = 1;
        pressed->start = 1; in->btn.start = 1;
    }
    if (frame > 2400 && frame < 2600) in->stick_y = 80;   // walk forward a bit
    if (frame > 2600 && frame % 45 == 0) { pressed->b = 1; in->btn.b = 1; }   // attack
}
#endif
