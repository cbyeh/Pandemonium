/**
 * Test aid (HLE_HUMAN_BOT): a bot that plays a fight the way a decent human does, for the balance audit.
 * - It learns each boss move's timing: the delay from the move's start (its tell: the boss's pose and sound)
 *   to each of its hits (hb_move / hb_blow), per boss and state, kept for the whole emulator session
 *   (a human who has met the move), seeded from hle/hb_table.h when present.
 * - At a move's start it schedules a roll so the hit lands HB_LEAD into the roll's invincibility, with a
 *   human's timing error (HB_SIGMA, more on moves whose timing varies) and lapses (HB_LAPSE); it can't act
 *   before HB_REACT after the tell. The game decides if the roll worked (its own i-frames).
 * - It swings in the boss's openings HB_REACT after they start, keeping a roll's stamina in hand, casts its
 *   skills there, drinks below a threshold when nothing is coming, and steps out of floor fire.
 * Everything it does is a press on the pad (autoplay.c); nothing in the game is changed by it.
 */
#ifdef HLE_HUMAN_BOT
#include <libdragon.h>
#include <math.h>
#include <string.h>
#include "character.h"
#include "game/boss/boss.h"
#include "game/tower/tower.h"
#include "utilities/game_time.h"
extern const char *tower_affix_tag(void);

#ifndef HB_REACT
#define HB_REACT 0.40f      // s from a tell to the first press (the metric's reaction)
#endif
#ifndef HB_LEAD
#define HB_LEAD 0.17f       // s into the roll the hit should land (the bot's roll, 0.48 s, is invincible 0.01-0.35 s: the middle)
#endif
#ifndef HB_SIGMA
#define HB_SIGMA 0.08f      // s: a human's timing error (sd)
#endif
#ifndef HB_LAPSE
#define HB_LAPSE 0.10f      // a tell missed now and then
#endif
#ifndef HB_POTION_AT
#define HB_POTION_AT 0.40f
#endif
#ifndef HB_REFLEX
#define HB_REFLEX 0.5f      // a blow it has no timing for (a projectile's flight, a flurry faster than its rolls): avoided this often (a human's
                            // eyes and spacing; the bot's rolls can't model them). The only dice in the bot: [hbfight] counts them apart (reflex)
#endif
#define HB_KEYS (8 * 64)
#define HB_SLOTS 6

float hb_dealt;             // all the player's damage to bosses (boss.c), for the bench's windows
int hb_src;                 // set by a hazard just before it deals its tick (1 Smouldering's floor fire, 2 Shadowborne's flames)
int hb_plan, hb_runs;       // the run's build plan (tower.c reads it), runs so far
uint32_t hb_seed;           // the next run's seed (tower.c)
int hb_route;               // 1 safe, 2 normal, 3 greedy, 4 adaptive (tower.c)
int hb_skill_pref = -1;     // the plan's skill (0-2, -1 none: the sword); tower.c sets it with the plan

typedef struct { float mean, m2; int n; } Slot;
static Slot tab[HB_KEYS][HB_SLOTS];
static int starts[HB_KEYS];     // how often each move has started (a hit it's known for must come at least a quarter of the time)
#if __has_include("hb_table.h")
#include "hb_table.h"       // { key, slot, mean, sd, n } rows learned by earlier runs
#define HB_HAVE_TABLE 1
#endif

static uint32_t rs = 12345;
static float frand(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return (rs >> 8) * (1.0f / 16777216.0f); }
static float gauss(void) { float u = frand() + 1e-6f, v = frand(); return sqrtf(-2.0f * logf(u)) * cosf(6.2831853f * v); }

// recent move starts (key, time)
#define HB_RECENT 3
static int recKey[HB_RECENT]; static float recT[HB_RECENT];
// rolls scheduled: when, for which key/slot, and what became of them
#define HB_PEND 8
typedef struct { float at, target; int key, slot; int8_t st; } Pend;    // st 0 waiting, 1 rolled, 2 lapsed, 3 blocked
static Pend pend[HB_PEND]; static int npend;
static Pend done[16]; static int ndone;     // for classifying a blow that lands

// a fight's numbers
static struct {
    bool on; int floor, boss, circle; float t0, hp0, maxHp; int pot0;
    float taken, healed, landed, castLanded, castEnd; int blows, castBlows, dodged, hazard, reflex, fast, lapse, timing, blocked, unknown, commit;
    int rolls, swings, casts, castBroken, castCancel; float castTaken; int potions;
    float openT, fireT; int lastKey; float lastAmt; char lastWhy[12];
    int whyFar, whyStam, whySoon, whyState, whyOk;
    float prevHp; bool inCast; float castStart; int castSkill;
} F;
static float sessionT;
static float s_fleeUntil;      // burnt by a lingering flame: step out of it

