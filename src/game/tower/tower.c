#include <libdragon.h>
#include <stdio.h>
#include <string.h>

#include "tower.h"
#include "character.h"
#include "globals.h"
#include "save_controller.h"
#include "video_controller.h"

// ---- Tuning ----
#define FLOORS_PER_CIRCLE   3
#define BOSS_HP_PER_CIRCLE  0.30f     // each circle after the first
#define BOSS_DMG_PER_CIRCLE 0.20f
#define DRAFT_CHOICES       3

typedef struct { const char *name, *desc; uint8_t maxStacks; } BoonDef;

static const BoonDef BOONS[BOON_COUNT] = {
    [BOON_EMBER_EDGE]    = { "Ember Edge",    "+15% damage",                  3 },
    [BOON_IRON_SKIN]     = { "Iron Skin",     "-15% damage taken",            2 },
    [BOON_DEEP_FLASK]    = { "Deep Flask",    "+1 potion",                    3 },
    [BOON_KINDLED_FLASK] = { "Kindled Flask", "Potions heal 15% more",        2 },
    [BOON_FLEETFOOT]     = { "Fleetfoot",     "Rolls cost 40% less stamina",  1 },
    [BOON_SECOND_WIND]   = { "Second Wind",   "+40% stamina regen",           2 },
    [BOON_MOON_MASTERY]  = { "Moon Mastery",  "Skill cooldowns -20%",         2 },
    [BOON_BLOODTHIRST]   = { "Bloodthirst",   "Heal 6% of damage dealt",      2 },
    [BOON_VIGOR]         = { "Vigor",         "+25 max health",               3 },
    [BOON_LAST_STAND]    = { "Last Stand",    "+40% damage below 30% health", 1 },
    [BOON_UNBROKEN]      = { "Unbroken",      "Once a floor, live through a killing blow", 1 },
    [BOON_CRESCENT_ECHO] = { "Crescent Echo", "Moon Blade Dance: 2 more waves", 1 },
};

typedef struct { const char *name, *desc; } UpgradeDef;

static const UpgradeDef UPGRADES[UPG_COUNT] = {
    [UPG_FLASK_CAPACITY] = { "Flask Capacity", "+1 potion" },
    [UPG_FLASK_POTENCY]  = { "Flask Potency",  "Potions heal 8% more" },
    [UPG_HONED_BLADE]    = { "Honed Blade",    "+8% damage" },
    [UPG_VIGOR]          = { "Vigor",          "+10 max health" },
    [UPG_SKILL_MASTERY]  = { "Skill Mastery",  "Skill cooldowns -8%" },
};
static const int UPGRADE_COST[UPG_MAX_LEVEL] = { 30, 70, 120 };

static struct {
    bool active;
    int floor;
    uint32_t rng;
    TowerBoss order[FLOORS_PER_CIRCLE];
    uint8_t stacks[BOON_COUNT];
    int runEmbers, lastFloorEmbers;
    bool unbrokenUsed;
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
    // The Guardian opens every circle; the other two come in either order
    s.order[0] = TOWER_GUARDIAN;
    bool swap = rng_next() & 1;
    s.order[1] = swap ? TOWER_SHACKLED_SUN : TOWER_FALLEN_KNIGHT;
    s.order[2] = swap ? TOWER_FALLEN_KNIGHT : TOWER_SHACKLED_SUN;
}

// ---- Run ----

void tower_begin_run(uint32_t seed)
{
    memset(&s, 0, sizeof(s));
    s.active = true;
    s.floor = 1;
    s.rng = seed ? seed : 1;
    shuffle_circle();
}

void tower_end_run(void) { s.active = false; s.draftOpen = false; }
bool tower_active(void) { return s.active; }
int  tower_floor(void) { return s.floor; }
int  tower_circle(void) { return (s.floor - 1) / FLOORS_PER_CIRCLE + 1; }
TowerBoss tower_floor_boss(void) { return s.order[(s.floor - 1) % FLOORS_PER_CIRCLE]; }
int  tower_run_embers(void) { return s.runEmbers; }
int  tower_boon_stacks(BoonId b) { return (s.active && b < BOON_COUNT) ? s.stacks[b] : 0; }

