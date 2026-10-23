#include <string.h>
#include "gpu_frame.h"

#define GPU_ARENA_BYTES (48 * 1024)     // a fight peaks near 5 KB; the Unbound's 16 swords can draw 2 KB ribbons each

const T3DFrustum *g_viewFrustum;
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

#ifdef HLE_LOG_FPS
void gpu_log_model(const char *name, const T3DModel *m)
{
    int objs = 0, parts = 0, tris = 0, verts = 0, mats = 0;
    const T3DMaterial *last = NULL;
    T3DModelIter it = t3d_model_iter_create(m, T3D_CHUNK_TYPE_OBJECT);
    while (t3d_model_iter_next(&it)) {
        const T3DObject *o = it.object;
        objs++; parts += o->numParts; tris += o->triCount;
        for (int i = 0; i < o->numParts; i++) verts += o->parts[i].vertLoadCount;
        if (o->material != last) { mats++; last = o->material; }
    }
    debugf("[model] %s: %d objects, %d parts, %d triangles, %d vertices, %d material changes\n", name, objs, parts, tris, verts, mats);
}
#endif

#if defined(HLE_PROF_SERIAL) || defined(HLE_PROF_DRAGON)
static rspq_queue_t *s_profQ;
void gpu_prof_begin(void)
{
    if (!s_profQ) s_profQ = rspq_queue_create();
    rspq_queue_clear(s_profQ);
    g_drawQueue = s_profQ;
    rspq_queue_switch(s_profQ);
}
uint32_t gpu_prof_split(void)
{
    if (g_drawQueue != s_profQ || !s_profQ) return 0;    // no stage being timed (a cutscene)
    uint32_t t = gpu_prof_end();
    gpu_prof_begin();
    return t;
}
uint32_t gpu_prof_end(void)
{
    rspq_queue_switch(NULL);
    g_drawQueue = NULL;
    rspq_wait();
    uint32_t t = TICKS_READ();
    rspq_queue_run(s_profQ);
    rspq_wait();
    return TICKS_READ() - t;
}
#endif