static int key_of(int phase, int state) { return (phase & 7) * 64 + (state & 63); }

void hb_move(int phase, int state)
{
    if (!F.on) return;
    int key = key_of(phase, state);
    float now = gameTime;
    for (int i = HB_RECENT - 1; i > 0; i--) { recKey[i] = recKey[i - 1]; recT[i] = recT[i - 1]; }
    recKey[0] = key; recT[0] = now;
    starts[key]++;
    // schedule a roll for each hit this move is known for (seen in a quarter of its starts at least)
    for (int k = 0; k < HB_SLOTS; k++) {
        Slot *s = &tab[key][k];
        if (s->n < 2 || s->n * 4 < starts[key]) continue;
        float sd = s->n > 1 ? sqrtf(s->m2 / (s->n - 1)) : 0.0f;
        float target = now + s->mean;
        float at = target - HB_LEAD + gauss() * sqrtf(HB_SIGMA * HB_SIGMA + sd * sd);
        float earliest = now + HB_REACT + 0.05f * gauss();
        if (at < earliest) at = earliest;
        bool dup = false;
        for (int i = 0; i < npend; i++) if (fabsf(pend[i].target - target) < 0.2f) dup = true;
        if (dup || npend >= HB_PEND) continue;
        Pend p = { at, target, key, k, 0 };
        if (frand() < HB_LAPSE) p.st = 2;
        pend[npend++] = p;
    }
}

static void remember(Pend p) { if (ndone == 16) { memmove(done, done + 1, 15 * sizeof(Pend)); ndone--; } done[ndone++] = p; }

static void learn(float now)
{
    for (int r = 0; r < 2; r++) {      // the move under way; and the one before it when this one began just now (its tell was that one's)
        if (r == 1 && now - recT[0] >= 0.45f) break;
        float d = now - recT[r];
        if (recT[r] <= 0.0f || d < 0.0f || d > 4.0f) continue;
        Slot *t = tab[recKey[r]];
        int best = -1; float bd = 0.15f;
        for (int k = 0; k < HB_SLOTS; k++) if (t[k].n && fabsf(t[k].mean - d) < bd) { bd = fabsf(t[k].mean - d); best = k; }
        if (best < 0) for (int k = 0; k < HB_SLOTS; k++) if (!t[k].n) { best = k; break; }
        if (best < 0) continue;
        Slot *s = &t[best];
        s->n++; float dm = d - s->mean; s->mean += dm / s->n; s->m2 += dm * (d - s->mean);
    }
}

