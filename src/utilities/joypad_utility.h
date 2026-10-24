#ifndef JOYPAD_UTILITY_H
#define JOYPAD_UTILITY_H

#include <libdragon.h>

// Declare global variables as extern so they can be accessed from other files
extern joypad_inputs_t joypad;
extern joypad_buttons_t btn;
extern joypad_buttons_t rel;

// Function prototype for joypad update
void joypad_utility_init(void);
void joypad_update(void);
void joypad_rumble_pulse_seconds(float seconds);
void joypad_rumble_stop(void);
void joypad_set_rumble_enabled(bool enabled);
bool joypad_is_rumble_enabled(void);

// Button layouts, applied in gameplay only (the menus keep A to confirm, B to go back)
enum { LAYOUT_CLASSIC, LAYOUT_SWAPPED, LAYOUT_SKILLS_ON_C, LAYOUT_COUNT };
void joypad_set_layout(int layout);
int  joypad_get_layout(void);
const char* joypad_layout_name(int layout);
const char* joypad_skill_button(int dir);   // a skill's button in the layout (0 up, 1 right, 2 down): "D-up" or "C-up"
void joypad_set_gameplay(bool on);     // the scene says, each frame, whether the player is in control

#endif