#ifndef WEAPON_H
#define WEAPON_H

#include <stdbool.h>

// The swords (2026-11-08, the user's design, docs/design/TOWER_ROGUELIKE.md "Swords"): bought in the
// Shrine's Armory (or Settings > Sword) once the best floor reaches theirs, one equipped, used in every
// mode. Each trades something; the Knight's sword is the all-rounder. Kept in the Tower save's
// weaponsOwned (bit id - 1) and weapon (the equipped id) bytes
typedef enum {     // (the ids are saved: new ones go at the end; the Armory lists them by floor, weapon_at_row)
    WPN_KNIGHT,         // the knight's own sword (always owned)
    WPN_LUNAR_EDGE,     // Lunar Edge: lighter swings, stronger skills, moonlight (1: in v1.1-beta's saves)
    WPN_NUBIUM,         // Mare Nubium: light, quick, cheap swings, lighter hits
    WPN_FRIGORIS,       // Mare Frigoris: heavy, slow, breaks stances
    WPN_TRANQUIL,       // Mare Tranquillitatis: perfect dodges pay
    WPN_SOLS_BANE,      // Sol's Bane: the Shackled Sun's hunter, fire halved
    WPN_SHADOWSLAYER,   // Shadowslayer: Shadowborne's hunter, its flames and beams halved
    WPN_LUNAR_REQUIEM,  // Lunar Requiem: the floor-100 reward, the Lunar Edge's skills without its cost, a second crescent
    WPN_COUNT
} WeaponId;

// What a blow on the player is, for the hunters (character_apply_damage_kind)
enum { DMG_PLAIN, DMG_FIRE, DMG_SHADOW };

int  weapon_at_row(int row);         // the Armory's row-th sword (by its floor)
const char *weapon_name(int id);
const char *weapon_desc(int id);    // what it does, one line
int  weapon_floor(int id);          // the best floor that puts it in the Armory (0: always)
int  weapon_cost(int id);           // Embers (-1 owned; 0 free: a milestone's, owned at its floor)
bool weapon_owned(int id);
bool weapon_available(int id);      // its floor reached (or owned)
int  weapon_equipped(void);
bool weapon_select(int id);         // buy (and equip) it, or equip an owned one; saved
float weapon_damage_mult(int kind, int bossPhase);  // a hit on a boss by kind (TOWER_HIT_*)
float weapon_poise_mult(int kind);  // its stagger build-up
float weapon_swing_speed(void);     // sword swings' speed (their clips and windows)
float weapon_stamina_mult(void);    // a swing's stamina
float weapon_taken_mult(int dmgKind);   // a blow on the player (DMG_*)
bool weapon_echo(void);             // Halfmoon Slash's second, smaller crescent
void weapon_on_perfect_dodge(void); // Mare Tranquillitatis' refund
void weapon_apply(void);            // the equipped one's look: the blade's colour, the skills' palette (boot, a slot loaded, equip)

#endif
