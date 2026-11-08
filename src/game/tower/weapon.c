#include "weapon.h"
#include "tower.h"
#include "save_controller.h"
#include "character.h"
#include "character_skin.h"
#include "game/boss/boss.h"
#include "game/sun_boss/sun_boss.h"
#include "game/dragon_boss/dragon_boss.h"

static const struct {
    const char *name, *desc;
    int floor, cost;                // the best floor that brings it to the Armory; Embers (0: free, owned at its floor)
    float sword, skill;             // damage: sword swings (and finishers), skills
    float speed, stamina, poise;    // swings' speed, a swing's stamina, the stagger build-up of sword hits
    int hunt; float huntMult, otherMult;   // a hunter: its boss's phase (-1 none), its damage on it, elsewhere
    float fire, shadow;             // damage taken from fire, from Shadowborne's flames and beams
    bool perfect, glow;             // perfect dodges pay; the blade lit by itself
    uint8_t r, g, b;                // the blade (0, 0, 0: the skin's own)
    uint8_t er, eg, eb;             // the Black Swordsman's slab edges (0, 0, 0: the blade's)
} WEAPONS[WPN_COUNT] = {
    [WPN_KNIGHT]        = { "Knight's Sword", "The all-rounder", 0, 0,
                            1.0f, 1.0f, 1.0f, 1.0f, 1.0f, -1, 1.0f, 1.0f, 1.0f, 1.0f, false, false, 0, 0, 0, 0, 0, 0 },
    [WPN_NUBIUM]        = { "Mare Nubium", "Swings 20% faster, 25% less stamina, -25% a hit", 10, 600,
                            0.75f, 1.0f, 1.2f, 0.75f, 1.0f, -1, 1.0f, 1.0f, 1.0f, 1.0f, false, false, 214, 220, 232, 0, 0, 0 },
    [WPN_FRIGORIS]      = { "Mare Frigoris", "Swings 15% slower, +40% stagger, -10% damage", 15, 900,
                            0.9f, 0.9f, 0.85f, 1.0f, 1.4f, -1, 1.0f, 1.0f, 1.0f, 1.0f, false, false, 178, 214, 255, 0, 0, 0 },
    [WPN_LUNAR_EDGE]    = { "Lunar Edge", "Sword swings -20%, skills +30%", 25, 1500,
                            0.8f, 1.3f, 1.0f, 1.0f, 1.0f, -1, 1.0f, 1.0f, 1.0f, 1.0f, false, true, 120, 196, 255, 0, 0, 0 },
    [WPN_TRANQUIL]      = { "Mare Requiem", "Perfect dodges: stamina back, skills sooner; -10% damage", 30, 1800,
                            0.9f, 0.9f, 1.0f, 1.0f, 1.0f, -1, 1.0f, 1.0f, 1.0f, 1.0f, true, false, 150, 222, 200, 0, 0, 0 },
    [WPN_SOLS_BANE]     = { "Sol's Bane", "+30% on the Shackled Sun, fire halved, -10% elsewhere", 40, 2000,
                            1.0f, 1.0f, 1.0f, 1.0f, 1.0f, SUN_BOSS_PHASE, 1.3f, 0.9f, 0.5f, 1.0f, false, false, 132, 112, 82, 0, 0, 0 },
    [WPN_SHADOWSLAYER]  = { "Shadowslayer", "+30% on Shadowborne, its flames halved, -10% elsewhere", 50, 2500,
                            1.0f, 1.0f, 1.0f, 1.0f, 1.0f, DRAGON_BOSS_PHASE, 1.3f, 0.9f, 1.0f, 0.5f, false, false, 64, 64, 76, 225, 232, 245 },
    };
#define PERFECT_STAMINA 0.25f   // Mare Requiem: of max stamina back
#define PERFECT_COOLDOWN 1.0f   //   and every skill's cooldown this much sooner

static bool valid(int id) { return id >= 0 && id < WPN_COUNT; }
int weapon_at_row(int row)
{
    static const uint8_t ORDER[WPN_COUNT] = { WPN_KNIGHT, WPN_NUBIUM, WPN_FRIGORIS, WPN_LUNAR_EDGE, WPN_TRANQUIL, WPN_SOLS_BANE, WPN_SHADOWSLAYER };
    return row >= 0 && row < WPN_COUNT ? ORDER[row] : -1;
}
static bool owned_t(const TowerSave *t, int id)
{
    if (!valid(id)) return false;
    if (id == WPN_KNIGHT) return true;
    if (!WEAPONS[id].cost) return t->bestFloor >= WEAPONS[id].floor;    // a milestone's: never a bit
    return (t->weaponsOwned >> (id - 1)) & 1;
}

