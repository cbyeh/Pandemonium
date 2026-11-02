#include <libdragon.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "tower.h"
#include "character.h"
#include "globals.h"
#include "save_controller.h"
#include "video_controller.h"
#include "video_layout.h"
#include "joypad_utility.h"
#include "game/boss/boss.h"

// ---- Tuning ----
#define FLOORS_PER_CIRCLE   5
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
// pack: in its family's pack (bought at the Shrine), not in the pool from the start
typedef struct { const char *name, *desc; uint8_t maxStacks, tier, family, pack; } BoonDef;

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
};
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
    int floor;
    uint32_t rng;
    TowerBoss order[FLOORS_PER_CIRCLE];
    uint8_t skills;                 // learned this run (bit per CharacterSkill)
    uint8_t skillsOffered;          // whose Learn card has been on the table (taken or not)
    uint8_t stacks[BOON_COUNT];     // a boon's level (0: not held)
    uint8_t slot[TOWER_SLOTS];      // the boon slots in the order taken (BoonId + 1, 0 empty); skill cards take none
    uint16_t packs;                 // the families' packs bought at the Shrine (bit per family), for this run
    // the new boons' timers and counters
    double sanguineUntil, waxingUntil, shadowEdgeUntil;
    float bleedLeft, bleedRate, bleedTick;   // Hemorrhage: damage still to bleed, a second, the next tick
    int overcharge;                 // Overcharge: hits left that call lightning
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

// A boon taken (a draft, a relic's gift): a new one fills the next free slot at Lv 1, a held one
// levels up; skill cards take no slot
static void take_boon(BoonId b)
{
    if (BOONS[b].family != FAM_SKILL && !s.stacks[b])
        for (int i = 0; i < TOWER_SLOTS; i++) if (!s.slot[i]) { s.slot[i] = (uint8_t)(b + 1); break; }
    if (s.stacks[b] < BOONS[b].maxStacks) s.stacks[b]++;
    if (b == BOON_DEEP_FLASK) character.healthPotions = character.healthPotions + 3 > POTION_CAP ? POTION_CAP : character.healthPotions + 3;
    if (b == BOON_LEARN_MOONBLADE) s.skills |= 1 << SKILL_MOON_BLADE_DANCE;
    if (b == BOON_LEARN_HALFMOON)  s.skills |= 1 << SKILL_HALFMOON_SLASH;
    if (b == BOON_LEARN_CLEAVE)    s.skills |= 1 << SKILL_CRESCENT_CLEAVE;
    if (b == BOON_VIGOR) character.health += 25.0f;
}

static uint32_t rng_next(void)
{
    // xorshift32
    uint32_t x = s.rng ? s.rng : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s.rng = x;
    return x;
}

