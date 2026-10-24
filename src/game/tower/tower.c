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
typedef struct { const char *name, *desc; uint8_t maxStacks, tier; } BoonDef;

static const BoonDef BOONS[BOON_COUNT] = {
    [BOON_EMBER_EDGE]    = { "Ember Edge",    "+15% damage",                  3, TIER_COMMON },
    [BOON_IRON_SKIN]     = { "Iron Skin",     "-15% damage taken",            2, TIER_COMMON },
    [BOON_DEEP_FLASK]    = { "Deep Flask",    "+3 potions",                   2, TIER_COMMON },
    [BOON_KINDLED_FLASK] = { "Kindled Flask", "Potions heal 25% more",        2, TIER_COMMON },
    [BOON_FLEETFOOT]     = { "Fleetfoot",     "Rolls cost 40% less stamina",  1, TIER_RARE },
    [BOON_SECOND_WIND]   = { "Second Wind",   "+40% stamina regen",           2, TIER_COMMON },
    [BOON_MOON_MASTERY]  = { "Moon Mastery",  "Skill cooldowns -20%",         2, TIER_RARE },
    [BOON_BLOODTHIRST]   = { "Bloodthirst",   "Heal 6% of damage dealt",      2, TIER_RARE },
    [BOON_VIGOR]         = { "Vigor",         "+25 max health",               3, TIER_COMMON },
    [BOON_LAST_STAND]    = { "Last Stand",    "+40% damage below 30% health", 1, TIER_RARE },
    [BOON_UNBROKEN]      = { "Unbroken",      "Survive one killing blow a floor", 1, TIER_LEGENDARY },
    [BOON_CRESCENT_ECHO] = { "Moon Blade Dance+", "2 more waves", 1, TIER_RARE },
    [BOON_PHANTOM_STEP]  = { "Phantom Step",  "Easier perfect dodges, each heals 8%", 1, TIER_RARE },
    [BOON_RIPOSTE]       = { "Riposte",       "Perfect dodge: +50% damage", 1, TIER_RARE },
    [BOON_CRUSHING_BLOWS]= { "Crushing Blows","Bosses stagger 40% sooner",    1, TIER_RARE },
    [BOON_EXECUTIONER]   = { "Executioner",   "Staggered bosses take +60%",   1, TIER_LEGENDARY },
    [BOON_STORM_BLADE]   = { "Storm Blade",   "Every 4th hit calls lightning", 1, TIER_LEGENDARY },
    [BOON_BLOOD_PACT]    = { "Blood Pact",    "+35% damage, potions -25%", 1, TIER_LEGENDARY },
    [BOON_RALLY]         = { "Rally",         "Hit back to win lost health", 1, TIER_RARE },
    [BOON_LEARN_MOONBLADE] = { "Moon Blade Dance", "Learn it: D-up", 1, TIER_COMMON },
    [BOON_LEARN_HALFMOON]  = { "Halfmoon Slash",   "Learn it: D-right", 1, TIER_COMMON },
    [BOON_LEARN_CLEAVE]    = { "Crescent Cleave",  "Learn it: D-down", 1, TIER_COMMON },
    [BOON_HALFMOON_PLUS]   = { "Halfmoon Slash+",  "+50% damage", 1, TIER_RARE },
    [BOON_CLEAVE_PLUS]     = { "Crescent Cleave+", "+50% damage", 1, TIER_RARE },
};

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
    uint8_t stacks[BOON_COUNT];
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
    int choices[DRAFT_CHOICES];     // a BoonId, or PACT_CARD
    int nChoices, sel;
} s;

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
    shuffle_circle();
#ifdef HLE_TOWER_GIVE   // test aid: start the run holding this boon
    s.stacks[HLE_TOWER_GIVE] = 1;
#endif
    // The equipped relic holds for the whole climb
    int e = tower_relic_equipped();
    uint8_t fx = e >= 0 ? RELICS[e].fx : 0;
#ifdef HLE_TOWER_RELIC      // test aid: climb with these relic effects (a * 16 + b)
    fx = HLE_TOWER_RELIC;