static int floor_embers(void) { return 10 * tower_circle() + 5 * ((s.floor - 1) % FLOORS_PER_CIRCLE); }

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
    tower_draft_open();
}

void tower_next_floor(void)
{
    s.floor++;
    s.unbrokenUsed = false;
    if ((s.floor - 1) % FLOORS_PER_CIRCLE == 0) shuffle_circle();
}

// ---- Draft ----

void tower_draft_open(void)
{
    // Three different boons that can still stack
    BoonId pool[BOON_COUNT]; int n = 0;
    for (int b = 0; b < BOON_COUNT; b++) if (s.stacks[b] < BOONS[b].maxStacks) pool[n++] = b;
    s.nChoices = 0;
    while (s.nChoices < DRAFT_CHOICES && n > 0) {
        int k = rng_next() % n;
        s.choices[s.nChoices++] = pool[k];
        pool[k] = pool[--n];
    }
    s.sel = 0;
    s.draftOpen = true;
}

bool tower_draft_is_open(void) { return s.draftOpen; }

void tower_draft_pick(int index)
{
    if (index >= 0 && index < s.nChoices) {
        BoonId b = s.choices[index];
        s.stacks[b]++;
        if (b == BOON_DEEP_FLASK) character.healthPotions++;
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

static void text(int x, int y, int w, rdpq_align_t align, const char *str)
{
    rdpq_text_printf(&(rdpq_textparms_t){ .align = align, .width = w, .wrap = WRAP_WORD }, FONT_UNBALANCED, x, y, "%s", str);
}

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

    char line[64];
    rdpq_set_prim_color(RGBA32(255, 210, 120, 255));
    snprintf(line, sizeof line, "Floor %d cleared   +%d Embers", s.floor, s.lastFloorEmbers);
    text(0, 36, SCREEN_WIDTH, ALIGN_CENTER, line);
    rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
    text(0, 58, SCREEN_WIDTH, ALIGN_CENTER, "Choose a Boon");

    const int cw = 92, ch = 112, gap = 8, y0 = 76;
    int x0 = (SCREEN_WIDTH - (cw * s.nChoices + gap * (s.nChoices - 1))) / 2;
    for (int i = 0; i < s.nChoices; i++) {
        int x = x0 + i * (cw + gap);
        const BoonDef *b = &BOONS[s.choices[i]];
        flat_mode();
        bool on = i == s.sel;
        rdpq_set_prim_color(on ? RGBA32(230, 150, 50, 255) : RGBA32(90, 70, 50, 255));
        rdpq_fill_rectangle(x - 2, y0 - 2, x + cw + 2, y0 + ch + 2);
        rdpq_set_prim_color(on ? RGBA32(60, 30, 20, 240) : RGBA32(30, 22, 18, 230));
        rdpq_fill_rectangle(x, y0, x + cw, y0 + ch);
        rdpq_set_prim_color(on ? RGBA32(255, 220, 140, 255) : RGBA32(220, 200, 170, 255));
        text(x + 4, y0 + 18, cw - 8, ALIGN_CENTER, b->name);
        rdpq_set_prim_color(RGBA32(235, 235, 235, 255));
        text(x + 6, y0 + 46, cw - 12, ALIGN_CENTER, b->desc);
        if (s.stacks[s.choices[i]] > 0) {
            snprintf(line, sizeof line, "Have %d", s.stacks[s.choices[i]]);
            rdpq_set_prim_color(RGBA32(170, 170, 170, 255));
            text(x, y0 + ch - 10, cw, ALIGN_CENTER, line);
        }
    }
    rdpq_set_prim_color(RGBA32(200, 200, 200, 255));
    snprintf(line, sizeof line, "Next: floor %d%s", s.floor + 1,
             (s.floor % FLOORS_PER_CIRCLE == 0) ? "  (the bosses grow stronger)" : "");
    text(0, y0 + ch + 22, SCREEN_WIDTH, ALIGN_CENTER, line);
}

void tower_hud_draw(void)
{
    if (!s.active) return;
    char line[32];
    snprintf(line, sizeof line, "Floor %d", s.floor);
    rdpq_set_prim_color(RGBA32(255, 210, 120, 255));
    text(0, 24 + uiOverscanY, SCREEN_WIDTH - 12 - uiOverscanX, ALIGN_RIGHT, line);
    if (tower_circle() > 1) {
        snprintf(line, sizeof line, "Ascension %d", tower_circle() - 1);
        rdpq_set_prim_color(RGBA32(230, 120, 90, 255));
        text(0, 36 + uiOverscanY, SCREEN_WIDTH - 12 - uiOverscanX, ALIGN_RIGHT, line);
    }
}

const char* tower_death_summary(void)
{
    static char line[64];
    snprintf(line, sizeof line, "Floor %d   +%d Embers (%d)", s.floor, s.runEmbers, tower_embers());
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

const char* tower_upgrade_name(UpgradeId u) { return UPGRADES[u].name; }
const char* tower_upgrade_desc(UpgradeId u) { return UPGRADES[u].desc; }

// ---- Modifiers ----

static float powf_int(float b, int e) { float r = 1.0f; while (e-- > 0) r *= b; return r; }

float tower_mod_damage_dealt(void)
{
    float m = (1.0f + 0.08f * tower_upgrade_level(UPG_HONED_BLADE)) * (1.0f + 0.15f * tower_boon_stacks(BOON_EMBER_EDGE));
    if (tower_boon_stacks(BOON_LAST_STAND) && character.health < character.maxHealth * 0.3f) m *= 1.4f;
    return m;
}

float tower_mod_damage_taken(void)
{
    float m = powf_int(0.85f, tower_boon_stacks(BOON_IRON_SKIN));
    if (s.active) m *= 1.0f + BOSS_DMG_PER_CIRCLE * (tower_circle() - 1);
    return m;
}

int   tower_mod_max_potions(void) { return 7 + tower_upgrade_level(UPG_FLASK_CAPACITY) + tower_boon_stacks(BOON_DEEP_FLASK); }
float tower_mod_potion_heal(void)
{
    return 0.45f * (1.0f + 0.08f * tower_upgrade_level(UPG_FLASK_POTENCY)) * (1.0f + 0.15f * tower_boon_stacks(BOON_KINDLED_FLASK));
}
float tower_mod_max_health(void) { return 150.0f + 10.0f * tower_upgrade_level(UPG_VIGOR) + 25.0f * tower_boon_stacks(BOON_VIGOR); }
float tower_mod_roll_cost(void) { return tower_boon_stacks(BOON_FLEETFOOT) ? 0.6f : 1.0f; }
float tower_mod_stamina_regen(void) { return 1.0f + 0.4f * tower_boon_stacks(BOON_SECOND_WIND); }
float tower_mod_skill_cooldown(void)
{
    return (1.0f - 0.08f * tower_upgrade_level(UPG_SKILL_MASTERY)) * powf_int(0.8f, tower_boon_stacks(BOON_MOON_MASTERY));
}
float tower_mod_lifesteal(void) { return 0.06f * tower_boon_stacks(BOON_BLOODTHIRST); }
int   tower_mod_extra_mbd_waves(void) { return tower_boon_stacks(BOON_CRESCENT_ECHO) ? 2 : 0; }

bool tower_mod_try_unbroken(void)
{
    if (!tower_boon_stacks(BOON_UNBROKEN) || s.unbrokenUsed) return false;
    s.unbrokenUsed = true;
    return true;
}

float tower_boss_health_scale(void) { return s.active ? 1.0f + BOSS_HP_PER_CIRCLE * (tower_circle() - 1) : 1.0f; }