const char *weapon_name(int id) { return valid(id) ? WEAPONS[id].name : ""; }
const char *weapon_desc(int id) { return valid(id) ? WEAPONS[id].desc : ""; }
int  weapon_floor(int id) { return valid(id) ? WEAPONS[id].floor : 0; }
bool weapon_owned(int id) { TowerSave t; save_controller_get_tower(&t); return owned_t(&t, id); }
int  weapon_cost(int id) { return weapon_owned(id) ? -1 : valid(id) ? WEAPONS[id].cost : -1; }
bool weapon_available(int id) { return weapon_owned(id) || tower_best_floor() >= weapon_floor(id); }
int  weapon_equipped(void)
{
#ifdef HLE_WEAPON_FORCE     // test aid: this sword equipped, owned or not (the multipliers, the looks)
    return HLE_WEAPON_FORCE;
#endif
    TowerSave t; save_controller_get_tower(&t);
    return owned_t(&t, t.weapon) ? t.weapon : WPN_KNIGHT;     // (an unknown or unowned byte: the knight's)
}

bool weapon_select(int id)
{
    if (!valid(id)) return false;
    TowerSave t; save_controller_get_tower(&t);
    if (!owned_t(&t, id)) {     // buy it (its floor reached, the Embers held)
        if (!WEAPONS[id].cost || tower_best_floor() < WEAPONS[id].floor || t.embers < WEAPONS[id].cost) return false;
        t.embers -= WEAPONS[id].cost;
        t.weaponsOwned |= 1 << (id - 1);
    }
    t.weapon = id;
    bool ok = save_controller_set_tower(&t);
    weapon_apply();
    return ok;
}

#define W (WEAPONS[weapon_equipped()])
float weapon_damage_mult(int kind, int bossPhase)
{
    float m = W.hunt < 0 ? 1.0f : bossPhase == W.hunt ? W.huntMult : W.otherMult;
    if (kind == TOWER_HIT_SWORD || kind == TOWER_HIT_FINISHER) return m * W.sword;
    if (kind == TOWER_HIT_SKILL) return m * W.skill;
    return m;       // (a bleed's tick, the rest)
}
float weapon_poise_mult(int kind) { return kind == TOWER_HIT_SWORD || kind == TOWER_HIT_FINISHER ? W.poise : 1.0f; }
float weapon_swing_speed(void) { return W.speed; }
float weapon_stamina_mult(void) { return W.stamina; }
float weapon_taken_mult(int dmgKind) { return dmgKind == DMG_FIRE ? W.fire : dmgKind == DMG_SHADOW ? W.shadow : 1.0f; }
void  weapon_on_perfect_dodge(void)
{
    if (!W.perfect) return;
    character.stamina = character.stamina + PERFECT_STAMINA * character.maxStamina;
    if (character.stamina > character.maxStamina) character.stamina = character.maxStamina;
    character_cut_skill_cooldowns(PERFECT_COOLDOWN);
#ifdef HLE_AUTOPLAY
    debugf("[weapon] %s: a perfect dodge, stamina %.0f, skills %.0f s sooner\n", W.name, (double)character.stamina, (double)PERFECT_COOLDOWN);
#endif
}

void weapon_apply(void)
{
    int e = weapon_equipped();
#ifdef HLE_AUTOPLAY
    debugf("[weapon] equipped: %s\n", WEAPONS[e].name);
#endif
    const bool own = WEAPONS[e].er | WEAPONS[e].eg | WEAPONS[e].eb;     // (its slab edges: their own colour, or the blade's)
    character_skin_set_blade(WEAPONS[e].r | WEAPONS[e].g | WEAPONS[e].b, WEAPONS[e].glow, WEAPONS[e].r, WEAPONS[e].g, WEAPONS[e].b,
                             own ? WEAPONS[e].er : WEAPONS[e].r, own ? WEAPONS[e].eg : WEAPONS[e].g, own ? WEAPONS[e].eb : WEAPONS[e].b);
}
#undef W
