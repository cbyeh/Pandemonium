#include <libdragon.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "tower.h"
#include "character.h"
#include "globals.h"
#include "save_controller.h"
#include "video_controller.h"
#include "video_layout.h"
#include "joypad_utility.h"
#include "game/boss/boss.h"

// ---- Tuning ----
// The climb starts well below the story fights and passes them by circle 3
#define BOSS_HP_START       0.70f     // circle 1, then +BOSS_HP_PER_CIRCLE a circle
#define BOSS_HP_PER_CIRCLE  0.30f
#define BOSS_HP_LATE_CIRCLE 6         // from this circle +BOSS_HP_PER_LATE a circle, so even full boon sets run out of floors
#define BOSS_HP_PER_LATE    0.40f
#define BOSS_DMG_START      0.60f     // circle 1, then +BOSS_DMG_PER_CIRCLE a circle
#define BOSS_DMG_PER_CIRCLE 0.20f
#define DRAFT_CHOICES       3
#define START_POTIONS       3         // as in the original game
// A floor is one fight: the Guardian's two phases are two floors of 100, and a Bonus boss has
// 55% of its story health (99, 132, 151), so each floor costs about what a Guardian phase does
#define BONUS_HP_IN_TOWER   0.55f
#define HP_PER_SKILL        0.08f     // per skill offered (a skill adds about 7-8% to the damage dealt)

enum { TIER_COMMON, TIER_RARE, TIER_LEGENDARY };
// Six families (the card's tag, its colour); skill cards have none and take no slot
enum { FAM_BLOOD, FAM_STORM, FAM_MOON, FAM_IRON, FAM_SHADOW, FAM_HEARTH, FAM_COUNT, FAM_SKILL = FAM_COUNT };
static const struct { const char *name; int style; } FAMILIES[FAM_COUNT + 1] = {
    [FAM_BLOOD] = { "Blood", FONT_STYLE_RED },   [FAM_STORM] = { "Storm", FONT_STYLE_BLUE }, [FAM_MOON] = { "Moon", FONT_STYLE_TEAL },
    [FAM_IRON]  = { "Iron", FONT_STYLE_WHITE },  [FAM_SHADOW] = { "Shadow", FONT_STYLE_VIOLET }, [FAM_HEARTH] = { "Hearth", FONT_STYLE_GOLD },
    [FAM_SKILL] = { "Skill", FONT_STYLE_GREY },
};
// pack: in its family's pack (bought at the Shrine), not in the pool from the start. up: a "+"
// card's boon (BoonId + 1); duo: a duo's second family (+ 1; family is its first)
typedef struct { const char *name, *desc; uint8_t maxStacks, tier, family, pack, up, duo; } BoonDef;
#define PLUS(b)   0, (b) + 1, 0
#define DUO(f)    0, 0, (f) + 1

// maxStacks is the boon's top level: each level adds the old per-stack value unless the desc says otherwise
static const BoonDef BOONS[BOON_COUNT] = {
    [BOON_EMBER_EDGE]    = { "Ember Edge",    "+15% damage (Lv 4-5: +10%)",   5, TIER_COMMON, FAM_HEARTH },
    [BOON_IRON_SKIN]     = { "Iron Skin",     "-15% damage taken (Lv 3: -10%)", 3, TIER_COMMON, FAM_IRON },
    [BOON_DEEP_FLASK]    = { "Deep Flask",    "+3 potions",                   2, TIER_COMMON, FAM_HEARTH },
    [BOON_KINDLED_FLASK] = { "Kindled Flask", "Potions heal 25% more",        2, TIER_COMMON, FAM_HEARTH },
    [BOON_FLEETFOOT]     = { "Fleetfoot",     "Rolls cost 40% less stamina",  1, TIER_RARE, FAM_SHADOW },
    [BOON_SECOND_WIND]   = { "Second Wind",   "+40% stamina regen",     3, TIER_COMMON, FAM_SHADOW },
    [BOON_MOON_MASTERY]  = { "Moon Mastery",  "Skill cooldowns -20%",         3, TIER_RARE, FAM_MOON },
    [BOON_BLOODTHIRST]   = { "Bloodthirst",   "Sword hits heal 12% of base damage", 2, TIER_RARE, FAM_BLOOD },
    [BOON_VIGOR]         = { "Vigor",         "+25 max health",               5, TIER_COMMON, FAM_IRON },
    [BOON_LAST_STAND]    = { "Last Stand",    "+40% damage below 30% health", 2, TIER_RARE, FAM_BLOOD },
    [BOON_UNBROKEN]      = { "Unbroken",      "Survive one killing blow a floor", 1, TIER_LEGENDARY, FAM_IRON },
    [BOON_CRESCENT_ECHO] = { "Moon Blade Dance+", "2 more waves", 1, TIER_RARE, FAM_SKILL },
    [BOON_PHANTOM_STEP]  = { "Phantom Step",  "Easier dodges, each heals 2%", 1, TIER_RARE, FAM_SHADOW },
    [BOON_RIPOSTE]       = { "Riposte",       "Perfect dodge: +50% damage", 1, TIER_RARE, FAM_SHADOW },
    [BOON_CRUSHING_BLOWS]= { "Crushing Blows","Staggers last 50% longer",     1, TIER_RARE, FAM_IRON },
    [BOON_EXECUTIONER]   = { "Executioner",   "Staggered bosses take +60%",   1, TIER_LEGENDARY, FAM_IRON },
    [BOON_STORM_BLADE]   = { "Storm Blade",   "Every 4th hit calls lightning", 1, TIER_LEGENDARY, FAM_STORM },
    [BOON_BLOOD_PACT]    = { "Blood Pact",    "+35% damage, potions -25%", 1, TIER_LEGENDARY, FAM_BLOOD },
    [BOON_RALLY]         = { "Rally",         "Hit back to win lost health", 2, TIER_RARE, FAM_BLOOD },
    [BOON_HEMORRHAGE]    = { "Hemorrhage",    "Finishers bleed 4 over 3 s", 3, TIER_COMMON, FAM_BLOOD, 1 },
    [BOON_SANGUINE_FLASK]= { "Sanguine Flask","A potion: +25% damage for 6 s", 1, TIER_RARE, FAM_BLOOD, 1 },
    [BOON_RED_THIRST]    = { "Red Thirst",    "+10% damage per 25% health lost", 3, TIER_COMMON, FAM_BLOOD, 1 },
    [BOON_STATIC]        = { "Static",        "Storm Blade +3, or a bolt each 8th hit", 4, TIER_COMMON, FAM_STORM, 1 },
    [BOON_THUNDERCLAP]   = { "Thunderclap",   "Combo finishers call lightning", 1, TIER_RARE, FAM_STORM, 1 },
    [BOON_CONDUCTOR]     = { "Conductor",     "Lightning fills the stagger meter", 1, TIER_RARE, FAM_STORM, 1 },
    [BOON_OVERCHARGE]    = { "Overcharge",    "A stagger: 5 hits call lightning", 1, TIER_LEGENDARY, FAM_STORM, 1 },
    [BOON_LUNAR_TIDE]    = { "Lunar Tide",    "Sword hits: cooldowns -0.3 s", 3, TIER_COMMON, FAM_MOON, 1 },
    [BOON_ECLIPSE]       = { "Eclipse",       "Skills +50% on staggers", 1, TIER_RARE, FAM_MOON, 1 },
    [BOON_WAXING_MOON]   = { "Waxing Moon",   "A cast: sword +15% for 4 s", 1, TIER_RARE, FAM_MOON, 1 },
    [BOON_HEAVY_HAND]    = { "Heavy Hand",    "Finishers: +20% stagger", 3, TIER_COMMON, FAM_IRON, 1 },
    [BOON_STALWART]      = { "Stalwart",      "Blows over 20% of max health -25%", 1, TIER_RARE, FAM_IRON, 1 },
    [BOON_SHADOW_EDGE]   = { "Shadow Edge",   "+8% damage for 2 s after a roll", 4, TIER_COMMON, FAM_SHADOW, 1 },
    [BOON_EVASION]       = { "Evasion",       "Perfect dodges refund the roll", 1, TIER_RARE, FAM_SHADOW, 1 },
    [BOON_VANISH]        = { "Vanish",        "3 perfect dodges: a blow deals 0", 1, TIER_LEGENDARY, FAM_SHADOW, 1 },
    [BOON_LEARN_MOONBLADE] = { "Moon Blade Dance", "Learn it: D-up", 1, TIER_COMMON, FAM_SKILL },
    [BOON_LEARN_HALFMOON]  = { "Halfmoon Slash",   "Learn it: D-right", 1, TIER_COMMON, FAM_SKILL },
    [BOON_LEARN_CLEAVE]    = { "Crescent Cleave",  "Learn it: D-down", 1, TIER_COMMON, FAM_SKILL },
    [BOON_HALFMOON_PLUS]   = { "Halfmoon Slash+",  "+20% damage", 1, TIER_RARE, FAM_SKILL },
    [BOON_CLEAVE_PLUS]     = { "Crescent Cleave+", "+20% damage", 1, TIER_RARE, FAM_SKILL },
    // "+" (docs/design/TOWER_ROGUELIKE.md section 10; Second Wind+ is held back: a potion is drunk at once)
    [BOON_EMBER_EDGE_PLUS]    = { "Ember Edge+",    "Finishers +25%",              1, TIER_RARE, FAM_HEARTH, PLUS(BOON_EMBER_EDGE) },
    [BOON_IRON_SKIN_PLUS]     = { "Iron Skin+",     "The first blow a floor deals half", 1, TIER_RARE, FAM_IRON, PLUS(BOON_IRON_SKIN) },
    [BOON_STORM_BLADE_PLUS]   = { "Storm Blade+",   "Lightning every 3rd hit",     1, TIER_RARE, FAM_STORM, PLUS(BOON_STORM_BLADE) },
    [BOON_EXECUTIONER_PLUS]   = { "Executioner+",   "Staggered bosses take +90%",  1, TIER_RARE, FAM_IRON, PLUS(BOON_EXECUTIONER) },
    [BOON_CRUSHING_BLOWS_PLUS]= { "Crushing Blows+","No stagger drain 3 s after it rises", 1, TIER_RARE, FAM_IRON, PLUS(BOON_CRUSHING_BLOWS) },
    [BOON_RIPOSTE_PLUS]       = { "Riposte+",       "Riposte lasts 4 s",           1, TIER_RARE, FAM_SHADOW, PLUS(BOON_RIPOSTE) },
    [BOON_UNBROKEN_PLUS]      = { "Unbroken+",      "When it saves you, heal 20%", 1, TIER_RARE, FAM_IRON, PLUS(BOON_UNBROKEN) },
    [BOON_BLOODTHIRST_PLUS]   = { "Bloodthirst+",   "Heals 16% a level",           1, TIER_RARE, FAM_BLOOD, PLUS(BOON_BLOODTHIRST) },
    [BOON_RALLY_PLUS]         = { "Rally+",         "Each hit wins back 6",        1, TIER_RARE, FAM_BLOOD, PLUS(BOON_RALLY) },
    [BOON_PHANTOM_STEP_PLUS]  = { "Phantom Step+",  "Dodge window 0.31 s",         1, TIER_RARE, FAM_SHADOW, PLUS(BOON_PHANTOM_STEP) },
    [BOON_STATIC_PLUS]        = { "Static+",        "Its lightning +50%",          1, TIER_RARE, FAM_STORM, PLUS(BOON_STATIC) },
    [BOON_THUNDERCLAP_PLUS]   = { "Thunderclap+",   "Its lightning +50%",          1, TIER_RARE, FAM_STORM, PLUS(BOON_THUNDERCLAP) },
    [BOON_MOON_MASTERY_PLUS]  = { "Moon Mastery+",  "Cooldowns -25% a level",      1, TIER_RARE, FAM_MOON, PLUS(BOON_MOON_MASTERY) },
    // Duos (section 8): Legendary, offered with a maxed boon of each family slotted; merged into one slot
    [BOON_CRIMSON_STORM]   = { "Crimson Storm",   "Bolts heal, or +25%", 1, TIER_LEGENDARY, FAM_BLOOD, DUO(FAM_STORM) },
    [BOON_THUNDERSTEP]     = { "Thunderstep",     "2 bolts each perfect dodge", 1, TIER_LEGENDARY, FAM_STORM, DUO(FAM_SHADOW) },
    [BOON_BLOOD_MOON_RITE] = { "Blood Moon Rite", "Casts: -3% health, +40%", 1, TIER_LEGENDARY, FAM_BLOOD, DUO(FAM_MOON) },
    [BOON_IRON_TIDE]       = { "Iron Tide",       "Staggers: skills ready", 1, TIER_LEGENDARY, FAM_IRON, DUO(FAM_MOON) },
    [BOON_SHADES_MERCY]    = { "Shade's Mercy",   "Perfects: Rally at 60%", 1, TIER_LEGENDARY, FAM_SHADOW, DUO(FAM_BLOOD) },
    [BOON_ANVIL_OF_STORMS] = { "Anvil of Storms", "Lightning x2 on staggers", 1, TIER_LEGENDARY, FAM_IRON, DUO(FAM_STORM) },
    [BOON_WANING_LIGHT]    = { "Waning Light",    "Perfects: skills -2 s", 1, TIER_LEGENDARY, FAM_MOON, DUO(FAM_SHADOW) },
    [BOON_STEEL_HEARTH]    = { "Steel Hearth",    "Potions: +10 max health, -40% taken", 1, TIER_LEGENDARY, FAM_HEARTH, DUO(FAM_IRON) },
    [BOON_SKYFALL]         = { "Skyfall",         "A skill's hit calls lightning", 1, TIER_LEGENDARY, FAM_STORM, DUO(FAM_MOON) },
};
#undef PLUS
#undef DUO
// Rival pairs (section 9): taking one locks the other out for the run
static const uint8_t RIVALS[][2] = {
    { BOON_RIPOSTE, BOON_IRON_SKIN }, { BOON_BLOOD_PACT, BOON_KINDLED_FLASK }, { BOON_STORM_BLADE, BOON_EXECUTIONER },
    { BOON_LAST_STAND, BOON_VIGOR }, { BOON_BLOODTHIRST, BOON_MOON_MASTERY }, { BOON_VANISH, BOON_UNBROKEN },
};
static int rival_of(int b)
{
    for (unsigned i = 0; i < sizeof RIVALS / sizeof RIVALS[0]; i++) {
        if (RIVALS[i][0] == b) return RIVALS[i][1];
        if (RIVALS[i][1] == b) return RIVALS[i][0];
    }
    return -1;
}
// The unlock ladder: the nth pack bought costs the nth price, whichever family
static const int PACK_COST[FAM_COUNT] = { 120, 160, 200, 240, 280, 320 };

#ifdef HLE_LOG_BOONFX       // test aid: each new boon's effect as it fires
#define BFX(...) debugf("[boonfx] " __VA_ARGS__)
#else
#define BFX(...) do { } while (0)
#endif

// ---- Pacts: a blessing and a curse, rolled for a card; stacks without end, so every draft has
// three cards. The best blessings (Might, Bulwark) with a curse come out a little ahead, the
// others about even: whether to take one or Skip depends on the build
enum { PB_MIGHT, PB_BULWARK, PB_VITALITY, PB_WOUNDED, PB_STAGGERED, PB_LAST_BREATH, PB_COUNT };
enum { PC_THIRST, PC_FRAILTY, PC_WEAKNESS, PC_EXPOSED, PC_IMPAIRED, PC_COUNT };
static const struct { const char *name, *desc; } BLESSINGS[PB_COUNT] = {
    [PB_MIGHT]       = { "Might",         "+8% damage" },
    [PB_BULWARK]     = { "Bulwark",       "-6% damage taken" },
    [PB_VITALITY]    = { "Vitality",      "+12 max health" },
    [PB_WOUNDED]     = { "Wounded Foe",   "+12% damage boss < 50%" },
    [PB_STAGGERED]   = { "Breaker",       "+15% damage on staggers" },
    [PB_LAST_BREATH] = { "Last Breath",   "+15% damage you < 30%" },
};
static const struct { const char *name, *desc; } CURSES[PC_COUNT] = {
    [PC_THIRST]   = { "Thirst",   "-1 potion" },
    [PC_FRAILTY]  = { "Frailty",  "+5% damage taken" },
    [PC_WEAKNESS] = { "Weakness", "-5% damage dealt" },
    [PC_EXPOSED]  = { "Exposed",  "+15% taken after rolls" },
    [PC_IMPAIRED] = { "Impaired", "3% of hits miss" },
};
#ifndef PACT_CHANCE             // (a test aid may set it)
#define PACT_CHANCE 4           // % a card is a pact while boons are left (legendary: 6% and up)
#endif
#define STORM_BOLT  6.0f        // Storm Blade's lightning (+12 was best in every build: section 14 of the design)
#define STATIC_BOLT 3.0f        // Static: a level
#define PACT_CARD(b, c) (BOON_COUNT + (b) * PC_COUNT + (c))
#define SKIP_EMBERS 5

// ---- Nights (from the second circle) ----
typedef enum { NIGHT_NONE, NIGHT_BLOOD_MOON, NIGHT_IRON, NIGHT_STARFALL, NIGHT_COUNT } Night;
static const struct { const char *name, *desc; float embers; } NIGHTS[NIGHT_COUNT] = {
    [NIGHT_NONE]       = { "", "", 1.0f },
    [NIGHT_BLOOD_MOON] = { "Blood Moon", "Bosses hit 20% harder", 1.5f },
    [NIGHT_IRON]       = { "Iron Night", "Bosses +25% health", 1.3f },
    [NIGHT_STARFALL]   = { "Starfall",   "Skills recharge 30% faster; bosses hit 10% harder", 1.2f },
};

// ---- Relics: two of these effects each ----
// Plain relics are two blessings, pacts a strong blessing with a curse (a 4-bit index each; RELICS below)
enum { RELIC_NONE, RELIC_DAMAGE, RELIC_WARD, RELIC_FLASK, RELIC_HEALTH, RELIC_STAMINA, RELIC_EMBERS, RELIC_SKILL,
       RELIC_FURY, RELIC_AVARICE, RELIC_BLESSED, RELIC_VAMPIRIC,         // the pacts' blessings
       RELIC_FRAIL, RELIC_PARCHED, RELIC_LEADEN, RELIC_DREAD,            // and their curses
       RELIC_FX_COUNT };
static const struct { const char *adj, *desc; } RELIC_FX[RELIC_FX_COUNT] = {
    [RELIC_DAMAGE]  = { "Keen",    "+6% damage" },
    [RELIC_WARD]    = { "Warding", "-6% damage taken" },
    [RELIC_FLASK]   = { "Brimming","+2 potions" },
    [RELIC_HEALTH]  = { "Hale",    "+15 max health" },
    [RELIC_STAMINA] = { "Swift",   "+20% stamina regen" },
    [RELIC_EMBERS]  = { "Gilded",  "+20% Embers" },
    [RELIC_SKILL]   = { "Sage",    "Start knowing a skill" },
    [RELIC_FURY]    = { "Wrathful",    "+15% damage" },
    [RELIC_AVARICE] = { "Avaricious",  "+40% Embers" },
    [RELIC_BLESSED] = { "Blessed",     "Start with a boon" },
    [RELIC_VAMPIRIC]= { "Vampiric",    "Heal 3% of damage dealt" },
    [RELIC_FRAIL]   = { "Frail",       "-20 max health" },
    [RELIC_PARCHED] = { "Parched",     "-1 potion" },
    [RELIC_LEADEN]  = { "Leaden",      "Rolls cost 25% more" },
    [RELIC_DREAD]   = { "Dreaded",     "Bosses hit 10% harder" },
};
#define NIGHTFARER_COST 450