#endif
    if (fx) {
        s.relicFx[0] = fx >> 4; s.relicFx[1] = fx & 15;
        if (has_relic(RELIC_SKILL) && character_skills_enabled()) s.skills |= 1 << (rng_next() % SKILL_COUNT);
        if (has_relic(RELIC_BLESSED)) {      // a boon that needs nothing applied on the spot
            static const BoonId GIFT[] = { BOON_EMBER_EDGE, BOON_IRON_SKIN, BOON_KINDLED_FLASK, BOON_SECOND_WIND,
                                           BOON_FLEETFOOT, BOON_BLOODTHIRST, BOON_CRUSHING_BLOWS, BOON_RIPOSTE, BOON_RALLY };
            s.stacks[GIFT[rng_next() % (sizeof GIFT / sizeof GIFT[0])]]++;
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
    if ((s.floor - 1) % FLOORS_PER_CIRCLE == 0) shuffle_circle();
}

// ---- Draft ----

static bool is_skill_card(BoonId b)
{
    return b == BOON_LEARN_MOONBLADE || b == BOON_LEARN_HALFMOON || b == BOON_LEARN_CLEAVE ||
           b == BOON_HALFMOON_PLUS || b == BOON_CLEAVE_PLUS || b == BOON_CRESCENT_ECHO || b == BOON_MOON_MASTERY;
}

static bool knows(int skill) { return (s.skills >> skill) & 1; }

// Can it be offered now? (stacks left, and an upgrade needs its skill)
static bool offerable(BoonId b)
{
    if (s.stacks[b] >= BOONS[b].maxStacks) return false;
    if (is_skill_card(b) && !character_skills_enabled()) return false;     // Skills off (the title's setting): none offered
    switch (b) {
    case BOON_MOON_MASTERY:  return s.skills != 0;
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
            int tier = (want + 3 - tries) % 3, cand[BOON_COUNT], nc = 0;
            for (int i = 0; i < n; i++) if (BOONS[pool[i]].tier == tier) cand[nc++] = i;
            if (nc) k = cand[rng_next() % nc];
        }
        s.choices[s.nChoices++] = pool[k];
        pool[k] = pool[--n];
    }
#ifdef HLE_DRAFT_GALLERY     // test aid: each floor shows set cards (every boon, the longest pacts) for a look
    {
        static const int G[][3] = {
            { PACT_CARD(PB_WOUNDED, PC_EXPOSED), PACT_CARD(PB_LAST_BREATH, PC_WEAKNESS), PACT_CARD(PB_STAGGERED, PC_IMPAIRED) },
            { PACT_CARD(PB_MIGHT, PC_THIRST), PACT_CARD(PB_BULWARK, PC_EXPOSED), PACT_CARD(PB_VITALITY, PC_FRAILTY) },
            { 0, 1, 2 }, { 3, 4, 5 }, { 6, 7, 8 }, { 9, 10, 11 }, { 12, 13, 14 }, { 15, 16, 17 }, { 18, 19, 20 }, { 21, 22, 23 },
        };
        int g = (s.floor - 1) % (int)(sizeof G / sizeof G[0]);
        for (int i = 0; i < 3; i++) s.choices[i] = G[g][i];
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
        BoonId b = (BoonId)s.choices[index];
        s.stacks[b]++;
        if (b == BOON_DEEP_FLASK) character.healthPotions = character.healthPotions + 3 > 9 ? 9 : character.healthPotions + 3;
        if (b == BOON_LEARN_MOONBLADE) s.skills |= 1 << SKILL_MOON_BLADE_DANCE;
        if (b == BOON_LEARN_HALFMOON)  s.skills |= 1 << SKILL_HALFMOON_SLASH;
        if (b == BOON_LEARN_CLEAVE)    s.skills |= 1 << SKILL_CRESCENT_CLEAVE;
        if (b == BOON_VIGOR) character.health += 25.0f;
    }
    s.draftOpen = false;
}

