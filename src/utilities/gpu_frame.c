#include <string.h>
#include "gpu_frame.h"

#define GPU_ARENA_BYTES (48 * 1024)     // a fight peaks near 5 KB; the Unbound's 16 swords can draw 2 KB ribbons each

static uint8_t *s_arena[GPU_FRAMES];
static int s_half, s_used;

void gpu_frame_begin(int half)
{
#ifdef HLE_LOG_FPS   // test aid: the most a frame used
    static int peak, n;
    if (s_used > peak) peak = s_used;
    if (++n == 120) { heap_stats_t h; sys_get_heap_stats(&h); debugf("[arena] peak %d bytes, heap %d KB free of %d\n", peak, (h.total - h.used) / 1024, h.total / 1024); peak = 0; n = 0; }
#endif
    s_half = half;
    s_used = 0;
}

void *gpu_alloc(int bytes)
{
    if (!s_arena[0])
        for (int i = 0; i < GPU_FRAMES; i++) s_arena[i] = malloc_uncached(GPU_ARENA_BYTES);
    bytes = (bytes + 15) & ~15;
    if (s_used + bytes > GPU_ARENA_BYTES) {     // shortcut: wraps (a glitch, not a crash); outside the main loop only, it never fills in a fight
        debugf("[gpu_frame] arena full (%d + %d bytes)\n", s_used, bytes);
        s_used = 0;
    }
    void *p = s_arena[s_half] + s_used;
    s_used += bytes;
    return p;
}

void *gpu_copy(const void *src, int bytes)
{
    void *c = gpu_alloc(bytes);
    memcpy(c, src, bytes);
    return c;
}

T3DMat4FP *gpu_skeleton_begin(T3DSkeleton *skel)
{
    T3DMat4FP *saved = skel->boneMatricesFP;
    int bytes = sizeof(T3DMat4FP) * skel->skeletonRef->boneCount;
    T3DMat4FP *c = gpu_alloc(bytes);
    memcpy(c, saved, bytes);
    skel->boneMatricesFP = c;
    t3d_segment_set(T3D_SEGMENT_SKELETON, c);
    return saved;
}

void gpu_skeleton_end(T3DSkeleton *skel, T3DMat4FP *saved)
{
    skel->boneMatricesFP = saved;
}