// The relics, bought at the Shrine (one equipped for a climb): four of two blessings that work
// together, and a pact whose curse is the price of its blessing
#define R(a, b) (uint8_t)(((a) << 4) | (b))
static const struct { uint8_t fx; const char *name; } RELICS[] = {
    { R(RELIC_DAMAGE, RELIC_HEALTH),  "Knight's Oath" },     // an all-rounder
    { R(RELIC_WARD, RELIC_FLASK),     "Saint's Chalice" },   // to last
    { R(RELIC_SKILL, RELIC_STAMINA),  "Moonlit Tome" },      // a skill from the start, the stamina to use it
    { R(RELIC_EMBERS, RELIC_DAMAGE),  "Ember Crown" },       // for Embers
    { R(RELIC_FURY, RELIC_FRAIL),     "Brand of Wrath" },    // glass cannon
};
#undef R
#define RELIC_COUNT ((int)(sizeof RELICS / sizeof RELICS[0]))
#define RELIC_COST  200
#define CIRCLE_EMBERS 25        // a circle cleared

// ---- The map (docs/design/TOWER_ROGUELIKE.md section 2): 7 rows of 3 lanes a circle, climbed bottom to
// top, each node linked to the one above it and its diagonal neighbours. Row 1 is the Guardian and row 7
// Guardian Unbound (one node each), row 6 a Rest on every lane; rows 2-5 are rolled (map_generate)
#define MAP_ROWS  7
#define MAP_LANES 3
enum { NODE_NONE, NODE_FIGHT, NODE_ELITE, NODE_EVENT, NODE_SHOP, NODE_REST, NODE_BOSS, NODE_TYPES };
// Rows 2-5's odds (%). Events and Shops are stage 4: until then none are rolled (the design's 20 and 12)
static const uint8_t NODE_ODDS[NODE_TYPES] = { [NODE_FIGHT] = 45, [NODE_ELITE] = 15, [NODE_EVENT] = 0, [NODE_SHOP] = 0, [NODE_REST] = 8 };
static const char *const NODE_NAMES[NODE_TYPES] = { "", "Fight", "Elite", "Event", "Shop", "Rest", "Boss" };
static const char *const BOSS_NAMES[] = { "Guardian", "Guardian Unbound", "Fallen Warrior", "Shackled Sun", "Shadowborne" };
#define ELITE_HP      1.25f     // an Elite: the boss at x1.25 health (its affixes are stage 6)
#define ELITE_EMBERS  1.5f
#define REST_HEAL     0.40f     // a Rest: heal 40%, Temper +1 level, or +2 potions (and Release a boon, free)
#define REST_POTIONS  2         // up to POTION_CAP: fights already bank potions past the run's max (section 12)
enum { SCR_NONE, SCR_MAP, SCR_REST, SCR_TEMPER, SCR_RELEASE };
enum { REST_HEAL_ROW, REST_TEMPER_ROW, REST_POTIONS_ROW, REST_ROWS };
#define LIST_MAX 12

typedef struct { const char *name, *desc; } UpgradeDef;

static const UpgradeDef UPGRADES[UPG_COUNT] = {
    [UPG_FLASK_CAPACITY] = { "Flask Capacity", "+1 potion" },
    [UPG_FLASK_POTENCY]  = { "Flask Potency",  "Potions heal 8% more" },
    [UPG_HONED_BLADE]    = { "Honed Blade",    "+8% damage" },
    [UPG_VIGOR]          = { "Vigor",          "+10 max health" },
    [UPG_SKILL_MASTERY]  = { "Skill Mastery",  "Skill cooldowns -8%" },
};
// Everything (the five upgrades to 3, the five relics, the Nightfarer) is 2400 Embers: about 6.5
// hours of climbing (2.5 min a floor, runs reaching floor 4 at first and 16 with it all), 7.5 with
// the story and Bonus bosses
static const int UPGRADE_COST[UPG_MAX_LEVEL] = { 30, 60, 100 };

static struct {
    bool active;
    int floor, circle;              // a floor is a fight; a circle, a map
    uint32_t rng;
    struct {
        uint8_t type[MAP_ROWS][MAP_LANES];  // a NODE_ (rows 1 and 7 only in the middle lane)
        uint8_t boss[MAP_ROWS][MAP_LANES];  // a Fight's or Elite's boss (TowerBoss + 1), set once it's reachable
        int8_t path[MAP_ROWS];              // the lane taken in each row (-1: not yet)
        int8_t row, sel;                    // the row of the node last entered (-1: none yet), the cursor's lane in the next
        uint8_t met;                        // the bosses met this circle (bit per TowerBoss)
        uint8_t fights;                     // fights entered this circle
    } map;
    TowerBoss boss;                 // the fight under way (or the next: the Guardian at a circle's start)
    bool elite;                     // it's an Elite (x1.25 health, a Rare-or-better draft, x1.5 Embers)
    uint8_t scr;                    // the screen after the draft: the map, a Rest's (SCR_)
    uint32_t scrOpenMs;
    int8_t list[LIST_MAX], listN, listSel;  // a Rest screen's rows: a choice, a BoonId to Temper (-1 Back), a slot to Release (-1 keep)
    uint8_t skills;                 // learned this run (bit per CharacterSkill)
    uint8_t skillsOffered;          // whose Learn card has been on the table (taken or not)
    uint8_t stacks[BOON_COUNT];     // a boon's level (0: not held)
    uint8_t slot[TOWER_SLOTS];      // the boon slots in the order taken (BoonId + 1, 0 empty); skill cards take none
    uint16_t packs;                 // the families' packs bought at the Shrine (bit per family), for this run
    // the new boons' timers and counters
    double sanguineUntil, waxingUntil, shadowEdgeUntil;
    float bleedLeft, bleedRate, bleedTick;   // Hemorrhage: damage still to bleed, a second, the next tick
    int overcharge;                 // Overcharge: hits left that call lightning
    bool locked[BOON_COUNT];        // rivals locked out for the run (their pair was taken)
    uint8_t merged[TOWER_SLOTS][2]; // a duo's slot: the two boons merged into it (BoonId + 1)
    double mercyUntil, steelUntil, drainHoldUntil;   // Shade's Mercy's window, Steel Hearth's ward, Crushing Blows+
    float steelHp;                  // Steel Hearth: max health added this floor
    bool ironFirstUsed, skyfallUsed;    // Iron Skin+ this floor, Skyfall this cast
    double boltAt[4]; float boltDmg[4]; // bolts still to fall (Thunderstep, Skyfall)
    int perfectDodges;              // this floor (Vanish)
    bool vanishArmed, vanishUsed;
    int runEmbers, lastFloorEmbers;
    bool unbrokenUsed;
    Night night, nextNight;
    uint8_t relicFx[2];             // the equipped relic's effects, for this run
    bool circleCleared;            // this floor ended a circle (its Ember bonus)
    int hitCount;                   // sword hits landed (Storm Blade)
    float rally, rallyHold, rallyRate;  // Rally: health to win back, its hold, then its drain a second
    double riposteUntil;
    uint16_t bless[PB_COUNT], curse[PC_COUNT];   // pacts taken
    int potionDebt;                 // Thirst with no potion: the next bosses' potions are owed
    double exposedUntil;            // Exposed: after a roll
    // draft
    bool draftOpen;
    uint32_t draftOpenMs;           // when it opened (input waits DRAFT_INPUT_DELAY_MS)
    int choices[DRAFT_CHOICES];     // a BoonId, or PACT_CARD
    int nChoices, sel;
} s;

static int slots_used(void)
{
    int n = 0;
    for (int i = 0; i < TOWER_SLOTS; i++) n += s.slot[i] != 0;
    return n;
}

static bool is_skill_card(BoonId b) { return BOONS[b].family == FAM_SKILL; }
static bool takes_slot(BoonId b) { return !is_skill_card(b) && !BOONS[b].up && !BOONS[b].duo; }   // ("+" none, a duo merges two)

// A family's boons are all in the pool: its pack bought, or it has none (Hearth's is held back)
static bool family_open(int f)
{
    if ((s.packs >> f) & 1) return true;
    for (int b = 0; b < BOON_COUNT; b++) if (BOONS[b].pack && BOONS[b].family == f) return false;
    return true;
}

// The two slots a duo would merge: the first maxed boon of each of its families, slotted
// (not already in a duo); false if there's no such pair
static bool duo_pair(BoonId d, int *sa, int *sb)
{
    *sa = *sb = -1;
    for (int i = 0; i < TOWER_SLOTS; i++) {
        if (!s.slot[i] || s.merged[i][0]) continue;
        BoonId b = (BoonId)(s.slot[i] - 1);
        if (s.stacks[b] < BOONS[b].maxStacks) continue;
        if (*sa < 0 && BOONS[b].family == BOONS[d].family) *sa = i;
        else if (*sb < 0 && BOONS[b].family == BOONS[d].duo - 1) *sb = i;
    }
    return *sa >= 0 && *sb >= 0;
}

static void sync_max_health(void)
{
    if (s.active) character.maxHealth = tower_mod_max_health();     // (before, Vigor and Vitality raised only the health)
}

// A boon taken (a draft, a relic's gift): a new one fills the next free slot at Lv 1, a held one
// levels up; skill cards and "+" take no slot; a duo merges its two boons into the first one's
// slot (both stay at their max) and opens the other. Its rival is locked out for the run
static void take_boon(BoonId b)
{
    int sa, sb;
    if (BOONS[b].duo && !s.stacks[b] && duo_pair(b, &sa, &sb)) {
        s.merged[sa][0] = s.slot[sa]; s.merged[sa][1] = s.slot[sb];
        s.slot[sa] = (uint8_t)(b + 1); s.slot[sb] = 0;
    } else if (takes_slot(b) && !s.stacks[b])
        for (int i = 0; i < TOWER_SLOTS; i++) if (!s.slot[i]) { s.slot[i] = (uint8_t)(b + 1); break; }
    if (rival_of(b) >= 0) s.locked[rival_of(b)] = true;
    if (s.stacks[b] < BOONS[b].maxStacks) s.stacks[b]++;
    if (b == BOON_DEEP_FLASK) character.healthPotions = character.healthPotions + 3 > POTION_CAP ? POTION_CAP : character.healthPotions + 3;
    if (b == BOON_LEARN_MOONBLADE) s.skills |= 1 << SKILL_MOON_BLADE_DANCE;
    if (b == BOON_LEARN_HALFMOON)  s.skills |= 1 << SKILL_HALFMOON_SLASH;
    if (b == BOON_LEARN_CLEAVE)    s.skills |= 1 << SKILL_CRESCENT_CLEAVE;
    if (b == BOON_VIGOR) character.health += 25.0f;
    sync_max_health();
}

static uint32_t rng_next(void)
{
    // xorshift32
    uint32_t x = s.rng ? s.rng : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s.rng = x;
    return x;
}

// ---- The map ----

static bool lone_row(int r) { return r == 0 || r == MAP_ROWS - 1; }
// row r's lane a links to row r + 1's lane b: straight up or a diagonal (a lone node links every lane)
static bool linked(int r, int a, int b) { return lone_row(r) || lone_row(r + 1) || abs(a - b) <= 1; }
static bool is_fight(int t) { return t == NODE_FIGHT || t == NODE_ELITE || t == NODE_BOSS; }

static int roll_node(void)
{
    int total = 0;
    for (int t = 0; t < NODE_TYPES; t++) total += NODE_ODDS[t];
    int r = rng_next() % total;
    for (int t = 0; t < NODE_TYPES; t++) if ((r -= NODE_ODDS[t]) < 0) return t;
    return NODE_FIGHT;
}

// Rows 2-5 (1-4 here) against section 2's rules
static bool map_rules_ok(void)
{
    bool shop = false;
    for (int l = 0; l < MAP_LANES; l++) {
        int f = 0;
        for (int r = 1; r <= 4; r++) f += s.map.type[r][l] == NODE_FIGHT;
        if (f < 2 || s.map.type[1][l] == NODE_ELITE) return false;     // 2 Fights a lane or more; no Elite in row 2
        shop |= s.map.type[2][l] == NODE_SHOP || s.map.type[3][l] == NODE_SHOP;
    }
    if (NODE_ODDS[NODE_SHOP] && !shop) return false;                    // a Shop in rows 3-4 on some lane (stage 4)
    for (int r = 1; r <= 4; r++)        // no two Shops or Rests in a row on any path (row 6 is all Rests)
        for (int a = 0; a < MAP_LANES; a++) for (int b = 0; b < MAP_LANES; b++) {
            int t = s.map.type[r][a];
            if ((t == NODE_SHOP || t == NODE_REST) && linked(r, a, b) && s.map.type[r + 1][b] == t) return false;
        }
    return true;
}

static int s_mapTries;      // the last map's rolls (the self-check logs them)
// A circle's map: rows 2-5 rolled until they keep every rule (about 1 in 5 does)
static void map_generate(void)
{
    memset(&s.map, 0, sizeof s.map);
    s_mapTries = 0;
    do {
        s_mapTries++;
        for (int r = 1; r <= 4; r++) for (int l = 0; l < MAP_LANES; l++) s.map.type[r][l] = roll_node();
#ifdef HLE_TOWER_SB_LAST    // test aid: Fights only (with the bosses in order below: the Fallen Warrior, the Sun, then Shadowborne)
        for (int r = 1; r <= 4; r++) for (int l = 0; l < MAP_LANES; l++) s.map.type[r][l] = NODE_FIGHT;
#endif
        for (int l = 0; l < MAP_LANES; l++) s.map.type[5][l] = NODE_REST;
    } while (!map_rules_ok() && s_mapTries < 1000);
    if (!map_rules_ok()) for (int r = 1; r <= 4; r++) for (int l = 0; l < MAP_LANES; l++) s.map.type[r][l] = NODE_FIGHT;
    s.map.type[0][1] = s.map.type[MAP_ROWS - 1][1] = NODE_BOSS;
    s.map.row = -1;
    memset(s.map.path, -1, sizeof s.map.path);
}

// a node the cursor can go to: in the next row, linked to the node last entered
static bool map_reachable(int r, int l)
{
    if (r != s.map.row + 1 || r >= MAP_ROWS || !s.map.type[r][l]) return false;
    return s.map.row < 0 || linked(s.map.row, s.map.path[s.map.row], l);
}

// The next row's Fights and Elites get their boss once they're reachable: one not met this circle
// while any are left (a path with 4 in the middle meets one twice)
static void map_assign_bosses(void)
{
    int r = s.map.row + 1;
    for (int l = 0; l < MAP_LANES; l++) {
        int t = r < MAP_ROWS ? s.map.type[r][l] : NODE_NONE;
        if (!map_reachable(r, l) || (t != NODE_FIGHT && t != NODE_ELITE) || s.map.boss[r][l]) continue;
        int left[3], n = 0;
        for (int b = TOWER_FALLEN_KNIGHT; b <= TOWER_SHADOWBORN; b++) if (!((s.map.met >> b) & 1)) left[n++] = b;
        if (!n) for (int b = TOWER_FALLEN_KNIGHT; b <= TOWER_SHADOWBORN; b++) left[n++] = b;
#ifdef HLE_TOWER_SB_LAST    // (the heap's lowest point, the same path each run)
        s.map.boss[r][l] = (uint8_t)(left[0] + 1);
#else
        s.map.boss[r][l] = (uint8_t)(left[rng_next() % n] + 1);
#endif
    }
}

// A new circle: its map, its night (rolled when the last one was cleared)
static void new_circle(void)
{
    s.circle++;
    map_generate();
    s.boss = TOWER_GUARDIAN;
    s.elite = false;
    s.night = s.nextNight;
#ifdef HLE_AUTOPLAY
    debugf("[night] circle %d from floor %d: %s\n", s.circle, s.floor, NIGHTS[s.night].name[0] ? NIGHTS[s.night].name : "none");
    for (int r = MAP_ROWS - 2; r >= 1; r--)
        debugf("[map] circle %d row %d: %s %s %s\n", s.circle, r + 1, NODE_NAMES[s.map.type[r][0]], NODE_NAMES[s.map.type[r][1]], NODE_NAMES[s.map.type[r][2]]);
#endif
}

// ---- Relics: bought at the Shrine, one equipped for a climb ----

static bool has_relic(int fx) { return s.active && (s.relicFx[0] == fx || s.relicFx[1] == fx); }

// Saves before 1.0 kept up to three found relics (a byte of two effects each) and the equipped
// slot: each that is one of today's is owned now (and stays equipped), any other is refunded
static void tower_load(TowerSave *t)
{
    save_controller_get_tower(t);
    if (!(t->relics[0] | t->relics[1] | t->relics[2])) return;
    int e = ((t->flags >> 1) & 3) - 1;
    for (int i = 0; i < TOWER_RELIC_SLOTS; i++) {
        if (!t->relics[i]) continue;
        int id = -1;
        for (int k = 0; k < RELIC_COUNT; k++) if (RELICS[k].fx == t->relics[i]) id = k;
        if (id >= 0) { t->relicsOwned |= 1 << id; if (i == e) t->relicEquipped = id + 1; }
        else { int m = t->embers + RELIC_COST; t->embers = m > 65535 ? 65535 : m; }
        t->relics[i] = 0;
    }
    t->flags &= ~6;
    save_controller_set_tower(t);
}

int  tower_relic_total(void) { return RELIC_COUNT; }
const char* tower_relic_name(int id) { return (id >= 0 && id < RELIC_COUNT) ? RELICS[id].name : "None"; }
const char* tower_relic_desc(int id)
{
    static char desc[64];
    if (id < 0 || id >= RELIC_COUNT) return "";
    snprintf(desc, sizeof desc, "%s, %s", RELIC_FX[RELICS[id].fx >> 4].desc, RELIC_FX[RELICS[id].fx & 15].desc);
    return desc;
}
bool tower_relic_owned(int id) { TowerSave t; tower_load(&t); return id >= 0 && id < RELIC_COUNT && ((t.relicsOwned >> id) & 1); }
int  tower_relic_cost(int id) { return tower_relic_owned(id) ? -1 : RELIC_COST; }
int  tower_relic_equipped(void)
{
    TowerSave t; tower_load(&t);
    int e = t.relicEquipped - 1;
    return (e >= 0 && e < RELIC_COUNT && ((t.relicsOwned >> e) & 1)) ? e : -1;
}
bool tower_relic_select(int id)
{
    TowerSave t; tower_load(&t);
    if (id < 0) { t.relicEquipped = 0; return save_controller_set_tower(&t); }     // climb without one
    if (id >= RELIC_COUNT) return false;
    if (!((t.relicsOwned >> id) & 1)) {             // buy it
        if (t.embers < RELIC_COST) return false;
        t.embers -= RELIC_COST;
        t.relicsOwned |= 1 << id;
        t.relicEquipped = id + 1;
    } else {                                        // equip it, or take it off
        t.relicEquipped = t.relicEquipped == id + 1 ? 0 : id + 1;
    }
    return save_controller_set_tower(&t);
}

// ---- Nightfarer ----

bool tower_nightfarer_owned(void) { TowerSave t; tower_load(&t); return t.flags & TOWER_FLAG_NIGHTFARER; }
int  tower_nightfarer_cost(void) { return tower_nightfarer_owned() ? -1 : NIGHTFARER_COST; }
bool tower_nightfarer_buy(void)
{
    TowerSave t; tower_load(&t);
    if ((t.flags & TOWER_FLAG_NIGHTFARER) || t.embers < NIGHTFARER_COST) return false;
    t.embers -= NIGHTFARER_COST;
    t.flags |= TOWER_FLAG_NIGHTFARER;
    return save_controller_set_tower(&t);
}

// ---- Run ----

