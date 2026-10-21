#ifndef TOWER_H
#define TOWER_H

#include <stdbool.h>
#include <stdint.h>

// The Tower: a boss-rush roguelite. A run climbs floors (one fight each); every
// five floors make a circle (the Guardian, the three Bonus bosses in a random order,
// then Guardian Unbound), and the bosses start well below the story fights' strength
// and grow every circle.
// A run starts as the original game did: 3 potions and the sword only. Between
// floors the player drafts a Boon (run-only); one of the three is always a skill
// card while any are left: learn a D-pad skill, or upgrade one learned this run.
// Cleared floors earn Embers, spent at the Shrine on permanent upgrades (Tower only).

typedef enum { TOWER_GUARDIAN, TOWER_UNBOUND, TOWER_FALLEN_KNIGHT, TOWER_SHACKLED_SUN, TOWER_SHADOWBORN } TowerBoss;

typedef enum {
    BOON_EMBER_EDGE, BOON_IRON_SKIN, BOON_DEEP_FLASK, BOON_KINDLED_FLASK, BOON_FLEETFOOT,
    BOON_SECOND_WIND, BOON_MOON_MASTERY, BOON_BLOODTHIRST, BOON_VIGOR, BOON_LAST_STAND,
    BOON_UNBROKEN, BOON_CRESCENT_ECHO,
    // Skill cards: learn a skill for the run, then upgrade it
    BOON_LEARN_MOONBLADE, BOON_LEARN_HALFMOON, BOON_LEARN_CLEAVE, BOON_HALFMOON_PLUS, BOON_CLEAVE_PLUS,
    BOON_COUNT
} BoonId;

typedef enum {
    UPG_FLASK_CAPACITY, UPG_FLASK_POTENCY, UPG_HONED_BLADE, UPG_VIGOR, UPG_SKILL_MASTERY,
    UPG_COUNT
} UpgradeId;

#define UPG_MAX_LEVEL 3

// ---- Run ----
void tower_begin_run(uint32_t seed);
void tower_end_run(void);
bool tower_active(void);
int  tower_floor(void);                 // 1-based
int  tower_circle(void);                // 1-based
TowerBoss tower_floor_boss(void);
int  tower_run_embers(void);            // earned this run (already banked)
int  tower_boon_stacks(BoonId b);
// The current floor's boss fell: bank its Embers, record the best floor, draw a draft.
void tower_floor_cleared(void);
// Take the drafted boon and move to the next floor.
void tower_draft_pick(int index);
void tower_next_floor(void);

// ---- Draft screen ----
void tower_draft_open(void);
bool tower_draft_is_open(void);
// Returns true once a boon was taken (the scene starts the next floor).
bool tower_draft_update(bool left, bool right, bool confirm);
void tower_draft_draw(void);
void tower_hud_draw(void);              // floor / circle during a run fight
const char* tower_death_summary(void);   // a line for the death screen

// ---- Shrine (persistent, per save slot) ----
int  tower_embers(void);
int  tower_upgrade_level(UpgradeId u);
int  tower_upgrade_cost(UpgradeId u);   // -1 at max level
bool tower_upgrade_buy(UpgradeId u);
const char* tower_upgrade_name(UpgradeId u);
const char* tower_upgrade_desc(UpgradeId u);
int  tower_best_floor(void);

// ---- Modifiers (Shrine upgrades and Boons, during a run only) ----
// In a run a skill can be used only once learned from a draft (the Skills setting is ignored)
bool  tower_mod_skill_allowed(int skill);
float tower_mod_skill_damage(int skill);    // the skill's upgrade card: 1.5
float tower_mod_damage_dealt(void);
float tower_mod_damage_taken(void);
int   tower_mod_max_potions(void);
float tower_mod_potion_heal(void);      // fraction of max health
float tower_mod_max_health(void);
float tower_mod_roll_cost(void);
float tower_mod_stamina_regen(void);
float tower_mod_skill_cooldown(void);
float tower_mod_lifesteal(void);
int   tower_mod_extra_mbd_waves(void);
bool  tower_mod_try_unbroken(void);     // spends it: survive a lethal hit at 1 HP
// Boss health in a run, from its usual (skills off) health for that phase
float tower_boss_health_scale(int phase);
// Between floors: the health and potions given back
void  tower_floor_restore(void);

#endif
