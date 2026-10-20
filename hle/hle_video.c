/**
 * FMV player for the HLE build: libdragon's H.264 decoder and YUV blitter need
 * the RSP, so the cutscene is shipped in a CPU-decodable block format
 * (see hle/tools/hbvenc.c) and drawn as a texture with rdpq.
 */
#include <libdragon.h>
#include <fmv.h>
#include <string.h>
#include <sys/stat.h>
#include "hle_video.h"

typedef struct {
    int w, h, fps, nframes;
    FILE *f;
    uint8_t *buf;
    surface_t pic;
} hbv_t;

static uint16_t mix5551(uint16_t a, uint16_t b, int wa, int wb)
{
    int r = (((a >> 11) & 31) * wa + ((b >> 11) & 31) * wb) / 3;
    int g = (((a >> 6) & 31) * wa + ((b >> 6) & 31) * wb) / 3;
    int bl = (((a >> 1) & 31) * wa + ((b >> 1) & 31) * wb) / 3;
    return (r << 11) | (g << 6) | (bl << 1) | 1;
}

/** Decodes the next frame into v->pic; returns false at the end of the stream. */
static bool hbv_next(hbv_t *v)
{
    uint32_t size;
    if (fread(&size, 4, 1, v->f) != 1 || fread(v->buf, 1, size, v->f) != size)
        return false;
    int bw = v->w / 4, nb = bw * (v->h / 4);
    const uint8_t *bitmap = v->buf;
    const uint32_t *blk = (const uint32_t*)(v->buf + (nb + 31) / 32 * 4);
    uint16_t *pix = v->pic.buffer;
    int stride = v->pic.stride / 2;
    for (int b = 0; b < nb; b++) {
        if (!(bitmap[b >> 3] & (0x80 >> (b & 7)))) continue;
        uint32_t c = blk[0], idx = blk[1];
        blk += 2;
        uint16_t pal[4] = { c >> 16, c, 0, 0 };
        pal[2] = mix5551(pal[0], pal[1], 2, 1);
        pal[3] = mix5551(pal[0], pal[1], 1, 2);
        uint16_t *dst = pix + (b / bw) * 4 * stride + (b % bw) * 4;
        for (int y = 0; y < 4; y++, dst += stride) {
            dst[0] = pal[(idx >> 30) & 3]; dst[1] = pal[(idx >> 28) & 3];
            dst[2] = pal[(idx >> 26) & 3]; dst[3] = pal[(idx >> 24) & 3];
            idx <<= 8;
        }
    }
    return true;
}

static bool hbv_open(hbv_t *v, const char *fn)
{
    memset(v, 0, sizeof(*v));
    v->f = fopen(fn, "rb");
    if (!v->f) return false;
    uint8_t hdr[16];
    if (fread(hdr, 1, 16, v->f) != 16 || memcmp(hdr, "HBV1", 4) != 0) {
        fclose(v->f);
        return false;
    }
    v->w = (hdr[4] << 8) | hdr[5];
    v->h = (hdr[6] << 8) | hdr[7];
    v->fps = (hdr[8] << 8) | hdr[9];
    v->nframes = (hdr[10] << 8) | hdr[11];
    int nb = (v->w / 4) * (v->h / 4);
    v->buf = malloc((nb + 31) / 32 * 4 + nb * 8);
    v->pic = surface_alloc(FMT_RGBA16, v->w, v->h);
    assertf(v->buf && v->pic.buffer, "HLE video: out of memory");
    memset(v->pic.buffer, 0, v->pic.stride * v->h);
    return true;
}

static void hbv_close(hbv_t *v)
{
    fclose(v->f);
    free(v->buf);
    surface_free(&v->pic);
}

static bool stop_requested;
static void ctrl_stop(fmv_control_t *ctrl) { (void)ctrl; stop_requested = true; }

void hle_fmv_play(const char *video_fn, const fmv_parms_t *parms)
{
    // Same base name as the original video, ".hbv" extension
    char fn[128];
    const char *ext = strrchr(video_fn, '.');
    int base = ext ? ext - video_fn : (int)strlen(video_fn);
    snprintf(fn, sizeof(fn), "%.*s.hbv", base, video_fn);
    hbv_t v;
    if (!hbv_open(&v, fn)) {
        debugf("[hle] video %s not found\n", fn);
        return;
    }

    snprintf(fn, sizeof(fn), "%.*s.wav64", base, video_fn);
    wav64_t *audio = NULL;
    struct stat st;
    if (!parms->disable_audio && stat(fn, &st) == 0) {
        audio = wav64_load(fn, NULL);
        mixer_ch_play(parms->audio_mixer_channel, &audio->wave);
    } else {
        display_set_fps_limit(v.fps);
    }

    fmv_control_t ctrl = { .stop = ctrl_stop };
    stop_requested = false;
    int frame_idx = 0;
    bool more = true;
    while (more && !stop_requested) {
        mixer_try_play();
        // Decode up to the frame the audio clock is at (frames depend on each other)
        int target = frame_idx + 1;
        if (audio && mixer_ch_playing(parms->audio_mixer_channel))
            target = (int)(mixer_ch_get_pos(parms->audio_mixer_channel) * (int64_t)v.fps / audio->wave.frequency) + 1;
        while (frame_idx < target && (more = hbv_next(&v)))
            frame_idx++;
        if (!more) break;

        surface_t *disp = display_get();
        rdpq_attach(disp, NULL);
        rdpq_set_mode_standard();
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_tex_blit(&v.pic, 0, 0, &(rdpq_blitparms_t){
            .scale_x = (float)display_get_width() / v.w,
            .scale_y = (float)display_get_height() / v.h,
        });
        if (parms->osd_callback)
            parms->osd_callback(parms->osd_ctx, frame_idx, (float)frame_idx / v.fps, &ctrl);
        rdpq_detach_show();
    }

    rspq_wait();
    if (audio) {
        mixer_ch_stop(parms->audio_mixer_channel);
        wav64_close(audio);
    }
    display_set_fps_limit(0);
    hbv_close(&v);
}