void tower_begin_run(uint32_t seed)
{
    memset(&s, 0, sizeof(s));
    s.active = true;
    s.floor = 1;
    s.rng = seed ? seed : 1;
    { TowerSave t; tower_load(&t); s.packs = t.unlocks & ((1 << FAM_COUNT) - 1); }
#ifdef HLE_TOWER_PACKS      // test aid: climb with these packs (bit per family) whatever the save holds
    s.packs = HLE_TOWER_PACKS;
#endif
#ifdef HLE_TOWER_SEED       // test aid: a set run seed (the map, the drafts)
    s.rng = HLE_TOWER_SEED;
#endif
    new_circle();
#if defined(HLE_TEST_TOWER_STATS) && !defined(HLE_STATS_BARE)   // test aid (BARE: as the run starts): a full bar (two of a family, two new boons, all at their most), every skill and its cards, every pact twice
    {
        static const BoonId FULL[TOWER_SLOTS] = { BOON_BLOODTHIRST, BOON_RALLY, BOON_PHANTOM_STEP, BOON_STATIC, BOON_EMBER_EDGE };
        for (int i = 0; i < TOWER_SLOTS; i++) while (s.stacks[FULL[i]] < BOONS[FULL[i]].maxStacks) take_boon(FULL[i]);
        s.stacks[BOON_RALLY] = 1;       // (one below its max: the page shows "Lv 1/2")
        s.skills = (1 << SKILL_COUNT) - 1;
        s.stacks[BOON_HALFMOON_PLUS] = s.stacks[BOON_CRESCENT_ECHO] = 1;
    }
    for (int b = 0; b < PB_COUNT; b++) s.bless[b] = 2;
    for (int c = 0; c < PC_COUNT; c++) s.curse[c] = 2;
#endif
#ifdef HLE_SDMG_TOWER   // test aid (HLE_TEST_SKILL_DMG): every skill known, both damage cards held
    s.skills = (1 << SKILL_COUNT) - 1;
    s.stacks[BOON_HALFMOON_PLUS] = s.stacks[BOON_CLEAVE_PLUS] = 1;
#endif
#ifdef HLE_TOWER_GIVE   // test aid: start the run holding this boon (a "+": its boon maxed first; a duo: a maxed boon of
                        // each family merged into it, and every skill, so its trigger can fire)
    {
        BoonId g = HLE_TOWER_GIVE;
        if (BOONS[g].up) while (s.stacks[BOONS[g].up - 1] < BOONS[BOONS[g].up - 1].maxStacks) take_boon((BoonId)(BOONS[g].up - 1));
        if (BOONS[g].duo) {
            static const BoonId REP[FAM_COUNT] = { BOON_BLOODTHIRST, BOON_STORM_BLADE, BOON_MOON_MASTERY, BOON_CRUSHING_BLOWS, BOON_RIPOSTE, BOON_KINDLED_FLASK };
            int fa = BOONS[g].family, fb = BOONS[g].duo - 1;
            BoonId a = (fa == FAM_BLOOD && fb == FAM_MOON) ? BOON_RALLY : REP[fa];     // (Bloodthirst and Moon Mastery are rivals)
            BoonId b = REP[fb];
            while (s.stacks[a] < BOONS[a].maxStacks) take_boon(a);
            while (s.stacks[b] < BOONS[b].maxStacks) take_boon(b);
            s.skills = (1 << SKILL_COUNT) - 1;
        }
        take_boon(g);
        debugf("[give] %s: slots %d %d %d %d %d, skills %x\n", BOONS[g].name, s.slot[0], s.slot[1], s.slot[2], s.slot[3], s.slot[4], s.skills);
    }
#endif
#ifdef HLE_TOWER_GIVE_NEW   // test aid (HLE_LOG_BOONFX): every pack's boon at its top level (past the 5 slots) and every skill
    for (int b = BOON_HEMORRHAGE; b <= BOON_VANISH; b++) s.stacks[b] = BOONS[b].maxStacks;
    s.skills = (1 << SKILL_COUNT) - 1;
#endif
    // The equipped relic holds for the whole climb
    int e = tower_relic_equipped();
    uint8_t fx = e >= 0 ? RELICS[e].fx : 0;
#ifdef HLE_TOWER_RELIC      // test aid: climb with these relic effects (a * 16 + b)
    fx = HLE_TOWER_RELIC;
#endif
    if (fx) {
        s.relicFx[0] = fx >> 4; s.relicFx[1] = fx & 15;
        if (has_relic(RELIC_SKILL) && character_skills_enabled()) {
            int k = rng_next() % SKILL_COUNT;
            s.skills |= 1 << k;
            s.stacks[BOON_LEARN_MOONBLADE + k] = 1;     // held as its Learn card: Stats lists it (and so its upgrades make sense)
        }
        if (has_relic(RELIC_BLESSED)) {      // a boon that needs nothing applied on the spot
            static const BoonId GIFT[] = { BOON_EMBER_EDGE, BOON_IRON_SKIN, BOON_KINDLED_FLASK, BOON_SECOND_WIND,
                                           BOON_FLEETFOOT, BOON_BLOODTHIRST, BOON_CRUSHING_BLOWS, BOON_RIPOSTE, BOON_RALLY };
            take_boon(GIFT[rng_next() % (sizeof GIFT / sizeof GIFT[0])]);
        }
#ifdef HLE_AUTOPLAY
        int gift = -1;
        for (int b = 0; b < BOON_COUNT; b++) if (s.stacks[b]) gift = b;
        debugf("[relic] climbing with %s / %s: skills %x, gift %s, max health %.0f, potions %d, damage x%.2f, taken x%.2f, roll x%.2f, lifesteal %.2f\n",
               RELIC_FX[s.relicFx[0]].adj, RELIC_FX[s.relicFx[1]].adj, s.skills, gift >= 0 ? BOONS[gift].name : "none",
               (double)tower_mod_max_health(), tower_mod_max_potions(), (double)tower_mod_damage_dealt(), (double)tower_mod_damage_taken(),
               (double)tower_mod_roll_cost(), (double)tower_mod_lifesteal());
#endif
    }
}

static void icons_free(void);
void tower_end_run(void) { s.active = false; s.draftOpen = false; s.scr = SCR_NONE; icons_free(); }
bool tower_active(void) { return s.active; }
int  tower_floor(void) { return s.floor; }
int  tower_circle(void) { return s.circle; }
TowerBoss tower_floor_boss(void) { return s.boss; }
int  tower_run_embers(void) { return s.runEmbers; }
int  tower_boon_stacks(BoonId b) { return (s.active && b < BOON_COUNT) ? s.stacks[b] : 0; }

static int floor_embers(void)
{
    // (the fight's place in its circle: 0 for the Guardian, up to 5 for Unbound on a 6-fight path)
    float e = (8 * tower_circle() + 3 * (s.map.fights - 1)) * NIGHTS[s.night].embers * (s.elite ? ELITE_EMBERS : 1.0f);
    if (has_relic(RELIC_EMBERS)) e *= 1.2f;
    if (has_relic(RELIC_AVARICE)) e *= 1.4f;
    return (int)(e + 0.5f);
}

void tower_floor_cleared(void)
{
    if (!s.active) return;
    TowerSave t;
    tower_load(&t);
    s.circleCleared = s.map.row == MAP_ROWS - 1;       // Guardian Unbound fell
    s.lastFloorEmbers = floor_embers() + (s.circleCleared ? CIRCLE_EMBERS : 0);
    s.runEmbers += s.lastFloorEmbers;
    int e = t.embers + s.lastFloorEmbers;
    t.embers = e > 65535 ? 65535 : e;
    if (s.floor > t.bestFloor) t.bestFloor = s.floor > 255 ? 255 : s.floor;
    save_controller_set_tower(&t);
    if (s.circleCleared) {      // a circle cleared: its bonus (above), and the next circle's night
        s.nextNight = (Night)(1 + rng_next() % (NIGHT_COUNT - 1));
    }
    tower_draft_open();
}

void tower_next_floor(void)
{
    s.floor++;
    s.rally = 0.0f;
    s.unbrokenUsed = false;
    s.bleedLeft = 0.0f; s.overcharge = 0;
    s.perfectDodges = 0; s.vanishArmed = s.vanishUsed = false;
    s.ironFirstUsed = false; s.steelHp = 0.0f; s.steelUntil = s.mercyUntil = s.drainHoldUntil = 0.0;
    for (int i = 0; i < 4; i++) s.boltAt[i] = 0.0;
    sync_max_health();      // (Steel Hearth's is for the floor)
    if (character.health > character.maxHealth) character.health = character.maxHealth;
    if (s.map.row == MAP_ROWS - 1) new_circle();     // its map shows before the Guardian
}

// ---- Draft ----

static bool knows(int skill) { return (s.skills >> skill) & 1; }

// Can it be offered now? (stacks left, and an upgrade needs its skill)
// A new boon needs a free slot and, from a family's pack, the pack bought: with the bar full
// a draft offers only levels of the slotted boons (and skill cards and pacts)
// A rival is never offered once its pair is taken; a "+" only with its boon at its max; a duo only
// with a maxed boon of each of its families slotted (and both families' packs bought)
static bool offerable(BoonId b)
{
    if (s.stacks[b] >= BOONS[b].maxStacks || s.locked[b]) return false;
    if (is_skill_card(b) && !character_skills_enabled()) return false;     // Skills off (the title's setting): none offered
    if (BOONS[b].up) { BoonId u = (BoonId)(BOONS[b].up - 1); return s.stacks[u] >= BOONS[u].maxStacks; }
    if (BOONS[b].duo) { int sa, sb; return family_open(BOONS[b].family) && family_open(BOONS[b].duo - 1) && duo_pair(b, &sa, &sb); }
    if (!is_skill_card(b) && !s.stacks[b]) {
        if (slots_used() >= TOWER_SLOTS) return false;
        if (BOONS[b].pack && !((s.packs >> BOONS[b].family) & 1)) return false;
    }
    switch (b) {
    case BOON_MOON_MASTERY: case BOON_LUNAR_TIDE: case BOON_ECLIPSE: case BOON_WAXING_MOON:   // the skill boons need a skill
        return s.skills != 0 && character_skills_enabled();
    case BOON_CONDUCTOR:     // something that calls lightning
        return s.stacks[BOON_STORM_BLADE] || s.stacks[BOON_STATIC] || s.stacks[BOON_THUNDERCLAP] || s.stacks[BOON_OVERCHARGE];
    case BOON_CRESCENT_ECHO: return knows(SKILL_MOON_BLADE_DANCE);
    case BOON_HALFMOON_PLUS: return knows(SKILL_HALFMOON_SLASH);
    case BOON_CLEAVE_PLUS:   return knows(SKILL_CRESCENT_CLEAVE);
    // never a skill already known (the Sage relic teaches one without its card)
    case BOON_LEARN_MOONBLADE: return !knows(SKILL_MOON_BLADE_DANCE);
    case BOON_LEARN_HALFMOON:  return !knows(SKILL_HALFMOON_SLASH);
    case BOON_LEARN_CLEAVE:    return !knows(SKILL_CRESCENT_CLEAVE);
    default: return true;
    }
}

void tower_draft_open(void)
{
    // Three different boons; the first a skill card (learn or upgrade) while any are left
    BoonId pool[BOON_COUNT], skills[BOON_COUNT]; int n = 0, ns = 0;
    int famHeld[FAM_COUNT + 1] = { 0 };     // Hades' focus: a family with 2+ boons slotted comes up twice as often
    for (int i = 0; i < TOWER_SLOTS; i++) {
        if (s.merged[i][0]) { famHeld[BOONS[s.merged[i][0] - 1].family]++; famHeld[BOONS[s.merged[i][1] - 1].family]++; }
        else if (s.slot[i]) famHeld[BOONS[s.slot[i] - 1].family]++;
    }
    for (int b = 0; b < BOON_COUNT; b++) {
        if (!offerable(b) || (s.elite && BOONS[b].tier == TIER_COMMON)) continue;   // an Elite's draft: Rare or better
        if (is_skill_card(b)) skills[ns++] = b; else pool[n++] = b;
    }
    s.nChoices = 0;
    if (ns > 0) {
        int k = rng_next() % ns;
        s.choices[s.nChoices++] = skills[k];
        skills[k] = skills[--ns];
        for (int i = 0; i < ns; i++) pool[n++] = skills[i];   // the rest may still turn up
    }
    // The rest by rarity: legendary 6% on the first circle, +4% a circle (up to 25%); rare 30%.
    // A pact now and then, and always once the boons run out
    while (s.nChoices < DRAFT_CHOICES) {
        if (n == 0 || (int)(rng_next() % 100) < PACT_CHANCE) {
            int card = 0;
            for (int tries = 0; tries < 8; tries++) {       // a pact not on the table yet
                int b = rng_next() % PB_COUNT, c;
                do c = rng_next() % PC_COUNT;               // never a curse that undoes its blessing
                while ((b == PB_BULWARK && c == PC_FRAILTY) || (b == PB_MIGHT && c == PC_WEAKNESS));
#ifdef HLE_PACT_CURSE       // test aid: every pact with this curse
                c = HLE_PACT_CURSE;
#endif
                card = PACT_CARD(b, c);
                bool dup = false;
                for (int i = 0; i < s.nChoices; i++) dup |= s.choices[i] == card;
                if (!dup) break;
            }
            s.choices[s.nChoices++] = card;
            continue;
        }
        int leg = 6 + 4 * (tower_circle() - 1), r = rng_next() % 100;
        if (leg > 25) leg = 25;
        int want = r < leg ? TIER_LEGENDARY : (r < leg + 30 || s.elite) ? TIER_RARE : TIER_COMMON;
        int k = -1;
        for (int tries = 0; tries < 3 && k < 0; tries++) {     // that tier, or the next lower one (wrapping)
            int tier = (want + 3 - tries) % 3, cand[2 * BOON_COUNT], nc = 0;
            for (int i = 0; i < n; i++) if (BOONS[pool[i]].tier == tier) {
                cand[nc++] = i;
                int f = BOONS[pool[i]].family;
                if (f < FAM_COUNT && famHeld[f] >= 2) cand[nc++] = i;
            }
            if (nc) k = cand[rng_next() % nc];
        }
        s.choices[s.nChoices++] = pool[k];
        int rv = rival_of(pool[k]);
        pool[k] = pool[--n];
        for (int i = 0; i < n; i++) if (pool[i] == rv) { pool[i] = pool[--n]; break; }     // never both rivals on the table
    }
#ifdef HLE_DRAFT_GALLERY     // test aid: each floor shows set cards (every boon, the longest pacts) for a look
    {
        static const int G[2][3] = {
            { PACT_CARD(PB_WOUNDED, PC_EXPOSED), PACT_CARD(PB_LAST_BREATH, PC_WEAKNESS), PACT_CARD(PB_STAGGERED, PC_IMPAIRED) },
            { PACT_CARD(PB_MIGHT, PC_THIRST), PACT_CARD(PB_BULWARK, PC_EXPOSED), PACT_CARD(PB_VITALITY, PC_FRAILTY) },
        };
#ifndef HLE_GALLERY_FROM
#define HLE_GALLERY_FROM 0      // the page the first floor shows (later pages: the "+" and duo cards)
#endif
        int g = (s.floor - 1 + HLE_GALLERY_FROM) % (2 + (BOON_COUNT + 2) / 3);     // the pacts, then every boon three at a time
        for (int i = 0; i < 3; i++) s.choices[i] = g < 2 ? G[g][i] : (3 * (g - 2) + i) % BOON_COUNT;
        s.nChoices = 3;
    }
#endif
    for (int i = 0; i < s.nChoices; i++) {
        if (s.choices[i] == BOON_LEARN_MOONBLADE) s.skillsOffered |= 1 << SKILL_MOON_BLADE_DANCE;
        if (s.choices[i] == BOON_LEARN_HALFMOON)  s.skillsOffered |= 1 << SKILL_HALFMOON_SLASH;
        if (s.choices[i] == BOON_LEARN_CLEAVE)    s.skillsOffered |= 1 << SKILL_CRESCENT_CLEAVE;
    }
    s.sel = 0;
    s.draftOpen = true;
    s.draftOpenMs = get_ticks_ms();
#ifdef HLE_AUTOPLAY
    debugf("[draft] floor %d:", s.floor);
    for (int i = 0; i < s.nChoices; i++) {
        int c = s.choices[i];
        if (c < BOON_COUNT) debugf(" %s (%d)", BOONS[c].name, BOONS[c].tier);
        else debugf(" Pact of %s / %s", BLESSINGS[(c - BOON_COUNT) / PC_COUNT].name, CURSES[(c - BOON_COUNT) % PC_COUNT].name);
    }
    debugf("%s%s\n", s.circleCleared ? " | circle cleared" : "", s.elite ? " | elite" : "");
#endif
}

bool tower_draft_is_open(void) { return s.draftOpen; }
int  tower_draft_selected(void) { return s.sel; }
int  tower_draft_card(int i) { return (i >= 0 && i < s.nChoices && s.choices[i] < BOON_COUNT) ? s.choices[i] : -1; }   // a boon, -1 a pact

void tower_draft_pick(int index)
{
    if (index == s.nChoices) {          // Skip: a few Embers instead
        TowerSave t; tower_load(&t);
        int e = t.embers + SKIP_EMBERS; t.embers = e > 65535 ? 65535 : e;
        save_controller_set_tower(&t);
        s.runEmbers += SKIP_EMBERS;
    } else if (index >= 0 && index < s.nChoices && s.choices[index] >= BOON_COUNT) {
        int b = (s.choices[index] - BOON_COUNT) / PC_COUNT, c = (s.choices[index] - BOON_COUNT) % PC_COUNT;
        s.bless[b]++; s.curse[c]++;
        if (b == PB_VITALITY) { character.health += 12.0f; sync_max_health(); }
#ifdef HLE_PACT_CURSE
        character.healthPotions = 0;      // test aid: no potion to give up
#endif
        if (c == PC_THIRST) { if (character.healthPotions > 0) character.healthPotions--; else s.potionDebt++; }
#ifdef HLE_AUTOPLAY
        debugf("[pact] took %s (%d) / %s (%d): damage x%.2f, taken x%.2f, max health %.0f, potions %d (owed %d)\n", BLESSINGS[b].name, s.bless[b],
               CURSES[c].name, s.curse[c], (double)tower_mod_damage_dealt(), (double)tower_mod_damage_taken(), (double)tower_mod_max_health(),
               character.healthPotions, s.potionDebt);
#endif
    } else if (index >= 0 && index < s.nChoices) {
        take_boon((BoonId)s.choices[index]);
    }
    s.draftOpen = false;
}

#define DRAFT_INPUT_DELAY_MS 750

static bool tower_draft_update(bool left, bool right, bool up, bool down, bool confirm)
{
    if (!s.draftOpen) return false;
    // A dodge (A) pressed as the cards come up took the first one unseen (always a skill card while
    // any are left): no input until they've been on screen a moment
    if (get_ticks_ms() - s.draftOpenMs < DRAFT_INPUT_DELAY_MS) return false;
    int n = s.nChoices + 1;             // the cards, then Skip (below them: Down, Up back)
    static int lastCard;
    if (s.sel < s.nChoices) lastCard = s.sel;
    if (down) s.sel = s.nChoices;
    else if (up && s.sel == s.nChoices) s.sel = lastCard < s.nChoices ? lastCard : 0;
    else if (left)  s.sel = (s.sel + n - 1) % n;
    else if (right) s.sel = (s.sel + 1) % n;
    if (confirm) { tower_draft_pick(s.sel); return true; }
    return false;
}

static int text_style(int x, int y, int w, rdpq_align_t align, int style, const char *str)
{
    return rdpq_text_printf(&(rdpq_textparms_t){ .align = align, .width = w, .wrap = WRAP_WORD, .style_id = style }, FONT_UNBALANCED, x, y, "%s", str).nlines;
}

static void text(int x, int y, int w, rdpq_align_t align, const char *str) { text_style(x, y, w, align, FONT_STYLE_WHITE, str); }

static void flat_mode(void)
{
    rdpq_sync_pipe();
    rdpq_set_mode_standard();
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
}