static void shuffle_circle(void)
{
    // The Guardian opens every circle, the three Bonus bosses follow in a random order,
    // and Guardian Unbound closes it (a floor of its own, no transformation cutscene)
    s.order[0] = TOWER_GUARDIAN;
    TowerBoss rest[3] = { TOWER_FALLEN_KNIGHT, TOWER_SHACKLED_SUN, TOWER_SHADOWBORN };
    for (int i = 2; i > 0; i--) { int j = rng_next() % (i + 1); TowerBoss t = rest[i]; rest[i] = rest[j]; rest[j] = t; }
#ifdef HLE_TOWER_SB_LAST    // test aid: the Fallen Warrior, the Sun, then Shadowborne (the heap's lowest point, the same path each run)
    rest[0] = TOWER_FALLEN_KNIGHT; rest[1] = TOWER_SHACKLED_SUN; rest[2] = TOWER_SHADOWBORN;
#endif
    for (int i = 0; i < 3; i++) s.order[1 + i] = rest[i];
    s.order[4] = TOWER_UNBOUND;
    s.night = s.nextNight;
#ifdef HLE_AUTOPLAY
    debugf("[night] circle from floor %d: %s\n", s.floor, NIGHTS[s.night].name[0] ? NIGHTS[s.night].name : "none");
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
    shuffle_circle();
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
#ifdef HLE_TOWER_GIVE   // test aid: start the run holding this boon
    take_boon(HLE_TOWER_GIVE);
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

void tower_end_run(void) { s.active = false; s.draftOpen = false; }
bool tower_active(void) { return s.active; }
int  tower_floor(void) { return s.floor; }
int  tower_circle(void) { return (s.floor - 1) / FLOORS_PER_CIRCLE + 1; }
TowerBoss tower_floor_boss(void) { return s.order[(s.floor - 1) % FLOORS_PER_CIRCLE]; }
int  tower_run_embers(void) { return s.runEmbers; }
int  tower_boon_stacks(BoonId b) { return (s.active && b < BOON_COUNT) ? s.stacks[b] : 0; }

static int floor_embers(void)
{
    float e = (8 * tower_circle() + 3 * ((s.floor - 1) % FLOORS_PER_CIRCLE)) * NIGHTS[s.night].embers;
    if (has_relic(RELIC_EMBERS)) e *= 1.2f;
    if (has_relic(RELIC_AVARICE)) e *= 1.4f;
    return (int)(e + 0.5f);
}

void tower_floor_cleared(void)
{
    if (!s.active) return;
    TowerSave t;
    tower_load(&t);
    s.circleCleared = s.floor % FLOORS_PER_CIRCLE == 0;
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
    if ((s.floor - 1) % FLOORS_PER_CIRCLE == 0) shuffle_circle();
}

// ---- Draft ----

static bool is_skill_card(BoonId b) { return BOONS[b].family == FAM_SKILL; }

static bool knows(int skill) { return (s.skills >> skill) & 1; }

// Can it be offered now? (stacks left, and an upgrade needs its skill)
// A new boon needs a free slot and, from a family's pack, the pack bought: with the bar full
// a draft offers only levels of the slotted boons (and skill cards and pacts)
static bool offerable(BoonId b)
{
    if (s.stacks[b] >= BOONS[b].maxStacks) return false;
    if (is_skill_card(b) && !character_skills_enabled()) return false;     // Skills off (the title's setting): none offered
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
    for (int i = 0; i < TOWER_SLOTS; i++) if (s.slot[i]) famHeld[BOONS[s.slot[i] - 1].family]++;
    for (int b = 0; b < BOON_COUNT; b++) {
        if (!offerable(b)) continue;
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
        int want = r < leg ? TIER_LEGENDARY : r < leg + 30 ? TIER_RARE : TIER_COMMON;
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
        pool[k] = pool[--n];
    }
#ifdef HLE_DRAFT_GALLERY     // test aid: each floor shows set cards (every boon, the longest pacts) for a look
    {
        static const int G[2][3] = {
            { PACT_CARD(PB_WOUNDED, PC_EXPOSED), PACT_CARD(PB_LAST_BREATH, PC_WEAKNESS), PACT_CARD(PB_STAGGERED, PC_IMPAIRED) },
            { PACT_CARD(PB_MIGHT, PC_THIRST), PACT_CARD(PB_BULWARK, PC_EXPOSED), PACT_CARD(PB_VITALITY, PC_FRAILTY) },
        };
        int g = (s.floor - 1) % (2 + (BOON_COUNT + 2) / 3);     // the pacts, then every boon three at a time
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
    debugf("%s\n", s.circleCleared ? " | circle cleared" : "");
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
        if (b == PB_VITALITY) character.health += 12.0f;
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
    character.maxHealth = tower_mod_max_health();     // Vigor, Vitality (set only at the run's start, they added to health but not the bar's top)
    if (character.health > character.maxHealth) character.health = character.maxHealth;
    s.draftOpen = false;
}

#define DRAFT_INPUT_DELAY_MS 750

bool tower_draft_update(bool left, bool right, bool up, bool down, bool confirm)
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

void tower_draft_draw(void)
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
    text(0, 56, SCREEN_WIDTH, ALIGN_CENTER, "Choose a Boon");

    const int cw = 96, ch = 114, gap = 6, y0 = 64;
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
        // the family's tag over the name, in its colour
        text_style(x + 4, y0 + 12, cw - 8, ALIGN_CENTER, FAMILIES[bd->family].style, FAMILIES[bd->family].name);
        int nameLines = text_style(x + 4, y0 + 27, cw - 8, ALIGN_CENTER, NAME_STYLE[bd->tier], bd->name);
        const char *desc = bd->desc;    // a Learn card names the skill's button in the layout
        if (card == BOON_LEARN_MOONBLADE || card == BOON_LEARN_HALFMOON || card == BOON_LEARN_CLEAVE) {
            snprintf(line, sizeof line, "Learn it: %s", joypad_skill_button(card - BOON_LEARN_MOONBLADE));
            desc = line;
        }
        text(x + 6, y0 + 31 + 17 * (nameLines > 1 ? nameLines : 1), cw - 12, ALIGN_CENTER, desc);     // under the name: 4 lines below one, 3 below two
        // the slot line: what taking it does to the bar
        char slotLine[40];
        if (bd->family == FAM_SKILL) snprintf(slotLine, sizeof slotLine, "Skill: no slot");
        else if (s.stacks[card]) snprintf(slotLine, sizeof slotLine, "Lv %d -> %d", s.stacks[card], s.stacks[card] + 1);
        else snprintf(slotLine, sizeof slotLine, "New: slot %d/%d", slots_used() < TOWER_SLOTS ? slots_used() + 1 : TOWER_SLOTS, TOWER_SLOTS);
        text_style(x, y0 + ch - 3, cw, ALIGN_CENTER, FONT_STYLE_GREY, slotLine);
    }
    // Skip, under the cards
    // A button of its own under the cards, framed like them: Down to it, Up back to the card
    bool skip = s.sel == s.nChoices;
    const int bw = 132, bh = 16, bx = (SCREEN_WIDTH - bw) / 2, by = y0 + ch + 5;
    flat_mode();
    rdpq_set_prim_color(skip ? RGBA32(255, 215, 90, 255) : RGBA32(110, 100, 90, 255));
    rdpq_fill_rectangle(bx - 2, by - 2, bx + bw + 2, by + bh + 2);
    rdpq_set_prim_color(skip ? RGBA32(60, 30, 20, 240) : RGBA32(30, 22, 18, 230));
    rdpq_fill_rectangle(bx, by, bx + bw, by + bh);
    snprintf(line, sizeof line, "Skip: +%d Embers", SKIP_EMBERS);
    text_style(bx, by + 12, bw, ALIGN_CENTER, skip ? FONT_STYLE_GOLD : FONT_STYLE_GREY, line);
    if (s.floor % FLOORS_PER_CIRCLE == 0) {     // a new circle: stronger, and its night
        // the next circle: stronger bosses, and its night (two lines at most)
        snprintf(line, sizeof line, "Next: floor %d, %s. %s", s.floor + 1, NIGHTS[s.nextNight].name, NIGHTS[s.nextNight].desc);
        text_style(12, y0 + ch + 36, SCREEN_WIDTH - 24, ALIGN_CENTER, FONT_STYLE_RED, line);
    } else {
        snprintf(line, sizeof line, "Next: floor %d", s.floor + 1);
        text_style(0, y0 + ch + 36, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_GREY, line);
    }
}

