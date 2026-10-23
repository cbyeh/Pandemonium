#ifndef GLOBALS_H
#define GLOBALS_H

#include <t3d/t3d.h>
#include <stdint.h>

#define MODEL_SCALE 0.0625f
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define FIXED_TIMESTEP_MS 33.3f
#define ANIM_SPEED 1.0f
#define FRAME_BUFFER_COUNT 3    // a fourth let the CPU run frames ahead unevenly (judder, latency): main.c takes one after the update

#define DEV_MODE false
#define SHOW_FPS false
#define DEBUG_DRAW false // requires dev_mode
#define DEBUG_DRAW_ENVIRONMENTAL_HAZARDS false
#define DEBUG_MEMORY false // requires dev_mode

#define SKIP_OPENING_CREDITS false
#define PAL_MODE false
#define DRAW_CRT_SAFE_AREA false

#define PHASE_2_ENABLED true   // second fight: Guardian Unbound

#define DITHER_ENABLED false
#define ARES_AA_ENABLED false

enum {
    FONT_UNBALANCED = 2,
};

// FONT_UNBALANCED's styles: the text colour comes from these, not the prim colour
enum {
    FONT_STYLE_WHITE = 0,
    FONT_STYLE_GOLD  = 1,     // beaten: the story cleared, a Bonus boss slain
    FONT_STYLE_GREY  = 2,     // unavailable
    FONT_STYLE_BLUE  = 3,     // rare
    FONT_STYLE_RED   = 4,     // danger: the Tower's ascensions and nights
};

#endif