static void tower_draft_draw(void)
{
    if (!s.draftOpen) return;
    flat_mode();
    rdpq_set_prim_color(RGBA32(0, 0, 0, 170));
    rdpq_fill_rectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

    char line[80];
    snprintf(line, sizeof line, "Floor %d cleared   +%d Embers", s.floor, s.lastFloorEmbers);
    text_style(0, 26, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_GOLD, line);
    if (s.circleCleared) {
        snprintf(line, sizeof line, "Circle cleared: +%d Embers", CIRCLE_EMBERS);
        text_style(0, 40, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_BLUE, line);
    }
    text(0, 53, SCREEN_WIDTH, ALIGN_CENTER, "Choose a Boon");

    const int cw = 96, ch = 126, gap = 6, y0 = 60;     // (tall enough for a rival or a duo's two boons under the text)
    int x0 = (SCREEN_WIDTH - (cw * s.nChoices + gap * (s.nChoices - 1))) / 2;
    for (int i = 0; i < s.nChoices; i++) {
        int x = x0 + i * (cw + gap), card = s.choices[i];
        bool pact = card >= BOON_COUNT;
        int tier = pact ? 3 : BOONS[card].tier;
        flat_mode();
        bool on = i == s.sel;
        // The frame tells the rarity: bronze, blue, gold; a pact crimson (brighter when selected)
        static const color_t FRAME[4][2] = {
            { RGBA32(90, 70, 50, 255),  RGBA32(230, 150, 50, 255) },
            { RGBA32(50, 80, 150, 255), RGBA32(110, 170, 255, 255) },
            { RGBA32(150, 115, 30, 255), RGBA32(255, 215, 90, 255) },
            { RGBA32(120, 20, 30, 255), RGBA32(230, 50, 60, 255) },
        };
        rdpq_set_prim_color(FRAME[tier][on]);
        rdpq_fill_rectangle(x - 2, y0 - 2, x + cw + 2, y0 + ch + 2);
        rdpq_set_prim_color(on ? RGBA32(60, 30, 20, 240) : RGBA32(30, 22, 18, 230));
        rdpq_fill_rectangle(x, y0, x + cw, y0 + ch);
        if (pact) {
            int b = (card - BOON_COUNT) / PC_COUNT, c = (card - BOON_COUNT) % PC_COUNT;
            // the blessing (a line of name, two of text), then its curse (name, two of text)
            // the curse in a band of its own at the bottom (a line holds about 11 characters)
            flat_mode();
            rdpq_set_prim_color(on ? RGBA32(110, 16, 22, 240) : RGBA32(70, 12, 16, 230));
            rdpq_fill_rectangle(x, y0 + 60, x + cw, y0 + ch);
            text_style(x + 4, y0 + 14, cw - 8, ALIGN_CENTER, FONT_STYLE_GOLD, BLESSINGS[b].name);
            text(x + 6, y0 + 30, cw - 12, ALIGN_CENTER, BLESSINGS[b].desc);
            text_style(x + 4, y0 + 74, cw - 8, ALIGN_CENTER, FONT_STYLE_RED, CURSES[c].name);
            text_style(x + 6, y0 + 92, cw - 12, ALIGN_CENTER, FONT_STYLE_WHITE, CURSES[c].desc);
            continue;
        }
        const BoonDef *bd = &BOONS[card];
        static const int NAME_STYLE[3] = { FONT_STYLE_WHITE, FONT_STYLE_BLUE, FONT_STYLE_GOLD };
        // the family's tag over the name, in its colour (a duo's two families, gold)
        if (bd->duo) {
            snprintf(line, sizeof line, "%s+%s", FAMILIES[bd->family].name, FAMILIES[bd->duo - 1].name);
            text_style(x + 2, y0 + 12, cw - 4, ALIGN_CENTER, FONT_STYLE_GOLD, line);
        } else text_style(x + 4, y0 + 12, cw - 8, ALIGN_CENTER, FAMILIES[bd->family].style, FAMILIES[bd->family].name);
        int nameLines = text_style(x + 4, y0 + 27, cw - 8, ALIGN_CENTER, NAME_STYLE[bd->tier], bd->name);
        const char *desc = bd->desc;    // a Learn card names the skill's button in the layout
        if (card == BOON_LEARN_MOONBLADE || card == BOON_LEARN_HALFMOON || card == BOON_LEARN_CLEAVE) {
            snprintf(line, sizeof line, "Learn it: %s", joypad_skill_button(card - BOON_LEARN_MOONBLADE));
            desc = line;
        }
        text(x + 6, y0 + 31 + 17 * (nameLines > 1 ? nameLines : 1), cw - 12, ALIGN_CENTER, desc);     // under the name: 4 lines below one, 3 below two
        // the bottom lines, up from the card's foot: the slot line (what taking it does to the bar; a
        // duo's, the two boons it merges), and over it the rival it locks out
        char bottom[3][40]; int bstyle[3] = { FONT_STYLE_GREY, FONT_STYLE_GREY, FONT_STYLE_GREY }, nb = 1;
        int r = rival_of(card);
        if (bd->duo) {
            int sa, sb;
            const char *na = "", *nbn = "";
            if (duo_pair(card, &sa, &sb)) { na = BOONS[s.slot[sa] - 1].name; nbn = BOONS[s.slot[sb] - 1].name; }
#ifdef HLE_DRAFT_GALLERY    // test aid: no pair held, so each family's longest name (the text that must fit)
            else for (int b = 0; b < BOON_COUNT; b++) if (takes_slot(b)) {
                if (BOONS[b].family == bd->family && strlen(BOONS[b].name) > strlen(na)) na = BOONS[b].name;
                if (BOONS[b].family == bd->duo - 1 && strlen(BOONS[b].name) > strlen(nbn)) nbn = BOONS[b].name;
            }
#endif
            snprintf(bottom[0], 40, "%s", nbn); snprintf(bottom[1], 40, "%s", na); snprintf(bottom[2], 40, "Merges:"); nb = 3;     // each in its family's colour
            bstyle[0] = FAMILIES[bd->duo - 1].style; bstyle[1] = FAMILIES[bd->family].style;
        } else {
            if (bd->up) snprintf(bottom[0], 40, "+ (top level)");
            else if (bd->family == FAM_SKILL) snprintf(bottom[0], 40, "Skill: no slot");
            else if (s.stacks[card]) snprintf(bottom[0], 40, "Lv %d -> %d", s.stacks[card], s.stacks[card] + 1);
            else snprintf(bottom[0], 40, "New: slot %d/%d", slots_used() < TOWER_SLOTS ? slots_used() + 1 : TOWER_SLOTS, TOWER_SLOTS);
            if (r >= 0) { snprintf(bottom[1], 40, "%s", BOONS[r].name); snprintf(bottom[2], 40, "Locks out"); bstyle[1] = bstyle[2] = FONT_STYLE_RED; nb = 3; }
        }
        for (int k = 0; k < nb; k++) text_style(x - 1, y0 + ch - 3 - 12 * k, cw + 2, ALIGN_CENTER, bstyle[k], bottom[k]);
    }
    // Skip, under the cards
    // A button of its own under the cards, framed like them: Down to it, Up back to the card
    bool skip = s.sel == s.nChoices;
    const int bw = 132, bh = 16, bx = (SCREEN_WIDTH - bw) / 2, by = y0 + ch + 4;
    flat_mode();
    rdpq_set_prim_color(skip ? RGBA32(255, 215, 90, 255) : RGBA32(110, 100, 90, 255));
    rdpq_fill_rectangle(bx - 2, by - 2, bx + bw + 2, by + bh + 2);
    rdpq_set_prim_color(skip ? RGBA32(60, 30, 20, 240) : RGBA32(30, 22, 18, 230));
    rdpq_fill_rectangle(bx, by, bx + bw, by + bh);
    snprintf(line, sizeof line, "Skip: +%d Embers", SKIP_EMBERS);
    text_style(bx, by + 12, bw, ALIGN_CENTER, skip ? FONT_STYLE_GOLD : FONT_STYLE_GREY, line);
    if (s.circleCleared) {     // a new circle: stronger, and its night
        // the next circle: stronger bosses, and its night (two lines at most)
        snprintf(line, sizeof line, "Next: floor %d, %s. %s", s.floor + 1, NIGHTS[s.nextNight].name, NIGHTS[s.nextNight].desc);
        text_style(12, y0 + ch + 31, SCREEN_WIDTH - 24, ALIGN_CENTER, FONT_STYLE_RED, line);
    } else {
        snprintf(line, sizeof line, "Next: floor %d", s.floor + 1);
        text_style(0, y0 + ch + 31, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_GREY, line);
    }
}

// ---- The map screen, and a Rest's ----

static sprite_t *s_icons;       // the six node icons (16x16 I4 one above the next: Fight, Elite, Event, Shop, Rest, Boss), loaded with the map
static void icons_free(void) { if (s_icons) { rspq_wait(); sprite_free(s_icons); s_icons = NULL; } }

bool tower_map_is_open(void) { return s.scr == SCR_MAP; }
bool tower_rest_is_open(void) { return s.scr >= SCR_REST; }
bool tower_between_open(void) { return s.draftOpen || s.scr != SCR_NONE; }

void tower_map_open(void)
{
    s.scr = SCR_MAP;
    s.scrOpenMs = get_ticks_ms();
    if (!s_icons) s_icons = sprite_load("rom:/ui/map/map_icons.i4.sprite");
    map_assign_bosses();
    int r = s.map.row + 1, l = s.map.row >= 0 ? s.map.path[s.map.row] : 1;     // the cursor: straight up, else the first reachable
    if (!map_reachable(r, l)) for (l = 0; l < MAP_LANES - 1 && !map_reachable(r, l); l++) {}
    s.map.sel = (int8_t)l;
#ifdef HLE_AUTOPLAY
    debugf("[map] open: circle %d, floor %d next, row %d next:", s.circle, s.floor, r + 1);
    for (int k = 0; k < MAP_LANES; k++) if (map_reachable(r, k))
        debugf(" lane %d %s%s%s", k, NODE_NAMES[s.map.type[r][k]], s.map.boss[r][k] ? " " : "", s.map.boss[r][k] ? BOSS_NAMES[s.map.boss[r][k] - 1] : "");
    debugf(", cursor %d\n", l);
#endif
}

// Enter the node under the cursor (its fight, or a Rest); returns its NODE_
static int map_enter(void)
{
    int r = ++s.map.row, l = s.map.sel, t = s.map.type[r][l];
    s.map.path[r] = (int8_t)l;
    s.elite = t == NODE_ELITE;
    if (t == NODE_BOSS) s.boss = r ? TOWER_UNBOUND : TOWER_GUARDIAN;
    else if (t == NODE_FIGHT || t == NODE_ELITE) s.boss = (TowerBoss)(s.map.boss[r][l] - 1);
    if (is_fight(t)) { s.map.fights++; s.map.met |= 1 << s.boss; }
#ifdef HLE_AUTOPLAY
    debugf("[map] entered row %d lane %d: %s%s%s (floor %d, fight %d of the circle)\n", r + 1, l, NODE_NAMES[t], is_fight(t) ? " " : "",
           is_fight(t) ? BOSS_NAMES[s.boss] : "", s.floor, s.map.fights);
#endif
    return t;
}

static bool is_learn(int b) { return b >= BOON_LEARN_MOONBLADE && b <= BOON_LEARN_CLEAVE; }
// What a Rest can Temper: a level of a slotted boon below its max, a "+" unlocked by a maxed one, a
// known skill's "+" card (stage 5 turns those into levels); never a new boon or a duo
static int temper_targets(int8_t out[], int max)
{
    int n = 0;
    for (int b = 0; b < BOON_COUNT && n < max; b++)
        if (offerable(b) && !BOONS[b].duo && !is_learn(b) && (s.stacks[b] || BOONS[b].up || is_skill_card(b))) out[n++] = (int8_t)b;
    return n;
}

static bool row_ok(int i)
{
    if (s.scr != SCR_REST) return true;
    int8_t t[LIST_MAX];
    if (s.list[i] == REST_TEMPER_ROW) return temper_targets(t, LIST_MAX) > 0;
    if (s.list[i] == REST_POTIONS_ROW) return character.healthPotions < POTION_CAP;
    return true;
}

static void list_open(int scr)
{
    s.scr = (uint8_t)scr;
    s.listN = s.listSel = 0;
    if (scr == SCR_REST) for (int i = 0; i < REST_ROWS; i++) s.list[s.listN++] = (int8_t)i;
    else if (scr == SCR_TEMPER) { s.listN = (int8_t)temper_targets(s.list, LIST_MAX - 1); s.list[s.listN++] = -1; }     // then Back
    else { s.list[s.listN++] = -1; for (int i = 0; i < TOWER_SLOTS; i++) if (s.slot[i]) s.list[s.listN++] = (int8_t)i; }   // Keep, then the slots
    while (s.listSel < s.listN - 1 && !row_ok(s.listSel)) s.listSel++;
}

// Release (free at a Rest, with its choice): the slot's boon and its levels are gone ("+" too; a duo
// with its two boons), the slot opens. A rival it locked out stays locked (stage 2's call)
static void release_slot(int i)
{
    int parts[3] = { s.slot[i] - 1, s.merged[i][0] - 1, s.merged[i][1] - 1 };
#ifdef HLE_AUTOPLAY
    debugf("[rest] release slot %d: %s Lv %d%s\n", i + 1, BOONS[parts[0]].name, s.stacks[parts[0]], parts[1] >= 0 ? " (a duo, with its two)" : "");
#endif
    for (int k = 0; k < 3; k++) {
        if (parts[k] < 0) continue;
        s.stacks[parts[k]] = 0;
        for (int u = 0; u < BOON_COUNT; u++) if (BOONS[u].up == parts[k] + 1) s.stacks[u] = 0;
    }
    s.slot[i] = 0; s.merged[i][0] = s.merged[i][1] = 0;
    sync_max_health();
    if (character.health > character.maxHealth) character.health = character.maxHealth;
}

static void release_open(void)
{
    if (slots_used()) list_open(SCR_RELEASE);
    else tower_map_open();
}

static void rest_take(int v)
{
    float mh = character.maxHealth, h = character.health;
    int p = character.healthPotions, pm = POTION_CAP;
    if (v == REST_HEAL_ROW) character.health = fminf(mh, h + REST_HEAL * mh);
    else if (p < pm) character.healthPotions = p + REST_POTIONS < pm ? p + REST_POTIONS : pm;
#ifdef HLE_AUTOPLAY
    if (v == REST_HEAL_ROW) debugf("[rest] heal 40%%: %.0f -> %.0f of %.0f\n", (double)h, (double)character.health, (double)mh);
    else debugf("[rest] +2 potions: %d -> %d (up to %d)\n", p, character.healthPotions, pm);
#endif
}

bool tower_between_update(bool left, bool right, bool up, bool down, bool confirm)
{
    if (s.draftOpen) {
        if (tower_draft_update(left, right, up, down, confirm)) { tower_floor_restore(); tower_next_floor(); tower_map_open(); }
        return false;
    }
    if (!s.scr || get_ticks_ms() - s.scrOpenMs < DRAFT_INPUT_DELAY_MS) return false;    // (as the draft: no press taken unseen)
    if (s.scr == SCR_MAP) {     // Left/Right along the next row's reachable nodes; A or Up enters
        int d = right - left, r = s.map.row + 1;
        for (int l = s.map.sel + d; d && l >= 0 && l < MAP_LANES; l += d) if (map_reachable(r, l)) { s.map.sel = (int8_t)l; break; }
        if (!confirm && !up) return false;
        if (map_enter() == NODE_REST) { list_open(SCR_REST); s.scrOpenMs = get_ticks_ms(); return false; }
        s.scr = SCR_NONE;
        icons_free();
        return true;
    }
    // a Rest's screens: Up/Down along the rows (past any that can't be chosen), A takes one
    int d = down - up;
    for (int i = s.listSel + d; d && i >= 0 && i < s.listN; i += d) if (row_ok(i)) { s.listSel = (int8_t)i; break; }
    if (!confirm) return false;
    int v = s.list[s.listSel];
    if (s.scr == SCR_REST) {
        if (v == REST_TEMPER_ROW) list_open(SCR_TEMPER);
        else { rest_take(v); release_open(); }
    } else if (s.scr == SCR_TEMPER) {
        if (v < 0) list_open(SCR_REST);
        else {
#ifdef HLE_AUTOPLAY
            debugf("[rest] temper: %s Lv %d -> %d\n", BOONS[v].name, s.stacks[v], s.stacks[v] + 1);
#endif
            take_boon((BoonId)v);
            release_open();
        }
    } else {
#ifdef HLE_AUTOPLAY
        if (v < 0) debugf("[rest] release: none\n");
#endif
        if (v >= 0) release_slot(v);
        tower_map_open();
    }
    return false;
}

#define MAP_X(l) (100 + 60 * (l))
#define MAP_Y(r) (198 - 26 * (r))
// a 1 px line up the screen (y1 < y0) in fill-rectangle steps, as the menus draw their frames
static void line_up(int x0, int y0, int x1, int y1)
{
    int dy = y0 - y1;
    for (int i = 0; i < dy; i++) {
        int xa = x0 + (x1 - x0) * i / dy, xb = x0 + (x1 - x0) * (i + 1) / dy;
        if (xb < xa) { int t = xa; xa = xb; xb = t; }
        rdpq_fill_rectangle(xa, y0 - i - 1, xb + 1, y0 - i);
    }
}
// the menus' selection box (menu_controller.c)
static void select_box(int x0, int y0, int x1, int y1)
{
    flat_mode();
    rdpq_set_prim_color(RGBA32(0, 0, 0, 140));
    rdpq_fill_rectangle(x0, y0, x1, y1);
    rdpq_set_prim_color(RGBA32(255, 255, 255, 70));
    rdpq_fill_rectangle(x0, y0, x1, y0 + 1);
    rdpq_fill_rectangle(x0, y1 - 1, x1, y1);
    rdpq_fill_rectangle(x0, y0, x0 + 1, y1);
    rdpq_fill_rectangle(x1 - 1, y0, x1, y1);
}

