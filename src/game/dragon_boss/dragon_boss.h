#ifndef DRAGON_BOSS_H
#define DRAGON_BOSS_H

#include "game/boss/boss.h"

// Shadowborne: Bonus fight 3 (boss phase 5), inspired by Darkeater Midir. A giant
// crystal-backed dragon built from procedural parts: it claws, bites, whips its tail,
// breathes sweeping fire, charges and strafes the hall from the air. At half health
// the dark takes it: a bursting nova, homing dark orbs, and a long dark beam that
// leaves explosions in its wake and exhausts it (its head drops: the opening).
//
// Like the other Bonus bosses it reuses the Boss struct; only update and draw are its own.

#define DRAGON_BOSS_PHASE 5

// Turns `boss` into Shadowborne (flies down from above, then fights).
void dragon_boss_begin(Boss* boss);
// Lands it at once and starts the fight (the intro cutscene ends or is skipped).
void dragon_boss_skip_intro(Boss* boss);
bool dragon_boss_stagger(Boss* boss);   // the stagger meter broke: false if it can't stagger now
void dragon_boss_update(Boss* boss, float dt);
void dragon_boss_draw(Boss* boss);
void dragon_boss_free(void);

#endif