// a blow about to land (after its scaling), before the roll's invincibility is checked
bool hb_blow(float amount, bool invuln)
{
    int src = hb_src; hb_src = 0;
    if (!F.on) return false;
    float now = gameTime;
    if (src) { s_fleeUntil = now + 1.5f; if (!invuln) { F.hazard++; F.landed += amount * boss_hp_loss_scale() * tower_mod_damage_taken(); F.lastKey = -src; F.lastAmt = amount; strcpy(F.lastWhy, src == 1 ? "fire" : "flames"); } return false; }
    if (!F.on || amount < 5.0f) return false;
    learn(now);
    if (invuln) { if (character_get_state() == CHAR_STATE_ROLLING) F.dodged++;
#ifdef HB_LOG_BLOWS
        debugf("[hbblow] t %.2f amt %.0f key %d (%.2f s in) DODGED st %d\n", (double)now, (double)amount, recKey[0], (double)(now - recT[0]), character_get_state());
#endif
        return false; }
    // why it would land: the roll meant for it, if any
    const char *why = "unknown";
    int best = -1; float bd = 0.35f;
    for (int i = 0; i < ndone; i++) if (fabsf(done[i].target - now) < bd) { bd = fabsf(done[i].target - now); best = i; }
    for (int i = 0; i < npend; i++) if (fabsf(pend[i].target - now) < bd) { best = 100 + i; bd = fabsf(pend[i].target - now); }
    if (best >= 0) {
        Pend *p = best >= 100 ? &pend[best - 100] : &done[best];
        float lead = tab[p->key][p->slot].mean;
        if (lead < HB_REACT + 0.02f) why = "fast";
        else why = p->st == 2 ? "lapse" : p->st == 3 ? "blocked" : p->st == 1 ? "timing" : "late";
    }
    if ((!strcmp(why, "unknown") || !strcmp(why, "blocked") || !strcmp(why, "fast")) && frand() < HB_REFLEX) {
        F.reflex++;
#ifdef HB_LOG_BLOWS
        debugf("[hbblow] t %.2f amt %.0f key %d (%.2f s in) REFLEX (%s)\n", (double)now, (double)amount, recKey[0], (double)(now - recT[0]), why);
#endif
        return true;
    }
    F.blows++; F.lastAmt = amount; F.lastKey = recKey[0];
    F.landed += amount * boss_hp_loss_scale() * tower_mod_damage_taken();
    if (character_get_state() == CHAR_STATE_SKILL || F.inCast || gameTime - F.castEnd < 0.5f) { F.castBlows++; F.castLanded += amount * boss_hp_loss_scale() * tower_mod_damage_taken(); }
    if (!strcmp(why, "fast")) F.fast++; else if (!strcmp(why, "lapse")) F.lapse++; else if (!strcmp(why, "timing") || !strcmp(why, "late")) F.timing++;
    else if (!strcmp(why, "blocked")) F.blocked++; else F.unknown++;
    strncpy(F.lastWhy, why, sizeof F.lastWhy - 1);
#ifdef HB_LOG_BLOWS
    debugf("[hbblow] t %.2f amt %.0f key %d (%.2f s in) prev %d (%.2f) why %s st %d stam %.0f\n", (double)now, (double)amount, recKey[0], (double)(now - recT[0]), recKey[1], (double)(now - recT[1]), why, character_get_state(), (double)character.stamina);
#endif
    return false;
}
void hb_cast_broken(void) { if (F.on) F.castBroken++; }

static float s_openSince = -1.0f, s_reactAt;
static int s_k, s_side = 1;
static float s_lastSwing;

void hb_fight_begin(Boss *boss)
{
    memset(&F, 0, sizeof F);
    F.on = true; F.floor = tower_floor(); F.boss = boss->phaseIndex; F.circle = tower_circle();
    F.t0 = gameTime; F.hp0 = character.health; F.maxHp = character.maxHealth; F.pot0 = character.healthPotions; F.prevHp = character.health;
    npend = ndone = 0; memset(recT, 0, sizeof recT);
    s_openSince = -1.0f;
    rs ^= (uint32_t)(F.floor * 2654435761u) ^ hb_seed;
}

// the fight's summary line; cost = damage taken over the metric's resources (max health x 2.35: 150 + 3 potions x 45%)
void hb_fight_end(const char *how, Boss *boss)
{
    if (!F.on) return;
    F.on = false;
    float dur = gameTime - F.t0;
    sessionT += dur;
    // (two lines: ISViewer cuts a line past 255 characters)
    debugf("[hbfight] floor %d circle %d boss %d %s%s | %.1f s | taken %.1f (cost %.1f%%) landed %.1f healed %.1f | hp %.0f->%.0f/%.0f potions %d->%d | boss %.0f/%.0f | last %d %.0f %s\n",
           F.floor, F.circle, F.boss, how, tower_affix_tag()[0] ? " elite" : "", (double)dur, (double)F.taken, (double)(F.taken * 100.0f / (2.35f * F.maxHp)), (double)F.landed, (double)F.healed,
           (double)F.hp0, (double)character.health, (double)character.maxHealth, F.pot0, character.healthPotions,
           (double)(boss ? boss->health : 0), (double)(boss ? boss->maxHealth : 0), F.lastKey, (double)F.lastAmt, F.lastWhy);
    debugf("[hbfight2] floor %d blows %d dodged %d reflex %d hazard %d | fast %d lapse %d timing %d blocked %d unknown %d | rolls %d swings %d casts %d broken %d cancelled %d castblows %d castlanded %.1f | open %.0f%%\n",
           F.floor, F.blows, F.dodged, F.reflex, F.hazard, F.fast, F.lapse, F.timing, F.blocked, F.unknown,
           F.rolls, F.swings, F.casts, F.castBroken, F.castCancel, F.castBlows, (double)F.castLanded, (double)(dur > 0 ? F.openT * 100.0f / dur : 0));
}