static void map_draw(void)
{
    flat_mode();
    rdpq_set_prim_color(RGBA32(0, 0, 0, 200));     // (darker than the draft's: the hall's boss stands behind the nodes)
    rdpq_fill_rectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    // the links: the path taken and the way on bright, the rest dim (the rows behind dimmer)
    for (int r = 0; r < MAP_ROWS - 1; r++) for (int a = 0; a < MAP_LANES; a++) for (int b = 0; b < MAP_LANES; b++) {
        if (!s.map.type[r][a] || !s.map.type[r + 1][b] || !linked(r, a, b)) continue;
        bool taken = s.map.path[r] == a && s.map.path[r + 1] == b, next = r == s.map.row && s.map.path[r] == a && map_reachable(r + 1, b);
        rdpq_set_prim_color(next ? RGBA32(235, 215, 170, 255) : taken ? RGBA32(150, 130, 105, 255) : r < s.map.row ? RGBA32(55, 48, 42, 255) : RGBA32(105, 92, 78, 255));
        // centre to centre, from where it leaves one icon's box (10 px each way) to where it enters the other's
        int dx = MAP_X(b) - MAP_X(a), dy = MAP_Y(r + 1) - MAP_Y(r);
        float t = fminf(dx ? 10.0f / abs(dx) : 1.0f, 10.0f / abs(dy));
        int ox = (int)(dx * t), oy = (int)(dy * t);
        line_up(MAP_X(a) + ox, MAP_Y(r) + oy, MAP_X(b) - ox, MAP_Y(r + 1) - oy);
    }
    int cr = s.map.row + 1, cl = s.map.sel;
    if (cr < MAP_ROWS) select_box(MAP_X(cl) - 11, MAP_Y(cr) - 11, MAP_X(cl) + 11, MAP_Y(cr) + 11);
    if (s_icons) {
        static const color_t TINT[NODE_TYPES] = {
            [NODE_FIGHT] = RGBA32(200, 200, 215, 255), [NODE_ELITE] = RGBA32(230, 70, 60, 255), [NODE_EVENT] = RGBA32(90, 200, 200, 255),
            [NODE_SHOP] = RGBA32(235, 190, 70, 255), [NODE_REST] = RGBA32(255, 150, 60, 255), [NODE_BOSS] = RGBA32(255, 215, 90, 255),
        };
        rdpq_sync_pipe();
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, 0, PRIM, 0), (TEX0, 0, PRIM, 0)));
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        for (int r = 0; r < MAP_ROWS; r++) for (int l = 0; l < MAP_LANES; l++) {
            int t = s.map.type[r][l];
            if (!t) continue;
            // visited dimmed, passed by dimmer, reachable full colour, the rows ahead a little dim
            int k = r <= s.map.row ? (s.map.path[r] == l ? 45 : 22) : map_reachable(r, l) ? 100 : 70;
            color_t c = TINT[t];
            rdpq_set_prim_color(RGBA32(c.r * k / 100, c.g * k / 100, c.b * k / 100, 255));
            rdpq_sprite_blit(s_icons, MAP_X(l) - 8, MAP_Y(r) - 8, &(rdpq_blitparms_t){ .t0 = 16 * (t - 1), .width = 16, .height = 16 });
        }
    }
    // the top line: floor, Ascension, night (gold is stage 4); the bottom: the node under the cursor
    char line[80];
    int n = snprintf(line, sizeof line, "Floor %d", s.floor);
    if (s.circle > 1) n += snprintf(line + n, sizeof line - n, "   Ascension %d", s.circle - 1);
    if (s.night) snprintf(line + n, sizeof line - n, "   %s", NIGHTS[s.night].name);
    text_style(0, 22, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_GOLD, line);
    if (cr < MAP_ROWS) {
        int t = s.map.type[cr][cl];
        const char *boss = t == NODE_BOSS ? BOSS_NAMES[cr ? TOWER_UNBOUND : TOWER_GUARDIAN] : s.map.boss[cr][cl] ? BOSS_NAMES[s.map.boss[cr][cl] - 1] : "";
        if (t == NODE_ELITE) snprintf(line, sizeof line, "Elite: %s, rare draft, x1.5 Embers", boss);
        else if (t == NODE_FIGHT) snprintf(line, sizeof line, "Fight: %s", boss);
        else if (t == NODE_REST) snprintf(line, sizeof line, "Rest: heal, Temper or potions");
        else snprintf(line, sizeof line, "%s", boss);
        text_style(0, 227, SCREEN_WIDTH, ALIGN_CENTER, t == NODE_ELITE ? FONT_STYLE_RED : FONT_STYLE_WHITE, line);
    }
}

static void rest_draw(void)
{
    flat_mode();
    rdpq_set_prim_color(RGBA32(0, 0, 0, 170));
    rdpq_fill_rectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    static const char *const HEAD[] = { [SCR_REST] = "Rest: choose one", [SCR_TEMPER] = "Temper: +1 level", [SCR_RELEASE] = "Release a boon? Free, with its levels" };
    text_style(0, 40, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_GOLD, HEAD[s.scr]);
    char line[80];
    snprintf(line, sizeof line, "Health %.0f/%.0f   Potions %d", (double)character.health, (double)character.maxHealth, character.healthPotions);
    text_style(0, 56, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_GREY, line);
    const int x = 50, w = SCREEN_WIDTH - 100, y0 = 84, pitch = 15;
    for (int i = 0; i < s.listN; i++) {
        int v = s.list[i], y = y0 + pitch * i, style = row_ok(i) ? FONT_STYLE_WHITE : FONT_STYLE_GREY;
        if (i == s.listSel) select_box(x - 6, y - 11, x + w + 6, y + 3);
        if (s.scr == SCR_REST) {
            int p = character.healthPotions, pm = POTION_CAP;
            if (v == REST_HEAL_ROW) snprintf(line, sizeof line, "Heal 40%%: +%.0f health", (double)fminf(REST_HEAL * character.maxHealth, character.maxHealth - character.health));
            else if (v == REST_TEMPER_ROW) snprintf(line, sizeof line, row_ok(i) ? "Temper: +1 level on a boon or skill" : "Temper: nothing to raise");
            else if (p < pm) snprintf(line, sizeof line, "+2 potions (%d -> %d)", p, p + REST_POTIONS < pm ? p + REST_POTIONS : pm);
            else snprintf(line, sizeof line, "+2 potions: full");
        } else if (v < 0) snprintf(line, sizeof line, s.scr == SCR_TEMPER ? "Back" : "Keep them all");
        else if (s.scr == SCR_TEMPER) {
            const BoonDef *bd = &BOONS[v];
            if (bd->up) snprintf(line, sizeof line, "%s (top level)", bd->name);
            else if (is_skill_card(v)) snprintf(line, sizeof line, "%s (skill)", bd->name);
            else snprintf(line, sizeof line, "%s   Lv %d -> %d", bd->name, s.stacks[v], s.stacks[v] + 1);
            style = FAMILIES[bd->family].style;
        } else {
            BoonId b = (BoonId)(s.slot[v] - 1);
            if (s.merged[v][0]) snprintf(line, sizeof line, "%s (duo: %s + %s)", BOONS[b].name, BOONS[s.merged[v][0] - 1].name, BOONS[s.merged[v][1] - 1].name);
            else snprintf(line, sizeof line, "%s   Lv %d", BOONS[b].name, s.stacks[b]);
            style = BOONS[b].duo ? FONT_STYLE_GOLD : FAMILIES[BOONS[b].family].style;
        }
        text_style(x, y, w, ALIGN_CENTER, style, line);
    }
    if (s.scr == SCR_REST && slots_used()) text_style(0, y0 + pitch * s.listN + 12, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_GREY, "Then, free: Release a boon if you wish");
}

#ifdef HLE_AUTOPLAY     // test aids: where the map and a Rest's screens stand (HLE_MAP_WALK)
int tower_screen(void) { return s.draftOpen ? -1 : s.scr; }
int tower_map_row(void) { return s.map.row; }
int tower_map_sel(void) { return s.map.sel; }
int tower_map_next(int l) { return map_reachable(s.map.row + 1, l) ? s.map.type[s.map.row + 1][l] : 0; }     // a reachable node's NODE_, else 0
int tower_list_sel(void) { return s.listSel; }
#endif

void tower_between_draw(void)
{
    if (s.draftOpen) tower_draft_draw();
    else if (s.scr == SCR_MAP) map_draw();
    else if (s.scr) rest_draw();
}

void tower_hud_draw(void)
{
    if (!s.active || tower_between_open()) return;     // the screens between fights have their own header
    char line[32];
    // inside the title-safe area, like the rest of the HUD (TVs and accurate emulators show the edges)
    const int w = SCREEN_WIDTH - ui_safe_margin_x(), top = ui_safe_margin_y() + 10;
    snprintf(line, sizeof line, "Floor %d", s.floor);
    text_style(0, top, w, ALIGN_RIGHT, FONT_STYLE_GOLD, line);
    if (tower_circle() > 1) {
        snprintf(line, sizeof line, "Ascension %d", tower_circle() - 1);
        text_style(0, top + 12, w, ALIGN_RIGHT, FONT_STYLE_RED, line);
        if (s.night) text_style(0, top + 24, w, ALIGN_RIGHT, FONT_STYLE_RED, NIGHTS[s.night].name);
    }
}

const char* tower_run_summary(void)
{
    static char b[640];
    int n = 0;
    #define ADD(...) n += snprintf(b + n, n < (int)sizeof b ? sizeof b - n : 0, __VA_ARGS__)
    ADD("Floor %d", s.floor);
    if (tower_circle() > 1) ADD(", Ascension %d", tower_circle() - 1);
    if (s.night) ADD(", %s", NIGHTS[s.night].name);
    ADD("\nRelic: %s\n", tower_relic_name(tower_relic_equipped()));
    ADD("Damage x%.2f, damage taken x%.2f\n", (double)tower_mod_damage_dealt(), (double)tower_mod_damage_taken());
    ADD("Max health %.0f, potions %d", (double)tower_mod_max_health(), character.healthPotions);
    if (s.potionDebt) ADD(" (%d owed)", s.potionDebt);
    ADD("\nBlessings:"); int k = 0;
    for (int i = 0; i < PB_COUNT; i++) if (s.bless[i]) ADD("%s %s %d", k++ ? "," : "", BLESSINGS[i].name, s.bless[i]);
    if (!k) ADD(" none");
    ADD("\nCurses:"); k = 0;
    for (int i = 0; i < PC_COUNT; i++) if (s.curse[i]) ADD("%s %s %d", k++ ? "," : "", CURSES[i].name, s.curse[i]);
    if (!k) ADD(" none");
    #undef ADD
    return b;
}

int tower_build_rows(char left[][40], char right[][12], int style[], int max)
{
    static const BoonId LEARN[SKILL_COUNT] = { BOON_LEARN_MOONBLADE, BOON_LEARN_HALFMOON, BOON_LEARN_CLEAVE };
    static const BoonId PLUS[SKILL_COUNT] = { BOON_CRESCENT_ECHO, BOON_HALFMOON_PLUS, BOON_CLEAVE_PLUS };
    int n = 0;
    for (int k = 0; k < SKILL_COUNT && n < max; k++, n++) {     // the skills: button, name, known or not (and its card)
        snprintf(left[n], 40, "%s %s", joypad_skill_button(k), BOONS[LEARN[k]].name);
        snprintf(right[n], 12, "%s", !knows(k) ? "-" : s.stacks[PLUS[k]] ? "known +" : "known");
        style[n] = knows(k) ? FONT_STYLE_WHITE : FONT_STYLE_GREY;
    }
    for (int i = 0; i < TOWER_SLOTS && n < max; i++, n++) {      // the slots: name in its family's colour, its level ("+" at the top)
        if (!s.slot[i]) { snprintf(left[n], 40, "Empty slot"); right[n][0] = 0; style[n] = FONT_STYLE_GREY; continue; }
        BoonId b = (BoonId)(s.slot[i] - 1);
        snprintf(left[n], 40, "%s", BOONS[b].name);
        if (BOONS[b].duo && s.merged[i][0]) {       // a duo: "Duo", its two boons on a row under it
            snprintf(right[n], 12, "Duo");
            style[n] = FONT_STYLE_GOLD;
            if (n + 1 < max) { n++; snprintf(left[n], 40, "  %s + %s", BOONS[s.merged[i][0] - 1].name, BOONS[s.merged[i][1] - 1].name); right[n][0] = 0; style[n] = FONT_STYLE_GREY; }
            continue;
        }
        bool plus = false;
        for (int p = 0; p < BOON_COUNT; p++) plus |= BOONS[p].up == b + 1 && s.stacks[p];
        snprintf(right[n], 12, "Lv %d/%d%s", s.stacks[b], BOONS[b].maxStacks, plus ? "+" : "");
        style[n] = FAMILIES[BOONS[b].family].style;
    }
    // the rivals locked out, greyed, a few to a row
    int len = 0;
    for (int b = 0; b < BOON_COUNT && n < max; b++) {
        if (!s.locked[b]) continue;
        int w = (int)strlen(BOONS[b].name);
        if (len && len + 2 + w > 38) { n++; len = 0; if (n >= max) break; }
        if (!len) { len = snprintf(left[n], 40, "Locked: %s", BOONS[b].name); right[n][0] = 0; style[n] = FONT_STYLE_GREY; }
        else len += snprintf(left[n] + len, 40 - len, ", %s", BOONS[b].name);
    }
    if (len) n++;
    return n;
}

// ---- The unlock ladder ----

// the families with new boons, in order (a pack with none isn't sold)
static int pack_family(int i)
{
    for (int f = 0; f < FAM_COUNT; f++) {
        bool any = false;
        for (int b = 0; b < BOON_COUNT; b++) any |= BOONS[b].pack && BOONS[b].family == f;
        if (any && i-- == 0) return f;
    }
    return -1;
}
int tower_pack_count(void) { int n = 0; while (pack_family(n) >= 0) n++; return n; }
const char* tower_pack_name(int i) { int f = pack_family(i); return f >= 0 ? FAMILIES[f].name : ""; }
const char* tower_pack_desc(int i)
{
    static char d[96];
    int f = pack_family(i), n = 0;
    d[0] = 0;
    for (int b = 0; b < BOON_COUNT && f >= 0; b++)
        if (BOONS[b].pack && BOONS[b].family == f) n += snprintf(d + n, sizeof d - n, "%s%s", n ? ", " : "New boons: ", BOONS[b].name);
    return d;
}
bool tower_pack_owned(int i) { int f = pack_family(i); TowerSave t; tower_load(&t); return f >= 0 && ((t.unlocks >> f) & 1); }
int  tower_pack_cost(int i)
{
    if (tower_pack_owned(i)) return -1;
    TowerSave t; tower_load(&t);
    int bought = 0;
    for (int f = 0; f < FAM_COUNT; f++) bought += (t.unlocks >> f) & 1;
    return PACK_COST[bought < FAM_COUNT ? bought : FAM_COUNT - 1];
}
bool tower_pack_buy(int i)
{
    int f = pack_family(i), cost = tower_pack_cost(i);
    TowerSave t; tower_load(&t);
    if (f < 0 || cost < 0 || t.embers < cost) return false;
    t.embers -= cost;
    t.unlocks |= 1 << f;
#ifdef HLE_AUTOPLAY
    debugf("[unlock] bought the %s pack for %d: unlocks %03x, Embers %d\n", FAMILIES[f].name, cost, t.unlocks, t.embers);
#endif
    return save_controller_set_tower(&t);
}

const char* tower_death_summary(void)
{
    static char line[96];
    int boons = 0;
    for (int b = 0; b < BOON_COUNT; b++) boons += s.stacks[b];
    for (int b = 0; b < PB_COUNT; b++) boons += s.bless[b];
    snprintf(line, sizeof line, "Floor %d   +%d Embers (%d)\n%d boons taken", s.floor, s.runEmbers, tower_embers(), boons);
    return line;
}

// ---- Shrine ----

int tower_embers(void) { TowerSave t; tower_load(&t); return t.embers; }
int tower_best_floor(void) { TowerSave t; tower_load(&t); return t.bestFloor; }

int tower_upgrade_level(UpgradeId u)
{
    TowerSave t; tower_load(&t);
    int l = t.levels[u];
    return l > UPG_MAX_LEVEL ? UPG_MAX_LEVEL : l;
}

int tower_upgrade_cost(UpgradeId u)
{
    int l = tower_upgrade_level(u);
    return l >= UPG_MAX_LEVEL ? -1 : UPGRADE_COST[l];
}

bool tower_upgrade_buy(UpgradeId u)
{
    int cost = tower_upgrade_cost(u);
    TowerSave t; tower_load(&t);
    if (cost < 0 || t.embers < cost) return false;
    t.embers -= cost;
    t.levels[u]++;
    return save_controller_set_tower(&t);
}

const char* tower_night_name(void) { return s.active ? NIGHTS[s.night].name : ""; }

const char* tower_upgrade_name(UpgradeId u) { return UPGRADES[u].name; }
const char* tower_upgrade_desc(UpgradeId u) { return UPGRADES[u].desc; }

// ---- Modifiers ----

// Shrine upgrades count only inside a run; the story and Bonus fights stay as tuned
static int shrine(UpgradeId u) { return s.active ? tower_upgrade_level(u) : 0; }

static float powf_int(float b, int e) { float r = 1.0f; while (e-- > 0) r *= b; return r; }

// The climb's baseline: each circle cleared, +10% damage and 6% less taken, through circle 5 (it
// helps a run catch up; past it, only boons and pacts grow, so even a full set runs out of floors)
#define BASELINE_CIRCLES 4
static int circles_cleared(void)
{
    int c = s.active ? tower_circle() - 1 : 0;
    return c < BASELINE_CIRCLES ? c : BASELINE_CIRCLES;
}

float tower_mod_damage_dealt(void)
{
    int ee = tower_boon_stacks(BOON_EMBER_EDGE);     // +15% a level, Lv 4-5 +10%
    float m = (1.0f + 0.08f * shrine(UPG_HONED_BLADE)) * (1.0f + 0.15f * (ee < 3 ? ee : 3) + 0.10f * (ee > 3 ? ee - 3 : 0));
    if (s.active) {
        m *= 1.0f + 0.10f * circles_cleared();
        m *= (1.0f + 0.08f * s.bless[PB_MIGHT]) * powf_int(0.95f, s.curse[PC_WEAKNESS]);
        if (character.health < character.maxHealth * 0.3f) m *= 1.0f + 0.15f * s.bless[PB_LAST_BREATH];
    }
    if (tower_boon_stacks(BOON_LAST_STAND) && character.health < character.maxHealth * 0.3f) m *= 1.0f + 0.4f * tower_boon_stacks(BOON_LAST_STAND);
    if (tower_boon_stacks(BOON_BLOOD_PACT)) m *= 1.35f;
    extern double nowS;
    if (tower_boon_stacks(BOON_RIPOSTE) && nowS < s.riposteUntil) m *= 1.5f;
    if (has_relic(RELIC_DAMAGE)) m *= 1.06f;
    if (has_relic(RELIC_FURY)) m *= 1.15f;
    return m;
}

float tower_mod_damage_vs(float bossHealthFrac, bool staggered, int kind)
{
    if (!s.active) return 1.0f;
    float m = 1.0f;
    if (bossHealthFrac < 0.5f) m *= 1.0f + 0.12f * s.bless[PB_WOUNDED];
    if (staggered) m *= 1.0f + 0.15f * s.bless[PB_STAGGERED];
    // the new boons that change a hit (logged as they do)
    extern double nowS;
    int rt = tower_boon_stacks(BOON_RED_THIRST);
    if (rt && character.maxHealth > 0.0f) {         // +10% a level per full 25% of health missing
        int q = (int)((1.0f - character.health / character.maxHealth) / 0.25f);
        if (q > 0) { m *= 1.0f + 0.10f * rt * q; BFX("Red Thirst: %.0f%% health, x%.2f\n", (double)(100.0f * character.health / character.maxHealth), (double)(1.0f + 0.10f * rt * q)); }
    }
    if (tower_boon_stacks(BOON_SANGUINE_FLASK) && nowS < s.sanguineUntil) { m *= 1.25f; BFX("Sanguine Flask: x1.25 (%.1f s left)\n", s.sanguineUntil - nowS); }
    int se = tower_boon_stacks(BOON_SHADOW_EDGE);
    if (se && nowS < s.shadowEdgeUntil) { m *= 1.0f + 0.08f * se; BFX("Shadow Edge: x%.2f after a roll\n", (double)(1.0f + 0.08f * se)); }
    bool sword = kind == TOWER_HIT_SWORD || kind == TOWER_HIT_FINISHER;
    if (sword && tower_boon_stacks(BOON_WAXING_MOON) && nowS < s.waxingUntil) { m *= 1.15f; BFX("Waxing Moon: sword hit x1.15\n"); }
    if (kind == TOWER_HIT_FINISHER && tower_boon_stacks(BOON_EMBER_EDGE_PLUS)) { m *= 1.25f; BFX("Ember Edge+: the finisher x1.25\n"); }
    if (kind == TOWER_HIT_SKILL && tower_boon_stacks(BOON_BLOOD_MOON_RITE)) { m *= 1.4f; BFX("Blood Moon Rite: a skill's hit x1.40\n"); }
    if (kind == TOWER_HIT_SKILL && staggered && tower_boon_stacks(BOON_ECLIPSE)) { m *= 1.5f; BFX("Eclipse: skill hit on a staggered boss x1.50\n"); }
    return m;
}

bool tower_hit_impaired(void)
{
    if (!s.active || !s.curse[PC_IMPAIRED]) return false;
    return (int)(rng_next() % 100) < 3 * s.curse[PC_IMPAIRED];
}

void tower_on_roll(void)
{
    extern double nowS;
    s.exposedUntil = nowS + 2.0;
    if (tower_boon_stacks(BOON_SHADOW_EDGE)) s.shadowEdgeUntil = nowS + 2.0;
}

