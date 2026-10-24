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

// ---- Tuning ----
#define FLOORS_PER_CIRCLE   5
// The climb starts well below the story fights and passes them by circle 3
#define BOSS_HP_START       0.70f     // circle 1, then +BOSS_HP_PER_CIRCLE a circle
#define BOSS_HP_PER_CIRCLE  0.30f
#define BOSS_DMG_START      0.60f     // circle 1, then +BOSS_DMG_PER_CIRCLE a circle
#define BOSS_DMG_PER_CIRCLE 0.20f
#define DRAFT_CHOICES       3
#define START_POTIONS       3         // as in the original game
// A floor is one fight: the Guardian's two phases (200 together in the story) are two floors of
// 100, so a Bonus boss has half its story health and the story's ratios hold (75, 100, 125)
#define BONUS_HP_IN_TOWER   0.5f
#define HP_PER_SKILL        (1.0f / 3.0f)   // all three unlocked: double, like the Skills setting

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

typedef struct { const char *name, *desc; } UpgradeDef;

static const UpgradeDef UPGRADES[UPG_COUNT] = {
    [UPG_FLASK_CAPACITY] = { "Flask Capacity", "+1 potion" },
    [UPG_FLASK_POTENCY]  = { "Flask Potency",  "Potions heal 8% more" },
    [UPG_HONED_BLADE]    = { "Honed Blade",    "+8% damage" },
    [UPG_VIGOR]          = { "Vigor",          "+10 max health" },
    [UPG_SKILL_MASTERY]  = { "Skill Mastery",  "Skill cooldowns -8%" },
};
// Everything (the five upgrades to 3, the Nightfarer) is 2400 Embers: about 6.5 hours of climbing
// (2.5 min a floor, runs reaching floor 4 at first and 16 with it all), 7.5 with the story and Bonus bosses
static const int UPGRADE_COST[UPG_MAX_LEVEL] = { 50, 120, 220 };

