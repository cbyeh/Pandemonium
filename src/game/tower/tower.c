#include <libdragon.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "tower.h"
#include "character.h"
#include "character_skin.h"
#include "globals.h"
#include "save_controller.h"
#include "video_controller.h"
#include "video_layout.h"
#include "joypad_utility.h"
#include "ui_pages.h"
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
    [BOON_IRON_SKIN]     = { "Iron Skin",     "-15% taken (Lv 3: -10%)", 3, TIER_COMMON, FAM_IRON },
    [BOON_DEEP_FLASK]    = { "Deep Flask",    "+3 potions",                   2, TIER_COMMON, FAM_HEARTH },
    [BOON_KINDLED_FLASK] = { "Kindled Flask", "Potions heal 25% more",        2, TIER_COMMON, FAM_HEARTH },
    [BOON_FLEETFOOT]     = { "Fleetfoot",     "Rolls cost 40% less stamina",  1, TIER_RARE, FAM_SHADOW },
    [BOON_SECOND_WIND]   = { "Second Wind",   "+40% stamina regen",     3, TIER_COMMON, FAM_SHADOW },
    [BOON_MOON_MASTERY]  = { "Moon Mastery",  "Skill cooldowns -20%",         3, TIER_RARE, FAM_MOON },
    [BOON_BLOODTHIRST]   = { "Bloodthirst",   "Sword hits heal 12% of base damage", 2, TIER_RARE, FAM_BLOOD },
    [BOON_VIGOR]         = { "Vigor",         "+25 max health",               5, TIER_COMMON, FAM_IRON },
    [BOON_LAST_STAND]    = { "Last Stand",    "+40% damage below 30% health", 2, TIER_RARE, FAM_BLOOD },
    [BOON_UNBROKEN]      = { "Unbroken",      "Survive one killing blow a floor", 1, TIER_LEGENDARY, FAM_IRON },
    [BOON_PHANTOM_STEP]  = { "Phantom Step",  "Easier dodges, each heals 2%", 1, TIER_RARE, FAM_SHADOW },
    [BOON_RIPOSTE]       = { "Riposte",       "Perfect dodge: +50% damage", 1, TIER_RARE, FAM_SHADOW },
    [BOON_CRUSHING_BLOWS]= { "Crushing Blows","Staggers last 50% longer",     1, TIER_RARE, FAM_IRON },
    [BOON_EXECUTIONER]   = { "Executioner",   "Staggered bosses take +60%",   1, TIER_LEGENDARY, FAM_IRON },
    [BOON_STORM_BLADE]   = { "Storm Blade",   "Every 4th hit calls lightning", 1, TIER_LEGENDARY, FAM_STORM },
    [BOON_BLOOD_PACT]    = { "Blood Pact",    "+35% damage, potions -25%", 1, TIER_LEGENDARY, FAM_BLOOD },
    [BOON_RALLY]         = { "Rally",         "Hit back to win lost health", 2, TIER_RARE, FAM_BLOOD },
    [BOON_HEMORRHAGE]    = { "Hemorrhage",    "Finishers bleed 4 over 3 s", 3, TIER_COMMON, FAM_BLOOD, 1 },
    [BOON_SANGUINE_FLASK]= { "Sanguine Flask","Potion: +25% damage 6 s", 1, TIER_RARE, FAM_BLOOD, 1 },
    [BOON_RED_THIRST]    = { "Red Thirst",    "+10% damage per 25% health lost", 3, TIER_COMMON, FAM_BLOOD, 1 },
    [BOON_STATIC]        = { "Static",        "Storm Blade +1.5, or a bolt each 8th hit", 4, TIER_COMMON, FAM_STORM, 1 },
    [BOON_THUNDERCLAP]   = { "Thunderclap",   "Combo finishers call lightning", 1, TIER_RARE, FAM_STORM, 1 },
    [BOON_CONDUCTOR]     = { "Conductor",     "Lightning fills the stagger meter", 1, TIER_RARE, FAM_STORM, 1 },
    [BOON_OVERCHARGE]    = { "Overcharge",    "A stagger: 5 hits call lightning", 1, TIER_LEGENDARY, FAM_STORM, 1 },
    [BOON_LUNAR_TIDE]    = { "Lunar Tide",    "Sword hits: cooldowns -0.3 s", 3, TIER_COMMON, FAM_MOON, 1 },
    [BOON_ECLIPSE]       = { "Eclipse",       "Skills +50% on staggers", 1, TIER_RARE, FAM_MOON, 1 },
    [BOON_WAXING_MOON]   = { "Waxing Moon",   "A cast: sword +15% for 4 s", 1, TIER_RARE, FAM_MOON, 1 },
    [BOON_HEAVY_HAND]    = { "Heavy Hand",    "Finishers: +20% stagger", 3, TIER_COMMON, FAM_IRON, 1 },
    [BOON_STALWART]      = { "Stalwart",      "Blows over 20% of max health -25%", 1, TIER_RARE, FAM_IRON, 1 },
    [BOON_SHADOW_EDGE]   = { "Shadow Edge",   "+8% damage for 2 s after a roll", 4, TIER_COMMON, FAM_SHADOW, 1 },
    [BOON_EVASION]       = { "Evasion",       "A perfect dodge's roll is free", 1, TIER_RARE, FAM_SHADOW, 1 },
    [BOON_VANISH]        = { "Vanish",        "3 perfect dodges: a blow deals 0", 1, TIER_LEGENDARY, FAM_SHADOW, 1 },
    [BOON_PROSPECTOR]    = { "Prospector",    "+20% gold",                    3, TIER_COMMON, FAM_HEARTH, 1 },   // (Quick Draught is still held back)
    [BOON_BERSERKER]     = { "Berserker",     "Below half: +8% a hit, no potions", 1, TIER_LEGENDARY, FAM_BLOOD, 1 },
    // a skill's card: Learn it (Common), then its levels 2-4 (Rare: card_tier), each level's text in SKILL_LV_DESC
    [BOON_LEARN_MOONBLADE] = { "Moon Blade Dance", "Learn it: D-up", 4, TIER_COMMON, FAM_SKILL },
    [BOON_LEARN_HALFMOON]  = { "Halfmoon Slash",   "Learn it: D-right", 4, TIER_COMMON, FAM_SKILL },
    [BOON_LEARN_CLEAVE]    = { "Crescent Cleave",  "Learn it: D-down", 4, TIER_COMMON, FAM_SKILL },
    // the Lv 5 forms (section 6), offered at Lv 4; the B forms once bought at the Shrine
    [BOON_FULL_MOON_DANCE] = { "Full Moon Dance",  "10 crosses, 2 each", 1, TIER_LEGENDARY, FAM_SKILL },
    [BOON_SHATTERING_DANCE]= { "Shattering Dance", "Crosses that stagger", 1, TIER_LEGENDARY, FAM_SKILL },
    [BOON_TWIN_MOON]       = { "Twin Moon",        "A second slash, 40% of the first", 1, TIER_LEGENDARY, FAM_SKILL },
    [BOON_FULL_MOON]       = { "Full Moon",        "360 degrees, unbreakable, 2x stagger", 1, TIER_LEGENDARY, FAM_SKILL },
    [BOON_CRESCENT_STORM]  = { "Crescent Storm",   "3 trailing blades", 1, TIER_LEGENDARY, FAM_SKILL },
    [BOON_LONG_CLEAVE]     = { "Long Cleave",      "1.6x the range, 6 hits", 1, TIER_LEGENDARY, FAM_SKILL },
    // "+" (docs/design/TOWER_ROGUELIKE.md section 10; Second Wind+ is held back: a potion is drunk at once)
    [BOON_EMBER_EDGE_PLUS]    = { "Ember Edge+",    "Finishers +25%",              1, TIER_RARE, FAM_HEARTH, PLUS(BOON_EMBER_EDGE) },
    [BOON_IRON_SKIN_PLUS]     = { "Iron Skin+",     "The first blow a floor deals half", 1, TIER_RARE, FAM_IRON, PLUS(BOON_IRON_SKIN) },
    [BOON_STORM_BLADE_PLUS]   = { "Storm Blade+",   "Lightning every 3rd hit",     1, TIER_RARE, FAM_STORM, PLUS(BOON_STORM_BLADE) },
    [BOON_EXECUTIONER_PLUS]   = { "Executioner+",   "Staggered bosses take +90%",  1, TIER_RARE, FAM_IRON, PLUS(BOON_EXECUTIONER) },
    [BOON_CRUSHING_BLOWS_PLUS]= { "Crushing Blows+","No stagger drain 3 s after it rises", 1, TIER_RARE, FAM_IRON, PLUS(BOON_CRUSHING_BLOWS) },
    [BOON_RIPOSTE_PLUS]       = { "Riposte+",       "Riposte lasts 4 s",           1, TIER_RARE, FAM_SHADOW, PLUS(BOON_RIPOSTE) },
    [BOON_UNBROKEN_PLUS]      = { "Unbroken+",      "When it saves you, heal 20%", 1, TIER_RARE, FAM_IRON, PLUS(BOON_UNBROKEN) },
    [BOON_BLOODTHIRST_PLUS]   = { "Bloodthirst+",   "Heals 16% a level",           1, TIER_RARE, FAM_BLOOD, PLUS(BOON_BLOODTHIRST) },
    [BOON_RALLY_PLUS]         = { "Rally+",         "Each hit wins back 3",        1, TIER_RARE, FAM_BLOOD, PLUS(BOON_RALLY) },
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
    { BOON_BERSERKER, BOON_IRON_SKIN },     // (Iron Skin's second: it locks out both)
    // a skill's two forms: one locks the other out (shown and greyed as rivals are)
    { BOON_FULL_MOON_DANCE, BOON_SHATTERING_DANCE }, { BOON_TWIN_MOON, BOON_FULL_MOON }, { BOON_CRESCENT_STORM, BOON_LONG_CLEAVE },
};
// A skill card's next level (Lv 2-4), what it adds
static const char *const SKILL_LV_DESC[SKILL_COUNT][3] = {
    { "Crosses deal 1.5", "+2 crosses", "12 s cooldown" },
    { "Charges in 1.6 s", "+20% damage", "Reach +25%" },
    { "+20% damage", "Blades hit 5 times", "Wind-up 0.6 s" },
};
// The Codex's pages (the Tower menu and the pause menu): each its text in tower_text.bin, and some its tables (codex_gen)
enum { CX_CLIMB, CX_MAP, CX_FIGHTS, CX_STOPS, CX_SLOTS, CX_FAMILIES, CX_RIVALS, CX_PLUS, CX_SKILLS, CX_MBD, CX_HALFMOON, CX_CLEAVE,
       CX_AFFIXES, CX_GOLD, CX_EMBERS, CX_OFFERINGS, CX_MILESTONES, CX_PACTS, CX_NIGHTS, CX_HEART, CX_COUNT };
#define OFFER_TXT 7         // (OFF_COUNT, below)
// What each card does in full, for its Details panel (R), the affixes', the Offerings' and the Codex's pages (the numbers
// are the code's): in ROM, rom:/ui/tower_text.bin, a text a line after its header line (every BoonId in order, then the
// six affixes, the Offerings, the Codex pages), read when a page is built and freed after it (the page keeps its own copy)
enum { TXT_BOON = 0, TXT_AFFIX = BOON_COUNT, TXT_OFFER = TXT_AFFIX + AFX_COUNT, TXT_CODEX = TXT_OFFER + OFFER_TXT, TXT_COUNT = TXT_CODEX + CX_COUNT };
static char *s_text;
static const char *s_line[TXT_COUNT];
static void text_load(void)
{
    if (s_text) return;
    int n = 0;
    s_text = asset_load("rom:/ui/tower_text.bin", &n);     // (libdragon's loader: stdio's would add 4 KB of code)
    if (!s_text) return;
    char *c = s_text, *end = s_text + n;
    for (int k = -1; k < TXT_COUNT && c < end; k++) {       // (line -1: the header)
        char *nl = memchr(c, '\n', end - c);
        if (!nl) break;     // (the file ends with a newline: a text without one is left out)
        *nl = 0;
        if (k >= 0) s_line[k] = c;
        c = nl + 1;
    }
}
static void text_free(void) { free(s_text); s_text = NULL; memset(s_line, 0, sizeof s_line); }
static const char *full_text(int k) { return k >= 0 && k < TXT_COUNT && s_line[k] ? s_line[k] : ""; }
#define SKILL_LV_MAX 4      // a skill card's levels; Lv 5 is a form
#define FORM_COST    200    // a B form at the Shrine (section 14b)
#define SKILL_CARD_CHANCE 33    // % of drafts with a skill card, once a skill is known
static bool is_learn(int b) { return b >= BOON_LEARN_MOONBLADE && b <= BOON_LEARN_CLEAVE; }
static bool knows(int skill);
static bool is_form(int b) { return b >= BOON_FULL_MOON_DANCE && b <= BOON_LONG_CLEAVE; }
void tower_test_skill_lv(int skill, int lv, int form);
static int card_tier(int b);
static int form_skill(int b) { return (b - BOON_FULL_MOON_DANCE) / 2; }
static BoonId skill_form_card(int skill, int form) { return (BoonId)(BOON_FULL_MOON_DANCE + 2 * skill + (form == SKILL_FORM_B)); }
// Its k-th rival (Iron Skin has two), -1 past the last
static int rival_k(int b, int k)
{
    for (unsigned i = 0; i < sizeof RIVALS / sizeof RIVALS[0]; i++) {
        if (RIVALS[i][0] == b && !k--) return RIVALS[i][1];
        if (RIVALS[i][1] == b && !k--) return RIVALS[i][0];
    }
    return -1;
}
static int rival_of(int b) { return rival_k(b, 0); }
// Its rivals' names ("Riposte, Berserker")
static void rival_names(int b, char *out, int n)
{
    int m = 0; out[0] = 0;
    for (int k = 0, r; (r = rival_k(b, k)) >= 0 && m < n; k++) m += snprintf(out + m, n - m, "%s%s", k ? ", " : "", BOONS[r].name);
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
#ifndef STATIC_BOLT
#define STATIC_BOLT 1.5f        // Static: a level (the Storm build against the builds' mean: +3 +21%, +2 +13%, +1.5 the mean)
#endif
#ifndef THUNDERCLAP_BOLT
#define THUNDERCLAP_BOLT 6.0f   // (8: with Static's +3, the same)
#endif
#define PACT_CARD(b, c) (BOON_COUNT + (b) * PC_COUNT + (c))
#define GOLD_CARD   PACT_CARD(PB_COUNT, 0)  // a complete build's draft: +40 gold (section 6)
#define SKIP_EMBERS 5

// ---- Gold (section 3): this run only, lost when it ends ----
#define GOLD_FIGHT      25      // a fight (Guardian, Unbound, a Bonus boss): 25 + 5 x circle
#define GOLD_PER_CIRCLE 5
#define ELITE_GOLD      1.5f
#define SKIP_GOLD       15
#define GOLD_CARD_GOLD  40
#define PROSPECTOR_GOLD 0.20f   // a level
// The Shop's prices: the same every circle (gold grows with the circle)
static const int TIER_PRICE[3] = { 60, 110, 180 };
#define PRICE_PLUS    150
#define PRICE_LEVEL   70
#define PRICE_RELEASE 20
#define PRICE_POTION  45
#define PRICE_MEND    50
#define MEND_HEAL     0.35f

// ---- Nights (from the second circle) ----
typedef enum { NIGHT_NONE, NIGHT_BLOOD_MOON, NIGHT_IRON, NIGHT_STARFALL, NIGHT_COUNT } Night;
static const struct { const char *name, *desc; float embers; const char *brief; } NIGHTS[NIGHT_COUNT] = {     // (brief: one line on the map)
    [NIGHT_NONE]       = { "", "", 1.0f, "" },
    [NIGHT_BLOOD_MOON] = { "Blood Moon", "Bosses hit 20% harder", 1.5f, "blows +20%" },
    [NIGHT_IRON]       = { "Iron Night", "Bosses +25% health", 1.3f, "bosses +25% health" },
    [NIGHT_STARFALL]   = { "Starfall",   "Skills recharge 30% faster; bosses hit 10% harder", 1.2f, "skills 30% faster, blows +10%" },
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
    { R(RELIC_WARD, RELIC_HEALTH),    "Climber's Sigil" },   // not sold: best floor 50's milestone (keep it last)
};
#undef R
#define RELIC_COUNT ((int)(sizeof RELICS / sizeof RELICS[0]))
#define RELIC_COST  200
#define RELIC_SIGIL (RELIC_COUNT - 1)   // the Climber's Sigil: not sold, owned from best floor 50 (a milestone)

// ---- Offerings (the Tower menu): gifts for the next climb only, bought before it, two at most; the second costs
// OFFER_SECOND more (prices never climb across climbs). Set aside in the save (_pad[26]), used when the climb starts;
// a climb quit (or restarted) before its first Guardian falls sets them aside again. Their full texts: tower_text.bin
enum { OFF_FLASK, OFF_OATH, OFF_PURSE, OFF_WHETSTONE, OFF_CANDLE, OFF_HEARTH, OFF_KINDLING, OFF_COUNT };
static const struct { const char *name, *brief; uint8_t price, floor; } OFFERINGS[OFF_COUNT] = {
    [OFF_FLASK]     = { "Spare Flask",      "+1 potion, and one more at most", 70, 0 },
    [OFF_OATH]      = { "Hale Oath",        "+10 max health for the climb", 70, 0 },
    [OFF_PURSE]     = { "Pilgrim's Purse",  "Start with 60 gold", 55, 0 },
    [OFF_WHETSTONE] = { "Whetstone",        "+25% damage in the first circle", 40, 0 },
    [OFF_CANDLE]    = { "Scholar's Candle", "The first draft: two skill cards", 30, 0 },
    [OFF_HEARTH]    = { "Hearth Ember",     "The first Rest also heals 40%", 55, 0 },
    [OFF_KINDLING]  = { "Kindling",         "Start with a random Common boon", 55, 20 },
};
_Static_assert(OFF_COUNT == OFFER_TXT, "tower_text.bin: one text an Offering");
#define OFFER_MAX     2
#define OFFER_SECOND  50        // the second Offering of a climb costs this much more
#define OFFER_GOLD    60        // Pilgrim's Purse
#define OFFER_HP      10.0f     // Hale Oath
#define OFFER_DAMAGE  1.25f     // Whetstone, circle 1
#define OFFER_HEAL    0.40f     // Hearth Ember, the first Rest

// ---- Floor milestones: lasting rewards from the best floor (no save bits). Floor 10 is the Teal Cloak, as ever
enum { MS_TEAL, MS_KINDLING, MS_POTION, MS_SIGIL, MS_HEART, MS_TITLE, MS_COUNT };
static const struct { uint8_t floor; const char *name, *reward; } MILESTONES[MS_COUNT] = {
    [MS_TEAL]     = { 10, "The Teal Cloak", "A skin, worn from Settings > Skin" },
    [MS_KINDLING] = { 20, "Kindling", "A new Offering: a Common boon to start" },
    [MS_POTION]   = { 30, "A deeper belt", "Every climb starts with one more potion" },
    [MS_SIGIL]    = { 50, "Climber's Sigil", "A relic: -6% damage taken, +15 max health" },
    [MS_HEART]    = { 75, "The Tower's Heart", "A Codex page" },
    [MS_TITLE]    = { 100, "Tower-Breaker", "A title, on the Tower's menu" },
};
#define CIRCLE_EMBERS 25        // a circle cleared

// ---- The map (docs/design/TOWER_ROGUELIKE.md section 2): 7 rows of 3 lanes a circle, climbed bottom to
// top, each node linked to the one above it and its diagonal neighbours. Row 1 is the Guardian and row 7
// Guardian Unbound (one node each), row 6 a Rest on every lane; rows 2-5 are rolled (map_generate)
#define MAP_ROWS  7
#define MAP_LANES 3
enum { NODE_NONE, NODE_FIGHT, NODE_ELITE, NODE_EVENT, NODE_SHOP, NODE_REST, NODE_BOSS, NODE_TYPES };
// Rows 2-5's odds (%)
static const uint8_t NODE_ODDS[NODE_TYPES] = { [NODE_FIGHT] = 45, [NODE_ELITE] = 15, [NODE_EVENT] = 20, [NODE_SHOP] = 12, [NODE_REST] = 8 };
static const char *const NODE_NAMES[NODE_TYPES] = { "", "Fight", "Elite", "Event", "Shop", "Rest", "Boss" };
static const char *const BOSS_NAMES[] = { "Guardian", "Guardian Unbound", "Fallen Warrior", "Shackled Sun", "Shadowborne" };
#define ELITE_HP      1.25f     // an Elite: the boss at x1.25 health, and its affixes (section 11)
// The affixes: one an Elite in circles 1-3, two from circle 4; the last three unlocked by the best floor (section 14b)
static const struct { const char *name; uint8_t floor; } AFFIXES[AFX_COUNT] = {     // (their Details: full_text(TXT_AFFIX + a))
    [AFX_SWIFT]       = { "Swift", 0 },
    [AFX_IRONCLAD]    = { "Ironclad", 0 },
    [AFX_ENRAGED]     = { "Enraged", 0 },
    [AFX_RELENTLESS]  = { "Relentless", 15 },
    [AFX_VAMPIRIC]    = { "Vampiric", 20 },
    [AFX_SMOULDERING] = { "Smouldering", 25 },
};
#define AFX_TWO_CIRCLE   4      // two affixes from this circle
#define SWIFT_PACE       1.20f  // footwork, breathers and its moves' cooldowns (strike timing unchanged; 1.12 cost x1.42, under the 1.5-1.7 band)
#define SWIFT_PITCH      1.12f  // its own sounds a little higher (the tell)
#define ENRAGED_BLOW     1.25f  // below half health
#define ENRAGED_BREATHER 0.85f  // (0.7 cost x1.77 / 2.25 / 1.88 on the three, over the band)
#define IRONCLAD_K       1.3f   // health (on the Elite's x1.25) and the stagger bar
#define RELENTLESS_DRAIN 2.0f
#define RELENTLESS_DOWN  (2.5f / 3.5f)  // down 2.5 s, not 3.5
#define RELENTLESS_BREATHER 0.85f  // and it presses on: breathers 15% shorter (the stagger alone cost x1.25-1.4, under the band)
#define VAMPIRIC_HEAL    0.25f  // of the health its blows take (8% cost x1.33 on the metric, out of the 1.5-1.7 band)
// Berserker (section 7): below half health; +8% a sword hit in a row up to +40%, a streak ends 2 s after its last hit
#define BERSERK_HP     0.5f
#ifndef BERSERK_STEP
#define BERSERK_STEP   0.08f    // (5% a hit to +30%: +14% on the sword a minute, under its rival Iron Skin's -15% everywhere: dominated)
#endif
#ifndef BERSERK_HITS
#define BERSERK_HITS   5        // to +40%
#endif
#define BERSERK_GAP    2.0
#define ELITE_EMBERS  1.5f
#define REST_HEAL     0.40f     // a Rest: heal 40%, Temper +1 level, or +2 potions (and Release a boon, free)
#define REST_POTIONS  2         // up to POTION_CAP: fights already bank potions past the run's max (section 12)
enum { SCR_NONE, SCR_MAP, SCR_REST, SCR_TEMPER, SCR_RELEASE, SCR_SHOP, SCR_SHOP_LEVEL, SCR_SHOP_PLUS, SCR_SHOP_RELEASE, SCR_EVENT, SCR_CONFIRM };   // (the confirm: the map's node page)
// The Shop's shelf: three rows of three, then Leave (Left/Right along it, Up/Down a row)
enum { SHOP_CARD0, SHOP_CARD1, SHOP_CARD2, SHOP_LEVEL, SHOP_PLUS, SHOP_PACT, SHOP_POTION, SHOP_MEND, SHOP_RELEASE, SHOP_LEAVE, SHOP_ITEMS };
// The Events (section 4): 1-4 from the start, 5-8 by best floor 10, 15, 20, 25 (section 14b)
enum { EV_ALTAR, EV_GRAVE, EV_MIRROR, EV_MERCHANT, EV_POOL, EV_PRISONER, EV_BONES, EV_BRAZIER, EV_COUNT };
static const uint8_t EVENT_FLOOR[EV_COUNT] = { 0, 0, 0, 0, 10, 15, 20, 25 };
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
        uint8_t afx[MAP_ROWS][MAP_LANES];   // its affixes as an Elite (bit per AFX_), rolled with its boss
        int8_t path[MAP_ROWS];              // the lane taken in each row (-1: not yet)
        int8_t row, sel;                    // the row of the node last entered (-1: none yet), the cursor's lane in the next
        uint8_t met;                        // the bosses met this circle (bit per TowerBoss)
        uint8_t fights;                     // fights entered this circle
    } map;
    TowerBoss boss;                 // the fight under way (or the next: the Guardian at a circle's start)
    bool elite;                     // it's an Elite (x1.25 health, a Rare-or-better draft, x1.5 gold and Embers)
    uint8_t affixes;                // an Elite's affixes (bit per AFX_; section 11)
    bool legendDraft;               // its draft is all Legendary (the Chained Prisoner's Elite)
    bool prisoner;                  // the Chained Prisoner freed: the next Fight is such an Elite
    int gold, lastFloorGold;
    float altarHp;                  // max health given to the Bleeding Altar
    uint8_t halfBless[PB_COUNT];    // the Ember Brazier's blessings, half a pact's stack each
    uint8_t eventsSeen;             // bit per event (no repeat in a run while any are left)
    int8_t event, eventPick;        // the Event on screen, its rolled blessing (the Brazier)
    bool eventDone;                 // its choice taken: the result, then Continue
    char eventResult[96];
    struct { int16_t card[3]; int pact; uint16_t sold; int8_t sel; } shop;  // a visit: the cards (-1 none), its pact, bought items (bit per SHOP_)
    uint8_t scr;                    // the screen after the draft: the map, a Rest's (SCR_)
    uint32_t scrOpenMs;
    int8_t list[LIST_MAX], listN, listSel;  // a Rest screen's rows: a choice, a BoonId to Temper (-1 Back), a slot to Release (-1 keep)
    uint8_t skills;                 // learned this run (bit per CharacterSkill); its level is its Learn card's stacks
    uint8_t forms;                  // the B forms bought at the Shrine (bit per skill), for this run
    uint8_t formsDue;               // a skill just reached Lv 4: the next draft leads with its forms (bit per skill)
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
    int berserkHits; double berserkAt;  // Berserker: sword hits in a row, the last one's time
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
    uint32_t seed;                  // the run's seed (shown at its end and in Stats)
    uint8_t offers;                 // the Offerings this climb (bit per OFF_), and the save's byte they came from
    uint8_t offerByte;
    bool firstCleared;              // its first Guardian fell: the Offerings are spent
    bool hearthUsed, milestonePotion;
    int8_t notice;                  // a milestone just reached (MS_ + 1): its notice over the draft
    // draft
    bool draftOpen;
    uint32_t draftOpenMs;           // when it opened (input waits DRAFT_INPUT_DELAY_MS)
    int choices[DRAFT_CHOICES];     // a BoonId, or PACT_CARD
    int nChoices, sel;
} s;

static bool knows(int skill) { return (s.skills >> skill) & 1; }
static float berserk_bonus(void);
static void affix_names(uint8_t m, char *out, int n);
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
    for (int k = 0, r; (r = rival_k(b, k)) >= 0; k++) s.locked[r] = true;
    if (s.stacks[b] < BOONS[b].maxStacks) s.stacks[b]++;
    if (b == BOON_DEEP_FLASK) character.healthPotions = character.healthPotions + 3 > POTION_CAP ? POTION_CAP : character.healthPotions + 3;
    if (is_learn(b)) {      // learned, or its next level: at Lv 4 the next draft leads with its forms
        int k = b - BOON_LEARN_MOONBLADE;
        s.skills |= 1 << k;
        if (s.stacks[b] == SKILL_LV_MAX) s.formsDue |= 1 << k;
    }
    if (is_form(b)) s.formsDue &= ~(1 << form_skill(b));
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
    if (!shop) return false;                                            // a Shop in rows 3-4 on some lane
    // every path 2-4 fights in rows 2-5 (4-6 a circle with the two bosses): the fewest from each node up
    int least[MAP_LANES];
    for (int l = 0; l < MAP_LANES; l++) least[l] = is_fight(s.map.type[4][l]);
    for (int r = 3; r >= 1; r--) {
        int up[MAP_LANES];
        for (int l = 0; l < MAP_LANES; l++) {
            int m = 99;
            for (int k = 0; k < MAP_LANES; k++) if (linked(r, l, k) && least[k] < m) m = least[k];
            up[l] = m + is_fight(s.map.type[r][l]);
        }
        memcpy(least, up, sizeof least);
    }
    for (int l = 0; l < MAP_LANES; l++) if (least[l] < 2) return false;
    for (int r = 1; r <= 4; r++)        // no two Shops or Rests in a row on any path (row 6 is all Rests)
        for (int a = 0; a < MAP_LANES; a++) for (int b = 0; b < MAP_LANES; b++) {
            int t = s.map.type[r][a];
            if ((t == NODE_SHOP || t == NODE_REST) && linked(r, a, b) && s.map.type[r + 1][b] == t) return false;
        }
    return true;
}

static int s_mapTries;      // the last map's rolls (the self-check logs them)
// A circle's map: rows 2-5 rolled until they keep every rule
static void map_generate(void)
{
    memset(&s.map, 0, sizeof s.map);
    s_mapTries = 0;
    do {
        s_mapTries++;
        for (int r = 1; r <= 4; r++) for (int l = 0; l < MAP_LANES; l++) s.map.type[r][l] = roll_node();
        for (int l = 0; l < MAP_LANES; l++) s.map.type[5][l] = NODE_REST;
    } while (!map_rules_ok() && s_mapTries < 1000);
    if (!map_rules_ok()) {      // (never seen in 1,000 maps) Fights, and the Shop the rules ask for
        for (int r = 1; r <= 4; r++) for (int l = 0; l < MAP_LANES; l++) s.map.type[r][l] = NODE_FIGHT;
        s.map.type[2][1] = NODE_SHOP;
    }
#ifdef HLE_TOWER_SB_LAST    // test aid: Fights only (with the bosses in order below: the Fallen Warrior, the Sun, then Shadowborne)
#ifdef HLE_TOWER_ELITES     // (and Elites, not Fights: their affixes' heap, with HLE_ELITE_FORCE)
    for (int r = 1; r <= 4; r++) for (int l = 0; l < MAP_LANES; l++) s.map.type[r][l] = NODE_ELITE;
#else
    for (int r = 1; r <= 4; r++) for (int l = 0; l < MAP_LANES; l++) s.map.type[r][l] = NODE_FIGHT;
#endif
#endif
#if defined(HLE_TEST_TOWER_SHOP) && !defined(HLE_TEST_TOWER)    // test aids: straight up from the Guardian, a Shop (and an Event above it)
    s.map.type[1][1] = NODE_SHOP;
#ifdef HLE_EVENT_FORCE
    s.map.type[2][1] = NODE_EVENT;
#endif
#elif defined(HLE_EVENT_FORCE) && !defined(HLE_TEST_TOWER)
    s.map.type[1][1] = NODE_EVENT;
#endif
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

#ifdef HLE_ELITE_FORCE      // test aid: every Elite this affix (1-6, AFX_ + 1; 0 none), and the Bonus bosses outside a run too (the dummy)
#ifdef HLE_ELITE_MASK       // (several at once, a bit each: the name bar's widest tags)
int hle_elite_force = HLE_ELITE_MASK;
#else
int hle_elite_force = HLE_ELITE_FORCE ? 1 << (HLE_ELITE_FORCE - 1) : 0;    // (HLE_ELITE_SWEEP steps it)
#endif
#endif
// An Elite's affixes (section 11): one in circles 1-3, two from circle 4, of the unlocked ones (by the best
// floor); never Smouldering on Shadowborne (it has its own fire)
static uint8_t roll_affixes(int boss)
{
    int open[AFX_COUNT], n = 0, best = tower_best_floor();
    for (int a = 0; a < AFX_COUNT; a++) if (best >= AFFIXES[a].floor && !(a == AFX_SMOULDERING && boss == TOWER_SHADOWBORN)) open[n++] = a;
    uint8_t m = 0;
    for (int k = 0, want = s.circle >= AFX_TWO_CIRCLE ? 2 : 1; k < want && n; k++) { int i = rng_next() % n; m |= 1 << open[i]; open[i] = open[--n]; }
#ifdef HLE_ELITE_FORCE
    m = hle_elite_force & ~(boss == TOWER_SHADOWBORN ? 1 << AFX_SMOULDERING : 0);
#endif
    return m;
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
        if (t == NODE_ELITE || s.prisoner) s.map.afx[r][l] = roll_affixes(s.map.boss[r][l] - 1);    // (the jailer's Fight too: named on the map)
    }
}