float tower_mod_damage_taken(void)
{
    int is = tower_boon_stacks(BOON_IRON_SKIN);     // -15% a level, Lv 3 -10%
    float m = powf_int(0.85f, is < 2 ? is : 2) * (is >= 3 ? 0.9f : 1.0f);
    if (s.active) {
        extern double nowS;
        m *= powf_int(0.94f, circles_cleared()) * powf_int(0.94f, s.bless[PB_BULWARK]) * (1.0f + 0.05f * s.curse[PC_FRAILTY]);
        if (s.curse[PC_EXPOSED] && nowS < s.exposedUntil) m *= 1.0f + 0.15f * s.curse[PC_EXPOSED];
        if (nowS < s.steelUntil) m *= 0.6f;     // Steel Hearth: a potion's ward
    }
    if (s.active) m *= BOSS_DMG_START + BOSS_DMG_PER_CIRCLE * (tower_circle() - 1);
    if (s.active && s.night == NIGHT_BLOOD_MOON) m *= 1.2f;
    if (s.active && s.night == NIGHT_STARFALL) m *= 1.1f;
    if (has_relic(RELIC_WARD)) m *= 0.94f;
    if (has_relic(RELIC_DREAD)) m *= 1.1f;
    return m;
}

float tower_mod_poise(void) { return 1.0f; }
float tower_mod_stagger_time(void)
{
#ifdef HLE_STAGGER_LONG     // test aid: Crushing Blows outside the Tower
    return 1.5f;
#endif
    return tower_boon_stacks(BOON_CRUSHING_BLOWS) ? 1.5f : 1.0f;
}
float tower_mod_stagger_damage(void) { return tower_boon_stacks(BOON_EXECUTIONER) ? (tower_boon_stacks(BOON_EXECUTIONER_PLUS) ? 1.9f : 1.6f) : 1.0f; }
float tower_mod_perfect_window(void)
{
    if (!tower_boon_stacks(BOON_PHANTOM_STEP)) return 0.2f;
    return tower_boon_stacks(BOON_PHANTOM_STEP_PLUS) ? 0.31f : 0.28f;   // (still inside a moving roll's 0.32 s of invincibility)
}

// A bolt to fall `delay` s from now on the boss (Thunderstep, Skyfall); tower_tick drops it
static void queue_bolt(float dmg, double delay)
{
    extern double nowS;
    for (int i = 0; i < 4; i++) if (s.boltAt[i] <= 0.0) { s.boltAt[i] = nowS + delay; s.boltDmg[i] = dmg; return; }
}   // (0.35 outlasted a roll's 0.32 s of invincibility: every dodge was perfect)

void tower_on_perfect_dodge(void)
{
    if (!s.active) return;
    if (tower_boon_stacks(BOON_PHANTOM_STEP)) character.health = fminf(character.maxHealth, character.health + character.maxHealth * 0.02f);
    extern double nowS;
    double window = tower_boon_stacks(BOON_RIPOSTE_PLUS) ? 4.0 : 2.5;
    if (tower_boon_stacks(BOON_RIPOSTE)) s.riposteUntil = nowS + window;
    if (tower_boon_stacks(BOON_SHADES_MERCY)) { s.mercyUntil = nowS + window; BFX("Shade's Mercy: %.1f s of Rally at 60%%\n", window); }
    if (tower_boon_stacks(BOON_THUNDERSTEP)) {      // two of Storm Blade's bolts, 0.3 s apart
        float bolt = (STORM_BOLT + STATIC_BOLT * tower_boon_stacks(BOON_STATIC)) * (tower_boon_stacks(BOON_STATIC_PLUS) ? 1.5f : 1.0f);
        queue_bolt(bolt, 0.0); queue_bolt(bolt, 0.3);
        BFX("Thunderstep: a perfect dodge calls 2 bolts of %.0f\n", (double)bolt);
    }
    if (tower_boon_stacks(BOON_WANING_LIGHT)) {
        BFX("Waning Light: a perfect dodge, skill cooldowns -2 s (%.1f %.1f %.1f s left)\n", (double)character_get_skill_cooldown(0), (double)character_get_skill_cooldown(1), (double)character_get_skill_cooldown(2));
        character_cut_skill_cooldowns(2.0f);
    }
    if (tower_boon_stacks(BOON_VANISH) && !s.vanishUsed && !s.vanishArmed && ++s.perfectDodges >= 3) {
        s.vanishArmed = true;
        BFX("Vanish: 3 perfect dodges this floor, the next blow deals 0\n");
    }
}

float tower_on_sword_hit(bool finisher)
{
    if (!s.active) return 0.0f;
    s.hitCount++;
    int st = tower_boon_stacks(BOON_STATIC);
    float bolt = 0.0f, amp = tower_boon_stacks(BOON_STATIC_PLUS) ? 1.5f : 1.0f, storm = (STORM_BOLT + STATIC_BOLT * st) * amp;
    bool sb = tower_boon_stacks(BOON_STORM_BLADE) && s.hitCount % (tower_boon_stacks(BOON_STORM_BLADE_PLUS) ? 3 : 4) == 0;   // Storm Blade+: every 3rd
    if (sb) {
        bolt += storm;
#ifdef HLE_AUTOPLAY
        debugf("[boon] Storm Blade: lightning on hit %d (%.0f)\n", s.hitCount, (double)storm);
#endif
        if (st) BFX("Static: Storm Blade's bolt %.0f (+%.0f)\n", (double)storm, (double)(STATIC_BOLT * st));
    } else if (st && !tower_boon_stacks(BOON_STORM_BLADE) && s.hitCount % 8 == 0) {    // no Storm Blade: a weak bolt of its own
        float w = (STORM_BOLT + STATIC_BOLT * (st - 1)) * amp;
        bolt += w;
        BFX("Static: a bolt on hit %d (%.0f)\n", s.hitCount, (double)w);
    }
    if (s.overcharge > 0) {     // every hit a bolt (one already called counts)
        s.overcharge--;
        if (!sb) bolt += storm;
        BFX("Overcharge: lightning on hit %d (%.0f), %d left\n", s.hitCount, (double)storm, s.overcharge);
    }
    if (finisher && tower_boon_stacks(BOON_THUNDERCLAP)) {
        float tc = tower_boon_stacks(BOON_THUNDERCLAP_PLUS) ? 12.0f : 8.0f;
        bolt += tc; BFX("Thunderclap: the finisher calls lightning (%.0f)\n", (double)tc);
    }
    int hm = tower_boon_stacks(BOON_HEMORRHAGE);
    if (finisher && hm) {       // a bleed of 4 a level over 3 s; a new finisher adds to it and starts the 3 s again
        s.bleedLeft += 4.0f * hm;
        s.bleedRate = s.bleedLeft / 3.0f;
        if (s.bleedTick <= 0.0f) s.bleedTick = 0.5f;
        BFX("Hemorrhage: the finisher bleeds, %.1f to come over 3 s\n", (double)s.bleedLeft);
    }
    return bolt;
}

float tower_lightning_poise(void)
{
    if (!tower_boon_stacks(BOON_CONDUCTOR)) return 0.0f;
    BFX("Conductor: the bolt fills the stagger meter like a sword hit\n");
    return 1.0f;
}

float tower_mod_finisher_poise(void)
{
    int hh = tower_boon_stacks(BOON_HEAVY_HAND);
    if (hh) BFX("Heavy Hand: the finisher's stagger x%.2f\n", (double)(1.0f + 0.2f * hh));
    return 1.0f + 0.2f * hh;
}

float tower_mod_hit_cooldown_cut(void)
{
    int lt = tower_boon_stacks(BOON_LUNAR_TIDE);
    if (lt) BFX("Lunar Tide: skill cooldowns -%.1f s\n", (double)(0.3f * lt));
    return 0.3f * lt;
}

void tower_on_potion(void)
{
    extern double nowS;
    if (tower_boon_stacks(BOON_SANGUINE_FLASK)) { s.sanguineUntil = nowS + 6.0; BFX("Sanguine Flask: a potion, +25%% damage for 6 s\n"); }
    if (tower_boon_stacks(BOON_STEEL_HEARTH)) {     // +10 max health for the floor (up to +30), -40% taken for 4 s
        if (s.steelHp < 30.0f) { s.steelHp += 10.0f; sync_max_health(); }
        s.steelUntil = nowS + 4.0;
        BFX("Steel Hearth: a potion, max health %.0f (+%.0f this floor), -40%% taken for 4 s\n", (double)character.maxHealth, (double)s.steelHp);
    }
}

void tower_on_skill_cast(void)
{
    extern double nowS;
    if (tower_boon_stacks(BOON_WAXING_MOON)) { s.waxingUntil = nowS + 4.0; BFX("Waxing Moon: a cast, +15%% sword damage for 4 s\n"); }
    s.skyfallUsed = false;
    if (tower_boon_stacks(BOON_BLOOD_MOON_RITE)) {      // 3% of max health a cast, never the last of it
        float h = character.health, cost = 0.03f * character.maxHealth;
        character.health = fmaxf(fminf(h, 1.0f), h - cost);
        BFX("Blood Moon Rite: a cast costs %.1f health (%.1f -> %.1f)\n", (double)cost, (double)h, (double)character.health);
    }
}

void tower_on_skill_hit(void)
{
    if (!tower_boon_stacks(BOON_SKYFALL) || s.skyfallUsed) return;
    s.skyfallUsed = true;       // once a cast
    queue_bolt(STORM_BOLT, 0.0);
    BFX("Skyfall: a skill's hit calls Storm Blade's pillar (%.0f)\n", (double)STORM_BOLT);
}

void tower_on_stagger(void)
{
    if (tower_boon_stacks(BOON_OVERCHARGE)) { s.overcharge = 5; BFX("Overcharge: a stagger, the next 5 hits call lightning\n"); }
    if (tower_boon_stacks(BOON_IRON_TIDE)) {
        BFX("Iron Tide: a stagger resets the skill cooldowns (%.1f %.1f %.1f s left)\n", (double)character_get_skill_cooldown(0), (double)character_get_skill_cooldown(1), (double)character_get_skill_cooldown(2));
        character_cut_skill_cooldowns(1e6f);
    }
}

void tower_on_stagger_end(void)
{
    extern double nowS;
    if (tower_boon_stacks(BOON_CRUSHING_BLOWS_PLUS)) { s.drainHoldUntil = nowS + 3.0; BFX("Crushing Blows+: the boss rose, no stagger drain for 3 s\n"); }
}
bool tower_poise_drain_held(void) { extern double nowS; return s.active && nowS < s.drainHoldUntil; }

float tower_on_lightning(float bolt, bool staggered)
{
    if (!s.active) return bolt;
    if (tower_boon_stacks(BOON_CRIMSON_STORM)) {
        if (tower_boon_stacks(BOON_BLOODTHIRST)) {      // feeds Bloodthirst at half a sword hit's rate (of the bolt)
            float h = character.health, heal = 0.5f * tower_mod_lifesteal() * bolt;
            character.health = fminf(character.maxHealth, h + heal);
            BFX("Crimson Storm: a bolt of %.0f heals %.2f (%.1f -> %.1f)\n", (double)bolt, (double)heal, (double)h, (double)character.health);
        } else { bolt *= 1.25f; BFX("Crimson Storm: no Bloodthirst, the bolt x1.25 (%.1f)\n", (double)bolt); }
    }
    if (staggered && tower_boon_stacks(BOON_ANVIL_OF_STORMS)) { bolt *= 2.0f; BFX("Anvil of Storms: a bolt on a staggered boss x2 (%.1f)\n", (double)bolt); }
    return bolt;
}

float tower_mod_blow(float amount)
{
    if (!s.active) return amount;
    if (s.vanishArmed) {
        s.vanishArmed = false; s.vanishUsed = true;     // once a floor
        BFX("Vanish: a blow of %.1f deals 0\n", (double)amount);
        return 0.0f;
    }
    if (tower_boon_stacks(BOON_IRON_SKIN_PLUS) && !s.ironFirstUsed && amount > 0.0f) {
        s.ironFirstUsed = true;     // the first blow a floor
        BFX("Iron Skin+: the floor's first blow of %.1f deals %.1f\n", (double)amount, (double)(amount * 0.5f));
        amount *= 0.5f;
    }
    if (tower_boon_stacks(BOON_STALWART) && amount > 0.2f * character.maxHealth) {
        BFX("Stalwart: a blow of %.1f (over 20%% of %.0f) deals %.1f\n", (double)amount, (double)character.maxHealth, (double)(amount * 0.75f));
        amount *= 0.75f;
    }
    return amount;
}

int   tower_mod_max_potions(void) { return s.active ? START_POTIONS + shrine(UPG_FLASK_CAPACITY) + 3 * tower_boon_stacks(BOON_DEEP_FLASK) + 2 * has_relic(RELIC_FLASK) - has_relic(RELIC_PARCHED) : 3; }   // Story and Bonus: 3
bool  tower_mod_skill_allowed(int skill) { return character_skills_enabled() && (!s.active || knows(skill)); }
float tower_mod_skill_damage(int skill)
{
    if (skill == SKILL_HALFMOON_SLASH && tower_boon_stacks(BOON_HALFMOON_PLUS)) return 1.2f;
    if (skill == SKILL_CRESCENT_CLEAVE && tower_boon_stacks(BOON_CLEAVE_PLUS)) return 1.2f;
    return 1.0f;
}
float tower_mod_potion_heal(void)
{
    return 0.45f * (1.0f + 0.08f * shrine(UPG_FLASK_POTENCY)) * (1.0f + 0.25f * tower_boon_stacks(BOON_KINDLED_FLASK))
           * (tower_boon_stacks(BOON_BLOOD_PACT) ? 0.75f : 1.0f);
}
float tower_mod_max_health(void) { return 150.0f + 10.0f * shrine(UPG_VIGOR) + 25.0f * tower_boon_stacks(BOON_VIGOR) + 15.0f * has_relic(RELIC_HEALTH) - 20.0f * has_relic(RELIC_FRAIL) + (s.active ? 12.0f * s.bless[PB_VITALITY] + s.steelHp : 0.0f); }
float tower_mod_roll_cost(void) { return (tower_boon_stacks(BOON_FLEETFOOT) ? 0.6f : 1.0f) * (has_relic(RELIC_LEADEN) ? 1.25f : 1.0f); }
float tower_mod_stamina_empty(void) { return 1.0f + 0.4f * tower_boon_stacks(BOON_SECOND_WIND); }   // the wait once the bar is empty: x1/1.4 a stack
float tower_mod_stamina_regen(void) { return (1.0f + 0.4f * tower_boon_stacks(BOON_SECOND_WIND)) * (has_relic(RELIC_STAMINA) ? 1.2f : 1.0f); }
float tower_mod_skill_cooldown(void)
{
    return (1.0f - 0.08f * shrine(UPG_SKILL_MASTERY)) * powf_int(tower_boon_stacks(BOON_MOON_MASTERY_PLUS) ? 0.75f : 0.8f, tower_boon_stacks(BOON_MOON_MASTERY))
           * ((s.active && s.night == NIGHT_STARFALL) ? 0.7f : 1.0f);
}
float tower_mod_lifesteal(void) { return (tower_boon_stacks(BOON_BLOODTHIRST_PLUS) ? 0.16f : 0.12f) * tower_boon_stacks(BOON_BLOODTHIRST) + 0.03f * has_relic(RELIC_VAMPIRIC); }   // of a hit before boons: it no longer grows with the damage multiplier

// Rally, at Bloodborne's pace in our numbers: lost health stays winnable 1 s, then drains away
// over 4 s; each hit wins back what it dealt before damage boons (a sword hit 4: about four hits for a typical blow,
// one Halfmoon Slash for most of a heavy one). A new blow adds to it and holds it again
#define RALLY_HOLD  1.0f
#define RALLY_DRAIN 4.0f
#define RALLY_RATIO 1.0f
void tower_on_player_hurt(float lost)
{
    if ((!tower_boon_stacks(BOON_RALLY) && !tower_boon_stacks(BOON_SHADES_MERCY)) || lost <= 0.0f) return;   // (Shade's Mercy keeps the pool too)
    s.rally += lost;
    s.rallyHold = tower_boon_stacks(BOON_RALLY) >= 2 ? 1.5f : RALLY_HOLD;    // Lv 2 holds it longer
#ifdef HLE_AUTOPLAY
    debugf("[rally] hurt %.1f, %.1f to win back\n", (double)lost, (double)s.rally);
#endif
}
void tower_on_boss_hurt(float dealt)
{
    float missing = character.maxHealth - character.health;
    if (s.rally > missing) s.rally = missing;           // a potion or the floor's restore took some
    if (s.rally <= 0.0f || dealt <= 0.0f) return;
    extern double nowS;
    // Rally's rate (Rally+: each hit counts 1.5x its base), and Shade's Mercy's 60% in Riposte's window
    float ratio = tower_boon_stacks(BOON_RALLY) ? (tower_boon_stacks(BOON_RALLY_PLUS) ? 1.5f : RALLY_RATIO) : 0.0f;
    bool mercy = tower_boon_stacks(BOON_SHADES_MERCY) && nowS < s.mercyUntil;
    if (mercy) ratio += 0.6f;
    if (ratio <= 0.0f) return;
    float r = fminf(s.rally, dealt * ratio);
    if (mercy) BFX("Shade's Mercy: a hit of %.1f wins back %.1f (x%.1f)\n", (double)dealt, (double)r, (double)ratio);
    s.rally -= r;
    character.health += r;
#ifdef HLE_AUTOPLAY
    debugf("[rally] won back %.1f, %.1f left\n", (double)r, (double)s.rally);
#endif
}
void tower_tick(float dt)
{
    extern double nowS;
    for (int i = 0; i < 4; i++) if (s.boltAt[i] > 0.0 && nowS >= s.boltAt[i]) {
        s.boltAt[i] = 0.0;
#ifdef HLE_LOG_BOONFX
        Boss *b = boss_get_instance(); float h0 = b ? b->health : 0.0f;
#endif
        character_call_lightning(s.boltDmg[i]);
        BFX("a queued bolt of %.0f lands: boss %.1f -> %.1f\n", (double)s.boltDmg[i], (double)h0, (double)(b ? b->health : 0.0f));
    }
    if (s.bleedLeft > 0.0f && (s.bleedTick -= dt) <= 0.0f) {     // Hemorrhage: a tick every 0.5 s
        s.bleedTick += 0.5f;
        float d = fminf(s.bleedLeft, s.bleedRate * 0.5f);
        s.bleedLeft -= d;
        if (s.bleedLeft < 0.01f) s.bleedLeft = 0.0f;
        Boss *b = boss_get_instance();
        if (b && b->mode == BOSS_MODE_COMBAT && b->health > 0.0f) {
            float h0 = b->health; (void)h0;
            bossHitByBleed = true; boss_apply_hit(b, d, 0.0f); bossHitByBleed = false;
            BFX("Hemorrhage: a bleed tick of %.2f (dealt %.2f), %.1f left\n", (double)d, (double)(h0 - b->health), (double)s.bleedLeft);
        } else s.bleedLeft = 0.0f;
    }
    if (s.rally <= 0.0f) return;
    if (s.rallyHold > 0.0f) {
        s.rallyHold -= dt;
        if (s.rallyHold <= 0.0f) s.rallyRate = s.rally / RALLY_DRAIN;
        return;
    }
    s.rally = fmaxf(0.0f, s.rally - s.rallyRate * dt);
}
float tower_rally_pool(void) { return s.active ? fminf(s.rally, character.maxHealth - character.health) : 0.0f; }
int   tower_mod_extra_mbd_waves(void) { return tower_boon_stacks(BOON_CRESCENT_ECHO) ? 2 : 0; }

#ifdef HLE_TEST_TOWER
void tower_test_grant_unbroken(void) { s.stacks[BOON_UNBROKEN]++; }     // test aid: the boon without the draft
void tower_test_pick_vigor(void) { s.choices[0] = BOON_VIGOR; s.nChoices = 1; s.draftOpen = true; tower_draft_pick(0); }   // test aid: Vigor taken from a draft

