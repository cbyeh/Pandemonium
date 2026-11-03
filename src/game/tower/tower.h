#ifndef TOWER_H
#define TOWER_H

#include <stdbool.h>
#include <stdint.h>

// The Tower: a boss-rush roguelite. A run climbs floors (one fight each). A circle is a
// map of 7 rows (docs/design/TOWER_ROGUELIKE.md section 2): the Guardian, four rows of
// Fights (Bonus bosses), Elites, Events, Shops and Rests to choose a path through, a Rest on every lane,
// then Guardian Unbound: 4-6 floors by the path. The bosses start well below the story
// fights' strength and grow every circle.
// A run starts as the original game did: 3 potions and the sword only. Between
// floors the player drafts a Boon (run-only); one of the three is always a skill
// card while any are left: learn a D-pad skill, or upgrade one learned this run.
// Cleared floors earn Embers, spent at the Shrine on permanent upgrades (Tower only).

typedef enum { TOWER_GUARDIAN, TOWER_UNBOUND, TOWER_FALLEN_KNIGHT, TOWER_SHACKLED_SUN, TOWER_SHADOWBORN } TowerBoss;

typedef enum {
    BOON_EMBER_EDGE, BOON_IRON_SKIN, BOON_DEEP_FLASK, BOON_KINDLED_FLASK, BOON_FLEETFOOT,
    BOON_SECOND_WIND, BOON_MOON_MASTERY, BOON_BLOODTHIRST, BOON_VIGOR, BOON_LAST_STAND,
    BOON_UNBROKEN,
    // Rare and legendary: change how a run plays
    BOON_PHANTOM_STEP, BOON_RIPOSTE, BOON_CRUSHING_BLOWS, BOON_EXECUTIONER, BOON_STORM_BLADE, BOON_BLOOD_PACT,
    BOON_RALLY,
    // The families' packs, bought at the Shrine (docs/design/TOWER_ROGUELIKE.md section 7)
    BOON_HEMORRHAGE, BOON_SANGUINE_FLASK, BOON_RED_THIRST,                          // Blood
    BOON_STATIC, BOON_THUNDERCLAP, BOON_CONDUCTOR, BOON_OVERCHARGE,                  // Storm
    BOON_LUNAR_TIDE, BOON_ECLIPSE, BOON_WAXING_MOON,                                 // Moon
    BOON_HEAVY_HAND, BOON_STALWART,                                                  // Iron
    BOON_SHADOW_EDGE, BOON_EVASION, BOON_VANISH,                                     // Shadow
    BOON_PROSPECTOR,                                                                 // Hearth (gold: stage 4)
    BOON_BERSERKER,                                                                  // Blood (stage 6)
    // Skill cards (no slot): learn a skill for the run, its card again its next level (Lv 2-4), then
    // one of its two Lv 5 forms (A, B: taking one locks the other out for the run)
    BOON_LEARN_MOONBLADE, BOON_LEARN_HALFMOON, BOON_LEARN_CLEAVE,
    BOON_FULL_MOON_DANCE, BOON_SHATTERING_DANCE, BOON_TWIN_MOON, BOON_FULL_MOON, BOON_CRESCENT_STORM, BOON_LONG_CLEAVE,
    // "+": a boon's top level, offered once it is at its max (no slot of its own)
    BOON_EMBER_EDGE_PLUS, BOON_IRON_SKIN_PLUS, BOON_STORM_BLADE_PLUS, BOON_EXECUTIONER_PLUS, BOON_CRUSHING_BLOWS_PLUS,
    BOON_RIPOSTE_PLUS, BOON_UNBROKEN_PLUS, BOON_BLOODTHIRST_PLUS, BOON_RALLY_PLUS, BOON_PHANTOM_STEP_PLUS,
    BOON_STATIC_PLUS, BOON_THUNDERCLAP_PLUS, BOON_MOON_MASTERY_PLUS,
    // Duos: two maxed boons of these families merged into one slot (both keep working)
    BOON_CRIMSON_STORM, BOON_THUNDERSTEP, BOON_BLOOD_MOON_RITE, BOON_IRON_TIDE, BOON_SHADES_MERCY,
    BOON_ANVIL_OF_STORMS, BOON_WANING_LIGHT, BOON_STEEL_HEARTH, BOON_SKYFALL,
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

// ---- Between fights: the draft, then the map (and a Rest's screens) ----
void tower_draft_open(void);
bool tower_draft_is_open(void);
int  tower_draft_selected(void);        // the card under the cursor (nChoices: Skip)
void tower_map_open(void);              // the circle's map (the scene opens it at a run's start)
bool tower_map_is_open(void);
bool tower_rest_is_open(void);          // a Rest's, a Shop's or an Event's screens
bool tower_between_open(void);          // any of them
// Returns true once a fight's node is entered on the map (the scene starts tower_floor_boss's fight).
// back (B) leaves a Shop; it does nothing on the other screens
bool tower_between_update(bool left, bool right, bool up, bool down, bool confirm, bool back);
int  tower_gold(void);                  // this run's gold (lost when it ends)
void tower_between_draw(void);
void tower_hud_draw(void);              // floor / circle during a run fight
const char* tower_death_summary(void);   // a line for the death screen
const char* tower_run_summary(void);     // the run so far, for the pause menu's Stats page (several lines)
// The Build page: the 3 skills, then the 5 boon slots (name, its level on the right, a font style); returns the rows
#define TOWER_SLOTS 5
// (a duo's two boons on a row under it, then the rivals locked out this run, greyed)
#define TOWER_BUILD_ROWS (3 + 2 * TOWER_SLOTS + 3)
int tower_build_rows(char left[][40], char right[][12], int style[], int max);

// ---- Nights: from the second circle each circle has one (a trade-off, more Embers) ----
const char* tower_night_name(void);     // "" for none

// ---- Relics: bought at the Shrine, one equipped for a climb ----
#define TOWER_RELIC_SLOTS 3             // saves before 1.0 kept up to three found ones (converted on load)
int  tower_relic_total(void);
const char* tower_relic_name(int id);   // "None" for -1
const char* tower_relic_desc(int id);
bool tower_relic_owned(int id);
int  tower_relic_cost(int id);          // -1 owned
int  tower_relic_equipped(void);        // -1: none
bool tower_relic_select(int id);        // buy (and equip) it, or equip / take off an owned one

// ---- Nightfarer, a skin bought at the Shrine ----
bool tower_nightfarer_owned(void);
int  tower_nightfarer_cost(void);
bool tower_nightfarer_buy(void);

// ---- The Shrine's unlock ladder: each family's new boons, a pack bought in the player's order ----
int  tower_pack_count(void);
const char* tower_pack_name(int i);
const char* tower_pack_desc(int i);     // the boons it adds
bool tower_pack_owned(int i);
int  tower_pack_cost(int i);            // -1 owned
bool tower_pack_buy(int i);

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
// A skill's level (docs/design/TOWER_ROGUELIKE.md section 6): 1-4, 5 with a form; outside the Tower 1
int   tower_skill_level(int skill);
enum { SKILL_FORM_NONE, SKILL_FORM_A, SKILL_FORM_B };
int   tower_skill_form(int skill);          // its Lv 5 form (SKILL_FORM_)
float tower_mod_damage_dealt(void);
float tower_mod_damage_taken(void);
enum { TOWER_HIT_OTHER, TOWER_HIT_SWORD, TOWER_HIT_SKILL, TOWER_HIT_FINISHER };   // (a finisher is a sword hit too)
float tower_mod_damage_vs(float bossHealthFrac, bool staggered, int kind);   // pacts and timed boons against this boss, for this hit
bool  tower_hit_impaired(void);         // the Impaired curse: this hit does nothing
void  tower_on_roll(void);              // a roll started (the Exposed curse)
int   tower_mod_max_potions(void);
#define POTION_CAP 15                   // potions held at most, in any run (two digits on the HUD)
float tower_mod_potion_heal(void);      // fraction of max health
float tower_mod_max_health(void);
float tower_mod_roll_cost(void);
float tower_mod_stamina_regen(void);
float tower_mod_stamina_empty(void);    // divides the wait before an empty stamina bar refills (Second Wind)
float tower_mod_skill_cooldown(void);
float tower_mod_lifesteal(void);
// Rally (Bloodborne's regain): health lost to a blow can be won back by hitting the boss soon
// after. tower_on_player_hurt adds to it, tower_on_boss_hurt wins it back, tower_tick drains it
void  tower_on_player_hurt(float lost);
void  tower_on_boss_hurt(float dealt);
void  tower_tick(float dt);
float tower_rally_pool(void);           // health still to be won back (0 without the boon)
bool  tower_mod_try_unbroken(void);     // spends it: survive a lethal hit at 1 HP
float tower_mod_unbroken_heal(void);    // health on top of that 1 (Unbroken+: 20% of max)
float tower_mod_poise(void);            // how fast the bosses' stagger meters fill
float tower_mod_stagger_time(void);     // how long a staggered boss stays down
float tower_mod_stagger_damage(int kind);   // damage taken by a staggered boss (TOWER_HIT_): its bonuses added, +100% at most
float tower_mod_perfect_window(void);   // seconds of a roll that count as a perfect dodge
void  tower_on_perfect_dodge(void);
float tower_on_sword_hit(bool finisher); // a landed sword hit: its lightning's damage (Storm Blade, Static, Thunderclap, Overcharge), 0 mostly
float tower_lightning_poise(void);      // a bolt's share of the stagger meter, in plain sword hits (Conductor: 1)
float tower_mod_finisher_poise(void);   // the combo's 3rd hit's stagger (Heavy Hand)
float tower_mod_hit_cooldown_cut(void); // seconds off every skill cooldown a sword hit (Lunar Tide)
void  tower_on_potion(void);            // a potion drunk (Sanguine Flask)
void  tower_on_skill_cast(void);        // a skill cast (Waxing Moon)
void  tower_on_stagger(void);           // a boss staggered (Overcharge, Iron Tide)
void  tower_on_stagger_end(void);       // a staggered boss rose (Crushing Blows+)
bool  tower_poise_drain_held(void);     // the stagger meter doesn't drain now (Crushing Blows+)
float tower_on_lightning(float bolt, bool staggered);   // a bolt about to land: its damage (Crimson Storm, Anvil of Storms)
void  tower_on_skill_hit(void);         // a skill's hit landed (Skyfall)
float tower_mod_blow(float amount);     // a blow about to land, after damage taken: what it deals (Stalwart, Vanish)
// ---- Elite affixes (docs/design/TOWER_ROGUELIKE.md section 11): the three Bonus bosses only ----
enum { AFX_SWIFT, AFX_IRONCLAD, AFX_ENRAGED, AFX_RELENTLESS, AFX_VAMPIRIC, AFX_SMOULDERING, AFX_COUNT };
bool  tower_affix(int a);               // the fight under way is an Elite with it
const char* tower_affix_tag(void);      // its affixes' names for the name bar ("Swift Ironclad"), "" for none
// The boss's footwork and breathers sped up: Swift x1.12; Enraged's breathers -30% below half health (a dt multiplier)
float tower_elite_pace(bool breather, float bossHealthFrac);
float tower_elite_blow(float bossHealthFrac);   // its blows (Enraged: +25% below half health)
float tower_mod_poise_drain(void);      // how fast the stagger meter drains (Relentless 2x)
float tower_mod_poise_bar(void);        // the stagger bar's drawn length (Ironclad: 1.3x)
void  tower_on_boss_dealt(float lost);  // health a blow took (Vampiric: the boss heals 8% of it)
bool  tower_elite_held_longer(void);    // Enraged: its half-health war cry or break held longer
float tower_elite_pitch(void);          // its own sounds' rate (Swift: 1.12, a little higher)
// ---- Berserker (Blood, section 7): below half health ----
bool  tower_berserk(void);              // active: blows don't flinch, no potions
bool  tower_potion_blocked(void);
// Boss health in a run, from its usual (skills off) health for that phase
float tower_boss_health_scale(int phase);
// Between floors: the health and potions given back
void  tower_floor_restore(void);

#endif