void tower_hud_draw(void)
{
    if (!s.active || s.draftOpen) return;     // the draft has its own header
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

int tower_build_rows(char left[][28], char right[][12], int style[], int max)
{
    static const BoonId LEARN[SKILL_COUNT] = { BOON_LEARN_MOONBLADE, BOON_LEARN_HALFMOON, BOON_LEARN_CLEAVE };
    static const BoonId PLUS[SKILL_COUNT] = { BOON_CRESCENT_ECHO, BOON_HALFMOON_PLUS, BOON_CLEAVE_PLUS };
    int n = 0;
    for (int k = 0; k < SKILL_COUNT && n < max; k++, n++) {     // the skills: button, name, known or not (and its card)
        snprintf(left[n], 28, "%s %s", joypad_skill_button(k), BOONS[LEARN[k]].name);
        snprintf(right[n], 12, "%s", !knows(k) ? "-" : s.stacks[PLUS[k]] ? "known +" : "known");
        style[n] = knows(k) ? FONT_STYLE_WHITE : FONT_STYLE_GREY;
    }
    for (int i = 0; i < TOWER_SLOTS && n < max; i++, n++) {      // the slots: name in its family's colour, its level
        if (!s.slot[i]) { snprintf(left[n], 28, "Empty slot"); right[n][0] = 0; style[n] = FONT_STYLE_GREY; continue; }
        BoonId b = (BoonId)(s.slot[i] - 1);
        snprintf(left[n], 28, "%s", BOONS[b].name);
        snprintf(right[n], 12, "Lv %d/%d", s.stacks[b], BOONS[b].maxStacks);
        style[n] = FAMILIES[BOONS[b].family].style;
    }
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
    if (kind == TOWER_HIT_SWORD && tower_boon_stacks(BOON_WAXING_MOON) && nowS < s.waxingUntil) { m *= 1.15f; BFX("Waxing Moon: sword hit x1.15\n"); }
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
float tower_mod_stagger_damage(void) { return tower_boon_stacks(BOON_EXECUTIONER) ? 1.6f : 1.0f; }
float tower_mod_perfect_window(void) { return tower_boon_stacks(BOON_PHANTOM_STEP) ? 0.28f : 0.2f; }   // (0.35 outlasted a roll's 0.32 s of invincibility: every dodge was perfect)

void tower_on_perfect_dodge(void)
{
    if (!s.active) return;
    if (tower_boon_stacks(BOON_PHANTOM_STEP)) character.health = fminf(character.maxHealth, character.health + character.maxHealth * 0.02f);
    extern double nowS;
    if (tower_boon_stacks(BOON_RIPOSTE)) s.riposteUntil = nowS + 2.5;
    if (tower_boon_stacks(BOON_VANISH) && !s.vanishUsed && !s.vanishArmed && ++s.perfectDodges >= 3) {
        s.vanishArmed = true;
        BFX("Vanish: 3 perfect dodges this floor, the next blow deals 0\n");
    }
}

#define STORM_BOLT  6.0f        // Storm Blade's lightning (+12 was best in every build: section 14 of the design)
#define STATIC_BOLT 3.0f        // Static: a level
float tower_on_sword_hit(bool finisher)
{
    if (!s.active) return 0.0f;
    s.hitCount++;
    int st = tower_boon_stacks(BOON_STATIC);
    float bolt = 0.0f, storm = STORM_BOLT + STATIC_BOLT * st;
    bool sb = tower_boon_stacks(BOON_STORM_BLADE) && s.hitCount % 4 == 0;
    if (sb) {
        bolt += storm;
#ifdef HLE_AUTOPLAY
        debugf("[boon] Storm Blade: lightning on hit %d (%.0f)\n", s.hitCount, (double)storm);
#endif
        if (st) BFX("Static: Storm Blade's bolt %.0f (+%.0f)\n", (double)storm, (double)(STATIC_BOLT * st));
    } else if (st && !tower_boon_stacks(BOON_STORM_BLADE) && s.hitCount % 8 == 0) {    // no Storm Blade: a weak bolt of its own
        float w = STORM_BOLT + STATIC_BOLT * (st - 1);
        bolt += w;
        BFX("Static: a bolt on hit %d (%.0f)\n", s.hitCount, (double)w);
    }
    if (s.overcharge > 0) {     // every hit a bolt (one already called counts)
        s.overcharge--;
        if (!sb) bolt += storm;
        BFX("Overcharge: lightning on hit %d (%.0f), %d left\n", s.hitCount, (double)storm, s.overcharge);
    }
    if (finisher && tower_boon_stacks(BOON_THUNDERCLAP)) { bolt += 8.0f; BFX("Thunderclap: the finisher calls lightning (8)\n"); }
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
}

void tower_on_skill_cast(void)
{
    extern double nowS;
    if (tower_boon_stacks(BOON_WAXING_MOON)) { s.waxingUntil = nowS + 4.0; BFX("Waxing Moon: a cast, +15%% sword damage for 4 s\n"); }
}

void tower_on_stagger(void)
{
    if (tower_boon_stacks(BOON_OVERCHARGE)) { s.overcharge = 5; BFX("Overcharge: a stagger, the next 5 hits call lightning\n"); }
}

float tower_mod_blow(float amount)
{
    if (!s.active) return amount;
    if (s.vanishArmed) {
        s.vanishArmed = false; s.vanishUsed = true;     // once a floor
        BFX("Vanish: a blow of %.1f deals 0\n", (double)amount);
        return 0.0f;
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
float tower_mod_max_health(void) { return 150.0f + 10.0f * shrine(UPG_VIGOR) + 25.0f * tower_boon_stacks(BOON_VIGOR) + 15.0f * has_relic(RELIC_HEALTH) - 20.0f * has_relic(RELIC_FRAIL) + 12.0f * (s.active ? s.bless[PB_VITALITY] : 0); }
float tower_mod_roll_cost(void) { return (tower_boon_stacks(BOON_FLEETFOOT) ? 0.6f : 1.0f) * (has_relic(RELIC_LEADEN) ? 1.25f : 1.0f); }
float tower_mod_stamina_empty(void) { return 1.0f + 0.4f * tower_boon_stacks(BOON_SECOND_WIND); }   // the wait once the bar is empty: x1/1.4 a stack
float tower_mod_stamina_regen(void) { return (1.0f + 0.4f * tower_boon_stacks(BOON_SECOND_WIND)) * (has_relic(RELIC_STAMINA) ? 1.2f : 1.0f); }
float tower_mod_skill_cooldown(void)
{
    return (1.0f - 0.08f * shrine(UPG_SKILL_MASTERY)) * powf_int(0.8f, tower_boon_stacks(BOON_MOON_MASTERY))
           * ((s.active && s.night == NIGHT_STARFALL) ? 0.7f : 1.0f);
}
float tower_mod_lifesteal(void) { return 0.12f * tower_boon_stacks(BOON_BLOODTHIRST) + 0.03f * has_relic(RELIC_VAMPIRIC); }   // of a hit before boons: it no longer grows with the damage multiplier

// Rally, at Bloodborne's pace in our numbers: lost health stays winnable 1 s, then drains away
// over 4 s; each hit wins back what it dealt before damage boons (a sword hit 4: about four hits for a typical blow,
// one Halfmoon Slash for most of a heavy one). A new blow adds to it and holds it again
#define RALLY_HOLD  1.0f
#define RALLY_DRAIN 4.0f
#define RALLY_RATIO 1.0f
void tower_on_player_hurt(float lost)
{
    if (!tower_boon_stacks(BOON_RALLY) || lost <= 0.0f) return;
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
    float r = fminf(s.rally, dealt * RALLY_RATIO);
    s.rally -= r;
    character.health += r;
#ifdef HLE_AUTOPLAY
    debugf("[rally] won back %.1f, %.1f left\n", (double)r, (double)s.rally);
#endif
}
void tower_tick(float dt)
{
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
static bool card_ok_full(int c) { return c >= BOON_COUNT || is_skill_card(c) || s.stacks[c] > 0; }
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
#endif

bool tower_mod_try_unbroken(void)
{
    if (!tower_boon_stacks(BOON_UNBROKEN) || s.unbrokenUsed) return false;
    s.unbrokenUsed = true;
    return true;
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
