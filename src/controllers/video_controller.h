#ifndef VIDEO_CONTROLLER_H
#define VIDEO_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Video/UI-related user settings.
 *
 * Persistence:
 * - `uiOverscanX/Y` and `hdAspect` are persisted in EEPROM via `save_controller.*`.
 */

// Render aspect toggle used by camera/projection and menu setting.
extern bool hdAspect;

// Additional user-controlled UI overscan padding (pixels).
// Positive values move edge-anchored UI further inward to avoid CRT clipping.
// Negative values widen UI toward the screen edges (min = widest).
#define UI_OVERSCAN_MIN -15
#ifdef HLE_BUILD
// Emulator front-ends (e.g. Delta skins) often crop/zoom the picture: start with wider margins
#define UI_OVERSCAN_DEFAULT_X -7
#define UI_OVERSCAN_DEFAULT_Y -5
#else
#define UI_OVERSCAN_DEFAULT_X UI_OVERSCAN_MIN
#define UI_OVERSCAN_DEFAULT_Y UI_OVERSCAN_MIN
#endif

extern int8_t uiOverscanX;
extern int8_t uiOverscanY;

#endif

