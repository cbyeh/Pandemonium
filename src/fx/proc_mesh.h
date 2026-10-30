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
    int16_t *idx;
    T3DMat4FP *mat;
    float bc[3], br;    // bounding sphere, in the stored 1/8 units (for culling to the view)
} ProcMesh;

void proc_mesh_begin(void);
// UVs for what follows (pixels; 0, 0: none): `around` across a whole tube, spike or sphere,
// `along` per unit along it (and per unit across a flat face, mapped on its two flattest axes).
// Unlit faces (glows, crystals) all sample texel (2, 2): a texture keeps its corner bright for them
void proc_mesh_uv(float around, float along);
// Shifts the UVs of the smooth faces that follow (pixels): copies of a mesh that don't repeat one another
void proc_mesh_uv_shift(float du, float dv);
// Loft quads of this colour sample the flat texel (untextured: a mouth's flesh in a scaled head)
void proc_mesh_uv_flat_color(uint32_t rgba);
// Shaded by how the face turns to the light; `unlit` keeps the colour as is (glows).
void proc_mesh_tri(const float a[3], const float b[3], const float c[3], uint32_t rgba, bool unlit);
void proc_mesh_quad(const float a[3], const float b[3], const float c[3], const float d[3], uint32_t rgba, bool unlit);
// Smooth: each corner shaded from its own normal
void proc_mesh_tri_smooth(const float a[3], const float b[3], const float c[3],
                          const float na[3], const float nb[3], const float nc[3], uint32_t rgba);
// A smooth tube through `rings` rings of `sides` points (pts[ring][side][xyz]); `cols` gives
// each quad's colour (ring-major, NULL: all `rgba`); `capEnd` closes the last ring with a fan.
void proc_mesh_loft(const float *pts, int rings, int sides, const uint32_t *cols, uint32_t rgba, bool capEnd);
// The same, a colour per point (ring-major): smooth gradients, the corners still shared
void proc_mesh_loft_vcol(const float *pts, int rings, int sides, const uint32_t *vcols, bool capEnd);
void proc_mesh_box(const float center[3], const float half[3], uint32_t rgba);
// Cone from a ring around `base` (perpendicular to base->tip) to `tip`.
void proc_mesh_spike(const float base[3], const float tip[3], float radius, int sides, uint32_t rgba);
// Upright prism/frustum: radius r0 at base[1], r1 at base[1] + h.
void proc_mesh_frustum(const float base[3], float r0, float r1, float h, int sides, uint32_t rgba);
// A molten sun with a fiery iris and slit pupil looking down -y, or +z with
// `facingZ`. `hot` shifts it to crimson and white.
void proc_mesh_eye_sphere(float radius, bool facingZ, bool hot);
// The ball alone (the eye sphere's)
void proc_mesh_sun_ball(float radius, bool facingZ, bool hot);
// The Shackled Sun's human face, looking down +z. Its parts' frames: proc_mesh_face_frame (axes
// x, y, z: +z out of the ball at (yaw, pitch)); the right side's (+x), mirrored in x for the left
#define SUN_FACE_EYE_YAW     0.34f
#define SUN_FACE_EYE_PITCH   0.1f
#define SUN_FACE_EYE_W       0.24f     // an eye's half-width and half-height (tangent units)
#define SUN_FACE_EYE_H       0.12f
#define SUN_FACE_IRIS_R      0.105f
#define SUN_FACE_PUPIL_R     0.042f
#define SUN_FACE_MOUTH_PITCH -0.37f
#define SUN_FACE_MOUTH_W     0.2f
#define SUN_FACE_CHEEK_YAW   0.46f
#define SUN_FACE_CHEEK_PITCH -0.19f
void proc_mesh_face_frame(float yaw, float pitch, float F[3][3]);
// The ball with what never moves: the eyes' whites, the nose, the cheeks' spirals
void proc_mesh_sun_face(float radius, bool hot);
// Both cheeks' blush (a glow: soft edges)
void proc_mesh_face_blush(float radius, bool hot);
// In an eye's frame: its iris (and glint), its round pupil, its upper lid shut (a glow; mirror it
// in y for the lower), its brow
void proc_mesh_face_iris(float radius, bool hot);
void proc_mesh_face_pupil(float radius);
void proc_mesh_face_lid(float radius, bool hot);
void proc_mesh_face_brow(float radius);
// In the mouth's frame: the right half of the upper or lower lip, the mouth's inside behind it (and the teeth)
void proc_mesh_face_lip(float radius, bool upper, bool hot);
// Rays round a disc facing +z: straight two-tone spikes and a ring, or wavy flames between them
void proc_mesh_sun_rays(float radius, bool wavy, bool hot);
void proc_mesh_end(ProcMesh *out);

void proc_mesh_set_transform(ProcMesh *m, const float scale, const float rot[3], const float pos[3]);
// Draws at the current matrix-stack slot (call between t3d_matrix_push_pos/pop).
void proc_mesh_draw(const ProcMesh *m);
// Glow (unlit triangles): blended over what is behind by brightness, no depth write.
// Never additive: the real RDP wraps an overflowing sum instead of clamping.
void proc_mesh_draw_glow(const ProcMesh *m);
// Either, with the vertex colours multiplied by tint
void proc_mesh_draw_tinted(const ProcMesh *m, bool glow, color_t tint);
// Opaque, the vertex colours multiplied by `tex` (repeating, filtered) and tint
void proc_mesh_draw_textured(const ProcMesh *m, sprite_t *tex, color_t tint);
// The texture and combiner for textured proc meshes drawn by hand (T3D_FLAG_TEXTURED)
void proc_mesh_tex_begin(sprite_t *tex);
// Several drawn under one mode (as proc_mesh_draw_tinted / _textured), each with its own matrix
void proc_mesh_batch_begin(bool glow, color_t tint, sprite_t *tex);
void proc_mesh_batch_draw(const ProcMesh *m);
void proc_mesh_batch_end(void);
void proc_mesh_free(ProcMesh *m);

static inline uint32_t proc_rgb(int r, int g, int b) { return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFF; }

#endif