static struct {
    bool active;
    int floor;
    uint32_t rng;
    TowerBoss order[FLOORS_PER_CIRCLE];
    uint8_t skills;                 // learned this run (bit per CharacterSkill)
    uint8_t stacks[BOON_COUNT];
    int runEmbers, lastFloorEmbers;
    bool unbrokenUsed;
    Night night, nextNight;
    uint8_t relicFx[2];             // the equipped relic's effects, for this run
    bool relicFound;
    int hitCount;                   // sword hits landed (Storm Blade)
    float rally, rallyHold, rallyRate;  // Rally: health to win back, its hold, then its drain a second
    double riposteUntil;
    // draft
    bool draftOpen;
    BoonId choices[DRAFT_CHOICES];
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

// ---- Relics ----

static int relic_fx(uint8_t r, int k) { return k ? (r & 15) : (r >> 4); }
static bool has_relic(int fx) { return s.active && (s.relicFx[0] == fx || s.relicFx[1] == fx); }

int tower_relic_count(void)
{
    TowerSave t; save_controller_get_tower(&t);
    int n = 0;
    for (int i = 0; i < TOWER_RELIC_SLOTS; i++) n += t.relics[i] != 0;
    return n;
}

const char* tower_relic_name(int i)
{
    static char name[40];
    TowerSave t; save_controller_get_tower(&t);
    if (i < 0 || i >= TOWER_RELIC_SLOTS || !t.relics[i]) return "None";
    snprintf(name, sizeof name, "%s %s Relic", RELIC_FX[relic_fx(t.relics[i], 0)].adj, RELIC_FX[relic_fx(t.relics[i], 1)].adj);
    return name;
}

const char* tower_relic_desc(int i)
{
    static char desc[64];
    TowerSave t; save_controller_get_tower(&t);
    if (i < 0 || i >= TOWER_RELIC_SLOTS || !t.relics[i]) return "Clear a circle to find a relic";
    snprintf(desc, sizeof desc, "%s, %s", RELIC_FX[relic_fx(t.relics[i], 0)].desc, RELIC_FX[relic_fx(t.relics[i], 1)].desc);
    return desc;
}

int tower_relic_equipped(void)
{
    TowerSave t; save_controller_get_tower(&t);
    int e = ((t.flags >> 1) & 3) - 1;
    return (e >= 0 && t.relics[e]) ? e : -1;
}

void tower_relic_cycle(int dir)
{
    TowerSave t; save_controller_get_tower(&t);
    int e = tower_relic_equipped();
    for (int n = 0; n < TOWER_RELIC_SLOTS + 1; n++) {   // the next filled slot, or none
        e += dir;
        if (e >= TOWER_RELIC_SLOTS) e = -1;
        if (e < -1) e = TOWER_RELIC_SLOTS - 1;
        if (e < 0 || t.relics[e]) break;
    }
    t.flags = (uint8_t)((t.flags & ~6) | ((e + 1) << 1));
    save_controller_set_tower(&t);
}

// The relics there are: four of two blessings that work together, four pacts whose curse is
// the price of a style (old saves' other pairs keep working)
#define R(a, b) (uint8_t)(((a) << 4) | (b))
static const uint8_t RELICS[] = {
    R(RELIC_DAMAGE, RELIC_HEALTH),     // Keen Hale: an all-rounder
    R(RELIC_WARD, RELIC_FLASK),        // Warding Brimming: to last
    R(RELIC_SKILL, RELIC_STAMINA),   // Sage Swift: a skill from the start, the stamina to use it
    R(RELIC_EMBERS, RELIC_DAMAGE),     // Gilded Keen: for Embers
    R(RELIC_FURY, RELIC_FRAIL),        // Wrathful Frail: glass cannon
    R(RELIC_VAMPIRIC, RELIC_DREAD),    // Vampiric Dreaded: heal by hitting, hit harder in turn
    R(RELIC_AVARICE, RELIC_PARCHED),   // Avaricious Parched: greed, fewer potions
    R(RELIC_BLESSED, RELIC_LEADEN),    // Blessed Leaden: a boon to start, heavier rolls
};
#undef R

// A circle cleared: one of the relics, preferring one not held; with all slots full the oldest goes
static void relic_award(void)
{
    TowerSave t; save_controller_get_tower(&t);
    enum { N = sizeof RELICS / sizeof RELICS[0] };
    uint8_t cand[N]; int nc = 0;
    for (int i = 0; i < N; i++) {
        bool held = false;
        for (int k = 0; k < TOWER_RELIC_SLOTS; k++) held |= t.relics[k] == RELICS[i];
        if (!held) cand[nc++] = RELICS[i];
    }
    uint8_t r = nc ? cand[rng_next() % nc] : RELICS[rng_next() % N];
    int slot = -1;
    for (int i = 0; i < TOWER_RELIC_SLOTS; i++) if (!t.relics[i]) { slot = i; break; }
    if (slot < 0) {
        int e = ((t.flags >> 1) & 3) - 1;
        for (int i = 0; i < TOWER_RELIC_SLOTS - 1; i++) t.relics[i] = t.relics[i + 1];
        slot = TOWER_RELIC_SLOTS - 1;
        e = e == 0 ? -1 : e - 1;            // the equipped one moved up a slot, or was the oldest
        t.flags = (uint8_t)((t.flags & ~6) | ((e + 1) << 1));
    }
    t.relics[slot] = r;
    save_controller_set_tower(&t);
    s.relicFound = true;
#ifdef HLE_AUTOPLAY
    debugf("[relic] %s: %s (slot %d)\n", tower_relic_name(slot), tower_relic_desc(slot), slot);
#endif
}

// ---- Nightfarer ----

bool tower_nightfarer_owned(void) { TowerSave t; save_controller_get_tower(&t); return t.flags & TOWER_FLAG_NIGHTFARER; }
int  tower_nightfarer_cost(void) { return tower_nightfarer_owned() ? -1 : NIGHTFARER_COST; }
bool tower_nightfarer_buy(void)
{
    TowerSave t; save_controller_get_tower(&t);
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
#ifdef HLE_TOWER_RELIC      // test aid: climb with this relic (two effect indices, a * 16 + b)
    e = 0;
#endif
    if (e >= 0) {
        TowerSave t; save_controller_get_tower(&t);
        s.relicFx[0] = relic_fx(t.relics[e], 0); s.relicFx[1] = relic_fx(t.relics[e], 1);
#ifdef HLE_TOWER_RELIC
        s.relicFx[0] = relic_fx(HLE_TOWER_RELIC, 0); s.relicFx[1] = relic_fx(HLE_TOWER_RELIC, 1);
#endif
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
    save_controller_get_tower(&t);
    s.lastFloorEmbers = floor_embers();
    s.runEmbers += s.lastFloorEmbers;
    int e = t.embers + s.lastFloorEmbers;
    t.embers = e > 65535 ? 65535 : e;
    if (s.floor > t.bestFloor) t.bestFloor = s.floor > 255 ? 255 : s.floor;
    save_controller_set_tower(&t);
    s.relicFound = false;
    if (s.floor % FLOORS_PER_CIRCLE == 0) {     // a circle cleared: a relic, and the next circle's night
        relic_award();
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
    // The rest by rarity: legendary 6% on the first circle, +4% a circle (up to 25%); rare 30%
    while (s.nChoices < DRAFT_CHOICES && n > 0) {
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
    s.sel = 0;
    s.draftOpen = true;
#ifdef HLE_AUTOPLAY
    debugf("[draft] floor %d:", s.floor);
    for (int i = 0; i < s.nChoices; i++) debugf(" %s (%d)", BOONS[s.choices[i]].name, BOONS[s.choices[i]].tier);
    debugf("%s\n", s.relicFound ? " | relic found" : "");
#endif
}

bool tower_draft_is_open(void) { return s.draftOpen; }

void tower_draft_pick(int index)
{
    if (index >= 0 && index < s.nChoices) {
        BoonId b = s.choices[index];
        s.stacks[b]++;
        if (b == BOON_DEEP_FLASK) character.healthPotions = character.healthPotions + 3 > 9 ? 9 : character.healthPotions + 3;
        if (b == BOON_LEARN_MOONBLADE) s.skills |= 1 << SKILL_MOON_BLADE_DANCE;
        if (b == BOON_LEARN_HALFMOON)  s.skills |= 1 << SKILL_HALFMOON_SLASH;
        if (b == BOON_LEARN_CLEAVE)    s.skills |= 1 << SKILL_CRESCENT_CLEAVE;
        if (b == BOON_VIGOR) character.health += 25.0f;
    }
    s.draftOpen = false;
}

bool tower_draft_update(bool left, bool right, bool confirm)
{
    if (!s.draftOpen) return false;
    if (s.nChoices == 0) { tower_draft_pick(-1); return true; }   // everything maxed
    if (left)  s.sel = (s.sel + s.nChoices - 1) % s.nChoices;
    if (right) s.sel = (s.sel + 1) % s.nChoices;
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
    text_style(0, 30, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_GOLD, line);
    if (s.relicFound) {
        TowerSave t; save_controller_get_tower(&t);
        int last = 0;
        for (int i = 0; i < TOWER_RELIC_SLOTS; i++) if (t.relics[i]) last = i;
        snprintf(line, sizeof line, "Relic found: %s", tower_relic_name(last));
        text_style(0, 44, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_BLUE, line);
    }
    text(0, 60, SCREEN_WIDTH, ALIGN_CENTER, "Choose a Boon");

    const int cw = 96, ch = 112, gap = 6, y0 = 76;
    int x0 = (SCREEN_WIDTH - (cw * s.nChoices + gap * (s.nChoices - 1))) / 2;
    for (int i = 0; i < s.nChoices; i++) {
        int x = x0 + i * (cw + gap);
        const BoonDef *b = &BOONS[s.choices[i]];
        flat_mode();
        bool on = i == s.sel;
        // The frame tells the rarity: bronze, blue, gold (brighter when selected)
        static const color_t FRAME[3][2] = {
            { RGBA32(90, 70, 50, 255),  RGBA32(230, 150, 50, 255) },
            { RGBA32(50, 80, 150, 255), RGBA32(110, 170, 255, 255) },
            { RGBA32(150, 115, 30, 255), RGBA32(255, 215, 90, 255) },
        };
        rdpq_set_prim_color(FRAME[b->tier][on]);
        rdpq_fill_rectangle(x - 2, y0 - 2, x + cw + 2, y0 + ch + 2);
        rdpq_set_prim_color(on ? RGBA32(60, 30, 20, 240) : RGBA32(30, 22, 18, 230));
        rdpq_fill_rectangle(x, y0, x + cw, y0 + ch);
        static const int NAME_STYLE[3] = { FONT_STYLE_WHITE, FONT_STYLE_BLUE, FONT_STYLE_GOLD };
        text_style(x + 4, y0 + 18, cw - 8, ALIGN_CENTER, NAME_STYLE[b->tier], b->name);
        text(x + 6, y0 + 52, cw - 12, ALIGN_CENTER, b->desc);     // below a name of up to two lines
        if (s.stacks[s.choices[i]] > 0) {
            snprintf(line, sizeof line, "Have %d", s.stacks[s.choices[i]]);
            text_style(x, y0 + ch - 8, cw, ALIGN_CENTER, FONT_STYLE_GREY, line);
        }
    }
    if (s.floor % FLOORS_PER_CIRCLE == 0) {     // a new circle: stronger, and its night
        snprintf(line, sizeof line, "Next: floor %d. The bosses grow stronger", s.floor + 1);
        text_style(0, y0 + ch + 18, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_GREY, line);
        snprintf(line, sizeof line, "%s falls: %s", NIGHTS[s.nextNight].name, NIGHTS[s.nextNight].desc);
        text_style(16, y0 + ch + 32, SCREEN_WIDTH - 32, ALIGN_CENTER, FONT_STYLE_RED, line);
    } else {
        snprintf(line, sizeof line, "Next: floor %d", s.floor + 1);
        text_style(0, y0 + ch + 22, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_GREY, line);
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

const char* tower_death_summary(void)
{
    static char line[96];
    int boons = 0;
    for (int b = 0; b < BOON_COUNT; b++) boons += s.stacks[b];
    snprintf(line, sizeof line, "Floor %d   +%d Embers (%d)\n%d boons taken", s.floor, s.runEmbers, tower_embers(), boons);
    return line;
}

// ---- Shrine ----

int tower_embers(void) { TowerSave t; save_controller_get_tower(&t); return t.embers; }
int tower_best_floor(void) { TowerSave t; save_controller_get_tower(&t); return t.bestFloor; }

int tower_upgrade_level(UpgradeId u)
{
    TowerSave t; save_controller_get_tower(&t);
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
    TowerSave t; save_controller_get_tower(&t);
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

float tower_mod_damage_dealt(void)
{
    float m = (1.0f + 0.08f * shrine(UPG_HONED_BLADE)) * (1.0f + 0.15f * tower_boon_stacks(BOON_EMBER_EDGE));
    if (tower_boon_stacks(BOON_LAST_STAND) && character.health < character.maxHealth * 0.3f) m *= 1.4f;
    if (tower_boon_stacks(BOON_BLOOD_PACT)) m *= 1.35f;
    extern double nowS;
    if (tower_boon_stacks(BOON_RIPOSTE) && nowS < s.riposteUntil) m *= 1.5f;
    if (has_relic(RELIC_DAMAGE)) m *= 1.06f;
    if (has_relic(RELIC_FURY)) m *= 1.15f;
    return m;
}

float tower_mod_damage_taken(void)
{
    float m = powf_int(0.85f, tower_boon_stacks(BOON_IRON_SKIN));
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
float tower_mod_stagger_damage(void) { return tower_boon_stacks(BOON_EXECUTIONER) ? 1.6f / 1.3f : 1.0f; }
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
float tower_mod_max_health(void) { return 150.0f + 10.0f * shrine(UPG_VIGOR) + 25.0f * tower_boon_stacks(BOON_VIGOR) + 15.0f * has_relic(RELIC_HEALTH) - 20.0f * has_relic(RELIC_FRAIL); }
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
    for (int k = 0; k < SKILL_COUNT; k++) n += character_skills_enabled() && ((s.skills >> k) & 1);
    float k = (BOSS_HP_START + BOSS_HP_PER_CIRCLE * (tower_circle() - 1)) * (1.0f + HP_PER_SKILL * n);
    if (s.night == NIGHT_IRON) k *= 1.25f;
    return phase >= 3 ? k * BONUS_HP_IN_TOWER : k;     // phases 3+ are the Bonus bosses
}

void tower_floor_restore(void)
{
    // One potion a fallen boss, no health: the potions are the run's healing
    if (character.healthPotions < 9) character.healthPotions++;    // a potion every fallen boss, even past the run's maximum (9: the HUD's one digit)
}