// the learned table, for hle/hb_table.h
void hb_dump_table(void)
{
    for (int k = 0; k < HB_KEYS; k++) for (int j = 0; j < HB_SLOTS; j++) if (tab[k][j].n)
        debugf("[hbtab] %d %d %.3f %.3f %d %d\n", k, j, (double)tab[k][j].mean, (double)(tab[k][j].n > 1 ? sqrtf(tab[k][j].m2 / (tab[k][j].n - 1)) : 0.0f), tab[k][j].n, starts[k]);
}

void hb_next_run(void);
void hb_init(void)
{
    static bool done0;
    if (done0) return;
    done0 = true;
    hb_next_run();
#ifdef HB_HAVE_TABLE
    for (unsigned i = 0; i < sizeof HB_TABLE / sizeof HB_TABLE[0]; i++) {
        Slot *s = &tab[(int)HB_TABLE[i][0]][(int)HB_TABLE[i][1]];
        int n = (int)HB_TABLE[i][4]; if (n > 20) n = 20;     // (still learning)
        s->mean = HB_TABLE[i][2]; s->n = n; s->m2 = HB_TABLE[i][3] * HB_TABLE[i][3] * (n - 1);
        int key = (int)HB_TABLE[i][0], st0 = (int)HB_TABLE[i][5] * n / (int)HB_TABLE[i][4]; if (st0 > starts[key]) starts[key] = st0;
    }
#endif
}

// a cast: which skill to press (0-2), -1 none
static int pick_skill(Boss *boss, float openAge, float d)
{
    int order[3] = { hb_skill_pref, 0, 1 }, n = 0;
    (void)n;
    for (int o = 0; o < 4; o++) {
        int k = o == 0 ? hb_skill_pref : o - 1;
        if (k < 0 || (o > 0 && k == hb_skill_pref)) continue;
        if (!character_skill_usable(k) || character_get_skill_cooldown(k) > 0.0f || !character_can_afford_skill(k)) continue;
        if (d > (k == SKILL_CRESCENT_CLEAVE ? 95.0f : boss->phaseIndex == 5 ? 62.0f : 48.0f)) continue;     // (the Cleave's blades fly; Shadowborne's position is its hips)
        // Halfmoon's 2 s charge: at an opening's start or on a staggered boss (Full Moon can't be broken)
        if (k == SKILL_HALFMOON_SLASH && openAge > HB_REACT + 0.35f && boss->staggerT < 1.5f && tower_skill_form(k) != SKILL_FORM_B) continue;
        return k;
    }
    (void)order;
    return -1;
}

void hb_fight(joypad_inputs_t *in, joypad_buttons_t *pressed, Boss *boss)
{
    extern bool hle_open_now;
    if (!F.on) return;
    if (boss->phaseIndex != F.boss) { hb_fight_end("phase", boss); hb_fight_begin(boss); }     // (the Story's Guardian, then Unbound)
    float now = gameTime, dt = deltaTime;
    int st = character_get_state();
    // health: damage taken and healed this fight (a potion's heal counted as healing too)
    float dh = character.health - F.prevHp;
    if (dh < 0.0f) { F.taken -= dh; if (st == CHAR_STATE_SKILL || F.inCast) F.castTaken += -dh; }
    else F.healed += dh;
    F.prevHp = character.health;
    s_k++;
    if (s_k % 60 == 1) pressed->z = 1;
    in->btn.z = 1;
    float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2], d = sqrtf(dx * dx + dz * dz);
#ifdef HLE_LOG_BOONFX     // (Iron Tide's check) the cooldowns about a stagger and each cast's start and end
    { static float wasStag, logUntil; static int lastSt = -1;
      if (boss->staggerT > 0.0f && wasStag <= 0.0f) logUntil = now + 1.0f;
      if (now < logUntil || (st == CHAR_STATE_SKILL) != (lastSt == CHAR_STATE_SKILL))
          debugf("[hbcd] t %.2f state %d (was %d) stagger %.2f cooldowns %.2f %.2f %.2f\n", (double)now, st, lastSt, (double)boss->staggerT,
                 (double)character_get_skill_cooldown(0), (double)character_get_skill_cooldown(1), (double)character_get_skill_cooldown(2));
      wasStag = boss->staggerT; lastSt = st; }
