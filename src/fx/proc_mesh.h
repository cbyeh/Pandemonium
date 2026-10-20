#ifndef PROC_MESH_H
#define PROC_MESH_H

#include <stdbool.h>
#include <stdint.h>
#include <libdragon.h>
#include <t3d/t3d.h>

// Procedural low-poly props: flat-shaded triangles with the light baked into vertex
// colours, recorded once into an rspq block. Coordinates are world units, relative
// to the matrix the mesh is drawn with.

typedef struct {
    rspq_block_t *block;
    T3DVertPacked *verts;
    T3DMat4FP *mat;
} ProcMesh;

void proc_mesh_begin(void);
// Shaded by how the face turns to the light; `unlit` keeps the colour as is (glows).
void proc_mesh_tri(const float a[3], const float b[3], const float c[3], uint32_t rgba, bool unlit);
void proc_mesh_quad(const float a[3], const float b[3], const float c[3], const float d[3], uint32_t rgba, bool unlit);
void proc_mesh_box(const float center[3], const float half[3], uint32_t rgba);
// Cone from a ring around `base` (perpendicular to base->tip) to `tip`.
void proc_mesh_spike(const float base[3], const float tip[3], float radius, int sides, uint32_t rgba);
// Upright prism/frustum: radius r0 at base[1], r1 at base[1] + h.
void proc_mesh_frustum(const float base[3], float r0, float r1, float h, int sides, uint32_t rgba);
void proc_mesh_end(ProcMesh *out);

void proc_mesh_set_transform(ProcMesh *m, const float scale, const float rot[3], const float pos[3]);
// Draws at the current matrix-stack slot (call between t3d_matrix_push_pos/pop).
void proc_mesh_draw(const ProcMesh *m);
void proc_mesh_free(ProcMesh *m);

static inline uint32_t proc_rgb(int r, int g, int b) { return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFF; }

#endif