// A new circle: its map, its night (rolled when the last one was cleared)
static void new_circle(void)
{
#ifdef HLE_AUTOPLAY     // test aid (HLE_RUN_PATH): what a circle left the run with
    { heap_stats_t h; sys_get_heap_stats(&h);
      debugf("[circle] %d ends at floor %d: health %.0f/%.0f, potions %d, gold %d, embers %d (%d fights) | heap %d KB free\n", s.circle, s.floor, (double)character.health,
             (double)character.maxHealth, character.healthPotions, s.gold, s.runEmbers, s.map.fights, (h.total - h.used) / 1024); }
#endif
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
        for (int k = 0; k < RELIC_SIGIL; k++) if (RELICS[k].fx == t->relics[i]) id = k;     // (never the Sigil: a milestone's)
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
static bool relic_owned_t(const TowerSave *t, int id)
{
    if (id == RELIC_SIGIL) return t->bestFloor >= MILESTONES[MS_SIGIL].floor;      // (never a bit: 1.0 has five relics)
    return id >= 0 && id < RELIC_COUNT && ((t->relicsOwned >> id) & 1);
}
bool tower_relic_owned(int id) { TowerSave t; tower_load(&t); return relic_owned_t(&t, id); }
int  tower_relic_cost(int id) { return tower_relic_owned(id) ? -1 : id == RELIC_SIGIL ? -2 : RELIC_COST; }   // (-2: not sold, a milestone's)
int  tower_relic_floor(int id) { return id == RELIC_SIGIL ? MILESTONES[MS_SIGIL].floor : 0; }
int  tower_relic_equipped(void)
{
    TowerSave t; tower_load(&t);
    int e = t.relicEquipped - 1;
    return relic_owned_t(&t, e) ? e : -1;
}
bool tower_relic_select(int id)
{
    TowerSave t; tower_load(&t);
    if (id < 0) { t.relicEquipped = 0; return save_controller_set_tower(&t); }     // climb without one
    if (id >= RELIC_COUNT) return false;
    if (!relic_owned_t(&t, id)) {                   // buy it
        if (id == RELIC_SIGIL) return false;        // (not sold)
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

// ---- Offerings: set aside in the save for the next climb (the byte's two nibbles, id + 1 each) ----

static int offer_at(uint8_t b, int k) { return ((b >> (4 * k)) & 15) - 1; }     // the k-th set aside, -1 none
static int offers_held(uint8_t b) { return (offer_at(b, 0) >= 0) + (offer_at(b, 1) >= 0); }
static bool offer_in(uint8_t b, int i) { return i >= 0 && (offer_at(b, 0) == i || offer_at(b, 1) == i); }
static uint8_t offers_now(void) { TowerSave t; save_controller_get_tower(&t); return t.offers; }
int  tower_offer_count(void) { return OFF_COUNT; }
const char *tower_offer_name(int i) { return i >= 0 && i < OFF_COUNT ? OFFERINGS[i].name : ""; }
const char *tower_offer_brief(int i) { return i >= 0 && i < OFF_COUNT ? OFFERINGS[i].brief : ""; }
bool tower_offer_set(int i) { return offer_in(offers_now(), i); }
int  tower_offers_held(void) { return offers_held(offers_now()); }
// why it can't be set aside now ("" if it can): a milestone not reached, the Skills setting off (the Candle)
const char *tower_offer_lock(int i)
{
    static char why[32];
    if (i < 0 || i >= OFF_COUNT) return "";
    if (tower_best_floor() < OFFERINGS[i].floor) { snprintf(why, sizeof why, "Reach floor %d", OFFERINGS[i].floor); return why; }
    if (i == OFF_CANDLE && !character_skills_enabled()) return "Skills are off";
    return "";
}
// its price now: to set it aside (the second one OFFER_SECOND more), or what taking it back returns (the
// price of a set of two is the two plus OFFER_SECOND, whichever came first)
int  tower_offer_price(int i)
{
    uint8_t b = offers_now();
    int n = offers_held(b);
    return OFFERINGS[i].price + ((offer_in(b, i) ? n == 2 : n >= 1) ? OFFER_SECOND : 0);
}
// A on it: set it aside (Embers paid now) or take it back (paid back); false if neither can be done
bool tower_offer_toggle(int i)
{
    if (i < 0 || i >= OFF_COUNT) return false;
    TowerSave t; tower_load(&t);
    int price = tower_offer_price(i);
    if (offer_in(t.offers, i)) {
        uint8_t keep = 0;
        for (int k = 0; k < OFFER_MAX; k++) if (offer_at(t.offers, k) >= 0 && offer_at(t.offers, k) != i) keep = (uint8_t)(offer_at(t.offers, k) + 1);
        int e = t.embers + price; t.embers = e > 65535 ? 65535 : e;
        t.offers = keep;
    } else {
        if (offers_held(t.offers) >= OFFER_MAX || tower_offer_lock(i)[0] || t.embers < price) return false;
        t.embers -= price;
        t.offers = offer_at(t.offers, 0) >= 0 ? (uint8_t)(t.offers | ((i + 1) << 4)) : (uint8_t)(i + 1);
    }
#ifdef HLE_AUTOPLAY
    debugf("[offer] %s %s for %d: set aside %02x, Embers %d\n", offer_in(t.offers, i) ? "set aside" : "taken back", OFFERINGS[i].name, price, t.offers, t.embers);
#endif
    return save_controller_set_tower(&t);
}
// "Spare Flask, Whetstone" (or "")
const char *tower_offers_line(void)
{
    static char line[48];
    uint8_t b = s.active ? s.offerByte : offers_now();
    int n = 0; line[0] = 0;
    for (int k = 0; k < OFFER_MAX; k++) if (offer_at(b, k) >= 0) n += snprintf(line + n, sizeof line - n, "%s%s", n ? ", " : "", OFFERINGS[offer_at(b, k)].name);
    return line;
}
// A climb ended before its first Guardian fell, alive (quit or restarted): its Offerings are set aside again
static void offers_restore(void)
{
    if (!s.active || s.firstCleared || !s.offerByte || character.health <= 0.0f) return;
    TowerSave t; tower_load(&t);
    if (!t.offers) { t.offers = s.offerByte; save_controller_set_tower(&t); }
#ifdef HLE_AUTOPLAY
    debugf("[offer] the climb ended before its first Guardian fell: %s set aside again\n", tower_offers_line());
#endif
    s.offerByte = 0;
}
bool tower_offer_active(int i) { return s.active && ((s.offers >> i) & 1); }

// ---- Milestones ----
int  tower_milestone_count(void) { return MS_COUNT; }
int  tower_milestone_floor(int i) { return MILESTONES[i].floor; }
const char *tower_milestone_name(int i) { return MILESTONES[i].name; }
const char *tower_milestone_reward(int i) { return MILESTONES[i].reward; }
bool tower_milestone_reached(int i) { return tower_best_floor() >= MILESTONES[i].floor; }
const char *tower_title_badge(void) { return tower_milestone_reached(MS_TITLE) ? MILESTONES[MS_TITLE].name : ""; }

// ---- Run ----

static bool offerable(BoonId b);
void tower_begin_run(uint32_t seed)
{
    offers_restore();       // (a restart before the first Guardian fell: the same Offerings again)
    memset(&s, 0, sizeof(s));
    if (ui_details()->owner == UI_OWNER_TOWER) ui_panel_close(ui_details());     // (one left open by a run quit from the pause menu)
    s.active = true;
    s.floor = 1;
    s.rng = seed ? seed : 1;
    { TowerSave t; tower_load(&t); s.packs = t.unlocks & ((1 << FAM_COUNT) - 1); s.forms = (t.unlocks >> FAM_COUNT) & ((1 << SKILL_COUNT) - 1); }
#ifdef HLE_TOWER_PACKS      // test aid: climb with these packs (bit per family) whatever the save holds
    s.packs = HLE_TOWER_PACKS;
#endif
#ifdef HLE_TOWER_SEED       // test aid: a set run seed (the map, the drafts)
    s.rng = HLE_TOWER_SEED;
#endif
    s.seed = s.rng;
#ifdef HLE_MILESTONE_AT     // test aid: the climb starts on this floor, the save's best one below it (its milestone's notice at the first kill)
    { TowerSave tm; tower_load(&tm); tm.bestFloor = HLE_MILESTONE_AT - 1; save_controller_set_tower(&tm); s.floor = HLE_MILESTONE_AT; }
#endif
    new_circle();
#if defined(HLE_TEST_TOWER_STATS) && !defined(HLE_STATS_BARE)   // test aid (BARE: as the run starts): a full bar (two of a family, two new boons, all at their most), every skill and its cards, every pact twice
    {
        static const BoonId FULL[TOWER_SLOTS] = { BOON_BLOODTHIRST, BOON_RALLY, BOON_PHANTOM_STEP, BOON_STATIC, BOON_EMBER_EDGE };
        for (int i = 0; i < TOWER_SLOTS; i++) while (s.stacks[FULL[i]] < BOONS[FULL[i]].maxStacks) take_boon(FULL[i]);
        s.stacks[BOON_RALLY] = 1;       // (one below its max: the page shows "Lv 1/2")
        s.skills = (1 << SKILL_COUNT) - 1;
        s.stacks[BOON_LEARN_MOONBLADE] = 2; s.stacks[BOON_LEARN_CLEAVE] = 1;
        s.stacks[BOON_LEARN_HALFMOON] = SKILL_LV_MAX; take_boon(BOON_TWIN_MOON);    // a form (its rival greyed)
    }
    for (int b = 0; b < PB_COUNT; b++) s.bless[b] = 2;
    for (int c = 0; c < PC_COUNT; c++) s.curse[c] = 2;
#endif
#ifdef HLE_SDMG_TOWER   // test aid (HLE_TEST_SKILL_DMG): every skill known (at Lv 1; HLE_SKILL_LV sets one's level and form)
    s.skills = (1 << SKILL_COUNT) - 1;
#ifdef HLE_SKILL_LV
    { static const int L[3] = { HLE_SKILL_LV }; tower_test_skill_lv(L[0] - 1, L[1] ? L[1] : 1, L[2]); }
#endif
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
    for (int b = BOON_HEMORRHAGE; b <= BOON_PROSPECTOR; b++) s.stacks[b] = BOONS[b].maxStacks;
    s.skills = (1 << SKILL_COUNT) - 1;
#endif
    for (int k = 0; k < SKILL_COUNT; k++)       // a skill known without its card (the Sage, a test aid) is at Lv 1
        if (knows(k) && !s.stacks[BOON_LEARN_MOONBLADE + k]) s.stacks[BOON_LEARN_MOONBLADE + k] = 1;
#ifdef HLE_RUN_GOLD         // test aid: start the climb with this much gold
    s.gold = HLE_RUN_GOLD;
#endif
#if defined(HLE_TEST_TOWER_SHOP) && !defined(HLE_TEST_TOWER)    // test aid: a level to buy (Vigor), a "+" (Storm Blade's), boons to release
    take_boon(BOON_STORM_BLADE); take_boon(BOON_VIGOR);
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
    // The Offerings set aside: this climb's, gone from the save (a quit before the first Guardian falls puts them back)
    {
        TowerSave t; tower_load(&t);
        s.milestonePotion = t.bestFloor >= MILESTONES[MS_POTION].floor;
        s.offerByte = t.offers;
        for (int k = 0; k < OFFER_MAX; k++) if (offer_at(t.offers, k) >= 0 && offer_at(t.offers, k) < OFF_COUNT) s.offers |= 1 << offer_at(t.offers, k);
        if (t.offers) { t.offers = 0; save_controller_set_tower(&t); }
    }
    if (tower_offer_active(OFF_PURSE)) s.gold += OFFER_GOLD;
    if (tower_offer_active(OFF_KINDLING)) {     // a random Common boon the draft could offer now (never Deep Flask: +3 potions at once)
        BoonId pool[BOON_COUNT]; int n = 0;
        for (int b = 0; b < BOON_COUNT; b++) if (BOONS[b].tier == TIER_COMMON && takes_slot(b) && b != BOON_DEEP_FLASK && offerable(b)) pool[n++] = b;
        if (n) take_boon(pool[rng_next() % n]);
    }
#ifdef HLE_AUTOPLAY
    if (s.offers) {
        int g = -1; for (int i = 0; i < TOWER_SLOTS; i++) if (s.slot[i]) g = s.slot[i] - 1;
        debugf("[offer] the climb starts with %s (seed %08lx): gold %d, max health %.0f, potions most %d, damage x%.2f, boon %s\n", tower_offers_line(), (unsigned long)s.seed, s.gold,
               (double)tower_mod_max_health(), tower_mod_max_potions(), (double)tower_mod_damage_dealt(), g >= 0 ? BOONS[g].name : "none");
    }
#endif
}

static void icons_free(void);
static int map_enter(void);
void tower_end_run(void) { offers_restore(); s.active = false; s.draftOpen = false; s.scr = SCR_NONE; icons_free(); if (ui_details()->owner == UI_OWNER_TOWER) ui_panel_close(ui_details()); }   // (a run quit from its Details)
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

// Gold won (Prospector: +20% a level, on every gain); returns it
static int gold_with(int g) { return (int)(g * (1.0f + PROSPECTOR_GOLD * tower_boon_stacks(BOON_PROSPECTOR)) + 0.5f); }
static int gain_gold(int g)
{
    g = gold_with(g);
    s.gold += g;
    return g;
}
int tower_gold(void) { return s.active ? s.gold : 0; }

// Embers banked now (the save and the run's count)
static void bank_embers(int e)
{
    TowerSave t; tower_load(&t);
    int m = t.embers + e; t.embers = m > 65535 ? 65535 : m;
    save_controller_set_tower(&t);
    s.runEmbers += e;
}

static void milestone_notice(int i);
void tower_floor_cleared(void)
{
    if (!s.active) return;
    TowerSave t;
    tower_load(&t);
    s.circleCleared = s.map.row == MAP_ROWS - 1;       // Guardian Unbound fell
    s.lastFloorGold = gain_gold((int)((GOLD_FIGHT + GOLD_PER_CIRCLE * s.circle) * (s.elite ? ELITE_GOLD : 1.0f) + 0.5f));
    s.lastFloorEmbers = floor_embers() + (s.circleCleared ? CIRCLE_EMBERS : 0);
    s.runEmbers += s.lastFloorEmbers;
    int e = t.embers + s.lastFloorEmbers;
    t.embers = e > 65535 ? 65535 : e;
    int was = t.bestFloor;
    if (s.floor > t.bestFloor) t.bestFloor = s.floor > 255 ? 255 : s.floor;
    save_controller_set_tower(&t);
    s.firstCleared = true;      // (the Offerings are spent)
    s.notice = 0;
    for (int i = 0; i < MS_COUNT; i++) if (was < MILESTONES[i].floor && t.bestFloor >= MILESTONES[i].floor) s.notice = (int8_t)(i + 1);
    if (s.circleCleared) {      // a circle cleared: its bonus (above), and the next circle's night
        s.nextNight = (Night)(1 + rng_next() % (NIGHT_COUNT - 1));
    }
    tower_draft_open();
    if (s.notice) milestone_notice(s.notice - 1);
}

void tower_next_floor(void)
{
    s.floor++;
    s.rally = 0.0f;
    s.unbrokenUsed = false;
    s.bleedLeft = 0.0f; s.overcharge = 0;
    s.perfectDodges = 0; s.vanishArmed = s.vanishUsed = false; s.berserkHits = 0;
    s.ironFirstUsed = false; s.steelHp = 0.0f; s.steelUntil = s.mercyUntil = s.drainHoldUntil = 0.0;
    for (int i = 0; i < 4; i++) s.boltAt[i] = 0.0;
    sync_max_health();      // (Steel Hearth's is for the floor)
    if (character.health > character.maxHealth) character.health = character.maxHealth;
    if (s.map.row == MAP_ROWS - 1) new_circle();     // its map shows before the Guardian
}

// ---- Draft ----


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
    // a Learn card, then the skill's levels (its stacks) to Lv 4; a form at Lv 4, a B form once
    // bought at the Shrine (its rival, the other form, is locked once one is taken)
    case BOON_FULL_MOON_DANCE: case BOON_TWIN_MOON: case BOON_CRESCENT_STORM:
        return s.stacks[BOON_LEARN_MOONBLADE + form_skill(b)] >= SKILL_LV_MAX;
    case BOON_SHATTERING_DANCE: case BOON_FULL_MOON: case BOON_LONG_CLEAVE:
        return s.stacks[BOON_LEARN_MOONBLADE + form_skill(b)] >= SKILL_LV_MAX && ((s.forms >> form_skill(b)) & 1);
    default: return true;
    }
}

// A card's rarity: a skill's level cards are Rare (as its "+" cards were), its Learn card Common
static int card_tier(int b) { return is_learn(b) && s.stacks[b] ? TIER_RARE : BOONS[b].tier; }

// A skill's level and form (section 6); outside a run Lv 1, no form
int tower_skill_level(int skill)
{
    if (!s.active || skill < 0 || skill >= SKILL_COUNT) return 1;
    if (tower_skill_form(skill)) return SKILL_LV_MAX + 1;
    int lv = s.stacks[BOON_LEARN_MOONBLADE + skill];
    return lv < 1 ? 1 : lv;
}
int tower_skill_form(int skill)
{
    if (!s.active || skill < 0 || skill >= SKILL_COUNT) return SKILL_FORM_NONE;
    return s.stacks[skill_form_card(skill, SKILL_FORM_A)] ? SKILL_FORM_A : s.stacks[skill_form_card(skill, SKILL_FORM_B)] ? SKILL_FORM_B : SKILL_FORM_NONE;
}
#ifdef HLE_AUTOPLAY
// test aid (HLE_SKILL_LV): a skill known at this level (1-5) and form (at 5: 1 A, 2 B), as if drafted
void tower_test_skill_lv(int skill, int lv, int form)
{
    if (skill < 0 || skill >= SKILL_COUNT) return;
    s.skills |= 1 << skill;
    s.stacks[BOON_LEARN_MOONBLADE + skill] = lv < SKILL_LV_MAX ? (lv < 1 ? 1 : lv) : SKILL_LV_MAX;
    BoonId a = skill_form_card(skill, SKILL_FORM_A), bf = skill_form_card(skill, SKILL_FORM_B);
    s.stacks[a] = s.stacks[bf] = 0; s.locked[a] = s.locked[bf] = false;
    if (lv >= SKILL_LV_MAX + 1) s.stacks[form == SKILL_FORM_B ? bf : a] = 1;
    s.formsDue = 0;
    debugf("[skilllv] skill %d: Lv %d, form %d\n", skill + 1, tower_skill_level(skill), tower_skill_form(skill));
}
// test aid (HLE_DUMMY_BUILD): a fresh run holding a build of section 14's table, its engine without the shared damage boons
// (and no "+"): 0 the sword alone, 1 Storm (Storm Blade, Static Lv 4, Thunderclap), 2 Full Moon Dance, 3 Twin Moon, 4 Crescent
// Storm, 5 the stagger build (Shattering Dance, Heavy Hand Lv 3, Crushing Blows, Executioner, Eclipse), 6 Full Moon, 7 Long
// Cleave, 8 Berserker, 9 Blood (Bloodthirst Lv 2, Rally Lv 2), 10 Rally Lv 1, 11 Rally Lv 2, 12 Bloodthirst Lv 2; the skill builds with Moon
// Mastery Lv 2. Returns the skill cast (1-3, 0 none)
int tower_test_build(int n)
{
    tower_begin_run(7);
    s.packs = (1 << FAM_COUNT) - 1; s.forms = (1 << SKILL_COUNT) - 1;
    #define MAXB(b) while (s.stacks[b] < BOONS[b].maxStacks) take_boon(b)
    static const int FORM[13][2] = { [2] = { 1, 1 }, [3] = { 2, 1 }, [4] = { 3, 1 }, [5] = { 1, 2 }, [6] = { 2, 2 }, [7] = { 3, 2 } };
    int skill = FORM[n][0];
    if (skill) { tower_test_skill_lv(skill - 1, 5, FORM[n][1]); take_boon(BOON_MOON_MASTERY); take_boon(BOON_MOON_MASTERY); }
    switch (n) {
    case 1: MAXB(BOON_STORM_BLADE); MAXB(BOON_STATIC); MAXB(BOON_THUNDERCLAP); break;
    case 5: MAXB(BOON_HEAVY_HAND); MAXB(BOON_CRUSHING_BLOWS); MAXB(BOON_EXECUTIONER); MAXB(BOON_ECLIPSE); break;
    case 8: MAXB(BOON_BERSERKER); break;
    case 9: MAXB(BOON_BLOODTHIRST); MAXB(BOON_RALLY); break;
    case 10: take_boon(BOON_RALLY); break;          // (the healing boons one at a time: Rally Lv 1, Lv 2, Bloodthirst Lv 2)
    case 11: MAXB(BOON_RALLY); break;
    case 12: MAXB(BOON_BLOODTHIRST); break;
    }
    #undef MAXB
    debugf("[build] %d: slots %d %d %d %d %d, skill %d Lv %d form %d, cooldowns x%.2f\n", n, s.slot[0], s.slot[1], s.slot[2], s.slot[3], s.slot[4], skill,
           skill ? tower_skill_level(skill - 1) : 0, skill ? tower_skill_form(skill - 1) : 0, (double)tower_mod_skill_cooldown());
    return skill;
}
// test aid (HLE_SDMG_STAGGERED): the stagger build's bonuses, past the cap (Executioner +60%, 3 Breakers +45%, Eclipse +50%)
void tower_test_stagger_build(void)
{
    take_boon(BOON_EXECUTIONER); take_boon(BOON_ECLIPSE); s.bless[PB_STAGGERED] = 3;
    debugf("[stagbuild] a staggered boss takes x%.2f from a skill, x%.2f from the sword\n", (double)tower_mod_stagger_damage(TOWER_HIT_SKILL), (double)tower_mod_stagger_damage(TOWER_HIT_SWORD));
}
#endif

// Hades' focus: a family with 2+ boons slotted comes up twice as often
static void fam_held(int famHeld[FAM_COUNT + 1])
{
    memset(famHeld, 0, sizeof(int) * (FAM_COUNT + 1));
    for (int i = 0; i < TOWER_SLOTS; i++) {
        if (s.merged[i][0]) { famHeld[BOONS[s.merged[i][0] - 1].family]++; famHeld[BOONS[s.merged[i][1] - 1].family]++; }
        else if (s.slot[i]) famHeld[BOONS[s.slot[i] - 1].family]++;
    }
}

// A card from the pool by rarity (legendary 6% on the first circle, +4% a circle, up to 25%; rare
// 30%; an Elite's Rare or better), family weighting included; it and its rival leave the pool
static BoonId pick_card(BoonId pool[], int *n, const int famHeld[])
{
    int leg = 6 + 4 * (tower_circle() - 1), r = rng_next() % 100;
    if (leg > 25) leg = 25;
    int want = r < leg ? TIER_LEGENDARY : (r < leg + 30 || s.elite) ? TIER_RARE : TIER_COMMON;
    int k = -1;
    for (int tries = 0; tries < 3 && k < 0; tries++) {     // that tier, or the next lower one (wrapping)
        int tier = (want + 3 - tries) % 3, cand[2 * BOON_COUNT], nc = 0;
        for (int i = 0; i < *n; i++) if (card_tier(pool[i]) == tier) {
            cand[nc++] = i;
            int f = BOONS[pool[i]].family;
            if (f < FAM_COUNT && famHeld[f] >= 2) cand[nc++] = i;
        }
        if (nc) k = cand[rng_next() % nc];
    }
    BoonId b = pool[k];
    pool[k] = pool[--*n];
    for (int j = 0, rv; (rv = rival_k(b, j)) >= 0; j++)
        for (int i = 0; i < *n; i++) if (pool[i] == rv) { pool[i] = pool[--*n]; break; }     // never both rivals on the table
    return b;
}

// A pact card not on the table yet (never a curse that undoes its blessing)
static int roll_pact(const int table[], int nTable)
{
    int card = 0;
    for (int tries = 0; tries < 8; tries++) {
        int b = rng_next() % PB_COUNT, c;
        do c = rng_next() % PC_COUNT;
        while ((b == PB_BULWARK && c == PC_FRAILTY) || (b == PB_MIGHT && c == PC_WEAKNESS));
#ifdef HLE_PACT_CURSE       // test aid: every pact with this curse
        c = HLE_PACT_CURSE;
#endif
        card = PACT_CARD(b, c);
        bool dup = false;
        for (int i = 0; i < nTable; i++) dup |= table[i] == card;
        if (!dup) break;
    }
    return card;
}

void tower_draft_open(void)
{
    // Three different boons. Until a skill is known a Learn card leads (section 14); after that a draft
    // holds one skill card a third of the time (about 11 by floor 35: one skill to Lv 5 and part of a
    // second), a skill at Lv 2+ twice as likely as each other one; a skill just at Lv 4 leads the next
    // draft with its forms (both, the B form once bought: two of the three cards)
    BoonId pool[BOON_COUNT], lead[BOON_COUNT], sk[BOON_COUNT]; int n = 0, nl = 0, nsk = 0;
    int famHeld[FAM_COUNT + 1];
    fam_held(famHeld);
    int due = -1;
    for (int k = 0; k < SKILL_COUNT && due < 0; k++) if ((s.formsDue >> k) & 1) due = k;
    s.formsDue = 0;
    for (int b = 0; b < BOON_COUNT; b++) {
        if (!offerable(b) || (s.elite && card_tier(b) == TIER_COMMON)) continue;    // an Elite's draft: Rare or better
        if (s.legendDraft && card_tier(b) != TIER_LEGENDARY) continue;              // the Prisoner's Elite: all Legendary
        if (due >= 0 && is_form(b) && form_skill(b) == due) continue;              // (they lead, below)
        if (!s.skills && is_learn(b)) lead[nl++] = b;
        else if (is_skill_card(b)) { sk[nsk++] = b; if (is_learn(b) && s.stacks[b] >= 2) sk[nsk++] = b; }
        else pool[n++] = b;
    }
    s.nChoices = 0;
    if (due >= 0) {
        for (int f = SKILL_FORM_A; f <= SKILL_FORM_B; f++) if (offerable(skill_form_card(due, f))) s.choices[s.nChoices++] = skill_form_card(due, f);
    } else if (nl > 0) {
        int k = rng_next() % nl;
        s.choices[s.nChoices++] = lead[k];
        lead[k] = lead[--nl];
    }
    if (nsk && s.nChoices < DRAFT_CHOICES && (int)(rng_next() % 100) < SKILL_CARD_CHANCE) s.choices[s.nChoices++] = sk[rng_next() % nsk];
    if (tower_offer_active(OFF_CANDLE) && s.floor == 1 && due < 0) {     // Scholar's Candle: the climb's first draft holds two skill cards
        int have = 0;
        for (int i = 0; i < s.nChoices; i++) have += is_skill_card((BoonId)s.choices[i]);
        while (have < 2 && s.nChoices < DRAFT_CHOICES && (nl || nsk)) {
            BoonId c;
            if (nl) { int k = rng_next() % nl; c = lead[k]; lead[k] = lead[--nl]; }
            else { int k = rng_next() % nsk; c = sk[k]; sk[k] = sk[--nsk]; }
            bool dup = false;
            for (int i = 0; i < s.nChoices; i++) dup |= s.choices[i] == (int)c;
            if (!dup) { s.choices[s.nChoices++] = c; have++; }
        }
    }
    // The rest by rarity, a pact now and then; once the boons run out, a +40 gold card and pacts
    bool purse = false;
    while (s.nChoices < DRAFT_CHOICES) {
        if (n == 0 && !purse) { purse = true; s.choices[s.nChoices++] = GOLD_CARD; continue; }
        if (n == 0 || (int)(rng_next() % 100) < PACT_CHANCE) { s.choices[s.nChoices] = roll_pact(s.choices, s.nChoices); s.nChoices++; continue; }
        s.choices[s.nChoices++] = pick_card(pool, &n, famHeld);
    }
    // An Elite with two affixes (circle 4 on): its last card a duo when one is ready (section 11)
    if (s.elite && __builtin_popcount(s.affixes) >= 2) {
        int duo[BOON_COUNT], nd = 0; bool has = false;
        for (int i = 0; i < s.nChoices; i++) has |= s.choices[i] < BOON_COUNT && BOONS[s.choices[i]].duo;
        for (int b = 0; b < BOON_COUNT; b++) if (BOONS[b].duo && offerable(b)) duo[nd++] = b;
        if (!has && nd) s.choices[s.nChoices - 1] = duo[rng_next() % nd];
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
        else if (c == GOLD_CARD) debugf(" +%d gold", GOLD_CARD_GOLD);
        else debugf(" Pact of %s / %s", BLESSINGS[(c - BOON_COUNT) / PC_COUNT].name, CURSES[(c - BOON_COUNT) % PC_COUNT].name);
    }
    debugf("%s%s%s | +%d gold, %d now\n", s.circleCleared ? " | circle cleared" : "", s.elite ? " | elite" : "", s.legendDraft ? " (all Legendary)" : "", s.lastFloorGold, s.gold);
#endif
}

bool tower_draft_is_open(void) { return s.draftOpen; }
int  tower_draft_selected(void) { return s.sel; }
int  tower_draft_card(int i) { return (i >= 0 && i < s.nChoices && s.choices[i] < BOON_COUNT) ? s.choices[i] : -1; }   // a boon, -1 a pact

// A curse taken (a pact's, the Mirror of Shadows'): Thirst takes a potion now, or the next one owed
static void add_curse(int c)
{
    s.curse[c]++;
#ifdef HLE_PACT_CURSE
    character.healthPotions = 0;      // test aid: no potion to give up
#endif
    if (c == PC_THIRST) { if (character.healthPotions > 0) character.healthPotions--; else s.potionDebt++; }
}

// A pact card taken (a draft's, the Shop's free one): its blessing and its curse
static void take_pact(int card)
{
    int b = (card - BOON_COUNT) / PC_COUNT, c = (card - BOON_COUNT) % PC_COUNT;
    s.bless[b]++;
    if (b == PB_VITALITY) { character.health += 12.0f; sync_max_health(); }
    add_curse(c);
#ifdef HLE_AUTOPLAY
    debugf("[pact] took %s (%d) / %s (%d): damage x%.2f, taken x%.2f, max health %.0f, potions %d (owed %d)\n", BLESSINGS[b].name, s.bless[b],
           CURSES[c].name, s.curse[c], (double)tower_mod_damage_dealt(), (double)tower_mod_damage_taken(), (double)tower_mod_max_health(),
           character.healthPotions, s.potionDebt);
#endif
}

void tower_draft_pick(int index)
{
    if (index == s.nChoices) {          // Skip: a few Embers and some gold instead
        bank_embers(SKIP_EMBERS);
        int g = gain_gold(SKIP_GOLD);
#ifdef HLE_AUTOPLAY
        debugf("[gold] skip: +%d, %d now\n", g, s.gold);
#endif
        (void)g;
    } else if (index >= 0 && index < s.nChoices && s.choices[index] == GOLD_CARD) {
        int g = gain_gold(GOLD_CARD_GOLD);
#ifdef HLE_AUTOPLAY
        debugf("[gold] the purse card: +%d, %d now\n", g, s.gold);
#endif
        (void)g;
    } else if (index >= 0 && index < s.nChoices && s.choices[index] >= BOON_COUNT) {
        take_pact(s.choices[index]);
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
// The title-safe area for the screens between fights: never wider than the fixed 7.5% one (the default
// overscan setting shows a little more of the screen, a larger one less: the edges' lines move in with it)
static int safe_mx(void) { int m = ui_safe_margin_x(); return m < TITLE_SAFE_MARGIN_X ? TITLE_SAFE_MARGIN_X : m; }
static int safe_my(void) { int m = ui_safe_margin_y(); return m < TITLE_SAFE_MARGIN_Y ? TITLE_SAFE_MARGIN_Y : m; }
#define SAFE_X0 safe_mx()
#define SAFE_X1 (SCREEN_WIDTH - safe_mx())
#define SAFE_Y0 safe_my()
#define SAFE_Y1 (SCREEN_HEIGHT - safe_my())
// A screen h px tall from its top margin: the margin above, as far as the screen still holds it (never under the
// 7.5% one): a full screen (the draft, the Shop) stays there at a larger overscan, a shorter one moves in with it
static int fit_my(int h) { int most = (SCREEN_HEIGHT - h) / 2; if (most < TITLE_SAFE_MARGIN_Y) most = TITLE_SAFE_MARGIN_Y; return safe_my() < most ? safe_my() : most; }

static void flat_mode(void)
{
    rdpq_sync_pipe();
    rdpq_set_mode_standard();
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
}

// A card's frame tells the rarity: bronze, blue, gold; a pact crimson (brighter when selected)
static const color_t FRAME[4][2] = {
    { RGBA32(90, 70, 50, 255),  RGBA32(230, 150, 50, 255) },
    { RGBA32(50, 80, 150, 255), RGBA32(110, 170, 255, 255) },
    { RGBA32(150, 115, 30, 255), RGBA32(255, 215, 90, 255) },
    { RGBA32(120, 20, 30, 255), RGBA32(230, 50, 60, 255) },
};
static const int NAME_STYLE[3] = { FONT_STYLE_WHITE, FONT_STYLE_BLUE, FONT_STYLE_GOLD };

#define DRAFT_CARD_H 136
static void tower_draft_draw(void)
{
    if (!s.draftOpen) return;
    flat_mode();
    rdpq_set_prim_color(RGBA32(0, 0, 0, 170));
    rdpq_fill_rectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

    char line[80];
    const int T = fit_my(200);      // (the header's three lines, the cards and Skip: 200 px)
    snprintf(line, sizeof line, "Floor %d cleared   +%d Embers   +%d gold", s.floor, s.lastFloorEmbers, s.lastFloorGold);
    text_style(0, T + 9, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_GOLD, line);
    if (s.circleCleared) {
        snprintf(line, sizeof line, "Circle cleared: +%d Embers", CIRCLE_EMBERS);
        text_style(0, T + 22, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_BLUE, line);
    }
    // the next floor on the third line (a new circle's night in red: its effect is on the map, its Guardian's page and Stats)
    if (s.circleCleared) snprintf(line, sizeof line, "Next: floor %d, a new circle: %s", s.floor + 1, NIGHTS[s.nextNight].name);
    else snprintf(line, sizeof line, "Choose a Boon   Next: floor %d", s.floor + 1);
    text_style(0, T + 35, SCREEN_WIDTH, ALIGN_CENTER, s.circleCleared ? FONT_STYLE_RED : FONT_STYLE_WHITE, line);

    const int cw = 96, ch = DRAFT_CARD_H, gap = 6, y0 = T + 42;     // (tall enough for a rival or a duo's two boons, or a form's rival, under the text)
    int x0 = (SCREEN_WIDTH - (cw * s.nChoices + gap * (s.nChoices - 1))) / 2;
    for (int i = 0; i < s.nChoices; i++) {
        int x = x0 + i * (cw + gap), card = s.choices[i];
        bool pact = card >= BOON_COUNT && card != GOLD_CARD;
        int tier = pact ? 3 : card == GOLD_CARD ? TIER_LEGENDARY : card_tier(card);
        flat_mode();
        bool on = i == s.sel;
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
        if (card == GOLD_CARD) {    // a complete build's purse
            text_style(x + 4, y0 + 27, cw - 8, ALIGN_CENTER, FONT_STYLE_GOLD, "Purse");
            snprintf(line, sizeof line, "+%d gold", GOLD_CARD_GOLD);
            text(x + 6, y0 + 48, cw - 12, ALIGN_CENTER, line);
            text_style(x - 1, y0 + ch - 3, cw + 2, ALIGN_CENTER, FONT_STYLE_GREY, "No slot");
            continue;
        }
        const BoonDef *bd = &BOONS[card];
        // the family's tag over the name, in its colour (a duo's two families, gold)
        if (bd->duo) {
            snprintf(line, sizeof line, "%s+%s", FAMILIES[bd->family].name, FAMILIES[bd->duo - 1].name);
            text_style(x + 2, y0 + 12, cw - 4, ALIGN_CENTER, FONT_STYLE_GOLD, line);
        } else if (is_form(card)) text_style(x + 4, y0 + 12, cw - 8, ALIGN_CENTER, FONT_STYLE_GOLD, "Lv 5 form");     // (in place of "Skill")
        else text_style(x + 4, y0 + 12, cw - 8, ALIGN_CENTER, FAMILIES[bd->family].style, FAMILIES[bd->family].name);
        int nameLines = text_style(x + 4, y0 + 27, cw - 8, ALIGN_CENTER, NAME_STYLE[tier], bd->name);
        const char *desc = bd->desc;    // a Learn card names the skill's button in the layout; a level what it adds
        if (is_learn(card) && s.stacks[card]) desc = SKILL_LV_DESC[card - BOON_LEARN_MOONBLADE][s.stacks[card] - 1];
        else if (is_learn(card)) {
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
            else if (bd->family == FAM_SKILL && !s.stacks[card]) snprintf(bottom[0], 40, "Skill: no slot");
            else if (s.stacks[card]) snprintf(bottom[0], 40, "Lv %d -> %d", s.stacks[card], s.stacks[card] + 1);
            else snprintf(bottom[0], 40, "New: slot %d/%d", slots_used() < TOWER_SLOTS ? slots_used() + 1 : TOWER_SLOTS, TOWER_SLOTS);
            if (r >= 0) { rival_names(card, bottom[1], 40); snprintf(bottom[2], 40, "Locks out"); bstyle[1] = bstyle[2] = FONT_STYLE_RED; nb = 3; }
            if (is_form(card)) { snprintf(bottom[0], 40, "%s", BOONS[r].name); snprintf(bottom[1], 40, "Locks out"); bstyle[0] = FONT_STYLE_RED; nb = 2; }    // (its tag says the form)
        }
        // up from the foot, 12 apart; a long name (a form's) wraps, its lines 17 apart
        for (int k = 0, y = y0 + ch - 3; k < nb; k++, y -= 12) {
            int n = (int)strlen(bottom[k]), lines = 1;
            if (n > 12) { rdpq_paragraph_t *p = rdpq_paragraph_build(&(rdpq_textparms_t){ .width = cw + 2, .wrap = WRAP_WORD }, FONT_UNBALANCED, bottom[k], &n); lines = p->nlines; rdpq_paragraph_free(p); }
            y -= 17 * (lines - 1);
            text_style(x - 1, y, cw + 2, ALIGN_CENTER, bstyle[k], bottom[k]);
        }
    }
    // Skip, under the cards
    // A button of its own under the cards, framed like them: Down to it, Up back to the card
    bool skip = s.sel == s.nChoices;
    const int bw = 160, bh = 16, by = y0 + ch + 4;
    const int bx = SAFE_X1 - 68 - bw < (SCREEN_WIDTH - bw) / 2 - 16 ? SAFE_X1 - 68 - bw : (SCREEN_WIDTH - bw) / 2 - 16;     // (left of centre, the R hint beside it at any overscan)
    if (s.sel < s.nChoices) ui_hint(SAFE_X1, by + 12, "R Details");     // (beside Skip)
    flat_mode();
    rdpq_set_prim_color(skip ? RGBA32(255, 215, 90, 255) : RGBA32(110, 100, 90, 255));
    rdpq_fill_rectangle(bx - 2, by - 2, bx + bw + 2, by + bh + 2);
    rdpq_set_prim_color(skip ? RGBA32(60, 30, 20, 240) : RGBA32(30, 22, 18, 230));
    rdpq_fill_rectangle(bx, by, bx + bw, by + bh);
    snprintf(line, sizeof line, "Skip: +%d Embers, +%d gold", SKIP_EMBERS, gold_with(SKIP_GOLD));
    text_style(bx, by + 12, bw, ALIGN_CENTER, skip ? FONT_STYLE_GOLD : FONT_STYLE_GREY, line);
}

// ---- The map screen, and a Rest's ----

static sprite_t *s_icons;       // the six node icons (16x16 I4 one above the next: Fight, Elite, Event, Shop, Rest, Boss), loaded with the map
static void icons_free(void) { if (s_icons) { rspq_wait(); sprite_free(s_icons); s_icons = NULL; } }

bool tower_map_is_open(void) { return s.scr == SCR_MAP || s.scr == SCR_CONFIRM; }    // (its confirm screen too)
bool tower_rest_is_open(void) { return s.scr >= SCR_REST && s.scr != SCR_CONFIRM; }
// The run's first fight is always the circle's Guardian: no map before it (the scene starts it at once)
void tower_enter_guardian(void) { if (s.map.row < 0) { s.map.sel = 1; map_enter(); } }
bool tower_between_open(void) { return s.draftOpen || s.scr != SCR_NONE; }

#ifdef HLE_RUN_PATH     // test aid: the climb's route (section 13): 1 safe (the fewest fights, a Shop or Event over a Fight, no
                        // Elite), 2 normal (3 fights in rows 2-5 where it can), 3 greedy (the most fights, Elites first)
static int path_score(int r, int l, int fights, int elites, int other)
{
    int t = s.map.type[r][l];
    fights += t == NODE_FIGHT || t == NODE_ELITE; elites += t == NODE_ELITE; other += t == NODE_SHOP || t == NODE_EVENT;
    if (r < MAP_ROWS - 3) {     // on up to row 5
        int best = -1 << 30;
        for (int k = 0; k < MAP_LANES; k++) if (s.map.type[r + 1][k] && linked(r, l, k)) { int v = path_score(r + 1, k, fights, elites, other); if (v > best) best = v; }
        return best;
    }
    return HLE_RUN_PATH == 1 ? -100 * fights - 30 * elites + 10 * other : HLE_RUN_PATH == 3 ? 100 * fights + 30 * elites : -100 * abs(fights - 3) - 30 * elites + 5 * other;
}
#endif
void tower_map_open(void)
{
    s.scr = SCR_MAP;
    s.scrOpenMs = get_ticks_ms();
    if (!s_icons) s_icons = sprite_load("rom:/ui/map/map_icons.i4.sprite");
    map_assign_bosses();
    int r = s.map.row + 1, l = s.map.row >= 0 ? s.map.path[s.map.row] : 1;     // the cursor: straight up, else the first reachable
    if (!map_reachable(r, l)) for (l = 0; l < MAP_LANES - 1 && !map_reachable(r, l); l++) {}
#ifdef HLE_RUN_PATH
    if (r >= 1 && r <= MAP_ROWS - 3) {
        int best = -1 << 30;
        for (int k = 0; k < MAP_LANES; k++) if (map_reachable(r, k)) { int v = path_score(r, k, s.map.fights > 0 ? s.map.fights - 1 : 0, 0, 0); if (v > best) { best = v; l = k; } }
    }
#endif
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
    s.legendDraft = false;
    if (t == NODE_FIGHT && s.prisoner) { s.elite = s.legendDraft = true; s.prisoner = false; }     // the Chained Prisoner's jailer
    if (t == NODE_BOSS) s.boss = r ? TOWER_UNBOUND : TOWER_GUARDIAN;
    else if (t == NODE_FIGHT || t == NODE_ELITE) s.boss = (TowerBoss)(s.map.boss[r][l] - 1);
    s.affixes = s.elite && (unsigned)s.boss >= TOWER_FALLEN_KNIGHT && (unsigned)s.boss <= TOWER_SHADOWBORN ? s.map.afx[r][l] : 0;     // the Bonus bosses only
    if (is_fight(t)) { s.map.fights++; s.map.met |= 1 << s.boss; }
#ifdef HLE_AUTOPLAY
    debugf("[map] entered row %d lane %d: %s%s%s (floor %d, fight %d of the circle)%s%s\n", r + 1, l, NODE_NAMES[t], is_fight(t) ? " " : "",
           is_fight(t) ? ((unsigned)s.boss <= TOWER_SHADOWBORN ? BOSS_NAMES[s.boss] : "(no boss set)") : "", s.floor, s.map.fights, s.affixes ? ", affixes: " : "", tower_affix_tag());
#endif
    return t;
}

// What a Rest can Temper: a level of a slotted boon below its max, a "+" unlocked by a maxed one, a
// known skill's next level below Lv 4; never a new boon, a duo or a skill's form (a draft's choice)
static int temper_targets(int8_t out[], int max)
{
    int n = 0;
    for (int b = 0; b < BOON_COUNT && n < max; b++)
        if (offerable(b) && !BOONS[b].duo && !is_form(b) && (is_learn(b) ? knows(b - BOON_LEARN_MOONBLADE) : s.stacks[b] || BOONS[b].up)) out[n++] = (int8_t)b;
    return n;
}


// ---- What a Shop and an Event can give ----

// A Shop's boon card: a new boon or a duo (no skill card, no "+": the shelf sells those as levels)
static bool shop_card_ok(BoonId b) { return offerable(b) && !is_skill_card(b) && !BOONS[b].up && !s.stacks[b]; }
// A level the Shop sells: a slotted boon below its max (a duo has none)
static int level_targets(int8_t out[], int max)
{
    int n = 0;
    for (int i = 0; i < TOWER_SLOTS && n < max; i++) {
        BoonId b = (BoonId)(s.slot[i] - 1);
        if (s.slot[i] && !s.merged[i][0] && s.stacks[b] < BOONS[b].maxStacks) out[n++] = (int8_t)b;
    }
    return n;
}
// A "+" the Shop sells: a maxed boon's, held
static int plus_targets(int8_t out[], int max)
{
    int n = 0;
    for (int b = 0; b < BOON_COUNT && n < max; b++) if (BOONS[b].up && offerable(b)) out[n++] = (int8_t)b;
    return n;
}
// An Event's random Rare boon (a new one or a level; never a skill card, a "+" or a duo); -1 none.
// roll false: only whether there is one (no roll used)
static int rare_boon(bool roll)
{
    int pool[BOON_COUNT], n = 0;
    for (int b = 0; b < BOON_COUNT; b++) if (offerable(b) && BOONS[b].tier == TIER_RARE && !is_skill_card(b) && !BOONS[b].up) pool[n++] = b;
    return !n ? -1 : roll ? pool[rng_next() % n] : pool[0];
}
// The Mirror of Shadows: a slotted boon's next level, or its "+" at its max
static int mirror_targets(int out[])
{
    int n = 0;
    for (int i = 0; i < TOWER_SLOTS; i++) {
        BoonId b = (BoonId)(s.slot[i] - 1);
        if (!s.slot[i] || s.merged[i][0]) continue;
        if (s.stacks[b] < BOONS[b].maxStacks) { out[n++] = b; continue; }
        for (int p = 0; p < BOON_COUNT; p++) if (BOONS[p].up == b + 1 && offerable(p)) out[n++] = p;
    }
    return n;
}
// The Moonlit Pool: a skill to learn, or a known skill's next level (below Lv 4: its card)
static int pool_targets(bool learn, int out[])
{
    int n = 0;
    for (int k = 0; k < SKILL_COUNT; k++) if (knows(k) != learn && offerable(BOON_LEARN_MOONBLADE + k)) out[n++] = BOON_LEARN_MOONBLADE + k;
    return n;
}

static const struct { const char *name, *text; uint8_t choices; } EVENTS[EV_COUNT] = {
    [EV_ALTAR]    = { "The Bleeding Altar",  "An altar of red stone, still wet. It asks for blood, for the rest of the climb.", 2 },
    [EV_GRAVE]    = { "A Knight's Grave",    "A knight lies under his shield. Coins were left on the stone, and a prayer is carved in it.", 3 },
    [EV_MIRROR]   = { "Mirror of Shadows",   "Your reflection holds out its hand. It will make a boon of yours stronger, and leave something of itself in you.", 2 },
    [EV_MERCHANT] = { "Merchant of Ash",     "A merchant by a cold fire. He sells potions past what your belt should hold, and buys them back.", 3 },
    [EV_POOL]     = { "Moonlit Pool",        "Moonlight lies on still water. Something in it remembers the old techniques.", 3 },
    [EV_PRISONER] = { "The Chained Prisoner","A prisoner begs to be freed. Its jailer will come for you at your next fight, and bring its treasure.", 2 },
    [EV_BONES]    = { "Gambler's Bones",     "A cup of bones, a voice in the dark: fifty to play, a hundred and twenty back if they fall your way.", 2 },
    [EV_BRAZIER]  = { "Ember Brazier",       "A brazier burns without fuel. Take its embers to the Shrine, or its warmth into the climb.", 3 },
};
#define MERCHANT_POTION 70      // over the Shop's 45: it sells past the run's max (up to POTION_CAP)
#define GRAVE_GOLD      60
#define BONES_STAKE     50
#define BONES_WIN       120
#define BRAZIER_EMBERS  30      // x circle

// can choice i of the Event on screen be taken now (the last, Leave, always)
static bool event_ok(int i)
{
    int e = s.event, t[BOON_COUNT];
    if (i == EVENTS[e].choices - 1) return true;
    switch (e) {
    case EV_ALTAR:    return rare_boon(false) >= 0;
    case EV_GRAVE:    return i == 0 || character.health < character.maxHealth;
    case EV_MIRROR:   return mirror_targets(t) > 0;
    case EV_MERCHANT: return i == 0 ? s.gold >= MERCHANT_POTION && character.healthPotions < POTION_CAP : character.healthPotions >= 2 && rare_boon(false) >= 0;
    case EV_POOL:     return pool_targets(i == 0, t) > 0;
    case EV_BONES:    return s.gold >= BONES_STAKE;
    default:          return true;
    }
}

static bool row_ok(int i)
{
    if (s.scr == SCR_EVENT) return s.eventDone || event_ok(s.list[i]);
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
    else if (scr == SCR_SHOP_LEVEL) { s.listN = (int8_t)level_targets(s.list, LIST_MAX - 1); s.list[s.listN++] = -1; }
    else if (scr == SCR_SHOP_PLUS) { s.listN = (int8_t)plus_targets(s.list, LIST_MAX - 1); s.list[s.listN++] = -1; }
    else if (scr == SCR_SHOP_RELEASE) { for (int i = 0; i < TOWER_SLOTS; i++) if (s.slot[i]) s.list[s.listN++] = (int8_t)i; s.list[s.listN++] = -1; }
    else if (scr == SCR_EVENT) { int n = s.eventDone ? 1 : EVENTS[s.event].choices; for (int i = 0; i < n; i++) s.list[s.listN++] = (int8_t)i; }   // its choices (Leave last), or Continue
    else { s.list[s.listN++] = -1; for (int i = 0; i < TOWER_SLOTS; i++) if (s.slot[i]) s.list[s.listN++] = (int8_t)i; }   // Keep, then the slots
    while (s.listSel < s.listN - 1 && !row_ok(s.listSel)) s.listSel++;
#ifdef HLE_RUN_PATH     // test aid: the route's Rest: heal below 60%, else Temper (else potions); A takes the row under the cursor
    if (scr == SCR_REST && character.health >= 0.6f * character.maxHealth) s.listSel = row_ok(REST_TEMPER_ROW) ? REST_TEMPER_ROW : row_ok(REST_POTIONS_ROW) ? REST_POTIONS_ROW : 0;
#endif
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

// Hearth Ember: the climb's first Rest heals 40% on top of its choice
static void rest_offering(void)
{
    if (!tower_offer_active(OFF_HEARTH) || s.hearthUsed) return;
    s.hearthUsed = true;
    float h = character.health;
    character.health = fminf(character.maxHealth, h + OFFER_HEAL * character.maxHealth);
#ifdef HLE_AUTOPLAY
    debugf("[offer] Hearth Ember: the first Rest heals 40%% more: %.0f -> %.0f of %.0f\n", (double)h, (double)character.health, (double)character.maxHealth);
#endif
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
    rest_offering();
}


// ---- The Shop: one menu, Left/Right along the shelf, A buys, B (or Leave) leaves ----

static int shop_price(int i)
{
    static const int P[SHOP_ITEMS] = { [SHOP_LEVEL] = PRICE_LEVEL, [SHOP_PLUS] = PRICE_PLUS, [SHOP_POTION] = PRICE_POTION, [SHOP_MEND] = PRICE_MEND, [SHOP_RELEASE] = PRICE_RELEASE };
    if (i <= SHOP_CARD2) return s.shop.card[i] >= 0 ? TIER_PRICE[BOONS[s.shop.card[i]].tier] : 0;
    return i < SHOP_ITEMS ? P[i] : 0;
}
// why item i can't be bought now ("": it can). Each item once a visit (one of the three cards); potions up to the run's max
static const char *shop_block(int i)
{
    int8_t t[LIST_MAX];
    if (i == SHOP_LEAVE) return "";
    if (i <= SHOP_CARD2 && s.shop.card[i] < 0) return "No new boon fits";
    if ((s.shop.sold >> i) & 1) return "Sold";
    if (i == SHOP_LEVEL && !level_targets(t, LIST_MAX)) return "No slotted boon below its max";
    if (i == SHOP_PLUS && !plus_targets(t, LIST_MAX)) return "No maxed boon with a \"+\"";
    if (i == SHOP_POTION && character.healthPotions >= tower_mod_max_potions()) return "Potions at the run's max";
    if (i == SHOP_MEND && character.health >= character.maxHealth) return "Health full";
    if (i == SHOP_RELEASE && !slots_used()) return "No boon slotted";
    if (s.gold < shop_price(i)) return "Not enough gold";
    return "";
}
#ifdef HLE_AUTOPLAY
static const char *const SHOP_NAMES[SHOP_ITEMS] = { "a boon", "a boon", "a boon", "a level", "a \"+\"", "the pact", "a potion", "Mend", "Release", "Leave" };
#endif

// pay for item i (never below 0: shop_block checked it); the item is sold for this visit (a card: all three)
static void shop_pay(int i, const char *what)
{
    int g = s.gold, p = shop_price(i);
    s.gold -= p;
    if (i != SHOP_POTION) s.shop.sold |= i <= SHOP_CARD2 ? 7 : 1 << i;
#ifdef HLE_AUTOPLAY
    debugf("[shop] bought %s (%s) for %d: gold %d -> %d%s\n", SHOP_NAMES[i], what, p, g, s.gold, s.gold < 0 ? " NEGATIVE" : "");
#endif
    (void)g; (void)what;
}

static void shop_buy(int i);
static void shop_open(void)
{
    BoonId pool[BOON_COUNT]; int n = 0, famHeld[FAM_COUNT + 1];
    fam_held(famHeld);
    for (int b = 0; b < BOON_COUNT; b++) if (shop_card_ok(b)) pool[n++] = b;
    for (int i = 0; i < 3; i++) s.shop.card[i] = (int16_t)(n ? pick_card(pool, &n, famHeld) : -1);     // rolled as a draft's (family weighting too)
    s.shop.pact = roll_pact(NULL, 0);
    s.shop.sold = 0; s.shop.sel = 0;
    s.scr = SCR_SHOP; s.scrOpenMs = get_ticks_ms();
#ifdef HLE_RUN_PATH     // test aid: the route's shopping (B leaves after): Mend below 65%, potions to the run's max, the cheapest card it can pay for
    if (character.health < 0.65f * character.maxHealth && !shop_block(SHOP_MEND)[0]) shop_buy(SHOP_MEND);
    while (!shop_block(SHOP_POTION)[0]) shop_buy(SHOP_POTION);
    { int c = -1; for (int i = SHOP_CARD0; i <= SHOP_CARD2; i++) if (!shop_block(i)[0] && (c < 0 || shop_price(i) < shop_price(c))) c = i;
      if (c >= 0) shop_buy(c); }
#endif
#ifdef HLE_AUTOPLAY
    int b = (s.shop.pact - BOON_COUNT) / PC_COUNT, c = (s.shop.pact - BOON_COUNT) % PC_COUNT;
    debugf("[shop] open, gold %d: cards", s.gold);
    for (int i = 0; i < 3; i++) debugf(" %s (%d)", s.shop.card[i] >= 0 ? BOONS[s.shop.card[i]].name : "-", s.shop.card[i] >= 0 ? shop_price(i) : 0);
    debugf(", pact %s / %s, health %.0f/%.0f, potions %d/%d\n", BLESSINGS[b].name, CURSES[c].name, (double)character.health, (double)character.maxHealth,
           character.healthPotions, tower_mod_max_potions());
#endif
}

static void shop_buy(int i)
{
    const char *why = shop_block(i);
    if (i == SHOP_LEAVE) {
#ifdef HLE_AUTOPLAY
        debugf("[shop] left with %d gold\n", s.gold);
#endif
        tower_map_open();
        return;
    }
    if (why[0]) {
#ifdef HLE_AUTOPLAY
        debugf("[shop] %s: can't (%s), gold %d\n", SHOP_NAMES[i], why, s.gold);
#endif
        return;
    }
    char what[48] = "";
    switch (i) {
    case SHOP_LEVEL:   list_open(SCR_SHOP_LEVEL); return;      // (paid when one is chosen)
    case SHOP_PLUS:    list_open(SCR_SHOP_PLUS); return;
    case SHOP_RELEASE: list_open(SCR_SHOP_RELEASE); return;
    case SHOP_PACT:
        snprintf(what, sizeof what, "%s / %s", BLESSINGS[(s.shop.pact - BOON_COUNT) / PC_COUNT].name, CURSES[(s.shop.pact - BOON_COUNT) % PC_COUNT].name);
        shop_pay(i, what); take_pact(s.shop.pact); return;
    case SHOP_POTION:
        character.healthPotions++;
        snprintf(what, sizeof what, "%d of %d now", character.healthPotions, tower_mod_max_potions());
        shop_pay(i, what); return;
    case SHOP_MEND: {
        float h = character.health;
        character.health = fminf(character.maxHealth, h + MEND_HEAL * character.maxHealth);
        snprintf(what, sizeof what, "health %.0f -> %.0f of %.0f", (double)h, (double)character.health, (double)character.maxHealth);
        shop_pay(i, what); return; }
    default: {
        BoonId b = (BoonId)s.shop.card[i];
        snprintf(what, sizeof what, "%s, tier %d", BOONS[b].name, BOONS[b].tier);
        shop_pay(i, what); take_boon(b); return; }
    }
}

static void shop_update(bool left, bool right, bool up, bool down, bool confirm, bool back)
{
    int i = s.shop.sel;
    if (left) i = (i + SHOP_ITEMS - 1) % SHOP_ITEMS;
    else if (right) i = (i + 1) % SHOP_ITEMS;
    else if (down) i = i >= SHOP_POTION ? SHOP_LEAVE : i + 3;           // rows of three, then Leave
    else if (up) i = i == SHOP_LEAVE ? SHOP_MEND : i >= 3 ? i - 3 : i;
    s.shop.sel = (int8_t)i;
    if (back) shop_buy(SHOP_LEAVE);
    else if (confirm) shop_buy(i);
}

// ---- Events: one screen, 2-3 choices (the last Leave), then the result and Continue ----

static void event_open(void)
{
    int best = tower_best_floor(), pick[EV_COUNT], n = 0;
    uint8_t open = 0;
    for (int e = 0; e < EV_COUNT; e++) if (best >= EVENT_FLOOR[e]) open |= 1 << e;
    if (!(open & ~s.eventsSeen)) s.eventsSeen &= ~open;     // every one met this run: they come round again
    for (int e = 0; e < EV_COUNT; e++) if ((open & ~s.eventsSeen) >> e & 1) pick[n++] = e;
    s.event = (int8_t)pick[rng_next() % n];
#ifdef HLE_EVENT_FORCE      // test aid: every Event is this one (1-8)
    s.event = HLE_EVENT_FORCE - 1;
#endif
    s.eventsSeen |= 1 << s.event;
    s.eventPick = (int8_t)(rng_next() % PB_COUNT);         // the Brazier's blessing
    s.eventDone = false;
    list_open(SCR_EVENT);
    s.scrOpenMs = get_ticks_ms();
#ifdef HLE_AUTOPLAY
    debugf("[event] %s (best floor %d: open %02x, seen %02x), gold %d, health %.0f/%.0f, potions %d\n", EVENTS[s.event].name, best, open, s.eventsSeen,
           s.gold, (double)character.health, (double)character.maxHealth, character.healthPotions);
#endif
}

static const char *boon_lv(int b, char *buf, int n)     // "Riposte", "Rally Lv 2"
{
    if (BOONS[b].maxStacks > 1 && s.stacks[b] > 1) snprintf(buf, n, "%s Lv %d", BOONS[b].name, s.stacks[b]);
    else snprintf(buf, n, "%s", BOONS[b].name);
    return buf;
}

static void event_take(int i)
{
    int e = s.event, t[BOON_COUNT], k;
    char *r = s.eventResult, nm[40];
    const int rn = sizeof s.eventResult;
    if (i == EVENTS[e].choices - 1) snprintf(r, rn, "You leave it be.");
    else switch (e) {
    case EV_ALTAR: {
        float cut = 0.15f * character.maxHealth;
        int b = rare_boon(true);
        s.altarHp += cut;
        sync_max_health();
        if (character.health > character.maxHealth) character.health = character.maxHealth;
        take_boon((BoonId)b);
        snprintf(r, rn, "The altar drinks %.0f max health. It gives you %s.", (double)cut, boon_lv(b, nm, sizeof nm));
        break; }
    case EV_GRAVE:
        if (i == 0) snprintf(r, rn, "You take %d gold from the stone.", gain_gold(GRAVE_GOLD));
        else { float h = character.health; character.health = fminf(character.maxHealth, h + REST_HEAL * character.maxHealth);
               snprintf(r, rn, "You pray: +%.0f health.", (double)(character.health - h)); }
        break;
    case EV_MIRROR: {
        int b = t[rng_next() % mirror_targets(t)], c = rng_next() % PC_COUNT;
        take_boon((BoonId)b);
        add_curse(c);
        snprintf(r, rn, "%s. Its curse: %s (%s).", boon_lv(b, nm, sizeof nm), CURSES[c].name, CURSES[c].desc);
        break; }
    case EV_MERCHANT:
        if (i == 0) { s.gold -= MERCHANT_POTION; character.healthPotions++; snprintf(r, rn, "One potion for %d gold: %d held.", MERCHANT_POTION, character.healthPotions); }
        else { int b = rare_boon(true); character.healthPotions -= 2; take_boon((BoonId)b); snprintf(r, rn, "Two potions for %s.", boon_lv(b, nm, sizeof nm)); }
        break;
    case EV_POOL:
        k = pool_targets(i == 0, t);
        { int b = t[rng_next() % k];
          take_boon((BoonId)b);
          if (is_learn(b)) s.skillsOffered |= 1 << (b - BOON_LEARN_MOONBLADE);
          if (i == 0) snprintf(r, rn, "You learn %s.", BOONS[b].name);
          else snprintf(r, rn, "The water sharpens it: %s Lv %d.", BOONS[b].name, s.stacks[b]); }
        break;
    case EV_PRISONER:
        s.prisoner = true;
        snprintf(r, rn, "The chains fall. Your next Fight is an Elite, its draft all Legendary.");
        break;
    case EV_BONES:
        s.gold -= BONES_STAKE;
        if (rng_next() % 2) snprintf(r, rn, "The bones fall your way: +%d gold.", gain_gold(BONES_WIN));
        else snprintf(r, rn, "The bones fall against you: the %d gold is gone.", BONES_STAKE);
        break;
    case EV_BRAZIER:
        if (i == 0) { int em = BRAZIER_EMBERS * s.circle; bank_embers(em); snprintf(r, rn, "You bank %d Embers.", em); }
        else {
            s.halfBless[s.eventPick]++;
            if (s.eventPick == PB_VITALITY) { character.health += 6.0f; sync_max_health(); }
            snprintf(r, rn, "Half a blessing, without a curse: %s.", BLESSINGS[s.eventPick].name);
        }
        break;
    }
#ifdef HLE_AUTOPLAY
    debugf("[event] %s, choice %d: %s | gold %d, health %.0f/%.0f, potions %d, damage x%.2f, taken x%.2f, Embers %d\n", EVENTS[e].name, i + 1, r, s.gold,
           (double)character.health, (double)character.maxHealth, character.healthPotions, (double)tower_mod_damage_dealt(), (double)tower_mod_damage_taken(), s.runEmbers);
#endif
    s.eventDone = true;
    list_open(SCR_EVENT);
    s.scrOpenMs = get_ticks_ms();       // (the result is read before Continue takes a press)
}

// ---- Details (R): what a card, a node, a choice or a Shrine entry does in full, in the shared paged panel
// (utilities/ui_pages.h). Each builder fills a UiDoc; the panel pages it ----

static const char *const TIER_NAME[3] = { "Common", "Rare", "Legendary" };
static void event_label(int i, char *out, int n);

// a skill's level track and its two forms (current level and the form taken gold, a locked one grey)
static void doc_skill(UiDoc *d, int k)
{
    int lv = knows(k) ? s.stacks[BOON_LEARN_MOONBLADE + k] : 0, f = tower_skill_form(k);
    ui_doc_text(d, FONT_STYLE_GREY, "Button: %s. Its levels take no boon slot.", joypad_skill_button(k));
    ui_doc_gap(d);
    ui_doc_row(d, lv >= 1 ? FONT_STYLE_GOLD : FONT_STYLE_WHITE, lv == 1 && !f ? "now" : NULL, "Lv 1: %s", full_text(BOON_LEARN_MOONBLADE + k));
    for (int l = 2; l <= SKILL_LV_MAX; l++)
        ui_doc_row(d, lv >= l ? FONT_STYLE_GOLD : FONT_STYLE_WHITE, lv == l && !f ? "now" : NULL, "Lv %d: %s", l, SKILL_LV_DESC[k][l - 2]);
    ui_doc_gap(d);
    ui_doc_text(d, FONT_STYLE_GREY, "Lv 5: one of two forms (taking one locks the other out for the run)");
    for (int fm = SKILL_FORM_A; fm <= SKILL_FORM_B; fm++) {
        BoonId c = skill_form_card(k, fm);
        bool taken = s.stacks[c], locked = s.locked[c], bought = fm == SKILL_FORM_A || ((s.forms >> k) & 1);
        ui_doc_row(d, taken ? FONT_STYLE_GOLD : locked || !bought ? FONT_STYLE_GREY : FONT_STYLE_WHITE,
                   taken ? "taken" : locked ? "locked" : !bought ? "Shrine" : NULL, "%s: %s", BOONS[c].name, full_text(c));
    }
    if (!((s.forms >> k) & 1)) ui_doc_text(d, FONT_STYLE_GREY, "%s is bought at the Shrine (Unlocks).", BOONS[skill_form_card(k, SKILL_FORM_B)].name);
}

// a boon's, a skill card's, a "+" or a duo's page; its title and colour out
static void doc_boon(UiDoc *d, int b, char *title, int tn, int *tstyle)
{
    const BoonDef *bd = &BOONS[b];
    int tier = card_tier(b);
    char line[96];
    snprintf(title, tn, "%s", bd->name);
    *tstyle = NAME_STYLE[tier];
    if (bd->duo) snprintf(line, sizeof line, "Duo of %s + %s", FAMILIES[bd->family].name, FAMILIES[bd->duo - 1].name);
    else if (is_form(b)) snprintf(line, sizeof line, "Lv 5 form of %s", BOONS[BOON_LEARN_MOONBLADE + form_skill(b)].name);
    else if (is_skill_card(b)) snprintf(line, sizeof line, "Skill");
    else if (bd->up) snprintf(line, sizeof line, "%s: the \"+\" of %s", FAMILIES[bd->family].name, BOONS[bd->up - 1].name);
    else snprintf(line, sizeof line, "%s family", FAMILIES[bd->family].name);
    ui_doc_row(d, bd->duo ? FONT_STYLE_GOLD : FAMILIES[bd->family].style, TIER_NAME[tier], "%s", line);
    if (is_learn(b) || is_form(b)) {      // the whole track
        int k = is_learn(b) ? b - BOON_LEARN_MOONBLADE : form_skill(b);
        if (is_form(b)) { ui_doc_text(d, FONT_STYLE_WHITE, "%s", full_text(b)); ui_doc_text(d, FONT_STYLE_RED, "Locks out: %s", BOONS[rival_of(b)].name); ui_doc_gap(d); }
        else ui_doc_text(d, FONT_STYLE_WHITE, knows(k) ? "Known: Lv %d of 5 now." : "Not learned yet: this card teaches it at Lv 1.", tower_skill_level(k));
        doc_skill(d, k);
        return;
    }
    // its level now and its most
    if (bd->up) {
        BoonId u = (BoonId)(bd->up - 1);
        ui_doc_text(d, FONT_STYLE_GREY, s.stacks[b] ? "Held: %s's top level." : "Offered once %s is at Lv %d (it's at %d). No slot of its own.",
                    BOONS[u].name, BOONS[u].maxStacks, s.stacks[u]);
    } else if (bd->duo) {
        int sa, sb;
        if (s.stacks[b]) ui_doc_text(d, FONT_STYLE_GREY, "Held: merged into one slot with its two boons.");
        else if (duo_pair((BoonId)b, &sa, &sb)) ui_doc_text(d, FONT_STYLE_WHITE, "Merges %s and %s into one slot (both keep working at their max), and opens the other slot.",
                                                           BOONS[s.slot[sa] - 1].name, BOONS[s.slot[sb] - 1].name);
        else ui_doc_text(d, FONT_STYLE_GREY, "Offered with a maxed %s boon and a maxed %s boon slotted: it merges them into one slot (both keep working) and opens the other.",
                         FAMILIES[bd->family].name, FAMILIES[bd->duo - 1].name);
        for (int f = 0; f < 2; f++) { int fam = f ? bd->duo - 1 : bd->family;
            if (!family_open(fam)) ui_doc_text(d, FONT_STYLE_RED, "Needs the %s pack (the Shrine's Unlocks).", FAMILIES[fam].name); }
    } else if (s.stacks[b]) ui_doc_row(d, FONT_STYLE_WHITE, NULL, "Lv %d of %d now%s", s.stacks[b], bd->maxStacks, s.stacks[b] >= bd->maxStacks ? " (its max)" : "");
    else ui_doc_text(d, FONT_STYLE_WHITE, "Not held. Lv 1 of %d; a new boon takes a slot (%d of %d used).", bd->maxStacks, slots_used(), TOWER_SLOTS);
    ui_doc_gap(d);
    ui_doc_text(d, FONT_STYLE_WHITE, "%s", full_text(b));
    // rivals: what it locks out, what locks it
    if (rival_of(b) >= 0) {
        char rn[48]; rival_names(b, rn, sizeof rn);
        ui_doc_gap(d);
        ui_doc_text(d, FONT_STYLE_RED, "Rival: %s. Taking one locks the other out for the run.", rn);
        if (s.locked[b]) ui_doc_text(d, FONT_STYLE_RED, "Locked out this run: its rival was taken.");
    }
    if (takes_slot(b)) {
        // the "+" at its top level
        for (int p = 0; p < BOON_COUNT; p++) if (BOONS[p].up == b + 1) {
            ui_doc_gap(d);
            ui_doc_text(d, s.stacks[p] ? FONT_STYLE_GOLD : FONT_STYLE_BLUE, "At Lv %d, its \"+\"%s: %s. %s", bd->maxStacks, s.stacks[p] ? " (held)" : "", BOONS[p].name, full_text(p));
        }
        // the duos it feeds once maxed
        int m = 0; line[0] = 0;
        for (int du = 0; du < BOON_COUNT; du++) {
            if (!BOONS[du].duo) continue;
            int other = BOONS[du].family == bd->family ? BOONS[du].duo - 1 : BOONS[du].duo - 1 == bd->family ? BOONS[du].family : -1;
            if (other >= 0) m += snprintf(line + m, m < (int)sizeof line ? sizeof line - m : 0, "%s%s (%s)", m ? ", " : "", BOONS[du].name, FAMILIES[other].name);
        }
        if (m) { ui_doc_gap(d); ui_doc_text(d, FONT_STYLE_GOLD, "Duos, maxed beside a maxed boon of the family named: %s", line); }
    }
}

static void doc_pact(UiDoc *d, int card, char *title, int tn, int *tstyle)
{
    int b = (card - BOON_COUNT) / PC_COUNT, c = (card - BOON_COUNT) % PC_COUNT;
    snprintf(title, tn, "Pact: %s / %s", BLESSINGS[b].name, CURSES[c].name);
    *tstyle = FONT_STYLE_RED;
    ui_doc_text(d, FONT_STYLE_GREY, "A blessing and its curse, both for the run. Pacts take no slot and stack without end.");
    ui_doc_gap(d);
    ui_doc_row(d, FONT_STYLE_GOLD, NULL, "Blessing, %s: %s (held: %d)", BLESSINGS[b].name, BLESSINGS[b].desc, s.bless[b]);
    ui_doc_row(d, FONT_STYLE_RED, NULL, "Curse, %s: %s (held: %d)", CURSES[c].name, CURSES[c].desc, s.curse[c]);
}

// a draft's or a Shop's card (a BoonId, a pact, the purse)
static void doc_card(UiDoc *d, int card, char *title, int tn, int *tstyle)
{
    if (card == GOLD_CARD) {
        snprintf(title, tn, "Purse"); *tstyle = FONT_STYLE_GOLD;
        ui_doc_text(d, FONT_STYLE_WHITE, "+%d gold now (Prospector raises it). It comes once no boon is left for your build.", gold_with(GOLD_CARD_GOLD));
    } else if (card >= BOON_COUNT) doc_pact(d, card, title, tn, tstyle);
    else doc_boon(d, card, title, tn, tstyle);
}

static void details_open_doc(const char *title, int style) { ui_panel_open(ui_details(), title, style, false); ui_details()->owner = UI_OWNER_TOWER; }
static void details_card(int card)
{
    char t[64]; int st;
    UiDoc *d = ui_panel_doc(ui_details());
    ui_doc_clear(d);
    text_load();
    doc_card(d, card, t, sizeof t, &st);
    text_free();
    details_open_doc(t, st);
}

// the pause menu's Build page and the Shrine (menu_controller.c): the same panel, drawn by the menu
void tower_details_card(int card) { details_card(card); ui_details()->owner = UI_OWNER_MENU; }

static const char *night_line(Night n, char *out, int nn) { snprintf(out, nn, "%s: %s (Embers x%.1f)", NIGHTS[n].name, NIGHTS[n].desc, (double)NIGHTS[n].embers); return out; }

// a map node's page: its title ("Enter: ..." on the confirm screen), what waits there, what it gives
static void doc_node(UiDoc *d, int r, int l, bool enter, char *title, int tn, int *tstyle)
{
    int t = s.map.type[r][l];
    const char *boss = t == NODE_BOSS ? BOSS_NAMES[r ? TOWER_UNBOUND : TOWER_GUARDIAN] : s.map.boss[r][l] ? BOSS_NAMES[s.map.boss[r][l] - 1] : "";
    bool jailer = t == NODE_FIGHT && s.prisoner;
    const char *pre = "";       // (the confirm's own button says Enter: the longest Elite's name then fits one line)
    (void)enter;
    char tag[40], nl[96];
    affix_names(s.map.afx[r][l], tag, sizeof tag);
    int gold = gold_with((int)((GOLD_FIGHT + GOLD_PER_CIRCLE * s.circle) * (t == NODE_ELITE || jailer ? ELITE_GOLD : 1.0f) + 0.5f));
    *tstyle = t == NODE_ELITE || jailer ? FONT_STYLE_RED : FONT_STYLE_GOLD;
    switch (t) {
    case NODE_BOSS:
        snprintf(title, tn, "%s%s", pre, boss);
        ui_doc_text(d, FONT_STYLE_WHITE, r ? "The circle's last fight: beat it to clear the circle (+%d Embers) and climb on." : "The circle's first fight.", CIRCLE_EMBERS);
        ui_doc_text(d, FONT_STYLE_WHITE, "Floor %d. Reward: a draft, %d gold and Embers.", s.floor, gold);
        if (s.night) { ui_doc_gap(d); ui_doc_text(d, FONT_STYLE_RED, "This circle's night, %s", night_line(s.night, nl, sizeof nl)); }
        break;
    case NODE_FIGHT: case NODE_ELITE:
        if (t == NODE_ELITE || jailer) snprintf(title, tn, "%sElite %s%s%s", pre, tag, tag[0] ? " " : "", boss);
        else snprintf(title, tn, "%sFight: %s", pre, boss);
        if (jailer) ui_doc_text(d, FONT_STYLE_RED, "Floor %d. The Prisoner's jailer, x1.25 health.", s.floor);
        else if (t == NODE_ELITE) ui_doc_text(d, FONT_STYLE_RED, "Floor %d. An Elite, x1.25 health.", s.floor);
        else ui_doc_text(d, FONT_STYLE_WHITE, "Floor %d. A Bonus boss of this circle.", s.floor);
        if (jailer) ui_doc_text(d, FONT_STYLE_GOLD, "Reward: a Legendary draft, x1.5 gold (%d) and Embers.", gold);
        else if (t == NODE_ELITE) ui_doc_text(d, FONT_STYLE_GOLD, "Reward: a Rare+ draft%s, x1.5 gold (%d) and Embers.", __builtin_popcount(s.map.afx[r][l]) >= 2 ? " (a duo if ready)" : "", gold);
        else ui_doc_text(d, FONT_STYLE_GOLD, "Reward: a draft, %d gold and Embers.", gold);
        for (int a = 0; a < AFX_COUNT; a++) if ((s.map.afx[r][l] >> a) & 1) ui_doc_text(d, FONT_STYLE_WHITE, "%s: %s", AFFIXES[a].name, full_text(TXT_AFFIX + a));
        break;
    case NODE_REST:
        snprintf(title, tn, "%sRest", pre);
        ui_doc_text(d, FONT_STYLE_WHITE, "Heal 40%% of max health, Temper a level (a boon, its \"+\" or a skill), or +2 potions; then Release a boon if you wish, free.");
        ui_doc_text(d, FONT_STYLE_GREY, "No fight. Health %.0f/%.0f, potions %d.", (double)character.health, (double)character.maxHealth, character.healthPotions);
        break;
    case NODE_SHOP:
        snprintf(title, tn, "%sShop", pre);
        ui_doc_text(d, FONT_STYLE_WHITE, "Three new boons or duos, a level, a \"+\", a pact, potions, Mend (heal 35%%) and Release, for gold.");
        ui_doc_text(d, FONT_STYLE_GOLD, "Gold %d. No fight.", s.gold);
        break;
    case NODE_EVENT:
        snprintf(title, tn, "%sEvent", pre);
        ui_doc_text(d, FONT_STYLE_WHITE, "Something waits on the way: a choice, for good or ill. You may always walk on. No fight, no draft.");
        break;
    }
}

// an Event's choice in full
static void doc_event_choice(UiDoc *d, int i)
{
    int e = s.event;
    if (i == EVENTS[e].choices - 1) { ui_doc_text(d, FONT_STYLE_WHITE, "Walk on: nothing gained, nothing lost."); return; }
    const char *rare = "a random Rare boon: a new one, or a level of one held (never a skill card, a \"+\" or a duo)";
    switch (e) {
    case EV_ALTAR:    ui_doc_text(d, FONT_STYLE_WHITE, "Give 15%% of your max health (%.0f) for the rest of the climb, for %s.", (double)(0.15f * character.maxHealth), rare); break;
    case EV_GRAVE:    { if (i == 0) ui_doc_text(d, FONT_STYLE_WHITE, "Take %d gold from the stone.", gold_with(GRAVE_GOLD));
                      else ui_doc_text(d, FONT_STYLE_WHITE, "Pray: heal 40%% of max health (+%.0f now).", (double)fminf(REST_HEAL * character.maxHealth, character.maxHealth - character.health)); } break;
    case EV_MIRROR:   ui_doc_text(d, FONT_STYLE_WHITE, "A random slotted boon gains a level (its \"+\" at its max; never a duo), and a random curse is laid on you:");
                      { for (int c = 0; c < PC_COUNT; c++) ui_doc_text(d, FONT_STYLE_RED, "%s: %s", CURSES[c].name, CURSES[c].desc); } break;
    case EV_MERCHANT: { if (i == 0) ui_doc_text(d, FONT_STYLE_WHITE, "One potion for %d gold, past the run's most potions, up to %d.", MERCHANT_POTION, POTION_CAP);
                      else ui_doc_text(d, FONT_STYLE_WHITE, "Give 2 potions for %s.", rare); } break;
    case EV_POOL:     ui_doc_text(d, FONT_STYLE_WHITE, i == 0 ? "Learn a skill you don't know (at random), at Lv 1." : "A skill you know (at random) gains a level, up to Lv 4."); break;
    case EV_PRISONER: ui_doc_text(d, FONT_STYLE_WHITE, "Your next Fight node becomes an Elite: x1.25 health, x1.5 gold and Embers, and an all-Legendary draft."); break;
    case EV_BONES:    ui_doc_text(d, FONT_STYLE_WHITE, "Pay %d gold. Half the time %d come back.", BONES_STAKE, gold_with(BONES_WIN)); break;
    case EV_BRAZIER:  { if (i == 0) ui_doc_text(d, FONT_STYLE_WHITE, "Bank %d Embers now, for the Shrine.", BRAZIER_EMBERS * s.circle);
                      else ui_doc_text(d, FONT_STYLE_WHITE, "Half a pact's blessing, with no curse: %s (%s at a full stack).", BLESSINGS[s.eventPick].name, BLESSINGS[s.eventPick].desc); } break;
    }
}

// a boon list's rows (Temper, the Shop's level and "+"): each with its level now
static void doc_targets(UiDoc *d, const int8_t *t, int n)
{
    if (!n) { ui_doc_text(d, FONT_STYLE_GREY, "Nothing to raise now."); return; }
    for (int i = 0; i < n; i++) ui_doc_row(d, is_skill_card(t[i]) ? FONT_STYLE_WHITE : FAMILIES[BOONS[t[i]].family].style, BOONS[t[i]].up ? "top" : NULL, "%s%s", BOONS[t[i]].name,
                                           BOONS[t[i]].up ? "" : (s.stacks[t[i]] ? "  (a level)" : ""));
}

// R on the screen under it: the card, item, node or row under the cursor; false if it has none
static bool details_cursor_impl(bool open);
static bool details_for_cursor(bool open)       // (the texts read from ROM while its page is built)
{
    if (!open) return details_cursor_impl(false);
    text_load();
    bool any = details_cursor_impl(true);
    text_free();
    return any;
}
static bool details_cursor_impl(bool open)
{
    UiDoc *d = open ? ui_panel_doc(ui_details()) : NULL;     // (a look at whether there are any: nothing allocated)
    char t[64] = ""; int st = FONT_STYLE_GOLD;
    int8_t tg[LIST_MAX];
    if (open) ui_doc_clear(d);
    if (s.draftOpen) {
        if (s.sel >= s.nChoices) return false;
        if (open) doc_card(d, s.choices[s.sel], t, sizeof t, &st);
    } else if (s.scr == SCR_MAP) {
        if (s.map.row + 1 >= MAP_ROWS) return false;
        if (open) doc_node(d, s.map.row + 1, s.map.sel, false, t, sizeof t, &st);
    } else if (s.scr == SCR_SHOP) {
        int i = s.shop.sel;
        if (i == SHOP_LEAVE || (i <= SHOP_CARD2 && s.shop.card[i] < 0)) return false;
        if (!open) return true;
        if (i <= SHOP_CARD2) doc_card(d, s.shop.card[i], t, sizeof t, &st);
        else if (i == SHOP_PACT) doc_pact(d, s.shop.pact, t, sizeof t, &st);
        else {
            static const char *const NM[SHOP_ITEMS] = { [SHOP_LEVEL] = "A level", [SHOP_PLUS] = "A \"+\"", [SHOP_POTION] = "Potion", [SHOP_MEND] = "Mend", [SHOP_RELEASE] = "Release" };
            snprintf(t, sizeof t, "%s: %d gold", NM[i], shop_price(i));
            if (i == SHOP_LEVEL) { ui_doc_text(d, FONT_STYLE_WHITE, "+1 level on a slotted boon below its max (a duo has none). You choose it from:"); doc_targets(d, tg, level_targets(tg, LIST_MAX)); }
            else if (i == SHOP_PLUS) { ui_doc_text(d, FONT_STYLE_WHITE, "A maxed boon's \"+\", its top level. You choose it from:"); doc_targets(d, tg, plus_targets(tg, LIST_MAX)); }
            else if (i == SHOP_POTION) ui_doc_text(d, FONT_STYLE_WHITE, "+1 potion, up to the run's most (%d). Each fight won still gives one, past it, up to %d.", tower_mod_max_potions(), POTION_CAP);
            else if (i == SHOP_MEND) ui_doc_text(d, FONT_STYLE_WHITE, "Heal 35%% of max health: +%.0f now. Once a visit.", (double)fminf(MEND_HEAL * character.maxHealth, character.maxHealth - character.health));
            else ui_doc_text(d, FONT_STYLE_WHITE, "Free a slot: you choose a boon; it goes with its levels and its \"+\" (a duo with its two boons). A rival it locked out stays locked.");
            const char *why = shop_block(i);
            if (why[0]) ui_doc_text(d, FONT_STYLE_RED, "%s.", why);
        }
    } else if (s.scr == SCR_EVENT) {
        if (s.eventDone) return false;
        if (open) { event_label(s.list[s.listSel], t, sizeof t); doc_event_choice(d, s.list[s.listSel]); }
    } else if (s.scr == SCR_REST) {
        if (!open) return true;
        int v = s.list[s.listSel];
        if (v == REST_HEAL_ROW) { snprintf(t, sizeof t, "Heal 40%%"); ui_doc_text(d, FONT_STYLE_WHITE, "Heal 40%% of max health: +%.0f now (%.0f/%.0f).", (double)fminf(REST_HEAL * character.maxHealth, character.maxHealth - character.health), (double)character.health, (double)character.maxHealth); }
        else if (v == REST_TEMPER_ROW) { snprintf(t, sizeof t, "Temper"); ui_doc_text(d, FONT_STYLE_WHITE, "+1 level on a slotted boon below its max, a maxed boon's \"+\", or a known skill's next level below Lv 4. Never a new boon, a duo or a skill's form. You choose from:"); doc_targets(d, tg, temper_targets(tg, LIST_MAX)); }
        else { snprintf(t, sizeof t, "+2 potions"); ui_doc_text(d, FONT_STYLE_WHITE, "+2 potions (%d now), up to %d: past the run's most, as a fight's.", character.healthPotions, POTION_CAP); }
        ui_doc_gap(d); ui_doc_text(d, FONT_STYLE_GREY, "After it, Release a boon if you wish (free).");
    } else if (s.scr == SCR_TEMPER || s.scr == SCR_SHOP_LEVEL || s.scr == SCR_SHOP_PLUS || s.scr == SCR_RELEASE || s.scr == SCR_SHOP_RELEASE) {
        int v = s.list[s.listSel];
        if (v < 0) return false;
        int b = s.scr == SCR_RELEASE || s.scr == SCR_SHOP_RELEASE ? s.slot[v] - 1 : v;
        if (b < 0) return false;
        if (open) doc_boon(d, b, t, sizeof t, &st);
    } else return false;
    if (open) details_open_doc(t, st);
    return true;
}

// After a map node is entered (confirmed): a Rest's, a Shop's or an Event's screen, or true for a fight
static bool map_take(void)
{
    int t = map_enter();
    if (t == NODE_REST) { list_open(SCR_REST); s.scrOpenMs = get_ticks_ms() - DRAFT_INPUT_DELAY_MS; return false; }    // (a press, not a fight's: no wait)
    if (t == NODE_SHOP) { shop_open(); s.scrOpenMs -= DRAFT_INPUT_DELAY_MS; return false; }
    if (t == NODE_EVENT) { event_open(); s.scrOpenMs -= DRAFT_INPUT_DELAY_MS; return false; }
    s.scr = SCR_NONE;
    icons_free();
    return true;
}
// A Rest left with B before a choice: back on the map, its node still to choose (nothing was rolled)
static void map_unenter(void)
{
    int r = s.map.row;
    s.map.sel = s.map.path[r];
    s.map.path[r] = -1;
    s.map.row--;
    s.scr = SCR_MAP;
#ifdef HLE_AUTOPLAY
    debugf("[map] back from the Rest: row %d lane %d to choose again\n", r + 1, s.map.sel);
#endif
}

static bool s_pauseReq;
bool tower_take_pause_request(void) { bool r = s_pauseReq; s_pauseReq = false; return r; }

// B goes back a level everywhere and never enters anything: the map's B opens the pause menu (the screen
// behind it), the confirm screen's B returns to the map, a Shop's B leaves it (its Leave), an Event's is
// its Leave (or Continue), a Rest's first screen returns to the map with the Rest still to choose, Temper
// back to the Rest, Release keeps every boon, a Shop's list back to its shelf. The draft ignores B (its
// cards come up under a fight's mashed attack: B is the sword in the Classic layout). R: Details
bool tower_between_update(bool left, bool right, bool up, bool down, bool confirm, bool back, bool details)
{
    UiPanel *dp = ui_details();
    if (dp->open && !dp->confirm) {     // Details over the screen (a milestone's notice over the draft waits out its delay: a fight's mashed press)
        if (!(s.draftOpen && get_ticks_ms() - s.draftOpenMs < DRAFT_INPUT_DELAY_MS)) ui_panel_input(dp, up, down, left, right, confirm, back, details);
        return false;
    }
    if (s.draftOpen) {
        if (details && get_ticks_ms() - s.draftOpenMs >= DRAFT_INPUT_DELAY_MS) { details_for_cursor(true); return false; }
        if (tower_draft_update(left, right, up, down, confirm)) { tower_floor_restore(); tower_next_floor(); tower_map_open(); }
        return false;
    }
    if (!s.scr || get_ticks_ms() - s.scrOpenMs < DRAFT_INPUT_DELAY_MS) return false;    // (as the draft: no press taken unseen)
    if (s.scr == SCR_CONFIRM) {     // the node's page: A (Enter) enters, B (or Back) returns to the map
        int r = ui_panel_input(dp, up, down, left, right, confirm, back, false);
        if (r == UI_PANEL_CLOSED) { s.scr = SCR_MAP; return false; }
        if (r != UI_PANEL_ENTER) return false;
        s.scr = SCR_MAP;
        return map_take();
    }
    if (s.scr == SCR_MAP) {     // pick, then confirm: Left/Right along the next row's reachable nodes, A opens its page; Up/Down nothing
        int d = right - left, r = s.map.row + 1;
        for (int l = s.map.sel + d; d && l >= 0 && l < MAP_LANES; l += d) if (map_reachable(r, l)) { s.map.sel = (int8_t)l; break; }
        if (details) { details_for_cursor(true); return false; }
        if (back) { s_pauseReq = true; return false; }
        if (!confirm || r >= MAP_ROWS) return false;
        char t[64]; int st;
        ui_doc_clear(ui_panel_doc(dp));
        text_load();
        doc_node(dp->doc, r, s.map.sel, true, t, sizeof t, &st);
        text_free();
        ui_panel_open(dp, t, st, true);
        dp->owner = UI_OWNER_TOWER;
        s.scr = SCR_CONFIRM;
#ifdef HLE_AUTOPLAY
        debugf("[map] confirm: %s (%d page%s)\n", t, dp->pg.pages, dp->pg.pages > 1 ? "s: LONG" : "");
#endif
        return false;
    }
    if (details) { details_for_cursor(true); return false; }
    if (s.scr == SCR_SHOP) { shop_update(left, right, up, down, confirm, back); return false; }
    bool shopList = s.scr >= SCR_SHOP_LEVEL && s.scr <= SCR_SHOP_RELEASE;
    if (back) {     // a level back
        if (shopList) s.scr = SCR_SHOP;                                     // to the shelf
        else if (s.scr == SCR_REST) map_unenter();                          // to the map, the Rest still to choose
        else if (s.scr == SCR_TEMPER) list_open(SCR_REST);                  // to the Rest's choices
        else if (s.scr == SCR_RELEASE) {                                    // keep them all
#ifdef HLE_AUTOPLAY
            debugf("[rest] release: none (B)\n");
#endif
            tower_map_open();
        } else if (s.scr == SCR_EVENT) {                                    // Leave, or Continue
#ifdef HLE_AUTOPLAY
            debugf("[event] B: %s\n", s.eventDone ? "Continue" : "Leave");
#endif
            tower_map_open();
        }
        return false;
    }
    // a list (a Rest's screens, the Shop's, an Event's): Up/Down along the rows (past any that can't be chosen), A takes one
    int d = down - up;
    for (int i = s.listSel + d; d && i >= 0 && i < s.listN; i += d) if (row_ok(i)) { s.listSel = (int8_t)i; break; }
    if (!confirm || !row_ok(s.listSel)) return false;
    int v = s.list[s.listSel];
    if (s.scr == SCR_EVENT) {
        if (s.eventDone) tower_map_open();
        else event_take(v);
    } else if (shopList) {
        if (v >= 0) {
            char nm[40];
            if (s.scr == SCR_SHOP_RELEASE) {
                snprintf(nm, sizeof nm, "%s", BOONS[s.slot[v] - 1].name);
                shop_pay(SHOP_RELEASE, nm); release_slot(v);
            } else {
                snprintf(nm, sizeof nm, "%s Lv %d -> %d", BOONS[v].name, s.stacks[v], s.stacks[v] + 1);
                shop_pay(s.scr == SCR_SHOP_LEVEL ? SHOP_LEVEL : SHOP_PLUS, nm); take_boon((BoonId)v);
            }
        }
        s.scr = SCR_SHOP;
    } else if (s.scr == SCR_REST) {
        if (v == REST_TEMPER_ROW) list_open(SCR_TEMPER);
        else { rest_take(v); release_open(); }
    } else if (s.scr == SCR_TEMPER) {
        if (v < 0) list_open(SCR_REST);
        else {
#ifdef HLE_AUTOPLAY
            debugf("[rest] temper: %s Lv %d -> %d\n", BOONS[v].name, s.stacks[v], s.stacks[v] + 1);
#endif
            take_boon((BoonId)v);
            rest_offering();
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
// the rows fit between the two top lines and the two bottom ones, inside the safe area (22 px apart at the default)
static int map_top(void) { return SAFE_Y0 + 10; }       // the first top line's baseline
static int map_bot(void) { return SAFE_Y1 - 2; }        // the last bottom line's
static int map_pitch(void) { int p = (map_bot() - 32 - (map_top() + 25)) / 6; return p > 26 ? 26 : p; }     // (row 7's icon under the night's line)
#define MAP_Y(r) (map_bot() - 12 - 20 - map_pitch() * (r))
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
    // the top lines: floor, Ascension, gold; the night and its effect. The bottom: the node under the cursor, the buttons
    char line[96];
    int n = snprintf(line, sizeof line, "Floor %d", s.floor);
    if (s.circle > 1) n += snprintf(line + n, sizeof line - n, "   Ascension %d", s.circle - 1);
    snprintf(line + n, sizeof line - n, "   Gold %d", s.gold);
    text_style(0, map_top(), SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_GOLD, line);
    if (s.night) { snprintf(line, sizeof line, "%s: %s", NIGHTS[s.night].name, NIGHTS[s.night].brief); text_style(0, map_top() + 13, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_RED, line); }
    if (cr < MAP_ROWS) {
        int t = s.map.type[cr][cl];
        const char *boss = t == NODE_BOSS ? BOSS_NAMES[cr ? TOWER_UNBOUND : TOWER_GUARDIAN] : s.map.boss[cr][cl] ? BOSS_NAMES[s.map.boss[cr][cl] - 1] : "";
        bool jailer = t == NODE_FIGHT && s.prisoner;      // the Chained Prisoner's: the next Fight is an Elite
        char tag[40];
        affix_names(s.map.afx[cr][cl], tag, sizeof tag);
        if (t == NODE_ELITE || jailer) snprintf(line, sizeof line, "Elite: %s%s%s", tag, tag[0] ? " " : "", boss);
        else if (t == NODE_FIGHT) snprintf(line, sizeof line, "Fight: %s", boss);
        else if (t == NODE_REST) snprintf(line, sizeof line, "Rest: heal, Temper or potions");
        else if (t == NODE_SHOP) snprintf(line, sizeof line, "Shop: boons, levels, potions, Mend");
        else if (t == NODE_EVENT) snprintf(line, sizeof line, "Event: a choice on the way");
        else snprintf(line, sizeof line, "%s", boss);
        text_style(0, map_bot() - 12, SCREEN_WIDTH, ALIGN_CENTER, t == NODE_ELITE || jailer ? FONT_STYLE_RED : FONT_STYLE_WHITE, line);
        text_style(0, map_bot(), SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_GREY, "A Choose    R Details    B Menu");
    }
}

static void rest_draw(void)
{
    flat_mode();
    rdpq_set_prim_color(RGBA32(0, 0, 0, 205));     // (dark enough to read over the hall's light)
    rdpq_fill_rectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    static const char *const HEAD[] = { [SCR_REST] = "Rest: choose one", [SCR_TEMPER] = "Temper: +1 level", [SCR_RELEASE] = "Release a boon? Free, with its levels",
                                        [SCR_SHOP_LEVEL] = "A level: 70 gold", [SCR_SHOP_PLUS] = "A \"+\": 150 gold", [SCR_SHOP_RELEASE] = "Release a boon: 20 gold, with its levels" };
    const int T = fit_my(66 + 15 * s.listN + 16);      // (the rows under three lines, and the Rest's last line)
    text_style(0, T + 22, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_GOLD, HEAD[s.scr]);
    static const char *const BACK[] = { [SCR_REST] = "B Map", [SCR_TEMPER] = "B Back", [SCR_RELEASE] = "B Keep all", [SCR_SHOP_LEVEL] = "B Back", [SCR_SHOP_PLUS] = "B Back", [SCR_SHOP_RELEASE] = "B Back" };
    text_style(SAFE_X0, T + 10, 100, ALIGN_LEFT, FONT_STYLE_GREY, BACK[s.scr]);
    if (details_for_cursor(false)) ui_hint(SAFE_X1, T + 10, "R Details");
    char line[80];
    bool shopList = s.scr >= SCR_SHOP_LEVEL, boonRows = s.scr == SCR_TEMPER || s.scr == SCR_SHOP_LEVEL || s.scr == SCR_SHOP_PLUS;
    if (shopList) snprintf(line, sizeof line, "Gold %d", s.gold);
    else snprintf(line, sizeof line, "Health %.0f/%.0f   Potions %d", (double)character.health, (double)character.maxHealth, character.healthPotions);
    text_style(0, T + 38, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_GREY, line);
    const int x = 50, w = SCREEN_WIDTH - 100, y0 = T + 66, pitch = 15;
    for (int i = 0; i < s.listN; i++) {
        int v = s.list[i], y = y0 + pitch * i, style = row_ok(i) ? FONT_STYLE_WHITE : FONT_STYLE_GREY;
        if (i == s.listSel) select_box(x - 6, y - 11, x + w + 6, y + 3);
        if (s.scr == SCR_REST) {
            int p = character.healthPotions, pm = POTION_CAP;
            if (v == REST_HEAL_ROW) snprintf(line, sizeof line, "Heal 40%%: +%.0f health", (double)fminf(REST_HEAL * character.maxHealth, character.maxHealth - character.health));
            else if (v == REST_TEMPER_ROW) snprintf(line, sizeof line, row_ok(i) ? "Temper: +1 level on a boon or skill" : "Temper: nothing to raise");
            else if (p < pm) snprintf(line, sizeof line, "+2 potions (%d -> %d)", p, p + REST_POTIONS < pm ? p + REST_POTIONS : pm);
            else snprintf(line, sizeof line, "+2 potions: full");
        } else if (v < 0) snprintf(line, sizeof line, s.scr == SCR_RELEASE ? "Keep them all" : "Back");
        else if (boonRows) {
            const BoonDef *bd = &BOONS[v];
            if (bd->up) snprintf(line, sizeof line, "%s (top level)", bd->name);
            else snprintf(line, sizeof line, "%s   Lv %d -> %d", bd->name, s.stacks[v], s.stacks[v] + 1);
            style = is_skill_card((BoonId)v) ? FONT_STYLE_WHITE : FAMILIES[bd->family].style;     // (a skill's grey would read as unavailable)
        } else {
            BoonId b = (BoonId)(s.slot[v] - 1);
            if (s.merged[v][0]) snprintf(line, sizeof line, "%s   Duo, with its two", BOONS[b].name);     // (their names: R)
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
int tower_shop_sel(void) { return s.shop.sel; }
int tower_event_id(void) { return s.event; }
bool tower_event_done(void) { return s.eventDone; }
void tower_test_set_gold(int g) { s.gold = g; }
#endif

// a framed button (the draft's Skip)
static void button(int x, int y, int w, int h, bool on, bool ok, const char *label)
{
    flat_mode();
    rdpq_set_prim_color(on ? RGBA32(255, 215, 90, 255) : RGBA32(110, 100, 90, 255));
    rdpq_fill_rectangle(x - 2, y - 2, x + w + 2, y + h + 2);
    rdpq_set_prim_color(on ? RGBA32(60, 30, 20, 240) : RGBA32(30, 22, 18, 230));
    rdpq_fill_rectangle(x, y, x + w, y + h);
    text_style(x, y + 12, w, ALIGN_CENTER, !ok ? FONT_STYLE_GREY : on ? FONT_STYLE_GOLD : FONT_STYLE_WHITE, label);
}

static void shop_draw(void)
{
    flat_mode();
    rdpq_set_prim_color(RGBA32(0, 0, 0, 205));     // (dark enough to read over the hall's light)
    rdpq_fill_rectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    char line[96];
    const int T = fit_my(204);      // (the header, the shelf, Leave and the line under it: 204 px)
    snprintf(line, sizeof line, "Shop   Gold %d", s.gold);
    text_style(0, T + 10, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_GOLD, line);
    text_style(SAFE_X0, T + 10, 100, ALIGN_LEFT, FONT_STYLE_GREY, "B Leave");
    if (details_for_cursor(false)) ui_hint(SAFE_X1, T + 10, "R Details");
    snprintf(line, sizeof line, "Health %.0f/%.0f   Potions %d/%d", (double)character.health, (double)character.maxHealth, character.healthPotions, tower_mod_max_potions());
    text_style(0, T + 22, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_GREY, line);
    // the three boon cards, as the draft's (shorter: the price at the foot)
    const int cw = 96, ch = 100, gap = 6, y0 = T + 28, x0 = (SCREEN_WIDTH - (3 * cw + 2 * gap)) / 2;     // (room for 3 lines of text over the price)
    for (int i = 0; i < 3; i++) {
        int x = x0 + i * (cw + gap), card = s.shop.card[i];
        bool on = i == s.shop.sel, sold = (s.shop.sold >> i) & 1;
        flat_mode();
        rdpq_set_prim_color(FRAME[card >= 0 ? BOONS[card].tier : 0][on]);
        rdpq_fill_rectangle(x - 2, y0 - 2, x + cw + 2, y0 + ch + 2);
        rdpq_set_prim_color(on ? RGBA32(60, 30, 20, 240) : RGBA32(30, 22, 18, 230));
        rdpq_fill_rectangle(x, y0, x + cw, y0 + ch);
        if (card < 0) { text_style(x + 4, y0 + 46, cw - 8, ALIGN_CENTER, FONT_STYLE_GREY, "No new boon fits"); continue; }
        const BoonDef *bd = &BOONS[card];
        if (bd->duo) snprintf(line, sizeof line, "%s+%s", FAMILIES[bd->family].name, FAMILIES[bd->duo - 1].name);
        text_style(x + 2, y0 + 12, cw - 4, ALIGN_CENTER, bd->duo ? FONT_STYLE_GOLD : FAMILIES[bd->family].style, bd->duo ? line : FAMILIES[bd->family].name);
        int nl = text_style(x + 4, y0 + 27, cw - 8, ALIGN_CENTER, sold ? FONT_STYLE_GREY : NAME_STYLE[bd->tier], bd->name);
        text_style(x + 6, y0 + 31 + 17 * (nl > 1 ? nl : 1), cw - 12, ALIGN_CENTER, sold ? FONT_STYLE_GREY : FONT_STYLE_WHITE, bd->desc);
        if (sold) snprintf(line, sizeof line, "Sold");
        else snprintf(line, sizeof line, "%d gold", shop_price(i));
        text_style(x, y0 + ch - 3, cw, ALIGN_CENTER, !shop_block(i)[0] ? FONT_STYLE_GOLD : FONT_STYLE_GREY, line);
    }
    // the services, two rows of three under the cards, then Leave
    for (int i = SHOP_LEVEL; i <= SHOP_RELEASE; i++) {
        static const char *const LABEL[SHOP_ITEMS] = { [SHOP_LEVEL] = "A level", [SHOP_PLUS] = "A \"+\"", [SHOP_PACT] = "Pact", [SHOP_POTION] = "Potion",
                                                       [SHOP_MEND] = "Mend", [SHOP_RELEASE] = "Release" };
        int k = i - SHOP_LEVEL, x = x0 + (k % 3) * (cw + gap), y = y0 + ch + 7 + 19 * (k / 3);
        if ((s.shop.sold >> i) & 1) snprintf(line, sizeof line, "%s: sold", LABEL[i]);
        else if (shop_price(i)) snprintf(line, sizeof line, "%s  %d", LABEL[i], shop_price(i));
        else snprintf(line, sizeof line, "%s  free", LABEL[i]);
        button(x, y, cw, 15, s.shop.sel == i, !shop_block(i)[0], line);
    }
    const int bw = 132, by = y0 + ch + 7 + 38;
    button((SCREEN_WIDTH - bw) / 2, by, bw, 15, s.shop.sel == SHOP_LEAVE, true, "Leave");
    // what the selected one does, and why it can't be bought
    int i = s.shop.sel, b = (s.shop.pact - BOON_COUNT) / PC_COUNT, c = (s.shop.pact - BOON_COUNT) % PC_COUNT;
    line[0] = 0;
    if (i <= SHOP_CARD2 && s.shop.card[i] >= 0) {
        int card = s.shop.card[i], sa, sb, r = rival_of(card);
        if (BOONS[card].duo && duo_pair(card, &sa, &sb)) snprintf(line, sizeof line, "Duo: merges %s + %s", BOONS[s.slot[sa] - 1].name, BOONS[s.slot[sb] - 1].name);
        else if (r >= 0) { char rn[40]; rival_names(card, rn, sizeof rn); snprintf(line, sizeof line, "New: slot %d/%d. Locks out %s", slots_used() + 1, TOWER_SLOTS, rn); }
        else snprintf(line, sizeof line, "New: slot %d/%d", slots_used() < TOWER_SLOTS ? slots_used() + 1 : TOWER_SLOTS, TOWER_SLOTS);
    } else if (i == SHOP_LEVEL) snprintf(line, sizeof line, "+1 level on a slotted boon (choose it)");
    else if (i == SHOP_PLUS) snprintf(line, sizeof line, "A maxed boon's \"+\", its top level (choose it)");
    else if (i == SHOP_PACT) line[0] = 0;     // (below, with its curse)
    else if (i == SHOP_POTION) snprintf(line, sizeof line, "+1 potion, up to the run's %d", tower_mod_max_potions());
    else if (i == SHOP_MEND) snprintf(line, sizeof line, "Heal 35%%: +%.0f health", (double)fminf(MEND_HEAL * character.maxHealth, character.maxHealth - character.health));
    else if (i == SHOP_RELEASE) snprintf(line, sizeof line, "Free a slot: a boon and its levels are gone");
    else snprintf(line, sizeof line, "Back to the map");
    // one line under the shelf: why it can't be bought (red), else what it does (R: all of it)
    const char *why = shop_block(i);
    if (!why[0] && i == SHOP_PACT) snprintf(line, sizeof line, "Blessing %s, its curse %s", BLESSINGS[b].name, CURSES[c].name);
    text_style(TITLE_SAFE_MARGIN_X, by + 15 + 12, SCREEN_WIDTH - 2 * TITLE_SAFE_MARGIN_X, ALIGN_CENTER, why[0] ? FONT_STYLE_RED : FONT_STYLE_WHITE, why[0] ? why : line);     // (one line under Leave: as wide as the shelf, which keeps the 7.5% area)
}

static void event_label(int i, char *out, int n)
{
    int e = s.event;
    if (s.eventDone) { snprintf(out, n, "Continue"); return; }
    if (i == EVENTS[e].choices - 1) { snprintf(out, n, "Leave"); return; }
    switch (e) {
    case EV_ALTAR:    snprintf(out, n, "Give %.0f max health: a Rare boon", (double)(0.15f * character.maxHealth)); break;
    case EV_GRAVE:
        if (i == 0) snprintf(out, n, "Take the %d gold", gold_with(GRAVE_GOLD));
        else snprintf(out, n, "Pray: heal 40%% (+%.0f)", (double)fminf(REST_HEAL * character.maxHealth, character.maxHealth - character.health));
        break;
    case EV_MIRROR:   snprintf(out, n, "A boon +1 level, and a curse"); break;
    case EV_MERCHANT: if (i == 0) snprintf(out, n, "1 potion for %d gold", MERCHANT_POTION); else snprintf(out, n, "2 potions for a Rare boon"); break;
    case EV_POOL:     snprintf(out, n, i == 0 ? "Learn a skill you don't know" : "A skill you know: +1 level"); break;
    case EV_PRISONER: snprintf(out, n, "Free it: your next Fight is an Elite"); break;
    case EV_BONES:    snprintf(out, n, "Pay %d gold: %d back, half the time", BONES_STAKE, gold_with(BONES_WIN)); break;
    case EV_BRAZIER:
        if (i == 0) snprintf(out, n, "Bank %d Embers", BRAZIER_EMBERS * s.circle);
        else snprintf(out, n, "Half a blessing: %s", BLESSINGS[s.eventPick].name);
        break;
    }
}

static void event_draw(void)
{
    flat_mode();
    rdpq_set_prim_color(RGBA32(0, 0, 0, 205));     // (dark enough to read over the hall's light)
    rdpq_fill_rectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    const int lh = 16, tw = SCREEN_WIDTH - 72;      // the font's line pitch, the text's width
    char line[96];
    if (s.event == EV_BRAZIER && !s.eventDone)     // the blessing's own words (a full pact's stack)
        snprintf(line, sizeof line, "%s: %s at a full stack", BLESSINGS[s.eventPick].name, BLESSINGS[s.eventPick].desc);
    // its height (the text, the blessing's line or the result, the rows, the bottom line), so it moves in only as far as it fits
    int h = 42 + lh * ui_text_lines(EVENTS[s.event].text, tw) + 12 + 15 * s.listN + 6;
    if (s.event == EV_BRAZIER && !s.eventDone) h += lh * ui_text_lines(line, tw);
    if (s.eventDone) h += lh * ui_text_lines(s.eventResult, tw) + 8;
    const int T = fit_my(h);
    text_style(0, T + 24, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_GOLD, EVENTS[s.event].name);
    text_style(SAFE_X0, T + 10, 100, ALIGN_LEFT, FONT_STYLE_GREY, s.eventDone ? "B Continue" : "B Leave");
    if (details_for_cursor(false)) ui_hint(SAFE_X1, T + 10, "R Details");
    int y = T + 42 + lh * text_style(36, T + 42, tw, ALIGN_CENTER, FONT_STYLE_WHITE, EVENTS[s.event].text);
    if (s.event == EV_BRAZIER && !s.eventDone) {
        y += lh * text_style(36, y, SCREEN_WIDTH - 72, ALIGN_CENTER, FONT_STYLE_GREY, line);
    }
    if (s.eventDone) y += lh * text_style(36, y + 4, SCREEN_WIDTH - 72, ALIGN_CENTER, FONT_STYLE_GOLD, s.eventResult) + 4;
    const int x = 50, w = SCREEN_WIDTH - 100, pitch = 15;
    y += 12;
    for (int i = 0; i < s.listN; i++, y += pitch) {
        if (i == s.listSel) select_box(x - 6, y - 11, x + w + 6, y + 3);
        event_label(s.list[i], line, sizeof line);
        text_style(x, y, w, ALIGN_CENTER, row_ok(i) ? FONT_STYLE_WHITE : FONT_STYLE_GREY, line);
    }
    snprintf(line, sizeof line, "Gold %d   Health %.0f/%.0f   Potions %d", s.gold, (double)character.health, (double)character.maxHealth, character.healthPotions);
    text_style(0, y + 6 > SCREEN_HEIGHT - T - 4 ? y + 6 : SCREEN_HEIGHT - T - 4, SCREEN_WIDTH, ALIGN_CENTER, FONT_STYLE_GREY, line);     // (never over the rows)
}

void tower_between_draw(void)
{
    if (s.draftOpen) tower_draft_draw();
    else if (s.scr == SCR_MAP || s.scr == SCR_CONFIRM) map_draw();
    else if (s.scr == SCR_SHOP) shop_draw();
    else if (s.scr == SCR_EVENT) event_draw();
    else if (s.scr) rest_draw();
    if (ui_details()->open && ui_details()->owner == UI_OWNER_TOWER) ui_panel_draw(ui_details());     // Details (R) or the map's confirm, over it
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

int tower_build_rows(char left[][40], char right[][12], int style[], int card[], int max)
{
    int n = 0;
    for (int k = 0; k < SKILL_COUNT && n < max; k++, n++) {     // the skills: button, name (its form's), its level
        int f = tower_skill_form(k);
        snprintf(left[n], 40, "%s %s", joypad_skill_button(k), BOONS[f ? skill_form_card(k, f) : BOON_LEARN_MOONBLADE + k].name);
        if (knows(k)) snprintf(right[n], 12, "Lv %d/%d", tower_skill_level(k), SKILL_LV_MAX + 1); else snprintf(right[n], 12, "-");
        style[n] = !knows(k) ? FONT_STYLE_GREY : f ? FONT_STYLE_GOLD : FONT_STYLE_WHITE;
        card[n] = BOON_LEARN_MOONBLADE + k;     // (its Details: the whole track and both forms)
    }
    for (int i = 0; i < TOWER_SLOTS && n < max; i++, n++) {      // the slots: name in its family's colour, its level ("+" at the top)
        if (!s.slot[i]) { snprintf(left[n], 40, "Empty slot"); right[n][0] = 0; style[n] = FONT_STYLE_GREY; card[n] = -1; continue; }
        BoonId b = (BoonId)(s.slot[i] - 1);
        snprintf(left[n], 40, "%s", BOONS[b].name);
        card[n] = b;
        if (BOONS[b].duo && s.merged[i][0]) {       // a duo: "Duo", its two boons on a row under it
            snprintf(right[n], 12, "Duo");
            style[n] = FONT_STYLE_GOLD;
            if (n + 1 < max) { n++; snprintf(left[n], 40, "  %s + %s", BOONS[s.merged[i][0] - 1].name, BOONS[s.merged[i][1] - 1].name); right[n][0] = 0; style[n] = FONT_STYLE_GREY; card[n] = b; }
            continue;
        }
        bool plus = false;
        for (int p = 0; p < BOON_COUNT; p++) plus |= BOONS[p].up == b + 1 && s.stacks[p];
        snprintf(right[n], 12, "Lv %d/%d%s", s.stacks[b], BOONS[b].maxStacks, plus ? "+" : "");
        style[n] = FAMILIES[BOONS[b].family].style;
    }
    // the rivals locked out, greyed, one a row (each its own Details)
    for (int b = 0; b < BOON_COUNT && n < max; b++) {
        if (!s.locked[b]) continue;
        snprintf(left[n], 40, "Locked: %s", BOONS[b].name); snprintf(right[n], 12, "rival"); style[n] = FONT_STYLE_GREY; card[n] = b; n++;
    }
    return n;
}

// The Stats pages: the run's numbers (pause menu > Stats; paged by ui_pages)
void tower_stats_doc(UiDoc *d)
{
    char nl[96];
    ui_doc_row(d, FONT_STYLE_GOLD, NULL, "Floor %d", s.floor);
    if (tower_circle() > 1) ui_doc_row(d, FONT_STYLE_RED, NULL, "Ascension %d", tower_circle() - 1);
    if (s.night) ui_doc_text(d, FONT_STYLE_RED, "%s", night_line(s.night, nl, sizeof nl));
    ui_doc_row(d, FONT_STYLE_GOLD, NULL, "Gold %d   Embers this run %d", s.gold, s.runEmbers);
    ui_doc_text(d, FONT_STYLE_GREY, "Seed %08lX", (unsigned long)s.seed);
    if (s.offers) ui_doc_text(d, FONT_STYLE_WHITE, "Offerings: %s", tower_offers_line());
    int e = tower_relic_equipped();
    if (e >= 0) ui_doc_text(d, FONT_STYLE_WHITE, "Relic: %s (%s)", tower_relic_name(e), tower_relic_desc(e));
    else ui_doc_text(d, FONT_STYLE_GREY, "Relic: none");
    ui_doc_gap(d);
    ui_doc_text(d, FONT_STYLE_WHITE, "Damage x%.2f, damage taken x%.2f", (double)tower_mod_damage_dealt(), (double)tower_mod_damage_taken());
    ui_doc_text(d, FONT_STYLE_WHITE, "Max health %.0f, potions %d (most %d)", (double)tower_mod_max_health(), character.healthPotions, tower_mod_max_potions());
    if (s.potionDebt) ui_doc_text(d, FONT_STYLE_RED, "Potions owed to Thirst: %d", s.potionDebt);
    ui_doc_gap(d);
    int k = 0;
    for (int i = 0; i < PB_COUNT; i++) if (s.bless[i] || s.halfBless[i]) {     // (a Brazier's half: "1.5")
        int h = 2 * s.bless[i] + s.halfBless[i];
        char r[12]; snprintf(r, sizeof r, "x%d%s", h / 2, h % 2 ? ".5" : "");
        ui_doc_row(d, FONT_STYLE_GOLD, r, "%s%s: %s", k++ ? "" : "Blessings: ", BLESSINGS[i].name, BLESSINGS[i].desc);
    }
    if (!k) ui_doc_text(d, FONT_STYLE_GREY, "Blessings: none");
    k = 0;
    for (int i = 0; i < PC_COUNT; i++) if (s.curse[i]) {
        char r[12]; snprintf(r, sizeof r, "x%d", s.curse[i]);
        ui_doc_row(d, FONT_STYLE_RED, r, "%s%s: %s", k++ ? "" : "Curses: ", CURSES[i].name, CURSES[i].desc);
    }
    if (!k) ui_doc_text(d, FONT_STYLE_GREY, "Curses: none");
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
// After the packs, the skills' B forms (section 14b: 200 Embers each, bits 6-8 of the unlocks)
static int family_packs(void) { int n = 0; while (pack_family(n) >= 0) n++; return n; }
static int pack_form(int i) { int k = i - family_packs(); return k >= 0 && k < SKILL_COUNT ? k : -1; }
int tower_pack_count(void) { return family_packs() + SKILL_COUNT; }
const char* tower_pack_name(int i)
{
    int f = pack_family(i), k = pack_form(i);
    return f >= 0 ? FAMILIES[f].name : k >= 0 ? BOONS[skill_form_card(k, SKILL_FORM_B)].name : "";
}
const char* tower_pack_desc(int i)
{
    static char d[96];
    int f = pack_family(i), n = 0, k = pack_form(i);
    d[0] = 0;
    if (k >= 0) {
        int m = snprintf(d, sizeof d, "%s's second Lv 5 form: ", BOONS[BOON_LEARN_MOONBLADE + k].name);
        snprintf(d + m, sizeof d - m, "%s", BOONS[skill_form_card(k, SKILL_FORM_B)].desc);
        if (d[m] >= 'A' && d[m] <= 'Z') d[m] += 'a' - 'A';     // (mid-sentence)
    }
    for (int b = 0; b < BOON_COUNT && f >= 0; b++)
        if (BOONS[b].pack && BOONS[b].family == f) n += snprintf(d + n, sizeof d - n, "%s%s", n ? ", " : "New boons: ", BOONS[b].name);
    return d;
}
// one line under the Unlocks list (R: all of it)
const char* tower_pack_brief(int i)
{
    static char d[64];
    int f = pack_family(i), k = pack_form(i), n = 0;
    if (k >= 0) snprintf(d, sizeof d, "%s's second Lv 5 form", BOONS[BOON_LEARN_MOONBLADE + k].name);
    else { for (int b = 0; b < BOON_COUNT; b++) n += f >= 0 && BOONS[b].pack && BOONS[b].family == f; snprintf(d, sizeof d, "%d new %s boon%s", n, f >= 0 ? FAMILIES[f].name : "", n == 1 ? "" : "s"); }
    return d;
}
static int pack_bit(int i) { int f = pack_family(i), k = pack_form(i); return f >= 0 ? f : k >= 0 ? FAM_COUNT + k : -1; }
bool tower_pack_owned(int i) { int bit = pack_bit(i); TowerSave t; tower_load(&t); return bit >= 0 && ((t.unlocks >> bit) & 1); }
int  tower_pack_cost(int i)
{
    if (tower_pack_owned(i)) return -1;
    if (pack_form(i) >= 0) return FORM_COST;
    TowerSave t; tower_load(&t);
    int bought = 0;
    for (int f = 0; f < FAM_COUNT; f++) bought += (t.unlocks >> f) & 1;
    return PACK_COST[bought < FAM_COUNT ? bought : FAM_COUNT - 1];
}
bool tower_pack_buy(int i)
{
    int bit = pack_bit(i), cost = tower_pack_cost(i);
    TowerSave t; tower_load(&t);
    if (bit < 0 || cost < 0 || t.embers < cost) return false;
    t.embers -= cost;
    t.unlocks |= 1 << bit;
#ifdef HLE_AUTOPLAY
    debugf("[unlock] bought %s for %d: unlocks %03x, Embers %d\n", tower_pack_name(i), cost, t.unlocks, t.embers);
#endif
    return save_controller_set_tower(&t);
}

// The Shrine's Details (R): an upgrade, a relic (id; -1 none), an unlock (a pack or a B form), the Nightfarer
void tower_details_shrine(int kind, int i)
{
    UiPanel *p = ui_details();
    UiDoc *d = ui_panel_doc(p);
    char t[64] = ""; int st = FONT_STYLE_GOLD;
    ui_doc_clear(d);
    text_load();
    if (kind == TOWER_SHRINE_UPGRADE) {
        int lv = tower_upgrade_level((UpgradeId)i), c = tower_upgrade_cost((UpgradeId)i);
        snprintf(t, sizeof t, "%s", UPGRADES[i].name);
        ui_doc_text(d, FONT_STYLE_WHITE, "%s a level, for every climb (the Tower only).", UPGRADES[i].desc);
        ui_doc_text(d, FONT_STYLE_GOLD, "Lv %d of %d now.", lv, UPG_MAX_LEVEL);
        if (c >= 0) ui_doc_text(d, tower_embers() >= c ? FONT_STYLE_WHITE : FONT_STYLE_RED, "Next level: %d Embers (you hold %d).", c, tower_embers());
        else ui_doc_text(d, FONT_STYLE_GREY, "At its max.");
    } else if (kind == TOWER_SHRINE_RELIC) {
        if (i < 0) { snprintf(t, sizeof t, "No relic"); ui_doc_text(d, FONT_STYLE_WHITE, "Climb with no relic: no blessing, no curse."); }
        else {
            snprintf(t, sizeof t, "%s", RELICS[i].name);
            for (int k = 0; k < 2; k++) { int fx = k ? RELICS[i].fx & 15 : RELICS[i].fx >> 4;
                ui_doc_text(d, fx >= RELIC_FRAIL ? FONT_STYLE_RED : FONT_STYLE_WHITE, "%s: %s", RELIC_FX[fx].adj, RELIC_FX[fx].desc); }
            ui_doc_gap(d);
            ui_doc_text(d, FONT_STYLE_GREY, "One relic is worn for a whole climb.");
            if (tower_relic_equipped() == i) ui_doc_text(d, FONT_STYLE_GOLD, "Worn. A takes it off.");
            else if (tower_relic_owned(i)) ui_doc_text(d, FONT_STYLE_WHITE, "Owned. A wears it.");
            else if (i == RELIC_SIGIL) ui_doc_text(d, FONT_STYLE_GREY, "Not sold: it's yours once you reach floor %d.", MILESTONES[MS_SIGIL].floor);
            else ui_doc_text(d, tower_embers() >= RELIC_COST ? FONT_STYLE_WHITE : FONT_STYLE_RED, "%d Embers (you hold %d). A buys and wears it.", RELIC_COST, tower_embers());
        }
    } else if (kind == TOWER_SHRINE_UNLOCK) {
        int f = pack_family(i), k = pack_form(i), c = tower_pack_cost(i);
        if (f >= 0) {
            snprintf(t, sizeof t, "%s pack", FAMILIES[f].name); st = FAMILIES[f].style;
            ui_doc_text(d, FONT_STYLE_GREY, "New %s boons, in the drafts and Shops of every climb from then on:", FAMILIES[f].name);
            for (int b = 0; b < BOON_COUNT; b++) if (BOONS[b].pack && BOONS[b].family == f) {
                char r[24]; snprintf(r, sizeof r, "%s", TIER_NAME[BOONS[b].tier]);
                ui_doc_gap(d); ui_doc_keep(d);
                ui_doc_row(d, NAME_STYLE[BOONS[b].tier], r, "%s  (max Lv %d)", BOONS[b].name, BOONS[b].maxStacks);
                ui_doc_text(d, FONT_STYLE_WHITE, "%s", full_text(b));
                if (rival_of(b) >= 0) { char rn[48]; rival_names(b, rn, sizeof rn); ui_doc_text(d, FONT_STYLE_RED, "Rival: %s", rn); }
            }
            char line[160]; int m = 0; line[0] = 0;
            for (int du = 0; du < BOON_COUNT; du++) if (BOONS[du].duo && (BOONS[du].family == f || BOONS[du].duo - 1 == f))
                m += snprintf(line + m, m < (int)sizeof line ? sizeof line - m : 0, "%s%s (%s)", m ? ", " : "", BOONS[du].name, FAMILIES[BOONS[du].family == f ? BOONS[du].duo - 1 : BOONS[du].family].name);
            ui_doc_gap(d);
            ui_doc_text(d, FONT_STYLE_GOLD, "Duos it opens, with the other family's pack: %s", line);
        } else if (k >= 0) {
            BoonId fb = skill_form_card(k, SKILL_FORM_B), fa = skill_form_card(k, SKILL_FORM_A);
            snprintf(t, sizeof t, "%s", BOONS[fb].name); st = FONT_STYLE_GOLD;
            ui_doc_text(d, FONT_STYLE_GREY, "%s's second Lv 5 form, offered beside %s once the skill reaches Lv 4:", BOONS[BOON_LEARN_MOONBLADE + k].name, BOONS[fa].name);
            ui_doc_text(d, FONT_STYLE_WHITE, "%s", full_text(fb));
            ui_doc_gap(d);
            ui_doc_text(d, FONT_STYLE_RED, "Its rival, %s: %s", BOONS[fa].name, full_text(fa));
        }
        ui_doc_gap(d);
        if (c < 0) ui_doc_text(d, FONT_STYLE_GREY, "Owned.");
        else ui_doc_text(d, tower_embers() >= c ? FONT_STYLE_WHITE : FONT_STYLE_RED, "%d Embers (you hold %d)%s.", c, tower_embers(), f >= 0 ? "; each pack bought raises the next one's price" : "");
    } else if (kind == TOWER_SHRINE_OFFERING) {
        snprintf(t, sizeof t, "%s", OFFERINGS[i].name);
        ui_doc_text(d, FONT_STYLE_WHITE, "%s", full_text(TXT_OFFER + i));
        ui_doc_gap(d);
        ui_doc_text(d, FONT_STYLE_GREY, "For the next climb only, used as it starts. Quit before its first Guardian falls and it waits for the next one.");
        const char *why = tower_offer_lock(i);
        int c = tower_offer_price(i);
        if (why[0]) ui_doc_text(d, FONT_STYLE_RED, "%s.", why);
        else if (tower_offer_set(i)) ui_doc_text(d, FONT_STYLE_GOLD, "Set aside. A takes it back: %d Embers returned.", c);
        else if (tower_offers_held() >= OFFER_MAX) ui_doc_text(d, FONT_STYLE_RED, "Two are set aside: take one back first.");
        else ui_doc_text(d, tower_embers() >= c ? FONT_STYLE_WHITE : FONT_STYLE_RED, "%d Embers (you hold %d)%s.", c, tower_embers(), tower_offers_held() ? ": a second Offering costs 50 more" : "");
    } else {
        snprintf(t, sizeof t, "Nightfarer");
        ui_doc_text(d, FONT_STYLE_WHITE, "A skin: a tracker's winged helm and blue coat. It changes nothing in a fight.");
        if (tower_nightfarer_owned()) ui_doc_text(d, FONT_STYLE_GREY, "Owned: worn from Settings > Skin.");
        else ui_doc_text(d, tower_embers() >= NIGHTFARER_COST ? FONT_STYLE_WHITE : FONT_STYLE_RED, "%d Embers (you hold %d).", NIGHTFARER_COST, tower_embers());
    }
    text_free();
    ui_panel_open(p, t, st, false);
    p->owner = UI_OWNER_MENU;
}

// A milestone reached: its notice over the draft (R, B or A closes it; it waits out the draft's input delay)
static void milestone_notice(int i)
{
    static const char *const WHERE[MS_COUNT] = { "It waited high within the Tower.", "Set it aside at Tower > Offerings.", "From the next climb on.",
                                                 "Worn from Tower > Shrine > Relics.", "Read it in the Codex.", "Yours for good." };
    UiPanel *p = ui_details();
    UiDoc *d = ui_panel_doc(p);
    char t[40];
    ui_doc_clear(d);
    snprintf(t, sizeof t, "Floor %d reached", MILESTONES[i].floor);
    ui_doc_text(d, FONT_STYLE_GOLD, "%s", MILESTONES[i].name);
    ui_doc_text(d, FONT_STYLE_WHITE, "%s.", MILESTONES[i].reward);
    ui_doc_gap(d);
    ui_doc_text(d, FONT_STYLE_GREY, "%s", WHERE[i]);
    ui_panel_open(p, t, FONT_STYLE_GOLD, false);
    p->owner = UI_OWNER_TOWER;
#ifdef HLE_AUTOPLAY
    debugf("[milestone] floor %d reached: %s (%s), notice over the draft\n", MILESTONES[i].floor, MILESTONES[i].name, MILESTONES[i].reward);
#endif
}

// ---- The Codex: a page a topic (tower_text.bin's text, segments split by '|': '#' a heading, '%' grey, '!' red,
// '+' after a gap, '~' a new page, '@' the topic's own table here), read from ROM while it's built ----
static const char *const CODEX_TITLE[CX_COUNT] = {
    "The climb", "The map", "Fights and Elites", "Rests, Shops, Events", "Slots and levels", "Families", "Rivals", "\"+\" and duos",
    "Skills", "Moon Blade Dance", "Halfmoon Slash", "Crescent Cleave", "Elite affixes", "Gold", "Embers and the Shrine", "Offerings",
    "Milestones", "Pacts", "Nights", "The Tower's Heart",
};
int  tower_codex_count(void) { return CX_COUNT; }
const char *tower_codex_title(int i) { return i >= 0 && i < CX_COUNT ? CODEX_TITLE[i] : ""; }
int  tower_codex_floor(int i) { return i == CX_HEART ? MILESTONES[MS_HEART].floor : 0; }     // a page found deeper (0: open)
bool tower_codex_open_ok(int i) { return i >= 0 && i < CX_COUNT && tower_best_floor() >= tower_codex_floor(i); }

// a short text mid-sentence: its first letter small ("Start with 60 gold" after a colon), a name's capitals kept
static const char *lc(const char *t)
{
    static char b[2][96]; static int k;
    char *o = b[k++ & 1];     // (two at once: a row's printf holds no more)
    snprintf(o, 96, "%s", t);
    if (o[0] >= 'A' && o[0] <= 'Z' && ((o[1] >= 'a' && o[1] <= 'z') || o[1] == ' ') && strncmp(o, "Storm Blade", 11)) o[0] += 'a' - 'A';
    return o;
}
static void codex_gen(UiDoc *d, int k)
{
    char line[200];
    int best = tower_best_floor();
    switch (k) {
    case CX_STOPS:
        ui_doc_text(d, FONT_STYLE_WHITE, "A new boon or duo: %d, %d or %d by its tier.", TIER_PRICE[0], TIER_PRICE[1], TIER_PRICE[2]);
        ui_doc_text(d, FONT_STYLE_WHITE, "A level: %d. A \"+\": %d. Release: %d.", PRICE_LEVEL, PRICE_PLUS, PRICE_RELEASE);
        ui_doc_text(d, FONT_STYLE_WHITE, "A potion: %d, up to your most. Mend: %d, heals 35%%.", PRICE_POTION, PRICE_MEND);
        ui_doc_text(d, FONT_STYLE_WHITE, "A pact: free. Its curse is the price.");
        break;
    case CX_FAMILIES: {
        TowerSave sv; tower_load(&sv);
        for (int f = 0; f < FAM_COUNT; f++) {
            int n = 0, m = 0; line[0] = 0;
            char pk[120]; pk[0] = 0;
            for (int b = 0; b < BOON_COUNT; b++) if (BOONS[b].family == f && takes_slot(b)) {
                if (BOONS[b].pack) m += snprintf(pk + m, m < (int)sizeof pk ? sizeof pk - m : 0, "%s%s", m ? ", " : "", BOONS[b].name);
                else n += snprintf(line + n, n < (int)sizeof line ? sizeof line - n : 0, "%s%s", n ? ", " : "", BOONS[b].name);
            }
            bool own = s.active ? family_open(f) : (sv.unlocks >> f) & 1;
            ui_doc_gap(d);
            ui_doc_text(d, FAMILIES[f].style, "%s: %s", FAMILIES[f].name, line);
            if (m) ui_doc_text(d, own ? FAMILIES[f].style : FONT_STYLE_GREY, "%s %s", own ? "Its pack:" : "Its pack (Shrine):", pk);
        }
        break; }
    case CX_RIVALS:
        for (unsigned i = 0; i < sizeof RIVALS / sizeof RIVALS[0]; i++)
            ui_doc_text(d, is_form(RIVALS[i][0]) ? FONT_STYLE_GOLD : FONT_STYLE_RED, "%s  /  %s", BOONS[RIVALS[i][0]].name, BOONS[RIVALS[i][1]].name);
        break;
    case CX_PLUS:
        ui_doc_gap(d);
        for (int b = 0; b < BOON_COUNT; b++) if (BOONS[b].duo)
            ui_doc_text(d, FONT_STYLE_WHITE, "%s, %s + %s: %s", BOONS[b].name, FAMILIES[BOONS[b].family].name, FAMILIES[BOONS[b].duo - 1].name, lc(BOONS[b].desc));
        break;
    case CX_MBD: case CX_HALFMOON: case CX_CLEAVE:
        if (!s.active) { TowerSave t; tower_load(&t); s.forms = (t.unlocks >> FAM_COUNT) & ((1 << SKILL_COUNT) - 1); }   // (outside a climb: the B forms bought)
        doc_skill(d, k - CX_MBD);
        break;
    case CX_AFFIXES:
        for (int a = 0; a < AFX_COUNT; a++) {
            if (best >= AFFIXES[a].floor) ui_doc_text(d, FONT_STYLE_WHITE, "%s: %s", AFFIXES[a].name, full_text(TXT_AFFIX + a));
            else ui_doc_text(d, FONT_STYLE_GREY, "%s (met from floor %d): %s", AFFIXES[a].name, AFFIXES[a].floor, full_text(TXT_AFFIX + a));
        }
        break;
    case CX_GOLD:
        ui_doc_text(d, FONT_STYLE_WHITE, "A fallen boss: %d, and %d more a circle. An Elite: x1.5.", GOLD_FIGHT + GOLD_PER_CIRCLE, GOLD_PER_CIRCLE);
        ui_doc_text(d, FONT_STYLE_WHITE, "Skip a draft: +%d. The Purse card: +%d, once no boon is left for you.", SKIP_GOLD, GOLD_CARD_GOLD);
        ui_doc_text(d, FONT_STYLE_WHITE, "Prospector: +20%% a level, on every gain.");
        break;
    case CX_EMBERS:
        ui_doc_text(d, FONT_STYLE_WHITE, "A fallen boss: 8 x its circle, and 3 more for each fight before it in the circle.");
        ui_doc_text(d, FONT_STYLE_WHITE, "An Elite: x1.5. A circle cleared: +%d. Skip a draft: +%d.", CIRCLE_EMBERS, SKIP_EMBERS);
        break;
    case CX_OFFERINGS: {
        for (int i = 0; i < OFF_COUNT; i++) {
            bool open = best >= OFFERINGS[i].floor;
            char r[16]; if (open) snprintf(r, sizeof r, "%d", OFFERINGS[i].price); else snprintf(r, sizeof r, "floor %d", OFFERINGS[i].floor);
            ui_doc_row(d, open ? FONT_STYLE_WHITE : FONT_STYLE_GREY, r, "%s: %s", OFFERINGS[i].name, lc(OFFERINGS[i].brief));
        }
        const char *now = tower_offers_line();
        ui_doc_gap(d);
        if (now[0]) ui_doc_text(d, FONT_STYLE_GOLD, "%s: %s", s.active ? "This climb's" : "Set aside", now);
        break; }
    case CX_MILESTONES:
        for (int i = 0; i < MS_COUNT; i++) {
            bool got = best >= MILESTONES[i].floor;
            char l[16]; snprintf(l, sizeof l, "Floor %d", MILESTONES[i].floor);
            ui_doc_text(d, got ? FONT_STYLE_GOLD : FONT_STYLE_WHITE, "%s, %s: %s%s", l, MILESTONES[i].name, lc(MILESTONES[i].reward), got ? " (reached)" : "");
        }
        ui_doc_gap(d);
        ui_doc_text(d, FONT_STYLE_GOLD, "Your best: floor %d.", best);
        break;
    case CX_PACTS:
        ui_doc_gap(d);      // the blessings in gold, the curses in red
        for (int b = 0; b < PB_COUNT; b++) ui_doc_text(d, FONT_STYLE_GOLD, "%s: %s", BLESSINGS[b].name, BLESSINGS[b].desc);
        ui_doc_gap(d);
        for (int c = 0; c < PC_COUNT; c++) ui_doc_text(d, FONT_STYLE_RED, "%s: %s", CURSES[c].name, CURSES[c].desc);
        break;
    case CX_NIGHTS:
        for (int n = 1; n < NIGHT_COUNT; n++) {
            ui_doc_text(d, FONT_STYLE_RED, "%s: %s. Embers x%.1f.", NIGHTS[n].name, lc(NIGHTS[n].desc), (double)NIGHTS[n].embers);
        }
        break;
    }
    (void)line;
}

// a paragraph a sentence a row (no gap between them): a page can then turn between two sentences, not only between paragraphs
static void codex_para(UiDoc *d, int style, const char *t)
{
    char one[256];
    while (*t) {
        const char *e = strstr(t, ". ");
        int n = e ? (int)(e - t) + 1 : (int)strlen(t);
        if (n > (int)sizeof one - 1) n = sizeof one - 1;
        memcpy(one, t, n); one[n] = 0;
        ui_doc_text(d, style, "%s", one);
        t += n; while (*t == ' ') t++;
    }
}
// a page's rows from its text (and its table where the text has '@'); returns its title
static const char *codex_doc(UiDoc *d, int k)
{
    ui_doc_clear(d);
    const char *src = full_text(TXT_CODEX + k);
    char seg[256];
    bool gen = false;
    while (*src) {
        const char *e = strchr(src, '|');
        int n = e ? (int)(e - src) : (int)strlen(src);
        if (n > (int)sizeof seg - 1) n = sizeof seg - 1;
        memcpy(seg, src, n); seg[n] = 0;
        src += e ? n + 1 : n;
        const char *t = seg + 1;
        switch (seg[0]) {
        case '#': ui_doc_gap(d); ui_doc_keep(d); ui_doc_text(d, FONT_STYLE_GOLD, "%s", t); break;
        case '%': ui_doc_gap(d); codex_para(d, FONT_STYLE_GREY, t); break;
        case '!': codex_para(d, FONT_STYLE_RED, t); break;
        case '+': ui_doc_gap(d); codex_para(d, FONT_STYLE_WHITE, t); break;
        case '~': ui_doc_break(d); if (*t) codex_para(d, FONT_STYLE_WHITE, t); break;
        case '@': codex_gen(d, k); gen = true; break;
        default:  if (seg[0]) codex_para(d, FONT_STYLE_WHITE, seg); break;
        }
    }
    if (!gen) codex_gen(d, k);
    return CODEX_TITLE[k];
}
void tower_codex_open(int k)
{
    if (!tower_codex_open_ok(k)) return;
    UiPanel *p = ui_details();
    UiDoc *d = ui_panel_doc(p);
    text_load();
    const char *t = codex_doc(d, k);
    text_free();
    ui_panel_open(p, t, FONT_STYLE_GOLD, false);
    p->owner = UI_OWNER_MENU;
#ifdef HLE_AUTOPLAY
    debugf("[codex] open %d %s: %d pages, %d rows, %d of %d bytes\n", k, t, p->pg.pages, d->n, d->used, UI_DOC_TEXT);
#endif
}

const char* tower_death_summary(void)
{
    static char line[96];
    int boons = 0;
    for (int b = 0; b < BOON_COUNT; b++) boons += s.stacks[b];
    for (int b = 0; b < PB_COUNT; b++) boons += s.bless[b];
    snprintf(line, sizeof line, "Floor %d   +%d Embers (%d)\n%d boon%s taken   Seed %08lX", s.floor, s.runEmbers, tower_embers(), boons, boons == 1 ? "" : "s", (unsigned long)s.seed);
    return line;
}
uint32_t tower_seed(void) { return s.seed; }

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
// a blessing's stacks: the pacts', and the Ember Brazier's halves
static float bl(int b) { return s.bless[b] + 0.5f * s.halfBless[b]; }

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
        if (s.circle == 1 && tower_offer_active(OFF_WHETSTONE)) m *= OFFER_DAMAGE;
        m *= (1.0f + 0.08f * bl(PB_MIGHT)) * powf_int(0.95f, s.curse[PC_WEAKNESS]);
        if (character.health < character.maxHealth * 0.3f) m *= 1.0f + 0.15f * bl(PB_LAST_BREATH);
    }
    if (tower_boon_stacks(BOON_LAST_STAND) && character.health < character.maxHealth * 0.3f) m *= 1.0f + 0.4f * tower_boon_stacks(BOON_LAST_STAND);
    if (tower_boon_stacks(BOON_BLOOD_PACT)) m *= 1.35f;
    extern double nowS;
    if (tower_boon_stacks(BOON_RIPOSTE) && nowS < s.riposteUntil) m *= 1.5f;
    if (has_relic(RELIC_DAMAGE)) m *= 1.06f;
    if (has_relic(RELIC_FURY)) m *= 1.15f;
#ifdef HLE_RUN_BOT      // test aid: the climbing bot's sword brought to the metric's (it swings 79 a minute in the openings, the metric's 148)
    if (s.active) m *= 1.9f;
#endif
    return m;
}

float tower_mod_damage_vs(float bossHealthFrac, bool staggered, int kind)
{
    if (!s.active) return 1.0f;
    float m = 1.0f;
    if (bossHealthFrac < 0.5f) m *= 1.0f + 0.12f * bl(PB_WOUNDED);
    (void)staggered;    // (Breaker and Eclipse: tower_mod_stagger_damage, added and capped)
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
    if (sword && berserk_bonus() > 0.0f) { m *= 1.0f + berserk_bonus(); BFX("Berserker: hit %d in a row x%.2f\n", s.berserkHits + 1, (double)(1.0f + berserk_bonus())); }
    if (kind == TOWER_HIT_SKILL && tower_boon_stacks(BOON_BLOOD_MOON_RITE)) { m *= 1.4f; BFX("Blood Moon Rite: a skill's hit x1.40\n"); }
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
        m *= powf_int(0.94f, circles_cleared()) * powf(0.94f, bl(PB_BULWARK)) * (1.0f + 0.05f * s.curse[PC_FRAILTY]);
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

float tower_mod_stagger_time(void)
{
#ifdef HLE_STAGGER_LONG     // test aid: Crushing Blows outside the Tower
    return 1.5f;
#endif
    return (tower_boon_stacks(BOON_CRUSHING_BLOWS) ? 1.5f : 1.0f) * (tower_affix(AFX_RELENTLESS) ? RELENTLESS_DOWN : 1.0f);     // (Relentless: up sooner)
}
// A staggered boss's bonuses add (Executioner +60%, +90% with its "+"; Breaker +15% a stack; Eclipse +50%
// on a skill's hit), capped at +100% (section 6, the stagger build)
#define STAGGER_BONUS_CAP 1.0f
float tower_mod_stagger_damage(int kind)
{
    if (!s.active) return 1.0f;
    float add = (tower_boon_stacks(BOON_EXECUTIONER) ? (tower_boon_stacks(BOON_EXECUTIONER_PLUS) ? 0.9f : 0.6f) : 0.0f) + 0.15f * bl(PB_STAGGERED);
    if (kind == TOWER_HIT_SKILL && tower_boon_stacks(BOON_ECLIPSE)) { add += 0.5f; BFX("Eclipse: skill hit on a staggered boss +50%%\n"); }
    return 1.0f + fminf(STAGGER_BONUS_CAP, add);
}
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
    if (tower_boon_stacks(BOON_BERSERKER)) {    // the streak: a hit within 2 s of the last, below half health
        extern double nowS;
        s.berserkHits = tower_berserk() && nowS - s.berserkAt <= BERSERK_GAP ? s.berserkHits + 1 : tower_berserk();
        s.berserkAt = nowS;
    }
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
        float tc = THUNDERCLAP_BOLT * (tower_boon_stacks(BOON_THUNDERCLAP_PLUS) ? 1.5f : 1.0f);
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

int   tower_mod_max_potions(void) { return s.active ? START_POTIONS + shrine(UPG_FLASK_CAPACITY) + 3 * tower_boon_stacks(BOON_DEEP_FLASK) + 2 * has_relic(RELIC_FLASK) - has_relic(RELIC_PARCHED)
                                                    + tower_offer_active(OFF_FLASK) + s.milestonePotion : 3; }   // Story and Bonus: 3
bool  tower_mod_skill_allowed(int skill) { return character_skills_enabled() && (!s.active || knows(skill)); }
float tower_mod_potion_heal(void)
{
    return 0.45f * (1.0f + 0.08f * shrine(UPG_FLASK_POTENCY)) * (1.0f + 0.25f * tower_boon_stacks(BOON_KINDLED_FLASK))
           * (tower_boon_stacks(BOON_BLOOD_PACT) ? 0.75f : 1.0f);
}
float tower_mod_max_health(void) { return 150.0f + 10.0f * shrine(UPG_VIGOR) + 25.0f * tower_boon_stacks(BOON_VIGOR) + 15.0f * has_relic(RELIC_HEALTH) - 20.0f * has_relic(RELIC_FRAIL) + (s.active ? 12.0f * bl(PB_VITALITY) + s.steelHp - s.altarHp + OFFER_HP * tower_offer_active(OFF_OATH) : 0.0f); }
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
#define RALLY_RATIO 0.25f    // of each hit's base won back, Lv 1 (a sword hit 1); Lv 2 0.5, Rally+ 0.75 (all of it out-healed every floor: 60-90% of what landed)
void tower_on_player_hurt(float lost)
{
    if (s.berserkHits) BFX("Berserker: a blow ends a streak of %d\n", s.berserkHits);
    s.berserkHits = 0;      // (Berserker's streak ends with a blow)
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
    float ratio = tower_boon_stacks(BOON_RALLY_PLUS) ? 3.0f * RALLY_RATIO : tower_boon_stacks(BOON_RALLY) >= 2 ? 2.0f * RALLY_RATIO : tower_boon_stacks(BOON_RALLY) ? RALLY_RATIO : 0.0f;
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

#ifdef HLE_TEST_TOWER
void tower_test_grant_unbroken(void) { s.stacks[BOON_UNBROKEN]++; }     // test aid: the boon without the draft
void tower_test_pick_vigor(void) { s.choices[0] = BOON_VIGOR; s.nChoices = 1; s.draftOpen = true; tower_draft_pick(0); }   // test aid: Vigor taken from a draft

// Self-check of the slots, levels, a full bar, the packs, family weighting and the unlock bits
// (HLE_TEST_TOWER); returns the failures
static void test_unnotice(void) { if (ui_details()->open && ui_details()->owner == UI_OWNER_TOWER) ui_panel_close(ui_details()); }   // (a milestone's notice, in a self-check that doesn't look at it)
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
    CHECK(bolts[0] == 0.0f && bolts[3] == 6.0f + STATIC_BOLT, "Storm Blade + Static Lv 1: hit 4 calls 6 + Static's (%.0f)", (double)bolts[3]);
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
      debugf("[slots] full bar, 500 drafts: Static %d, Shadow Edge %d, Iron Skin %d, Halfmoon Slash Lv 2 %d\n", counts[BOON_STATIC], counts[BOON_SHADOW_EDGE], counts[BOON_IRON_SKIN], counts[BOON_LEARN_HALFMOON]); }
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
// Stage 5's self-check (HLE_TEST_TOWER): the skills' levels and forms, their cards and draft rules,
// Temper, the Moonlit Pool, the B forms' unlocks at the Shrine, the stagger bonuses' cap, and the
// pick math (how many skill cards a run sees); returns the failures
int tower_test_skills(void)
{
    int fails = 0, counts[BOON_COUNT];
    #define CHECK(c, ...) do { if (!(c)) { fails++; debugf("[skills] FAIL: " __VA_ARGS__); debugf("\n"); } } while (0)
    #define RUN(seed) do { tower_begin_run(seed); s.packs = (1 << FAM_COUNT) - 1; s.relicFx[0] = s.relicFx[1] = 0; s.forms = 0; } while (0)
    // levels: Learn is Lv 1, the card again Lv 2-4, then a form (Lv 5); outside a run Lv 1
    RUN(301);
    CHECK(tower_skill_level(SKILL_HALFMOON_SLASH) == 1 && !tower_skill_form(SKILL_HALFMOON_SLASH) && offerable(BOON_LEARN_HALFMOON) && card_tier(BOON_LEARN_HALFMOON) == TIER_COMMON,
          "unknown: its Learn card, Common");
    take_boon(BOON_LEARN_HALFMOON);
    CHECK(knows(SKILL_HALFMOON_SLASH) && tower_skill_level(SKILL_HALFMOON_SLASH) == 1 && offerable(BOON_LEARN_HALFMOON) && card_tier(BOON_LEARN_HALFMOON) == TIER_RARE && slots_used() == 0,
          "learned: Lv 1, its card now its Lv 2 (Rare, no slot)");
    take_boon(BOON_LEARN_HALFMOON); take_boon(BOON_LEARN_HALFMOON);
    CHECK(tower_skill_level(SKILL_HALFMOON_SLASH) == 3 && !s.formsDue && !offerable(BOON_TWIN_MOON), "Lv 3: no form yet (%d)", tower_skill_level(SKILL_HALFMOON_SLASH));
    take_boon(BOON_LEARN_HALFMOON);
    CHECK(tower_skill_level(SKILL_HALFMOON_SLASH) == 4 && !offerable(BOON_LEARN_HALFMOON) && offerable(BOON_TWIN_MOON) && !offerable(BOON_FULL_MOON) && s.formsDue == 1 << SKILL_HALFMOON_SLASH,
          "Lv 4: the card stops, Twin Moon offerable, Full Moon not (not bought)");
    // the next draft leads with its forms (A only: B not bought), then they're ordinary cards
    tower_draft_open();
    CHECK(s.choices[0] == BOON_TWIN_MOON && s.choices[1] != BOON_FULL_MOON && !s.formsDue, "the draft after Lv 4 leads with Twin Moon (B not bought) (%d %d)", s.choices[0], s.choices[1]);
    s.draftOpen = false;
    s.forms = 1 << SKILL_HALFMOON_SLASH; s.formsDue = 1 << SKILL_HALFMOON_SLASH;
    tower_draft_open();
    CHECK(s.choices[0] == BOON_TWIN_MOON && s.choices[1] == BOON_FULL_MOON && s.nChoices == 3, "B bought: both forms, two of the three cards (%d %d)", s.choices[0], s.choices[1]);
    s.draftOpen = false;
    draft_counts(200, counts);
    CHECK(counts[BOON_TWIN_MOON] < 200 && counts[BOON_TWIN_MOON] + counts[BOON_FULL_MOON] > 0 && counts[BOON_TWIN_MOON] < 200, "after that draft the forms are ordinary cards (%d %d of 200)", counts[BOON_TWIN_MOON], counts[BOON_FULL_MOON]);
    { int both = 0; for (int k = 0; k < 200; k++) { tower_draft_open(); bool a = false, b = false; for (int i = 0; i < s.nChoices; i++) { a |= s.choices[i] == BOON_TWIN_MOON; b |= s.choices[i] == BOON_FULL_MOON; } both += a && b; }
      s.draftOpen = false; CHECK(!both, "an ordinary draft never holds both forms (%d)", both); }
    take_boon(BOON_FULL_MOON);
    CHECK(tower_skill_level(SKILL_HALFMOON_SLASH) == 5 && tower_skill_form(SKILL_HALFMOON_SLASH) == SKILL_FORM_B && s.locked[BOON_TWIN_MOON] && !offerable(BOON_TWIN_MOON) && !offerable(BOON_FULL_MOON),
          "Full Moon taken: Lv 5, form B, Twin Moon locked out for the run");
    { char l[TOWER_BUILD_ROWS][40], r[TOWER_BUILD_ROWS][12]; int st[TOWER_BUILD_ROWS], cd[TOWER_BUILD_ROWS];
      int n = tower_build_rows(l, r, st, cd, TOWER_BUILD_ROWS); bool locked = false;
      for (int i = 0; i < n; i++) locked |= strstr(l[i], "Locked:") && strstr(l[i], "Twin Moon");
      CHECK(strstr(l[1], "Full Moon") && !strcmp(r[1], "Lv 5/5") && st[1] == FONT_STYLE_GOLD && !strcmp(r[0], "-") && locked,
            "Build: \"%s\" \"%s\", the other form greyed under Locked", l[1], r[1]); }
    // a Learn card leads every draft until a skill is known; then skill cards are ordinary
    RUN(302);
    { int lead = 0; for (int k = 0; k < 200; k++) { tower_draft_open(); lead += is_learn(s.choices[0]); } s.draftOpen = false;
      CHECK(lead == 200, "no skill known: a Learn card leads every draft (%d of 200)", lead); }
    take_boon(BOON_LEARN_CLEAVE);
    { int lead = 0, any = 0; for (int k = 0; k < 300; k++) { tower_draft_open(); lead += is_skill_card(s.choices[0]); for (int i = 0; i < s.nChoices; i++) any += s.choices[i] < BOON_COUNT && is_skill_card(s.choices[i]); }
      s.draftOpen = false;
      CHECK(lead < 150 && any > 0, "a skill known: skill cards are ordinary (first card %d, %d in all, of 300 drafts)", lead, any);
      debugf("[skills] one skill known at Lv 1: %d skill cards in 300 drafts (%.2f a draft)\n", any, (double)(any / 300.0f)); }
    // a third of the drafts hold a skill card; a skill at Lv 2+ twice as likely as each other one
    draft_counts(3000, counts); int c2 = counts[BOON_LEARN_CLEAVE], h2 = counts[BOON_LEARN_HALFMOON];
    take_boon(BOON_LEARN_CLEAVE); draft_counts(3000, counts); int c3 = counts[BOON_LEARN_CLEAVE], h3 = counts[BOON_LEARN_HALFMOON];
    int all3 = 0; for (int b = 0; b < BOON_COUNT; b++) if (is_skill_card(b)) all3 += counts[b];
    debugf("[skills] 3000 drafts: the Cleave at Lv 1 %d (Halfmoon's Learn %d), at Lv 2 %d (Halfmoon's %d): x%.2f each other one; skill cards in %.0f%% of drafts\n",
           c2, h2, c3, h3, (double)(c3 / fmaxf(1.0f, (float)h3)), (double)(all3 / 30.0f));
    CHECK(c3 > h3 * 8 / 5 && c2 < h2 * 5 / 4 && all3 > 900 && all3 < 1100, "a skill at Lv 2+: its card twice as likely as another's (%d vs %d); a skill card in a third of the drafts (%d of 3000)", c3, h3, all3);
    // an Elite's draft (Rare or better) has the level cards, not the Learn ones
    s.elite = true; draft_counts(300, counts); s.elite = false;
    CHECK(counts[BOON_LEARN_CLEAVE] > 0 && !counts[BOON_LEARN_HALFMOON] && !counts[BOON_LEARN_MOONBLADE], "an Elite's draft: levels (Rare), no Learn cards (Common) (%d %d %d)",
          counts[BOON_LEARN_CLEAVE], counts[BOON_LEARN_HALFMOON], counts[BOON_LEARN_MOONBLADE]);
    // Temper raises a known skill below Lv 4, never gives a form; the Pool the same
    { int8_t t[LIST_MAX]; int n = temper_targets(t, LIST_MAX); bool cl = false, other = false, form = false;
      for (int i = 0; i < n; i++) { cl |= t[i] == BOON_LEARN_CLEAVE; other |= t[i] == BOON_LEARN_HALFMOON || t[i] == BOON_LEARN_MOONBLADE; form |= is_form(t[i]); }
      CHECK(cl && !other && !form, "Temper: the known skill's next level, no unknown skill, no form"); }
    take_boon(BOON_LEARN_CLEAVE); take_boon(BOON_LEARN_CLEAVE);
    { int8_t t[LIST_MAX]; int n = temper_targets(t, LIST_MAX); bool cl = false, form = false;
      for (int i = 0; i < n; i++) { cl |= t[i] == BOON_LEARN_CLEAVE; form |= is_form(t[i]); }
      int p[BOON_COUNT];
      CHECK(!cl && !form && !pool_targets(false, p), "Lv 4: nothing to Temper or sharpen at the Pool (its forms come in a draft)"); }
    // the Moonlit Pool's +1 level: a real level
    RUN(303); take_boon(BOON_LEARN_MOONBLADE);
    { int p[BOON_COUNT]; int n = pool_targets(false, p);
      CHECK(n == 1 && p[0] == BOON_LEARN_MOONBLADE, "the Pool: +1 level on the known skill (%d)", n);
      take_boon((BoonId)p[0]);
      CHECK(tower_skill_level(SKILL_MOON_BLADE_DANCE) == 2, "the Pool's level: Moon Blade Dance Lv 2 (%d)", tower_skill_level(SKILL_MOON_BLADE_DANCE)); }
    // the B forms at the Shrine: 200 Embers each, bits 6-8 of the unlocks, read at a run's start; an old save has none
    {
        TowerSave was, t; tower_load(&was); t = was;
        t.unlocks = 0x3F; t.embers = 450; save_controller_set_tower(&t);       // all packs, no form (a save before stage 5)
        int fp = family_packs(), n = tower_pack_count();
        bool none = true; for (int k = 0; k < SKILL_COUNT; k++) none &= !tower_pack_owned(fp + k) && tower_pack_cost(fp + k) == FORM_COST;
        tower_begin_run(304);
        CHECK(n == fp + SKILL_COUNT && none && !s.forms && !strcmp(tower_pack_name(fp), "Shattering Dance") && !strcmp(tower_pack_name(fp + 2), "Long Cleave"),
              "the Shrine: %d packs then the 3 B forms at %d, none owned on an older save", fp, FORM_COST);
        bool b1 = tower_pack_buy(fp + 1), b2 = tower_pack_buy(fp + 1), b0 = tower_pack_buy(fp), b3 = tower_pack_buy(fp + 2);
        tower_load(&t);
        CHECK(b1 && !b2 && b0 && !b3 && t.unlocks == (0x3F | 1 << 7 | 1 << 6) && t.embers == 50 && tower_pack_owned(fp + 1) && tower_pack_cost(fp + 1) < 0,
              "Full Moon then Shattering Dance bought (200 each, once), Long Cleave refused at 50 (unlocks %03x, Embers %d)", t.unlocks, t.embers);
        tower_begin_run(305);
        CHECK(s.forms == 3 && (s.packs & 0x3F) == 0x3F, "a run reads them (forms %x)", s.forms);
        save_controller_set_tower(&was);
    }
    // the stagger bonuses add, +100% at most
    RUN(306);
    float e0 = tower_mod_stagger_damage(TOWER_HIT_SWORD);
    take_boon(BOON_EXECUTIONER); float e1 = tower_mod_stagger_damage(TOWER_HIT_SWORD);
    s.bless[PB_STAGGERED] = 1; float e2 = tower_mod_stagger_damage(TOWER_HIT_SWORD);
    s.bless[PB_STAGGERED] = 3; float e3 = tower_mod_stagger_damage(TOWER_HIT_SWORD);
    s.bless[PB_STAGGERED] = 0; s.stacks[BOON_ECLIPSE] = 1; float e4 = tower_mod_stagger_damage(TOWER_HIT_SKILL), e5 = tower_mod_stagger_damage(TOWER_HIT_SWORD);
    s.stacks[BOON_EXECUTIONER] = 0; float e6 = tower_mod_stagger_damage(TOWER_HIT_SKILL);
    s.bless[PB_STAGGERED] = 2; float e7 = tower_mod_stagger_damage(TOWER_HIT_SKILL), e8 = tower_mod_damage_vs(1.0f, true, TOWER_HIT_SKILL);
    debugf("[skills] stagger damage: none %.2f, Executioner %.2f, +Breaker %.2f, +3 Breakers %.2f (capped), Executioner+Eclipse a skill %.2f (capped) a sword hit %.2f, Eclipse alone %.2f, Eclipse+2 Breakers %.2f\n",
           (double)e0, (double)e1, (double)e2, (double)e3, (double)e4, (double)e5, (double)e6, (double)e7);
    CHECK(e0 == 1.0f && fabsf(e1 - 1.6f) < 1e-4f && fabsf(e2 - 1.75f) < 1e-4f && e3 == 2.0f && e4 == 2.0f && fabsf(e5 - 1.6f) < 1e-4f && fabsf(e6 - 1.5f) < 1e-4f && fabsf(e7 - 1.8f) < 1e-4f && e8 == 1.0f,
          "the bonuses add: 1, 1.6, 1.75, 3 Breakers 2.0, a skill with Executioner and Eclipse 2.0, its sword hit 1.6, Eclipse 1.5, +2 Breakers 1.8; none multiply in damage_vs");
    // the pick math (section 14): a skill build takes every skill card of one skill; a sword build learns
    // one skill and takes nothing else of them: their skill cards by floor 35 (33 drafts), 40 runs each
    for (int build = 0; build < 2; build++) {
        int offered = 0, lv5at = 0, lv5n = 0, lv4 = 0;
        for (int run = 0; run < 40; run++) {
            RUN(400 + run); s.forms = 7;
            int focus = -1, got5 = 0;
            for (int d = 0; d < 33; d++) {
                tower_draft_open();
                int take = -1;
                for (int i = 0; i < s.nChoices; i++) {
                    int c = s.choices[i];
                    if (c >= BOON_COUNT || !is_skill_card(c)) continue;
                    offered++;
                    int k = is_form(c) ? form_skill(c) : c - BOON_LEARN_MOONBLADE;
                    if (take < 0 && (focus < 0 || (build == 0 && k == focus))) take = i;
                }
                if (take < 0) for (int i = 0; i < s.nChoices && take < 0; i++) if (s.choices[i] >= BOON_COUNT || !is_skill_card(s.choices[i])) take = i;
                if (take >= 0 && focus < 0 && s.choices[take] < BOON_COUNT && is_learn(s.choices[take])) focus = s.choices[take] - BOON_LEARN_MOONBLADE;
                tower_draft_pick(take >= 0 ? take : s.nChoices);
                if (focus >= 0 && !got5 && tower_skill_level(focus) == 5) { got5 = d + 2; lv5at += got5; lv5n++; }
            }
            if (focus >= 0 && tower_skill_level(focus) >= 4) lv4++;
        }
        debugf("[skills] pick math, %s build, 40 runs of 33 drafts: %.1f skill cards offered a run (%.2f a draft), Lv 5 in %d runs (at floor %.1f), Lv 4+ in %d\n",
               build ? "sword" : "skill", (double)(offered / 40.0f), (double)(offered / 1320.0f), lv5n, (double)(lv5n ? (float)lv5at / lv5n : 0.0f), lv4);
    }
    tower_end_run();
    character.maxHealth = tower_mod_max_health(); character.health = character.maxHealth;
    #undef CHECK
    #undef RUN
    debugf("[skills] stage 5 self-check: %d failed\n", fails);
    return fails;
}

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
    { char l[TOWER_BUILD_ROWS][40], r[TOWER_BUILD_ROWS][12]; int st[TOWER_BUILD_ROWS], cd[TOWER_BUILD_ROWS];
      int n = tower_build_rows(l, r, st, cd, TOWER_BUILD_ROWS); bool duoRow = false, both = false; int lock = 0;
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
    CHECK(fabsf(tower_mod_stagger_damage(TOWER_HIT_SWORD) - 1.9f) < 0.001f, "Executioner+: +90%%");
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
      CHECK(fabsf(w - 1.5f * (6.0f + STATIC_BOLT * 3)) < 0.01f, "Static+ (Lv 4, no Storm Blade): the 8th hit's bolt x1.5 (%.1f)", (double)w); }
    RUN(27); MAX(BOON_THUNDERCLAP); take_boon(BOON_THUNDERCLAP_PLUS);
    CHECK(tower_on_sword_hit(true) == 1.5f * THUNDERCLAP_BOLT, "Thunderclap+: the finisher's bolt x1.5");
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
static int s_walkRestBack;
static int s_mapFails, s_walkShops, s_walkEvents, s_walkFightGold, s_walkEventGold;
#define MCHECK(c, ...) do { if (!(c)) { s_mapFails++; debugf("[map] FAIL: " __VA_ARGS__); debugf("\n"); } } while (0)
static bool test_press(int key)     // a press on the screens between fights, past their input delay
{
    s.scrOpenMs = s.draftOpenMs = get_ticks_ms() - DRAFT_INPUT_DELAY_MS - 1;
    return tower_between_update(key == 'L', key == 'R', key == 'U', key == 'D', key == 'A', key == 'B', key == 'r');     // ('r': the R button)
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
        // pick, then confirm: Up and Down never enter, A opens the node's page, its B the map again, its A enters;
        // the map's B asks for the pause menu, R its Details (closed by R)
        int row0 = s.map.row, sel0 = s.map.sel;
        test_press('U'); test_press('D');
        MCHECK(s.scr == SCR_MAP && s.map.row == row0, "Up and Down never enter a node (screen %d, row %d)", s.scr, s.map.row);
        test_press('B');
        MCHECK(tower_take_pause_request() && s.scr == SCR_MAP && s.map.row == row0, "the map's B: the pause menu, nothing entered");
        test_press('r');
        MCHECK(ui_details()->open && !ui_details()->confirm && s.map.row == row0, "the map's R: the node's Details");
        test_press('r');
        MCHECK(!ui_details()->open && s.scr == SCR_MAP, "R again closes them");
        test_press('A');
        MCHECK(s.scr == SCR_CONFIRM && ui_details()->open && ui_details()->confirm && ui_details()->pg.pages == 1 && s.map.row == row0,
               "A: the node's page (one page: %d), not entered", ui_details()->pg.pages);
        if (rng_next() % 3 == 0) {
            test_press('B');
            MCHECK(s.scr == SCR_MAP && !ui_details()->open && s.map.row == row0 && s.map.sel == sel0, "B on the node's page: the map, the cursor where it was");
            test_press('A');
        }
        if (!test_press('A')) {      // a Rest, a Shop or an Event
            int t = s.map.type[s.map.row][s.map.path[s.map.row]];
            MCHECK((t == NODE_REST && s.scr == SCR_REST) || (t == NODE_SHOP && s.scr == SCR_SHOP) || (t == NODE_EVENT && s.scr == SCR_EVENT),
                   "a node that isn't a fight opens its own screen (node %d, screen %d)", t, s.scr);
            int g = s.gold;
            if (t == NODE_SHOP) { test_press('B'); s_walkShops++; MCHECK(s.gold == g, "B leaves a Shop, nothing bought"); }
            else if (t == NODE_EVENT) { test_press('A'); s_walkEventGold += s.gold - g; test_press('A'); s_walkEvents++; }   // its first choice, then Continue
            else {
                if (s_walkRestBack++ % 2 == 0) {    // a Rest's B: the map, the Rest still to choose; then in again
                    test_press('B');
                    MCHECK(s.scr == SCR_MAP && s.map.row == row0 && map_reachable(row0 + 1, s.map.sel) && s.map.type[row0 + 1][s.map.sel] == NODE_REST, "a Rest's B: the map, its node to choose again");
                    test_press('A'); test_press('A');
                    MCHECK(s.scr == SCR_REST, "and A, A: the Rest again (%d)", s.scr);
                }
                test_press('A'); if (s.scr == SCR_RELEASE) test_press('A');
            }
            MCHECK(tower_map_is_open() && s.floor == floor0 + fights, "after a Rest, a Shop or an Event, the map again, no floor counted (%d)", s.floor);
            continue;
        }
        fights++;
        MCHECK(s.floor == floor0 + fights - 1 && s.map.fights == fights && is_fight(s.map.type[s.map.row][s.map.path[s.map.row]]), "a fight is the next floor (%d)", s.floor);
        if (fights == 1) MCHECK(s.boss == TOWER_GUARDIAN, "the Guardian first (%d)", s.boss);
        if (s.boss >= TOWER_FALLEN_KNIGHT) { if (nb < 3 && ((seen >> s.boss) & 1)) distinct = false; seen |= 1 << s.boss; nb++; }
        last = s.boss;
        if (drafts) {
            bool el = s.elite;
            tower_floor_cleared();
            if (ui_details()->open) {       // a milestone's notice over the draft: A closes it, the draft under it
                test_press('A');
                MCHECK(!ui_details()->open && s.draftOpen, "a milestone's notice (floor %d): A closes it, the draft under it", s.floor);
            }
            int want = (int)((GOLD_FIGHT + GOLD_PER_CIRCLE * c) * (el ? ELITE_GOLD : 1.0f) + 0.5f);
            if (!s.stacks[BOON_PROSPECTOR]) MCHECK(s.lastFloorGold == want, "a fight's gold: 25 + 5 x circle %d%s = %d (%d)", c, el ? ", x1.5 an Elite" : "", want, s.lastFloorGold);
            s_walkFightGold += s.lastFloorGold;
            test_press('A');
        }
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
        int b = 0, shop = 0;
        b += s.map.type[0][0] || s.map.type[0][2] || s.map.type[0][1] != NODE_BOSS || s.map.type[6][0] || s.map.type[6][2] || s.map.type[6][1] != NODE_BOSS;  // rows 1 and 7: one node, a boss
        for (int l = 0; l < MAP_LANES; l++) {
            int f = 0;
            b += s.map.type[5][l] != NODE_REST;                                         // row 6: a Rest on every lane
            for (int r = 1; r <= 4; r++) {
                int t = s.map.type[r][l];
                count[t]++; f += t == NODE_FIGHT;
                b += t < NODE_FIGHT || t > NODE_REST;                                   // Fight, Elite, Event, Shop or Rest
                b += r == 1 && t == NODE_ELITE;                                         // no Elite in row 2
                shop += (r == 2 || r == 3) && t == NODE_SHOP;
            }
            b += f < 2;                                                                 // 2 Fights a lane at least
        }
        b += !shop;                                                                     // a Shop in rows 3-4 on some lane
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
    int cells = 12000, np = paths[4] + paths[5] + paths[6];
    #define PCT(v, of) (v) * 100 / (of), (v) * 1000 / (of) % 10
    debugf("[map] 1000 maps: %d break a rule; rows 2-5: Fight %d.%d%%, Elite %d.%d%%, Event %d.%d%%, Shop %d.%d%%, Rest %d.%d%%; %d.%d rolls a map (most %d)\n",
           bad, PCT(count[NODE_FIGHT], cells), PCT(count[NODE_ELITE], cells), PCT(count[NODE_EVENT], cells), PCT(count[NODE_SHOP], cells), PCT(count[NODE_REST], cells),
           tries / 1000, tries / 100 % 10, most);
    debugf("[map] paths (every one of every map, %d): 4 fights %d.%d%%, 5 %d.%d%%, 6 %d.%d%%\n", np, PCT(paths[4], np), PCT(paths[5], np), PCT(paths[6], np));
    #undef PCT
    MCHECK(!bad, "every map keeps the rules (%d don't)", bad);
    MCHECK(count[NODE_ELITE] && count[NODE_REST] && count[NODE_FIGHT] && count[NODE_EVENT] && count[NODE_SHOP], "all five kinds rolled");
    // the generator's own check turns each rule down
    // the generator's own check turns each rule down, one at a time on a map of Fights
    { int no = 0;
      #define FIGHTS() do { for (int r = 1; r <= 4; r++) for (int l = 0; l < MAP_LANES; l++) s.map.type[r][l] = NODE_FIGHT; s.map.type[3][2] = NODE_SHOP; } while (0)
      FIGHTS(); bool base = map_rules_ok();
      s.map.type[1][0] = s.map.type[3][0] = s.map.type[4][0] = NODE_ELITE; no += !map_rules_ok(); FIGHTS();   // a lane with 1 Fight
      s.map.type[1][2] = NODE_ELITE; no += !map_rules_ok(); FIGHTS();                                         // an Elite in row 2
      s.map.type[4][0] = NODE_REST; no += !map_rules_ok(); FIGHTS();                                          // a Rest under row 6's
      s.map.type[2][1] = s.map.type[3][0] = NODE_REST; no += !map_rules_ok(); FIGHTS();                       // a Rest, then a diagonal Rest
      s.map.type[3][2] = NODE_FIGHT; s.map.type[1][2] = NODE_SHOP; no += !map_rules_ok(); FIGHTS();           // no Shop in rows 3-4 (one in row 2)
      s.map.type[2][1] = NODE_SHOP; no += !map_rules_ok(); FIGHTS();                                          // a Shop, then a diagonal Shop
      s.map.type[1][0] = s.map.type[3][0] = s.map.type[2][1] = s.map.type[4][1] = NODE_EVENT; no += !map_rules_ok(); FIGHTS();   // a zigzag with no fight
      #undef FIGHTS
      MCHECK(base && no == 7, "map_rules_ok turns down each broken rule (%d of 7)", no); }
    // 2. walks: whole circles by the screens' presses, with drafts (their Embers and best floor put back after)
    TowerSave was; tower_load(&was);
    int hist[8] = { 0 };
    s_walkShops = s_walkEvents = s_walkFightGold = s_walkEventGold = 0;
    for (uint32_t seed = 1; seed <= 40; seed++) {
        tower_begin_run(seed);
        character.maxHealth = tower_mod_max_health(); character.health = character.maxHealth; character.healthPotions = START_POTIONS;
        tower_map_open();
        for (int c = 0; c < 2; c++) hist[walk_circle(true)]++;
    }
    debugf("[map] 80 circles walked: %d with 4 fights, %d with 5, %d with 6; %d Shops and %d Events entered; fights' gold %d a circle (circles 1-2), events' %d in all\n",
           hist[4], hist[5], hist[6], s_walkShops, s_walkEvents, s_walkFightGold / 80, s_walkEventGold);
    save_controller_set_tower(&was);
    // 2b. every node's confirm page fits one page: each Elite with every pair of affixes on each Bonus boss (and as
    // the jailer), each boss node under each night; and every card's Details page count (they page)
    {
        UiPanel *dp = ui_details(); char t[64]; int st, worst = 0, cards = 0, most = 0, mostCard = 0, cardBad = 0, noText = 0;
        text_load();
        for (int k = 0; k < TXT_COUNT; k++) if (!full_text(k)[0] && noText++ < 3) debugf("[map] FAIL: rom:/ui/tower_text.bin has no text %d\n", k);
        MCHECK(!noText && s_text, "every card's and affix's Details text is in ROM (%d missing)", noText);
        tower_begin_run(9); s.circle = AFX_TWO_CIRCLE; s.map.row = 0; s.map.path[0] = 1; s.floor = 123; s.gold = 99999;
        for (int pr = 0; pr < 2; pr++) for (int b = TOWER_FALLEN_KNIGHT; b <= TOWER_SHADOWBORN; b++)
            for (int a = 0; a < AFX_COUNT; a++) for (int c = a + 1; c < AFX_COUNT; c++) {
                s.prisoner = pr; s.map.type[1][1] = pr ? NODE_FIGHT : NODE_ELITE; s.map.boss[1][1] = (uint8_t)(b + 1); s.map.afx[1][1] = (uint8_t)((1 << a) | (1 << c));
                ui_doc_clear(ui_panel_doc(dp)); doc_node(dp->doc, 1, 1, true, t, sizeof t, &st); ui_panel_open(dp, t, st, true);
                if (dp->pg.pages > 1 && worst++ < 3) debugf("[map] FAIL: %s takes %d pages\n", t, dp->pg.pages);
            }
        s.prisoner = false;
        for (int n = 1; n < NIGHT_COUNT; n++) for (int r = 0; r < MAP_ROWS; r += MAP_ROWS - 1) {
            s.night = (Night)n; s.map.row = r - 1;
            for (int ty = NODE_FIGHT; ty <= NODE_BOSS; ty++) {
                int rr = r ? 3 : r; if (ty != NODE_BOSS) { s.map.type[rr][1] = (uint8_t)ty; s.map.boss[rr][1] = TOWER_SHACKLED_SUN + 1; s.map.afx[rr][1] = 0x30; } else rr = r;
                ui_doc_clear(ui_panel_doc(dp)); doc_node(dp->doc, rr, 1, true, t, sizeof t, &st); ui_panel_open(dp, t, st, true);
                if (dp->pg.pages > 1 && worst++ < 6) debugf("[map] FAIL: %s under %s takes %d pages\n", t, NIGHTS[n].name, dp->pg.pages);
            }
        }
        s.packs = (1 << FAM_COUNT) - 1;
        for (int b = 0; b < BOON_COUNT; b++) { details_card(b); cards++; if (dp->pg.pages > most) { most = dp->pg.pages; mostCard = b; } }
        // every card's text fits its card: the draft's (name, text, then its bottom lines up from the foot) and the Shop's
        // (name, text, then the price): the lines as drawn (tower_draft_draw, shop_draw)
        for (int b = 0; b < BOON_COUNT; b++) for (int lv = 0; lv < (is_learn(b) ? SKILL_LV_MAX : 1); lv++) {
            const BoonDef *bd = &BOONS[b];
            const char *desc = is_learn(b) ? (lv ? SKILL_LV_DESC[b - BOON_LEARN_MOONBLADE][lv - 1] : "Learn it: C-right") : bd->desc;
            int nl = ui_text_lines(bd->name, 88), dl = ui_text_lines(desc, 84), descEnd = 31 + 17 * (nl > 1 ? nl : 1) + 17 * (dl - 1);
            int nb = 1, extra = 0;
            if (bd->duo) nb = 3;
            else if (is_form(b)) { nb = 2; extra = ui_text_lines(BOONS[rival_of(b)].name, 98) - 1; }
            else if (rival_of(b) >= 0) { char rn[48]; rival_names(b, rn, sizeof rn); nb = 3; extra = ui_text_lines(rn, 98) - 1; }
            int topBottom = DRAFT_CARD_H - 3 - 12 * (nb - 1) - 17 * extra;
            if (descEnd + 16 > topBottom && cardBad++ < 30) debugf("[map] FAIL: draft card %s%s: its text ends at %d, its bottom lines start at %d\n", bd->name, lv ? " (a level)" : "", descEnd, topBottom);
            if (!is_skill_card(b) && !bd->up && descEnd + 15 > 100 - 3 && cardBad++ < 30) debugf("[map] FAIL: shop card %s: its text ends at %d, the price at 97\n", bd->name, descEnd);
        }
        // every one-line text inside its room: the map's lines and the screens' rows, with 5-digit gold, floor 123, Ascension 19
        {
            int wide = 0; char ln[128];
            #define FIT(w, ...) do { snprintf(ln, sizeof ln, __VA_ARGS__); if (ui_text_width(ln) > (w) && wide++ < 20) debugf("[map] FAIL: too wide (%d > %d): %s\n", ui_text_width(ln), (w), ln); } while (0)
            FIT(272, "Floor %d   Ascension %d   Gold %d", 123, 19, 99999);
            FIT(272, "Floor %d cleared   +%d Embers   +%d gold", 123, 999, 999);
            FIT(272, "Choose a Boon   Next: floor %d", 124);
            FIT(160, "Skip: +%d Embers, +%d gold", SKIP_EMBERS, 99);
            FIT(272, "Shop   Gold %d", 99999);
            FIT(272, "Health %d/%d   Potions %d/%d", 999, 999, 15, 15);
            FIT(272, "Gold %d   Health %d/%d   Potions %d", 99999, 999, 999, 15);
            for (int n = 1; n < NIGHT_COUNT; n++) { FIT(272, "%s: %s", NIGHTS[n].name, NIGHTS[n].brief); FIT(272, "Next: floor %d, a new circle: %s", 124, NIGHTS[n].name); }
            for (int b = TOWER_FALLEN_KNIGHT; b <= TOWER_SHADOWBORN; b++) for (int a = 0; a < AFX_COUNT; a++) for (int c = a + 1; c < AFX_COUNT; c++)
                FIT(272, "Elite: %s %s %s", AFFIXES[a].name, AFFIXES[c].name, BOSS_NAMES[b]);
            for (int pb = 0; pb < PB_COUNT; pb++) for (int pc = 0; pc < PC_COUNT; pc++) FIT(272, "Blessing %s, its curse %s", BLESSINGS[pb].name, CURSES[pc].name);
            for (int i = 0; i < SHOP_ITEMS; i++) { s.gold = 0; FIT(272, "%s", shop_block(i)); }
            for (int b = 0; b < BOON_COUNT; b++) {      // the Shop's line under Leave: a card's rivals, a duo's two longest boons
                if (rival_of(b) >= 0) { char rn[48]; rival_names(b, rn, sizeof rn); FIT(272, "New: slot %d/%d. Locks out %s", 5, 5, rn); }
                if (!BOONS[b].duo) continue;
                const char *na = "", *nb = "";
                for (int k = 0; k < BOON_COUNT; k++) if (takes_slot(k) && !BOONS[k].duo) {
                    if (BOONS[k].family == BOONS[b].family && strlen(BOONS[k].name) > strlen(na)) na = BOONS[k].name;
                    if (BOONS[k].family == BOONS[b].duo - 1 && strlen(BOONS[k].name) > strlen(nb)) nb = BOONS[k].name;
                }
                FIT(272, "Duo: merges %s + %s", na, nb);
            }
            for (int b = 0; b < BOON_COUNT; b++) { FIT(220, "%s (top level)", BOONS[b].name); FIT(220, "%s   Lv %d -> %d", BOONS[b].name, 4, 5); FIT(220, "%s   Duo, with its two", BOONS[b].name); }
            for (int e = 0; e < EV_COUNT; e++) for (int i = 0; i < EVENTS[e].choices; i++) { s.event = (int8_t)e; character.maxHealth = 999; character.health = 1; char lb[96]; event_label(i, lb, sizeof lb); FIT(220, "%s", lb); }
            FIT(220, "Heal 40%%: +%d health", 999); FIT(220, "Temper: +1 level on a boon or skill");
            #undef FIT
            MCHECK(!wide, "every one-line text fits its room (%d don't)", wide);
            s.gold = 99999;
        }
        ui_panel_close(dp);
        text_free();
        debugf("[ui] pitch %d; widths: %d (alphabet a-z), %d (Enter: Elite Relentless Smouldering Shackled Sun), %d (an Elite's reward line)\n", ui_line_pitch(), ui_text_width("abcdefghijklmnopqrstuvwxyz"),
               ui_text_width("Enter: Elite Relentless Smouldering Shackled Sun"), ui_text_width("x1.25 health. Reward: a Rare-or-better draft (a duo if one is ready), x1.5 gold (75) and Embers."));
        debugf("[map] node pages: %d too long; %d cards' Details, the longest %d pages (%s)\n", worst, cards, most, BOONS[mostCard].name);
        MCHECK(!worst && most <= 4 && !cardBad, "every node's page fits one page (%d don't), every card's Details 4 pages at most (%d), every card's text fits it (%d don't)", worst, most, cardBad);
    }
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
    MCHECK(s.scr == SCR_TEMPER && s.listN == 4 && s.list[0] == BOON_EMBER_EDGE && s.list[1] == BOON_LEARN_HALFMOON && s.list[2] == BOON_STORM_BLADE_PLUS && s.list[3] == -1,
           "Temper's list: Ember Edge's level, Halfmoon Slash's Lv 2, Storm Blade+ (a maxed boon's \"+\"), Back (%d rows)", s.listN);
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

// Stage 4's self-checks (HLE_TEST_TOWER): gold, the Shop (each item by the shelf's presses, prices,
// gold never below 0, B leaves) and the Events (every choice's result, the unlocks); return the failures
#define RUN4(seed) do { tower_begin_run(seed); s.packs = (1 << FAM_COUNT) - 1; s.relicFx[0] = s.relicFx[1] = 0; character.maxHealth = tower_mod_max_health(); \
                        character.health = character.maxHealth; character.healthPotions = START_POTIONS; } while (0)
static int stacks_sum(void) { int n = 0; for (int b = 0; b < BOON_COUNT; b++) n += s.stacks[b]; return n; }
static int curses_sum(void) { int n = 0; for (int c = 0; c < PC_COUNT; c++) n += s.curse[c]; return n; }
int tower_test_shop(void)
{
    int fails = 0;
    #define CHECK(c, ...) do { if (!(c)) { fails++; debugf("[shop] FAIL: " __VA_ARGS__); debugf("\n"); } } while (0)
    TowerSave was; tower_load(&was);
    // gold from fights: 25 + 5 x circle, an Elite x1.5, Skip +15, the purse +40, Prospector +20% a level; lost with the run
    RUN4(51);
    s.map.row = 1; s.map.fights = 2;
    tower_floor_cleared(); test_unnotice(); int g1 = s.lastFloorGold; s.draftOpen = false;
    s.elite = true; tower_floor_cleared(); test_unnotice(); int ge = s.lastFloorGold; s.draftOpen = false; s.elite = false;
    s.circle = 2; tower_floor_cleared(); test_unnotice(); int g2 = s.lastFloorGold; s.draftOpen = false;
    s.elite = true; tower_floor_cleared(); test_unnotice(); int ge2 = s.lastFloorGold; s.draftOpen = false; s.elite = false; s.circle = 1;
    int before = s.gold; s.nChoices = 3; tower_draft_pick(3); int skip = s.gold - before;
    for (int i = 0; i < 3; i++) take_boon(BOON_PROSPECTOR);
    tower_floor_cleared(); test_unnotice(); int gp = s.lastFloorGold; s.draftOpen = false;
    before = s.gold; s.nChoices = 3; tower_draft_pick(3); int skipP = s.gold - before;
    debugf("[shop] gold: a fight %d (circle 1), an Elite %d, circle 2 %d / %d, Skip +%d; Prospector Lv 3: a fight %d, Skip +%d; %d held\n", g1, ge, g2, ge2, skip, gp, skipP, s.gold);
    CHECK(g1 == 30 && ge == 45 && g2 == 35 && ge2 == 53 && skip == 15 && gp == 48 && skipP == 24, "gold by the numbers");
    // a complete build's draft: the purse card (+40) and pacts
    for (int b = 0; b < BOON_COUNT; b++) s.locked[b] = true;
    tower_draft_open();
    { int purse = 0, k = -1; for (int i = 0; i < s.nChoices; i++) if (s.choices[i] == GOLD_CARD) { purse++; k = i; }
      before = s.gold; if (k >= 0) tower_draft_pick(k);
      CHECK(purse == 1 && s.gold - before == gold_with(GOLD_CARD_GOLD), "no boon left: one purse card (%d), +40 gold (x Prospector: +%d)", purse, s.gold - before); }
    s.draftOpen = false;
    RUN4(52);
    CHECK(s.gold == 0, "a new run starts with no gold (%d)", s.gold);
    // the Shop by its presses: a level (Vigor), a "+" (Storm Blade's), boons to release
    RUN4(53);
    take_boon(BOON_STORM_BLADE); take_boon(BOON_VIGOR); take_boon(BOON_EMBER_EDGE);
    s.gold = 1000; character.health = 50.0f; character.healthPotions = 1;
    shop_open();
    CHECK(s.scr == SCR_SHOP && s.shop.sel == 0, "the Shop opens on its first card");
    { bool ok = true; for (int i = 0; i < 3; i++) { int c = s.shop.card[i]; ok &= c >= 0 && shop_card_ok(c) && shop_price(i) == TIER_PRICE[BOONS[c].tier];
                                                    for (int j = 0; j < i; j++) ok &= c != s.shop.card[j] && rival_of(c) != s.shop.card[j]; }
      CHECK(ok, "three different new boons (no skill, no \"+\", no rivals together), each at its tier's price");
      CHECK(shop_price(SHOP_LEVEL) == 70 && shop_price(SHOP_PLUS) == 150 && shop_price(SHOP_PACT) == 0 && shop_price(SHOP_POTION) == 45
            && shop_price(SHOP_MEND) == 50 && shop_price(SHOP_RELEASE) == 20, "the services' prices"); }
    // the shelf's moves: Right/Left along it (wrapping), Down/Up by rows, Leave under the last row
    { int seq[12], k = 0;
      test_press('L'); seq[k++] = s.shop.sel; test_press('R'); seq[k++] = s.shop.sel; test_press('R'); seq[k++] = s.shop.sel;
      test_press('D'); seq[k++] = s.shop.sel; test_press('D'); seq[k++] = s.shop.sel; test_press('D'); seq[k++] = s.shop.sel; test_press('D'); seq[k++] = s.shop.sel;
      test_press('U'); seq[k++] = s.shop.sel; test_press('U'); seq[k++] = s.shop.sel; test_press('U'); seq[k++] = s.shop.sel; test_press('U'); seq[k++] = s.shop.sel;
      CHECK(seq[0] == SHOP_LEAVE && seq[1] == 0 && seq[2] == 1 && seq[3] == 4 && seq[4] == 7 && seq[5] == SHOP_LEAVE && seq[6] == SHOP_LEAVE
            && seq[7] == SHOP_MEND && seq[8] == 4 && seq[9] == 1 && seq[10] == 1, "the shelf's moves (%d %d %d %d %d %d %d %d %d %d %d)",
            seq[0], seq[1], seq[2], seq[3], seq[4], seq[5], seq[6], seq[7], seq[8], seq[9], seq[10]); }
    // buy each item
    int g = s.gold, card = s.shop.card[1], st = stacks_sum();
    test_press('A');
    CHECK(s.gold == g - TIER_PRICE[BOONS[card].tier] && s.stacks[card] == 1 && slots_used() == 4 && (s.shop.sold & 7) == 7, "a card: its price, the boon slotted, all three gone");
    g = s.gold; test_press('L'); test_press('A');
    CHECK(s.gold == g && stacks_sum() == st + 1, "another card after it: sold, nothing paid");
    s.shop.sel = SHOP_LEVEL; test_press('A');
    CHECK(s.scr == SCR_SHOP_LEVEL && s.listN >= 3 && s.list[0] == BOON_VIGOR && s.list[1] == BOON_EMBER_EDGE && s.list[s.listN - 1] == -1, "a level: the slotted boons below their max, then Back (%d rows)", s.listN);
    test_press('B');
    CHECK(s.scr == SCR_SHOP && s.gold == g, "B in its list: back to the shelf, nothing paid");
    test_press('A'); test_press('A');
    CHECK(s.scr == SCR_SHOP && s.gold == g - 70 && s.stacks[BOON_VIGOR] == 2 && ((s.shop.sold >> SHOP_LEVEL) & 1), "a level on Vigor: 70, Lv 2, sold");
    g = s.gold; test_press('A');
    CHECK(s.gold == g && s.scr == SCR_SHOP, "a second level this visit: sold, nothing paid");
    test_press('R'); test_press('A');
    CHECK(s.scr == SCR_SHOP_PLUS && s.list[0] == BOON_STORM_BLADE_PLUS, "a \"+\": Storm Blade+ on its list");
    test_press('A');
    CHECK(s.gold == g - 150 && s.stacks[BOON_STORM_BLADE_PLUS] == 1, "Storm Blade+ for 150");
    g = s.gold; int cu = curses_sum(), bs = 0; for (int b = 0; b < PB_COUNT; b++) bs += s.bless[b];
    test_press('R'); test_press('A');
    { int bs2 = 0; for (int b = 0; b < PB_COUNT; b++) bs2 += s.bless[b];
      CHECK(s.gold == g && bs2 == bs + 1 && curses_sum() == cu + 1, "the pact: free, a blessing and its curse"); }
    int pmax = tower_mod_max_potions();
    test_press('R'); g = s.gold; int p0 = character.healthPotions;
    for (int i = 0; i < 6; i++) test_press('A');
    CHECK(character.healthPotions == pmax && s.gold == g - 45 * (pmax - p0), "potions: 45 each up to the run's %d (%d, paid %d)", pmax, character.healthPotions, g - s.gold);
    test_press('R'); g = s.gold; float h = character.health;
    test_press('A'); test_press('A');
    CHECK(s.gold == g - 50 && fabsf(character.health - fminf(character.maxHealth, h + 0.35f * character.maxHealth)) < 0.01f, "Mend: 50, +35%% max health once (%.0f -> %.0f)", (double)h, (double)character.health);
    test_press('R'); g = s.gold; test_press('A');
    CHECK(s.scr == SCR_SHOP_RELEASE && s.listN == slots_used() + 1 && s.list[s.listN - 1] == -1, "Release: the slots, then Back");
    test_press('A');
    CHECK(s.gold == g - 20 && !s.stacks[BOON_STORM_BLADE] && !s.stacks[BOON_STORM_BLADE_PLUS] && s.locked[BOON_EXECUTIONER] && slots_used() == 3,
          "Release Storm Blade: 20, it and its \"+\" gone, Executioner still locked");
    test_press('R');
    CHECK(s.shop.sel == SHOP_LEAVE, "Leave last on the shelf");
    // gold never below 0: every item pressed with little gold
    RUN4(54); take_boon(BOON_VIGOR); character.health = 50.0f; character.healthPotions = 0;
    { int least = 1 << 30, refused = 0;
      for (int g0 = 0; g0 <= 75; g0 += 15) {
          s.gold = g0; shop_open();
          for (int i = 0; i < SHOP_LEAVE; i++) {
              s.scr = SCR_SHOP; s.shop.sel = (int8_t)i; int gb = s.gold;
              test_press('A');
              if (s.scr != SCR_SHOP) test_press('A');     // a list: its first row
              if (s.gold == gb) refused++;
              if (s.gold < least) least = s.gold;
              if (s.scr != SCR_SHOP) { s.scr = SCR_SHOP; }
          }
      }
      debugf("[shop] 6 visits with 0-75 gold, every item pressed: lowest gold %d, %d presses refused\n", least, refused);
      CHECK(least >= 0, "gold never below 0 (%d)", least); }
    // B leaves (the map, nothing bought), from the shelf
    s.gold = 300; shop_open(); g = s.gold; test_press('B');
    CHECK(tower_map_is_open() && s.gold == g, "B leaves the Shop: the map, gold kept");
    shop_open(); s.shop.sel = SHOP_LEAVE; test_press('A');
    CHECK(tower_map_is_open(), "Leave (A): the map");
    // a full bar: the cards hold no new boon (a duo only when one is ready)
    RUN4(55);
    take_boon(BOON_VIGOR); take_boon(BOON_EMBER_EDGE); take_boon(BOON_FLEETFOOT); take_boon(BOON_RIPOSTE); take_boon(BOON_RALLY);
    shop_open();
    CHECK(s.shop.card[0] < 0 && s.shop.card[1] < 0 && s.shop.card[2] < 0 && !strcmp(shop_block(0), "No new boon fits"), "a full bar: no card fits");
    while (s.stacks[BOON_RALLY] < 2) take_boon(BOON_RALLY);
    shop_open();
    CHECK(s.shop.card[0] == BOON_SHADES_MERCY && s.shop.card[1] < 0 && shop_price(0) == 180, "a full bar with a duo ready: the duo, 180 (%d)", s.shop.card[0]);
    // the cards' family weighting, as a draft's: two Storm boons slotted, a Storm Rare about twice as often
    RUN4(56); take_boon(BOON_STORM_BLADE); take_boon(BOON_STATIC);
    { int counts[BOON_COUNT] = { 0 }; float storm = 0, other = 0; int ns = 0, no = 0;
      for (int k = 0; k < 3000; k++) { shop_open(); for (int i = 0; i < 3; i++) if (s.shop.card[i] >= 0) counts[s.shop.card[i]]++; }
      for (int b = 0; b < BOON_COUNT; b++) if (BOONS[b].tier == TIER_RARE && shop_card_ok(b)) { if (BOONS[b].family == FAM_STORM) { storm += counts[b]; ns++; } else { other += counts[b]; no++; } }
      float r = (ns && no && other > 0) ? (storm / ns) / (other / no) : 0.0f;
      debugf("[shop] 3000 visits, Storm Blade + Static slotted: a Storm Rare card %.2fx as often as another Rare\n", (double)r);
      CHECK(r > 1.6f && r < 2.4f, "the cards' family weighting (%.2f)", (double)r); }
    s.scr = SCR_NONE;
    tower_end_run();
    save_controller_set_tower(&was);
    character.maxHealth = tower_mod_max_health(); character.health = character.maxHealth;
    #undef CHECK
    debugf("[shop] self-check: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    return fails;
}

int tower_test_events(void)
{
    int fails = 0;
    #define CHECK(c, ...) do { if (!(c)) { fails++; debugf("[event] FAIL: " __VA_ARGS__); debugf("\n"); } } while (0)
    TowerSave was, t; tower_load(&was);
    bool skillsWere = character_skills_enabled();
    character_set_skills_enabled(true);
    // the unlocks: best floor 0 rolls events 1-4, 10 adds the 5th ... 25 all 8; no repeat in a run until all are met
    static const int BEST[5] = { 0, 10, 15, 20, 25 };
    for (int k = 0; k < 5; k++) {
        t = was; t.bestFloor = BEST[k]; save_controller_set_tower(&t);
        RUN4(70 + k);
        uint8_t seenAll = 0; bool repeat = false;
        for (int n = 0; n < 4 + k; n++) { event_open(); repeat |= (seenAll >> s.event) & 1; seenAll |= 1 << s.event; }
        uint8_t first = seenAll;
        for (int n = 0; n < 200; n++) { event_open(); seenAll |= 1 << s.event; }
        debugf("[event] best floor %d: the first %d events %02x (no repeat %d), 200 more %02x\n", BEST[k], 4 + k, first, !repeat, seenAll);
        CHECK(!repeat && first == (1 << (4 + k)) - 1 && seenAll == first, "best floor %d: events 1-%d, no repeat in a run until all are met", BEST[k], 4 + k);
    }
    save_controller_set_tower(&was);
    // every choice of every event, by its screen's presses
    for (int e = 0; e < EV_COUNT; e++) for (int i = 0; i < EVENTS[e].choices; i++) {
        RUN4(80 + 3 * e + i);
        take_boon(BOON_STORM_BLADE); take_boon(BOON_EMBER_EDGE); take_boon(BOON_LEARN_HALFMOON);
        s.gold = 200; character.health = 60.0f; character.healthPotions = 4;
        s.event = (int8_t)e; s.eventPick = PB_MIGHT; s.eventDone = false; list_open(SCR_EVENT);
        int g = s.gold, p = character.healthPotions, st = stacks_sum(), cu = curses_sum(), em = s.runEmbers, sk = s.skills;
        float mh = character.maxHealth, hp = character.health, dd = tower_mod_damage_dealt();
        for (int k = 0; k < 4 && s.listSel < i; k++) test_press('D');
        test_press('A');
        bool leave = i == EVENTS[e].choices - 1, ok = s.eventDone && s.listN == 1;
        if (leave) ok &= s.gold == g && character.healthPotions == p && stacks_sum() == st && curses_sum() == cu && character.maxHealth == mh && character.health == hp && !s.prisoner;
        else switch (e) {
        case EV_ALTAR:    ok &= fabsf(character.maxHealth - 0.85f * mh) < 0.01f && stacks_sum() == st + 1; break;
        case EV_GRAVE:    ok &= i == 0 ? s.gold == g + 60 : fabsf(character.health - hp - 0.4f * mh) < 0.01f; break;
        case EV_MIRROR:   ok &= stacks_sum() == st + 1 && curses_sum() == cu + 1; break;
        case EV_MERCHANT: ok &= i == 0 ? s.gold == g - 70 && character.healthPotions == p + 1 : character.healthPotions == p - 2 && stacks_sum() == st + 1; break;
        case EV_POOL:     ok &= i == 0 ? s.skills != sk && stacks_sum() == st + 1 : s.stacks[BOON_LEARN_HALFMOON] == 2; break;
        case EV_PRISONER: ok &= s.prisoner; break;
        case EV_BONES:    ok &= s.gold == g - 50 || s.gold == g + 70; break;
        case EV_BRAZIER:  ok &= i == 0 ? s.runEmbers == em + 30 : s.halfBless[PB_MIGHT] == 1 && fabsf(tower_mod_damage_dealt() / dd - 1.04f) < 0.001f; break;
        }
        debugf("[event] %s, choice %d: \"%s\" -> %s\n", EVENTS[e].name, i + 1, s.eventResult, ok ? "ok" : "FAIL");
        CHECK(ok, "%s, choice %d", EVENTS[e].name, i + 1);
        test_press('A');
        CHECK(tower_map_is_open(), "Continue: the map");
    }
    save_controller_set_tower(&was);
    // the jailer: the next Fight an Elite (x1.25, x1.5 gold), its draft all Legendary; an Elite node or a boss doesn't take it
    RUN4(100);
    s.prisoner = true;
    s.map.type[1][1] = NODE_ELITE; s.map.row = 0; s.map.path[0] = 1; s.map.sel = 1; map_enter();
    CHECK(s.prisoner && !s.legendDraft, "an Elite node isn't the jailer's");
    s.map.type[2][1] = NODE_FIGHT; s.map.boss[2][1] = TOWER_SHACKLED_SUN + 1; s.map.sel = 1; map_enter();
    { int counts[BOON_COUNT], other = 0, cards = 0;
      bool el = s.elite && s.legendDraft && !s.prisoner;
      draft_counts(300, counts);
      for (int b = 0; b < BOON_COUNT; b++) { cards += counts[b]; if (BOONS[b].tier != TIER_LEGENDARY) other += counts[b]; }
      s.map.fights = 2; s.map.row = 2;
      tower_floor_cleared(); test_unnotice(); s.draftOpen = false;
      debugf("[event] the jailer's fight: Elite %d, 300 drafts %d boon cards, %d not Legendary; gold %d\n", el, cards, other, s.lastFloorGold);
      CHECK(el && cards > 0 && !other && s.lastFloorGold == 45, "the freed Prisoner: the next Fight an Elite, an all-Legendary draft, x1.5 gold"); }
    s.map.type[3][1] = NODE_FIGHT; s.map.boss[3][1] = TOWER_FALLEN_KNIGHT + 1; s.map.sel = 1; map_enter();
    CHECK(!s.elite && !s.legendDraft, "the Fight after it: plain again");
    // Gambler's Bones: half the time (the run seed)
    { int wins = 0;
      for (int k = 0; k < 400; k++) { RUN4(200 + k); s.gold = 50; s.event = EV_BONES; s.eventDone = false; list_open(SCR_EVENT); test_press('A'); wins += s.gold == 120; }
      debugf("[event] Gambler's Bones: %d wins in 400\n", wins);
      CHECK(wins > 160 && wins < 240, "Gambler's Bones wins about half the time (%d of 400)", wins); }
    // greyed choices: the cursor passes them; A on one does nothing
    RUN4(101); s.gold = 30; s.event = EV_BONES; s.eventDone = false; list_open(SCR_EVENT);
    CHECK(!row_ok(0) && s.listSel == 1, "Bones with 30 gold: the bet greyed, the cursor on Leave");
    RUN4(102); character.healthPotions = 1; s.event = EV_MERCHANT; s.eventDone = false; list_open(SCR_EVENT);
    CHECK(!row_ok(0) && !row_ok(1) && s.listSel == 2, "the Merchant with no gold and 1 potion: only Leave");
    RUN4(103); s.event = EV_GRAVE; s.eventDone = false; list_open(SCR_EVENT);
    CHECK(row_ok(0) && !row_ok(1), "the Grave at full health: Pray greyed");
    RUN4(104); s.event = EV_MIRROR; s.eventDone = false; list_open(SCR_EVENT);
    CHECK(!row_ok(0), "the Mirror with no boon: greyed");
    RUN4(105); s.skills = 7; s.event = EV_POOL; s.eventDone = false; list_open(SCR_EVENT);
    CHECK(!row_ok(0) && row_ok(1), "the Pool knowing every skill: Learn greyed");
    s.scr = SCR_NONE;
    tower_end_run();
    character.maxHealth = tower_mod_max_health(); character.health = character.maxHealth;
    character_set_skills_enabled(skillsWere);
    #undef CHECK
    debugf("[event] self-check: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    return fails;
}
#undef RUN4

// Stage 6's self-check: the affixes' rolls and unlocks, an Elite's affixes and their numbers, the duo in a
// two-affix Elite's draft, Berserker (its rivals, pack, potions, streak); returns the failures
int tower_test_affixes(void)
{
    int fails = 0;
    #define CHECK(c, ...) do { if (!(c)) { fails++; debugf("[affix] FAIL: " __VA_ARGS__); debugf("\n"); } } while (0)
    #define RUN(seed) do { tower_begin_run(seed); s.packs = (1 << FAM_COUNT) - 1; s.relicFx[0] = s.relicFx[1] = 0; character.maxHealth = tower_mod_max_health(); character.health = character.maxHealth; } while (0)
    TowerSave was, t; tower_load(&was);
    bool skillsWere = character_skills_enabled();
    character_set_skills_enabled(true);
    // 1. the rolls: one an Elite in circles 1-3, two from circle 4; Swift, Ironclad, Enraged from the start, Relentless,
    //    Vampiric, Smouldering by best floor 15, 20, 25; never Smouldering on Shadowborne
    static const int BEST[4] = { 0, 15, 20, 25 };
    for (int k = 0; k < 4; k++) {
        t = was; t.bestFloor = BEST[k]; save_controller_set_tower(&t);
        RUN(90 + k);
        int seen = 0, bad = 0, sbSm = 0, two = 0;
        for (int n = 0; n < 600; n++) {
            int boss = TOWER_FALLEN_KNIGHT + n % 3;
            s.circle = 1 + n % 6;
            uint8_t m = roll_affixes(boss);
            int want = s.circle >= AFX_TWO_CIRCLE ? 2 : 1;
            bad += __builtin_popcount(m) != want;
            two += want == 2;
            sbSm += boss == TOWER_SHADOWBORN && ((m >> AFX_SMOULDERING) & 1);
            seen |= m;
        }
        int open = (1 << (3 + k)) - 1;
        debugf("[affix] best floor %d: affixes rolled %02x (open %02x), %d with the wrong count (of 600, %d at two), Smouldering on Shadowborne %d\n", BEST[k], seen, open, bad, two, sbSm);
        CHECK(seen == open && !bad && !sbSm, "best floor %d: the affixes %02x (want %02x), counts, never Smouldering on Shadowborne", BEST[k], seen, open);
    }
    save_controller_set_tower(&was);
    // 2. an Elite's node: its affixes in the fight (named for the bar and the map); a Fight none
    RUN(95);
    s.map.row = 0; s.map.path[0] = 1; s.map.type[1][0] = NODE_ELITE; s.map.type[1][2] = NODE_FIGHT;
    s.map.boss[1][0] = TOWER_SHACKLED_SUN + 1; s.map.boss[1][2] = TOWER_FALLEN_KNIGHT + 1;
    s.map.afx[1][0] = (1 << AFX_SWIFT) | (1 << AFX_IRONCLAD); s.map.afx[1][2] = 1 << AFX_ENRAGED;
    s.map.sel = 0; map_enter();
    debugf("[affix] an Elite entered: affixes %02x, \"%s\"; Swift %d Ironclad %d Enraged %d\n", s.affixes, tower_affix_tag(), tower_affix(AFX_SWIFT), tower_affix(AFX_IRONCLAD), tower_affix(AFX_ENRAGED));
    CHECK(tower_affix(AFX_SWIFT) && tower_affix(AFX_IRONCLAD) && !tower_affix(AFX_ENRAGED) && !strcmp(tower_affix_tag(), "Swift Ironclad"), "the Elite's affixes and its tag");
    // 3. their numbers
    float hp = tower_boss_health_scale(4), hpG = tower_boss_health_scale(1);
    s.affixes = 0; float hpE = tower_boss_health_scale(4); s.elite = false; float hpP = tower_boss_health_scale(4), hpGP = tower_boss_health_scale(1); s.elite = true; s.affixes = 1 << AFX_IRONCLAD;
    hpG /= hpGP;     // (the Guardian: no Elite)
    debugf("[affix] Ironclad: health x%.3f on a plain fight (an Elite x%.3f), the Guardian x%.2f; stagger meter x%.3f, its bar drawn x%.2f\n", (double)(hp / hpP), (double)(hpE / hpP), (double)hpG, (double)tower_mod_poise(), (double)tower_mod_poise_bar());
    CHECK(fabsf(hp / hpP - 1.625f) < 0.001f && fabsf(hpE / hpP - 1.25f) < 0.001f && hpG == 1.0f && fabsf(tower_mod_poise() - 1.0f / 1.3f) < 1e-4f && tower_mod_poise_bar() == 1.3f,
          "Ironclad: +30%% health on the Elite's x1.25 (x1.625), the meter 30%% slower to fill, the bar 1.3x long");
    s.affixes = 1 << AFX_RELENTLESS;
    float st = tower_mod_stagger_time(); s.stacks[BOON_CRUSHING_BLOWS] = 1; float stC = tower_mod_stagger_time(); s.stacks[BOON_CRUSHING_BLOWS] = 0;
    debugf("[affix] Relentless: drain x%.1f, down %.2f s (Crushing Blows %.2f s), breathers x%.2f, footwork x%.2f\n", (double)tower_mod_poise_drain(), (double)(3.5f * st), (double)(3.5f * stC),
           (double)tower_elite_pace(true, 0.9f), (double)tower_elite_pace(false, 0.9f));
    CHECK(tower_mod_poise_drain() == 2.0f && fabsf(3.5f * st - 2.5f) < 0.01f && fabsf(3.5f * stC - 3.75f) < 0.01f && fabsf(tower_elite_pace(true, 0.9f) - 1.0f / RELENTLESS_BREATHER) < 1e-4f
          && tower_elite_pace(false, 0.9f) == 1.0f, "Relentless: the drain 2x, down 2.5 s (not 3.5), breathers 15%% shorter");
    s.affixes = 1 << AFX_SWIFT;
    CHECK(fabsf(tower_elite_pace(false, 0.9f) - SWIFT_PACE) < 1e-4f && fabsf(tower_elite_pace(true, 0.3f) - SWIFT_PACE) < 1e-4f && tower_elite_blow(0.3f) == 1.0f && tower_elite_pitch() == SWIFT_PITCH,
          "Swift: footwork, breathers and cooldowns x%.2f, its sounds higher, blows as ever", (double)SWIFT_PACE);
    s.affixes = 1 << AFX_ENRAGED;
    debugf("[affix] Enraged: breathers x%.2f below half (x%.2f above), blows x%.2f below (x%.2f above), footwork x%.2f\n", (double)tower_elite_pace(true, 0.4f), (double)tower_elite_pace(true, 0.6f),
           (double)tower_elite_blow(0.4f), (double)tower_elite_blow(0.6f), (double)tower_elite_pace(false, 0.4f));
    CHECK(fabsf(tower_elite_pace(true, 0.4f) - 1.0f / ENRAGED_BREATHER) < 1e-4f && tower_elite_pace(true, 0.6f) == 1.0f && tower_elite_blow(0.4f) == 1.25f && tower_elite_blow(0.6f) == 1.0f
          && tower_elite_pace(false, 0.4f) == 1.0f && tower_elite_held_longer(), "Enraged: below half, breathers shorter and blows +25%%");
    s.affixes = 0;
    CHECK(tower_elite_pace(true, 0.3f) == 1.0f && tower_elite_blow(0.3f) == 1.0f && tower_mod_poise() == 1.0f && tower_mod_poise_drain() == 1.0f && !tower_affix_tag()[0], "no affix: nothing changes");
    // 4. two affixes (circle 4 on): a duo card in the draft when one is ready
    {
        RUN(96);
        while (s.stacks[BOON_BLOODTHIRST] < BOONS[BOON_BLOODTHIRST].maxStacks) take_boon(BOON_BLOODTHIRST);
        while (s.stacks[BOON_STORM_BLADE] < BOONS[BOON_STORM_BLADE].maxStacks) take_boon(BOON_STORM_BLADE);
        s.elite = true; int withDuo[3] = { 0, 0, 0 };
        for (int a = 1; a <= 2; a++) for (int n = 0; n < 200; n++) {
            s.affixes = a == 1 ? 1 << AFX_SWIFT : (1 << AFX_SWIFT) | (1 << AFX_VAMPIRIC);
            tower_draft_open(); s.draftOpen = false;
            for (int i = 0; i < s.nChoices; i++) if (s.choices[i] < BOON_COUNT && BOONS[s.choices[i]].duo) { withDuo[a]++; break; }
        }
        s.elite = false; s.affixes = 0;
        debugf("[affix] Crimson Storm ready: 200 Elite drafts with a duo card: one affix %d, two %d\n", withDuo[1], withDuo[2]);
        CHECK(withDuo[2] == 200 && withDuo[1] < 200, "a two-affix Elite's draft holds a duo when one is ready");
    }
    // 5. Berserker: Blood, Legendary, in the Blood pack; Iron Skin's rival (Iron Skin locks it and Riposte)
    RUN(97); s.packs = 0;
    CHECK(!offerable(BOON_BERSERKER), "Berserker: not without the Blood pack");
    s.packs = 1 << FAM_BLOOD;
    CHECK(offerable(BOON_BERSERKER) && card_tier(BOON_BERSERKER) == TIER_LEGENDARY && BOONS[BOON_BERSERKER].family == FAM_BLOOD, "Berserker: Legendary, Blood, with its pack");
    take_boon(BOON_BERSERKER);
    CHECK(s.locked[BOON_IRON_SKIN] && !s.locked[BOON_RIPOSTE] && !offerable(BOON_IRON_SKIN), "Berserker locks Iron Skin out");
    RUN(98); take_boon(BOON_IRON_SKIN);
    CHECK(s.locked[BOON_BERSERKER] && s.locked[BOON_RIPOSTE], "Iron Skin locks out both its rivals");
    {   // never both on the table: 500 drafts
        RUN(99); int both = 0;
        for (int n = 0; n < 500; n++) { tower_draft_open(); s.draftOpen = false; bool a = false, b = false;
            for (int i = 0; i < s.nChoices; i++) { a |= s.choices[i] == BOON_BERSERKER; b |= s.choices[i] == BOON_IRON_SKIN; } both += a && b; }
        CHECK(!both, "Berserker and Iron Skin never in one draft (%d)", both);
    }
    // its effects below half health: no potions (the button does nothing), the streak, a blow or 2 s ends it
    RUN(100); take_boon(BOON_BERSERKER);
    extern double nowS; double now0 = nowS;
    character.health = 0.6f * character.maxHealth;
    bool above = tower_potion_blocked() || tower_berserk();
    character.health = 0.4f * character.maxHealth; character.healthPotions = 3;
    character_set_state(CHAR_STATE_NORMAL);
    bool drank = character_try_use_health_potion();
    debugf("[affix] Berserker: at 60%% active %d; at 40%% active %d, a potion drunk %d (potions %d), health %.0f\n", above, tower_berserk(), drank, character.healthPotions, (double)character.health);
    CHECK(!above && tower_berserk() && !drank && character.healthPotions == 3, "Berserker: below half no potion (the press does nothing), above half as ever");
    float k[9]; int nk = 0;
    for (int i = 0; i < 8; i++) { nowS = now0 + 0.5 * i; k[nk++] = tower_mod_damage_vs(1.0f, false, TOWER_HIT_SWORD); tower_on_sword_hit(false); }
    float skill = tower_mod_damage_vs(1.0f, false, TOWER_HIT_SKILL);
    tower_on_player_hurt(5.0f); float afterBlow = tower_mod_damage_vs(1.0f, false, TOWER_HIT_SWORD);
    tower_on_sword_hit(false); tower_on_sword_hit(false); nowS += 2.1; float afterGap = tower_mod_damage_vs(1.0f, false, TOWER_HIT_SWORD);
    debugf("[affix] Berserker's streak: %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f; a skill's hit x%.2f; after a blow x%.2f; after 2.1 s x%.2f\n",
           (double)k[0], (double)k[1], (double)k[2], (double)k[3], (double)k[4], (double)k[5], (double)k[6], (double)k[7], (double)skill, (double)afterBlow, (double)afterGap);
    float top = 1.0f + BERSERK_STEP * BERSERK_HITS;
    CHECK(k[0] == 1.0f && fabsf(k[1] - (1.0f + BERSERK_STEP)) < 1e-4f && fabsf(k[BERSERK_HITS + 1] - top) < 1e-4f && fabsf(k[7] - top) < 1e-4f && skill == 1.0f && afterBlow == 1.0f && afterGap == 1.0f,
          "Berserker: +%.0f%% a sword hit in a row to +%.0f%%, ended by a blow or 2 s", (double)(100 * BERSERK_STEP), (double)(100 * BERSERK_STEP * BERSERK_HITS));
    character.health = 0.8f * character.maxHealth; tower_on_sword_hit(false); nowS += 0.5;
    CHECK(tower_mod_damage_vs(1.0f, false, TOWER_HIT_SWORD) == 1.0f, "Berserker: above half, no streak");
    nowS = now0;
    character_set_state(CHAR_STATE_TITLE_IDLE);
    tower_end_run();
    character.maxHealth = tower_mod_max_health(); character.health = character.maxHealth;
    character_set_skills_enabled(skillsWere);
    #undef CHECK
    #undef RUN
    debugf("[affix] self-check: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    return fails;
}

// The Offerings, the milestones, the seed and the Codex (HLE_TEST_TOWER): pricing, the two-a-climb limit, taking one back,
// 0 and 65535 Embers, the locks, each Offering's effect, their use as a climb starts and their return when it's quit before
// its first Guardian falls (not after it, nor after a death), each milestone from the best floor and its notice, the seed on
// the death line, and every Codex page inside its panel at the smallest, the default and a large overscan; returns the failures
int tower_test_offerings(void)
{
    int fails = 0;
    #define CHECK(c, ...) do { if (!(c)) { fails++; debugf("[offer] FAIL: " __VA_ARGS__); debugf("\n"); } } while (0)
    TowerSave was, t; tower_load(&was);
    bool skillsWere = character_skills_enabled();
    character_set_skills_enabled(true);
    tower_end_run();
    #define SAVE(e, best, off) do { tower_load(&t); t.embers = (e); t.bestFloor = (best); t.offers = (off); t.relicEquipped = 0; save_controller_set_tower(&t); } while (0)
    // pricing and the limit: the first at its price, the second 50 more, no third; taking one back returns what it cost
    SAVE(1000, 0, 0);
    int pW = tower_offer_price(OFF_WHETSTONE), pF = tower_offer_price(OFF_FLASK);
    bool b1 = tower_offer_toggle(OFF_WHETSTONE);
    int pF2 = tower_offer_price(OFF_FLASK), e1 = tower_embers();
    bool b2 = tower_offer_toggle(OFF_FLASK), b3 = tower_offer_toggle(OFF_PURSE);
    int e2 = tower_embers(), back = tower_offer_price(OFF_WHETSTONE);
    CHECK(pW == 40 && pF == 70 && b1 && e1 == 960 && pF2 == 120 && b2 && !b3 && e2 == 840 && tower_offers_held() == 2 && back == 90,
          "prices 40 and 70, the second +50 (%d), no third (%d), Embers 1000 -> %d -> %d, Whetstone back for %d", pF2, b3, e1, e2, back);
    bool t1 = tower_offer_toggle(OFF_WHETSTONE);
    int e3 = tower_embers(), pF1 = tower_offer_price(OFF_FLASK);
    bool t2 = tower_offer_toggle(OFF_FLASK);
    CHECK(t1 && e3 == 930 && pF1 == 70 && t2 && tower_embers() == 1000 && !tower_offers_held(), "taken back in either order: every Ember back (%d, then %d), the one left at its own price (%d)", e3, tower_embers(), pF1);
    // the order doesn't matter: either way round, a pair costs both and 50
    tower_offer_toggle(OFF_FLASK); tower_offer_toggle(OFF_WHETSTONE); int ea = tower_embers(); tower_offer_toggle(OFF_FLASK); tower_offer_toggle(OFF_WHETSTONE);
    CHECK(ea == 840 && tower_embers() == 1000, "Flask then Whetstone: 160 too (%d)", 1000 - ea);
    // 0 Embers, the exact price, 65535
    SAVE(0, 0, 0);
    CHECK(!tower_offer_toggle(OFF_CANDLE) && !tower_offers_held() && tower_embers() == 0, "0 Embers: nothing set aside");
    SAVE(30, 0, 0);
    CHECK(tower_offer_toggle(OFF_CANDLE) && tower_embers() == 0 && !tower_offer_toggle(OFF_WHETSTONE), "30 Embers: the Candle (30), then nothing (0 left)");
    SAVE(65535, 0, 0);
    tower_offer_toggle(OFF_OATH); tower_offer_toggle(OFF_HEARTH);
    int eMax = tower_embers();
    tower_offer_toggle(OFF_HEARTH); tower_offer_toggle(OFF_OATH);
    CHECK(eMax == 65535 - 175 && tower_embers() == 65535, "65535 Embers: two (%d left), both back to 65535 (%d)", eMax, tower_embers());
    // the locks: Kindling at best floor 20, the Candle with Skills off
    SAVE(1000, 19, 0);
    CHECK(tower_offer_lock(OFF_KINDLING)[0] && !tower_offer_toggle(OFF_KINDLING), "Kindling locked below floor 20 (%s)", tower_offer_lock(OFF_KINDLING));
    SAVE(1000, 20, 0);
    CHECK(!tower_offer_lock(OFF_KINDLING)[0] && tower_offer_toggle(OFF_KINDLING), "Kindling at floor 20");
    character_set_skills_enabled(false);
    CHECK(tower_offer_lock(OFF_CANDLE)[0] && !tower_offer_toggle(OFF_CANDLE), "the Candle locked with Skills off");
    character_set_skills_enabled(true);
    // used as the climb starts: each one's effect
    SAVE(1000, 0, 0);
    tower_begin_run(5); float dmg0 = tower_mod_damage_dealt(), hp0 = tower_mod_max_health(); int pot0 = tower_mod_max_potions(), gold0 = s.gold; tower_end_run();
    SAVE(1000, 0, (OFF_FLASK + 1) | ((OFF_WHETSTONE + 1) << 4));
    character.health = 100.0f;
    tower_begin_run(5);
    TowerSave mid; tower_load(&mid);
    float dmg1 = tower_mod_damage_dealt(); int pot1 = tower_mod_max_potions();
    s.circle = 2; float dmg2 = tower_mod_damage_dealt(); s.circle = 1;
    CHECK(mid.offers == 0 && s.offers == ((1 << OFF_FLASK) | (1 << OFF_WHETSTONE)) && pot1 == pot0 + 1 && fabsf(dmg1 / dmg0 - 1.25f) < 1e-4f && fabsf(dmg2 / dmg0 - 1.10f) < 1e-4f,
          "Flask + Whetstone used as the climb starts (save %02x): potions most %d -> %d, damage x%.3f in circle 1, x%.3f in circle 2 (its baseline only)", mid.offers, pot0, pot1, (double)(dmg1 / dmg0), (double)(dmg2 / dmg0));
    // quit before the first Guardian falls: set aside again; a restart uses them again
    tower_end_run(); tower_load(&mid);
    CHECK(mid.offers == ((OFF_FLASK + 1) | ((OFF_WHETSTONE + 1) << 4)) && tower_embers() == 1000, "a climb quit before its first Guardian fell: set aside again (%02x), nothing charged twice", mid.offers);
    tower_begin_run(6); character.health = 100.0f; tower_begin_run(7); tower_load(&mid);     // (a restart from the pause menu)
    CHECK(s.offers == ((1 << OFF_FLASK) | (1 << OFF_WHETSTONE)) && mid.offers == 0, "a restart before it fell: the same Offerings again (%02x)", s.offers);
    character.health = 0.0f; tower_end_run(); tower_load(&mid);
    CHECK(mid.offers == 0, "a death on floor 1: spent (%02x)", mid.offers);
    character.health = 100.0f;
    SAVE(1000, 0, OFF_PURSE + 1);
    tower_begin_run(8); tower_floor_cleared(); test_unnotice(); s.draftOpen = false; ui_panel_close(ui_details()); tower_end_run(); tower_load(&mid);
    CHECK(mid.offers == 0, "quit after the first Guardian fell: spent (%02x)", mid.offers);
    // Purse, Oath, Kindling (never Deep Flask), the Candle (two skill cards, the first draft only), Hearth (the first Rest only)
    SAVE(1000, 20, (OFF_PURSE + 1) | ((OFF_OATH + 1) << 4));
    tower_begin_run(9);
    CHECK(s.gold == gold0 + OFFER_GOLD && fabsf(tower_mod_max_health() - hp0 - OFFER_HP) < 1e-4f, "Pilgrim's Purse +%d gold (%d), Hale Oath +%.0f max health (%.0f)", OFFER_GOLD, s.gold, (double)OFFER_HP, (double)tower_mod_max_health());
    tower_end_run();
    int kinds = 0, deep = 0, kseen[BOON_COUNT] = { 0 };
    for (int k = 0; k < 200; k++) {
        SAVE(1000, 20, OFF_KINDLING + 1); tower_begin_run(100 + k);
        int g = s.slot[0] - 1;
        if (g >= 0 && BOONS[g].tier == TIER_COMMON && s.stacks[g] == 1 && slots_used() == 1) { if (!kseen[g]++) kinds++; } else fails++;
        deep += g == BOON_DEEP_FLASK;
        tower_end_run();
    }
    CHECK(kinds >= 4 && !deep, "Kindling: a Common boon at Lv 1 in slot 1 each of 200 climbs (%d kinds), never Deep Flask (%d)", kinds, deep);
    int two = 0, later = 0;
    for (int k = 0; k < 200; k++) {
        SAVE(1000, 0, OFF_CANDLE + 1); tower_begin_run(300 + k);
        tower_draft_open(); int n = 0; for (int i = 0; i < s.nChoices; i++) n += is_skill_card((BoonId)s.choices[i]);
        two += n == 2;
        s.floor = 2; tower_draft_open(); n = 0; for (int i = 0; i < s.nChoices; i++) n += is_skill_card((BoonId)s.choices[i]);
        later += n >= 2;
        s.draftOpen = false; tower_end_run();
    }
    CHECK(two == 200 && later < 40, "the Candle: two skill cards in the first draft (%d of 200), not after (%d with two)", two, later);
    SAVE(1000, 0, OFF_HEARTH + 1); tower_begin_run(10);
    character.maxHealth = tower_mod_max_health(); character.health = 50.0f; character.healthPotions = 3;
    rest_take(REST_POTIONS_ROW); float h1 = character.health;
    character.health = 50.0f; rest_take(REST_POTIONS_ROW); float h2 = character.health;
    CHECK(fabsf(h1 - 110.0f) < 0.01f && h2 == 50.0f, "Hearth Ember: the first Rest (its potions) heals 40%% too (50 -> %.0f), the second not (%.0f)", (double)h1, (double)h2);
    tower_end_run();
    SAVE(1000, 0, OFF_HEARTH + 1); tower_begin_run(10);
    character.maxHealth = tower_mod_max_health(); character.health = 50.0f;
    s.hearthUsed = false; rest_take(REST_HEAL_ROW);
    CHECK(fabsf(character.health - 150.0f) < 0.01f, "Hearth Ember with the Rest's own heal: 40%% + 40%% (50 -> %.0f)", (double)character.health);
    tower_end_run();
    // the milestones: each from the best floor, its notice the floor it's first reached, its reward
    static const int MS_AT[MS_COUNT] = { 10, 20, 30, 50, 75, 100 };
    for (int i = 0; i < MS_COUNT; i++) {
        SAVE(0, MS_AT[i] - 1, 0);
        bool before = tower_milestone_reached(i);
        tower_begin_run(20); s.floor = MS_AT[i]; character.health = 100.0f;
        tower_floor_cleared();
        int note = s.notice; bool open = ui_details()->open && ui_details()->owner == UI_OWNER_TOWER;
        s.draftOpen = false; ui_panel_close(ui_details()); tower_end_run();
        tower_begin_run(21); s.floor = MS_AT[i] + 1; tower_floor_cleared(); test_unnotice(); int again = s.notice; s.draftOpen = false; ui_panel_close(ui_details()); tower_end_run();
        CHECK(!before && tower_milestone_reached(i) && note == i + 1 && open && !again, "floor %d: %s reached, its notice over the draft once (%d %d %d)", MS_AT[i], MILESTONES[i].name, note, open, again);
    }
    SAVE(0, 29, 0); tower_begin_run(22); int potA = tower_mod_max_potions(); tower_end_run();
    SAVE(0, 30, 0); tower_begin_run(22); int potB = tower_mod_max_potions(); tower_end_run();
    CHECK(potB == potA + 1, "floor 30: one more potion every climb (%d -> %d)", potA, potB);
    SAVE(5000, 49, 0);
    bool sig0 = tower_relic_owned(RELIC_SIGIL), buy = tower_relic_select(RELIC_SIGIL); int c0 = tower_relic_cost(RELIC_SIGIL);
    SAVE(5000, 50, 0);
    bool sig1 = tower_relic_owned(RELIC_SIGIL), wear = tower_relic_select(RELIC_SIGIL);
    CHECK(!sig0 && !buy && c0 == -2 && sig1 && wear && tower_relic_equipped() == RELIC_SIGIL && tower_embers() == 5000, "the Climber's Sigil: not sold (%d), owned at floor 50, worn for nothing", c0);
    { tower_begin_run(23); float hp = tower_mod_max_health(); tower_end_run(); CHECK(fabsf(hp - hp0 - 15.0f) < 1e-3f, "the Sigil worn: +15 max health (%.0f)", (double)hp); }
    SAVE(0, 74, 0); bool h74 = tower_codex_open_ok(CX_HEART); SAVE(0, 75, 0);
    CHECK(!h74 && tower_codex_open_ok(CX_HEART) && !tower_title_badge()[0], "the Codex's last page at floor 75");
    SAVE(0, 100, 0); CHECK(tower_title_badge()[0], "the title at floor 100");
    // Teal Cloak: the skin's own rule (save_controller) says the same floor
    { const SaveData *sd = save_controller_get_slot_data(save_controller_get_active_slot());
      SAVE(0, 9, 0); bool t9 = save_progress_skin_unlocked(sd, SKIN_TEAL_CLOAK); SAVE(0, 10, 0);
      CHECK(!t9 && save_progress_skin_unlocked(sd, SKIN_TEAL_CLOAK) && MILESTONES[MS_TEAL].floor == 10, "floor 10's milestone is the Teal Cloak's own floor"); }
    // the seed: on the death line and in Stats
    SAVE(0, 0, 0);
    tower_begin_run(0x1A2B3C4Du);
    CHECK(tower_seed() == 0x1A2B3C4Du && strstr(tower_death_summary(), "Seed 1A2B3C4D"), "the seed on the death line: %s", tower_death_summary());
    { UiDoc *d = malloc(sizeof *d); ui_doc_clear(d); tower_stats_doc(d); bool found = false;
      for (int i = 0; i < d->n; i++) found |= strstr(d->text + d->row[i].l, "Seed 1A2B3C4D") != NULL;
      CHECK(found, "the seed in Stats"); free(d); }
    tower_end_run();
    // the menus' one-line texts in their room: the Offerings screen's (240 px) head with the widest pair and each row's line,
    // the Tower menu's (200) records at 255 and 65535 and its line of the pair set aside, a milestone notice's title
    {
        int wPair = 0, wBrief = 0; char l[96];
        for (int a = 0; a < OFF_COUNT; a++) for (int b2 = 0; b2 < OFF_COUNT; b2++) if (a != b2) {
            snprintf(l, sizeof l, "%s, %s", OFFERINGS[a].name, OFFERINGS[b2].name); int w = ui_text_width(l); if (w > wPair) wPair = w; }
        for (int a = 0; a < OFF_COUNT; a++) { int w = ui_text_width(OFFERINGS[a].brief); if (w > wBrief) wBrief = w; }
        int wTwo = ui_text_width("Two are set aside: take one back first"), wRec = ui_text_width("Best floor 255   Embers 65535");
        snprintf(l, sizeof l, "%s, %s", OFFERINGS[OFF_CANDLE].name, OFFERINGS[OFF_PURSE].name); int wTower = ui_text_width(l);
        int wRow = ui_text_width("Scholar's Candle") + 8 + ui_text_width("105");
        CHECK(wPair <= 240 && wBrief <= 240 && wTwo <= 240 && wRow <= 224 && wRec <= 200 && wTower <= 200,
              "one-line texts: Offerings head %d, briefs %d, the full line %d, a row %d (of 240 / 224); the Tower menu's records %d, pair %d (of 200)", wPair, wBrief, wTwo, wRow, wRec, wTower);
        debugf("[offer] widths: Offerings head %d, briefs %d, full %d, row %d of 240; Tower menu records %d, pair %d of 200\n", wPair, wBrief, wTwo, wRow, wRec, wTower);
    }
    // every Codex page inside its panel: its rows and text within the page's room, its title on one line, its pages fewer than
    // the panel's most (a page past the most would be cut), at the smallest overscan, the default and a large one
    text_load();
    int empty = 0;
    for (int k = TXT_OFFER; k < TXT_COUNT; k++) empty += !full_text(k)[0];
    CHECK(!empty, "tower_text.bin holds every Offering's and Codex page's text (%d missing)", empty);
    text_free();
    static const int8_t OS[3] = { -15, 0, 20 };
    int8_t ox = uiOverscanX, oy = uiOverscanY;
    for (int b = 0; b < 2; b++) {       // (a fresh save, then everything reached: the generated tables at their longest)
        SAVE(65535, b ? 255 : 0, b ? ((OFF_CANDLE + 1) | ((OFF_PURSE + 1) << 4)) : 0);
        for (int o = 0; o < 3; o++) {
            uiOverscanX = uiOverscanY = OS[o];
            int worst = 0, worstK = 0;
            for (int k = 0; k < CX_COUNT; k++) {
                if (!tower_codex_open_ok(k)) continue;
                tower_codex_open(k);
                UiPanel *p = ui_details(); UiDoc *d = p->doc;
                int tw = SCREEN_WIDTH - 2 * (ui_safe_margin_x() > TITLE_SAFE_MARGIN_X ? ui_safe_margin_x() : TITLE_SAFE_MARGIN_X) - 12;
                bool fit = d->n < UI_DOC_ROWS && d->used < UI_DOC_TEXT - 1 && p->pg.pages < UI_DOC_PAGES && ui_text_width(p->title) <= tw;
                bool stray = false;
                for (int i = 0; i < d->n; i++) stray |= strchr(d->text + d->row[i].l, '|') != NULL || !d->text[d->row[i].l];
                CHECK(fit && !stray, "Codex %s (overscan %d, save %d): %d rows, %d bytes, %d pages, title %d px of %d%s", CODEX_TITLE[k], OS[o], b, d->n, d->used, p->pg.pages,
                      ui_text_width(p->title), tw, stray ? ", a stray or empty row" : "");
                if (p->pg.pages > worst) { worst = p->pg.pages; worstK = k; }
                if (o == 1 && b == 1) debugf("[codex] %s: %d pages, %d rows, %d bytes\n", CODEX_TITLE[k], p->pg.pages, d->n, d->used);
                ui_panel_close(p);
            }
            debugf("[offer] the Codex at overscan %d (save %d): the longest page %s, %d pages\n", OS[o], b, CODEX_TITLE[worstK], worst);
        }
    }
    uiOverscanX = ox; uiOverscanY = oy;
    save_controller_set_tower(&was);
    character_set_skills_enabled(skillsWere);
    character.maxHealth = tower_mod_max_health(); character.health = character.maxHealth;
    #undef SAVE
    #undef CHECK
    debugf("[offer] self-check: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    return fails;
}

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

// ---- Elite affixes (section 11): the fight's own, or a test aid's on the Bonus bosses outside a run ----
#ifdef HLE_LOG_AFFIX        // test aid: each affix as it works
#define AFX_LOG(...) debugf("[affix] " __VA_ARGS__)
#else
#define AFX_LOG(...) do { } while (0)
#endif
static uint8_t affixes_now(void)
{
#if !defined(HLE_DUMMY_BUILD)   // (the build bot's fights are in a run, its affixes the test aid's)
    if (s.active) return s.elite ? s.affixes : 0;
#endif
#ifdef HLE_ELITE_FORCE
    Boss *b = boss_get_instance();
    if (b && b->phaseIndex >= 3) return hle_elite_force & ~(b->phaseIndex == 5 ? 1 << AFX_SMOULDERING : 0);
#endif
    return 0;
}
bool tower_affix(int a) { return (affixes_now() >> a) & 1; }
static void affix_names(uint8_t m, char *out, int n)
{
    int k = 0; out[0] = 0;
    for (int a = 0; a < AFX_COUNT; a++) if ((m >> a) & 1) k += snprintf(out + k, k < n ? n - k : 0, "%s%s", k ? " " : "", AFFIXES[a].name);
}
const char* tower_affix_tag(void) { static char b[40]; affix_names(affixes_now(), b, sizeof b); return b; }
float tower_elite_pace(bool breather, float frac)
{
    float k = tower_affix(AFX_SWIFT) ? SWIFT_PACE : 1.0f;
    if (breather && frac < 0.5f && tower_affix(AFX_ENRAGED)) k /= ENRAGED_BREATHER;
    if (breather && tower_affix(AFX_RELENTLESS)) k /= RELENTLESS_BREATHER;
    return k;
}
float tower_elite_blow(float frac) { return frac < 0.5f && tower_affix(AFX_ENRAGED) ? ENRAGED_BLOW : 1.0f; }
float tower_mod_poise(void) { return tower_affix(AFX_IRONCLAD) ? 1.0f / IRONCLAD_K : 1.0f; }
float tower_mod_poise_bar(void) { return tower_affix(AFX_IRONCLAD) ? IRONCLAD_K : 1.0f; }
float tower_mod_poise_drain(void) { return tower_affix(AFX_RELENTLESS) ? RELENTLESS_DRAIN : 1.0f; }
bool  tower_elite_held_longer(void) { return tower_affix(AFX_ENRAGED); }
float tower_elite_pitch(void) { return tower_affix(AFX_SWIFT) ? SWIFT_PITCH : 1.0f; }
void  tower_on_boss_dealt(float lost)
{
    Boss *b = boss_get_instance();
    if (!tower_affix(AFX_VAMPIRIC) || !b || lost <= 0.0f || b->health <= 0.0f || b->mode != BOSS_MODE_COMBAT) return;
    float h = b->health;
    b->health = fminf(b->maxHealth, h + VAMPIRIC_HEAL * lost);
    AFX_LOG("Vampiric: a blow took %.1f, the boss heals %.2f (%.1f -> %.1f)\n", (double)lost, (double)(b->health - h), (double)h, (double)b->health);
    (void)h;
}

// ---- Berserker (section 7): below half health a blow that doesn't knock you down can't flinch you out of an
// attack, each sword hit in a row +8% (up to +40%; a blow taken or 2 s without a hit ends the streak), no potions ----
bool tower_berserk(void) { return tower_boon_stacks(BOON_BERSERKER) && character.health > 0.0f && character.health < BERSERK_HP * character.maxHealth; }
bool tower_potion_blocked(void) { return tower_berserk(); }
static float berserk_bonus(void)
{
    extern double nowS;
    if (!tower_berserk() || nowS - s.berserkAt > BERSERK_GAP) return 0.0f;
    return BERSERK_STEP * (s.berserkHits < BERSERK_HITS ? s.berserkHits : BERSERK_HITS);
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
    if (s.elite && phase >= 3) k *= ELITE_HP * ((s.affixes >> AFX_IRONCLAD) & 1 ? IRONCLAD_K : 1.0f);   // (Ironclad: +30% on the x1.25)
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

#ifdef HLE_TOWER_GALLERY
// Test aid (HLE_TOWER_GALLERY, driven by hle/autoplay.c between floors): every Tower screen in its worst case
// for a look, one a step (the longest names, two-affix Elites, 5-digit gold, floor 123, Ascension 19, every
// night, every pact pair, a full bar with a duo and every rival locked, 15 potions, 0 gold); returns the
// step's name, NULL past the last. [gallery] logs each with its VI frame (HLE_LOG_MOVE_MS)
static bool gal_press(int key)
{
    s.scrOpenMs = s.draftOpenMs = get_ticks_ms() - DRAFT_INPUT_DELAY_MS - 1;
    return tower_between_update(key == 'L', key == 'R', key == 'U', key == 'D', key == 'A', key == 'B', key == 'r');
}
static void gallery_full_run(void)
{
    tower_begin_run(77);
    s.packs = (1 << FAM_COUNT) - 1; s.forms = (1 << SKILL_COUNT) - 1;
    s.floor = 123; s.circle = 20; s.night = NIGHT_STARFALL; s.gold = 99999; s.runEmbers = 99999;
    #define MAXB(b) while (s.stacks[b] < BOONS[b].maxStacks) take_boon(b)
    MAXB(BOON_PHANTOM_STEP); MAXB(BOON_BLOODTHIRST); take_boon(BOON_SHADES_MERCY);     // a duo (Shadow + Blood)
    MAXB(BOON_CRUSHING_BLOWS); take_boon(BOON_CRUSHING_BLOWS_PLUS); MAXB(BOON_STATIC); take_boon(BOON_EMBER_EDGE); take_boon(BOON_RALLY); take_boon(BOON_LAST_STAND);
    #undef MAXB
    for (unsigned i = 0; i < sizeof RIVALS / sizeof RIVALS[0]; i++) s.locked[RIVALS[i][1]] = true;     // every rival locked
    s.skills = 7; s.stacks[BOON_LEARN_MOONBLADE] = 3; s.stacks[BOON_LEARN_CLEAVE] = 2; s.stacks[BOON_LEARN_HALFMOON] = 4; s.stacks[BOON_TWIN_MOON] = 1;
    for (int b = 0; b < PB_COUNT; b++) s.bless[b] = 2;
    for (int c = 0; c < PC_COUNT; c++) s.curse[c] = 2;
    s.halfBless[PB_MIGHT] = 1; s.potionDebt = 2;
    character.maxHealth = tower_mod_max_health(); character.health = character.maxHealth * 0.6f; character.healthPotions = POTION_CAP;
}
static void gallery_draft(int a, int b, int c, int sel, bool cleared)
{
    s.choices[0] = a; s.choices[1] = b; s.choices[2] = c; s.nChoices = 3; s.sel = sel;
    s.draftOpen = true; s.draftOpenMs = get_ticks_ms() - 2000; s.scr = SCR_NONE;
    s.circleCleared = cleared; s.nextNight = NIGHT_STARFALL; s.lastFloorEmbers = 999; s.lastFloorGold = 999;
}
static void gallery_map(int t, uint8_t afx, int boss, bool prisoner)
{
    s.draftOpen = false; ui_panel_close(ui_details());
    s.map.row = 0; s.map.path[0] = 1;
    for (int l = 0; l < MAP_LANES; l++) s.map.type[1][l] = NODE_FIGHT;
    s.map.type[1][1] = (uint8_t)t; s.map.boss[1][1] = (uint8_t)(boss + 1); s.map.afx[1][1] = afx;
    s.prisoner = prisoner;
    tower_map_open();
    s.map.sel = 1;
}
static void gallery_details(void) { details_for_cursor(true); }
static void gallery_confirm(int page) { s.scr = SCR_MAP; gal_press('A'); ui_details()->pg.page = (int8_t)(page < ui_details()->pg.pages ? page : ui_details()->pg.pages - 1); }
static void gallery_event(int e, bool done)
{
    s.draftOpen = false; ui_panel_close(ui_details());
    s.event = (int8_t)e; s.eventPick = PB_LAST_BREATH; s.eventDone = false;
    s.scr = SCR_EVENT; list_open(SCR_EVENT);
    if (done) { int g = s.gold, p = character.healthPotions; event_take(0); s.gold = g; character.healthPotions = p; }
    s.scrOpenMs = get_ticks_ms() - 2000;
}
const char *tower_test_gallery(int k)
{
    static char name[80];
    if (k == 0) gallery_full_run();
    ui_panel_close(ui_details());
    int i = 0;
    #define STEP(nm, ...) if (k == i++) { __VA_ARGS__; snprintf(name, sizeof name, "%s", nm); return name; }
    STEP("draft: skill cards, a new circle", gallery_draft(BOON_LEARN_MOONBLADE, BOON_FULL_MOON_DANCE, BOON_LONG_CLEAVE, 0, true))
    STEP("draft: duos", gallery_draft(BOON_BLOOD_MOON_RITE, BOON_ANVIL_OF_STORMS, BOON_STEEL_HEARTH, 1, false))
    STEP("draft: the longest pacts", gallery_draft(PACT_CARD(PB_WOUNDED, PC_EXPOSED), PACT_CARD(PB_LAST_BREATH, PC_WEAKNESS), PACT_CARD(PB_STAGGERED, PC_IMPAIRED), 2, false))
    STEP("draft: rivals", gallery_draft(BOON_RIPOSTE, BOON_IRON_SKIN, BOON_BERSERKER, 1, false))
    STEP("draft: pluses and the purse", gallery_draft(BOON_CRUSHING_BLOWS_PLUS, BOON_PHANTOM_STEP_PLUS, GOLD_CARD, 2, false))
    STEP("details: Iron Skin (rivals)", gallery_draft(BOON_RIPOSTE, BOON_IRON_SKIN, BOON_BERSERKER, 1, false); gallery_details())
    STEP("details: Full Moon Dance 1", gallery_draft(BOON_FULL_MOON_DANCE, BOON_SHATTERING_DANCE, BOON_RALLY, 0, false); gallery_details())
    STEP("details: Full Moon Dance 2", gallery_draft(BOON_FULL_MOON_DANCE, BOON_SHATTERING_DANCE, BOON_RALLY, 0, false); gallery_details(); gal_press('A'))
    STEP("details: Full Moon Dance 3", gallery_draft(BOON_FULL_MOON_DANCE, BOON_SHATTERING_DANCE, BOON_RALLY, 0, false); gallery_details(); gal_press('A'); gal_press('A'))
    STEP("details: Full Moon Dance 4", gallery_draft(BOON_FULL_MOON_DANCE, BOON_SHATTERING_DANCE, BOON_RALLY, 0, false); gallery_details(); gal_press('A'); gal_press('A'); gal_press('A'))
    STEP("details: Rally (held, its +)", gallery_draft(BOON_FULL_MOON_DANCE, BOON_SHATTERING_DANCE, BOON_RALLY, 2, false); gallery_details())
    STEP("details: Shade's Mercy (a duo held)", gallery_draft(BOON_SHADES_MERCY, BOON_STALWART, BOON_RALLY, 0, false); gallery_details())
    STEP("details: Stalwart (new)", gallery_draft(BOON_SHADES_MERCY, BOON_STALWART, BOON_RALLY, 1, false); gallery_details())
    STEP("details: a pact", gallery_draft(PACT_CARD(PB_WOUNDED, PC_EXPOSED), PACT_CARD(PB_LAST_BREATH, PC_WEAKNESS), PACT_CARD(PB_STAGGERED, PC_IMPAIRED), 0, false); gallery_details())
    STEP("details: the purse", gallery_draft(BOON_CRUSHING_BLOWS_PLUS, BOON_PHANTOM_STEP_PLUS, GOLD_CARD, 2, false); gallery_details())
    STEP("map: Elite Relentless Smouldering Shackled Sun, Starfall", gallery_map(NODE_ELITE, (1 << AFX_RELENTLESS) | (1 << AFX_SMOULDERING), TOWER_SHACKLED_SUN, false))
    STEP("map: the jailer, Swift Ironclad", gallery_map(NODE_FIGHT, (1 << AFX_SWIFT) | (1 << AFX_IRONCLAD), TOWER_FALLEN_KNIGHT, true))
    STEP("map: Blood Moon", s.night = NIGHT_BLOOD_MOON; gallery_map(NODE_SHOP, 0, 0, false))
    STEP("map: Iron Night", s.night = NIGHT_IRON; gallery_map(NODE_REST, 0, 0, false))
    STEP("map: no night", s.night = NIGHT_NONE; gallery_map(NODE_EVENT, 0, 0, false))
    STEP("map: R on the Elite", s.night = NIGHT_STARFALL; gallery_map(NODE_ELITE, (1 << AFX_RELENTLESS) | (1 << AFX_SMOULDERING), TOWER_SHACKLED_SUN, false); gallery_details())
    STEP("confirm: Elite, page 1", gallery_map(NODE_ELITE, (1 << AFX_RELENTLESS) | (1 << AFX_SMOULDERING), TOWER_SHACKLED_SUN, false); gallery_confirm(0))
    STEP("confirm: Elite, page 2", gallery_map(NODE_ELITE, (1 << AFX_RELENTLESS) | (1 << AFX_SMOULDERING), TOWER_SHACKLED_SUN, false); gallery_confirm(1))
    STEP("confirm: the jailer Swift Ironclad", gallery_map(NODE_FIGHT, (1 << AFX_SWIFT) | (1 << AFX_IRONCLAD), TOWER_FALLEN_KNIGHT, true); gallery_confirm(0))
    STEP("confirm: a Fight", gallery_map(NODE_FIGHT, 0, TOWER_SHADOWBORN, false); gallery_confirm(0))
    STEP("confirm: Rest", gallery_map(NODE_REST, 0, 0, false); gallery_confirm(0))
    STEP("confirm: Shop", gallery_map(NODE_SHOP, 0, 0, false); gallery_confirm(0))
    STEP("confirm: Event", gallery_map(NODE_EVENT, 0, 0, false); gallery_confirm(0))
    STEP("confirm: Guardian Unbound, Starfall", gallery_map(NODE_FIGHT, 0, 2, false); s.map.row = MAP_ROWS - 2; s.map.path[s.map.row] = 1; s.map.sel = 1; gallery_confirm(0))
    STEP("shop: 99999 gold, the longest cards", s.draftOpen = false; shop_open(); s.shop.card[0] = BOON_SANGUINE_FLASK; s.shop.card[1] = BOON_SHADES_MERCY; s.shop.card[2] = BOON_CRUSHING_BLOWS; s.shop.pact = PACT_CARD(PB_WOUNDED, PC_EXPOSED); s.scrOpenMs -= 2000)
    STEP("shop: on the pact", s.shop.sel = SHOP_PACT)
    STEP("shop: on Potion, 15 held", s.shop.sel = SHOP_POTION)
    STEP("shop: 0 gold, on a card", s.gold = 0; s.shop.sel = SHOP_CARD1)
    STEP("shop: R on a card", s.gold = 99999; s.shop.sel = SHOP_CARD0; gallery_details())
    STEP("shop: R on A level", s.shop.sel = SHOP_LEVEL; gallery_details())
    STEP("shop: a level's list", list_open(SCR_SHOP_LEVEL))
    STEP("shop: a +'s list", s.stacks[BOON_RALLY] = 2; s.stacks[BOON_EMBER_EDGE] = 5; list_open(SCR_SHOP_PLUS))
    STEP("shop: Release's list", list_open(SCR_SHOP_RELEASE); s.listSel = 0)
    STEP("shop: R on Release's duo", list_open(SCR_SHOP_RELEASE); s.listSel = 0; gallery_details())
    STEP("event 1", gallery_event(EV_ALTAR, false)) STEP("event 1 result", gallery_event(EV_ALTAR, true))
    STEP("event 2", gallery_event(EV_GRAVE, false)) STEP("event 2 result", gallery_event(EV_GRAVE, true))
    STEP("event 3", gallery_event(EV_MIRROR, false)) STEP("event 3 result", gallery_event(EV_MIRROR, true))
    STEP("event 4", gallery_event(EV_MERCHANT, false)) STEP("event 4 result", gallery_event(EV_MERCHANT, true))
    STEP("event 5", gallery_event(EV_POOL, false)) STEP("event 5 result", s.skills = 3; gallery_event(EV_POOL, true); s.skills = 7)
    STEP("event 6", gallery_event(EV_PRISONER, false)) STEP("event 6 result", gallery_event(EV_PRISONER, true))
    STEP("event 7", gallery_event(EV_BONES, false)) STEP("event 7 result", gallery_event(EV_BONES, true))
    STEP("event 8", gallery_event(EV_BRAZIER, false)) STEP("event 8 result", gallery_event(EV_BRAZIER, true))
    STEP("event: R on the Mirror", gallery_event(EV_MIRROR, false); gallery_details())
    STEP("rest", s.draftOpen = false; list_open(SCR_REST); s.scrOpenMs -= 2000)
    STEP("rest: R on Temper", list_open(SCR_REST); s.listSel = 1; gallery_details())
    STEP("temper's list", s.stacks[BOON_EMBER_EDGE] = 1; s.stacks[BOON_RALLY] = 1; list_open(SCR_TEMPER))
    STEP("release's list", list_open(SCR_RELEASE))
    STEP("map: to the Elite (A, A fights it)", s.prisoner = false; gallery_map(NODE_ELITE, (1 << AFX_RELENTLESS) | (1 << AFX_SMOULDERING), TOWER_SHACKLED_SUN, false))
    #undef STEP
    return NULL;
}
#endif
