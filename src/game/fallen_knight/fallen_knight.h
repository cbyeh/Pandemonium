#ifndef FALLEN_KNIGHT_H
#define FALLEN_KNIGHT_H

#include "game/boss/boss.h"

// The Fallen Knight: Bonus fight from the title menu (boss phase 3). A dark,
// larger mirror of the player built from the knight model and animations, with
// melee combos, rolls and hostile versions of the player's skills.
//
// It reuses the Boss struct (position, health, colliders, lock-on, health bar,
// damage and death flow); only its update and draw are its own.

#define FALLEN_KNIGHT_PHASE 3

// Turns `boss` into the Fallen Knight (descends from above, then fights).
void fallen_knight_begin(Boss* boss);
// Lands it at once and starts the fight (cutscene skip).
void fallen_knight_skip_intro(Boss* boss);
bool fallen_knight_stagger(Boss* boss);
void fallen_knight_half_stagger(Boss* boss);   // the meter half full: a flinch that breaks off a windup   // the stagger meter broke: false if it can't stagger now
void fallen_knight_update(Boss* boss, float dt);
void fallen_knight_draw(Boss* boss);
// Frees the knight instance (skeleton/animations); safe to call any time.
void fallen_knight_free(void);

#endif