// Self-check of the slots, levels, a full bar, the packs, family weighting and the unlock bits
// (HLE_TEST_TOWER); returns the failures
static bool card_ok_full(int c) { return c >= BOON_COUNT || is_skill_card(c) || BOONS[c].up || BOONS[c].duo || s.stacks[c] > 0; }   // ("+" and duos take no new slot)
// how often each boon is drafted, over n drafts
static void draft_counts(int n, int counts[BOON_COUNT])
{
    memset(counts, 0, sizeof(int) * BOON_COUNT);
    for (int k = 0; k < n; k++) {
        tower_draft_open();
        for (int i = 0; i < s.nChoices; i++) if (s.choices[i] < BOON_COUNT) counts[s.choices[i]]++;
    }
    s.draftOpen = false;
}
int tower_test_slots(void)
{
    int fails = 0, counts[BOON_COUNT];
    #define CHECK(c, ...) do { if (!(c)) { fails++; debugf("[slots] FAIL: " __VA_ARGS__); debugf("\n"); } } while (0)
    // levels and slots
    tower_begin_run(11); s.packs = (1 << FAM_COUNT) - 1;
    take_boon(BOON_EMBER_EDGE);
    CHECK(s.slot[0] == BOON_EMBER_EDGE + 1 && slots_used() == 1 && s.stacks[BOON_EMBER_EDGE] == 1, "a new boon takes slot 1 at Lv 1");
    for (int i = 0; i < 9; i++) take_boon(BOON_EMBER_EDGE);
    CHECK(slots_used() == 1 && s.stacks[BOON_EMBER_EDGE] == 5 && !offerable(BOON_EMBER_EDGE), "levels stay in the slot, Ember Edge stops at Lv 5 (%d)", s.stacks[BOON_EMBER_EDGE]);
    take_boon(BOON_LEARN_HALFMOON);
    CHECK(slots_used() == 1 && knows(SKILL_HALFMOON_SLASH), "a skill card takes no slot");
    float d5 = tower_mod_damage_dealt(); s.stacks[BOON_EMBER_EDGE] = 3; float d3 = tower_mod_damage_dealt(); s.stacks[BOON_EMBER_EDGE] = 0;
    float d0 = tower_mod_damage_dealt(); s.stacks[BOON_EMBER_EDGE] = 5;
    CHECK(fabsf(d5 / d0 - 1.65f) < 0.001f && fabsf(d3 / d0 - 1.45f) < 0.001f, "Ember Edge Lv 3 x1.45, Lv 5 x1.65 (%.3f, %.3f)", (double)(d3 / d0), (double)(d5 / d0));
    float t0 = tower_mod_damage_taken();
    take_boon(BOON_IRON_SKIN); take_boon(BOON_IRON_SKIN); take_boon(BOON_IRON_SKIN);
    CHECK(fabsf(tower_mod_damage_taken() / t0 - 0.85f * 0.85f * 0.9f) < 0.001f, "Iron Skin Lv 3: x0.650 taken (%.3f)", (double)(tower_mod_damage_taken() / t0));
    take_boon(BOON_STORM_BLADE); take_boon(BOON_STATIC);
    float bolts[4]; for (int i = 0; i < 4; i++) bolts[i] = tower_on_sword_hit(false);
    CHECK(bolts[0] == 0.0f && bolts[3] == 9.0f, "Storm Blade + Static Lv 1: hit 4 calls 9 (%.0f)", (double)bolts[3]);
    CHECK(slots_used() == 4 && offerable(BOON_VIGOR) && offerable(BOON_HEMORRHAGE), "4 slots: a new boon still offered");
    take_boon(BOON_SHADOW_EDGE);
    CHECK(slots_used() == 5 && s.slot[4] == BOON_SHADOW_EDGE + 1, "the fifth boon in slot 5");
    CHECK(!offerable(BOON_VIGOR) && !offerable(BOON_HEMORRHAGE) && offerable(BOON_STATIC) && offerable(BOON_SHADOW_EDGE) && !offerable(BOON_STORM_BLADE),
          "a full bar: no new boon, levels of the slotted ones below their max");
    take_boon(BOON_VIGOR);
    CHECK(slots_used() == 5 && s.stacks[BOON_VIGOR] == 1, "(a forced sixth has no slot)");
    s.stacks[BOON_VIGOR] = 0;
    draft_counts(500, counts);
    { int bad = 0; for (int b = 0; b < BOON_COUNT; b++) if (counts[b] && !card_ok_full(b)) { bad++; debugf("[slots] full bar offered %s\n", BOONS[b].name); }
      CHECK(!bad, "a full bar's drafts hold only slotted boons, skills and pacts (%d bad)", bad);
      debugf("[slots] full bar, 500 drafts: Static %d, Shadow Edge %d, Iron Skin %d, Halfmoon+ %d\n", counts[BOON_STATIC], counts[BOON_SHADOW_EDGE], counts[BOON_IRON_SKIN], counts[BOON_HALFMOON_PLUS]); }
    // the packs: none bought, none of their boons; the Storm pack, only its own
    tower_begin_run(12); s.packs = 0;
    draft_counts(500, counts);
    { int n = 0; for (int b = 0; b < BOON_COUNT; b++) if (BOONS[b].pack) n += counts[b];
      CHECK(!n, "no pack bought: no pack boon in 500 drafts (%d)", n); }
    s.packs = 1 << FAM_STORM;
    draft_counts(500, counts);
    { int in = 0, out = 0; for (int b = 0; b < BOON_COUNT; b++) if (BOONS[b].pack) { if (BOONS[b].family == FAM_STORM) in += counts[b]; else out += counts[b]; }
      CHECK(in > 0 && !out, "the Storm pack: its boons offered (%d), no other pack's (%d)", in, out); }
    // family weighting: two Storm boons slotted double the Storm Rares' odds; one doesn't
    for (int pass = 0; pass < 2; pass++) {
        tower_begin_run(13); s.packs = (1 << FAM_COUNT) - 1;
        take_boon(BOON_STORM_BLADE);
        if (pass) take_boon(BOON_STATIC); else take_boon(BOON_VIGOR);
        draft_counts(4000, counts);
        float storm = 0, other = 0; int ns = 0, no = 0;
        for (int b = 0; b < BOON_COUNT; b++) {
            if (BOONS[b].tier != TIER_RARE || is_skill_card(b) || !offerable(b)) continue;
            if (BOONS[b].family == FAM_STORM) { storm += counts[b]; ns++; } else { other += counts[b]; no++; }
        }
        float r = (ns && no && other > 0) ? (storm / ns) / (other / no) : 0.0f;
        debugf("[slots] weighting, %s: a Storm Rare drafted %.2fx as often as another Rare (%d and %d of them)\n",
               pass ? "Storm Blade + Static" : "Storm Blade + Vigor", (double)r, ns, no);
        if (pass) CHECK(r > 1.6f && r < 2.4f, "two of a family: about twice as often (%.2f)", (double)r);
        else      CHECK(r > 0.8f && r < 1.25f, "one of a family: as often (%.2f)", (double)r);
    }
    tower_end_run();
    // the unlock bits in _pad[24..25] and the ladder's prices; the slot is put back as it was
    {
        TowerSave was, t; tower_load(&was);
        const SaveData *d = save_controller_get_slot_data(save_controller_get_active_slot());
        uint8_t p25 = d->_pad[25], p26 = d->_pad[26], p27 = d->_pad[27];
        t = was; t.unlocks = 0x1FF; save_controller_set_tower(&t); tower_load(&t);
        CHECK(t.unlocks == 0x1FF && d->_pad[24] == 0xFF && (d->_pad[25] & 1) && (d->_pad[25] & ~1) == (p25 & ~1) && d->_pad[26] == p26 && d->_pad[27] == p27
              && t.embers == was.embers && t.bestFloor == was.bestFloor && t.relicsOwned == was.relicsOwned, "9 bits stored in _pad[24..25], nothing else touched");
        t.unlocks = 0; t.embers = 1000; save_controller_set_tower(&t);
        int c0 = tower_pack_cost(2);
        bool b0 = tower_pack_buy(2);
        int c1 = tower_pack_cost(0), c2 = tower_pack_cost(2);
        bool b1 = tower_pack_buy(0), again = tower_pack_buy(2);
        tower_load(&t);
        CHECK(c0 == 120 && b0 && c1 == 160 && c2 == -1 && b1 && !again && t.embers == 1000 - 280 && t.unlocks == ((1 << pack_family(2)) | (1 << pack_family(0))),
              "the ladder: 120 then 160 whichever pack, a pack bought once (%d %d %d, Embers %d, unlocks %03x)", c0, c1, c2, t.embers, t.unlocks);
        save_controller_set_tower(&was);
    }
    #undef CHECK
    debugf("[slots] self-check: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    return fails;
}

static int duos_offerable(void) { int n = 0; for (int b = 0; b < BOON_COUNT; b++) n += BOONS[b].duo && offerable(b); return n; }

// Stage 2's self-check: rivals, "+" and duos (HLE_TEST_TOWER); returns the failures
int tower_test_stage2(void)
{
    int fails = 0, counts[BOON_COUNT];
    extern double nowS;
    #define CHECK(c, ...) do { if (!(c)) { fails++; debugf("[stage2] FAIL: " __VA_ARGS__); debugf("\n"); } } while (0)
    #define RUN(seed) do { tower_begin_run(seed); s.packs = (1 << FAM_COUNT) - 1; } while (0)
    #define MAX(b) do { while (s.stacks[b] < BOONS[b].maxStacks) take_boon(b); } while (0)
    // duos: only with a maxed boon of each of two different families slotted
    RUN(21);
    MAX(BOON_STORM_BLADE);
    CHECK(!duos_offerable(), "one maxed boon: no duo (%d)", duos_offerable());
    take_boon(BOON_THUNDERCLAP); take_boon(BOON_CONDUCTOR);
    CHECK(!duos_offerable(), "three maxed Storm boons (one family): no duo (%d)", duos_offerable());
    take_boon(BOON_BLOODTHIRST);
    CHECK(!offerable(BOON_CRIMSON_STORM), "Bloodthirst Lv 1 of 2 (not maxed): no Crimson Storm");
    take_boon(BOON_BLOODTHIRST);
    CHECK(offerable(BOON_CRIMSON_STORM) && duos_offerable() == 1, "Bloodthirst maxed + Storm Blade: Crimson Storm, the only duo (%d)", duos_offerable());
    s.packs = 1 << FAM_BLOOD;
    CHECK(!offerable(BOON_CRIMSON_STORM), "the Storm pack not bought: no Crimson Storm");
    s.packs = (1 << FAM_COUNT) - 1;
    // the merge: a full bar, the duo takes Bloodthirst's slot, Storm Blade's opens; both keep working
    take_boon(BOON_VIGOR);
    CHECK(slots_used() == 5 && !offerable(BOON_HEMORRHAGE) && offerable(BOON_CRIMSON_STORM), "a full bar still offers the duo");
    draft_counts(500, counts);
    debugf("[stage2] full bar, 500 drafts: Crimson Storm %d, Static %d\n", counts[BOON_CRIMSON_STORM], counts[BOON_STATIC]);
    CHECK(counts[BOON_CRIMSON_STORM] > 0, "the full bar's drafts hold the duo (%d)", counts[BOON_CRIMSON_STORM]);
    int sBt = -1, sSb = -1;
    for (int i = 0; i < TOWER_SLOTS; i++) { if (s.slot[i] == BOON_BLOODTHIRST + 1) sBt = i; if (s.slot[i] == BOON_STORM_BLADE + 1) sSb = i; }
    take_boon(BOON_CRIMSON_STORM);
    CHECK(slots_used() == 4 && s.slot[sBt] == BOON_CRIMSON_STORM + 1 && !s.slot[sSb] && s.merged[sBt][0] == BOON_BLOODTHIRST + 1 && s.merged[sBt][1] == BOON_STORM_BLADE + 1,
          "the duo merges Bloodthirst and Storm Blade into one slot and opens the other (%d used)", slots_used());
    CHECK(s.stacks[BOON_BLOODTHIRST] == 2 && s.stacks[BOON_STORM_BLADE] == 1 && !offerable(BOON_CRIMSON_STORM) && offerable(BOON_HEMORRHAGE),
          "both stay at their max, the duo once, a new boon fits again");
    { s.hitCount = 0; float b4 = 0; for (int i = 0; i < 4; i++) b4 = tower_on_sword_hit(false);
      CHECK(b4 == 6.0f, "merged Storm Blade still calls lightning on hit 4 (%.0f)", (double)b4); }
    { character.maxHealth = 200.0f; character.health = 100.0f; float d = tower_on_lightning(6.0f, false);
      CHECK(d == 6.0f && fabsf(character.health - 100.0f - 0.5f * 0.24f * 6.0f) < 0.01f, "Crimson Storm: a bolt of 6 heals 0.72 with Bloodthirst Lv 2 (%.2f, bolt %.1f)", (double)(character.health - 100.0f), (double)d); }
    { char l[TOWER_BUILD_ROWS][40], r[TOWER_BUILD_ROWS][12]; int st[TOWER_BUILD_ROWS];
      int n = tower_build_rows(l, r, st, TOWER_BUILD_ROWS); bool duoRow = false, both = false; int lock = 0;
      for (int i = 0; i < n; i++) { duoRow |= !strcmp(l[i], "Crimson Storm") && !strcmp(r[i], "Duo"); both |= strstr(l[i], "Bloodthirst + Storm Blade") != NULL;
                                    lock += (strstr(l[i], "Executioner") != NULL) + (strstr(l[i], "Moon Mastery") != NULL) + (strstr(l[i], "Last Stand") != NULL);
                                    debugf("[stage2] build row %d: %s | %s\n", i, l[i], r[i]); }
      CHECK(duoRow && both && lock == 3, "the Build page: the duo, its two boons, the 3 rivals locked (%d rows)", n); }
    // Crimson Storm without Bloodthirst: +25%
    RUN(22); MAX(BOON_RALLY); MAX(BOON_STORM_BLADE); take_boon(BOON_CRIMSON_STORM);
    CHECK(fabsf(tower_on_lightning(8.0f, false) - 10.0f) < 0.01f, "Crimson Storm, no Bloodthirst: a bolt x1.25");
    // rivals: never both offered in a draft, never both held, locked for the run
    RUN(23);
    { int both = 0, seen[2] = { 0, 0 };
      for (int k = 0; k < 3000; k++) {
          tower_draft_open();
          for (unsigned p = 0; p < sizeof RIVALS / sizeof RIVALS[0]; p++) {
              bool a = false, b = false;
              for (int i = 0; i < s.nChoices; i++) { a |= s.choices[i] == RIVALS[p][0]; b |= s.choices[i] == RIVALS[p][1]; }
              both += a && b; seen[0] += a; seen[1] += b;
          }
      }
      s.draftOpen = false;
      debugf("[stage2] rivals, 3000 drafts: a rival pair together %d times (left offered %d, right %d)\n", both, seen[0], seen[1]);
      CHECK(!both && seen[0] && seen[1], "never both rivals in one draft (%d)", both); }
    { int held = 0;
      for (unsigned p = 0; p < sizeof RIVALS / sizeof RIVALS[0]; p++) {
          RUN(24);
          MAX((BoonId)RIVALS[p][0]);
          if (offerable(RIVALS[p][1]) || !s.locked[RIVALS[p][1]]) held++;
          s.stacks[RIVALS[p][0]] = 0;     // as if released (stage 3): still locked
          for (int i = 0; i < TOWER_SLOTS; i++) if (s.slot[i] == RIVALS[p][0] + 1) s.slot[i] = 0;
          if (offerable(RIVALS[p][1])) held++;
          draft_counts(300, counts);
          if (counts[RIVALS[p][1]]) held++;
      }
      CHECK(!held, "a rival taken locks its pair out for the run (%d bad)", held); }
    // "+": only at the max level, no slot, once
    RUN(25);
    for (int i = 0; i < 4; i++) take_boon(BOON_EMBER_EDGE);
    CHECK(!offerable(BOON_EMBER_EDGE_PLUS), "Ember Edge Lv 4: no Ember Edge+");
    take_boon(BOON_EMBER_EDGE);
    CHECK(offerable(BOON_EMBER_EDGE_PLUS), "Ember Edge Lv 5 (its max): Ember Edge+ offered");
    { int plusBelow = 0;
      for (int b = 0; b < BOON_COUNT; b++) if (BOONS[b].up) {
          RUN(26); BoonId u = (BoonId)(BOONS[b].up - 1);
          for (int l = 0; l < BOONS[u].maxStacks; l++) { if (offerable(b)) plusBelow++; take_boon(u); }
          if (!offerable(b)) plusBelow++;
      }
      CHECK(!plusBelow, "every \"+\" offered only at its boon's max (%d bad)", plusBelow); }
    RUN(25); MAX(BOON_EMBER_EDGE);
    take_boon(BOON_EMBER_EDGE_PLUS);
    CHECK(slots_used() == 1 && !offerable(BOON_EMBER_EDGE_PLUS), "a \"+\" takes no slot, once");
    // the "+" and duo effects, by the numbers
    RUN(27); MAX(BOON_STORM_BLADE); take_boon(BOON_STORM_BLADE_PLUS);
    { int n = 0; s.hitCount = 0; for (int i = 0; i < 9; i++) n += tower_on_sword_hit(false) > 0.0f;
      CHECK(n == 3, "Storm Blade+: every 3rd hit (%d bolts in 9)", n); }
    RUN(27); MAX(BOON_EXECUTIONER); take_boon(BOON_EXECUTIONER_PLUS);
    CHECK(fabsf(tower_mod_stagger_damage() - 1.9f) < 0.001f, "Executioner+: +90%%");
    RUN(27); MAX(BOON_PHANTOM_STEP); take_boon(BOON_PHANTOM_STEP_PLUS);
    CHECK(fabsf(tower_mod_perfect_window() - 0.31f) < 0.001f, "Phantom Step+: 0.31 s");
    RUN(27); s.skills = 1; float cd0 = tower_mod_skill_cooldown(); MAX(BOON_MOON_MASTERY); float cd3 = tower_mod_skill_cooldown(); take_boon(BOON_MOON_MASTERY_PLUS);
    CHECK(fabsf(cd3 / cd0 - 0.512f) < 0.001f && fabsf(tower_mod_skill_cooldown() / cd0 - 0.421875f) < 0.001f, "Moon Mastery Lv 3 x0.512, + x0.422 (%.3f)", (double)(tower_mod_skill_cooldown() / cd0));
    RUN(27); MAX(BOON_BLOODTHIRST); take_boon(BOON_BLOODTHIRST_PLUS);
    CHECK(fabsf(tower_mod_lifesteal() - 0.32f) < 0.001f, "Bloodthirst+: 16%% a level (%.2f)", (double)tower_mod_lifesteal());
    RUN(27); MAX(BOON_IRON_SKIN); take_boon(BOON_IRON_SKIN_PLUS);
    { float a = tower_mod_blow(20.0f), b = tower_mod_blow(20.0f); tower_next_floor(); float c = tower_mod_blow(20.0f);
      CHECK(a == 10.0f && b == 20.0f && c == 10.0f, "Iron Skin+: the floor's first blow half (%.0f %.0f, next floor %.0f)", (double)a, (double)b, (double)c); }
    RUN(27); MAX(BOON_UNBROKEN); character.maxHealth = 150.0f; take_boon(BOON_UNBROKEN_PLUS);
    CHECK(fabsf(tower_mod_unbroken_heal() - 0.2f * character.maxHealth) < 0.01f, "Unbroken+: heals 20%%");
    RUN(27); MAX(BOON_STATIC); take_boon(BOON_STATIC_PLUS);
    { float w = 0; s.hitCount = 0; for (int i = 0; i < 8; i++) w += tower_on_sword_hit(false);
      CHECK(fabsf(w - 1.5f * (6.0f + 3.0f * 3)) < 0.01f, "Static+ (Lv 4, no Storm Blade): the 8th hit's bolt x1.5 (%.1f)", (double)w); }
    RUN(27); MAX(BOON_THUNDERCLAP); take_boon(BOON_THUNDERCLAP_PLUS);
    CHECK(tower_on_sword_hit(true) == 12.0f, "Thunderclap+: the finisher's bolt 12");
    RUN(28); MAX(BOON_CRUSHING_BLOWS); MAX(BOON_STORM_BLADE); take_boon(BOON_ANVIL_OF_STORMS);
    CHECK(tower_on_lightning(6.0f, true) == 12.0f && tower_on_lightning(6.0f, false) == 6.0f, "Anvil of Storms: x2 on a staggered boss only");
    RUN(29); MAX(BOON_RALLY); s.skills = 1; MAX(BOON_MOON_MASTERY); take_boon(BOON_BLOOD_MOON_RITE);
    { character.maxHealth = 200.0f; character.health = 100.0f; tower_on_skill_cast();
      CHECK(fabsf(character.health - 94.0f) < 0.01f && fabsf(tower_mod_damage_vs(1.0f, false, TOWER_HIT_SKILL) - 1.4f) < 0.001f && tower_mod_damage_vs(1.0f, false, TOWER_HIT_SWORD) == 1.0f,
            "Blood Moon Rite: a cast costs 3%% (%.1f left of 100), skills x1.4", (double)character.health);
      character.health = 1.0f; tower_on_skill_cast(); CHECK(character.health == 1.0f, "Blood Moon Rite never takes the last health"); }
    RUN(30); MAX(BOON_KINDLED_FLASK); MAX(BOON_CRUSHING_BLOWS); take_boon(BOON_STEEL_HEARTH);
    { float m0 = character.maxHealth, t0 = tower_mod_damage_taken();
      for (int i = 0; i < 4; i++) tower_on_potion();
      float m4 = character.maxHealth, t4 = tower_mod_damage_taken();
      nowS += 5.0; float t5 = tower_mod_damage_taken(); nowS -= 5.0;
      tower_next_floor();
      CHECK(m4 - m0 == 30.0f && fabsf(t4 / t0 - 0.6f) < 0.001f && fabsf(t5 / t0 - 1.0f) < 0.001f && character.maxHealth == m0,
            "Steel Hearth: +10 max health a potion up to +30 (%.0f), x0.6 taken for 4 s (%.2f, after %.2f), gone the next floor (%.0f)", (double)(m4 - m0), (double)(t4 / t0), (double)(t5 / t0), (double)character.maxHealth); }
    RUN(31); { float m0 = character.maxHealth; take_boon(BOON_VIGOR); CHECK(character.maxHealth - m0 == 25.0f, "Vigor raises max health in the run (%.0f)", (double)(character.maxHealth - m0)); }
    tower_end_run();
    character.maxHealth = tower_mod_max_health(); character.health = character.maxHealth;
    #undef CHECK
    #undef RUN
    #undef MAX
    debugf("[stage2] self-check: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    return fails;
}

// Stage 3's self-check (HLE_TEST_TOWER): 1,000 seeded maps against section 2's rules (checked path by
// path, apart from map_rules_ok), walks through whole circles by the screens' own presses, a Rest's
// choices and Release, Elites; returns the failures
static int s_mapFails;
#define MCHECK(c, ...) do { if (!(c)) { s_mapFails++; debugf("[map] FAIL: " __VA_ARGS__); debugf("\n"); } } while (0)
static bool test_press(int key)     // a press on the screens between fights, past their input delay
{
    s.scrOpenMs = s.draftOpenMs = get_ticks_ms() - DRAFT_INPUT_DELAY_MS - 1;
    return tower_between_update(key == 'L', key == 'R', key == 'U', key == 'D', key == 'A');
}
// One circle through the map from its first screen: random moves, A or Up to enter; a Rest heals and
// keeps the boons; a fight is won (with its draft's first card, or none); returns its fights
static int walk_circle(bool drafts)
{
    int c = s.circle, fights = 0, floor0 = s.floor, nb = 0, last = -1;
    uint8_t seen = 0; bool distinct = true, onlyReach = true;
    MCHECK(tower_map_is_open() && s.map.row == -1 && map_reachable(0, 1) && !map_reachable(0, 0) && !map_reachable(0, 2) && s.boss == TOWER_GUARDIAN,
           "circle %d: the map is up before its first fight, the Guardian the only node to enter", c);
    for (int step = 0; step < 12 && s.circle == c; step++) {
        for (int m = rng_next() % 3; m > 0; m--) test_press(rng_next() % 2 ? 'L' : 'R');
        onlyReach &= map_reachable(s.map.row + 1, s.map.sel);
        if (!test_press(rng_next() % 2 ? 'A' : 'U')) {      // a Rest
            MCHECK(tower_rest_is_open() && s.map.type[s.map.row][s.map.path[s.map.row]] == NODE_REST, "a node that isn't a fight opens a Rest");
            test_press('A');
            if (s.scr == SCR_RELEASE) test_press('A');
            MCHECK(tower_map_is_open() && s.floor == floor0 + fights, "after a Rest, the map again, no floor counted (%d)", s.floor);
            continue;
        }
        fights++;
        MCHECK(s.floor == floor0 + fights - 1 && s.map.fights == fights && is_fight(s.map.type[s.map.row][s.map.path[s.map.row]]), "a fight is the next floor (%d)", s.floor);
        if (fights == 1) MCHECK(s.boss == TOWER_GUARDIAN, "the Guardian first (%d)", s.boss);
        if (s.boss >= TOWER_FALLEN_KNIGHT) { if (nb < 3 && ((seen >> s.boss) & 1)) distinct = false; seen |= 1 << s.boss; nb++; }
        last = s.boss;
        if (drafts) { tower_floor_cleared(); test_press('A'); }
        else { tower_next_floor(); tower_map_open(); }
    }
    MCHECK(onlyReach, "the cursor stays on reachable nodes");
    MCHECK(last == TOWER_UNBOUND && fights >= 4 && fights <= 6 && distinct && s.circle == c + 1 && s.floor == floor0 + fights,
           "circle %d: Unbound last (%d), 4-6 fights (%d), no Bonus boss twice before all three (%d), then circle %d (floor %d)", c, last, fights, distinct, s.circle, s.floor);
    return fights;
}
int tower_test_map(void)
{
    s_mapFails = 0;
    bool skillsWere = character_skills_enabled();
    character_set_skills_enabled(true);
    // 1. 1,000 maps
    int count[NODE_TYPES] = { 0 }, tries = 0, most = 0, bad = 0, paths[8] = { 0 };
    for (uint32_t seed = 1; seed <= 1000; seed++) {
        s.rng = seed * 2654435761u | 1;
        map_generate();
        tries += s_mapTries; if (s_mapTries > most) most = s_mapTries;
        int b = 0;
        b += s.map.type[0][0] || s.map.type[0][2] || s.map.type[0][1] != NODE_BOSS || s.map.type[6][0] || s.map.type[6][2] || s.map.type[6][1] != NODE_BOSS;  // rows 1 and 7: one node, a boss
        for (int l = 0; l < MAP_LANES; l++) {
            int f = 0;
            b += s.map.type[5][l] != NODE_REST;                                         // row 6: a Rest on every lane
            for (int r = 1; r <= 4; r++) {
                int t = s.map.type[r][l];
                count[t]++; f += t == NODE_FIGHT;
                b += t != NODE_FIGHT && t != NODE_ELITE && t != NODE_REST;              // stage 3: no Events or Shops
                b += r == 1 && t == NODE_ELITE;                                         // no Elite in row 2
            }
            b += f < 2;                                                                 // 2 Fights a lane at least
        }
        for (int p = 0; p < 243; p++) {     // every path: a lane in row 2, then up or a diagonal each row to row 6
            int lane[MAP_ROWS] = { 1, p % 3 }, q = p / 3, fights = 0; bool ok = true;
            for (int r = 2; r <= 5; r++, q /= 3) { lane[r] = lane[r - 1] + q % 3 - 1; ok &= lane[r] >= 0 && lane[r] < MAP_LANES; }
            if (!ok) continue;
            lane[6] = 1;
            for (int r = 0; r < MAP_ROWS; r++) {
                int t = s.map.type[r][lane[r]];
                fights += is_fight(t);
                if (r < MAP_ROWS - 1) { int u = s.map.type[r + 1][lane[r + 1]]; b += (t == NODE_REST || t == NODE_SHOP) && u == t; }     // no two Rests or Shops in a row
            }
            paths[fights]++;
            b += fights < 4 || fights > 6;                                              // 4-6 fights a path
        }
        if (b && bad++ < 3) debugf("[map] seed %lu breaks %d rules\n", (unsigned long)seed, b);
    }
    int cells = 12000;
    debugf("[map] 1000 maps: %d break a rule; rows 2-5: Fight %d.%d%%, Elite %d.%d%%, Rest %d.%d%%, Event %d, Shop %d; %d.%d rolls a map (most %d); paths with 4/5/6 fights: %d %d %d\n",
           bad, count[NODE_FIGHT] * 100 / cells, count[NODE_FIGHT] * 1000 / cells % 10, count[NODE_ELITE] * 100 / cells, count[NODE_ELITE] * 1000 / cells % 10,
           count[NODE_REST] * 100 / cells, count[NODE_REST] * 1000 / cells % 10, count[NODE_EVENT], count[NODE_SHOP], tries / 1000, tries / 100 % 10, most, paths[4], paths[5], paths[6]);
    MCHECK(!bad, "every map keeps the rules (%d don't)", bad);
    MCHECK(count[NODE_ELITE] && count[NODE_REST] && count[NODE_FIGHT], "all three kinds rolled");
    // the generator's own check turns each rule down
    // the generator's own check turns each rule down, one at a time on a map of Fights
    { int no = 0;
      #define FIGHTS() for (int r = 1; r <= 4; r++) for (int l = 0; l < MAP_LANES; l++) s.map.type[r][l] = NODE_FIGHT
      FIGHTS(); bool base = map_rules_ok();
      s.map.type[1][0] = s.map.type[3][0] = s.map.type[4][0] = NODE_ELITE; no += !map_rules_ok(); FIGHTS();   // a lane with 1 Fight
      s.map.type[1][2] = NODE_ELITE; no += !map_rules_ok(); FIGHTS();                                         // an Elite in row 2
      s.map.type[4][0] = NODE_REST; no += !map_rules_ok(); FIGHTS();                                          // a Rest under row 6's
      s.map.type[2][1] = s.map.type[3][2] = NODE_REST; no += !map_rules_ok(); FIGHTS();                       // a Rest, then a diagonal Rest
      #undef FIGHTS
      MCHECK(base && no == 4, "map_rules_ok turns down each broken rule (%d of 4)", no); }
    // 2. walks: whole circles by the screens' presses, with drafts (their Embers and best floor put back after)
    TowerSave was; tower_load(&was);
    int hist[8] = { 0 };
    for (uint32_t seed = 1; seed <= 40; seed++) {
        tower_begin_run(seed);
        character.maxHealth = tower_mod_max_health(); character.health = character.maxHealth; character.healthPotions = START_POTIONS;
        tower_map_open();
        for (int c = 0; c < 2; c++) hist[walk_circle(true)]++;
    }
    debugf("[map] 80 circles walked: %d with 4 fights, %d with 5, %d with 6\n", hist[4], hist[5], hist[6]);
    save_controller_set_tower(&was);
    // 3. the circle's scaling, by maps (no drafts: no skills offered, no nights)
    tower_begin_run(7); tower_map_open();
    walk_circle(false);
    MCHECK(fabsf(tower_boss_health_scale(1) - 1.0f) < 0.01f, "circle 2: x1.0 (%.2f)", (double)tower_boss_health_scale(1));
    for (int c = 0; c < 4; c++) walk_circle(false);
    MCHECK(s.circle == 6 && fabsf(tower_boss_health_scale(1) - 2.3f) < 0.01f, "circle 6: 0.7 + 4 x 0.3 + 0.4 (%.2f)", (double)tower_boss_health_scale(1));
    // 4. a Rest's choices, by its screens: heal 40%, +2 potions, Temper, Release (free), a row that can't be chosen passed by
    #define RUN(seed) do { tower_begin_run(seed); s.packs = (1 << FAM_COUNT) - 1; s.relicFx[0] = s.relicFx[1] = 0; character.maxHealth = tower_mod_max_health(); } while (0)
    RUN(41);
    float mh0 = character.maxHealth;
    character.health = 50.0f; character.healthPotions = 1;
    list_open(SCR_REST); test_press('A');
    MCHECK(fabsf(character.health - 50.0f - 0.4f * mh0) < 0.01f && tower_map_is_open(), "heal 40%%: 50 -> %.0f of %.0f, no boon to release: the map", (double)character.health, (double)mh0);
    list_open(SCR_REST); test_press('D');
    MCHECK(s.listSel == REST_POTIONS_ROW, "nothing to Temper: Down passes it (row %d)", s.listSel);
    test_press('A');
    MCHECK(character.healthPotions == 3, "+2 potions: 1 -> 3 (%d)", character.healthPotions);
    character.healthPotions = POTION_CAP - 1; list_open(SCR_REST); s.listSel = REST_POTIONS_ROW; test_press('A');
    MCHECK(character.healthPotions == POTION_CAP, "+2 potions stop at the cap of %d (%d)", POTION_CAP, character.healthPotions);
    list_open(SCR_REST);
    MCHECK(!row_ok(REST_POTIONS_ROW) && !row_ok(REST_TEMPER_ROW) && row_ok(REST_HEAL_ROW), "potions full and nothing to Temper: only Heal");
    take_boon(BOON_EMBER_EDGE); take_boon(BOON_STORM_BLADE); take_boon(BOON_LEARN_HALFMOON);
    list_open(SCR_REST); test_press('D'); test_press('A');
    MCHECK(s.scr == SCR_TEMPER && s.listN == 4 && s.list[0] == BOON_EMBER_EDGE && s.list[1] == BOON_HALFMOON_PLUS && s.list[2] == BOON_STORM_BLADE_PLUS && s.list[3] == -1,
           "Temper's list: Ember Edge's level, Halfmoon Slash+, Storm Blade+ (a maxed boon's \"+\"), Back (%d rows)", s.listN);
    test_press('D'); test_press('D'); test_press('D'); test_press('A');
    MCHECK(s.scr == SCR_REST, "Back: the Rest's choices again");
    test_press('D'); test_press('A'); test_press('A');
    MCHECK(s.stacks[BOON_EMBER_EDGE] == 2 && s.scr == SCR_RELEASE && s.listN == 3 && s.listSel == 0, "Temper: Ember Edge Lv 1 -> 2, then Release (Keep first)");
    take_boon(BOON_STORM_BLADE_PLUS);
    test_press('D'); test_press('D'); test_press('A');
    MCHECK(!s.stacks[BOON_STORM_BLADE] && !s.stacks[BOON_STORM_BLADE_PLUS] && !s.slot[1] && slots_used() == 1 && s.locked[BOON_EXECUTIONER] && !offerable(BOON_EXECUTIONER)
           && offerable(BOON_STORM_BLADE) && tower_map_is_open(), "Release: Storm Blade and its \"+\" gone, its slot open, its rival still locked, the map");
    take_boon(BOON_VIGOR);
    MCHECK(s.slot[1] == BOON_VIGOR + 1, "the opened slot takes the next new boon");
    for (int i = 0; i < 4; i++) take_boon(BOON_VIGOR);
    character.health = character.maxHealth;
    list_open(SCR_RELEASE); test_press('D'); test_press('D'); test_press('A');
    MCHECK(character.maxHealth == mh0 && character.health == mh0 && !s.stacks[BOON_VIGOR], "Release Vigor Lv 5: max health %.0f -> %.0f (%.0f, health %.0f)", (double)(mh0 + 125.0f), (double)mh0, (double)character.maxHealth, (double)character.health);
    RUN(42);
    while (s.stacks[BOON_BLOODTHIRST] < 2) take_boon(BOON_BLOODTHIRST);
    take_boon(BOON_STORM_BLADE); take_boon(BOON_CRIMSON_STORM);
    list_open(SCR_RELEASE); test_press('D'); test_press('A');
    MCHECK(!s.stacks[BOON_CRIMSON_STORM] && !s.stacks[BOON_BLOODTHIRST] && !s.stacks[BOON_STORM_BLADE] && !slots_used(), "Release a duo: all three gone (%d slots used)", slots_used());
    // 5. Elites: x1.25 health (Bonus bosses only), a Rare-or-better draft, x1.5 Embers
    RUN(43);
    s.elite = true; s.map.fights = 2;
    { int counts[BOON_COUNT], common = 0, cards = 0;
      draft_counts(500, counts);
      for (int b = 0; b < BOON_COUNT; b++) { cards += counts[b]; if (BOONS[b].tier == TIER_COMMON) common += counts[b]; }
      float h3 = tower_boss_health_scale(3), h1 = tower_boss_health_scale(1); int e = floor_embers();
      s.elite = false;
      float n3 = tower_boss_health_scale(3); int ne = floor_embers();
      debugf("[map] elite: 500 drafts, %d boon cards, %d Common; Bonus health x%.3f (plain x%.3f), Guardian x%.2f; Embers %d (plain %d)\n", cards, common, (double)h3, (double)n3, (double)h1, e, ne);
      MCHECK(!common && cards > 1000 && fabsf(h3 / n3 - 1.25f) < 0.001f && h1 == tower_boss_health_scale(1) && e == 17 && ne == 11,
             "an Elite: no Common card, x1.25 health on a Bonus boss only, Embers x1.5 (17 for 11)"); }
    #undef RUN
    tower_end_run();
    character.maxHealth = tower_mod_max_health(); character.health = character.maxHealth;
    character_set_skills_enabled(skillsWere);
    debugf("[map] self-check: %s (%d failed)\n", s_mapFails ? "FAIL" : "ok", s_mapFails);
    return s_mapFails;
}
#undef MCHECK
#endif

bool tower_mod_try_unbroken(void)
{
    if (!tower_boon_stacks(BOON_UNBROKEN) || s.unbrokenUsed) return false;
    s.unbrokenUsed = true;
    return true;
}
float tower_mod_unbroken_heal(void)
{
    if (!tower_boon_stacks(BOON_UNBROKEN_PLUS)) return 0.0f;
    BFX("Unbroken+: saved, and healed %.0f\n", (double)(0.2f * character.maxHealth));
    return 0.2f * character.maxHealth;
}

float tower_boss_health_scale(int phase)
{
    if (!s.active) return 1.0f;
    int n = 0;
    // Every skill offered counts, learned or passed over: skipping them doesn't keep the bosses weak
    for (int k = 0; k < SKILL_COUNT; k++) n += character_skills_enabled() && (((s.skills | s.skillsOffered) >> k) & 1);
    int c = tower_circle(), late = c >= BOSS_HP_LATE_CIRCLE ? c - BOSS_HP_LATE_CIRCLE + 1 : 0;
    float k = (BOSS_HP_START + BOSS_HP_PER_CIRCLE * (c - 1 - late) + BOSS_HP_PER_LATE * late) * (1.0f + HP_PER_SKILL * n);
    if (s.night == NIGHT_IRON) k *= 1.25f;
    if (s.elite && phase >= 3) k *= ELITE_HP;
    return phase >= 3 ? k * BONUS_HP_IN_TOWER : k;     // phases 3+ are the Bonus bosses
}

void tower_floor_restore(void)
{
    // One potion a fallen boss, no health: the potions are the run's healing
    if (s.potionDebt > 0) s.potionDebt--;      // Thirst taken with no potion: this one is owed
    else if (character.healthPotions < POTION_CAP) character.healthPotions++;    // a potion every fallen boss, even past the run's maximum
#ifdef HLE_AUTOPLAY
    debugf("[potions] floor %d: %d (max %d, owed %d)\n", s.floor, character.healthPotions, tower_mod_max_potions(), s.potionDebt);
#endif
}
