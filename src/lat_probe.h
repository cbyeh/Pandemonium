// test aid: input-to-screen latency, frames matched by framebuffer (the display is FIFO)
#include <libdragon.h>
static volatile uint32_t lat_vbl;
// pacing: each frame's game step (in refreshes) against how long it is on screen
static volatile uint32_t pac_shownAt[256], pac_shownN;
static float pac_step[256]; static uint32_t pac_sub;
static float pac_lastT;
static inline void pacing_on_show(uint32_t vbl) { pac_shownAt[pac_shownN++ & 255] = vbl; }
static inline void pacing_frame(void)
{
    extern float gameTime;
    float period = 1.0f / display_get_refresh_rate();
    pac_step[pac_sub++ & 255] = (gameTime - pac_lastT) / period; pac_lastT = gameTime;
    static uint32_t chk = 2; static float err; static int n, bad;
    while (chk + 1 < pac_shownN && chk < pac_sub) {
        float gap = (float)(pac_shownAt[chk & 255] - pac_shownAt[(chk - 1) & 255]);
        float e = pac_step[chk & 255] - gap; if (e < 0) e = -e;
        err += e; if (e > 0.5f) bad++; n++; chk++;
    }
    if (n >= 120) { debugf("[pacing] step off the screen time by %.2f refreshes on average, over half a refresh on %d of %d frames\n", (double)(err / n), bad, n); err = 0; n = bad = 0; }
}

static struct { const void *buf; uint32_t at; } lat_q[8];
static volatile int lat_head, lat_tail;
static volatile uint32_t lat_sum, lat_n, lat_max;
static void lat_vblank(void)
{
    static const void *last;
    lat_vbl++;
    surface_t cur = display_get_current_framebuffer();
    if (cur.buffer == last) return;
    last = cur.buffer;
    pacing_on_show(lat_vbl);
    while (lat_head != lat_tail) {
        int h = lat_head; lat_head = (h + 1) & 7;
        if (lat_q[h].buf == cur.buffer) { uint32_t l = lat_vbl - lat_q[h].at; lat_sum += l; lat_n++; if (l > lat_max) lat_max = l; break; }
    }
}
static uint32_t lat_padAt;
static inline void lat_pad_read(void) { lat_padAt = lat_vbl; }
static inline void lat_submit(const surface_t *s)
{
    int t = lat_tail; lat_q[t].buf = s->buffer; lat_q[t].at = lat_padAt; lat_tail = (t + 1) & 7;
    if (lat_n >= 120) { debugf("[latency] %.2f refreshes from the pad read to the screen (worst %lu)\n", (double)lat_sum / lat_n, (unsigned long)lat_max); lat_sum = lat_n = lat_max = 0; }
}