#endif
    // a cast under way: its end
    if (F.inCast && st != CHAR_STATE_SKILL) { F.inCast = false; F.castEnd = now; }
    if (st == CHAR_STATE_SKILL && !F.inCast) { F.inCast = true; F.castStart = now; F.casts++; }
    // the rolls due
    bool rolled = false;
    for (int i = 0; i < npend; i++) {
        Pend *p = &pend[i];
        if (now < p->at) continue;
        if (p->st == 0) {
            bool can = (st == CHAR_STATE_NORMAL || st == CHAR_STATE_ATTACKING || st == CHAR_STATE_SKILL) && character.stamina > 0.0f;
            if (st == CHAR_STATE_SKILL && (tower_skill_form(SKILL_HALFMOON_SLASH) == SKILL_FORM_B && F.castSkill == SKILL_HALFMOON_SLASH)) can = false;   // Full Moon: it takes the blow
            if (st == CHAR_STATE_ROLLING) can = false;
            if (can && !rolled) {
                if (st == CHAR_STATE_SKILL) F.castCancel++;
                pressed->a = 1; in->btn.a = 1; in->stick_x = 55 * s_side; in->stick_y = -60;     // back and to a side: out of a combo's reach
                if (frand() < 0.5f) s_side = -s_side;
                F.rolls++; rolled = true; p->st = 1;
#ifdef HB_LOG_BLOWS
                debugf("[hbroll] t %.2f for %.2f (key %d slot %d mean %.2f n %d) st %d stam %.0f\n", (double)now, (double)p->target, p->key, p->slot, (double)tab[p->key][p->slot].mean, tab[p->key][p->slot].n, st, (double)character.stamina);
#endif
            } else if (now > p->at + 0.12f || rolled) p->st = 3;
            else continue;      // (try next frame)
        }
        remember(*p);
        pend[i--] = pend[--npend];
    }
    for (int i = 0; i < npend; i++) if (now > pend[i].target + 0.5f) { remember(pend[i]); pend[i--] = pend[--npend]; }     // (lapsed ones)
    if (rolled) return;
    if (st == CHAR_STATE_ROLLING || st == CHAR_STATE_KNOCKDOWN) return;
    // a roll coming soon: hold still (no new swing, cast or potion)
    float soon = 99.0f;
    for (int i = 0; i < npend; i++) if (pend[i].st == 0 && pend[i].at - now < soon) soon = pend[i].at - now;
    // floor fire under or by the player: step out, away from it
    extern bool floor_fire_near(float x, float z, float r);
    if (floor_fire_near(character.pos[0], character.pos[2], 10.0f) || now < s_fleeUntil) { in->stick_y = -80; in->stick_x = 40 * s_side; F.fireT += dt; return; }
    // open
    if (hle_open_now) { if (s_openSince < 0.0f) { s_openSince = now; s_reactAt = now + HB_REACT + 0.05f * gauss(); } F.openT += dt; }
    else s_openSince = -1.0f;
    float openAge = s_openSince >= 0.0f ? now - s_openSince : -1.0f;
    // a potion: low, nothing coming, in an opening or out of reach (Berserker's are blocked by the game); with plenty held a human
    // drinks sooner, and tops up as a fight starts
    float potAt = HB_POTION_AT + (tower_circle() >= 5 ? 0.1f : 0.0f) + (character.healthPotions >= 4 ? 0.15f : 0.0f);
    if (now - F.t0 < 3.0f && character.healthPotions >= 2) potAt = fmaxf(potAt, 0.70f);
    if (character.health < potAt * character.maxHealth && character.healthPotions > 0 && st == CHAR_STATE_NORMAL && soon > 0.6f && (hle_open_now || d > 55.0f || now - F.t0 < 3.0f) && s_k % 6 == 0) {
        int p0 = character.healthPotions;
        pressed->d_left = 1; in->btn.d_left = 1;
        (void)p0; F.potions++;
        return;
    }
    float reach = boss->phaseIndex == 5 ? 60.0f : 40.0f;     // (Shadowborne's position is its hips: its body is long)
    if (d > reach - 6.0f && st != CHAR_STATE_SKILL) in->stick_y = 70;
    else if (d < 16.0f && st == CHAR_STATE_NORMAL) in->stick_y = -50;
    // a window: the boss between its moves (the metric's openings), or no hit of its move due for a while (a long wind-up, a
    // recovery, a walk: what a human who knows the move punishes), its tell seen HB_REACT ago
    bool inOpen = openAge >= 0.0f && now >= s_reactAt;
    bool safe = now - recT[0] >= HB_REACT && soon > 0.7f;
    if ((inOpen || safe) && soon <= 0.5f) F.whySoon++;
    if ((inOpen && soon > 0.5f) || safe) {
        int k = st == CHAR_STATE_NORMAL && d < 95.0f && (inOpen || soon > 2.2f) ? pick_skill(boss, inOpen ? openAge : 0.0f, d) : -1;
        if (k >= 0 && character.stamina >= 25.0f) {
            if (k == 0) { pressed->d_up = 1; in->btn.d_up = 1; } else if (k == 1) { pressed->d_right = 1; in->btn.d_right = 1; } else { pressed->d_down = 1; in->btn.d_down = 1; }
            F.castSkill = k;
            return;
        }
        if (d >= reach) F.whyFar++; else if (character.stamina < 24.0f) F.whyStam++; else if (st != CHAR_STATE_NORMAL && st != CHAR_STATE_ATTACKING) F.whyState++; else F.whyOk++;
        if (d < reach && character.stamina >= 18.0f + 6.0f && now - s_lastSwing > 0.16f && (st == CHAR_STATE_NORMAL || st == CHAR_STATE_ATTACKING)) {
            pressed->b = 1; in->btn.b = 1; s_lastSwing = now; F.swings++;
        }
    }
}


