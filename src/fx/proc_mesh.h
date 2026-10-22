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
// Smooth: each corner shaded from its own normal
void proc_mesh_tri_smooth(const float a[3], const float b[3], const float c[3],
                          const float na[3], const float nb[3], const float nc[3], uint32_t rgba);
// A smooth tube through `rings` rings of `sides` points (pts[ring][side][xyz]); `cols` gives
// each quad's colour (ring-major, NULL: all `rgba`); `capEnd` closes the last ring with a fan.
void proc_mesh_loft(const float *pts, int rings, int sides, const uint32_t *cols, uint32_t rgba, bool capEnd);
void proc_mesh_box(const float center[3], const float half[3], uint32_t rgba);
// Cone from a ring around `base` (perpendicular to base->tip) to `tip`.
void proc_mesh_spike(const float base[3], const float tip[3], float radius, int sides, uint32_t rgba);
// Upright prism/frustum: radius r0 at base[1], r1 at base[1] + h.
void proc_mesh_frustum(const float base[3], float r0, float r1, float h, int sides, uint32_t rgba);
// A molten sun with a fiery iris and slit pupil looking down -y, or +z with
// `facingZ`. `hot` shifts it to crimson and white.
void proc_mesh_eye_sphere(float radius, bool facingZ, bool hot);
// Flame tongues round a disc of `radius` facing +z, up to `reach` long (see
// proc_mesh_draw_glow: their dark tips fade out).
void proc_mesh_corona(float radius, int tongues, float reach, int seed, bool hot);
// One chain link: an oval ring in the xy plane, `len` long along y.
void proc_mesh_chain_link(float len, float width, float thick, uint32_t rgba, bool unlit);
void proc_mesh_end(ProcMesh *out);

void proc_mesh_set_transform(ProcMesh *m, const float scale, const float rot[3], const float pos[3]);
// Draws at the current matrix-stack slot (call between t3d_matrix_push_pos/pop).
void proc_mesh_draw(const ProcMesh *m);
// Glow (unlit triangles): blended over what is behind by brightness, no depth write.
// Never additive: the real RDP wraps an overflowing sum instead of clamping.
void proc_mesh_draw_glow(const ProcMesh *m);
// Either, with the vertex colours multiplied by tint
void proc_mesh_draw_tinted(const ProcMesh *m, bool glow, color_t tint);
void proc_mesh_free(ProcMesh *m);

static inline uint32_t proc_rgb(int r, int g, int b) { return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFF; }

#endif
