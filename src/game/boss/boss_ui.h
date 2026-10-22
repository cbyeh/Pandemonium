#ifndef BOSS_UI_H
#define BOSS_UI_H

#include <stdbool.h>

#include <t3d/t3d.h>
#include <t3d/t3dmath.h>

void boss_ui_init(void);
void boss_ui_cleanup(void);
void boss_ui_reset(void);

void boss_ui_set_intro(float progress);
// Name plate for the boss phase: 1 Guardian, 2 Guardian Unbound, 3 Fallen Knight.
void boss_ui_set_form(int phase);
void boss_ui_snap_health_trail(float ratio);

void boss_ui_draw_health_bar(const char *name, float ratio, float flash);
void boss_ui_draw_poise_bar(float poise, bool staggered, float time);

void boss_ui_draw_lockon_marker(
    T3DViewport *viewport,
    const T3DVec3 *worldPos,
    bool visible
);

void boss_ui_draw_post_boss_a_prompt(
    T3DViewport *viewport,
    const T3DVec3 *worldPos,
    bool visible
);

#endif // BOSS_UI_H