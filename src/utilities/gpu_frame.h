#ifndef GPU_FRAME_H
#define GPU_FRAME_H

#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dskeleton.h>
#include <t3d/t3dmodel.h>

// The frame's draw is recorded into a queue and run as a whole (main.c), so the RSP draws one
// frame while the CPU updates the next. While it records, blocks can't be recorded nor the RSP
// waited on: these step outside the queue for a moment (and are the plain calls otherwise).
// Whatever the RSP reads that the CPU rewrites each frame needs GPU_FRAMES copies.
#define GPU_FRAMES 2

extern rspq_queue_t *g_drawQueue;   // being recorded, or NULL
extern const T3DFrustum *g_viewFrustum;     // the frame's view (set by the scene that draws), or NULL

static inline void gpu_block_begin(void) { if (g_drawQueue) rspq_queue_switch(NULL); rspq_block_begin(); }
static inline rspq_block_t *gpu_block_end(void)
{
    rspq_block_t *b = rspq_block_end();
    if (g_drawQueue) rspq_queue_switch(g_drawQueue);
    return b;
}
// The RSP done with everything so far, this frame's draw too: what it has recorded is run first
// (a block freed after this may have been called already this frame)
static inline void gpu_wait(void)
{
    if (g_drawQueue) { rspq_queue_switch(NULL); rspq_queue_run(g_drawQueue); }
    rspq_wait();
    if (g_drawQueue) rspq_queue_switch(g_drawQueue);
}


// Per-frame GPU memory: two halves, the CPU writing one while the RSP may still read the other.
// main.c calls gpu_frame_begin(half) once the RSP is done with that half's frame
void gpu_frame_begin(int half);
void *gpu_alloc(int bytes);
#if defined(HLE_PROF_SERIAL) || defined(HLE_PROF_DRAGON)
// Test aid: what is drawn between the two recorded, then run alone: its RSP+RDP time (ticks)
void gpu_prof_begin(void);
uint32_t gpu_prof_end(void);
#endif
// A copy for this frame's draw of what the CPU rewrites each frame
void *gpu_copy(const void *src, int bytes);
static inline const T3DMat4FP *gpu_mat(const T3DMat4FP *m) { return gpu_copy(m, sizeof(T3DMat4FP)); }
// Draws the skeleton with this frame's copy of its bone matrices (the skeleton segment set to
// it, and boneMatricesFP pointing at it until gpu_skeleton_end). Blocks recorded with
// gpu_skeleton_matrices() follow it
T3DMat4FP *gpu_skeleton_begin(T3DSkeleton *skel);
void gpu_skeleton_end(T3DSkeleton *skel, T3DMat4FP *saved);
#ifdef HLE_LOG_FPS
void gpu_log_model(const char *name, const T3DModel *m);   // test aid: objects, parts, triangles, vertices
#else
static inline void gpu_log_model(const char *name, const T3DModel *m) { (void)name; (void)m; }
#endif
static inline const T3DMat4FP *gpu_skeleton_matrices(void) { return (const T3DMat4FP *)t3d_segment_placeholder(T3D_SEGMENT_SKELETON); }

#endif
