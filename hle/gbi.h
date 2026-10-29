/** Minimal F3DEX2 display-list encoding, only what the HLE backend emits. */
#ifndef HLE_GBI_H
#define HLE_GBI_H

#include <stdint.h>

#define G_VTX           0x01
#define G_TRI1          0x05
#define G_TRI2          0x06
#define G_TEXTURE       0xD7
#define G_POPMTX        0xD8
#define G_GEOMETRYMODE  0xD9
#define G_MTX           0xDA
#define G_MOVEWORD      0xDB
#define G_MOVEMEM       0xDC
#define G_DL            0xDE
#define G_ENDDL         0xDF
#define G_RDPHALF_1     0xE1
#define G_RDPHALF_2     0xF1
#define G_RDPFULLSYNC   0xE9

// Geometry mode bits
#define G_ZBUFFER         0x00000001
#define G_SHADE           0x00000004
#define G_CULL_FRONT      0x00000200
#define G_CULL_BACK       0x00000400
#define G_FOG             0x00010000
#define G_LIGHTING        0x00020000
#define G_SHADING_SMOOTH  0x00200000
#define G_CLIPPING        0x00800000

// G_MTX params
#define G_MTX_NOPUSH      0x00
#define G_MTX_PUSH        0x01
#define G_MTX_MUL         0x00
#define G_MTX_LOAD        0x02
#define G_MTX_MODELVIEW   0x00
#define G_MTX_PROJECTION  0x04

#define G_MW_FOG          0x08
#define G_MW_SEGMENT      0x06
#define G_MV_VIEWPORT     8

typedef struct { int16_t ob[3]; uint16_t flag; int16_t tc[2]; uint8_t cn[4]; } gbi_vtx_t;
typedef struct { int16_t i[16]; uint16_t f[16]; } gbi_mtx_t;

#define GBI_VTX(v0, n, addr)    gbi(((uint32_t)G_VTX << 24) | ((n) << 12) | (((v0) + (n)) << 1), (addr))
#define GBI_TRI_IDX(a, b, c)    ((((a) * 2) << 16) | (((b) * 2) << 8) | ((c) * 2))
#define GBI_MTX(addr, params)   gbi(((uint32_t)G_MTX << 24) | 0x380000 | ((params) ^ G_MTX_PUSH), (addr))
#define GBI_VIEWPORT(addr)      gbi(((uint32_t)G_MOVEMEM << 24) | (((16 - 1) / 8) << 19) | G_MV_VIEWPORT, (addr))
#define GBI_GEOMETRY(clr, set)  gbi(((uint32_t)G_GEOMETRYMODE << 24) | (~(uint32_t)(clr) & 0x00FFFFFF), (set))
#define GBI_TEXTURE(on, level, tile, s, t) \
    gbi(((uint32_t)G_TEXTURE << 24) | ((level) << 11) | ((tile) << 8) | ((on) << 1), ((uint32_t)(s) << 16) | (t))
#define GBI_SEGMENT(seg, base) gbi(((uint32_t)G_MOVEWORD << 24) | (G_MW_SEGMENT << 16) | ((seg) * 4), (base))
#define GBI_FOG(fm, fo)         gbi(((uint32_t)G_MOVEWORD << 24) | (G_MW_FOG << 16), ((uint32_t)(uint16_t)(fm) << 16) | (uint16_t)(fo))

#endif
