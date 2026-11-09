#ifndef BOSS_RENDER_H
#define BOSS_RENDER_H

#include "boss.h"

// Render module - handles drawing and debug visualization
// Read-only access to Boss state
void boss_draw_init(void);
// The fog texture, loaded once for good: the boss's scrolling material and the fog door share it
sprite_t *boss_fog_sprite(void);
void boss_draw_shadow(Boss* boss);
void boss_render_draw(Boss* boss);
/** The crucifix pose's weight (Guardian Unbound): 0 the sword in hand, 1 floating over the head. */
void boss_render_set_sword_float(float w);
void boss_render_forget_model(void);   // before the model is freed
void boss_render_forget_flash(void);   // the flash tint's cached materials (before the model is freed)
void boss_render_debug(Boss* boss, void* viewport);  // T3DViewport* but avoiding header dependency

#endif // BOSS_RENDER_H


