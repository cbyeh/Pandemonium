#ifndef FALLEN_KNIGHT_H
#define FALLEN_KNIGHT_H

#include "game/boss/boss.h"

// The Fallen Knight: final fight after Guardian Unbound (boss phase 3). A dark,
// larger mirror of the player built from the knight model and animations, with
// melee combos, rolls and hostile versions of the player's skills.
//
// It reuses the Boss struct (position, health, colliders, lock-on, health bar,
// damage and death flow); only its update and draw are its own.

#define FALLEN_KNIGHT_PHASE 3

// Turns `boss` into the Fallen Knight (descends from above, then fights).
void fallen_knight_begin(Boss* boss);
void fallen_knight_update(Boss* boss, float dt);
void fallen_knight_draw(Boss* boss);
// Frees the knight instance (skeleton/animations); safe to call any time.
void fallen_knight_free(void);

#endif