// The next run: its seed, plan and route (HLE_HB_SEED0, HLE_HB_PLANS, HLE_HB_ROUTES); with every unlock off
// (HB_UNLOCK_ALL=0) the Embers banked are spent at the Shrine first, the cheapest thing still to own each time
// (packs and B forms, the upgrades, the relics, the Nightfarer): the unlock pacing ([hbshrine])
#ifndef HLE_HB_SEED0
#define HLE_HB_SEED0 1000
#endif
#ifndef HLE_HB_PLANS
#define HLE_HB_PLANS 0,1,2,3,4,5,6,7,8,9
#endif
#ifndef HLE_HB_ROUTES
#define HLE_HB_ROUTES 2
#endif
extern int hb_unlock_all;
static void shrine_spend(void)
{
    for (;;) {
        int best = -1, cost = 1 << 30, kind = 0;
        for (int i = 0; i < tower_pack_count(); i++) { int c = tower_pack_cost(i); if (c >= 0 && c < cost) { cost = c; best = i; kind = 0; } }
        for (int u = 0; u < UPG_COUNT; u++) { int c = tower_upgrade_cost((UpgradeId)u); if (c >= 0 && c < cost) { cost = c; best = u; kind = 1; } }
        for (int r = 0; r < tower_relic_total(); r++) { int c = tower_relic_cost(r); if (c >= 0 && c < cost) { cost = c; best = r; kind = 2; } }
        { int c = tower_nightfarer_cost(); if (c >= 0 && c < cost) { cost = c; best = 0; kind = 3; } }
        if (best < 0) { debugf("[hbshrine] run %d: everything owned, Embers %d\n", hb_runs, tower_embers()); return; }
        if (tower_embers() < cost) { debugf("[hbshrine] run %d: next %s %d costs %d, Embers %d\n", hb_runs, kind == 0 ? "unlock" : kind == 1 ? "upgrade" : kind == 2 ? "relic" : "nightfarer", best, cost, tower_embers()); return; }
        bool ok = kind == 0 ? tower_pack_buy(best) : kind == 1 ? tower_upgrade_buy((UpgradeId)best) : kind == 2 ? tower_relic_select(best) : tower_nightfarer_buy();
        debugf("[hbshrine] run %d: bought %s %d for %d (%s), Embers %d\n", hb_runs, kind == 0 ? "unlock" : kind == 1 ? "upgrade" : kind == 2 ? "relic" : "nightfarer", best, cost, ok ? "ok" : "FAILED", tower_embers());
        if (!ok) return;
    }
}
void hb_next_run(void)
{
    static const int P[] = { HLE_HB_PLANS }, R[] = { HLE_HB_ROUTES };
    int np = sizeof P / sizeof P[0], nr = sizeof R / sizeof R[0];
    hb_plan = P[hb_runs % np];
    hb_route = R[(hb_runs / np) % nr];
    hb_seed = (uint32_t)(HLE_HB_SEED0 + 7919 * hb_runs) | 1u;
    if (!hb_unlock_all) shrine_spend();
    debugf("[hbrun] run %d next: plan %d route %d seed %08lx | best floor %d, Embers %d, fight time so far %.0f s\n", hb_runs, hb_plan, hb_route, (unsigned long)hb_seed,
           tower_best_floor(), tower_embers(), (double)sessionT);
}
bool hb_fight_on(void) { return F.on; }
float hb_session_fight_s(void) { return sessionT; }
#endif
