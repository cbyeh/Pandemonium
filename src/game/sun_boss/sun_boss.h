#ifndef SUN_BOSS_H
#define SUN_BOSS_H

#include "game/boss/boss.h"

// The Shackled Sun: Bonus fight 2 (boss phase 4). The chained sun the King keeps
// above the hall, a floating eye that glares crossed waves, rains pillars of light,
// sweeps a corona around itself and dives onto the player (then rests, open to
// attack). At half health it breaks its shackles and burns hotter and faster.
//
// Like the Fallen Knight it reuses the Boss struct; only update and draw are its own.

#define SUN_BOSS_PHASE 4

// Turns `boss` into the Shackled Sun (descends from the ceiling, then fights).
void sun_boss_begin(Boss* boss);
// Ends its descent and starts the fight (the intro cutscene ends or is skipped).
void sun_boss_skip_intro(Boss* boss);
bool sun_boss_stagger(Boss* boss);
void sun_boss_half_stagger(Boss* boss);   // the meter half full: a flinch that breaks off a windup   // the stagger meter broke: false if it can't stagger now
void sun_boss_update(Boss* boss, float dt);
void sun_boss_draw(Boss* boss);
void sun_boss_free(void);

#endif