bool tower_draft_update(bool left, bool right, bool up, bool down, bool confirm)
{
    if (!s.draftOpen) return false;
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

static void text_style(int x, int y, int w, rdpq_align_t align, int style, const char *str)
{
    rdpq_text_printf(&(rdpq_textparms_t){ .align = align, .width = w, .wrap = WRAP_WORD, .style_id = style }, FONT_UNBALANCED, x, y, "%s", str);
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
        text_style(x + 4, y0 + 14, cw - 8, ALIGN_CENTER, NAME_STYLE[bd->tier], bd->name);
        const char *desc = bd->desc;    // a Learn card names the skill's button in the layout
        if (card == BOON_LEARN_MOONBLADE || card == BOON_LEARN_HALFMOON || card == BOON_LEARN_CLEAVE) {
            snprintf(line, sizeof line, "Learn it: %s", joypad_skill_button(card - BOON_LEARN_MOONBLADE));
            desc = line;
        }
        text(x + 6, y0 + 50, cw - 12, ALIGN_CENTER, desc);     // below a name of up to two lines
        if (s.stacks[card] > 0) {
            snprintf(line, sizeof line, "Have %d", s.stacks[card]);
            text_style(x, y0 + ch - 2, cw, ALIGN_CENTER, FONT_STYLE_GREY, line);
        }
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
    ADD("\nBoons:");
    int k = 0;
    for (int i = 0; i < BOON_COUNT; i++) if (s.stacks[i]) {
        ADD("%s %s", k++ ? "," : "", BOONS[i].name);
        if (BOONS[i].maxStacks > 1) ADD(" %d", s.stacks[i]);
    }
    if (!k) ADD(" none");
    ADD("\nBlessings:"); k = 0;
    for (int i = 0; i < PB_COUNT; i++) if (s.bless[i]) ADD("%s %s %d", k++ ? "," : "", BLESSINGS[i].name, s.bless[i]);
    if (!k) ADD(" none");
    ADD("\nCurses:"); k = 0;
    for (int i = 0; i < PC_COUNT; i++) if (s.curse[i]) ADD("%s %s %d", k++ ? "," : "", CURSES[i].name, s.curse[i]);
    if (!k) ADD(" none");
    #undef ADD
    return b;
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
    float m = (1.0f + 0.08f * shrine(UPG_HONED_BLADE)) * (1.0f + 0.15f * tower_boon_stacks(BOON_EMBER_EDGE));
    if (s.active) {
        m *= 1.0f + 0.10f * circles_cleared();
        m *= (1.0f + 0.08f * s.bless[PB_MIGHT]) * powf_int(0.95f, s.curse[PC_WEAKNESS]);
        if (character.health < character.maxHealth * 0.3f) m *= 1.0f + 0.15f * s.bless[PB_LAST_BREATH];
    }
    if (tower_boon_stacks(BOON_LAST_STAND) && character.health < character.maxHealth * 0.3f) m *= 1.4f;
    if (tower_boon_stacks(BOON_BLOOD_PACT)) m *= 1.35f;
    extern double nowS;
    if (tower_boon_stacks(BOON_RIPOSTE) && nowS < s.riposteUntil) m *= 1.5f;
    if (has_relic(RELIC_DAMAGE)) m *= 1.06f;
    if (has_relic(RELIC_FURY)) m *= 1.15f;
    return m;
}

float tower_mod_damage_vs(float bossHealthFrac, bool staggered)
{
    if (!s.active) return 1.0f;
    float m = 1.0f;
    if (bossHealthFrac < 0.5f) m *= 1.0f + 0.12f * s.bless[PB_WOUNDED];
    if (staggered) m *= 1.0f + 0.15f * s.bless[PB_STAGGERED];
    return m;
}

bool tower_hit_impaired(void)
{
    if (!s.active || !s.curse[PC_IMPAIRED]) return false;
    return (int)(rng_next() % 100) < 3 * s.curse[PC_IMPAIRED];
}

void tower_on_roll(void) { extern double nowS; s.exposedUntil = nowS + 2.0; }

float tower_mod_damage_taken(void)
{
    float m = powf_int(0.85f, tower_boon_stacks(BOON_IRON_SKIN));
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

float tower_mod_poise(void)
{
    float m = 1.0f;
    if (tower_boon_stacks(BOON_CRUSHING_BLOWS)) m *= 1.4f;
    return m;
}
float tower_mod_stagger_damage(void) { return tower_boon_stacks(BOON_EXECUTIONER) ? 1.6f : 1.0f; }
float tower_mod_perfect_window(void) { return tower_boon_stacks(BOON_PHANTOM_STEP) ? 0.35f : 0.2f; }

void tower_on_perfect_dodge(void)
{
    if (!s.active) return;
    if (tower_boon_stacks(BOON_PHANTOM_STEP)) character.health = fminf(character.maxHealth, character.health + character.maxHealth * 0.08f);
    extern double nowS;
    if (tower_boon_stacks(BOON_RIPOSTE)) s.riposteUntil = nowS + 2.5;
}

float tower_on_sword_hit(void)
{
    if (!tower_boon_stacks(BOON_STORM_BLADE)) return 0.0f;
    bool storm = ++s.hitCount % 4 == 0;
#ifdef HLE_AUTOPLAY
    if (storm) debugf("[boon] Storm Blade: lightning on hit %d\n", s.hitCount);
#endif
    return storm ? 12.0f : 0.0f;
}

int   tower_mod_max_potions(void) { return s.active ? START_POTIONS + shrine(UPG_FLASK_CAPACITY) + 3 * tower_boon_stacks(BOON_DEEP_FLASK) + 2 * has_relic(RELIC_FLASK) - has_relic(RELIC_PARCHED) : 3; }   // Story and Bonus: 3
bool  tower_mod_skill_allowed(int skill) { return character_skills_enabled() && (!s.active || knows(skill)); }
float tower_mod_skill_damage(int skill)
{
    if (skill == SKILL_HALFMOON_SLASH && tower_boon_stacks(BOON_HALFMOON_PLUS)) return 1.5f;
    if (skill == SKILL_CRESCENT_CLEAVE && tower_boon_stacks(BOON_CLEAVE_PLUS)) return 1.5f;
    return 1.0f;
}
float tower_mod_potion_heal(void)
{
    return 0.45f * (1.0f + 0.08f * shrine(UPG_FLASK_POTENCY)) * (1.0f + 0.25f * tower_boon_stacks(BOON_KINDLED_FLASK))
           * (tower_boon_stacks(BOON_BLOOD_PACT) ? 0.75f : 1.0f);
}
float tower_mod_max_health(void) { return 150.0f + 10.0f * shrine(UPG_VIGOR) + 25.0f * tower_boon_stacks(BOON_VIGOR) + 15.0f * has_relic(RELIC_HEALTH) - 20.0f * has_relic(RELIC_FRAIL) + 12.0f * (s.active ? s.bless[PB_VITALITY] : 0); }
float tower_mod_roll_cost(void) { return (tower_boon_stacks(BOON_FLEETFOOT) ? 0.6f : 1.0f) * (has_relic(RELIC_LEADEN) ? 1.25f : 1.0f); }
float tower_mod_stamina_regen(void) { return (1.0f + 0.4f * tower_boon_stacks(BOON_SECOND_WIND)) * (has_relic(RELIC_STAMINA) ? 1.2f : 1.0f); }
float tower_mod_skill_cooldown(void)
{
    return (1.0f - 0.08f * shrine(UPG_SKILL_MASTERY)) * powf_int(0.8f, tower_boon_stacks(BOON_MOON_MASTERY))
           * ((s.active && s.night == NIGHT_STARFALL) ? 0.7f : 1.0f);
}
float tower_mod_lifesteal(void) { return 0.06f * tower_boon_stacks(BOON_BLOODTHIRST) + 0.03f * has_relic(RELIC_VAMPIRIC); }

// Rally, at Bloodborne's pace in our numbers: lost health stays winnable 1 s, then drains away
// over 4 s; each hit wins back what it dealt (a sword hit 4: about four hits for a typical blow,
// one Halfmoon Slash for most of a heavy one). A new blow adds to it and holds it again
#define RALLY_HOLD  1.0f
#define RALLY_DRAIN 4.0f
#define RALLY_RATIO 1.0f
void tower_on_player_hurt(float lost)
{
    if (!tower_boon_stacks(BOON_RALLY) || lost <= 0.0f) return;
    s.rally += lost;
    s.rallyHold = RALLY_HOLD;
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
    else if (character.healthPotions < 9) character.healthPotions++;    // a potion every fallen boss, even past the run's maximum (9: the HUD's one digit)
}
