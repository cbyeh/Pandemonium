/*
 * hbvenc: encodes raw RGB24 frames (stdin, e.g. from ffmpeg) into the HLE build's
 * block video format, which the N64 CPU can decode in real time:
 *
 *   header: "HBV1" u16 width, u16 height, u16 fps, u16 frames, u32 0   (big endian)
 *   frame:  u32 payload size, then the payload:
 *             bitmap of coded 4x4 blocks (1 bit each, MSB first, padded to 4 bytes)
 *             per coded block: u16 c0, u16 c1 (RGBA5551), u32 2-bit indices
 *             (pixel 0 in the top bits); palette = c0, c1, (2c0+c1)/3, (c0+2c1)/3
 *   Uncoded blocks keep the previous frame's pixels.
 *
 * usage: ffmpeg -i in.mp4 -vf scale=W:H,fps=F -f rawvideo -pix_fmt rgb24 - | hbvenc W H F out.hbv
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// Max average squared error per pixel channel (8-bit scale) to keep a block.
#define SKIP_THRESHOLD 48

static void put16(FILE *f, unsigned v) { fputc(v >> 8, f); fputc(v, f); }
static void put32(FILE *f, unsigned v) { put16(f, v >> 16); put16(f, v); }

static uint16_t to5551(const int *c)
{
    int r = (c[0] * 31 + 127) / 255, g = (c[1] * 31 + 127) / 255, b = (c[2] * 31 + 127) / 255;
    return (r << 11) | (g << 6) | (b << 1) | 1;
}

static void from5551(uint16_t v, int *c)
{
    c[0] = ((v >> 11) & 31) * 255 / 31;
    c[1] = ((v >> 6) & 31) * 255 / 31;
    c[2] = ((v >> 1) & 31) * 255 / 31;
}

/** Same rounding as the decoder. */
static uint16_t mix5551(uint16_t a, uint16_t b, int wa, int wb)
{
    int r = (((a >> 11) & 31) * wa + ((b >> 11) & 31) * wb) / 3;
    int g = (((a >> 6) & 31) * wa + ((b >> 6) & 31) * wb) / 3;
    int bl = (((a >> 1) & 31) * wa + ((b >> 1) & 31) * wb) / 3;
    return (r << 11) | (g << 6) | (bl << 1) | 1;
}

static int sqerr(const int *a, const int *b)
{
    int d0 = a[0]-b[0], d1 = a[1]-b[1], d2 = a[2]-b[2];
    return d0*d0 + d1*d1 + d2*d2;
}

int main(int argc, char **argv)
{
    if (argc != 5) { fprintf(stderr, "usage: hbvenc W H FPS out.hbv < rgb24\n"); return 1; }
    int w = atoi(argv[1]), h = atoi(argv[2]), fps = atoi(argv[3]);
    if (w <= 0 || h <= 0 || w % 4 || h % 4 || fps <= 0) { fprintf(stderr, "hbvenc: bad size\n"); return 1; }
    FILE *out = fopen(argv[4], "wb");
    if (!out) { perror(argv[4]); return 1; }

    int bw = w / 4, bh = h / 4, nb = bw * bh;
    int bitmap_bytes = (nb + 31) / 32 * 4;
    uint8_t *rgb = malloc(w * h * 3);
    uint16_t *prev = calloc(w * h, 2);          // decoder-side picture
    uint8_t *bitmap = malloc(bitmap_bytes);
    uint8_t *blocks = malloc(nb * 8);

    fwrite("HBV1", 1, 4, out);
    put16(out, w); put16(out, h); put16(out, fps);
    long nframes_pos = ftell(out);
    put16(out, 0); put32(out, 0);

    int nframes = 0;
    long total = 0;
    while (fread(rgb, 1, w * h * 3, stdin) == (size_t)(w * h * 3)) {
        memset(bitmap, 0, bitmap_bytes);
        int ncoded = 0;
        for (int b = 0; b < nb; b++) {
            int x0 = (b % bw) * 4, y0 = (b / bw) * 4;
            int px[16][3];
            int keep_err = 0;
            for (int i = 0; i < 16; i++) {
                const uint8_t *p = rgb + ((y0 + i/4) * w + x0 + i%4) * 3;
                px[i][0] = p[0]; px[i][1] = p[1]; px[i][2] = p[2];
                int c[3]; from5551(prev[(y0 + i/4) * w + x0 + i%4], c);
                keep_err += sqerr(px[i], c);
            }
            if (nframes > 0 && keep_err <= SKIP_THRESHOLD * 16 * 3)
                continue;

            // Endpoints: darkest and brightest pixel
            int lo = 0, hi = 0;
            for (int i = 1; i < 16; i++) {
                int l = px[i][0]*3 + px[i][1]*6 + px[i][2];
                if (l < px[lo][0]*3 + px[lo][1]*6 + px[lo][2]) lo = i;
                if (l > px[hi][0]*3 + px[hi][1]*6 + px[hi][2]) hi = i;
            }
            uint16_t pal[4] = { to5551(px[hi]), to5551(px[lo]) };
            pal[2] = mix5551(pal[0], pal[1], 2, 1);
            pal[3] = mix5551(pal[0], pal[1], 1, 2);
            int palc[4][3];
            for (int k = 0; k < 4; k++) from5551(pal[k], palc[k]);

            uint32_t idx = 0;
            for (int i = 0; i < 16; i++) {
                int best = 0, be = sqerr(px[i], palc[0]);
                for (int k = 1; k < 4; k++) {
                    int e = sqerr(px[i], palc[k]);
                    if (e < be) { be = e; best = k; }
                }
                idx |= (uint32_t)best << (30 - 2*i);
                prev[(y0 + i/4) * w + x0 + i%4] = pal[best];
            }
            bitmap[b >> 3] |= 0x80 >> (b & 7);
            uint8_t *o = blocks + ncoded * 8;
            o[0] = pal[0] >> 8; o[1] = pal[0]; o[2] = pal[1] >> 8; o[3] = pal[1];
            o[4] = idx >> 24; o[5] = idx >> 16; o[6] = idx >> 8; o[7] = idx;
            ncoded++;
        }
        put32(out, bitmap_bytes + ncoded * 8);
        fwrite(bitmap, 1, bitmap_bytes, out);
        fwrite(blocks, 1, ncoded * 8, out);
        total += 4 + bitmap_bytes + ncoded * 8;
        nframes++;
    }
    fseek(out, nframes_pos, SEEK_SET);
    put16(out, nframes);
    fclose(out);
    fprintf(stderr, "hbvenc: %d frames, %ld KB\n", nframes, total / 1024);
    return nframes ? 0 : 1;
}
