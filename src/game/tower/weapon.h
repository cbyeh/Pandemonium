#ifndef WEAPON_H
#define WEAPON_H

#include <stdbool.h>

// The swords (2026-11-08, the user's design): bought in the Shrine's Armory once the best floor
// reaches theirs, one equipped, used everywhere (Story, Bonus, the Tower). Each trades something;
// the Knight's sword is the all-rounder. Kept in the Tower save's weaponsOwned (bit id - 1) and
// weapon (the equipped id) bytes
typedef enum {
    WPN_KNIGHT,         // the knight's own sword (always owned)
    WPN_LUNAR_EDGE,     // sword swings -20%, skills +30%; a moonlit blue blade, the skills in moonlight
    WPN_COUNT
} WeaponId;

const char *weapon_name(int id);
const char *weapon_desc(int id);    // what it does, one line
int  weapon_floor(int id);          // the best floor that puts it in the Armory (0: always)
int  weapon_cost(int id);           // Embers (-1 owned)
bool weapon_owned(int id);
bool weapon_available(int id);      // its floor reached (or owned)
int  weapon_equipped(void);
bool weapon_select(int id);         // buy (and equip) it, or equip an owned one; saved
float weapon_damage_mult(int kind); // a hit on a boss by kind (TOWER_HIT_*)
void weapon_apply(void);            // the equipped one's look: the blade's colour, the skills' palette (boot, a slot loaded, equip)

#endif
