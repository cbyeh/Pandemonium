// test aid: every frame longer than 40 ms, with what the scene was doing
#include <libdragon.h>
static inline void hitch_frame(int frameNo)
{
    static uint32_t last;
    uint32_t now = TICKS_READ();
    if (last && now - last > TICKS_FROM_MS(40)) debugf("[hitch] %lu ms at frame %d\n", (unsigned long)TICKS_TO_MS(now - last), frameNo);
    last = now;
}
