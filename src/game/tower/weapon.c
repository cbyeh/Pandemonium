#include "weapon.h"
#include "tower.h"
#include "save_controller.h"
#include "character_skin.h"
#include "fx/skill_fx.h"

static const struct {
    const char *name, *desc;
    int floor, cost;
    float sword, skill;         // damage multipliers: sword swings (and finishers), skills
    uint8_t r, g, b;            // the blade (0, 0, 0: the skin's own)
    bool lunar;                 // the skills in the moonlight palette
} WEAPONS[WPN_COUNT] = {
    [WPN_KNIGHT]     = { "Knight's Sword", "The all-rounder", 0, 0, 1.0f, 1.0f, 0, 0, 0, false },
    [WPN_LUNAR_EDGE] = { "Lunar Edge", "Sword swings -20%, skills +30%", 25, 1500, 0.8f, 1.3f, 120, 196, 255, true },
};

static bool valid(int id) { return id >= 0 && id < WPN_COUNT; }
static bool owned_t(const TowerSave *t, int id) { return id == WPN_KNIGHT || (valid(id) && ((t->weaponsOwned >> (id - 1)) & 1)); }

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
        if (tower_best_floor() < WEAPONS[id].floor || t.embers < WEAPONS[id].cost) return false;
        t.embers -= WEAPONS[id].cost;
        t.weaponsOwned |= 1 << (id - 1);
    }
    t.weapon = id;
    bool ok = save_controller_set_tower(&t);
    weapon_apply();
    return ok;
}

float weapon_damage_mult(int kind)
{
    int e = weapon_equipped();
    if (kind == TOWER_HIT_SWORD || kind == TOWER_HIT_FINISHER) return WEAPONS[e].sword;
    if (kind == TOWER_HIT_SKILL) return WEAPONS[e].skill;
    return 1.0f;    // (a bleed's tick, the rest)
}

void weapon_apply(void)
{
    int e = weapon_equipped();
#ifdef HLE_AUTOPLAY
    debugf("[weapon] equipped: %s\n", WEAPONS[e].name);
#endif
    character_skin_set_blade(WEAPONS[e].r | WEAPONS[e].g | WEAPONS[e].b, WEAPONS[e].r, WEAPONS[e].g, WEAPONS[e].b);
    skill_fx_set_lunar(WEAPONS[e].lunar);
}
