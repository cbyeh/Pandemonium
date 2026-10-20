#ifndef GUARDIAN_ENDING_H
#define GUARDIAN_ENDING_H

#include <stdbool.h>

// The ending, rendered live (it used to be a pre-rendered video): the knight walks
// down the hall to the King on his throne of gold; the King calls down the Shackled
// Sun, and the knight falls through it, made small: "a fitting image".

void ending_begin(void);
// Returns false once the ending is over.
bool ending_update(float dt);
// The hall (with the knight) is drawn by the scene in the shots where it is visible.
bool ending_room_visible(void);
void ending_draw_world(void);
void ending_draw_overlay(void);
void ending_free(void);

#endif
