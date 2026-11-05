/**
 * Test-only input script (build with HLE_EXTRA_CFLAGS=-DHLE_AUTOPLAY): taps A to
 * walk through the menus so automated emulator runs reach gameplay.
 */
#ifdef HLE_AUTOPLAY
#include <libdragon.h>
#include <string.h>
#if defined(HLE_LOG_HITVOL) || defined(HLE_LOG_HITGAP)
#include <t3d/t3dmodel.h>
#include <t3d/t3dskeleton.h>
#endif
#ifndef HLE_STRIKE_EVERY
#define HLE_STRIKE_EVERY 24     // frames between presses (fewer chains the combo)
#endif
#ifdef HLE_TEST_TOWER     // the Unbroken check hits the player
#include "character.h"
#endif
#if defined(HLE_TEST_PACKS) || defined(HLE_LOG_TOWERSAVE) || defined(HLE_TEST_SKILLS) || defined(HLE_TEST_SKILL_DMG) || defined(HLE_TEST_TOGGLE) || defined(HLE_TEST_BOSS2) || defined(HLE_TEST_TWO_RUNS) || defined(HLE_TEST_BONUS) || defined(HLE_TEST_ENDING) || defined(HLE_TEST_HEAVY) || defined(HLE_TEST_DUMMY) || defined(HLE_TEST_COVER) || defined(HLE_TEST_ROLLCANCEL) || defined(HLE_TEST_SKIN) || defined(HLE_TEST_UNBOUND) || defined(HLE_TEST_CRUCIFIX) || defined(HLE_TEST_TOWER_RUN) || defined(HLE_TEST_TOWER_UI) || defined(HLE_TEST_TOWER_STATS) || defined(HLE_TEST_TOWER_NAV) || defined(HLE_TOWER_GALLERY) || defined(HLE_TEST_SKIN_TITLE) || defined(HLE_TEST_SKIN_SAVE) || defined(HLE_TEST_SKIN_SLOT) || defined(HLE_TEST_SAVE_FLOW) || defined(HLE_TEST_SAVE_FRESH) || defined(HLE_BOOT_SKIN) || defined(HLE_TEST_LOADMENU) || defined(HLE_TEST_MARKS) || defined(HLE_TEST_BKEY) || defined(HLE_TEST_ANIMVIEW) || defined(HLE_TEST_GETUP) || defined(HLE_TEST_OOB) || defined(HLE_TEST_SHRINE) || defined(HLE_TEST_CONTROLS) || defined(HLE_TEST_CYCLE) || defined(HLE_TEST_MENUS) || defined(HLE_SHOW_HURTBOX)
#include "camera_controller.h"
#include "joypad_utility.h"
#include "character_skin.h"
#include <math.h>
#include "character.h"
#include "fx/skill_fx.h"
#include "game/boss/boss.h"
#include "controllers/menu_controller.h"
#include "game/boss/environmental_mechanics/multi_sword_attacks.h"
#include "scenes/guardian/guardian_scene_bounds.h"
#ifdef HLE_SHOW_HURTBOX
#include "utilities/game_math.h"
#endif
#endif
#if defined(HLE_TEST_PACKS) || defined(HLE_LOG_TOWERSAVE) || defined(HLE_TEST_AUDIO) || defined(HLE_TEST_SHRINE) || defined(HLE_TEST_LOADMENU) || defined(HLE_TEST_MARKS) || defined(HLE_TEST_QUIT) || defined(HLE_TEST_TOWER_RUN) || defined(HLE_TEST_TOWER_UI) || defined(HLE_TEST_TOWER_STATS) || defined(HLE_TEST_TOWER_NAV) || defined(HLE_TOWER_GALLERY) || defined(HLE_TEST_SKIN_TITLE) || defined(HLE_TEST_SKIN_SAVE) || defined(HLE_TEST_SKIN_SLOT) || defined(HLE_TEST_SAVE_FLOW) || defined(HLE_TEST_SAVE_FRESH) || defined(HLE_TEST_MENUS)
#include "utilities/save_controller.h"
#include "controllers/scene_controller.h"
#include "controllers/menu_controller.h"
#include "controllers/audio_controller.h"
#include "scenes/guardian/guardian_scene_sfx.h"
#include "game/boss/boss.h"
#endif

#include "utilities/ui_pages.h"
#include <string.h>
int hle_task_log;
float hle_dummy_damage;
int hle_dummy_hits;
int hle_build_breaks;       // (HLE_DUMMY_BUILD) casts broken by a blow
// Openings (HLE_TEST_DUMMY): time a Bonus boss spends not attacking, and how many such gaps
float hle_open_time, hle_open_cur, hle_open_dmg;
int hle_open_gaps;
bool hle_open_now;      // the boss is between attacks this frame (the bot playtest strikes then)
void hle_open_tick(bool open, float dt)
{
#ifdef HLE_TEST_DUMMY   // (a Smouldering Elite: an opening with its fire burning by the boss is no opening: the sword can't reach it from there)
    { extern bool floor_fire_near(float, float, float); Boss *b = boss_get_instance(); if (open && b && floor_fire_near(b->pos[0], b->pos[2], 26.0f)) open = false; }
#endif
    hle_open_now = open;
    if (open) { hle_open_time += dt; hle_open_cur += dt; }
    else if (hle_open_cur > 0.0f) {
        hle_open_gaps++;
        hle_open_dmg += fmaxf(0.0f, hle_open_cur - 0.4f) / 0.65f * 4.0f;     // the metric's sword in it: 4 a 0.65 s, after a 0.4 s reaction
        hle_open_cur = 0.0f;
    }
}

#ifdef HLE_TEST_FOG_AB
bool hle_fog_ab_cpu;
#endif
#ifdef HLE_TEST_LEAK
// Test aid (with HLE_TEST_CYCLE; link with HLE_EXTRA_LDFLAGS="--wrap malloc --wrap free --wrap calloc
// --wrap realloc --wrap memalign"): every live allocation's size, caller and cycle; hle_leak_dump
// lists the ones made in a cycle that are still held cycles later (what each title return leaks)
#define LK_N 16384
static struct { void *p; uint32_t size, ra; int16_t cyc; } lk[LK_N];
int hle_leak_cyc;
void *__real_malloc(size_t), __real_free(void *), *__real_calloc(size_t, size_t), *__real_realloc(void *, size_t), *__real_memalign(size_t, size_t);
static void lk_add2(void *p, size_t size, void *ra, int cyc)
{
    if (!p) return;
    uint32_t h = ((uint32_t)p >> 3) % LK_N;
    for (int n = 0; n < LK_N; n++, h = (h + 1) % LK_N)
        if (!lk[h].p) { lk[h].p = p; lk[h].size = size; lk[h].ra = (uint32_t)ra; lk[h].cyc = cyc; return; }
}
static void lk_add(void *p, size_t size, void *ra) { lk_add2(p, size, ra, hle_leak_cyc); }
static void lk_del(void *p)
{
    if (!p) return;
    uint32_t h = ((uint32_t)p >> 3) % LK_N;
    for (int n = 0; n < LK_N && lk[h].p; n++, h = (h + 1) % LK_N)
        if (lk[h].p == p) {     // re-place the run after it (linear probing)
            lk[h].p = NULL;
            for (uint32_t j = (h + 1) % LK_N; lk[j].p; j = (j + 1) % LK_N) { typeof(lk[0]) e = lk[j]; lk[j].p = NULL; lk_add2(e.p, e.size, (void *)e.ra, e.cyc); }
            return;
        }
}
// The lowest free heap ever, per tag (hle_leak_cyc), caught at the allocation that made it
int hle_low_kb[64]; static uint32_t lowRa[64], lowSize[64];
static void lk_low(size_t s, void *ra)
{
    int t = hle_leak_cyc & 63; heap_stats_t h; sys_get_heap_stats(&h); int f = (h.total - h.used) / 1024;
    if (!hle_low_kb[t] || f < hle_low_kb[t]) { hle_low_kb[t] = f; lowRa[t] = (uint32_t)ra; lowSize[t] = s; }
}
void hle_low_report(int t) { debugf("[low] tag %d: least free %d KB, at a %lu-byte allocation from %08lx\n", t, hle_low_kb[t & 63], (unsigned long)lowSize[t & 63], (unsigned long)lowRa[t & 63]); }
void *__wrap_malloc(size_t s) { disable_interrupts(); void *p = __real_malloc(s); lk_add(p, s, __builtin_return_address(0)); lk_low(s, __builtin_return_address(0)); enable_interrupts(); return p; }
void *__wrap_calloc(size_t a, size_t b) { disable_interrupts(); void *p = __real_calloc(a, b); lk_add(p, a * b, __builtin_return_address(0)); lk_low(a * b, __builtin_return_address(0)); enable_interrupts(); return p; }
void *__wrap_memalign(size_t a, size_t s) { disable_interrupts(); void *p = __real_memalign(a, s); lk_add(p, s, __builtin_return_address(0)); lk_low(s, __builtin_return_address(0)); enable_interrupts(); return p; }
void *__wrap_realloc(void *o, size_t s) { disable_interrupts(); lk_del(o); void *p = __real_realloc(o, s); lk_add(p, s, __builtin_return_address(0)); lk_low(s, __builtin_return_address(0)); enable_interrupts(); return p; }
// Every live allocation, totalled by caller (blocks of 4 KB and up listed on their own)
void hle_leak_by_caller(const char *why)
{
    static uint32_t ra[512], tot[512]; static int cnt[512]; int n = 0; uint32_t all = 0;
    for (int i = 0; i < LK_N; i++) if (lk[i].p) {
        all += lk[i].size;
        if (lk[i].size >= 4096) debugf("[bycaller] %s big %p %lu from %08lx (tag %d)\n", why, lk[i].p, (unsigned long)lk[i].size, (unsigned long)lk[i].ra, lk[i].cyc);
        int k = 0; while (k < n && ra[k] != lk[i].ra) k++;
        if (k == n) { if (n == 512) continue; ra[n] = lk[i].ra; tot[n] = 0; cnt[n] = 0; n++; }
        tot[k] += lk[i].size; cnt[k]++;
    }
    for (int k = 0; k < n; k++) if (tot[k] >= 1024) debugf("[bycaller] %s %08lx %lu bytes in %d\n", why, (unsigned long)ra[k], (unsigned long)tot[k], cnt[k]);
    heap_stats_t h; sys_get_heap_stats(&h);
    debugf("[bycaller] %s total %lu bytes live, heap %d KB free\n", why, (unsigned long)all, (h.total - h.used) / 1024);
}
void *__real_malloc_uncached(size_t), *__real_asset_load(const char *, int *);
static uint32_t lkBlockRa;     // a block being recorded: its buffers are its caller's
void *__wrap_malloc_uncached(size_t s) { void *p = __real_malloc_uncached(s); disable_interrupts(); uint32_t ra = lkBlockRa ? lkBlockRa : (uint32_t)__builtin_return_address(0);
    for (int i = 0; i < LK_N; i++) if (lk[i].p && (uint32_t)lk[i].p == ((uint32_t)p & 0xDFFFFFFF)) lk[i].ra = ra;  // uncached pointer: the cached block it came from
    enable_interrupts();
    if (!lkBlockRa && s >= 8192) { extern bool tower_between_open(void), tower_draft_is_open(void), tower_map_is_open(void); debugf("[qgrow] a draw queue grew %u bytes (tag %d, %s)\n", (unsigned)s, hle_leak_cyc, tower_draft_is_open() ? "the draft" : tower_map_is_open() ? "the map" : tower_between_open() ? "a Rest, Shop or Event" : "in a fight"); }   // (no block recording: the frame's draw queue)
    return p; }
void *__real_malloc_uncached_aligned(int, size_t);
void *__wrap_malloc_uncached_aligned(int a, size_t s) { void *p = __real_malloc_uncached_aligned(a, s); disable_interrupts(); uint32_t ra = (uint32_t)__builtin_return_address(0);
    for (int i = 0; i < LK_N; i++) if (lk[i].p && (uint32_t)lk[i].p == ((uint32_t)p & 0xDFFFFFFF)) lk[i].ra = ra;
    enable_interrupts(); return p; }
void *__real_rspq_block_end(void), __real_rspq_block_begin_reuse(rspq_block_t *);
void __wrap_rspq_block_begin_reuse(rspq_block_t *r) { lkBlockRa = (uint32_t)__builtin_return_address(0); __real_rspq_block_begin_reuse(r); }
void __real_rspq_block_begin(void);
void __wrap_rspq_block_begin(void) { lkBlockRa = (uint32_t)__builtin_return_address(0); __real_rspq_block_begin(); }   // (link with --wrap rspq_block_begin: a block's later buffers are its recorder's too)
void *__wrap_rspq_block_end(void) { void *b = __real_rspq_block_end(); disable_interrupts(); uint32_t ra = (uint32_t)__builtin_return_address(0); lkBlockRa = 0;
    for (int i = 0; i < LK_N; i++) if ((uint32_t)lk[i].p == ((uint32_t)b & 0xDFFFFFFF)) lk[i].ra = ra;
    enable_interrupts(); return b; }
void *__wrap_asset_load(const char *f, int *sz) { void *p = __real_asset_load(f, sz); debugf("[leak] asset_load %s -> %p (cyc %d, from %08lx)\n", f, p, hle_leak_cyc, (unsigned long)__builtin_return_address(0)); return p; }
void __wrap_free(void *p) { disable_interrupts(); lk_del(p); __real_free(p); enable_interrupts(); }
void hle_leak_dump(int from, int to)
{
    uint32_t total = 0; int n = 0;
    for (int i = 0; i < LK_N; i++) if (lk[i].p && lk[i].cyc >= from && lk[i].cyc < to) {
        total += lk[i].size; n++;
        debugf("[leak] cyc %d %p %lu bytes from %08lx\n", lk[i].cyc, lk[i].p, (unsigned long)lk[i].size, (unsigned long)lk[i].ra);
    }
    debugf("[leak] cycles %d-%d: %d blocks, %lu bytes still held\n", from, to - 1, n, (unsigned long)total);
}
#endif
#if defined(HLE_TEST_TOWER_RUN) || defined(HLE_TEST_TOWER_UI) || defined(HLE_TEST_TOWER_STATS)
// Test aid: the Tower's Shop and Event screens in a scripted climb, a press every 15 frames once each is up.
// An Event: HLE_EVENT_CHOICE (1-based; default the first it can take), then Continue. A Shop: B leaves at
// once; with HLE_TEST_TOWER_SHOP it buys every item by the shelf's presses (the D-pad and the stick in
// turn): a card with too little gold (refused), then with enough, a level, a "+", the pact, potions past
// the run's max (refused), Mend twice (once sold), Release, Up/Down along the rows, then B ([shopwalk] lines;
// tower.c logs each [shop] buy). Each purchase's confirm: the first cancelled by B, the second by its Back row (A),
// then Up (to Buy) and A, and the A that opened them pressed again. In a Shop's or Temper's list, A takes the first row
#ifdef HLE_LOG_MOVE_MS
extern uint32_t dragon_vi_frame(void);
#define SCR_VI() ((int)dragon_vi_frame())
#else
#define SCR_VI() -1
#endif
void hle_tower_screen(joypad_inputs_t *in, joypad_buttons_t *pressed, int frame)
{
    extern int tower_screen(void), tower_shop_sel(void), tower_list_sel(void), tower_gold(void);
    extern bool tower_event_done(void);
    extern void tower_test_set_gold(int);
    enum { SCR_SHOP = 5, SCR_SHOP_LEVEL, SCR_SHOP_PLUS, SCR_SHOP_RELEASE, SCR_EVENT };     // (tower.c's)
    static int scr = -99, since, dirs;
    int now = tower_screen();
    if (now != scr) { scr = now; since = 0; debugf("[shopwalk] screen %d up (frame %d, VI %d)\n", now, frame, SCR_VI()); }
    if (++since < 60 || (since - 60) % 15) return;      // past the screen's input delay
    #define PRESS(f) do { pressed->f = 1; in->btn.f = 1; } while (0)
    extern bool tower_leave_asking(void);
    static int la;
    if (tower_leave_asking()) {     // Leave's confirm (a Shop's B, an Event's Leave): Up to Leave, then A
        if (la++ % 2 == 0) PRESS(d_up); else PRESS(a);
        debugf("[shopwalk] leave confirm: %s (frame %d, VI %d)\n", la % 2 ? "Up" : "A on Leave", frame, SCR_VI());
        return;
    }
    la = 0;
    if (now == SCR_EVENT) {
#if defined(HLE_EVENT_CHOICE) && HLE_EVENT_CHOICE > 0
        if (!tower_event_done() && tower_list_sel() < HLE_EVENT_CHOICE - 1) { PRESS(d_down); return; }
#endif
        debugf("[shopwalk] event: A on row %d (%s, frame %d, VI %d)\n", tower_list_sel() + 1, tower_event_done() ? "Continue" : "its choice", frame, SCR_VI());
        PRESS(a);
        return;
    }
#ifdef HLE_TEST_TOWER_SHOP
    static int asked, cs, k;     // (k: the shelf's script below)
    if (ui_details()->open && ui_details()->confirm && now != 10) {      // a purchase's confirm (10: the map's node page)
        if (asked < 2) { if (asked++ == 0) PRESS(b); else PRESS(a); k--; }      // B, then its Back row (A): cancelled; that A again
        else if (cs++ % 2 == 0) { if (dirs++ % 2) in->stick_y = 70; else PRESS(d_up); }      // Up: Buy
        else PRESS(a);
        debugf("[shopwalk] confirm: %s, gold %d (frame %d)\n", asked <= 2 && cs == 0 ? (asked == 1 ? "B" : "A on Back") : cs % 2 ? "Up" : "A on Buy", tower_gold(), frame);
        return;
    }
    cs = 0;
#endif
    if (now != SCR_SHOP) { PRESS(a); return; }      // a list: its first row
#ifdef HLE_TEST_TOWER_SHOP
    static const char seq[] = "gAGADARARADLLAAAARAARAUDDB";
    if (!seq[k]) return;
    char c = seq[k++];
    if (c == 'g') { tower_test_set_gold(40); character.health = character.maxHealth * 0.5f; character.healthPotions = 1; }     // too little for a card; hurt and low, for Mend and potions
    else if (c == 'G') tower_test_set_gold(600);
    else if (c == 'A') PRESS(a);
    else if (c == 'B') PRESS(b);
    else if (dirs++ % 2) { if (c == 'L') in->stick_x = -70; else if (c == 'R') in->stick_x = 70; else if (c == 'U') in->stick_y = 70; else in->stick_y = -70; }
    else { if (c == 'L') PRESS(d_left); else if (c == 'R') PRESS(d_right); else if (c == 'U') PRESS(d_up); else PRESS(d_down); }
    debugf("[shopwalk] '%c'%s on item %d, gold %d (frame %d, VI %d)\n", c, (c == 'L' || c == 'R' || c == 'U' || c == 'D') ? (dirs % 2 ? " D-pad" : " stick") : "",
           tower_shop_sel(), tower_gold(), frame, SCR_VI());
#else
    PRESS(b);
#endif
    #undef PRESS
}
#endif
#ifdef HLE_MAP_WALK
// Test aid (with HLE_TEST_TOWER_RUN): a path through the Tower's map that enters a Shop, an Event, a Rest in
// rows 2-5 and an Elite when it can, once each (else a Fight), moving with the D-pad and the stick in turn, Up in between (it enters
// nothing), then A (the node's page) and A (in); at each Rest, two of HLE_REST_SEQ's letters: H heal, T Temper (the first on its list),
// P potions, then k (keep the boons) or n (release the n-th on the list: slot order)
#ifndef HLE_REST_SEQ
#define HLE_REST_SEQ "T1Hk"
#endif
#ifdef HLE_LOG_MOVE_MS     // (Mupen's shot numbers for each screen)
extern uint32_t dragon_vi_frame(void);
#define WALK_VI() ((int)dragon_vi_frame())
#else
#define WALK_VI() -1
#endif
void hle_map_walk(joypad_inputs_t *in, joypad_buttons_t *pressed, int frame)
{
    extern int tower_screen(void), tower_map_row(void), tower_map_sel(void), tower_map_next(int), tower_list_sel(void);
    enum { NODE_FIGHT = 1, NODE_ELITE = 2, NODE_EVENT = 3, NODE_SHOP = 4, NODE_REST = 5, SCR_MAP = 1, SCR_REST, SCR_TEMPER, SCR_RELEASE, SCR_SHOP };     // (tower.c's)
    static const char seq[] = HLE_REST_SEQ;
    static int scr = -99, row = -99, since, target, moves, enters, rp, want; static bool sawRest, sawElite, sawShop, sawEvent;
    int now = tower_screen(), r = tower_map_row();
    if (now == 10) { if (frame % 12 == 0) { pressed->a = 1; in->btn.a = 1; debugf("[walk] the node's page: A enters (frame %d)\n", frame); } return; }     // (SCR_CONFIRM)
    if (now >= SCR_SHOP) { hle_tower_screen(in, pressed, frame); return; }      // a Shop's screens, an Event's
    if (now != scr || r != row) { scr = now; row = r; since = 0; want = -1; }
    if (++since < 60) return;       // past the screen's input delay
    bool tick = (since - 60) % 12 == 0;
    if (now == SCR_MAP) {
        if (since == 60) {
            target = -1;
            static const int PREF[5] = { NODE_SHOP, NODE_EVENT, NODE_REST, NODE_ELITE, NODE_FIGHT };
            for (int p = 0; p < 5 && target < 0; p++) {
                if ((PREF[p] == NODE_REST && (sawRest || r + 1 < 1 || r + 1 > 4)) || (PREF[p] == NODE_ELITE && sawElite)
                    || (PREF[p] == NODE_SHOP && sawShop) || (PREF[p] == NODE_EVENT && sawEvent)) continue;
                for (int l = 0; l < 3; l++) if (tower_map_next(l) == PREF[p]) { target = l; break; }
            }
            debugf("[walk] map row %d next: %d %d %d, cursor %d, going to %d (frame %d, VI %d)\n", r + 2, tower_map_next(0), tower_map_next(1), tower_map_next(2), tower_map_sel(), target, frame, WALK_VI());
        }
        if (!tick) return;
        int sel = tower_map_sel();
        if (target >= 0 && sel != target) {     // a step along the row: the D-pad, then the stick
            bool right = target > sel;
            if (moves++ % 2) in->stick_x = right ? 70 : -70;
            else if (right) { pressed->d_right = 1; in->btn.d_right = 1; } else { pressed->d_left = 1; in->btn.d_left = 1; }
            debugf("[walk] %s %s (frame %d)\n", moves % 2 ? "D-pad" : "stick", right ? "right" : "left", frame);
            return;
        }
        int t = target >= 0 ? tower_map_next(target) : tower_map_next(sel);
        if (t == NODE_REST && r + 1 >= 1 && r + 1 <= 4) sawRest = true;
        if (t == NODE_ELITE) sawElite = true;
        if (t == NODE_SHOP) sawShop = true;
        if (t == NODE_EVENT) sawEvent = true;
        if (enters++ % 2 && row == r) { pressed->d_up = 1; in->btn.d_up = 1; debugf("[walk] Up: enters nothing (frame %d)\n", frame); return; }   // (pick then confirm: Up never enters)
        pressed->a = 1; in->btn.a = 1;
        debugf("[walk] A: the node's page (frame %d)\n", frame);
        return;
    }
    if (now == SCR_REST && since == 60) character.health = character.maxHealth * 0.5f;    // hurt (instant kills cost nothing): the heal shows
    if (!tick || !seq[rp]) { if (tick) { pressed->a = 1; in->btn.a = 1; } return; }
    if (want < 0) {
        char c = seq[rp];
        want = now == SCR_REST ? (c == 'H' ? 0 : c == 'T' ? 1 : 2) : now == SCR_RELEASE ? (c == 'k' ? 0 : c - '0') : 0;
        debugf("[walk] rest screen %d: '%c', row %d (frame %d, VI %d)\n", now, now == SCR_TEMPER ? 'A' : c, want, frame, WALK_VI());
    }
    if (tower_list_sel() < want) { pressed->d_down = 1; in->btn.d_down = 1; return; }
    pressed->a = 1; in->btn.a = 1;
    if (now != SCR_TEMPER) rp++;
}
#endif

void hle_autoplay(joypad_inputs_t *in, joypad_buttons_t *pressed)
{
    static int frame;
    frame++;
#ifdef HLE_LOG_HITGAP
    { extern int hle_hg_frame; extern void hle_hg_tick(void); hle_hg_frame = frame; hle_hg_tick(); }
#endif
#ifdef HLE_SHOW_HURTBOX
    { extern int hle_hb_frame; hle_hb_frame = frame; }
#endif
#ifdef HLE_TEST_DIRTY_HEAP  // test aid: freed heap left full of garbage (finds memory used before it is set)
    if (frame == 400 || frame == 900) {
        void *p[64]; int n = 0;
        for (; n < 64; n++) { p[n] = malloc(16 * 1024); if (!p[n]) break; memset(p[n], 0xA5, 16 * 1024); }
        for (int i = 0; i < n; i++) free(p[i]);
        debugf("[dirty] %d KB of freed heap set to A5\n", n * 16);
    }
#endif
#ifdef HLE_TEST_MEMBENCH    // test aid: emulated cost of cached vs uncached loads/stores, and of plain ALU work
    if (frame == 60) {
        static uint32_t buf[4096] __attribute__((aligned(16)));
        volatile uint32_t *c = buf, *u = UncachedAddr(buf);
        uint32_t s = 0, t;
        t = TICKS_READ(); for (int i = 0; i < 4096; i++) s += c[i]; t = TICKS_READ() - t; debugf("[membench] cached load   %lu ticks / 4096\n", t);
        t = TICKS_READ(); for (int i = 0; i < 4096; i++) s += u[i]; t = TICKS_READ() - t; debugf("[membench] uncached load %lu ticks / 4096\n", t);
        t = TICKS_READ(); for (int i = 0; i < 4096; i++) c[i] = i; t = TICKS_READ() - t; debugf("[membench] cached store  %lu ticks / 4096\n", t);
        t = TICKS_READ(); for (int i = 0; i < 4096; i++) u[i] = i; t = TICKS_READ() - t; debugf("[membench] uncached store %lu ticks / 4096\n", t);
        t = TICKS_READ(); for (volatile int i = 0; i < 4096; i++) s ^= s << 1; t = TICKS_READ() - t; debugf("[membench] alu loop     %lu ticks / 4096 (%lu)\n", t, s);
    }
#endif
#ifdef HLE_TEST_SPRITE_COST     // test aid: the heap each of these sprites holds once loaded
    if (frame == 300) {
        static const char *const P[] = { "rom:/buttons/B.sprite", "rom:/buttons/Z.sprite", "rom:/buttons/Start.sprite", "rom:/buttons/StickTexture.sprite", "rom:/buttons/CLeft.sprite", "rom:/buttons/Dpad.sprite", "rom:/buttons/A.sprite" };
        for (unsigned i = 0; i < sizeof P / sizeof P[0]; i++) {
            heap_stats_t h0, h1; sys_get_heap_stats(&h0); sprite_t *sp = sprite_load(P[i]); sys_get_heap_stats(&h1);
            debugf("[spritecost] %s: %d bytes (%dx%d format %d)\n", P[i], h1.used - h0.used, sp->width, sp->height, sprite_get_format(sp));
            sprite_free(sp);
        }
    }
#endif
#ifdef HLE_OVERSCAN_X      // test aid: the Video menu's overscan setting (X and Y: -15 the smallest, 0 the 7.5% title-safe margin, more moves the HUD in)
    { extern int8_t uiOverscanX, uiOverscanY; uiOverscanX = HLE_OVERSCAN_X; uiOverscanY = HLE_OVERSCAN_Y; }
#endif
#ifdef HLE_PAD_LAYOUT       // test aid: play in this button layout (0 Classic, 1 A/B Swapped, 2 Skills on C)
    { extern void joypad_set_layout(int); joypad_set_layout(HLE_PAD_LAYOUT); }     // every frame: loading the save sets it too
#endif
#ifdef HLE_SKILLS_OFF   // the Skills setting off throughout (a loaded save can't turn it back on)
    { extern void character_set_skills_enabled(bool); character_set_skills_enabled(false); }
#endif
#ifdef HLE_TEST_T3D_EXACT
    if (frame % 300 == 0) { extern uint32_t hle_exact_calls, hle_exact_strips, hle_exact_bad; debugf("[exact] %lu vertex loads, %lu strips, %lu mismatched\n", (unsigned long)hle_exact_calls, (unsigned long)hle_exact_strips, (unsigned long)hle_exact_bad); }
#endif
#ifdef HLE_TEST_ARC_REACH   // test aid: when the boss goes out of Halfmoon's reach and back (its height, its state)
    {
        extern bool skill_arc_reaches(const struct Boss *b, float arcY); extern float deltaTime;
        Boss *bb = boss_get_instance();
        static int was = -1; static float outT, allT;
        if (bb && bb->mode == BOSS_MODE_COMBAT && bb->health > 0.0f) {
            int r = skill_arc_reaches(bb, character.pos[1] + 10.0f);
            float low = fminf(bb->capsuleCollider.localCapA.v[1], bb->capsuleCollider.localCapB.v[1]) + bb->pos[1] - bb->capsuleCollider.radius - character.pos[1];
            allT += deltaTime; if (!r) outT += deltaTime;
            if (r != was) { debugf("[reach] boss %d %s at state %d, lowest point %.0f above the floor\n", bb->phaseIndex, r ? "back in reach" : "OUT of reach", bb->state, (double)low); was = r; }
            if (frame % 900 == 0) debugf("[reach] out of reach %.0f%% of the fight\n", (double)(outT * 100.0f / fmaxf(allT, 0.01f)));
        }
    }
#endif
#ifdef HLE_LOG_HURTBOX     // test aid: the hit capsule against the model's bones (height span, and how far the body leans out of it)
    {
        Boss *bb = boss_get_instance();
        if (bb && bb->mode == BOSS_MODE_COMBAT && bb->phaseIndex <= 3 && frame % 10 == 0) {     // (Shadowborne logs its own: dragon_boss.c)
            static const char *names[] = { "Head", "Waist", "LowerLeg-Left", "LowerLeg-Right" };
            float lo = fminf(bb->capsuleCollider.localCapA.v[1], bb->capsuleCollider.localCapB.v[1]) - bb->capsuleCollider.radius;
            float hi = fmaxf(bb->capsuleCollider.localCapA.v[1], bb->capsuleCollider.localCapB.v[1]) + bb->capsuleCollider.radius;
            static float worstOut[4], worstH[4][2] = { { 1e9f, -1e9f }, { 1e9f, -1e9f }, { 1e9f, -1e9f }, { 1e9f, -1e9f } }; static int n;
            for (int i = 0; i < 4; i++) {
                float w[3]; bool ok = false;
                if (bb->phaseIndex <= 2) { T3DVec3 v; extern bool boss_get_bone_world_pos(const Boss*, int, T3DVec3*);
                    int idx = i == 0 ? bb->headBoneIndex : i == 1 ? bb->waistBoneIndex : i == 2 ? bb->lowerLegLeftBoneIndex : bb->lowerLegRightBoneIndex;
                    ok = boss_get_bone_world_pos(bb, idx, &v); w[0] = v.v[0]; w[1] = v.v[1]; w[2] = v.v[2]; }
                else if (bb->phaseIndex == 3) { extern bool fallen_knight_bone(const char*, float*); ok = fallen_knight_bone(names[i], w); }
                if (!ok) continue;
                float rel = w[1] - bb->pos[1], dx = w[0] - bb->pos[0], dz = w[2] - bb->pos[2];
                float out = sqrtf(dx * dx + dz * dz) - bb->capsuleCollider.radius;      // > 0: outside the capsule sideways
                if (out > worstOut[i]) worstOut[i] = out;
                worstH[i][0] = fminf(worstH[i][0], rel); worstH[i][1] = fmaxf(worstH[i][1], rel);
            }
            if (++n % 90 == 0) debugf("[hurtbox] boss %d capsule %.0f..%.0f r %.0f | head %.0f..%.0f out %.0f | waist %.0f..%.0f out %.0f | legs %.0f..%.0f out %.0f\n", bb->phaseIndex, (double)lo, (double)hi, (double)bb->capsuleCollider.radius,
                (double)worstH[0][0], (double)worstH[0][1], (double)worstOut[0], (double)worstH[1][0], (double)worstH[1][1], (double)worstOut[1], (double)fminf(worstH[2][0], worstH[3][0]), (double)fmaxf(worstH[2][1], worstH[3][1]), (double)fmaxf(worstOut[2], worstOut[3]));
        }
    }
#endif
#ifdef HLE_LOG_HITVOL     // test aid: the drawn bodies (every skinned vertex, swords left out) against the hit capsules
    if (frame % 30 == 0) debugf("[f] %d\n", frame);     // (to find a logged moment's frame, for a screenshot)
    if (frame % 6 == 0) {
        extern T3DModel *characterModel;
        extern bool hle_silhouette(const T3DModel *m, const T3DSkeleton *sk, const T3DMat4FP *M, const float o[3], float yaw, float h, float out[7]);
        enum { NS = 1200, NC = 7 };
        static float smp[2][NC][NS]; static int ns[2];
        static const char *cn[NC] = { "legs r", "torso r", "arms r", "head r", "top", "front", "side" };
        Boss *bb = boss_get_instance();
        CharacterState st = character_get_state();
        float o[NC];
        if (bb && bb->mode == BOSS_MODE_COMBAT && bb->health > 0.0f && st != CHAR_STATE_DEAD) {
            // the player at all heights; a boss below 32 above its feet (what a sword on the floor reaches)
            if (ns[0] < NS && hle_silhouette(characterModel, character.skeleton, character.modelMat, character.pos, character.rot[1], 1e9f, o)) {
                for (int c = 0; c < NC; c++) smp[0][c][ns[0]] = o[c];
                ns[0]++;
            }
            bool ok = false;
            if (bb->phaseIndex <= 2) ok = hle_silhouette(bb->model, bb->skeleton, bb->modelMat, bb->pos, bb->rot[1], 32.0f, o);
            else if (bb->phaseIndex == 3) { extern bool fallen_knight_hle_pose(const T3DSkeleton **, const T3DMat4FP **); const T3DSkeleton *sk; const T3DMat4FP *M;
                if (fallen_knight_hle_pose(&sk, &M)) ok = hle_silhouette(characterModel, sk, M, bb->pos, bb->rot[1], 32.0f, o); }
            if (ok && ns[1] < NS) { for (int c = 0; c < NC; c++) smp[1][c][ns[1]] = o[c]; ns[1]++; }
            if (ok && frame % 240 == 0) debugf("[hitvol] boss %d state %d: below 32 legs %.1f torso %.1f arms %.1f head %.1f | top %.1f | capsule r %.0f, %.0f..%.0f\n", bb->phaseIndex, bb->state,
                (double)o[0], (double)o[1], (double)o[2], (double)o[3], (double)o[4], (double)bb->capsuleCollider.radius,
                (double)(fminf(bb->capsuleCollider.localCapA.v[1], bb->capsuleCollider.localCapB.v[1]) - bb->capsuleCollider.radius),
                (double)(fmaxf(bb->capsuleCollider.localCapA.v[1], bb->capsuleCollider.localCapB.v[1]) + bb->capsuleCollider.radius));
        }
        if (frame % 1800 == 0)     // percentiles (50 / 90 / max) of each extent
            for (int w = 0; w < 2; w++) for (int c = 0; c < NC; c++) {
                int n = ns[w]; if (n < 10) continue;
                static float tmp[NS]; memcpy(tmp, smp[w][c], n * sizeof(float));
                for (int i = 1; i < n; i++) { float v = tmp[i]; int j = i - 1; while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; } tmp[j + 1] = v; }
                debugf("[hitvol] %s %s: p50 %.1f p90 %.1f p99 %.1f max %.1f (%d)\n", w ? "boss" : "player", cn[c], (double)tmp[n / 2], (double)tmp[n * 9 / 10], (double)tmp[n * 99 / 100], (double)tmp[n - 1], n);
            }
    }
#endif
#if defined(HLE_LOG_JUMPBODY) && defined(HLE_LOG_HITVOL)    // test aid: the Guardian's/Unbound's drawn body against its hit capsule in its jumps (heights over the player's floor)
    {
        Boss *bb = boss_get_instance();
        extern bool hle_body_yspan(const T3DModel *m, const T3DSkeleton *sk, const T3DMat4FP *M, float out[2]);
        float y[2];
        bool leap = bb && (bb->state == BOSS_STATE_POWER_JUMP || bb->state == BOSS_STATE_FLIP_ATTACK || bb->state == BOSS_STATE_COMBO_ATTACK);
        if (bb && leap && frame % 2 == 0 && bb->mode == BOSS_MODE_COMBAT && bb->phaseIndex <= 2 && hle_body_yspan(bb->model, bb->skeleton, bb->modelMat, y)) {
            float g = character.pos[1], r = bb->capsuleCollider.radius;
            float lo = bb->pos[1] + fminf(bb->capsuleCollider.localCapA.v[1], bb->capsuleCollider.localCapB.v[1]) - r - g;
            float hi = bb->pos[1] + fmaxf(bb->capsuleCollider.localCapA.v[1], bb->capsuleCollider.localCapB.v[1]) + r - g;
            T3DVec3 w; float wy = boss_get_bone_world_pos(bb, bb->waistBoneIndex, &w) ? w.v[1] - g : -999.0f;
            static int fl = -2, fr; if (fl == -2) { fl = t3d_skeleton_find_bone((T3DSkeleton*)bb->skeleton, "Foot-Left"); fr = t3d_skeleton_find_bone((T3DSkeleton*)bb->skeleton, "Foot-Right"); }
            T3DVec3 f1, f2; float fy = boss_get_bone_world_pos(bb, fl, &f1) && boss_get_bone_world_pos(bb, fr, &f2) ? fminf(f1.v[1], f2.v[1]) - g : -999.0f;
                debugf("[jb] f%d boss %d st %d t %.2f origin %.0f | body %.0f..%.0f | capsule %.0f..%.0f | waist %.0f | player %.0f away, state %d | feet %.0f\n", frame, bb->phaseIndex, bb->state, (double)bb->stateTimer,
                       (double)(bb->pos[1] - g), (double)(y[0] - g), (double)(y[1] - g), (double)lo, (double)hi, (double)wy,
                       (double)sqrtf((character.pos[0] - bb->pos[0]) * (character.pos[0] - bb->pos[0]) + (character.pos[2] - bb->pos[2]) * (character.pos[2] - bb->pos[2])), (int)character_get_state(), (double)fy);
        }
    }
#endif
#ifdef HLE_LOG_JUMPS     // test aid: any frame the boss moves further than it could in one step (a teleport)
    {
        Boss *bb = boss_get_instance(); static float last[3]; static bool have;
        if (bb && bb->mode == BOSS_MODE_COMBAT) {
            float dx = bb->pos[0] - last[0], dy = bb->pos[1] - last[1], dz = bb->pos[2] - last[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (have && d > 8.0f) debugf("[jump] boss moved %.1f in one frame (to %.0f %.0f %.0f)\n", (double)d, (double)bb->pos[0], (double)bb->pos[1], (double)bb->pos[2]);
            last[0] = bb->pos[0]; last[1] = bb->pos[1]; last[2] = bb->pos[2]; have = true;
        } else have = false;
    }
#endif
#ifdef HLE_LOG_RELIC    // test aid: the save's relics after boot (does the Shrine's choice survive a reboot?)
    if (frame == 600) { extern int tower_relic_equipped(void); extern bool tower_relic_owned(int); debugf("[relic] after boot: equipped %d, owned%s%s%s%s%s\n", tower_relic_equipped(),
        tower_relic_owned(0) ? " 0" : "", tower_relic_owned(1) ? " 1" : "", tower_relic_owned(2) ? " 2" : "", tower_relic_owned(3) ? " 3" : "", tower_relic_owned(4) ? " 4" : ""); }
#endif
#if defined(HLE_LOG_TOWERSAVE) || defined(HLE_TEST_PACKS)   // test aid: the slot's Tower progress after boot (old saves, unlocks across a reboot)
    if (frame == 150) {
        TowerSave ts; save_controller_get_tower(&ts);
        const SaveData *d = save_controller_get_slot_data(save_controller_get_active_slot());
        debugf("[tsave] slot %d: Embers %d, best floor %d, upgrades %d %d %d %d %d, relics owned %02x equipped %d, flags %02x, unlocks %03x | pad[24..27] %02x %02x %02x %02x\n",
               save_controller_get_active_slot(), ts.embers, ts.bestFloor, ts.levels[0], ts.levels[1], ts.levels[2], ts.levels[3], ts.levels[4],
               ts.relicsOwned, ts.relicEquipped, ts.flags, ts.unlocks, d ? d->_pad[24] : 0, d ? d->_pad[25] : 0, d ? d->_pad[26] : 0, d ? d->_pad[27] : 0);
        extern int tower_pack_count(void); extern bool tower_pack_owned(int); extern const char *tower_pack_name(int);
        for (int i = 0; i < tower_pack_count(); i++) debugf("[tsave] pack %s: %s\n", tower_pack_name(i), tower_pack_owned(i) ? "owned" : "not bought");
        extern const char *tower_offers_line(void); extern int tower_milestone_count(void), tower_milestone_floor(int); extern bool tower_milestone_reached(int);
        extern int tower_relic_equipped(void);
        debugf("[tsave] set aside for the next climb: %s | relic worn %d | milestones reached:", tower_offers_line()[0] ? tower_offers_line() : "none", tower_relic_equipped());
        for (int i = 0; i < tower_milestone_count(); i++) if (tower_milestone_reached(i)) debugf(" %d", tower_milestone_floor(i));
        debugf("\n");
    }
#endif
#ifdef HLE_TEST_PACKS
    // The Shrine's Unlocks on a save with none: 500 Embers; Tower > Shrine > Unlocks (row 7), A, Up, A buys Blood (120: A its
    // confirm, Up its Buy row, A), Down, A, Up, A buys Storm (160), Down x5, the same buys Shattering Dance (200, the first B form), then B, B. A second boot
    // (the same ROM) logs them owned ([tsave]) and buys nothing
    {
        static int phase = 0, t0 = 0, step = 0;
        int t = frame - t0;
        if (phase == 0) {
            if (menu_controller_is_active()) {
                TowerSave ts; save_controller_get_tower(&ts);
                if (!ts.unlocks) { ts.embers = 500; save_controller_set_tower(&ts); phase = 1; } else phase = 3;
                t0 = frame;
            } else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {
            static const char seq[] = "DDADADDDDDDDAAUA.DAUA.DDDDDAUA.B";
            if (t >= 25 && seq[step]) {
                char c = seq[step];
                if (c == 'D') pressed->d_down = 1; else if (c == 'U') pressed->d_up = 1; else if (c == 'B') { pressed->b = 1; in->btn.b = 1; } else if (c == 'A') { pressed->a = 1; in->btn.a = 1; }
                if (c == '.') debugf("[packs] step %d at frame %d\n", step, frame);
                step++; t0 = frame;
            }
            if (!seq[step] && t == 40) {
                extern int tower_embers(void), tower_pack_count(void); extern bool tower_pack_owned(int);
                extern const char *tower_pack_name(int);
                debugf("[packs] owned:"); for (int i = 0; i < tower_pack_count(); i++) if (tower_pack_owned(i)) debugf(" %s", tower_pack_name(i));
                debugf(", Embers %d\n", tower_embers());
                phase = 2;
            }
        }
        return;
    }
#endif
#ifdef HLE_LOG_FPS
    {   // game frames per second, every 2 s (for hardware-path runs in Ares)
        static uint32_t last; static int lastFrame;
        uint32_t now = get_ticks_ms();
        if (now - last >= 2000) { if (last) debugf("[fps] %.1f\n", (double)((frame - lastFrame) * 1000.0f / (now - last))); last = now; lastFrame = frame; }
    }
#endif
#ifdef HLE_TEST_TASKS
    {   // gfx tasks per game frame (a frame should be exactly one)
        extern uint32_t gsp_epoch;
        static uint32_t last;
        extern int hle_task_log;
        hle_task_log = (frame == 300 || frame == 301 || frame == 700 || frame == 701);
        if (hle_task_log) debugf("[tasks] frame %d\n", frame);
        if (frame % 30 == 0) { debugf("[tasks] f%d %.2f per frame\n", frame, (double)(gsp_epoch - last) / 30.0); last = gsp_epoch; }
    }
#endif
#ifdef HLE_TEST_PERF
    {
        extern uint32_t __hle_perf_ticks;
        static uint32_t last, start;
        uint32_t now = TICKS_READ();
        static int minq = 99; static uint32_t maxdt;
        int q = audio_get_queued_buffers();
        if (q < minq) minq = q;
        if (last && now - last > maxdt) maxdt = now - last;
        if (frame % 120 == 0) {
            debugf("[hle] perf: %lu frames in %lu ms, rsp-hle %lu%% of CPU time, audio min queued %d/%d, max frame %lu ms\n", 120UL,
                (unsigned long)TICKS_TO_MS(now - start), (unsigned long)(__hle_perf_ticks * 100ULL / (now - start)),
                minq, audio_get_num_buffers(), (unsigned long)TICKS_TO_MS(maxdt));
            minq = 99; maxdt = 0;
            extern void hle_perf_dump(uint32_t total);
            hle_perf_dump(now - start);
#ifdef HLE_TEST_FOG_AB
            extern bool hle_fog_ab_cpu;
            debugf("[fogab] the frames above: %s fog\n", hle_fog_ab_cpu ? "cpu" : "gpu");
            hle_fog_ab_cpu = !hle_fog_ab_cpu;
#endif
            __hle_perf_ticks = 0; start = now;
        }
        last = now; (void)last;
    }
#endif
#ifdef HLE_TEST_ROLLCANCEL
    // A during an attack, the Halfmoon charge and an airborne Moon Blade Dance
    // (states: 0 normal, 1 rolling, 2 attacking, 9 skill)
    {
        static int start = -1;
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) start = frame;
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
        if (t == 60)  { pressed->b = 1; in->btn.b = 1; }
        if (t == 200) pressed->d_right = 1;
        if (t == 400) pressed->d_up = 1;
        if (t == 70 || t == 230 || t == 440) { pressed->a = 1; in->btn.a = 1; }
        if (t == 69 || t == 72 || t == 229 || t == 232 || t == 439 || t == 442)
            debugf("[rc] t %d state %d\n", t, character_get_state());
        return;
    }
#endif
#ifndef HLE_SKIN_DIST       // the skin check's camera: distance, height, where it looks (README shots: closer)
#define HLE_SKIN_DIST 70.0f
#endif
#ifndef HLE_SKIN_CAM_Y
#define HLE_SKIN_CAM_Y 30.0f
#define HLE_SKIN_LOOK_Y 20.0f
#endif
#ifdef HLE_TEST_SKIN
    // Skin check: HLE_TEST_SKIN=<id>. Title, then the knight
    // runs a circle and stands while the camera orbits: back, side, front every 60 frames.
    {
        static int start = -1;
        if (frame == 100 || start == -1) character_skin_set(HLE_TEST_SKIN);
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) { start = frame; debugf("[skin] start %d\n", frame); }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
#ifdef HLE_SKIN_MENU
        // pause menu > Settings > Skin, then step the skin right twice (pause menu: Resume, Restart, Settings)
        {
            static const char seq[] = "SDDADDDDRR";
            if (t >= 30 && t % 15 == 0 && t / 15 - 2 < (int)sizeof(seq) - 1) {
                char c = seq[t / 15 - 2];
                if (c == 'S') { pressed->start = 1; in->btn.start = 1; }
                if (c == 'D') pressed->d_down = 1;
                if (c == 'A') { pressed->a = 1; in->btn.a = 1; }
                if (c == 'R') pressed->d_right = 1;
            }
            return;
        }
#endif
        cameraLockOnActive = false;
        if (t == 1) { character.pos[0] = boss->pos[0] - 400.0f; character.pos[2] = boss->pos[2]; character_skin_reset_cloth(); }
#ifndef HLE_SKIN_GATE_ROT
#define HLE_SKIN_GATE_ROT 5.06f   // three-quarter front
#endif
#ifdef HLE_SKIN_GATE      // README shots: on the fog gate's landing (x ~ -700), turned HLE_SKIN_GATE_ROT, the camera along +x
        {
            // the height only placed once: pinned every frame, he falls and is put back, and the cape is dragged down past his feet
            character.pos[0] = HLE_SKIN_GATE; character.pos[2] = 0.0f; character.rot[1] = HLE_SKIN_GATE_ROT;
#ifdef HLE_SKIN_GATE_Y
            if (t == 1) character.pos[1] = HLE_SKIN_GATE_Y;
#endif
            if (t == 60) character_skin_reset_cloth();
#ifdef HLE_SKIN_CYCLE   // every caped skin in turn, 300 frames each (HLE_LOG_HEM: where its hem hangs)
            if (t >= 60 && (t - 60) % 300 == 0) character_skin_set(1 + (t - 60) / 300 % 7);
#endif
            if (t % 60 == 0) debugf("[skin] f%d knight y %.1f state %d\n", frame, (double)character.pos[1], (int)character_get_state());
            float *p = character.pos;
            customCamPos = (T3DVec3){{ p[0] + HLE_SKIN_DIST, p[1] + HLE_SKIN_CAM_Y, p[2] }};
            customCamTarget = (T3DVec3){{ p[0], p[1] + HLE_SKIN_LOOK_Y, p[2] }};
            camera_mode_smooth(CAMERA_CUSTOM, 0.01f);
            return;
        }
#endif
#ifdef HLE_SKIN_ACTION
        // run, a combo, two rolls; the camera steps round every 20 frames
        const int period = 20; const float dist = 50.0f;
        if (t < 120) { in->stick_x = (int)(75 * cosf(t * 0.05f)); in->stick_y = (int)(75 * sinf(t * 0.05f)); }
        if (t >= 120 && t < 260 && t % 30 == 0) { pressed->b = 1; in->btn.b = 1; }
        if (t == 280 || t == 330) { in->stick_y = 80; pressed->a = 1; in->btn.a = 1; }
#else
        const int period = 60; const float dist = HLE_SKIN_DIST;
        if (t < 180) { in->stick_x = (int)(75 * cosf(t * 0.05f)); in->stick_y = (int)(75 * sinf(t * 0.05f)); }
#endif
        float yaw = character.rot[1];
        float a = -yaw + (float)(t / period % 3) * 1.5708f;   // facing direction rotated by 0/90/180
        float *p = character.pos;
        customCamPos = (T3DVec3){{ p[0] + dist * sinf(a), p[1] + HLE_SKIN_CAM_Y, p[2] + dist * cosf(a) }};
        customCamTarget = (T3DVec3){{ p[0], p[1] + HLE_SKIN_LOOK_Y, p[2] }};
        camera_mode_smooth(CAMERA_CUSTOM, 0.01f);   // also ends the fight-intro transition
        if (t % period == 0) debugf("[skin] view %d at frame %d\n", t / period % 3, frame);
        return;
    }
#endif
#ifdef HLE_TEST_TOWER
    // Self-check of the Tower rules (no fight): circle order, boss health, potions, skills
    if (frame == 120) {
        extern void tower_begin_run(uint32_t), tower_end_run(void), tower_next_floor(void);
        extern int tower_floor_boss(void), tower_mod_max_potions(void);
        extern float tower_boss_health_scale(int);
        extern bool tower_mod_skill_allowed(int);
        int fails = 0;
        (void)tower_floor_boss;     // (the circle's order, its scaling: tower_test_map walks the maps)
        { extern int tower_test_map(void); fails += tower_test_map(); }     // the map: 1,000 maps against the rules, paths, Rests, Release, Elites
        tower_begin_run(7);
        if (tower_mod_max_potions() != 3) fails++;
        if (fabsf(tower_boss_health_scale(1) - 0.7f) > 0.01f || fabsf(tower_boss_health_scale(5) - 0.385f) > 0.01f) fails++;   // circle 1, no skills (a Bonus boss: 0.7 x 0.55)
        { extern float tower_mod_damage_taken(void); if (fabsf(tower_mod_damage_taken() - 0.6f) > 0.01f) fails++; }
        if (tower_mod_skill_allowed(0)) fails++;
        {   // Unbroken: one killing blow a floor survived at 1 HP; the next one kills; never a revive
            extern void tower_test_grant_unbroken(void);
            #define HIT(hp0) do { character_set_state(CHAR_STATE_NORMAL); character.health = (hp0); character_apply_damage(1000.0f); } while (0)
            #define ALIVE1 (character.health == 1.0f && character_get_state() != CHAR_STATE_DEAD)
            #define DEAD0  (character.health == 0.0f && character_get_state() == CHAR_STATE_DEAD)
            int uf = 0;
            HIT(10.0f); if (!DEAD0) uf |= 1;                                    // without it: dead
            tower_test_grant_unbroken();
            HIT(10.0f); if (!ALIVE1) uf |= 2;                                   // with it: 1 HP
            HIT(1.0f);  if (!DEAD0) uf |= 4;                                    // spent this floor: dead
            character_apply_damage(1000.0f); if (!DEAD0) uf |= 8;               // hit while dead: no revive
            tower_next_floor();
            HIT(10.0f); if (!ALIVE1) uf |= 16;                                  // the next floor: again
            tower_end_run();
            HIT(10.0f); if (!DEAD0) uf |= 32;                                   // outside a run: nothing
            #undef HIT
            #undef ALIVE1
            #undef DEAD0
            character_set_state(CHAR_STATE_TITLE_IDLE); character.health = character.maxHealth;
            debugf("[tower] unbroken: %s (bits %d)\n", uf ? "FAIL" : "ok", uf);
            if (uf) fails++;
        }
        {   // Vigor and Vitality taken mid-run raise the bar's top, not just health
            extern void tower_test_pick_vigor(void); extern float tower_mod_max_health(void);
            tower_begin_run(3); character.maxHealth = tower_mod_max_health(); character.health = 100.0f;
            tower_test_pick_vigor();
            float mh = character.maxHealth, h = character.health;
            debugf("[tower] vigor mid-run: max %.0f (175), health %.0f (125)\n", (double)mh, (double)h);
            if (mh != 175.0f || h != 125.0f) fails++;
            tower_end_run(); character.maxHealth = 150.0f; character.health = 150.0f;
        }
        if (tower_mod_max_potions() != 3 || !tower_mod_skill_allowed(0) || tower_boss_health_scale(3) != 1.0f) fails++;
        { extern int tower_test_slots(void); fails += tower_test_slots(); }     // slots, levels, a full bar, packs, weighting, save bits
        { extern int tower_test_stage2(void); fails += tower_test_stage2(); }   // rivals, "+", duos
        { extern int tower_test_skills(void); fails += tower_test_skills(); }   // the skills' levels and forms, the B forms' unlocks, the stagger cap
        { extern int tower_test_shop(void), tower_test_events(void); fails += tower_test_shop(); fails += tower_test_events(); }   // gold, the Shop, the Events
        { extern int tower_test_affixes(void); fails += tower_test_affixes(); }     // stage 6: the Elites' affixes, Berserker
        { extern int tower_test_offerings(void); fails += tower_test_offerings(); }     // the Offerings, the milestones, the seed, the Codex's fit
        debugf("[tower] self-check: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    }
    return;
#endif
#ifdef HLE_TEST_SKIN_SLOT
    // Run twice: the first run (booting on Save 1 with the Knight) loads Save 2 (back on the
    // title) and picks Wolf Knight; the second reports the slot and skin
    {
        static int phase = 0, t0 = 0, step = 0;
        int t = frame - t0;
        static const char *seqs[3] = { "DDDADA", "", "DADDDDADDDDA" };     // Load Game > Save 2 | (from Load Game) Settings > Skin: the list, Down x4 (Wolf Knight), A
        if (phase == 0) {
            if (menu_controller_is_active()) {
                debugf("[slot] booted on slot %d with skin %d (%s)\n", save_controller_get_active_slot(), character_skin_get(), character_skin_name(character_skin_get()));
                phase = (save_controller_get_active_slot() == 0 && character_skin_get() == 0) ? 1 : 9; t0 = frame;
            } else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1 || phase == 3) {
            const char *q = seqs[phase - 1];
            if (q[step] && t >= 20 + step * 15 && t < 21 + step * 15) {
                char c = q[step];
                if (c == 'D') pressed->d_down = 1; else if (c == 'R') pressed->d_right = 1; else { pressed->a = 1; in->btn.a = 1; }
                step++;
            }
            if (!q[step] && t > 20 + step * 15 + 60) {
                if (phase == 3) { debugf("[slot] set skin %d on slot %d\n", character_skin_get(), save_controller_get_active_slot()); phase = 9; }
                else { phase = 3; t0 = frame; step = 0; }
            }
        }
        return;
    }
#endif
#ifdef HLE_TEST_SAVE_FRESH
    // Run twice on a fresh EEPROM. Boot 1: Settings > Skin x3, Audio > Master volume down 3,
    // back out to the title, open Load Game (Save 1's card must show it used). Boot 2 should
    // come back with that skin and volume; it opens Load Game too
    {
        static int start = -1, step = 0;
        static const char *q = "";
        if (start < 0) {
            if (menu_controller_is_active()) {
                start = frame;
                const SaveData *d = save_controller_get_slot_data(0);
                debugf("[fresh] booted on slot %d, skin %d (%s), master %d, slot 1 stamp %lu\n", save_controller_get_active_slot(), character_skin_get(),
                       character_skin_name(character_skin_get()), audio_get_master_volume(), (unsigned long)(d ? d->last_played_timestamp : 0));
                q = (character_skin_get() == SKIN_KNIGHT) ? "DDDDADDDDRRRUUUUALLLBBUA" : "DDDDADDDD";    // boot 2: Settings, on the Skin row
            } else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
        if (q[step] && t == 20 + step * 15) {
            char c = q[step++];
            if (c == 'D') pressed->d_down = 1; else if (c == 'U') pressed->d_up = 1; else if (c == 'R') pressed->d_right = 1; else if (c == 'L') pressed->d_left = 1;
            else if (c == 'B') { pressed->b = 1; in->btn.b = 1; } else { pressed->a = 1; in->btn.a = 1; }
        }
        if (!q[step] && t == 20 + step * 15 + 120) {
            const SaveData *d = save_controller_get_slot_data(0);
            debugf("[fresh] done: skin %d (%s), master %d, slot 1 stamp %lu\n", character_skin_get(), character_skin_name(character_skin_get()),
                   audio_get_master_volume(), (unsigned long)(d ? d->last_played_timestamp : 0));
        }
        return;
    }
#endif
#ifdef HLE_TEST_SAVE_FLOW
    // Run three times on one EEPROM, fresh first. Boot 1: Black Swordsman on Save 1, then Load Game >
    // Save 2 (it must stay on the title) and the Umber cloak there. Boot 2 should open on Save 2
    // in Umber, then loads Save 1. Boot 3 should open on Save 1 in Black Swordsman.
    {
        static int start = -1, step = 0;
        static const char *q = "";
        if (start < 0) {
            if (menu_controller_is_active()) {
                start = frame;
                int sl = save_controller_get_active_slot(), k = character_skin_get();
                debugf("[flow] booted on slot %d with skin %d (%s)\n", sl, k, character_skin_name(k));
                if (sl == 0 && k == SKIN_KNIGHT) q = "DDDDADDDDRRRBUADADADDDDR";   // Settings > Skin x3, back, Load Game > Save 2, Settings > Skin x1
                else if (sl == 1) q = "DDDAA";                                  // Load Game > Save 1
            } else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
        if (q[step] && t == 20 + step * 15) {
            char c = q[step++];
            if (c == 'D') pressed->d_down = 1; else if (c == 'U') pressed->d_up = 1; else if (c == 'R') pressed->d_right = 1;
            else if (c == 'B') { pressed->b = 1; in->btn.b = 1; } else { pressed->a = 1; in->btn.a = 1; }
        }
        if (!q[step] && t == 20 + step * 15 + 120)
            debugf("[flow] done: slot %d, skin %d (%s), scene %d (1: title), menu %d\n", save_controller_get_active_slot(), character_skin_get(),
                   character_skin_name(character_skin_get()), scene_controller_get_active_scene(), menu_controller_is_active());
        return;
    }
#endif
#ifdef HLE_TEST_SKIN_SAVE
    // Run twice with the same ROM: the first run (booting with the Knight) picks Wolf Knight
    // in Title > Settings > Skin; the second reports what it booted with
    {
        static int start = -1, step = 0;
        if (start < 0) {
            if (menu_controller_is_active()) {
                start = frame;
                debugf("[save] booted with skin %d (%s), slot %d\n", character_skin_get(), character_skin_name(character_skin_get()), save_controller_get_active_slot());
            } else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        if (character_skin_get() != 0 && step == 0) return;    // second run: nothing to do
#ifdef HLE_SKIN_SAVE_UMBER
#define HLE_SKIN_SAVE_SEQ "DDDDADDDDADDDA"                       // the Umber Cloak (a hood)
#else
#define HLE_SKIN_SAVE_SEQ "DDDDADDDDADDDDA"                      // Settings > Skin: the list, Down x4 (Wolf Knight), A
#endif
        static const char seq[] = HLE_SKIN_SAVE_SEQ;
        int t = frame - start;
        if (seq[step] && t >= 20 + step * 15 && t < 21 + step * 15) {
            char c = seq[step];
            if (c == 'D') pressed->d_down = 1; else if (c == 'R') pressed->d_right = 1; else { pressed->a = 1; in->btn.a = 1; }
            step++;
        }
        if (!seq[step] && t == 400) debugf("[save] set skin %d (%s)\n", character_skin_get(), character_skin_name(character_skin_get()));
        return;
    }
#endif
#ifdef HLE_TEST_LOADMENU
    {   // test aid: open Title > Load Game and stay there (a look at the slot cards)
        static int start = -1, step = 0;
        if (start < 0) {
            if (menu_controller_is_active()) { start = frame; (void)save_controller_increment_run_count(); }   // a used slot
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
#ifndef HLE_MENU_ROW
#define HLE_MENU_ROW 3      // Load Game; 5: Credits
#endif
        if (step < HLE_MENU_ROW + 1 && t == 30 + step * 15) { if (step < HLE_MENU_ROW) pressed->d_down = 1; else { pressed->a = 1; in->btn.a = 1; } step++; }
        return;
    }
#endif
#ifdef HLE_TEST_BKEY
    {   // test aid: B on each title menu row must do nothing there (it only goes back)
        extern int scene_controller_get_active_scene(void);
        static int start = -1;
        if (start < 0) {
            if (menu_controller_is_active()) start = frame;
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start, row = t / 60;
        if (row < 6 && t % 60 == 20) { pressed->b = 1; in->btn.b = 1; debugf("[bkey] B on row %d\n", row); }
        if (row < 6 && t % 60 == 50) { debugf("[bkey] row %d: scene %d menu %d submenu %d\n", row, scene_controller_get_active_scene(), menu_controller_is_active(), menu_controller_is_title_submenu_active()); }
        if (row < 5 && t % 60 == 55) pressed->d_down = 1;
        return;
    }
#endif
#ifdef HLE_TEST_MARKS
    {   // test aid: a save with the story and two Bonus bosses beaten, then Title > Bonus
        static int start = -1, step = 0;
        if (start < 0) {
            if (menu_controller_is_active()) {
                start = frame; (void)save_controller_record_boss_clear_time_ms(754321);
                save_controller_mark_bonus_beaten(0); save_controller_mark_bonus_beaten(2);
            } else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
        if (step < 2 && t == 200 + step * 15) { if (step < 1) pressed->d_down = 1; else { pressed->a = 1; in->btn.a = 1; } step++; }
        return;
    }
#endif
#ifdef HLE_BOOT_SKIN
    if (frame == 2) character_skin_set(HLE_BOOT_SKIN);   // test aid: A/B the title with one skin
#ifdef HLE_SKIN_CYCLE
    if (frame % 300 == 2) { character_skin_set(1 + frame / 300 % 7); debugf("[skin] title f%d skin %d\n", frame, 1 + frame / 300 % 7); }
#endif
    return;
#endif
#ifdef HLE_TEST_SKIN_TITLE
    // Title > Settings (4 down) > Skin (4 down), then Right every 60 frames through the skins
    {
        static int start = -1, step = 0;
        if (start < 0) {
            if (menu_controller_is_active()) start = frame;
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        static const char seq[] = "DDDDADDDDA";      // Settings > Skin: the list
        int t = frame - start;
        if (step < 10 && t >= 20 + step * 15 && t < 20 + step * 15 + 1) { if (seq[step] == 'D') pressed->d_down = 1; else { pressed->a = 1; in->btn.a = 1; } step++; }
        {   // the longest frame after each switch (a long one jerks the fog gate's scroll)
            static uint32_t last; static int worst;
            uint32_t now = get_ticks_ms();
            int d = last ? (int)(now - last) : 0; last = now;
            if (step >= 10 && t > 200) { if (d > worst) worst = d; if ((t - 200) % 60 == 59) { debugf("[skin] worst frame %d ms\n", worst); worst = 0; } }
        }
        if (step >= 10 && t > 200 && (t - 200) % 60 == 0 && (t - 200) / 60 <= 8) {
            pressed->d_down = 1;
            debugf("[skin] down at %d\n", frame);
        }
        if (step >= 10 && t > 200 && (t - 200) % 60 == 8) { pressed->a = 1; in->btn.a = 1; }     // wear it (a locked one stays as it was)
        if (step >= 10 && t > 200 && (t - 200) % 60 == 30)
            debugf("[skin] now %d (%s) at %d | cam %.1f,%.1f,%.1f -> %.1f,%.1f,%.1f scale %.3f\n", character_skin_get(), character_skin_name(character_skin_get()), frame,
                   (double)camPos.v[0], (double)camPos.v[1], (double)camPos.v[2], (double)camTarget.v[0], (double)camTarget.v[1], (double)camTarget.v[2], (double)character.scale[0]);
        return;
    }
#endif
#ifdef HLE_TEST_MENUS
#include "scenes/guardian/guardian_scene.h"
    // Every menu by its presses, from the title and from the pause menus of a Story, a Bonus and a Tower fight
    // (HLE_NAV_MODE: 0 D-pad and B, 1 stick and B, 2 D-pad and "A only", 3 stick and "A only": every level left by its
    // Back row and A, a pager's Back corner by Left and A; HLE_PAD_LAYOUT the layout). Each visit walks down its path
    // (the rows, A at each), logs "[menus] at <name> (frame f)" (Mupen's shot numbers: shoot a second run at f+20),
    // checks a paged screen's Left/Right (a page each way, or nothing when it has one: never a cursor sideways) and that no
    // "(R)" is drawn in it, opens a purchase's confirm with A where marked ('c': on Back, nothing spent; B, or its Back row by
    // A, cancels with the cursor on the row) or a Codex topic's Details ('d': Left/Right its pages, closed by B or its Back row),
    // then backs out a level at a time: each level must land on its parent's row it came from. [menus] PASS/FAIL, [menus] done
    // with the counts. The save starts with 5000 Embers and nothing bought (Relics: its first relic's row)
    {
#ifndef HLE_NAV_MODE
#define HLE_NAV_MODE 0
#endif
        extern int menu_controller_test_nav(int *row, int *back, int *corner);
        extern bool scene_is_cutscene_active(void); extern bool tower_active(void), tower_between_open(void);
        typedef struct { const char *name, *path; int menu; char r; } Visit;     // r: 'c' a purchase's confirm, 'd' a topic's Details
        static const Visit TITLE[] = {
            { "Title", "", MENU_TITLE, false }, { "Bonus", "1", MENU_BONUS, false }, { "Tower", "2", MENU_TOWER, false },
            { "Shrine", "21", MENU_SHRINE, 'c' }, { "Relics", "215", MENU_RELICS, 'c' }, { "Unlocks", "217", MENU_UNLOCKS, 'c' },
            { "Offerings", "22", MENU_OFFERINGS, 'c' }, { "Codex", "23", MENU_CODEX, 'd' }, { "Nightfarer", "21", MENU_SHRINE, 'n' }, { "Load Game", "3", MENU_LOAD_GAME, false },
            { "Settings", "4", MENU_SETTINGS, false }, { "Audio", "40", MENU_AUDIO, false }, { "Video", "41", MENU_VIDEO, false },
            { "Overscan", "411", 100, false }, { "Controls", "42", MENU_CONTROLS, false }, { "Skins", "44", MENU_SKINS, false },
            { "Credits", "5", MENU_CREDITS, false }, { NULL, NULL, 0, false } };
        static const Visit PAUSE[] = {
            { "Pause", "", MENU_MAIN, false }, { "Pause Settings", "3", MENU_SETTINGS, false }, { "Pause Audio", "30", MENU_AUDIO, false },
            { "Pause Video", "31", MENU_VIDEO, false }, { "Pause Overscan", "311", 100, false }, { "Pause Settings Controls", "32", MENU_CONTROLS, false },
            { "Pause Controls", "4", MENU_CONTROLS, false }, { NULL, NULL, 0, false } };     // (no Skins: the pause Settings hide them)
        static const Visit TPAUSE[] = {
            { "Tower Pause", "", MENU_MAIN, false }, { "Stats", "1", MENU_RUN, false }, { "Tower Pause Codex", "5", MENU_CODEX, 'd' },
            { "Tower Pause Settings", "3", MENU_SETTINGS, false }, { "Tower Pause Controls", "4", MENU_CONTROLS, false }, { NULL, NULL, 0, false } };
        static const char *const GAME[] = { "Story", "Bonus", "Tower" };
        static int phase, t0, vi, lvl, sub, game, pass, fail;
        static int at[8];       // the row taken at each level of the path
        static bool seen; static int rd;     // (rd: "(R)" strings drawn when the screen came up)
        #define PR(f) do { pressed->f = 1; in->btn.f = 1; } while (0)
        #define CHECK(c, ...) do { if (c) pass++; else fail++; debugf("[menus] %s: ", (c) ? "PASS" : "FAIL"); debugf(__VA_ARGS__); debugf(" (frame %d)\n", frame); } while (0)
        // the box fits its content (ui_fit): all it laid out at least 10 inside its box (the padding, 14, clear of the rounded
        // corners), and (paged screens aside, whose pages fill the safe rect) at most 20 over and under it: as tall as its
        // content (its width one of the fixed three)
        #define BOXCHECK(nm, paged) do { UiRect b_ = ui_last_box(), c_ = ui_tracked(false), f_ = ui_tracked(true); \
            if (f_.x1 > f_.x0 || f_.y1 > f_.y0) { if (f_.x0 < c_.x0) c_.x0 = f_.x0; if (f_.y0 < c_.y0) c_.y0 = f_.y0; if (f_.x1 > c_.x1) c_.x1 = f_.x1; if (f_.y1 > c_.y1) c_.y1 = f_.y1; } \
            int gl_ = c_.x0 - b_.x0, gt_ = c_.y0 - b_.y0, gr_ = b_.x1 - c_.x1, gb_ = b_.y1 - c_.y1; \
            bool in_ = gl_ >= 10 && gt_ >= 10 && gr_ >= 10 && gb_ >= 10, snug_ = (paged) || (gt_ <= 20 && gb_ <= 20); \
            CHECK(in_ && snug_, "%s: its box %d,%d-%d,%d, its content %d,%d-%d,%d: gaps left %d top %d right %d bottom %d%s", nm, \
                  b_.x0, b_.y0, b_.x1, b_.y1, c_.x0, c_.y0, c_.x1, c_.y1, gl_, gt_, gr_, gb_, (paged) ? " (paged)" : ""); } while (0)
        bool stick = HLE_NAV_MODE & 1, aOnly = HLE_NAV_MODE >= 2;
        int t = frame - t0, row, back, corner, m = menu_controller_test_nav(&row, &back, &corner);
        Boss *boss = boss_get_instance();
        const Visit *vs = phase == 1 ? TITLE : game == 2 ? TPAUSE : PAUSE;
        if (phase == 0) {       // the title menu
            if (menu_controller_is_active() && !boss) { if (t > 40) { phase = 1; t0 = frame; vi = 0; lvl = 0; sub = 0;
                TowerSave ts; save_controller_get_tower(&ts);       // (every purchase there to ask for)
                ts.embers = 5000; ts.unlocks = 0; ts.relicsOwned = 0; ts.relicEquipped = 0; ts.offers = 0; ts.flags &= ~TOWER_FLAG_NIGHTFARER;
                for (int u = 0; u < 5; u++) ts.levels[u] = 0;
                save_controller_set_tower(&ts); } }
            else { t0 = frame; if (!boss && frame > 200 && frame % 30 == 0) PR(a); }
            return;
        }
        if (phase == 2) {       // start the next game: Story (row 0), Bonus > Fallen Warrior, Tower > Climb; skip to the fight; pause
            static const char *const START[] = { "A", "AA", "AA" };
            const char *sq = START[game];
            if (t >= 20 && sub == 0 && m == MENU_TITLE && row != game) { if (row > game) pressed->d_up = 1; else pressed->d_down = 1; t0 = frame; return; }   // (the title's row: Story, Bonus, Tower)
            if (t >= 20 && sq[sub]) { PR(a); sub++; t0 = frame; return; }
            if (!sq[sub]) {
                if (boss && boss->mode == BOSS_MODE_COMBAT && !scene_is_cutscene_active() && scene_get_game_state() == GAME_STATE_PLAYING && !(game == 2 && tower_between_open())) {
                    if (t > 30 && frame % 1500 == 0) { PR(start); phase = 3; t0 = frame; vi = 0; lvl = 0; sub = 0; debugf("[menus] %s fight, paused (frame %d)\n", GAME[game], frame); }
                } else { t0 = frame; if (frame % 20 == 0) PR(a); }
                if (character.health < character.maxHealth) character.health = character.maxHealth;
            }
            return;
        }
        if (phase == 4) {       // Main Menu: back to the title for the next game (or done)
            if (t == 30) { if (row != MENU_MAIN_QUIT) { pressed->d_down = 1; t0 = frame - 20; } else PR(a); }
            if (t > 40 && menu_controller_is_active() && !boss) {
                if (++game < 3) { phase = 2; t0 = frame; sub = 0; }
                else { debugf("[menus] done: mode %d layout %d, %d PASS, %d FAIL\n", HLE_NAV_MODE,
#ifdef HLE_PAD_LAYOUT
                               HLE_PAD_LAYOUT,
#else
                               0,
#endif
                               pass, fail); phase = 5; }
            }
            return;
        }
        if (phase != 1 && phase != 3) return;
        if (t < 20) return;     // a press every 20 frames
        t0 = frame;
        const Visit *v = &vs[vi];
        if (!v->name) {         // the root's visits done
            if (phase == 1) { phase = 2; game = 0; sub = 0; } else { phase = 4; }
            return;
        }
        int depth = (int)strlen(v->path);
        // a move: the D-pad or the stick, one push a press
        #define MOVE(d_) do { char dir = (d_); if (stick) { if (dir == 'U') in->stick_y = 70; else if (dir == 'D') in->stick_y = -70; else if (dir == 'L') in->stick_x = -70; else in->stick_x = 70; } \
                               else if (dir == 'U') pressed->d_up = 1; else if (dir == 'D') pressed->d_down = 1; else if (dir == 'L') pressed->d_left = 1; else pressed->d_right = 1; } while (0)
        if (sub == 0) {         // down the path
            if (lvl < depth) {
                int want = v->path[lvl] - '0';
                if (row != want) { MOVE(row < want ? 'D' : 'U'); return; }
                at[lvl++] = want; PR(a); return;
            }
            CHECK(m == v->menu, "%s: menu %d (want %d), row %d, Back row %d", v->name, m, v->menu, row, back);
            debugf("[menus] at %s%s%s (frame %d)\n", phase == 3 && game < 2 ? GAME[game] : "", phase == 3 && game < 2 ? " " : "", v->name, frame);
            if (m != MENU_TITLE && m != 100) BOXCHECK(v->name, m == MENU_RUN || m == MENU_CODEX);     // (the overscan screen: its corner marks and hint over the scene)
            bool paged = m == MENU_RUN || m == MENU_CODEX || m == MENU_SHRINE || m == MENU_RELICS || m == MENU_UNLOCKS || m == MENU_OFFERINGS || m == MENU_SKINS;
            sub = paged ? 10 : v->r ? 1 : 3; rd = ui_test_r_drawn; return;
        }
        extern int menu_controller_test_pages(int *per), tower_embers(void);
        static int row0, pg0, e0, lr, pgR;
        // a paged screen: no "(R)" drawn in it (the frames since it came up), then Right and Left: a page each way (a list's
        // cursor onto that page's first row; Stats'/the Codex's page), or with one page nothing (the cursor where it was)
        int per_, pages_ = menu_controller_test_pages(&per_), pgNow = m == MENU_RUN || m == MENU_CODEX ? corner : per_ ? row / per_ : 0;
        if (sub == 10) { CHECK(ui_test_r_drawn == rd, "%s: no \"(R)\" drawn in it (%d)", v->name, ui_test_r_drawn - rd); row0 = row; pg0 = pgNow; lr = pages_; MOVE('R'); sub = 11; return; }
        if (sub == 11) {
            pgR = pg0 + 1 < lr ? pg0 + 1 : pg0;
            CHECK(m == v->menu && pgNow == pgR && (lr > 1 || row == row0), "%s: Right: page %d -> %d of %d, row %d -> %d", v->name, pg0 + 1, pgNow + 1, lr, row0, row);
            MOVE('L'); sub = 12; return;
        }
        if (sub == 12) {
            int want = pgR > 0 ? pgR - 1 : 0;
            CHECK(m == v->menu && pgNow == want && (lr > 1 || row == row0), "%s: Left: page %d (want %d), row %d", v->name, pgNow + 1, want + 1, row);
            CHECK(ui_test_r_drawn == rd, "%s: still no \"(R)\" after the turns", v->name);
            sub = v->r ? 1 : 3; return;
        }
        if (sub == 1) {         // 'c': a purchase's confirm (A on the row; Relics: its first relic's row); 'd': a Codex topic's Details (A on row 0)
            if (v->r == 'c' && m == MENU_RELICS && row != 1) { MOVE(row < 1 ? 'D' : 'U'); return; }
            if (v->r == 'n' && row != 6) { MOVE(row < 6 ? 'D' : 'U'); return; }     // (the Shrine's Nightfarer row)
            if (v->r == 'd' && row != 0) { MOVE(row < 0 ? 'D' : 'U'); return; }
            row0 = row; e0 = tower_embers(); PR(a); sub = 2; seen = false; return;
        }
        if (sub == 2) {
            UiPanel *dp = ui_details();
            if (!seen) {
                seen = true; rd = ui_test_r_drawn;
                if (v->r != 'd') CHECK(dp->open && dp->confirm && dp->owner == UI_OWNER_MENU && dp->button == 1 && tower_embers() == e0, "%s: A opens its confirm (\"%s\"), on Back, nothing spent (Embers %d)", v->name, dp->title, tower_embers());
                else CHECK(dp->open && !dp->confirm && dp->owner == UI_OWNER_MENU, "%s: A opens the topic's Details (%d pages)", v->name, dp->pg.pages);
                debugf("[menus] at %s %s (frame %d)\n", v->name, v->r != 'd' ? "confirm" : "Details", frame);
                if (!dp->open) { sub = 3; return; }
                BOXCHECK(v->r != 'd' ? "Confirm" : "Details", v->r == 'd');
                lr = dp->pg.pages; pg0 = dp->pg.page;
                if (v->r == 'd') { MOVE('R'); sub = 6; } return;
            }
            if (!dp->open) {
                CHECK(m == v->menu && row == row0 && tower_embers() == e0, "%s: %s closed by %s: nothing spent (Embers %d), the cursor on its row (%d)", v->name, v->r != 'd' ? "the confirm" : "Details", aOnly ? "its Back row" : "B", tower_embers(), row);
                sub = 3; return;
            }
            CHECK(ui_test_r_drawn == rd, "%s: no \"(R)\" in its %s", v->name, v->r != 'd' ? "confirm" : "Details");
            if (!aOnly) PR(b); else PR(a);      // (the cursor on Back: A takes Back)
            return;
        }
        if (sub == 6) { UiPanel *dp = ui_details(); CHECK(dp->open && dp->pg.page == (lr > 1 ? pg0 + 1 : pg0), "%s: Details' Right turns a page (%d of %d)", v->name, dp->pg.page + 1, lr); MOVE('L'); sub = 7; return; }
        if (sub == 7) { UiPanel *dp = ui_details(); CHECK(dp->open && dp->pg.page == pg0, "%s: Details' Left turns back (%d)", v->name, dp->pg.page + 1); sub = 2; return; }
        if (sub == 3) { sub = 4; return; }      // (a moment for the shot)
        // back up the path, a level at a time
        if (lvl == 0) { vi++; sub = 0; return; }
        if (sub == 4) {
            if (!aOnly || m == 100) { if (aOnly) PR(a); else PR(b); sub = 5; return; }     // (the overscan screen: A saves, B cancels)
            if (back >= 0) { if (row != back) { MOVE(row < back ? 'D' : 'U'); return; } PR(a); sub = 5; return; }
            if (row != -1) { MOVE('U'); return; }               // a pager: Up to its Back row (-1), A
            PR(a); sub = 5; return;
        }
        if (sub == 5) {
            lvl--;
            const char *pm = v->path;
            int pmenu = lvl == 0 ? vs[0].menu : -1;
            for (int k = 0; vs[k].name && pmenu < 0; k++) if ((int)strlen(vs[k].path) == lvl && !strncmp(vs[k].path, pm, lvl)) pmenu = vs[k].menu;
            CHECK(m == pmenu && row == at[lvl], "%s: %s from level %d: menu %d row %d (want %d row %d)", v->name, aOnly ? "its Back row" : "B", lvl + 1, m, row, pmenu, at[lvl]);
            sub = 4; return;
        }
        #undef MOVE
        #undef PR
        #undef CHECK
        return;
    }
#endif
#ifdef HLE_TEST_CONTROLS
    // Title > Settings > Controls: Right (A/B Swapped), Right (Skills on C), Down, Right (camera 4);
    // the remap of each layout; then B back
    {
        extern void joypad_debug_remap(joypad_buttons_t *b);
        static int phase = 0, t0 = 0, step = 0;
        int t = frame - t0;
        if (phase == 0) {
            if (menu_controller_is_active()) { phase = 1; t0 = frame; }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {
            static const char seq[] = "DDDDADDA.R.R.DR";
            if (t >= 30 && seq[step]) {
                char c = seq[step];
                if (c == 'D') pressed->d_down = 1; else if (c == 'R') pressed->d_right = 1; else if (c == 'A') { pressed->a = 1; in->btn.a = 1; }
                if (c == '.') debugf("[ctl] layout %d (%s), camera %d\n", joypad_get_layout(), joypad_layout_name(joypad_get_layout()), camera_get_speed());
                step++; t0 = frame;
            }
            if (!seq[step] && t == 30) {
                debugf("[ctl] final: layout %d camera %d sens %.1f\n", joypad_get_layout(), camera_get_speed(), (double)cameraSensitivity);
                for (int l = 0; l < LAYOUT_COUNT; l++) {
                    joypad_set_layout(l);
                    joypad_buttons_t b = { 0 }; b.a = 1; b.c_up = 1;
                    joypad_debug_remap(&b);
                    debugf("[ctl] layout %d: physical A+C-up reads a%d b%d c_up%d d_up%d\n", l, b.a, b.b, b.c_up, b.d_up);
                }
                joypad_set_layout(LAYOUT_SKILLS_ON_C);
                phase = 2;
            }
        }
        return;
    }
#endif
#ifdef HLE_TEST_SHRINE
    // The Shrine's relic and Nightfarer rows: 200 Embers and two relics; Tower > Shrine, down to
    // Relic, right twice (the second relic), down, A (buy Nightfarer); then the checks
    {
        extern int tower_relic_equipped(void), tower_embers(void); extern bool tower_nightfarer_owned(void);
        static int phase = 0, t0 = 0, step = 0;
        int t = frame - t0;
        if (phase == 0) {
            if (menu_controller_is_active()) {
                TowerSave ts; save_controller_get_tower(&ts); debugf("[shrine] Embers at the menu %d\n", ts.embers); ts.embers = 500; ts.relics[0] = 0x12; ts.relics[1] = 0x8C; save_controller_set_tower(&ts);   // the second relic a pact (Brand of Wrath)
                phase = 1; t0 = frame;
            } else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {
#ifdef HLE_SHRINE_NONE
            static const char seq[] = "DDADADDDDDADDAUABDAUAUAUUAB";    // ... and back up to Relics, Up x2 (to None), A: no relic
#else
            static const char seq[] = "DDADADDDDDADDAUABDAUA";    // ... Relics: A, Down x2 (Saint's Chalice), A buys it, B; Nightfarer
#endif
            if (t >= 25 && seq[step]) {
                char c = seq[step];
#ifdef HLE_SHRINE_STICK     // the same, steered with the control stick
                if (c == 'D') in->stick_y = -80; else if (c == 'R') in->stick_x = 80;
                else if (c == 'U') in->stick_y = 80; else if (c == 'L') in->stick_x = -80; else if (c == 'B') { pressed->b = 1; in->btn.b = 1; } else { pressed->a = 1; in->btn.a = 1; }
#else
                if (c == 'D') pressed->d_down = 1; else if (c == 'R') pressed->d_right = 1;
                else if (c == 'U') pressed->d_up = 1; else if (c == 'L') pressed->d_left = 1; else if (c == 'B') { pressed->b = 1; in->btn.b = 1; } else { pressed->a = 1; in->btn.a = 1; }
#endif
                step++; t0 = frame;
            }
            if (!seq[step] && t == 40) {
                extern bool tower_relic_owned(int id);
                debugf("[shrine] owned:%s%s%s%s%s\n", tower_relic_owned(0) ? " 0" : "", tower_relic_owned(1) ? " 1" : "", tower_relic_owned(2) ? " 2" : "",
                       tower_relic_owned(3) ? " 3" : "", tower_relic_owned(4) ? " 4" : "");
                debugf("[shrine] relic equipped %d, Nightfarer owned %d, skin unlocked %d, Embers %d\n", tower_relic_equipped(),
                       tower_nightfarer_owned(), save_controller_skin_unlocked(SKIN_NIGHTFARER), tower_embers());
                pressed->d_up = 1; phase = 2;
            }
        }
        return;
    }
#endif
#ifdef HLE_TEST_TOWER_NAV
    // The Tower's screens by their presses (HLE_NAV_MODE: 0 the D-pad, 1 the stick, 2 "A only": moves on the D-pad, every
    // exit by its own row or button and A, never B or R), in the layout HLE_PAD_LAYOUT: a run from the title, its first
    // fight (no map before it), the draft, the map (Up/Down enter nothing, B the pause menu, R Details, A the node's page,
    // its B or Back the map, its A in), a Shop, an Event and a Rest on the way (R, B a level back), the music on each
    // screen, then a death. [nav] PASS / FAIL lines; [nav] done with the counts
    {
#ifndef HLE_NAV_MODE
#define HLE_NAV_MODE 0
#endif
        extern int tower_screen(void), tower_map_row(void), tower_map_sel(void), tower_map_next(int), tower_list_sel(void), tower_shop_sel(void), tower_floor(void), tower_draft_selected(void), tower_gold(void);
        extern bool tower_active(void), tower_between_open(void), tower_draft_is_open(void), tower_event_done(void), tower_leave_asking(void);
        extern const char *audio_music_now(void); extern int menu_controller_test_state(int *);
        enum { S_MAP = 1, S_REST, S_TEMPER, S_RELEASE, S_SHOP, S_EVENT = 9, S_CONFIRM = 10, N_SHOP = 4, N_EVENT = 3, N_REST = 5, N_ELITE = 2 };
        static int phase, t0, k, pass, fail, entry = -99, mapsSeen, lastRow = -9, sawShop, sawEvent, sawRest, sawElite, fights;
        static char seq[64]; static int expectRow, shopGold, rdc; static bool rdOpen;
        Boss *boss = boss_get_instance();
        int t = frame - t0;
        bool det = ui_details()->open && !ui_details()->confirm, menu = menu_controller_is_active();
        #define PR(f) do { pressed->f = 1; in->btn.f = 1; } while (0)
        #define CHECK(c, ...) do { if (c) pass++; else fail++; debugf("[nav] %s: ", (c) ? "PASS" : "FAIL"); debugf(__VA_ARGS__); debugf(" (frame %d)\n", frame); } while (0)
        #define MUSIC(where) debugf("[nav] music on %s: %s\n", where, audio_music_now()[0] ? audio_music_now() : "silent")
        if (phase == 0) {       // the title: Tower, Climb
            if (menu_controller_is_active()) { phase = 1; t0 = frame; k = 0; }
            else if (frame > 200 && frame % 30 == 0) PR(a);
            return;
        }
        if (phase == 1) {
            static const char ts[] = "DDAA";
            if (t >= 20 && ts[k]) { if (ts[k] == 'D') pressed->d_down = 1; else PR(a); k++; t0 = frame; }
            if (!ts[k] && boss && tower_active() && boss->mode == BOSS_MODE_COMBAT && t > 30) {
                CHECK(!tower_between_open() && boss->phaseIndex == 1 && tower_floor() == 1, "the run's first floor is the Guardian, no map before it (between %d, phase %d)", tower_between_open(), boss->phaseIndex);
                MUSIC("the first fight");
                phase = 2; t0 = frame; fights = 1;
            }
            return;
        }
        if (phase == 2) {       // a fight: 60 frames, then the kill
            if (t == 30) MUSIC("a fight");
            if (t >= 60 && t % 60 == 0 && boss->health > 0.0f) boss_apply_damage(boss, boss->maxHealth * 2.0f);     // (again if a move's state shrugged it off)
            if (t > 60 && tower_draft_is_open()) { phase = 3; t0 = frame; entry = -99; }
            return;
        }
        if (phase == 5) {       // the end: a death, its screen
            if ((t == 10 || t == 40) && character_get_state() != CHAR_STATE_DEAD) { character.health = 1.0f; character_set_state(CHAR_STATE_NORMAL); character_apply_damage(1000.0f); }   // (twice: Unbroken)
            if (t == 200) { CHECK(character_get_state() == CHAR_STATE_DEAD, "the death screen"); MUSIC("the death screen");
                            debugf("[nav] done: mode %d layout %d, %d PASS, %d FAIL, shop %d event %d rest %d elite %d, %d fights\n", HLE_NAV_MODE,
#ifdef HLE_PAD_LAYOUT
                                   HLE_PAD_LAYOUT,
#else
                                   0,
#endif
                                   pass, fail, sawShop, sawEvent, sawRest, sawElite, fights); phase = 6; }
            return;
        }
        if (phase != 3) return;
        // between floors: a script for each screen as it comes up, a press every 24 frames (past the screens' input delay)
        int now = tower_draft_is_open() ? -1 : tower_screen();
        if (!tower_between_open() && !menu) {      // a fight began
            fights++;
            CHECK(boss && boss->mode == BOSS_MODE_COMBAT, "a fight's node entered: its fight (floor %d)", tower_floor());
            if (tower_floor() >= 9 || (sawShop && sawEvent && sawRest)) { phase = 5; t0 = frame; return; }
            phase = 2; t0 = frame; return;
        }
        if (now != entry && !menu && !det && now != S_CONFIRM && now != S_TEMPER && now != S_RELEASE && !(entry == S_REST && now == S_MAP && seq[0] == 'B' && k == 1)) {
            if (entry != -99 && !seq[k]) {      // the script's last press left that screen: where it went
                if (entry == -1) CHECK(now == S_MAP, "the draft's A takes the card: the map (%d)", now);
                else if (entry == S_MAP) CHECK(tower_map_row() == expectRow + 1, "A, A: the node entered (row %d -> %d, screen %d)", expectRow + 2, tower_map_row() + 2, now);
                else CHECK(now == S_MAP, "screen %d left by its last press (%c): the map (%d)", entry, k ? seq[k - 1] : '-', now);
            }
            entry = now; k = 0; t0 = frame;
            bool aOnly = HLE_NAV_MODE == 2;
            if (now == -1) { MUSIC("the draft"); snprintf(seq, sizeof seq, aOnly ? ">v^A" : "rBB>v^A"); }
            else if (now == S_MAP) {
                MUSIC("the map");
                // the node to go to: a Shop, an Event, a Rest, an Elite not seen yet, else straight on
                int target = -1;
                static const int PREF[4] = { N_SHOP, N_EVENT, N_REST, N_ELITE };
                for (int p = 0; p < 4 && target < 0; p++) {
                    if ((PREF[p] == N_SHOP && sawShop) || (PREF[p] == N_EVENT && sawEvent) || (PREF[p] == N_REST && (sawRest || tower_map_row() + 1 >= 5)) || (PREF[p] == N_ELITE && sawElite)) continue;
                    for (int l = 0; l < 3; l++) if (tower_map_next(l) == PREF[p]) { target = l; break; }
                }
                if (target < 0) target = tower_map_sel();
                int n = 0, sel = tower_map_sel();
                n += snprintf(seq + n, sizeof seq - n, aOnly ? "AvA" : "^vBBrBABAvA");     // (A only: the page's Back row by Down, A)
                for (; sel < target; sel++) seq[n++] = '>';
                for (; sel > target; sel--) seq[n++] = '<';
                seq[n++] = 'A'; seq[n++] = 'A'; seq[n] = 0;
                int nt = tower_map_next(target);
                sawShop |= nt == N_SHOP; sawEvent |= nt == N_EVENT; sawRest |= nt == N_REST; sawElite |= nt == N_ELITE;
                expectRow = tower_map_row();
                mapsSeen++;
                debugf("[nav] map row %d: next %d %d %d, cursor %d, to %d: \"%s\"\n", tower_map_row() + 2, tower_map_next(0), tower_map_next(1), tower_map_next(2), tower_map_sel(), target, seq);
            }
            else if (now == S_SHOP) {      // Mend's confirm (gold and hurt for it): B or its Back row cancels, then Buy
                MUSIC("a Shop"); extern void tower_test_set_gold(int); tower_test_set_gold(999); character.health = character.maxHealth * 0.5f; shopGold = 999;
                snprintf(seq, sizeof seq, aOnly ? ">vvAAA^AvAAA^A" : "vrB>vABAvABBB^A"); }     // (then Leave's confirm: cancelled, asked again, Up, Leave)
            else if (now == S_EVENT) { MUSIC("an Event"); snprintf(seq, sizeof seq, aOnly ? "vvvAAA^A" : "rBBBB^A"); }     // (Leave's confirm the same way)
            else if (now == S_REST) { MUSIC("a Rest"); snprintf(seq, sizeof seq, aOnly ? "AA" : "BAAvABrB^^AB"); }
            else seq[0] = 0;
        }
        if (t < 50 || (t - 50) % 24 || !seq[k]) {      // (the first press past the screens' 0.75 s input delay)
            if (t > 50 && (t - 50) % 24 == 14 && k > 0 && rdOpen && ui_details()->open)      // a panel (Details, a confirm) up: no "(R)" in it
                CHECK(ui_test_r_drawn == rdc, "no \"(R)\" drawn in the %s (screen %d)", ui_details()->confirm ? "confirm" : "Details", tower_screen());
            if (t > 50 && (t - 50) % 24 == 12) { rdc = ui_test_r_drawn; rdOpen = ui_details()->open; }
            if (t > 50 && (t - 50) % 24 == 12 && k > 0) {     // the last press's check, half-way to the next
                char c = seq[k - 1]; int sc = tower_screen();
                if (entry == -1) {
                    if (c == 'r') CHECK(det, "the draft's R: Details open");
                    if (c == 'B') CHECK(tower_draft_is_open() && !det && !menu, "the draft's B: %s (still the draft)", k == 2 ? "Details closed" : "nothing");
                    if (c == 'v') CHECK(tower_draft_selected() == 3, "Down: Skip (%d)", tower_draft_selected());
                    if (c == '^') CHECK(tower_draft_selected() == 1, "Up: back on the card (%d)", tower_draft_selected());
                    if (c == 'A') CHECK(!tower_draft_is_open() && (sc == S_MAP || !tower_between_open()), "A takes the card: the map (%d)", sc);
                } else if (entry == S_MAP) {
                    int pos = k - 1; bool aOnly = HLE_NAV_MODE == 2;
                    if (!aOnly && pos <= 1) CHECK(sc == S_MAP && tower_map_row() == expectRow && !menu, "the map's %s enters nothing", c == '^' ? "Up" : "Down");
                    if (!aOnly && pos == 2) { int opt; int m = menu_controller_test_state(&opt); CHECK(menu && m == 0 && opt == 1, "the map's B: the pause menu, on Stats (menu %d row %d)", m, opt); }
                    if (!aOnly && pos == 3) CHECK(!menu && sc == S_MAP, "B again closes it: the map");
                    if (!aOnly && pos == 4) CHECK(det && sc == S_MAP, "the map's R: the node's Details");
                    if (!aOnly && pos == 5) CHECK(!det && sc == S_MAP && !menu, "B closes them: the map (no pause menu)");
                    if ((!aOnly && pos == 6) || (aOnly && pos == 0)) CHECK(sc == S_CONFIRM && tower_map_row() == expectRow, "A: the node's page, nothing entered (%d)", sc);
                    if (!aOnly && pos == 7) CHECK(sc == S_MAP && tower_map_row() == expectRow, "the page's B: the map");
                    if ((!aOnly && pos == 10) || (aOnly && pos == 2)) CHECK(sc == S_MAP && tower_map_row() == expectRow, "the page's Back row (Down, A): the map");
                    if (!seq[k]) CHECK(tower_map_row() == expectRow + 1, "A, A: the node entered (row %d -> %d)", expectRow + 2, tower_map_row() + 2);
                } else if (entry == S_SHOP) {
                    bool aOnly = HLE_NAV_MODE == 2; UiPanel *dp = ui_details();
                    int ask1 = aOnly ? 4 : 6, cancel = ask1 + 1, ask2 = ask1 + 2, toBuy = ask1 + 3, buy = ask1 + 4;      // (Mend's confirm, by k)
                    if (!aOnly && c == 'r') CHECK(det, "the Shop's R on A level: Details open");
                    if (!aOnly && k == 3) CHECK(!det && sc == S_SHOP, "B closes them: still the Shop (%d)", sc);
                    if (!aOnly && k == 1) CHECK(tower_shop_sel() == 3, "Down: the row under (%d)", tower_shop_sel());
                    if (!aOnly && k == 4) CHECK(tower_shop_sel() == 4, "Right: the next item (%d)", tower_shop_sel());
                    if (k == ask1 - 1) CHECK(tower_shop_sel() == 7, "on Mend (%d)", tower_shop_sel());
                    if (k == ask1 || k == ask2) CHECK(dp->open && dp->confirm && dp->button == 1 && tower_gold() == shopGold, "Mend's A: its confirm (\"%s\"), on Back, nothing paid (gold %d)", dp->title, tower_gold());
                    if (k == cancel) CHECK(!dp->open && sc == S_SHOP && tower_shop_sel() == 7 && tower_gold() == shopGold, "%s cancels: nothing paid, the cursor on Mend (gold %d)", aOnly ? "its Back row (A)" : "B", tower_gold());
                    if (k == toBuy) CHECK(dp->open && dp->button == 0, "%s: on Buy", aOnly ? "Up" : "Down");
                    if (k == buy) CHECK(!dp->open && sc == S_SHOP && tower_gold() == shopGold - 50, "Buy: Mend paid (gold %d -> %d)", shopGold, tower_gold());
                    int L = (int)strlen(seq);
                    if (k == L - 4 || k == L - 2) CHECK(tower_leave_asking() && dp->button == 1 && sc == S_SHOP, "the Shop's %s: Leave's confirm, on Back", aOnly ? "Leave (A)" : "B");
                    if (k == L - 3) CHECK(!dp->open && sc == S_SHOP && tower_gold() == shopGold - 50, "%s on it: still the Shop", aOnly ? "its Back row (A)" : "B");
                    if (!seq[k]) CHECK(sc == S_MAP, "Leave's Leave: the Shop left, the map (%d)", sc);
                } else if (entry == S_EVENT) {
                    if (c == 'r') CHECK(det, "the Event's R: Details open");
                    if (c == 'B' && k == 2) CHECK(!det && sc == S_EVENT, "B closes them: still the Event (%d)", sc);
                    bool aOnly = HLE_NAV_MODE == 2; UiPanel *dp = ui_details(); int L = (int)strlen(seq);
                    if (k == L - 4 || k == L - 2) CHECK(tower_leave_asking() && dp->button == 1 && sc == S_EVENT, "the Event's %s: Leave's confirm, on Back", aOnly ? "Leave (A)" : "B");
                    if (k == L - 3) CHECK(!dp->open && sc == S_EVENT && !tower_event_done(), "%s on it: still the Event, nothing chosen", aOnly ? "its Back row (A)" : "B");
                    if (!seq[k]) CHECK(sc == S_MAP, "Leave's Leave: the Event left, the map (%d)", sc);
                } else if (entry == S_REST) {
                    bool aOnly = HLE_NAV_MODE == 2;
                    if (!aOnly && k == 1) CHECK(sc == S_MAP && tower_map_row() == expectRow, "the Rest's B: the map, the Rest still to choose");
                    if (!aOnly && k == 3) CHECK(sc == S_REST, "A, A: the Rest again");
                    if (!aOnly && k == 5) CHECK(sc == S_TEMPER || sc == S_RELEASE || sc == S_MAP, "Down, A: Temper's list (or, nothing to Temper, Down passed it: the potions taken) (%d)", sc);
                    if (!aOnly && k == 6) CHECK(sc == S_REST && !det, "B: back to the Rest's choices (%d)", sc);
                    if (!aOnly && k == 7) CHECK(det && sc == S_REST, "R on its row: Details");
                    if (!aOnly && k == 8) CHECK(!det && sc == S_REST, "B closes them: still the Rest");
                    if (!seq[k]) CHECK(sc == S_MAP, "the Rest taken, %s: the map (%d)", aOnly ? "Keep (A)" : "B keeps the boons", sc);
                }
            }
            return;
        }
        // a press: the moves on the D-pad, or the stick (one push a press)
        char c = seq[k++];
        bool stick = HLE_NAV_MODE == 1;
        switch (c) {
        case 'A': PR(a); break;
        case 'B': PR(b); break;
        case 'r': PR(r); break;
        case '>': if (stick) in->stick_x = 70; else pressed->d_right = 1; break;
        case '<': if (stick) in->stick_x = -70; else pressed->d_left = 1; break;
        case '^': if (stick) in->stick_y = 70; else pressed->d_up = 1; break;
        case 'v': if (stick) in->stick_y = -70; else pressed->d_down = 1; break;
        default: break;     // '.': a press's time, no press
        }
        debugf("[nav] press %c%s on screen %d (frame %d)\n", c, stick && strchr("<>^v", c) ? " (stick)" : "", now, frame);
        #undef PR
        #undef CHECK
        #undef MUSIC
        return;
    }
#endif
#ifdef HLE_TOWER_GALLERY
    // Every Tower screen in its worst case (tower.c's tower_test_gallery), then an Elite fight's HUD and name bar, the
    // pause menu's Stats and Build pages (A along them, Down to rows, A for a row's Details, R to close) and the death
    // screen. [gallery] lines give each one's VI frame (build with HLE_LOG_MOVE_MS: Mupen's shot numbers)
    {
        extern const char *tower_test_gallery(int); extern bool tower_active(void), tower_draft_is_open(void), tower_between_open(void);
        extern uint32_t dragon_vi_frame(void);
#ifndef HLE_GALLERY_HOLD
#define HLE_GALLERY_HOLD 40
#endif
        static int phase = 0, t0 = 0, step = 0, k = 0;
        Boss *boss = boss_get_instance();
        int t = frame - t0;
        #define PR(f) do { pressed->f = 1; in->btn.f = 1; } while (0)
        if (phase == 0) {
            if (menu_controller_is_active()) { phase = 1; t0 = frame; }
            else if (frame > 200 && frame % 30 == 0) PR(a);
        } else if (phase == 1) {
            static const char seq[] = "DDAA";
            if (t >= 20 && step < 4) { if (seq[step] == 'D') pressed->d_down = 1; else PR(a); step++; t0 = frame; }
            if (step == 4 && boss && tower_active() && boss->mode == BOSS_MODE_COMBAT) { phase = 2; t0 = frame; debugf("[gallery] the run's first floor: %s, map %d (VI %d)\n", boss->name, tower_between_open(), (int)dragon_vi_frame()); }
        } else if (phase == 2) {
            if (t >= 60 && t % 60 == 0 && boss->health > 0.0f) boss_apply_damage(boss, boss->maxHealth * 2.0f);     // (again if a move shrugged it off)
            if (tower_draft_is_open() && t > 60) { phase = 3; t0 = frame; }
        } else if (phase == 3) {
            if (t % HLE_GALLERY_HOLD == 0) {
                const char *nm = tower_test_gallery(k);
                if (!nm) { phase = 4; t0 = frame; step = 0; return; }
                debugf("[gallery] %d %s (VI %d, frame %d)\n", k, nm, (int)dragon_vi_frame(), frame);
                k++;
            }
        } else if (phase == 4) {      // A, A: the Elite's fight; its HUD; then the pause menu's pages
            static const char seq[] = "AA.....SDARRRDAr.BB.....";      // (Right x3: the Build page; Down a row, A its Details, R closes)
            if (t >= 30 && seq[step]) {
                char c = seq[step];
                if (c == 'A') PR(a); else if (c == 'B') PR(b); else if (c == 'S') PR(start); else if (c == 'r') PR(r);
                else if (c == 'D') pressed->d_down = 1; else if (c == 'U') pressed->d_up = 1; else if (c == 'R') pressed->d_right = 1;
                debugf("[gallery] press %c (VI %d, frame %d)\n", c, (int)dragon_vi_frame(), frame);
                step++; t0 = frame;
            }
            if (!seq[step]) { phase = 6; t0 = frame; }
        } else if (phase == 6) {      // the Elite down: its draft over the Sun's hall (its fire)
            if (t == 1) boss_apply_damage(boss, boss->maxHealth * 2.0f);
            if (tower_draft_is_open() && t > 1) { debugf("[gallery] the draft over the Sun's hall (VI %d, frame %d)\n", (int)dragon_vi_frame(), frame); phase = 5; t0 = frame; }     // (then the death: over the draft)
            if (t > 600) { phase = 5; t0 = frame; }
        } else if (phase == 5) {
            if (t == 100 && character_get_state() != CHAR_STATE_DEAD) { character.health = 1.0f; character_set_state(CHAR_STATE_NORMAL); character_apply_damage(1000.0f); }
            if (t == 70) { character.health = 1.0f; character_set_state(CHAR_STATE_NORMAL); character_apply_damage(1000.0f); debugf("[gallery] death (VI %d, frame %d)\n", (int)dragon_vi_frame(), frame); }
        }
        #undef PR
        return;
    }
#endif
#ifdef HLE_TEST_TOWER_STATS
    // The pause menu's Stats pages with every boon held (tower.c fills the run): Title > Tower >
    // Climb; in the fight Start, D-down (Stats), A: page 1; A: page 2; Right: page 1; Left: page 2;
    // A: back to the pause menu
    {
        static int phase = 0, t0 = 0, step = 0;
        Boss *boss = boss_get_instance();
        int t = frame - t0;
        if (phase == 0) {
            if (menu_controller_is_active()) {
                phase = 1; t0 = frame; debugf("[stats] title at %d\n", frame);
#ifdef HLE_SHRINE_STATE     // test aid: the Shrine's extremes: 0 nothing owned and no Embers, 1 everything owned and 65535 Embers
                TowerSave ts; save_controller_get_tower(&ts);
                if (HLE_SHRINE_STATE) { ts.embers = 65535; ts.unlocks = 0x1ff; ts.relicsOwned = 0x1f; ts.relicEquipped = 3; ts.flags |= TOWER_FLAG_NIGHTFARER; for (int u = 0; u < 5; u++) ts.levels[u] = 3; }
                else { ts.embers = 0; ts.unlocks = 0; ts.relicsOwned = 0; ts.relicEquipped = 0; ts.flags &= ~TOWER_FLAG_NIGHTFARER; for (int u = 0; u < 5; u++) ts.levels[u] = 0; }
                save_controller_set_tower(&ts);
#endif
#ifdef HLE_BEST_FLOOR       // test aid: the save's best floor (the milestones, the Offerings' locks, the Codex's last page)
                { TowerSave tb; save_controller_get_tower(&tb); tb.bestFloor = HLE_BEST_FLOOR; save_controller_set_tower(&tb); }
#endif
            }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {
            extern bool tower_active(void);
#ifdef HLE_CODEX_WALK   // instead: Tower > Codex, every topic open and every page of it (Right turns), B closes it; to the next
            // topic by Down (and Right onto its page)
            {
                extern int tower_codex_count(void); extern bool tower_codex_open_ok(int); extern const char *tower_codex_title(int);
                extern int menu_controller_test_nav(int *, int *, int *), menu_controller_test_pages(int *);
                static int k = 0, sub = 0; static const char pre[] = "DDADDDA";
                UiPanel *dp = ui_details();
                if (t < 45) return;
                t0 = frame;
                if (pre[step]) { char c = pre[step++]; if (c == 'D') pressed->d_down = 1; else { pressed->a = 1; in->btn.a = 1; } return; }
                if (k >= tower_codex_count()) { if (k++ == tower_codex_count()) debugf("[codexwalk] done (frame %d)\n", frame); return; }
                if (!dp->open) {
                    int row, back, page, per; menu_controller_test_nav(&row, &back, &page); menu_controller_test_pages(&per);
                    if (page < k / per) { pressed->d_right = 1; return; }
                    if (row != k % per) { if (row < k % per) pressed->d_down = 1; else pressed->d_up = 1; return; }
                    if (sub == 0) { debugf("[codexwalk] list, topic %d %s%s (frame %d)\n", k, tower_codex_title(k), tower_codex_open_ok(k) ? "" : " (locked)", frame); sub = 1; return; }
                    if (sub == 1 && tower_codex_open_ok(k)) { pressed->a = 1; in->btn.a = 1; sub = 2; return; }
                    k++; sub = 0; return;     // (closed, or locked: the next topic)
                }
                if (sub == 2) { debugf("[codexwalk] topic %d %s page %d/%d (frame %d)\n", k, tower_codex_title(k), dp->pg.page + 1, dp->pg.pages, frame); sub = 3; return; }
                if (dp->pg.page + 1 < dp->pg.pages) { pressed->d_right = 1; sub = 2; return; }      // (Right: the next page)
                pressed->b = 1; in->btn.b = 1; sub = 4; return;
            }
#endif
#ifdef HLE_TITLE_SEQ    // instead: these presses on the title's menus (D/U/L/R, A, B), 1 s apart, and no climb
            {
                static const char tseq[] = HLE_TITLE_SEQ;
                if (t >= 60 && tseq[step]) {
                    char c = tseq[step];
                    if (c == 'D') pressed->d_down = 1; else if (c == 'U') pressed->d_up = 1; else if (c == 'L') pressed->d_left = 1;
                    else if (c == 'R') pressed->d_right = 1; else if (c == 'A') { pressed->a = 1; in->btn.a = 1; } else if (c == 'B') { pressed->b = 1; in->btn.b = 1; }
                    else if (c == 'r') { pressed->r = 1; in->btn.r = 1; }
                    debugf("[stats] title step %c at frame %d\n", c, frame);
                    step++; t0 = frame;
                }
                return;
            }
#endif
            static const char seq[] = "DDAA";     // (Story is the first row)
            if (t >= 20 && step < 4) { if (seq[step] == 'D') pressed->d_down = 1; else { pressed->a = 1; in->btn.a = 1; } step++; t0 = frame; }
            if (step == 4 && boss && tower_active()) { phase = 2; t0 = frame; step = 0; debugf("[stats] climbing at %d\n", frame); }
        } else if (phase == 2) {
            static bool fighting;
            extern bool tower_between_open(void);
            if (!fighting) { if (!boss || boss->mode != BOSS_MODE_COMBAT || tower_between_open()) { t0 = frame; if (frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; } return; } fighting = true; t0 = frame; return; }   // (the map first: A enters the Guardian)
            { extern int tower_screen(void); if (tower_screen() >= 5) { hle_tower_screen(in, pressed, frame); t0 = frame; return; } }     // a Shop or an Event on the way
#ifdef HLE_TOWER_HEALCHECK   // the healing boons, measured in the fight (every boon held: Bloodthirst 2, Rally, Phantom Step)
            if (t == 100) {
                extern void tower_on_perfect_dodge(void); extern float tower_mod_perfect_window(void), tower_mod_damage_dealt(void);
                float mh = character.maxHealth;
                character.health = 100.0f; boss_apply_hit(boss, 4.0f, 1.0f);
                float skillSteal = character.health - 100.0f;     // as a skill lands: nothing
                character.health = 100.0f; bossHitBySword = true; for (int i = 0; i < 20; i++) { boss->health = boss->maxHealth; boss_apply_hit(boss, 4.0f, 0.0f); } bossHitBySword = false;
                float steal = (character.health - 100.0f) / 20.0f;     // (an average: the Impaired curse drops some)
                debugf("[heal] a skill's hit of 4 heals %.2f (0)\n", (double)skillSteal);
                character.health = mh; character_set_state(CHAR_STATE_NORMAL); character_apply_damage(40.0f);
                float lost = mh - character.health, h0 = character.health;
                boss->health = boss->maxHealth; boss_apply_hit(boss, 4.0f, 0.0f);
                float back1 = character.health - h0;
                for (int i = 0; i < 40; i++) { boss->health = boss->maxHealth; boss_apply_hit(boss, 4.0f, 0.0f); }
                float back = character.health - h0;
                debugf("[heal] Rally: one hit of 4 (x%.2f after boons) wins back %.1f (0.5: Lv 1, an eighth of its base; TOWER_STATS holds Rally Lv 1/2)\n", (double)tower_mod_damage_dealt(), (double)back1);
                character.health = 100.0f; tower_on_perfect_dodge();
                debugf("[heal] damage x%.2f max %.0f | sword hit 4 heals %.2f (12%%x2 of base: 0.96) | lost %.1f, Rally won back %.1f (%.0f%%, all: 100) | perfect dodge +%.1f (2%%: %.1f), window %.2f s\n",
                       (double)tower_mod_damage_dealt(), (double)mh, (double)steal, (double)lost, (double)back, (double)(lost > 0.0f ? 100.0f * back / lost : 0.0f),
                       (double)(character.health - 100.0f), (double)(mh * 0.02f), (double)tower_mod_perfect_window());
            }
            if (t < 120) return;
#endif
            character.health = character.maxHealth;     // (alive for the shots)
#ifdef HLE_STATS_SEQ     // another walk through the pause menu (S, D/L/R, A, B)
            static const char seq[] = HLE_STATS_SEQ;
#else
            static const char seq[] = "SDARRRRRLDDADBBDDDADDAB";
#endif   // pause, Stats, Right x4 (the full bar: Stats 1-3, Build 1-2), Right on the last (stays), Left (Build 1),
         // Down x2 (a row), A (its Details), Down (it stays open), B closes it, B (the pause menu, on Stats), down to Controls, its Back
         // (A), B closes the pause menu
            if (t >= (step ? 60 : 300) && seq[step]) {
                char c = seq[step];
                extern bool tower_active(void);
                if (!step) debugf("[stats] in the Tower: %d\n", tower_active());
                if (c == 'S') { pressed->start = 1; in->btn.start = 1; } else if (c == 'D') pressed->d_down = 1;
                else if (c == 'A') { pressed->a = 1; in->btn.a = 1; } else if (c == 'B') { pressed->b = 1; in->btn.b = 1; }
                else if (c == 'R') pressed->d_right = 1; else if (c == 'L') pressed->d_left = 1; else if (c == 'U') pressed->d_up = 1;
                else if (c == 'r') { pressed->r = 1; in->btn.r = 1; }
                debugf("[stats] step %c at frame %d\n", c, frame);
                step++; t0 = frame;
            }
            if (step && t == 20) {     // where the press left it
                extern int menu_controller_test_state(int *), menu_controller_test_run(int *, int *, int *);
                int opt, page, pages, row, m = menu_controller_test_state(&opt), sel = menu_controller_test_run(&page, &pages, &row);
                debugf("[stats] after %c: menu %d (active %d) row %d | Stats page %d of %d, corner %d, Build row %d, Details %d\n", seq[step - 1], m, menu_controller_is_active(), opt,
                       page + 1, pages, sel, row, ui_details()->open);
            }
        }
        return;
    }
#endif
#ifdef HLE_TEST_TOWER_UI
    // Screens and the skill cards: 200 Embers; Title > Tower > Shrine: buy Honed Blade
    // (row 3); back, Climb; no skill works on floor 1; kill the boss: the draft, take its
    // first card (always a skill card): on floor 2 that one skill works; then pause >
    // Main Menu > Load Game.
    {
        extern bool tower_draft_is_open(void), tower_mod_skill_allowed(int);
        extern int tower_floor(void);
        extern int tower_upgrade_level(int), tower_embers(void);
        static int phase = 0, t0 = 0, step = 0;
        Boss *boss = boss_get_instance();
        int t = frame - t0;
        if (phase == 0) {
            if (menu_controller_is_active()) {
                TowerSave ts; save_controller_get_tower(&ts); ts.embers = 200; save_controller_set_tower(&ts);
                phase = 1; t0 = frame; debugf("[ui] title at %d\n", frame);
            } else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {
            // DDD A (Tower) . A? no: D (Shrine) A; D D A (Honed Blade); D D D D A (Halfmoon); B; U A (Climb)
            static const char seq[] = "DDA.DA.DDAUA.B.UA";
            if (t >= 25 && seq[step]) {
                char c = seq[step];
                if (c == 'D') pressed->d_down = 1; else if (c == 'U') pressed->d_up = 1;
                else if (c == 'A') { pressed->a = 1; in->btn.a = 1; } else if (c == 'B') { pressed->b = 1; in->btn.b = 1; }
                if (c == '.') debugf("[ui] step %d at %d\n", step, frame);
                step++; t0 = frame;
            }
            if (!seq[step]) {
                debugf("[ui] bought: Honed Blade %d; Embers left %d\n", tower_upgrade_level(2), tower_embers());
                phase = 2; t0 = frame;
            }
        } else if (phase == 2) {
            static bool fighting;
            extern bool tower_between_open(void), tower_map_is_open(void);
            if (!fighting) { if (!boss || boss->mode != BOSS_MODE_COMBAT || tower_between_open()) { t0 = frame; if (frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; } return; } fighting = true; t0 = frame; return; }   // (the map first)
            if (t == 40) pressed->d_up = 1;
            if (t == 44) debugf("[ui] floor 1, D-up: state %d (%s)\n", character_get_state(), character_get_state() == CHAR_STATE_SKILL ? "CAST: not locked" : "locked");
            if (t == 120) { debugf("[ui] kill at %d\n", frame); boss_apply_damage(boss, boss->maxHealth * 2.0f); }
            if (t == 240) debugf("[ui] draft %s at %d\n", tower_draft_is_open() ? "open" : "CLOSED", frame);
            if (t == 300) { pressed->a = 1; in->btn.a = 1; }      // the first card
            static bool mapSeen;
            if (t == 340 && !mapSeen) { mapSeen = true; debugf("[ui] map %s at %d\n", tower_map_is_open() ? "open" : "CLOSED", frame); }
            if (t >= 340 && t < 420 && tower_between_open()) {     // the map: A on its node (a Rest: heal, keep the boons; a Shop: B; an Event: A), then the map again
                extern int tower_screen(void);
                t0 = frame - 341;
                if (tower_screen() >= 5) hle_tower_screen(in, pressed, frame);
                else if (frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            }
            if (t == 420) debugf("[ui] floor %d: skills usable %d %d %d\n", tower_floor(), tower_mod_skill_allowed(0), tower_mod_skill_allowed(1), tower_mod_skill_allowed(2));
            if (t == 430) { int k = tower_mod_skill_allowed(0) ? 0 : tower_mod_skill_allowed(1) ? 1 : 2;
                            if (k == 0) pressed->d_up = 1; else if (k == 1) pressed->d_right = 1; else pressed->d_down = 1; }
            if (t == 434) debugf("[ui] the learned skill: state %d (%s)\n", character_get_state(), character_get_state() == CHAR_STATE_SKILL ? "cast" : "NOT CAST");
            if (t == 520) { pressed->start = 1; in->btn.start = 1; }
            if (t == 540 || t == 550 || t == 560 || t == 570) pressed->d_down = 1;
            if (t == 590) { pressed->a = 1; in->btn.a = 1; }
            if (t == 700) { phase = 3; t0 = frame; }
        } else if (phase == 3) {
            if (t == 30 || t == 40 || t == 50) pressed->d_down = 1;   // Load Game
            if (t == 70) { pressed->a = 1; in->btn.a = 1; debugf("[ui] load game at %d\n", frame); }
        }
        return;
    }
#endif
#ifdef HLE_TEST_TOWER_RUN
#ifndef HLE_STRIKE_FRAMES
#define HLE_STRIKE_FRAMES 1200      // HLE_RUN_STRIKE's fight (frames)
#endif
    // A Tower run from the title: Down x3 (Tower), A, A (Climb). Each floor: check the
    // boss, a locked skill (D-up does nothing), kill it, take a boon (A). After floor 6,
    // die: the death screen's summary, then A back to the title.
    {
        extern int tower_floor(void), tower_floor_boss(void);
        extern bool tower_draft_is_open(void), tower_active(void), tower_between_open(void), tower_map_is_open(void), tower_rest_is_open(void); extern int tower_draft_selected(void);
        extern const char *tower_death_summary(void);
        static int phase = 0, t0 = 0, step = 0, lastFloor = 0;
        Boss *boss = boss_get_instance();
        int t = frame - t0;
        if (phase == 0) {
#ifdef HLE_HUMAN_BOT
            { extern void hb_init(void); hb_init(); }
#endif
            if (menu_controller_is_active()) { phase = 1; t0 = frame; }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {
            static const char seq[] = "DDAA";     // (Story is the first row)
            if (t >= 20 && step < 4) { if (seq[step] == 'D') pressed->d_down = 1; else { pressed->a = 1; in->btn.a = 1; } step++; t0 = frame; }
            if (step == 4 && boss && tower_active()) { phase = 2; t0 = frame; }
        } else if (phase == 2) {
#ifdef HLE_TEST_LEAK    // tag allocations and the low point by floor; each floor's live heap by caller a second in
            { extern int hle_leak_cyc; extern void hle_low_report(int), hle_leak_by_caller(const char *);
              hle_leak_cyc = tower_floor();
              if (lastFloor && t == 60) { char w[16]; snprintf(w, sizeof w, "floor%d", lastFloor); hle_leak_by_caller(w); }
              if (lastFloor && t == 59) for (int f = 1; f <= lastFloor; f++) hle_low_report(f); }
#endif
#ifdef HLE_HUMAN_BOT       // a run over, back at the title: the next one (HLE_HB_RUNS in all)
#ifndef HLE_HB_RUNS
#define HLE_HB_RUNS 1
#endif
            if (!boss && !tower_active()) {
                static int idle;
                if (++idle > 120 && menu_controller_is_active()) {
                    extern int hb_runs; extern void hb_next_run(void);
                    idle = 0; hb_runs++;
                    if (hb_runs >= HLE_HB_RUNS) { debugf("[hbrun] all %d runs done\n", hb_runs); phase = 9; return; }
                    hb_next_run();
                    phase = 1; step = 0; t0 = frame; lastFloor = 0;
                }
                return;
            }
            { static int fr, between; fr++; if (tower_between_open()) between++;
              if (fr % 3600 == 0) debugf("[hbtime] %d frames in the run, %d on the screens between fights\n", fr, between); }
#endif
            if (!boss) return;
#ifdef HLE_RUN_CLIMB
            static int heapMin = 1 << 30;     // the lowest free heap in the climb, sampled every 10 frames
            if (frame % 10 == 0) { heap_stats_t h; sys_get_heap_stats(&h); if (h.total - h.used < heapMin) heapMin = h.total - h.used; }
#endif
            { static bool was; bool now = tower_draft_is_open();     // the draft's open and close (a milestone's notice over it), for shots
              if (now != was) debugf("[run] draft %s at floor %d, notice %d (frame %d)\n", now ? "open" : "closed", tower_floor(), ui_details()->open, frame);
              was = now; }
            if (tower_map_is_open() || tower_rest_is_open()) {     // the map (and a Rest): A on the cursor's node (straight up), heal, keep the boons
#ifdef HLE_MAP_WALK
                extern void hle_map_walk(joypad_inputs_t *, joypad_buttons_t *, int); hle_map_walk(in, pressed, frame);
#else
                extern int tower_screen(void);
                if (tower_screen() >= 5) hle_tower_screen(in, pressed, frame);     // a Shop (B leaves; HLE_TEST_TOWER_SHOP buys), an Event (A)
                else if (frame % 40 == 0) { pressed->a = 1; in->btn.a = 1; }
#endif
                return;
            }
            if (tower_floor() != lastFloor && boss->mode == BOSS_MODE_COMBAT && !tower_between_open()) {
                lastFloor = tower_floor(); t0 = frame;
#ifdef HLE_LOG_FPS
                { extern int hle_floor_mark; hle_floor_mark = 1; }   // test aid: count this floor's fight apart from its load
#endif
                debugf("[run] floor %d boss %d phase %d hp %.0f/%.0f | player hp %.0f/%.0f potions %d\n", lastFloor, tower_floor_boss(),
                       boss->phaseIndex, (double)boss->health, (double)boss->maxHealth, (double)character.health, (double)character.maxHealth,
                       character.healthPotions);
#ifdef HLE_RUN_CLIMB    // the long climb: the run's numbers each floor
                {
                    extern float tower_mod_damage_dealt(void), tower_mod_damage_taken(void), tower_mod_max_health(void);
                    extern int tower_run_embers(void), tower_circle(void);
                    heap_stats_t h; sys_get_heap_stats(&h);
                    extern int tower_gold(void); extern const char *tower_affix_tag(void);
                    debugf("[climb] floor %d circle %d boss %d hp %.0f%s%s | dealt x%.2f taken x%.2f health %.0f/%.0f potions %d gold %d | embers %d | heap %d KB free (min so far %d)\n",
                           lastFloor, tower_circle(), tower_floor_boss(), (double)boss->maxHealth, tower_affix_tag()[0] ? " elite " : "", tower_affix_tag(),
                           (double)tower_mod_damage_dealt(), (double)tower_mod_damage_taken(), (double)character.health, (double)tower_mod_max_health(),
                           character.healthPotions, tower_gold(), tower_run_embers(), (h.total - h.used) / 1024, heapMin / 1024);
                }
#endif
            }
            if (t == 30) pressed->d_up = 1;
            if (t == 34) debugf("[run] D-up: state %d (%s)\n", character_get_state(), character_get_state() == CHAR_STATE_SKILL ? "cast: Moon Blade Dance learned" : "no skill");
#ifdef HLE_RUN_ROLLS       // roll until the stamina runs out (the wait before it refills)
            if (t == 40) character.stamina = 1.0f;          // a sliver left: this roll empties the bar
            if (t == 41) { pressed->a = 1; in->btn.a = 1; in->stick_y = 80; }
#endif
#ifdef HLE_BOONFX_RUN      // (with HLE_TOWER_GIVE_NEW, HLE_LOG_BOONFX, HLE_RUN_STRIKE) the new boons' triggers in the fight
            if (t > 0 && t < HLE_STRIKE_FRAMES) {
                static int dodges, rollAt = -1, swingAt = -1;
                extern float tower_mod_damage_taken(void);
                if (character.health < character.maxHealth * 0.4f) character.health = character.maxHealth * 0.55f;   // alive, and hurt (Red Thirst)
                static int potionsAt = -1, drankAt = -1;
                if (t == 200) { potionsAt = character.healthPotions; character.health = character.maxHealth * 0.5f; }
                if (potionsAt >= 0) {       // a potion (Sanguine Flask), pressed until it's drunk (not mid-swing)
                    if (character.healthPotions < potionsAt) { debugf("[boonfx-run] potion drunk at t %d\n", t); potionsAt = -1; drankAt = t; }
                    else if (t % 2) { pressed->d_left = 1; in->btn.d_left = 1; }
                }
                if (t >= 300 && t <= 320 && t % 4 == 0 && character_get_state() != CHAR_STATE_SKILL && character_get_skill_cooldown(SKILL_CRESCENT_CLEAVE) <= 0.0f) {
                    character_set_state(CHAR_STATE_NORMAL); character.stamina = character.maxStamina;   // a Cleave before the rolls: a cooldown running (Waning Light)
                    pressed->d_down = 1; in->btn.d_down = 1;
                }
                if (drankAt > 0 && t == drankAt + 30) {     // a blow 1 s after it (Steel Hearth's ward)
                    float h = character.health; character_set_state(CHAR_STATE_NORMAL); character_apply_damage(20.0f);
                    debugf("[boonfx-run] a blow of 20 a second after the potion (x%.2f taken): health %.1f -> %.1f\n", (double)tower_mod_damage_taken(), (double)h, (double)character.health);
                }
                if (t >= 400 && t % 200 == 199 && dodges < 4) { character.stamina = character.maxStamina * 0.5f; character_set_state(CHAR_STATE_NORMAL); }
                if (t >= 400 && t % 200 == 0 && dodges < 4) { pressed->a = 1; in->btn.a = 1; in->stick_y = 80; rollAt = t; }   // a roll...
                if (rollAt > 0 && t == rollAt + 4) {     // ... and a blow inside its first 0.2 s: a perfect dodge (Evasion, Vanish)
                    float st0 = character.stamina;
                    debugf("[boonfx-run] blow in a roll: state %d (rolling %d), stamina %.0f\n", character_get_state(), CHAR_STATE_ROLLING, (double)st0);
                    character_apply_damage(10.0f);
                    debugf("[boonfx-run] stamina after %.0f\n", (double)character.stamina);
                    dodges++; swingAt = t + 16;
                }
                if (rollAt > 0 && t == swingAt) {     // out of the roll, at the boss, a swing: a hit inside the window
                    float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2], d = sqrtf(dx * dx + dz * dz);
                    if (d > 30.0f) { character.pos[0] += dx / d * (d - 30.0f); character.pos[2] += dz / d * (d - 30.0f); }
                    character.rot[1] = atan2f(-dx, dz);
                    character_set_state(CHAR_STATE_NORMAL);
                    pressed->b = 1; in->btn.b = 1;
                    debugf("[boonfx-run] a swing after the roll\n");
                    rollAt = -1;
                }
                if (t == 1300) { float h = character.health; character_set_state(CHAR_STATE_NORMAL); character_apply_damage(20.0f);    // Vanish armed: 0
                                 debugf("[boonfx-run] a blow of 20 after 3 perfect dodges: health %.1f -> %.1f\n", (double)h, (double)character.health); }
                if (t == 1400) { character.health = character.maxHealth; float h = character.health; character_set_state(CHAR_STATE_NORMAL); character_apply_damage(70.0f);   // Stalwart: over 20%
                                 debugf("[boonfx-run] a blow of 70 (x%.2f taken): health %.1f -> %.1f\n", (double)tower_mod_damage_taken(), (double)h, (double)character.health); }
                static int staggerCast = -1; static bool cast;
                if (boss->staggerT > 0.0f && staggerCast < 0) {     // a stagger: Crescent Cleave on it (Eclipse, Waxing Moon), then swings
                    character_set_state(CHAR_STATE_NORMAL); character.stamina = character.maxStamina;
                    pressed->d_down = 1; in->btn.d_down = 1; staggerCast = t; cast = false;
                    debugf("[boonfx-run] Crescent Cleave on the staggered boss (%.1f s left, cooldown %.1f s)\n", (double)boss->staggerT, (double)character_get_skill_cooldown(SKILL_CRESCENT_CLEAVE));
                }
                if (staggerCast >= 0 && !cast) {      // pressed again until it casts (a press mid-swing or mid-roll does nothing)
                    if (character_get_state() == CHAR_STATE_SKILL) cast = true;
                    else if (t > staggerCast && t < staggerCast + 60 && t % 4 == 0) {
                        character_set_state(CHAR_STATE_NORMAL); character.stamina = character.maxStamina;
                        pressed->d_down = 1; in->btn.d_down = 1;
                    }
                }
                if (boss->staggerT <= 0.0f) staggerCast = -1;
                bool castOver = staggerCast >= 0 && (cast || t > staggerCast + 60) && t > staggerCast + 10 && character_get_state() != CHAR_STATE_SKILL;
                if (castOver || (t > 400 && t < 1100 && t % 200 > 10 && t % 200 < 130)) {     // close in after the cast and the rolls: sword hits in the windows (Waxing Moon, Shadow Edge)
                    float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2], d = sqrtf(dx * dx + dz * dz);
                    if (d > 34.0f) { character.pos[0] += dx / d * (d - 34.0f); character.pos[2] += dz / d * (d - 34.0f); }
                    if (t % 8 == 0) { pressed->b = 1; in->btn.b = 1; }     // and swing
                }
                if (t == 2100 || t == 2600) { boss->poise = 0.99f; debugf("[boonfx-run] the stagger meter set to 0.99: the next hit breaks it\n"); }
                if (t % 600 == 0) debugf("[boonfx-run] t %d boss hp %.0f/%.0f poise %.2f player %.0f\n", t, (double)boss->health, (double)boss->maxHealth, (double)boss->poise, (double)character.health);
                if (boss->health < boss->maxHealth * 0.7f) boss->health = boss->maxHealth;     // the fight lasts the window
            }
#endif
#ifdef HLE_HUMAN_BOT       // the human-like bot (hle/human_bot.c): it learns the moves' timing and rolls through them, swings and casts in the
                            // openings, drinks when it's safe; dies for real. Runs follow one another (HLE_HB_RUNS), each its own seed, plan and route
            {
                extern void hb_fight_begin(Boss *), hb_fight_end(const char *, Boss *), hb_fight(joypad_inputs_t *, joypad_buttons_t *, Boss *), hb_init(void), hb_dump_table(void);
                extern bool hb_fight_on(void);
                static int deadAt;
                hb_init();
                if (character_get_state() == CHAR_STATE_DEAD) {
                    if (!deadAt) { deadAt = frame; hb_fight_end("died", boss); extern int tower_circle(void), tower_gold(void);
                        debugf("[bot] died on floor %d (circle %d), boss %d at %.0f/%.0f, potions %d, gold %d\n", tower_floor(), tower_circle(), tower_floor_boss(),
                               (double)boss->health, (double)boss->maxHealth, character.healthPotions, tower_gold());
                        extern int tower_run_embers(void), tower_best_floor(void), tower_embers(void);
                        debugf("[hbrun] end: floor %d circle %d run embers %d, gold %d, best floor %d, Embers banked %d\n", tower_floor(), tower_circle(), tower_run_embers(), tower_gold(), tower_best_floor(), tower_embers());
                        hb_dump_table(); }
                    if (frame - deadAt == 60) debugf("[run] died: \"%s\"\n", tower_death_summary());
                    if (frame - deadAt > 150 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
                    return;
                }
                deadAt = 0;
#ifndef HLE_HB_MAXCIRCLE
#define HLE_HB_MAXCIRCLE 13     // a run this deep is ended (the time a marathon has): "[hbrun] capped"
#endif
                { extern int tower_circle(void); static int capped;
                  if (tower_circle() > HLE_HB_MAXCIRCLE && !capped) { capped = 1; debugf("[hbrun] capped at circle %d floor %d\n", tower_circle(), tower_floor()); character.health = 0.0f; character_set_state(CHAR_STATE_DEAD); return; }
                  if (tower_circle() <= HLE_HB_MAXCIRCLE) capped = 0; }
                if (boss->mode == BOSS_MODE_COMBAT && boss->health > 0.0f && boss->state != BOSS_STATE_DEAD) {
                    if (!hb_fight_on()) { hb_fight_begin(boss); t0 = frame; t = 0; }
                    hb_fight(in, pressed, boss);
                    if (t == 300 * 60) { debugf("[bot] floor %d: timed out at 300 s, boss %.0f/%.0f\n", tower_floor(), (double)boss->health, (double)boss->maxHealth); hb_fight_end("timeout", boss); boss_apply_damage(boss, boss->maxHealth * 2.0f); }
                } else if (hb_fight_on() && (boss->health <= 0.0f || boss->state == BOSS_STATE_DEAD)) hb_fight_end("won", boss);   // (not a break: the Sun's)
            }
#elif defined(HLE_RUN_BOT)         // the climbing bot (HLE_RUN_PATH's climbs): stays close and strikes in the boss's openings, drinks below 40%;
                            // it takes the metric's 30% of each blow (character.c), its sword x1.9 (tower.c: the metric's rate), and dies for real
            {
                extern bool hle_open_now;
                static int k, lastRoll, deadAt;
                if (character_get_state() == CHAR_STATE_DEAD) {
                    if (!deadAt) { deadAt = frame; extern int tower_circle(void), tower_gold(void);
                        debugf("[bot] died on floor %d (circle %d), boss %d at %.0f/%.0f, potions %d, gold %d\n", tower_floor(), tower_circle(), tower_floor_boss(),
                               (double)boss->health, (double)boss->maxHealth, character.healthPotions, tower_gold()); }
                    if (frame - deadAt == 60) debugf("[run] died: \"%s\"\n", tower_death_summary());
                    if (frame - deadAt > 150 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
                    return;
                }
                if (boss->mode == BOSS_MODE_COMBAT && boss->health > 0.0f) {
                    k++;
                    if (k % 60 == 1) pressed->z = 1;
                    in->btn.z = 1;
                    float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2], d = sqrtf(dx * dx + dz * dz);
                    if (character.health < character.maxHealth * 0.4f && character.healthPotions > 0 && character_get_state() == CHAR_STATE_NORMAL && k % 10 == 0) { pressed->d_left = 1; in->btn.d_left = 1; }
                    else {      // close in and stay there, as the passive dummy the metric is taken on; swing in the openings
                        if (d > 34.0f) in->stick_y = 70;
                        if (hle_open_now && d < 40.0f && k % 20 == 0) { pressed->b = 1; in->btn.b = 1; }
                    }
                    (void)lastRoll;
                    if (t == 300 * 60) { debugf("[bot] floor %d: timed out at 300 s, boss %.0f/%.0f\n", tower_floor(), (double)boss->health, (double)boss->maxHealth); boss_apply_damage(boss, boss->maxHealth * 2.0f); }
                }
            }
#elif defined(HLE_RUN_STRIKE)      // fight with the sword for 20 s first (the boons that hook into hits)
#ifdef HLE_RUN_TOPUP       // (and never die: a full climb, every floor's sounds and effects, for the heap's low point)
            if (character.health < character.maxHealth * 0.6f) character.health = character.maxHealth;
#endif
#ifdef HLE_BERSERK_RUN     // (with HLE_TOWER_GIVE=BOON_BERSERKER HLE_LOG_BOONFX): the fight below half health (Berserker on), a potion pressed now and then
            if (t < HLE_STRIKE_FRAMES && (character.health > character.maxHealth * 0.45f || character.health < character.maxHealth * 0.2f)) character.health = character.maxHealth * 0.4f;
            extern bool tower_potion_blocked(void);
            if (t > 0 && t % 600 == 300) { int p0 = character.healthPotions; pressed->d_left = 1; in->btn.d_left = 1;
                debugf("[berserk-run] t %d: the potion pressed at %.0f/%.0f health, %d potions (blocked %d)\n", t, (double)character.health, (double)character.maxHealth, p0, tower_potion_blocked()); }
            if (t > 0 && t % 600 == 320) debugf("[berserk-run] t %d: potions now %d\n", t, character.healthPotions);
#endif
#ifdef HLE_HUD_LEVELS      // (icons step 1) the D-pad keys' marks: every skill known, all three at Lv 1, 2, 3, 4, then 5 (A, B, A), 120 frames each
            if (t % 120 == 60) {
                extern void tower_test_skill_lv(int skill, int lv, int form);
                int lv = (t / 120) % 5 + 1;
                for (int k = 0; k < 3; k++) tower_test_skill_lv(k, lv, 1 + (k & 1));
                debugf("[hudlv] skills at Lv %d (frame %d)\n", lv, frame);
            }
            { extern float tower_rally_pool(void); float rp = tower_rally_pool();      // the Rally pool as the bar draws it
              if (rp > 0.0f && t % 10 == 0) debugf("[rallybar] pool %d.%d of %d (frame %d)\n", (int)rp, (int)(rp * 10) % 10, (int)character.maxHealth, frame); }
#endif
            if (t < HLE_STRIKE_FRAMES) {
                if (t == 1) pressed->z = 1;
                in->btn.z = 1;
                float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2];
                if (sqrtf(dx * dx + dz * dz) > 40.0f) in->stick_y = 70;
                if (t % 24 == 0) { pressed->b = 1; in->btn.b = 1; }
                if (t % 300 == 299) debugf("[run] boss hp %.0f/%.0f\n", (double)boss->health, (double)boss->maxHealth);
            }
            if (t == HLE_STRIKE_FRAMES) {
#ifdef HLE_LOG_FPS
                { extern int hle_floor_mark; hle_floor_mark = 2; }   // the fight's end (before the next load)
#endif
                boss_apply_damage(boss, boss->maxHealth * 2.0f);
            }
#elif !defined(HLE_RUN_BOT) && !defined(HLE_HUMAN_BOT)
            if (t == 90) boss_apply_damage(boss, boss->maxHealth * 2.0f);
#endif
#if defined(HLE_RUN_BOT) || defined(HLE_HUMAN_BOT)
            if (boss->state == BOSS_STATE_DEAD) {
#else
            if (t >= 90 && t < 140) {   // stand facing the fallen boss, where its "A" prompt would show
#endif
                float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2], d = sqrtf(dx * dx + dz * dz);
                if (d > 60.0f) { character.pos[0] += dx / d * (d - 60.0f); character.pos[2] += dz / d * (d - 60.0f); }
            }
#if defined(HLE_RUN_SKIP) || defined(HLE_RUN_PICK) || defined(HLE_RUN_UPDOWN) || defined(HLE_RUN_CLIMB)   // a set card: HLE_RUN_PICK moves right that many, Skip is left of the first
            {
                static int k;
                if (!tower_draft_is_open()) k = 0;
                else {
                    k++;
#if defined(HLE_HUMAN_BOT)    // (tower.c put the cursor on the bot's card: A below)
#elif defined(HLE_RUN_CLIMB)    // take a boon while any are offered (so they run out), else the first pact
                    static int want;
                    if (k == 1) {
                        extern int tower_draft_card(int i);
                        want = 0;
                        for (int i = 2; i >= 0; i--) if (tower_draft_card(i) >= 0) want = i;
                        debugf("[climb] cards %d %d %d, taking %d\n", tower_draft_card(0), tower_draft_card(1), tower_draft_card(2), want);
                    }
                    if (k >= 50 && k < 50 + 6 * want && (k - 50) % 6 == 0) { pressed->d_right = 1; in->btn.d_right = 1; }
#elif defined(HLE_RUN_SKIP)
                    if (k == 50) { pressed->d_down = 1; in->btn.d_down = 1; }     // Down to Skip (past the draft's input delay)
#elif defined(HLE_RUN_UPDOWN)   // Right to the second card, Down to Skip, Up back: the second card is taken
#ifdef HLE_RUN_STICK            // (the same with the control stick)
                    if (k >= 50 && k < 53) in->stick_x = 70;
                    if (k >= 60 && k < 63) in->stick_y = -70;
                    if (k >= 70 && k < 73) in->stick_y = 70;
#else
                    if (k == 50) { pressed->d_right = 1; in->btn.d_right = 1; }
                    if (k == 60) { pressed->d_down = 1; in->btn.d_down = 1; }
                    if (k == 70) { pressed->d_up = 1; in->btn.d_up = 1; }
#endif
                    if (k == 80) debugf("[updown] the draft reads: card %d selected\n", tower_draft_selected());
#else
                    if (k >= 50 && k < 50 + 6 * HLE_RUN_PICK && (k - 50) % 6 == 0) { pressed->d_right = 1; in->btn.d_right = 1; }
#endif
#ifndef HLE_DRAFT_HOLD
#define HLE_DRAFT_HOLD 100      // frames the draft stays up (longer: for a screenshot; past its 0.75 s input delay)
#endif
#ifdef HLE_HUMAN_BOT    // (again every 40 frames: a milestone's notice over the draft takes the first A)
                    if (k >= HLE_DRAFT_HOLD && (k - HLE_DRAFT_HOLD) % 40 == 0) { pressed->a = 1; in->btn.a = 1; }
#else
                    if (k == HLE_DRAFT_HOLD) { pressed->a = 1; in->btn.a = 1; }
#endif
                }
            }
#elif defined(HLE_RUN_MASH)  // dodge (A) every 15 frames from the kill on, as a player rolling about after a fight: does a card get taken unseen?
            {
                extern int tower_draft_card(int i);
                static bool wasOpen; static int openAt, took; static uint32_t openMs;
                bool open = tower_draft_is_open();
                if (open && !wasOpen) { openAt = frame; openMs = get_ticks_ms(); debugf("[mash] draft open at %d: cards %d %d %d\n", frame, tower_draft_card(0), tower_draft_card(1), tower_draft_card(2)); }
                if (open) took = tower_draft_card(tower_draft_selected());
                if (!open && wasOpen) debugf("[mash] draft closed after %d frames, %lu ms: took card %d (selected slot, -1 a pact)\n", frame - openAt, (unsigned long)(get_ticks_ms() - openMs), took);
                wasOpen = open;
                if (t >= 90 && t % 15 == 0) { pressed->a = 1; in->btn.a = 1; }
            }
#else
            if (tower_draft_is_open() && t % 40 == 0) { pressed->a = 1; in->btn.a = 1; }
#endif
#ifdef HLE_RUN_PAUSE        // on floor 2: pause, open Run (one down) and stay on the page
            if (lastFloor == 2) {
                if (t == 30) pressed->start = 1;
                if (t == 50) { pressed->d_down = 1; in->btn.d_down = 1; }
                if (t == 70) { pressed->a = 1; in->btn.a = 1; }
                if (t == 400 || t == 420) { pressed->b = 1; in->btn.b = 1; }     // back, then close the pause menu
                if (t >= 70 && t < 430) return;
            }
#endif
#ifndef HLE_RUN_FLOORS
#define HLE_RUN_FLOORS 7
#endif
            if (lastFloor == HLE_RUN_FLOORS && t == 60) { phase = 3; t0 = frame; character_apply_damage(9999.0f); }
        } else if (phase == 3) {
            if (t == 30 || t == 160) debugf("[run] died: state %d, \"%s\" (frame %d)\n", character_get_state(), tower_death_summary(), frame);
            if (t > 150 && t % 30 == 0) { pressed->a = 1; in->btn.a = 1; in->btn.a = 1; }
            if (t > 150 && !boss) { debugf("[run] back at the title, run %s\n", tower_active() ? "STILL ACTIVE" : "ended"); phase = 4; }
        }
        return;
    }
#endif
#ifdef HLE_TEST_QUIT
    // Pause mid-fight > Main Menu (4 down: Resume, Restart, Settings, Controls, Main Menu) > A:
    // back at the title, then into the fight again
    {
        static int start = -1, again = 0;
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT) { start = frame; debugf("[quit] fight at %d\n", frame); }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
        if (t == 60) { pressed->start = 1; in->btn.start = 1; }
        if (t == 80 || t == 90 || t == 100 || t == 110) pressed->d_down = 1;
        if (t == 125) { pressed->a = 1; in->btn.a = 1; }
        if (t == 160) debugf("[quit] after Main Menu: boss %s, title menu %s\n", boss ? "still there" : "gone", menu_controller_is_active() ? "open" : "closed");
        if (t > 200 && t < 1400 && t % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        if (!again && t > 200 && boss && boss->mode == BOSS_MODE_COMBAT) { again = 1; debugf("[quit] fight again at %d\n", frame); }
        return;
    }
#endif
#ifdef HLE_TEST_AUDIO
    // Every new sound and track loads and plays: in the fight, play each in turn and
    // check the mixer has a channel going a few frames later
    {
        static int start = -1, step = 0, fails = 0;
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT) start = frame;
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        static const char *const music[] = { "rom:/audio/music/shadowforge_convergence-22k.wav64", "rom:/audio/music/veil_of_eternal_nightfall-22k.wav64",
                                             "rom:/audio/music/whispers_of_the_abyss-22k.wav64" };
        const int nsfx = SCENE1_SFX_COUNT - SCENE1_SFX_DRAGON_ROAR, total = nsfx + 3;
        int t = frame - start;
        // One at a time, 3.5 s apart. A sound counts as silent only if three tries in a row
        // start no channel (another sound ending in the same frame can hide one try)
        static int before, tries;
        if (step < total && t == 20 + step * 130 + tries * 30) {
            before = 0;
            for (int ch = SFX_CH_FIRST; ch <= SFX_CH_LAST; ch++) before += mixer_ch_playing(ch);
            if (step < nsfx) audio_play_scene_sfx_dist(SCENE1_SFX_DRAGON_ROAR + step, 1.0f, 0.0f);
            else audio_play_music(music[step - nsfx], true);
        }
        if (step < total && t == 20 + step * 130 + tries * 30 + 1) {
            bool on = false;
            if (step < nsfx) { int n = 0; for (int ch = SFX_CH_FIRST; ch <= SFX_CH_LAST; ch++) n += mixer_ch_playing(ch); on = n > before; }
            else on = audio_is_music_playing();
            if (!on && ++tries < 3) return;
            if (!on) fails++;
            debugf("[audio] %d %s\n", step, on ? "playing" : "SILENT");
            step++; tries = 0;
            if (step == total) debugf("[audio] self-check: %s (%d silent)\n", fails ? "FAIL" : "ok", fails);
        }
        return;
    }
#endif
#ifdef HLE_TEST_CRUCIFIX
#ifndef HLE_CX_DIST
#define HLE_CX_DIST 120.0f      // the camera's distance and height
#define HLE_CX_Y 20.0f
#endif
#ifndef HLE_CX_LOOK_Y      // where it looks (below the camera: from above)
#define HLE_CX_LOOK_Y HLE_CX_Y
#endif
    // Guardian Unbound's hanging pose (forced on by boss_anim.c): reach phase 2, the player
    // stands off 300 units, the camera between them looking at the boss (front, then side)
    {
        static int phase = 0, t0 = 0;
        Boss *boss = boss_get_instance();
        int t = frame - t0;
        if (phase == 0) {
            if (boss && boss->mode == BOSS_MODE_COMBAT && boss->phaseIndex == 1) { phase = 1; t0 = frame; }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {
            if (t == 60) boss_apply_damage(boss, boss->maxHealth * 2.0f);
            if (t > 60 && boss->phaseIndex == 2 && boss->mode == BOSS_MODE_COMBAT) { phase = 2; t0 = frame; debugf("[cx] fight at %d\n", frame); }
            else if (t > 90 && frame % 20 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else {
            float dx = character.pos[0] - boss->pos[0], dz = character.pos[2] - boss->pos[2], d = sqrtf(dx * dx + dz * dz) + 1e-3f;
            dx /= d; dz /= d;
            if (d < 250.0f) { character.pos[0] = boss->pos[0] + dx * 300.0f; character.pos[2] = boss->pos[2] + dz * 300.0f; }
            static const float VA[3] = { 0.0f, 1.5708f, 0.7854f };     // front, side, three quarters: 1.5 s each
            int v = (t / 90) % 3;
            float ca = cosf(VA[v]), sa = sinf(VA[v]), cx = dx * ca - dz * sa, cz = dz * ca + dx * sa;
            const char *side = v == 0 ? "" : "side";
            cameraLockOnActive = false;
            customCamPos = (T3DVec3){{ boss->pos[0] + cx * HLE_CX_DIST, boss->pos[1] + HLE_CX_Y, boss->pos[2] + cz * HLE_CX_DIST }};
            customCamTarget = (T3DVec3){{ boss->pos[0], boss->pos[1] + HLE_CX_LOOK_Y, boss->pos[2] }};
            camera_mode(CAMERA_CUSTOM);
            if (t % 90 == 0) debugf("[cx] t %d view %d %s\n", t, v, side);
        }
        return;
    }
#endif
#ifdef HLE_TEST_UNBOUND
    // Hazard check: reach Guardian Unbound, circle it for 150 s (every hit is logged with
    // its source, the player is topped up), kill it, then keep walking through where its
    // swords and walls were for 10 s: nothing may land after the death.
    {
        static int phase = 0, t0 = 0;
        Boss *boss = boss_get_instance();
        int t = frame - t0;
        if (phase == 0) {
            if (boss && boss->mode == BOSS_MODE_COMBAT && boss->phaseIndex == 1) { phase = 1; t0 = frame; }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {
            if (t == 60) boss_apply_damage(boss, boss->maxHealth * 2.0f);
            if (t > 60 && boss->phaseIndex == 2 && boss->mode == BOSS_MODE_COMBAT) { phase = 2; t0 = frame; debugf("[ub] fight at %d\n", frame); }
#ifndef HLE_UB_WATCH     // HLE_UB_WATCH: sit through the phase-2 cutscene (the clear time must leave it out)
            else if (t > 90 && frame % 20 == 0) { pressed->a = 1; in->btn.a = 1; }
#endif
        } else if (phase == 2) {
#ifdef HLE_UB_PAUSE     // the pause menu open for 300 frames mid-fight (left out of the clear time too)
            if (t == 600 || t == 900) { pressed->start = 1; in->btn.start = 1; debugf("[ub] start pressed at %d ms %lu\n", frame, (unsigned long)get_ticks_ms()); }
            if (t >= 600 && t < 905) return;
#endif
            in->stick_x = (int)(80 * cosf(t * 0.02f)); in->stick_y = (int)(80 * sinf(t * 0.02f));   // circle about
            if (t % 300 == 0) debugf("[ub] t %d hp %.0f boss state %d\n", t, (double)character_get_health(), boss->state);
            // the kill lands mid-sweep, with the walls up (the sweep's 3rd cycle, about 14 s in)
            static int sweeps, sweepAt, wasActive;
            int active = msa_ground_sweep_is_active();
            if (active && !wasActive && ++sweeps == 3) sweepAt = frame;
            wasActive = active;
            if (sweepAt && frame - sweepAt == 500) { boss_apply_damage(boss, boss->maxHealth * 2.0f); debugf("[ub] killed at %d ms %lu\n", frame, (unsigned long)get_ticks_ms()); phase = 3; t0 = frame; }
        }
        if (phase >= 2) {   // overhead shot of the player so the swords and walls are in frame
            cameraLockOnActive = false;
            float *p = character.pos;
            customCamPos = (T3DVec3){{ p[0] + 1.0f, p[1] + 260.0f, p[2] + 120.0f }};
            customCamTarget = (T3DVec3){{ p[0], p[1], p[2] }};
            camera_mode_smooth(CAMERA_CUSTOM, 0.01f);
        }
        if (phase == 3) {
            in->stick_x = (int)(80 * cosf(t * 0.03f)); in->stick_y = (int)(80 * sinf(t * 0.03f));
            if (t == 300) { debugf("[ub] after-death window over ms %lu\n", (unsigned long)get_ticks_ms()); phase = 4; }
        }
        return;
    }
#endif
#ifdef HLE_TEST_ANIMVIEW
    // A Guardian clip (HLE_TEST_ANIMVIEW = its index; with HLE_TEST_FREEZE_BOSS), posed at
    // 0, 0.4, 0.8 ... s, 60 frames each, from the side
    {
        static int start = -1;
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) start = frame;
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
        T3DAnim *a = ((T3DAnim**)boss->animations)[HLE_TEST_ANIMVIEW];
        if (t == 1) { t3d_anim_attach(a, (T3DSkeleton*)boss->skeleton); t3d_anim_set_playing(a, true); debugf("[anim] clip %d lasts %.2f s\n", HLE_TEST_ANIMVIEW, (double)a->animRef->duration); }
        float at = (t / 60) * 0.4f;
        if (t % 60 == 1) { t3d_anim_set_time(a, at); t3d_anim_update(a, 0.0f); t3d_skeleton_update((T3DSkeleton*)boss->skeleton); }
        if (t % 60 == 30) { T3DVec3 h = {0}; boss_get_head_world_pos(boss, &h); debugf("[anim] at %.1f s head y %.1f\n", (double)at, (double)h.v[1]); }
        cameraLockOnActive = false;
        character.pos[0] = boss->pos[0] + 120.0f; character.pos[2] = boss->pos[2];
        customCamPos = (T3DVec3){{ boss->pos[0] + 60.0f, boss->pos[1] + 30.0f, boss->pos[2] + 70.0f }};
        customCamTarget = (T3DVec3){{ boss->pos[0], boss->pos[1] + 22.0f, boss->pos[2] }};
        camera_mode_smooth(CAMERA_CUSTOM, 0.01f);
        return;
    }
#endif
#if defined(HLE_SHOW_HURTBOX) && defined(HLE_TEST_DUMMY)    // with the bosses: their capsules each frame ([hbb], over the player's floor)
    {
        Boss *bb = boss_get_instance();
        if (bb && bb->mode == BOSS_MODE_COMBAT) {
            const CapsuleCollider *c = &bb->capsuleCollider;
            float g = character.pos[1], *p = bb->pos;
            extern int hle_fk_state(void), hle_sun_state(void), hle_dg_state(void);
            int st = bb->phaseIndex == 3 ? hle_fk_state() : bb->phaseIndex == 4 ? hle_sun_state() : bb->phaseIndex == 5 ? hle_dg_state() : (int)bb->state;
            debugf("[hbb] f%d boss %d st %d at %.0f up | A %.1f %.1f %.1f B %.1f %.1f %.1f (over the floor; x z off its centre, along its facing / side) r %.1f\n", frame, bb->phaseIndex, st, (double)(p[1] - g),
                   (double)(c->localCapA.v[0] * -sinf(bb->rot[1]) + c->localCapA.v[2] * cosf(bb->rot[1])), (double)(c->localCapA.v[0] * cosf(bb->rot[1]) + c->localCapA.v[2] * sinf(bb->rot[1])), (double)(p[1] + c->localCapA.v[1] - g),
                   (double)(c->localCapB.v[0] * -sinf(bb->rot[1]) + c->localCapB.v[2] * cosf(bb->rot[1])), (double)(c->localCapB.v[0] * cosf(bb->rot[1]) + c->localCapB.v[2] * sinf(bb->rot[1])), (double)(p[1] + c->localCapB.v[1] - g), (double)c->radius);
        }
    }
#endif
#if defined(HLE_SHOW_HURTBOX) && !defined(HLE_TEST_DUMMY)
    // The hit capsules drawn (with HLE_TEST_FREEZE_BOSS, HLE_LOG_MOVE_MS for [hb]'s vi): four 12 s rounds, the knight then the
    // Black Swordsman, each seen from the side then the front: idle (wide, then close), a swing at
    // 2 s, a roll at 4 s, knocked down at 6 s. [hb] logs the capsule and the bones over the floor
    {
        extern float deltaTime;
        static int start = -1;
        static float tm = -0.001f;     // below 0: the first round's 0 s step fires
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) { start = frame; debugf("[hb] start f%d ms %lu\n", frame, (unsigned long)get_ticks_ms()); }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        float prev = tm;
        tm += deltaTime;
        int round = (int)(tm / 12.0f);
        float r = tm - round * 12.0f, pr = prev - round * 12.0f;
#define AT(x) (pr < (x) && r >= (x))
        bool front = round & 1;
        static float floorY;
        cameraLockOnActive = false;
        if (round >= 4) return;
        if (AT(0.0f) || AT(3.6f) || AT(5.6f)) {     // back on the spot, facing the boss (-x)
            if (AT(0.0f)) { character_skin_set(round < 2 ? SKIN_KNIGHT : SKIN_BLACK_SWORDSMAN); floorY = character.pos[1]; }
            character.pos[0] = boss->pos[0] + 70.0f; character.pos[2] = boss->pos[2] + 10.0f; character.rot[1] = 1.5708f;
            character.health = character.maxHealth; character.stamina = character.maxStamina;
            character_skin_reset_cloth();
            debugf("[hb] f%d round %d %s skin %d\n", frame, round, front ? "front" : "side", character_skin_get());
        }
        if (AT(2.0f)) { pressed->b = 1; in->btn.b = 1; debugf("[hb] f%d swing\n", frame); }
        if (r >= 4.0f && r < 4.2f) { in->stick_y = 80; if (AT(4.0f)) { pressed->a = 1; in->btn.a = 1; debugf("[hb] f%d roll\n", frame); } }
        if (AT(6.0f)) { character_apply_damage(25.0f); debugf("[hb] f%d knocked down\n", frame); }
        {   // the capsule and the drawn body's bones, over the floor
            static const char *NB[] = { "Head", "Hand-Left", "Hand-Right", "Foot-Left", "Foot-Right", "Waist" };
            static int bone[6] = { -2 };
            T3DSkeleton *sk = (T3DSkeleton*)character.skeleton;
            if (bone[0] == -2) for (int i = 0; i < 6; i++) bone[i] = t3d_skeleton_find_bone(sk, NB[i]);
            if (frame % 3 == 0 && sk && character.modelMat) {
                const CapsuleCollider *c = &character.capsuleCollider;
                char buf[200]; int n = 0;
                for (int i = 0; i < 6; i++) {
                    if (bone[i] < 0) continue;
                    float m[3], w[3];
                    mat4fp_mul_point_f32_row3_colbasis(&sk->boneMatricesFP[bone[i]], (float[3]){ 0, 0, 0 }, m);
                    mat4fp_mul_point_f32_row3_colbasis((const T3DMat4FP*)character.modelMat, m, w);
                    float dx = w[0] - character.pos[0], dz = w[2] - character.pos[2];
                    n += snprintf(buf + n, sizeof buf - n, " %s y%.1f out%.1f", NB[i], (double)(w[1] - floorY), (double)sqrtf(dx * dx + dz * dz));
                }
                extern uint32_t dragon_vi_frame(void);     // HLE_LOG_MOVE_MS: Mupen's frame (shot) number
                debugf("[hb] vi %lu f%d t %.2f st %d skin %d pos y%.1f | capsule y %.1f..%.1f r %.1f |%s\n", (unsigned long)dragon_vi_frame(), frame, (double)r, character_get_state(), character_skin_get(),
                       (double)(character.pos[1] - floorY), (double)(character.pos[1] + c->localCapA.v[1] - c->radius - floorY), (double)(character.pos[1] + c->localCapB.v[1] + c->radius - floorY), (double)c->radius, buf);
            }
        }
        // the camera: wide on both (side rounds, the first second), else close on the knight
        float *p = character.pos;
        if (!front && r < 1.0f) {
            customCamPos = (T3DVec3){{ p[0] - 35.0f, floorY + 30.0f, p[2] + 160.0f }};
            customCamTarget = (T3DVec3){{ p[0] - 35.0f, floorY + 25.0f, p[2] }};
        } else if (!front) {
            customCamPos = (T3DVec3){{ p[0], floorY + 11.0f, p[2] + 42.0f }};
            customCamTarget = (T3DVec3){{ p[0], floorY + 9.0f, p[2] }};
        } else {
            customCamPos = (T3DVec3){{ p[0] - 42.0f, floorY + 11.0f, p[2] }};
            customCamTarget = (T3DVec3){{ p[0], floorY + 9.0f, p[2] }};
        }
        camera_mode(CAMERA_CUSTOM);
#undef AT
        return;
    }
#endif
#ifdef HLE_TEST_GETUP
    // The knight knocked down twice (with HLE_TEST_FREEZE_BOSS), seen from the side: the
    // first time left to get up on its own at 5 s, the second broken away by moving at 2 s
    {
        extern float deltaTime;
        static int start = -1;
        static float tm, kdAt = -1.0f;
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) start = frame;
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        float prev = tm;
        tm += deltaTime;
        cameraLockOnActive = false;
        if (prev < 0.5f && tm >= 0.5f) { character.pos[0] = boss->pos[0] + 150.0f; character.pos[2] = boss->pos[2]; }
        if ((prev < 1.0f && tm >= 1.0f) || (prev < 8.0f && tm >= 8.0f)) { character_apply_damage(25.0f); kdAt = tm; debugf("[getup] f%d knocked down\n", frame); }
        if (tm >= 8.0f + 2.0f && tm < 8.0f + 3.0f) in->stick_y = 70;     // the breakaway: move
        if ((int)(prev * 5) != (int)(tm * 5)) debugf("[getup] f%d t %.1f since %.1f state %d\n", frame, (double)tm, (double)(tm - kdAt), character_get_state());
        customCamPos = (T3DVec3){{ character.pos[0] + 50.0f, character.pos[1] + 20.0f, character.pos[2] + 50.0f }};
        customCamTarget = (T3DVec3){{ character.pos[0], character.pos[1] + 8.0f, character.pos[2] }};
        camera_mode(CAMERA_CUSTOM);
        return;
    }
#endif
#ifdef HLE_TEST_OOB
#ifndef HLE_OOB_DIST
#define HLE_OOB_DIST 68.0f
#endif
    // The knight knocked down (a 30 hit: the full 100 unit shove) with his back to each wall of
    // the hall, the stairs' slanted walls too, every 1.5 s (with HLE_TEST_FREEZE_BOSS), his
    // middle HLE_OOB_DIST from the wall's (68: he meets it at the shove's fastest);
    // HLE_OOB_SLOW_MS=<ms> stalls every frame that long (Delta's slow frames). Logs where he
    // ends up and whether he is out of the hall (outside the walls' middle lines)
    {
        extern float deltaTime;
        static const float W[][4] = {   // a point on a wall's middle line, its outward normal
            {  600.0f,    0.0f,  1.0f,  0.0f },     // back wall, between the pillars
            {  382.5f, -507.5f,  0.9191f, -0.3939f }, // stairs' slanted walls
            {  382.5f,  507.5f,  0.9191f,  0.3939f },
            {  360.0f, -560.0f,  0.9191f, -0.3939f },
            {  510.0f, -415.0f,  0.0f, -1.0f },     // the narrow end's sides
            {  510.0f,  415.0f,  0.0f,  1.0f },
            {    0.0f,  595.0f,  0.0f,  1.0f },     // long sides, front
            {    0.0f, -595.0f,  0.0f, -1.0f },
            { -430.0f,    0.0f, -1.0f,  0.0f },
        };
        static const float P[][2] = { {-430,-595}, {345,-595}, {420,-417}, {600,-417}, {600,417}, {420,417}, {345,595}, {-430,595} };
        static int start = -1, trial = -1, outs;
        static float tm, maxStep, px, pz, maxDt;
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) { start = frame; tm = 1.5f; }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
#ifdef HLE_OOB_SLOW_MS
        wait_ms(HLE_OOB_SLOW_MS);
#endif
        cameraLockOnActive = false;
        int nw = (int)(sizeof W / sizeof W[0]);
        float x = character.pos[0], z = character.pos[2];
        bool in_hall = false;   // crossing test against the walls' middle lines
        for (int i = 0, j = 7; i < 8; j = i++)
            if ((P[i][1] > z) != (P[j][1] > z) && x < (P[j][0] - P[i][0]) * (z - P[i][1]) / (P[j][1] - P[i][1]) + P[i][0]) in_hall = !in_hall;
        if (trial >= 0 && trial < nw) {
            float st = sqrtf((x - px) * (x - px) + (z - pz) * (z - pz));
            if (st > maxStep) maxStep = st;
            if (deltaTime > maxDt) maxDt = deltaTime;
        }
        px = x; pz = z;
        tm += deltaTime;
        if (tm < 1.5f) return;
        if (trial >= 0 && trial < nw) {
            const float *w = W[trial];
            float out = (x - w[0]) * w[2] + (z - w[1]) * w[3];   // past the wall's middle line (+ = through)
            if (!in_hall) outs++;
            debugf("[oob] wall %d end (%.1f, %.1f), %.1f from the wall's middle (+ = through), %s, biggest step %.1f, dt up to %.3f\n",
                   trial, (double)x, (double)z, (double)out, in_hall ? "in the hall" : "OUT OF THE HALL", (double)maxStep, (double)maxDt);
        }
        trial++;
        if (trial == nw) { debugf("[oob] done: %d of %d knockbacks ended out of the hall\n", outs, nw); }
        if (trial >= nw) { trial = nw; return; }
        const float *w = W[trial];
        character_set_state(CHAR_STATE_NORMAL);
        character.health = character.maxHealth;
        character.pos[0] = w[0] - w[2] * HLE_OOB_DIST; character.pos[2] = w[1] - w[3] * HLE_OOB_DIST;
        character.rot[1] = atan2f(w[2], -w[3]);         // facing in: the shove is straight back, through the wall
        character_apply_damage(30.0f);
        px = character.pos[0]; pz = character.pos[2]; maxStep = 0.0f; maxDt = 0.0f; tm = 0.0f;
        return;
    }
#endif
#ifdef HLE_TEST_COVER
    // Cover art poses (with HLE_TEST_FREEZE_BOSS): the knight facing the Guardian;
    // camera shots switch every 60 frames (see the shot list in the log)
    {
        static int start = -1;
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) start = frame;
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
        cameraLockOnActive = false;
        character.pos[0] = boss->pos[0] - (t < 120 ? 600.0f : 70.0f); character.pos[2] = boss->pos[2] + 6.0f;   // out of the Guardian's shot
        character.rot[1] = T3D_PI * 1.5f;
        static const float shots[][6] = {
            { -80, 6, 50,    -5, 34, 0 },      // 0 (t < 120): the Guardian, from below
            { -40, 14, 26,   -70, 14, 6 },     // 1 (t >= 120): the knight, close, mid-swing
        };
        int k = t < 120 ? 0 : 1;
        customCamPos = (T3DVec3){{ shots[k][0], shots[k][1], shots[k][2] }};
        customCamTarget = (T3DVec3){{ shots[k][3], shots[k][4], shots[k][5] }};
        camera_mode(CAMERA_CUSTOM);
        if (t % 60 == 0) debugf("[cover] shot %d at frame %d\n", k, frame);
        if (t >= 125 && t < 129) { pressed->b = 1; in->btn.b = 1; }     // a swing, for an action pose
        return;
    }
#endif
#ifdef HLE_TEST_CYCLE
    // Load/unload stress: title -> a fight -> back to the title, over and over, each cycle a
    // different way in and out (the Fallen Warrior's intro watched, skipped early, skipped as it
    // drops, mid-landing; Restart; other bosses first; death; a kill and its outro). Logs free
    // heap at the title every cycle (a leak shows as it falling) and a heartbeat (a freeze: it stops)
    {
        extern bool scene_is_cutscene_active(void);
        enum { V_WATCH, V_SKIP_EARLY, V_SKIP_DROP, V_SKIP_LAND, V_RESTART, V_SUN, V_DRAGON, V_STORY, V_DIE, V_KILL, V_N };
        static const char *const VN[V_N] = { "fw-watch", "fw-skip-early", "fw-skip-drop", "fw-skip-land", "fw-restart", "sun", "dragon", "story", "fw-die", "fw-kill" };
        static int phase = 0, t0 = 0, step = 0, cyc = 0, dropF = -1, restarted = 0, heap0 = -1, lastMs = 0, worstMs = 0;
        Boss *boss = boss_get_instance();
        int t = frame - t0, v = cyc % V_N;
        heap_stats_t h; sys_get_heap_stats(&h);
        int ms = (int)get_ticks_ms(), freeKB = (h.total - h.used) / 1024;
        if (lastMs && ms - lastMs > worstMs) worstMs = ms - lastMs;
        lastMs = ms;
        if (frame % 120 == 0) debugf("[cyc] hb f%d ms %d cyc %d %s phase %d boss %d mode %d cut %d free %d KB worst frame %d ms\n", frame, ms, cyc, VN[v], phase,
                                     boss ? boss->phaseIndex : -1, boss ? boss->mode : -1, scene_is_cutscene_active(), freeKB, worstMs);
        #define TAP_A() do { pressed->a = 1; in->btn.a = 1; } while (0)
        if (phase == 0) {               // the title menu (A opens it)
            if (menu_controller_is_active() && !boss) {
                if (t > 30) {
                    if (heap0 < 0) heap0 = freeKB;
                    debugf("[cyc] title cyc %d next %s free %d KB (first %d, %+d) worst frame %d ms\n", cyc, VN[v], freeKB, heap0, freeKB - heap0, worstMs);
                    phase = 1; t0 = frame; step = 0; dropF = -1; restarted = 0; worstMs = 0;
                }
            } else { t0 = frame; if (!boss && frame > 200 && frame % 30 == 0) TAP_A(); }
        } else if (phase == 1) {        // pick the fight
            const char *seq = v == V_SUN ? "DADA" : v == V_DRAGON ? "DADDA" : v == V_STORY ? "A" : "DAA";
            if (t >= 20 && step < (int)strlen(seq)) { if (seq[step] == 'D') pressed->d_down = 1; else TAP_A(); step++; t0 = frame; }
            else if (step == (int)strlen(seq) && boss && scene_is_cutscene_active()) { debugf("[cyc] intro cyc %d %s f%d free %d KB\n", cyc, VN[v], frame, freeKB); phase = 2; t0 = frame; }
            else if (t > 1500) { debugf("[cyc] STUCK picking cyc %d\n", cyc); phase = 0; }
        } else if (phase == 2) {        // the intro, left or skipped as the variant says
            if (dropF < 0 && boss && boss->visible && boss->phaseIndex >= 3) { dropF = frame; debugf("[cyc] boss drops in at intro +%d free %d KB\n", t, freeKB); }
            bool skip = false;
            if (restarted) skip = false;
            else if (v == V_SKIP_EARLY) skip = t == 30 || t == 40;
            else if (v == V_SKIP_DROP) skip = dropF >= 0 && (frame == dropF || frame == dropF + 8);
            else if (v == V_SKIP_LAND) skip = dropF >= 0 && (frame == dropF + 60 || frame == dropF + 68);
            else if (v == V_SUN || v == V_DRAGON || v == V_STORY) skip = t > 60 && t % 20 == 0;
            if (skip) TAP_A();
            if (boss && boss->mode == BOSS_MODE_COMBAT && !scene_is_cutscene_active() && character_get_state() == CHAR_STATE_NORMAL) {
                debugf("[cyc] fight cyc %d boss %d after %d frames free %d KB worst frame %d ms\n", cyc, boss->phaseIndex, t, freeKB, worstMs);
                phase = 3; t0 = frame;
            } else if (t > 4000) { debugf("[cyc] STUCK in intro cyc %d\n", cyc); phase = 3; t0 = frame; }
        } else if (phase == 3) {        // fight a moment, then out
            if (character.health < character.maxHealth * 0.5f && v != V_DIE) character.health = character.maxHealth;
            if (t == 180) {
                if (v == V_DIE) character_apply_damage(9999.0f);
                else if (v == V_KILL) boss_apply_damage(boss, boss->maxHealth * 2.0f);
                else pressed->start = 1;
            }
            int downs = v == V_RESTART && !restarted ? 1 : 4;     // Resume, Restart, Settings, Controls, Main Menu
            if (v != V_DIE && v != V_KILL) {
                if (t > 180 && t <= 180 + downs * 10 && (t - 180) % 10 == 0) pressed->d_down = 1;
                if (t == 200 + downs * 10) {
                    TAP_A();
                    if (downs == 1) { restarted = 1; phase = 2; t0 = frame; dropF = -1; debugf("[cyc] restart cyc %d\n", cyc); return; }
                }
            } else if (t > 400 && t % 30 == 0 && boss) TAP_A();     // the death screen / the outro
            if (t > 200 && !boss) {
                cyc++; phase = 0; t0 = frame;
#ifdef HLE_TEST_LEAK
                { extern int hle_leak_cyc; extern void hle_leak_dump(int, int), hle_leak_by_caller(const char *); hle_leak_cyc = cyc; if (cyc == 7) hle_leak_dump(2, 6);
                  if (cyc == 8 || cyc == 12) hle_leak_by_caller(cyc == 8 ? "title8" : "title12"); }
#endif
            }
            else if (t > 3000) { debugf("[cyc] STUCK leaving cyc %d\n", cyc); }
        }
        #undef TAP_A
        return;
    }
#endif
#ifdef HLE_TEST_DUMMY
    // Threat measurement: a passive, invincible dummy at the start position; logs the
    // damage each boss lands over 120 s. HLE_DUMMY_BOSS: 1 Guardian, 2 Unbound,
    // 3 Fallen Warrior, 4 Shackled Sun, 5 Shadowborne.
    {
        extern float deltaTime;
        static int phase = 0, step = 0, t0 = 0;
        static float tm;
        Boss *boss = boss_get_instance();
        int t = frame - t0;
        if (phase == 0) {       // reach the title menu
            if (menu_controller_is_active()) { phase = 1; t0 = frame; }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {    // pick the fight
            const char *seq = HLE_DUMMY_BOSS == 3 ? "DAA" : HLE_DUMMY_BOSS == 4 ? "DADA" : HLE_DUMMY_BOSS == 5 ? "DADDA" : "A";
            int n = (int)strlen(seq);
            if (t >= 20 && step < n) { if (seq[step] == 'D') pressed->d_down = 1; else { pressed->a = 1; in->btn.a = 1; } step++; t0 = frame; }
            if (step == n) { phase = 2; t0 = frame; }
        } else if (phase == 2) {    // through the intro (A skips cutscenes)
            int want = HLE_DUMMY_BOSS <= 2 ? 1 : HLE_DUMMY_BOSS;
            if (boss && boss->mode == BOSS_MODE_COMBAT && boss->phaseIndex == want && character_get_state() == CHAR_STATE_NORMAL) {
                if (HLE_DUMMY_BOSS == 2) { boss_apply_damage(boss, boss->maxHealth * 2.0f); phase = 3; }
                else {
#ifdef HLE_DUMMY_HALF   // measure the second half: past the Shackled Sun's break (and its cutscene-free 2 s)
                    boss_apply_damage(boss, boss->maxHealth * 0.55f);
#endif
#ifdef HLE_DUMMY_SKIN   // the fight in this skin (e.g. a cloak, to time its cloth)
                    character_skin_set(HLE_DUMMY_SKIN);
#endif
                    phase = 4; tm = 0.0f; hle_dummy_damage = 0.0f; hle_dummy_hits = 0; debugf("[dummy] start\n"); }
            } else if (t > 60 && frame % 20 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 3) {    // Unbound: skip the transformation
            if (boss->phaseIndex == 2 && boss->mode == BOSS_MODE_COMBAT) {
#ifdef HLE_DUMMY_SKIN
                character_skin_set(HLE_DUMMY_SKIN);
#endif
                phase = 4; tm = 0.0f; hle_dummy_damage = 0.0f; hle_dummy_hits = 0; debugf("[dummy] start\n"); }
            else if (frame % 20 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 4) {
#ifdef HLE_HUMAN_BOT     // test aid: the human-like bot (hle/human_bot.c) in the dummy's fight: windows of HLE_SWEEP_S s ([hbfight] each), with
                         // HLE_DUMMY_BUILD each window that build (section 14's, tower_test_build), the boss's health held in its half and the
                         // player's at 42%; without HLE_DUMMY_REAL the blows don't hurt (the bench: what lands is counted, "landed")
            {
#ifndef HLE_SWEEP_S
#define HLE_SWEEP_S 240.0f
#endif
                extern void hb_fight_begin(Boss *), hb_fight_end(const char *, Boss *), hb_fight(joypad_inputs_t *, joypad_buttons_t *, Boss *), hb_init(void), hb_dump_table(void);
                extern bool hb_fight_on(void); extern int hb_skill_pref;
                static int w = -1; static float w0, prevHp, dealt, wasStag, dealt0; static int staggers; extern float hb_dealt;
                hb_init();
                if (!boss) return;
                if (w < 0 || tm - w0 >= HLE_SWEEP_S) {
                    if (w >= 0) { debugf("[hbwin] window %d build %d: %.0f s dealt %.1f (%.1f a minute) staggers %d | boss max %.0f\n", w,
#ifdef HLE_DUMMY_BUILD
                                         (int)((const int[]){ HLE_DUMMY_BUILD })[w % (int)(sizeof (const int[]){ HLE_DUMMY_BUILD } / sizeof(int))],
#else
                                         -1,
#endif
                                         (double)(tm - w0), (double)(hb_dealt - dealt0), (double)((hb_dealt - dealt0) * 60.0f / (tm - w0)), staggers, (double)boss->maxHealth);
                                  hb_fight_end("window", boss); hb_dump_table(); }
                    w++; w0 = tm; dealt = 0.0f; staggers = 0; dealt0 = hb_dealt;
#ifdef HLE_DUMMY_BUILD
                    { extern int tower_test_build(int); static const int L[] = { HLE_DUMMY_BUILD };
                      int sk = tower_test_build(L[w % (int)(sizeof L / sizeof L[0])]); hb_skill_pref = sk - 1; character_cut_skill_cooldowns(1e6f); }
#endif
#ifdef HLE_ELITE_SWEEP
                    { extern int hle_elite_force; static const int A[] = { HLE_ELITE_SWEEP }; int a = A[w % (int)(sizeof A / sizeof A[0])];
                      hle_elite_force = a ? 1 << (a - 1) : 0; debugf("[hbwin] window %d: affix %d\n", w, a); }
#endif
#ifdef HLE_ELITE_MASKS      // (with HLE_ELITE_FORCE=0) each window these affixes at once (a bit each: two-affix Elites)
                    { extern int hle_elite_force; static const int A[] = { HLE_ELITE_MASKS }; hle_elite_force = A[w % (int)(sizeof A / sizeof A[0])];
                      debugf("[hbwin] window %d: affixes %02x\n", w, hle_elite_force); }
#endif
                    prevHp = boss->health;
                    hb_fight_begin(boss);
                }
#ifdef HLE_DUMMY_BUILD
#ifdef HLE_DUMMY_HALF
                const float lo = 0.1f, hi = 0.45f;
#else
                const float lo = 0.55f, hi = 1.0f;
#endif
                dealt += prevHp - boss->health;
                if (boss->health < boss->maxHealth * lo) boss->health = boss->maxHealth * hi;
                if (boss->health > boss->maxHealth * hi) boss->health = boss->maxHealth * hi;
                prevHp = boss->health;
                character.health = character.maxHealth * 0.42f;
#else
                dealt += prevHp - boss->health;
#ifdef HB_HOLD          // (the fight goes on: its health kept over 30%)
                if (boss->health < boss->maxHealth * 0.3f) boss->health = boss->maxHealth * 0.6f;
#endif
                prevHp = boss->health;
#endif
                if (boss->staggerT > 0.0f && wasStag <= 0.0f) staggers++;
                wasStag = boss->staggerT;
#ifdef HLE_DUMMY_REAL
                if (character_get_state() == CHAR_STATE_DEAD || boss->health <= 0.0f || (HLE_DUMMY_BOSS == 1 && boss->state == BOSS_STATE_DEAD)) {
                    static bool once; if (!once) { once = true; hb_fight_end(character_get_state() == CHAR_STATE_DEAD ? "died" : "won", boss); hb_dump_table(); debugf("[hbrun] all 1 runs done\n"); }
                    return;
                }
#endif
                if (boss->mode == BOSS_MODE_COMBAT) hb_fight(in, pressed, boss);
                tm += deltaTime;
                return;
            }
#endif
#ifdef HLE_DUMMY_CAST    // test aid: a skill (1-3; 4 five in turn, [palcast]) cast every HLE_DUMMY_CAST_EVERY frames, locked on, its cooldown zeroed, in a Tower
                         // run begun here; with HLE_SKILL_LV=<skill>,<lv>,<form> the casts alternate Lv 1 and that level: [castft] the
                         // frame time through each (mean, worst), the most effects alive, any dropped ([fxfull])
            {
#ifndef HLE_DUMMY_CAST_EVERY
#define HLE_DUMMY_CAST_EVERY 300
#endif
                extern void tower_begin_run(uint32_t); extern bool tower_active(void); extern void tower_test_skill_lv(int, int, int);
                extern int skill_fx_live(void); extern void character_cut_skill_cooldowns(float);
                static int k, n; static uint32_t prev, sum, worst, cnt; static int live;
                if (!tower_active()) {
                    tower_begin_run(1);
#ifdef HLE_STRIP_FULL       // (the slot strip full: its cost in [castft], against HLE_NO_STRIP)
                    { extern void tower_test_fill_slots(void); tower_test_fill_slots(); }
#endif
                }
                int c = k % HLE_DUMMY_CAST_EVERY;
#if HLE_DUMMY_CAST == 4     // 4: each in turn, Moon Blade Dance, Full Moon Dance, Halfmoon Slash, Crescent Cleave, Crescent Storm (palette shots)
                static const int CYC[5][3] = { { 1, 1, 0 }, { 1, 5, 1 }, { 2, 1, 0 }, { 3, 1, 0 }, { 3, 5, 1 } };
                int sk = CYC[n % 5][0];
                if (c == 0) {
                    tower_test_skill_lv(sk - 1, CYC[n % 5][1], CYC[n % 5][2]);
                    debugf("[palcast] cast %d skill %d lv %d at VI frame ~%lu\n", n, sk, CYC[n % 5][1], (unsigned long)(get_ticks_ms() * 60 / 1000));
#elif defined(HLE_SKILL_LV)
                const int sk = HLE_DUMMY_CAST;
                if (c == 0) {
                    static const int L[3] = { HLE_SKILL_LV };
                    tower_test_skill_lv(HLE_DUMMY_CAST - 1, n % 2 ? L[1] : 1, n % 2 ? L[2] : 0);
#else
                const int sk = HLE_DUMMY_CAST;
                if (c == 0) {
                    tower_test_skill_lv(HLE_DUMMY_CAST - 1, 1, 0);
#endif
                    character_cut_skill_cooldowns(1e6f); character.stamina = character.maxStamina;
                }
#if HLE_DUMMY_CAST == 4 && defined(HLE_PALCAM)   // HLE_PALCAM=<back>: the player set 60 from the boss each cast, a still camera
                // that far behind him and off to his right, above, looking past him at the boss (its effects against the boss)
                if (boss) {
                    static float bx, bz, gy;
                    float dx = character.pos[0] - boss->pos[0], dz = character.pos[2] - boss->pos[2], d = sqrtf(dx * dx + dz * dz) + 1e-3f;
                    if (c == 0) { boss->health = boss->maxHealth; character.pos[0] = boss->pos[0] + dx / d * 60.0f; character.pos[2] = boss->pos[2] + dz / d * 60.0f; bx = dx / d; bz = dz / d; gy = character.pos[1]; }
                    float px = boss->pos[0] + bx * 60.0f, pz = boss->pos[2] + bz * 60.0f;
                    customCamPos = (T3DVec3){{ px + bx * HLE_PALCAM - bz * HLE_PALCAM * 0.45f, gy + 40.0f, pz + bz * HLE_PALCAM + bx * HLE_PALCAM * 0.45f }};
                    customCamTarget = (T3DVec3){{ px - bx * 28.0f, gy + 40.0f, pz - bz * 28.0f }};
                    camera_mode_smooth(CAMERA_CUSTOM, 0.0f);
                }
#endif
                if (k % 60 == 1) pressed->z = 1;
                in->btn.z = 1;
                if (c == 5) { if (sk == 1) pressed->d_up = 1; else if (sk == 2) pressed->d_right = 1; else pressed->d_down = 1; }
                uint32_t now = get_ticks_us();
                if (c > 5 && c <= HLE_DUMMY_CAST_EVERY - 10) { uint32_t d = now - prev; sum += d; cnt++; if (d > worst) worst = d; int l = skill_fx_live(); if (l > live) live = l; }
                prev = now;
                if (c == HLE_DUMMY_CAST_EVERY - 9 && cnt) {
                    extern int tower_skill_level(int), tower_skill_form(int);
                    debugf("[castft] cast %d (Lv %d form %d): mean %u us, worst %u us over %u frames, at most %d effects alive\n", n, tower_skill_level(sk - 1),
                           tower_skill_form(sk - 1), (unsigned)(sum / cnt), (unsigned)worst, (unsigned)cnt, live);
                    sum = worst = cnt = 0; live = 0; n++;
                }
                k++;
            }
#endif
#ifdef HLE_DUMMY_BUILD   // test aid: the builds compared (section 14) on this boss: a window of HLE_SWEEP_S s each of the list HLE_DUMMY_BUILD
                         // (tower_test_build), round and round. The bot stays close as the passive dummy, swings in the boss's openings and
                         // casts the build's skill there when it's ready; 30% of the blows land (character.c), its health held at 40% (Berserker
                         // on) and what the build heals counted; the boss's health kept in its half. [build] each window's damage a minute,
                         // staggers, casts, casts broken, healing a minute
            {
#ifndef HLE_SWEEP_S
#define HLE_SWEEP_S 240.0f
#endif
                extern int tower_test_build(int);
                extern bool hle_open_now; extern int hle_build_breaks;
                static const int L[] = { HLE_DUMMY_BUILD };
                static int w = -1, skill, casts, staggers, b0, k; static float w0, dealt, heal, prevHp, wasStag, took0;
                if (w < 0 || tm - w0 >= HLE_SWEEP_S) {
                    if (w >= 0) debugf("[build] boss %d build %d window %d: %.0f s dealt %.1f (%.1f a minute) | staggers %d, casts %d, broken %d | healed %.1f (%.1f a minute) | boss max %.0f | took %.1f\n",
                                       HLE_DUMMY_BOSS, L[w % (int)(sizeof L / sizeof L[0])], w, (double)(tm - w0), (double)dealt, (double)(dealt * 60.0f / (tm - w0)), staggers, casts,
                                       hle_build_breaks - b0, (double)heal, (double)(heal * 60.0f / (tm - w0)), (double)boss->maxHealth, (double)(hle_dummy_damage - took0));
                    w++; w0 = tm; dealt = heal = 0.0f; casts = staggers = 0; b0 = hle_build_breaks; took0 = hle_dummy_damage;
                    skill = tower_test_build(L[w % (int)(sizeof L / sizeof L[0])]);
#ifdef HLE_ELITE_SWEEP      // (and each window this affix: a list as long as the builds'; with HLE_ELITE_FORCE=0)
                    { extern int hle_elite_force; static const int A[] = { HLE_ELITE_SWEEP }; int a = A[w % (int)(sizeof A / sizeof A[0])];
                      hle_elite_force = a ? 1 << (a - 1) : 0; debugf("[build] window %d: affix %d\n", w, a); }
#endif
                    character_cut_skill_cooldowns(1e6f);
                    prevHp = boss->health;
                }
#ifdef HLE_DUMMY_HALF
                const float lo = 0.1f, hi = 0.45f;
#else
                const float lo = 0.55f, hi = 1.0f;
#endif
                dealt += prevHp - boss->health;     // (net: a Vampiric Elite's heals come off it)
                if (boss->health < boss->maxHealth * lo) boss->health = boss->maxHealth * hi;
                if (boss->health > boss->maxHealth * hi) boss->health = boss->maxHealth * hi;
                prevHp = boss->health;
                if (boss->staggerT > 0.0f && wasStag <= 0.0f) staggers++;
                wasStag = boss->staggerT;
                float keep = character.maxHealth * 0.4f;
                if (character.health > keep) heal += character.health - keep;
                character.health = keep; character.stamina = character.maxStamina;
                k++;
                if (k % 60 == 1) pressed->z = 1;
                in->btn.z = 1;
                float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2], d = sqrtf(dx * dx + dz * dz);
                if (d > 34.0f && character_get_state() != CHAR_STATE_SKILL) in->stick_y = 70;
                if (hle_open_now && d < 45.0f && character_get_state() == CHAR_STATE_NORMAL && skill && character_get_skill_cooldown(skill - 1) <= 0.0f) {
                    if (skill == 1) pressed->d_up = 1; else if (skill == 2) pressed->d_right = 1; else pressed->d_down = 1;
                    casts++;
                } else if (hle_open_now && d < 40.0f && k % 20 == 0) { pressed->b = 1; in->btn.b = 1; }
                tm += deltaTime;
                return;
            }
#endif
#ifdef HLE_DUMMY_REAL    // the bot playtest: it fights (HLE_DUMMY_STRIKE, HLE_DUMMY_ROLL), drinks below 40% and says how it went
            {
                static int potions0 = -1, done;
                if (potions0 < 0) potions0 = character.healthPotions;
                if (!done && character.health < character.maxHealth * 0.4f && character_get_state() == CHAR_STATE_NORMAL) {
                    extern bool character_try_use_health_potion(void);
                    if (character_try_use_health_potion()) debugf("[bot] t %.0f s: potion at %.0f health\n", (double)tm, (double)character.health);
                }
                bool won = boss && boss->state == BOSS_STATE_DEAD && (HLE_DUMMY_BOSS != 1 || boss->phaseIndex == 2);
                bool lost = character_get_state() == CHAR_STATE_DEAD;
                if (!done && (won || lost || tm > 300.0f)) {
                    done = 1;
                    debugf("[bot] boss %d %s at %.0f s: took %.0f in %d hits, %d potions used, %.0f health left, boss at %.0f/%.0f\n", HLE_DUMMY_BOSS,
                           won ? "beaten" : lost ? "player died" : "timed out", (double)tm, (double)hle_dummy_damage, hle_dummy_hits,
                           potions0 - character.healthPotions, (double)character.health, (double)(boss ? boss->health : 0), (double)(boss ? boss->maxHealth : 0));
                }
                if (done) return;
                if (HLE_DUMMY_BOSS == 1 && boss && boss->phaseIndex == 1 && boss->state == BOSS_STATE_DEAD) {   // the story: on to Unbound
                    if (frame % 20 == 0) { pressed->a = 1; in->btn.a = 1; }
                    tm += deltaTime;
                    return;
                }
                // Strike in the boss's openings, keep away and roll out while it attacks
                if (boss && boss->mode == BOSS_MODE_COMBAT) {
                    extern bool hle_open_now;
                    static int k, lastRoll;
                    k++;
                    if (k % 60 == 1) pressed->z = 1;       // (re)lock on
                    in->btn.z = 1;
                    float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2], d = sqrtf(dx * dx + dz * dz);
                    if (hle_open_now) {
                        if (d > 38.0f) in->stick_y = 70;                       // in to sword reach
                        else if (k % 20 == 0) { pressed->b = 1; in->btn.b = 1; }
                    } else {
                        in->stick_y = -70;                                     // back off
                        if (d < 70.0f && k - lastRoll > 45 && character.stamina > 30.0f) { pressed->a = 1; in->btn.a = 1; lastRoll = k; }
                    }
                }
                tm += deltaTime;
                return;
            }
#endif
#ifdef HLE_DUMMY_LOCK_KILL  // locked on, circle the boss, kill it at 4 s: the camera after the death
            {
                if (tm < 4.0f) {
                    if ((int)(tm * 60.0f) == 2) pressed->z = 1;
                    in->btn.z = 1; in->stick_x = 70;
                } else if (boss->state != BOSS_STATE_DEAD) boss_apply_damage(boss, boss->maxHealth * 2.0f);
                if (tm > 3.5f && tm < 7.0f && frame % 3 == 0)
                    debugf("[lockkill] t %.2f lock %d blend %.2f angX %.2f cam %.0f %.0f %.0f tgt %.0f %.0f %.0f\n", (double)tm, cameraLockOnActive,
                           (double)cameraLockBlend, (double)cameraAngleX, (double)characterCamPos.v[0], (double)characterCamPos.v[1], (double)characterCamPos.v[2],
                           (double)characterCamTarget.v[0], (double)characterCamTarget.v[1], (double)characterCamTarget.v[2]);
                tm += deltaTime;
                return;
            }
#endif
#ifdef HLE_DUMMY_CAM      // the camera on the boss, from its front a little below (a pose check)
#ifndef HLE_DUMMY_CAM_YAW // turned round the boss (1.57: from its side)
#define HLE_DUMMY_CAM_YAW 0.0f
#endif
#ifndef HLE_DUMMY_CAM_DIST
#define HLE_DUMMY_CAM_DIST 120.0f
#endif
            if (boss) {
                cameraLockOnActive = false;
                float a = boss->rot[1] + HLE_DUMMY_CAM_YAW;
#ifdef HLE_DUMMY_CAM_ALT    // the front and the side by turns, this many frames each (side: +1.57), both views of one moment
                if (frame / HLE_DUMMY_CAM_ALT & 1) a += 1.5708f;
#endif
#ifndef HLE_DUMMY_CAM_H     // the camera's height and where it looks, over the boss's origin
#define HLE_DUMMY_CAM_H 30.0f
#define HLE_DUMMY_CAM_LOOK 35.0f
#endif
                customCamPos = (T3DVec3){{ boss->pos[0] - sinf(a) * HLE_DUMMY_CAM_DIST, boss->pos[1] + HLE_DUMMY_CAM_H, boss->pos[2] + cosf(a) * HLE_DUMMY_CAM_DIST }};
                customCamTarget = (T3DVec3){{ boss->pos[0], boss->pos[1] + HLE_DUMMY_CAM_LOOK, boss->pos[2] }};
                camera_mode_smooth(CAMERA_CUSTOM, 0.01f);     // (10 ms: a cut)
                if (boss->state == BOSS_STATE_AERIAL_SWORD_BARRAGE && frame % 20 == 0) debugf("[cam] f%d aerial, y %.0f\n", frame, (double)boss->pos[1]);
            }
#endif
#ifdef HLE_DUMMY_SIDECAM  // the camera off to the side of the boss and the player, both in view (attack videos)
            if (boss) {
                cameraLockOnActive = false;
                float mx = (boss->pos[0] + character.pos[0]) * 0.5f, mz = (boss->pos[2] + character.pos[2]) * 0.5f;
                float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2], d = sqrtf(dx * dx + dz * dz) + 1e-3f;
#ifndef HLE_SIDECAM_K       // how far out (2: twice as far and high)
#define HLE_SIDECAM_K 1.0f
#endif
                float back = fmaxf(110.0f, d * 1.05f) * HLE_SIDECAM_K;
                customCamPos = (T3DVec3){{ mx - dz / d * back - dx / d * 25.0f, character.pos[1] + 55.0f * HLE_SIDECAM_K, mz + dx / d * back - dz / d * 25.0f }};
                customCamTarget = (T3DVec3){{ mx, character.pos[1] + 18.0f, mz }};
                camera_mode_smooth(CAMERA_CUSTOM, 0.02f);
#ifdef HLE_LOG_SIDECAM  // test aid: where everything is
                { static int n; if (++n % 300 == 0) debugf("[sidecam] %d boss %.0f %.0f %.0f mode %d player %.0f %.0f %.0f ticks %lu\n", n, (double)boss->pos[0], (double)boss->pos[1], (double)boss->pos[2],
                      boss->mode, (double)character.pos[0], (double)character.pos[1], (double)character.pos[2], (unsigned long)get_ticks_ms()); }
#endif
            }
#endif
#ifdef HLE_DUMMY_CAM_NEAR  // close on the boss from its front-left (past the player): its poses
            if (boss) {
                cameraLockOnActive = false;
#ifndef HLE_CAM_NEAR_A      // its angle off the boss's front (1.57: its side) and distance
#define HLE_CAM_NEAR_A 0.9f
#define HLE_CAM_NEAR_D 80.0f
#endif
                float a = boss->rot[1] + HLE_CAM_NEAR_A;
#ifndef HLE_CAM_NEAR_H      // its height, and the height it looks at (above the boss's root)
#define HLE_CAM_NEAR_H 28.0f
#define HLE_CAM_NEAR_LOOK 18.0f
#endif
                customCamPos = (T3DVec3){{ boss->pos[0] - sinf(a) * HLE_CAM_NEAR_D, boss->pos[1] + HLE_CAM_NEAR_H, boss->pos[2] + cosf(a) * HLE_CAM_NEAR_D }};
                customCamTarget = (T3DVec3){{ boss->pos[0], boss->pos[1] + HLE_CAM_NEAR_LOOK, boss->pos[2] }};
                camera_mode_smooth(CAMERA_CUSTOM, 0.01f);
            }
#endif
#ifdef HLE_DUMMY_CAM_PORTRAIT  // the boss alone, centred, from its front (turned until the camera is inside the hall): README shots
            if (boss) {
                cameraLockOnActive = false;
#ifndef HLE_PORTRAIT_D
#define HLE_PORTRAIT_D 110.0f
#endif
#ifndef HLE_PORTRAIT_H
#define HLE_PORTRAIT_H 30.0f
#endif
#ifndef HLE_PORTRAIT_LOOK
#define HLE_PORTRAIT_LOOK 30.0f
#endif
#ifndef HLE_PORTRAIT_TURN
#define HLE_PORTRAIT_TURN 0.5f
#endif
                float a = boss->rot[1] + HLE_PORTRAIT_TURN, cx = 0, cz = 0;
                for (int i = 0; i < 16; i++, a += 0.39f) {
#ifdef HLE_PORTRAIT_FLIPX   // a boss whose front is (+sin, +cos) of its yaw (the Shackled Sun)
                    cx = boss->pos[0] + sinf(a) * HLE_PORTRAIT_D; cz = boss->pos[2] + cosf(a) * HLE_PORTRAIT_D;
#else
                    cx = boss->pos[0] - sinf(a) * HLE_PORTRAIT_D; cz = boss->pos[2] + cosf(a) * HLE_PORTRAIT_D;
#endif
                    float hx = (cx + boss->pos[0]) * 0.5f, hz = (cz + boss->pos[2]) * 0.5f;
                    if (scene_bounds_check_room(cx, character.pos[1], cz) && scene_bounds_check_room(hx, character.pos[1], hz)) break;
                }
                customCamPos = (T3DVec3){{ cx, boss->pos[1] + HLE_PORTRAIT_H, cz }};
                customCamTarget = (T3DVec3){{ boss->pos[0], boss->pos[1] + HLE_PORTRAIT_LOOK, boss->pos[2] }};
                camera_mode_smooth(CAMERA_CUSTOM, 0.01f);
                if (frame % 10 == 0) debugf("[portrait] f%d state %d y %.0f\n", frame, (int)boss->state, (double)boss->pos[1]);
            }
#endif
#ifdef HLE_DUMMY_CAM_HIGH  // a wide view from high up, fixed in the world, on the boss and the player (aim and turning)
            if (boss) {
                cameraLockOnActive = false;
                float mx = (boss->pos[0] + character.pos[0]) * 0.5f, mz = (boss->pos[2] + character.pos[2]) * 0.5f;
#ifndef HLE_CAM_HIGH_K     // how far out (2: twice as high and far back)
#define HLE_CAM_HIGH_K 1.0f
#endif
                customCamPos = (T3DVec3){{ mx, boss->pos[1] + 120.0f * HLE_CAM_HIGH_K, mz - 95.0f * HLE_CAM_HIGH_K }};
                customCamTarget = (T3DVec3){{ mx, boss->pos[1] + 10.0f, mz }};
                camera_mode_smooth(CAMERA_CUSTOM, 0.01f);
            }
#endif
#ifdef HLE_DUMMY_ORBIT   // run round the boss about 50 away (a sidestep the whole time)
            if (boss) {
                float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2], d = sqrtf(dx * dx + dz * dz) + 1e-3f;
                float k = fmaxf(-1.0f, fminf(1.0f, (d - 50.0f) * 0.08f));
                float wx = -dz / d + dx / d * k, wz = dx / d + dz / d * k;      // round it, and in or out to the radius
                float ya = cameraAngleX;     // the stick back from world (character.c, compute_desired_velocity)
                in->stick_y = (int)(70 * (wx * -fm_sinf(ya) + wz * -fm_cosf(ya)));
                in->stick_x = (int)(70 * (wx * fm_cosf(ya) + wz * -fm_sinf(ya)));
                if (frame % 15 == 0) debugf("[orbit] d %.0f player %.0f,%.0f boss %.0f,%.0f stick %d,%d cam %.2f\n", (double)d, (double)character.pos[0], (double)character.pos[2],
                                            (double)boss->pos[0], (double)boss->pos[2], in->stick_x, in->stick_y, (double)ya);
            }
#endif
#ifdef HLE_DUMMY_SIDESTEP   // the Fallen Warrior: step sideways for this many frames the moment a swing commits its aim
            if (boss) {          // (HLE_SIDESTEP_EARLY: as the swing begins, before it commits: it should turn and follow)
#ifdef HLE_SIDESTEP_EARLY
                extern int hle_fk_swing; int now = hle_fk_swing;
#else
                extern int hle_fk_commit; int now = hle_fk_commit;
#endif
                static int seen, left, side = 1;
                if (now != seen) { seen = now; left = HLE_DUMMY_SIDESTEP; side = -side; }
                if (left > 0) {
                    left--;
                    float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2], d = sqrtf(dx * dx + dz * dz) + 1e-3f;
                    float wx = -dz / d * side, wz = dx / d * side, ya = cameraAngleX;     // square to the boss (the stick from world as in HLE_DUMMY_ORBIT)
                    in->stick_y = (int)(80 * (wx * -fm_sinf(ya) + wz * -fm_cosf(ya)));
                    in->stick_x = (int)(80 * (wx * fm_cosf(ya) + wz * -fm_sinf(ya)));
                    if (cameraLockOnActive) { in->stick_x = 80 * side; in->stick_y = 0; }     // locked on, the stick is already square to it
                }
            }
#endif
#ifdef HLE_TEST_STAGGER   // the stagger meter filled in steps: a full stagger every other second, and again
            if (boss && (frame % 60) == 0 && boss->staggerT <= 0.0f) { boss->poise = fminf(1.0f, boss->poise + 0.55f); boss->poiseIdleT = 0.0f;
                debugf("[stagtest] f%d poise %.2f\n", frame, (double)boss->poise); }
#endif
#ifdef HLE_DUMMY_MOVE   // keep running in a wide circle (the cloth in motion)
            in->stick_x = (int)(70 * cosf(tm * 0.8f)); in->stick_y = (int)(70 * sinf(tm * 0.8f));
#endif
#ifdef HLE_DUMMY_CENTER   // held in the middle of the hall, facing the boss (moves that travel, away from the walls)
            if (boss) { character.pos[0] = 0.0f; character.pos[2] = 0.0f; }
#endif
#ifdef HLE_DUMMY_PIN      // held where it started: a blow's knockback can't shove it out past a wall (Shadowborne then stood stuck at the wall)
            { static float pin[2]; static bool set; if (!set) { pin[0] = character.pos[0]; pin[1] = character.pos[2]; set = true; }
              character.pos[0] = pin[0]; character.pos[2] = pin[1]; }
#endif
#ifdef HLE_DUMMY_KEEP     // locked on, keep about this far from the boss, strafing round it (its moves at range, from the side)
            if (boss) { static int k; if (k++ % 60 == 0) pressed->z = 1;
              in->btn.z = 1;
              float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2], d = sqrtf(dx * dx + dz * dz);
              in->stick_y = d < HLE_DUMMY_KEEP - 15.0f ? -70 : d > HLE_DUMMY_KEEP + 15.0f ? 70 : 0;
#ifdef HLE_KEEP_SWING     // and swing on a loop where it stands (what a leap that comes to it meets)
              if (k % 12 == 0) { pressed->b = 1; in->btn.b = 1; }
#endif
#ifndef HLE_DUMMY_KEEP_STILL    // (or only in and out while the boss isn't attacking: no strafe)
              in->stick_x = 45;
#else
              { extern bool hle_open_now; if (!hle_open_now) in->stick_y = 0; }
#endif
            }
#endif
#ifdef HLE_DUMMY_STRIKE   // and fight back: locked on, walk in, swing (the stagger meter, hit-stop)
            if (boss) { static int k; k++;     // (no boss once the run is back at the title)
              if (k == 1) pressed->z = 1;
              in->btn.z = 1;
              float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2];
              if (sqrtf(dx * dx + dz * dz) > 40.0f) in->stick_y = 70;
#ifdef HLE_STRIKE_OPEN     // swing only while the boss isn't attacking (a player who dodges its moves)
              extern bool hle_open_now;
              if (k % HLE_STRIKE_EVERY == 0 && hle_open_now) { pressed->b = 1; in->btn.b = 1; }
#else
              if (k % HLE_STRIKE_EVERY == 0) { pressed->b = 1; in->btn.b = 1; }
#endif
#ifdef HLE_STRIKE_STAMINA     // a player who manages stamina: whole combos
              character.stamina = character_get_max_stamina();
#endif
            }
#endif
#ifdef HLE_DUMMY_ROLL     // roll every 0.7 s: some catch a blow right at the start (a perfect dodge)
            { static int k; if (++k % 42 == 0) { pressed->a = 1; in->btn.a = 1; } }
#endif
#ifdef HLE_ELITE_SWEEP   // test aid (with HLE_ELITE_FORCE=0): the fight in windows of HLE_SWEEP_S s, each with the next affix of the list
                         // (AFX_ + 1; 0 none), round and round: [afxwin] each window's damage, hits and openings (the metric's sword in them)
            {
#ifndef HLE_SWEEP_S
#define HLE_SWEEP_S 240.0f
#endif
                extern int hle_elite_force;
                static const int L[] = { HLE_ELITE_SWEEP };
                static int w = -1; static float w0, d0, o0, od0; static int h0, g0;
                if (w < 0 || tm - w0 >= HLE_SWEEP_S) {
                    if (w >= 0) debugf("[afxwin] boss %d affix %d window %d: %.0f s damage %.1f hits %d | open %.1f s, %d gaps, sword %.1f\n", HLE_DUMMY_BOSS, L[w % (int)(sizeof L / sizeof L[0])], w,
                                       (double)(tm - w0), (double)(hle_dummy_damage - d0), hle_dummy_hits - h0, (double)(hle_open_time - o0), hle_open_gaps - g0, (double)(hle_open_dmg - od0));
                    w++; w0 = tm; d0 = hle_dummy_damage; h0 = hle_dummy_hits; o0 = hle_open_time; g0 = hle_open_gaps; od0 = hle_open_dmg;
                    int a = L[w % (int)(sizeof L / sizeof L[0])];
                    hle_elite_force = a ? 1 << (a - 1) : 0;
                }
            }
#endif
            float prev = tm;
            tm += deltaTime;
            static int heapMin = 1 << 30;     // the lowest free heap, sampled each second
            if ((int)tm != (int)prev) { heap_stats_t h; sys_get_heap_stats(&h); if (h.total - h.used < heapMin) heapMin = h.total - h.used; }
            if ((int)(tm / 30.0f) != (int)(prev / 30.0f))
                debugf("[dummy] boss %d t %.0f s damage %.0f hits %d | open %.1f%% of the time, %d gaps of %.2f s, sword %.0f | %.1f fps, heap min %d KB free\n", HLE_DUMMY_BOSS, (double)tm,
                       (double)hle_dummy_damage, hle_dummy_hits, (double)(100.0f * hle_open_time / (tm > 0 ? tm : 1)), hle_open_gaps,
                       (double)(hle_open_gaps ? hle_open_time / hle_open_gaps : 0.0f), (double)hle_open_dmg, (double)display_get_fps(), heapMin / 1024);
        }
        return;
    }
#endif
#ifdef HLE_TEST_HEAVY
    // Hold B for 0.8 s in the fight: must turn into the strong attack
    {
        static int start = -1;
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) start = frame;
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
        int t = frame - start;
        if (t >= 60 && t < 90) in->btn.b = 1;
        if (t == 60) pressed->b = 1;
        if (t < 60) {       // keep right in front of the boss, facing it
            float a = boss->rot[1];
            character.pos[0] = boss->pos[0] - fm_sinf(a) * 30.0f; character.pos[2] = boss->pos[2] + fm_cosf(a) * 30.0f;
            character.rot[1] = a + T3D_PI;
        }
        if (t == 140) { pressed->b = 1; in->btn.b = 1; }    // a light attack, for comparison
        if (t == 59) debugf("[heavy] frame %d knight %.0f,%.0f boss %.0f,%.0f\n", frame, (double)character.pos[0], (double)character.pos[2], (double)boss->pos[0], (double)boss->pos[2]);
        if (t >= 60 && t < 170 && t % 4 == 0) debugf("[heavy] t %d state %d boss hp %.1f stamina %.0f\n", t, character_get_state(), (double)boss->health, (double)character.stamina);
        return;
    }
#endif
#ifdef HLE_TEST_ENDING
    {
        static int start = -1;
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT) { start = frame; debugf("[end] fight at %d\n", frame); }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (frame == start + 30) {
            extern void scene_test_start_ending(void);
            scene_test_start_ending();
            debugf("[end] ending at %d\n", frame);
        } else if (frame % 150 == 0) {
            debugf("[end] f%d boss %p\n", frame, (void *)boss);
        }
        return;
    }
#endif
#ifdef HLE_TEST_BONUS
    // Title -> Bonus -> Fallen Warrior; watch the intro, stand still (it leaps at range),
    // kill it, watch the outro and the return to the title
    {
        static int phase = 0, t0 = 0, step = 0;
        Boss *boss = boss_get_instance();
        int t = frame - t0;
        static int heapMin = 1 << 30;     // the lowest free heap in the fight, sampled every 10 frames
        if (phase == 3 && frame % 10 == 0) { heap_stats_t h; sys_get_heap_stats(&h); if (h.total - h.used < heapMin) heapMin = h.total - h.used; }
#ifdef HLE_TEST_LEAK
        { extern int hle_leak_cyc; extern void hle_low_report(int), hle_leak_by_caller(const char *);
          hle_leak_cyc = phase; if (phase == 3 && t == 60) { hle_low_report(2); hle_low_report(3); hle_leak_by_caller("bonus"); }
          if (phase == 3 && t == 3000) { hle_low_report(3); hle_leak_by_caller("bonuslate"); } }
#endif
        if (frame % 60 == 0) debugf("[bonus] f%d phase %d boss %d mode %d state %d hp %.0f heap min %d KB\n", frame, phase,
                                    boss ? boss->phaseIndex : -1, boss ? boss->mode : -1, boss ? boss->state : -1,
                                    boss ? (double)boss->health : 0.0, phase == 3 ? heapMin / 1024 : -1);
        if (phase == 0) {               // tap A until the title menu opens
            if (menu_controller_is_active()) { phase = 1; t0 = frame; }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {        // Down, Down (Bonus), A, A (Fallen Warrior)
#if defined(HLE_TEST_BONUS_SUN)
            static const char seq[] = "DADA";       // Bonus > The Shackled Sun
#elif defined(HLE_TEST_BONUS_DRAGON)
            static const char seq[] = "DADDA";      // Bonus > Shadowborne
#else
            static const char seq[] = "DAA";        // Bonus > Fallen Warrior
#endif
            int n = (int)sizeof(seq) - 1;
            if (t >= 20 && step < n) {
                if (seq[step] == 'D') pressed->d_down = 1; else { pressed->a = 1; in->btn.a = 1; }
                step++; t0 = frame;
            }
            if (step == n && boss) { phase = 2; t0 = frame; debugf("[bonus] scene at %d\n", frame); }
        } else if (phase == 2) {
            if (boss && boss->phaseIndex >= 3 && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) {
                phase = 3; t0 = frame; debugf("[bonus] fight at %d\n", frame);
            }
        } else if (phase == 3) {
#ifdef HLE_TEST_FK_LEAP
            in->stick_x = 70;       // circle it (locked on) for a side view
#endif
#ifdef HLE_TEST_BONUS_SUN
            if (t == 450) { boss_apply_damage(boss, boss->maxHealth * 0.55f); debugf("[bonus] half at %d\n", frame); }
#endif
#ifdef HLE_TEST_STRIKE
            // Can the sword land? Locked on, walk in and swing for 60 s (topped up): every
            // drop in the boss's health is a hit
            {
                static float lastHp; static int hits; static float lost;
                if (t == 1) { lastHp = boss->health; pressed->z = 1; }
                in->btn.z = 1;
                float dx = boss->pos[0] - character.pos[0], dz = boss->pos[2] - character.pos[2];
                if (sqrtf(dx * dx + dz * dz) > 45.0f) in->stick_y = 70;
                if (t % HLE_STRIKE_EVERY == 0) { pressed->b = 1; in->btn.b = 1; }
                {   // the blade's closest approach to the boss's capsule this swing (surface gap)
                    static float gap = 1e9f, lowY, tipY;
                    extern T3DVec3 charWeaponCapA, charWeaponCapB;     // the blade, as the collision system has it
                    const float *sb = charWeaponCapA.v, *st = charWeaponCapB.v;
                    if (character_get_state() == CHAR_STATE_ATTACKING) {
                        const float *bp = boss->pos; const T3DVec3 *ca = &boss->capsuleCollider.localCapA, *cb = &boss->capsuleCollider.localCapB;
                        for (int i = 0; i <= 8; i++) {
                            float p[3]; for (int m = 0; m < 3; m++) p[m] = sb[m] + (st[m] - sb[m]) * i / 8.0f;
                            float a0[3] = { bp[0] + ca->v[0], bp[1] + ca->v[1], bp[2] + ca->v[2] }, u[3] = { cb->v[0] - ca->v[0], cb->v[1] - ca->v[1], cb->v[2] - ca->v[2] };
                            float l2 = u[0]*u[0] + u[1]*u[1] + u[2]*u[2], k = l2 > 0 ? ((p[0]-a0[0])*u[0] + (p[1]-a0[1])*u[1] + (p[2]-a0[2])*u[2]) / l2 : 0;
                            k = k < 0 ? 0 : k > 1 ? 1 : k;
                            float e[3] = { a0[0] + u[0]*k - p[0], a0[1] + u[1]*k - p[1], a0[2] + u[2]*k - p[2] };
                            float g = sqrtf(e[0]*e[0] + e[1]*e[1] + e[2]*e[2]) - boss->capsuleCollider.radius - 2.0f;
                            if (g < gap) { gap = g; lowY = p[1] - character.pos[1]; tipY = st[1] - character.pos[1]; }
                        }
                    }
                    if (t % 24 == 23) { if (gap < 1e8f) debugf("[strike] blade gap %.0f (closest point %.0f up, tip %.0f up)\n", (double)gap, (double)lowY, (double)tipY); gap = 1e9f; }
                }
                if (t % 24 == 12) debugf("[strike] swing: dist %.0f, boss %.0f above the player, %s\n", (double)sqrtf(dx * dx + dz * dz),
                                         (double)(boss->pos[1] - character.pos[1]), boss->health < lastHp - 0.01f ? "hit" : "-");
                if (boss->health < lastHp - 0.01f) { hits++; lost += lastHp - boss->health; }
                lastHp = boss->health;
                if (t == 2000) {
                    debugf("[strike] boss %d: %d hits, %.0f of %.0f health in 60 s\n", boss->phaseIndex, hits, (double)lost, (double)boss->maxHealth);
                    boss_apply_damage(boss, boss->maxHealth * 2.0f); phase = 4; t0 = frame;
                }
            }
#elif defined(HLE_DRAGON_FACE)
            // Its head up close: in front of the snout (level, below, above), at its side (the
            // jaw against the neck), from the front at 3/4 and from behind it; 40 frames a view
            {
                extern void dragon_boss_hle_head(float p[3], float z[3], float y[3]);
                cameraLockOnActive = false;
                if (t == 1) { character.pos[0] = boss->pos[0] - 260.0f; character.pos[2] = boss->pos[2] + 200.0f; }
                float hp[3], hz[3], hy[3];
                dragon_boss_hle_head(hp, hz, hy);
                float hx[3] = { hy[1] * hz[2] - hy[2] * hz[1], hy[2] * hz[0] - hy[0] * hz[2], hy[0] * hz[1] - hy[1] * hz[0] };
                static const float FZ[6] = { 60, 55, 55, 12, 45, -28 }, FY[6] = { 4, -22, 26, -2, 8, 30 }, FX[6] = { 0, 0, 0, 45, 32, 6 };
                int v = (t / 40) % 6;      // (5: from behind and above, onto the back of the skull)
                float c[3], g[3];
                for (int m = 0; m < 3; m++) { c[m] = hp[m] + hz[m] * FZ[v] + hy[m] * FY[v] + hx[m] * FX[v]; g[m] = hp[m] + hz[m] * (v == 3 ? 4.0f : 12.0f); }
                customCamPos = (T3DVec3){{ c[0], c[1], c[2] }};
                customCamTarget = (T3DVec3){{ g[0], g[1], g[2] }};
                camera_mode_smooth(CAMERA_CUSTOM, 0.01f);
                if (t == 1200) { boss_apply_damage(boss, boss->maxHealth * 2.0f); phase = 4; t0 = frame; }
            }
#elif defined(HLE_DRAGON_SHOWCASE)
            // Model check: the camera orbits Shadowborne (front, 3/4, side, back, then the head
            // up close), 50 frames a view, the player standing well clear
            {
                cameraLockOnActive = false;
                if (t == 1) { character.pos[0] = boss->pos[0] - 260.0f; character.pos[2] = boss->pos[2] + 200.0f; }
                static const float A[6] = { 0.0f, 0.8f, 1.57f, 2.4f, 3.14f, 0.35f }, D[6] = { 150, 150, 160, 150, 160, 70 }, H[6] = { 50, 60, 45, 70, 60, 45 };
                int v = (t / 50) % 6;
                float a = boss->rot[1] + A[v], *bp = boss->pos;
                customCamPos = (T3DVec3){{ bp[0] + sinf(a) * D[v], bp[1] + H[v], bp[2] + cosf(a) * D[v] }};
                customCamTarget = (T3DVec3){{ bp[0] + sinf(boss->rot[1]) * (v == 5 ? 70.0f : 0.0f), bp[1] + (v == 5 ? 55.0f : 30.0f), bp[2] + cosf(boss->rot[1]) * (v == 5 ? 70.0f : 0.0f) }};
                camera_mode_smooth(CAMERA_CUSTOM, 0.01f);
                if (t == 900) { boss_apply_damage(boss, boss->maxHealth * 0.55f); debugf("[bonus] half at %d\n", frame); }
                if (t == 1800) { boss_apply_damage(boss, boss->maxHealth * 2.0f); phase = 4; t0 = frame; }
            }
#elif defined(HLE_TEST_DRAGON_AIR)
            // Claw rings (HLE_DRAGON_FORCE=2): held 45 in front of it, in the air for the first
            // 1000 frames, then on the floor; every hit is logged
            {
                float a = boss->rot[1];
                character.pos[0] = boss->pos[0] + sinf(a) * 45.0f; character.pos[2] = boss->pos[2] + cosf(a) * 45.0f;
                character.pos[1] = boss->pos[1] + (t < 1000 ? 25.0f : 0.0f);
                cameraLockOnActive = false;     // a 3/4 view from its left front, to see the legs
                customCamPos = (T3DVec3){{ boss->pos[0] + sinf(a + 0.9f) * 150.0f, boss->pos[1] + 40.0f, boss->pos[2] + cosf(a + 0.9f) * 150.0f }};
                customCamTarget = (T3DVec3){{ boss->pos[0] + sinf(a) * 25.0f, boss->pos[1] + 20.0f, boss->pos[2] + cosf(a) * 25.0f }};
                camera_mode_smooth(CAMERA_CUSTOM, 0.01f);
                if (t == 1000) debugf("[air] now on the floor\n");
                if (t == 2000) { debugf("[air] done\n"); boss_apply_damage(boss, boss->maxHealth * 2.0f); phase = 4; t0 = frame; }
            }
#elif defined(HLE_TEST_DRAGON_WHIP)
            // The spin (HLE_DRAGON_FORCE=14): stand behind it; log its yaw through each spin
            {
                float a = boss->rot[1] + T3D_PI;
                if (t == 1) { character.pos[0] = boss->pos[0] + sinf(a) * 70.0f; character.pos[2] = boss->pos[2] + cosf(a) * 70.0f; }
                if (t % 4 == 0) debugf("[whip] f%d yaw %.2f\n", frame, (double)boss->rot[1]);
            }
#elif defined(HLE_TEST_BONUS_DRAGON)
            // a long fight, circling it (every hit is logged, the player is topped up): half at 60 s, dead at 120 s
            in->stick_x = (int)(70 * cosf(t * 0.015f)); in->stick_y = (int)(70 * sinf(t * 0.015f));
            if (t == 2000) { boss_apply_damage(boss, boss->maxHealth * 0.55f); debugf("[bonus] half at %d\n", frame); }
            if (t == 4000) { boss_apply_damage(boss, boss->maxHealth * 2.0f); debugf("[bonus] killed at %d\n", frame); phase = 4; t0 = frame; }
#else
            if (t == 900) { boss_apply_damage(boss, boss->maxHealth * 2.0f); debugf("[bonus] killed at %d\n", frame); phase = 4; t0 = frame; }
#endif
        } else if (phase == 4) {
            if (!boss) { debugf("[bonus] back at the title at %d\n", frame); phase = 5; }
        }
        return;
    }
#endif
#ifdef HLE_TEST_TWO_RUNS
    // Run 1: kill the Guardian, reach phase 2, die, back to the title. Run 2: kill the
    // Guardian again; phase 2 must start again.
    {
        static int run = 1, phase = 0, t0 = 0;
        Boss *boss = boss_get_instance();
        int t = frame - t0;
        if (frame % 60 == 0) debugf("[2r] f%d run %d phase %d boss %d mode %d hp %.0f\n", frame, run, phase,
                                    boss ? boss->phaseIndex : -1, boss ? boss->mode : -1, boss ? (double)boss->health : 0.0);
        if (phase == 0) {               // menus / intro: tap A until the phase-1 fight
            if (boss && boss->mode == BOSS_MODE_COMBAT && boss->phaseIndex == 1) { phase = 1; t0 = frame; }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (phase == 1) {
#ifdef HLE_2R_LOCK          // locked on at the kill, as a player usually is; the camera logged through the cutscene
            if (t == 20) pressed->z = 1;
            if (t >= 20 && t <= 60) in->btn.z = 1;
            if (t % 15 == 0) debugf("[2rcam] t %d lock %d blend %.3f cam %.1f %.1f %.1f tgt %.1f %.1f %.1f lockTgt %.1f %.1f %.1f\n", t, cameraLockOnActive, (double)cameraLockBlend,
                                    (double)characterCamPos.v[0], (double)characterCamPos.v[1], (double)characterCamPos.v[2],
                                    (double)characterCamTarget.v[0], (double)characterCamTarget.v[1], (double)characterCamTarget.v[2],
                                    (double)cameraLockOnTarget.v[0], (double)cameraLockOnTarget.v[1], (double)cameraLockOnTarget.v[2]);
#endif
#ifndef HLE_2R_KILL_T       // frames of phase 1 before the kill (a real fight records far more blocks)
#define HLE_2R_KILL_T 60
#endif
#if HLE_2R_KILL_T > 60
            if (t < HLE_2R_KILL_T) {
                character.health = character.maxHealth;
                in->stick_x = (t / 90) % 2 ? 60 : -60; in->stick_y = (t / 140) % 2 ? 40 : -40;
                if (t % 25 == 0) { pressed->b = 1; in->btn.b = 1; }
            }
#endif
            if (t == HLE_2R_KILL_T) boss_apply_damage(boss, boss->maxHealth * 2.0f);
            if (t > HLE_2R_KILL_T && boss->phaseIndex == 2 && boss->mode == BOSS_MODE_COMBAT) {
                debugf("[2r] run %d: phase 2 fight OK\n", run);
                phase = 2; t0 = frame;
            }
#ifndef HLE_2R_NOSKIP       // the cutscenes played out, as a player who doesn't skip them
            else if (t > 90 && frame % 20 == 0) { pressed->a = 1; in->btn.a = 1; }
#endif
            if (t > 3000 && t % 300 == 0) debugf("[2r] run %d: phase 2 NOT started\n", run);
        } else if (phase == 2 && run == 1) {
            if (character_get_state() != CHAR_STATE_DEAD) { if (t % 10 == 0) character_apply_damage(50.0f); }
            else { run = 2; phase = 3; t0 = frame; }
        } else if (phase == 3) {        // death screen -> title -> Continue
            if (!boss_get_instance() || boss->phaseIndex != 2) { phase = 0; t0 = frame; }
            else if (frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        }
        return;
    }
#endif
#ifdef HLE_TEST_BOSS2
    // Kill the Guardian, skip the transformation, fight Guardian Unbound, kill it too
    {
        static int phase = 0, t0 = 0;
        Boss *boss = boss_get_instance();
        int t = frame - t0;
        if (frame % 60 == 0 && boss)
            debugf("[b2] f%d phase %d boss phase %d mode %d state %d hp %.0f/%.0f msa %d\n", frame, phase,
                   boss->phaseIndex, boss->mode, boss->state, (double)boss->health, (double)boss->maxHealth,
                   msa_has_active_aerial_swords());
        if (phase == 0) {               // get to the fight
            if (boss && boss->mode == BOSS_MODE_COMBAT && boss->phaseIndex == 1) { phase = 1; t0 = frame; }
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
        } else if (!boss) {
            return;
        } else if (phase == 1) {        // first death
            if (t == 120) { boss_apply_damage(boss, boss->maxHealth * 2.0f); debugf("[b2] killed guardian\n"); }
            if (t > 120 && boss->phaseIndex == 2 && boss->mode == BOSS_MODE_COMBAT) { phase = 2; t0 = frame; debugf("[b2] unbound fight at %d\n", frame); }
#ifndef HLE_TEST_WATCH_CUTSCENE
            else if (t > 150 && frame % 20 == 0) { pressed->a = 1; in->btn.a = 1; }    // skip the cutscene
#endif
        } else if (phase == 2) {        // fight a while, then the final death
            if (t % 45 == 0) { pressed->b = 1; in->btn.b = 1; }
#ifdef HLE_TEST_BOSS2_DIE
            if (t % 60 == 0) debugf("[b2] player hp %.0f state %d\n", (double)character_get_health(), character_get_state());
            if (character_get_state() == CHAR_STATE_DEAD) { phase = 4; t0 = frame; debugf("[b2] player died at %d\n", frame); }
#else
            if (t == 900) { boss_apply_damage(boss, boss->maxHealth * 2.0f); debugf("[b2] killed unbound\n"); phase = 3; t0 = frame; }
#endif
        } else if (phase == 3) {        // the Fallen Knight descends
            if (boss->phaseIndex == 3 && boss->mode == BOSS_MODE_COMBAT) { phase = 5; t0 = frame; debugf("[b2] fallen knight fight at %d\n", frame); }
        } else if (phase == 5) {        // fight it, then the final death
            if (t % 40 == 0) { pressed->b = 1; in->btn.b = 1; }
            if (t % 60 == 0) debugf("[b2] FK hp %.0f state %d | player hp %.0f state %d\n", (double)boss->health, boss->state,
                                    (double)character_get_health(), character_get_state());
            if (t == 1500) { boss_apply_damage(boss, boss->maxHealth * 2.0f); debugf("[b2] killed fallen knight\n"); phase = 6; t0 = frame; }
        } else if (phase == 6) {
            if (t % 120 == 0) debugf("[b2] end: mode %d state %d hp %.0f\n", boss->mode, boss->state, (double)boss->health);
        } else if (phase == 4) {        // restart from the death screen: back to the first Guardian
            if (frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            if (t % 120 == 0) debugf("[b2] after restart: boss phase %d name %s mode %d hp %.0f\n", boss->phaseIndex, boss->name, boss->mode, (double)boss->health);
        }
        return;
    }
#endif
#ifdef HLE_TEST_TOGGLE
    // Skills OFF in the pause menu, die, restart, skills ON in the pause menu, try Cleave
    {
        enum { WAIT1, MENU_OFF, DIE, WAIT2, MENU_ON, TRY, DONE };
        static int phase = WAIT1, t0 = 0, step = 0;
        Boss *boss = boss_get_instance();
        bool combat = boss && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL;
        int t = frame - t0;
        // pause menu: Resume, Restart, Settings, Controls / settings: Audio, Video, Controls, Skills, Skin, Back
        // O = open the pause menu, C = close it (both wait until it took effect)
        static const char seq[] = "ODDADDDABBC";
        switch (phase) {
        case WAIT1: case WAIT2:
            if (!combat) {
                t0 = frame;
                if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            } else if (t > 60) {
                phase++; t0 = frame; step = 0;
                debugf("[tog] fight at %d\n", frame);
            }
            break;
        case MENU_OFF: case MENU_ON:
            if (t >= 12 && step < (int)sizeof(seq) - 1) {
                char c = seq[step];
                bool open = menu_controller_is_active();
                if (c == 'O' || c == 'C') {
                    if (open == (c == 'O')) step++;
                    else { pressed->start = 1; in->btn.start = 1; }
                } else {
                    if (c == 'D') pressed->d_down = 1;
                    if (c == 'A') { pressed->a = 1; in->btn.a = 1; }
                    if (c == 'B') { pressed->b = 1; in->btn.b = 1; }
                    step++;
                }
                t0 = frame;
            }
            if (step >= (int)sizeof(seq) - 1) {
                debugf("[tog] menu done: skills %d\n", character_skills_enabled());
                phase++; t0 = frame;
            }
            break;
        case DIE:
            if (character_get_state() == CHAR_STATE_DEAD) {
                if (frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            } else if (t > 300 && !combat) {
                phase = WAIT2; t0 = frame;
                debugf("[tog] restarted at %d\n", frame);
            }
            break;
        case TRY:
            if (t == 30 || t == 120) {
                pressed->d_down = 1;
                debugf("[tog] D-down: skills %d state %d\n", character_skills_enabled(), character_get_state());
            }
            if (t == 60 || t == 150) {
                debugf("[tog] after: state %d cooldown %.1f\n", character_get_state(),
                       (double)character_get_skill_cooldown(SKILL_CRESCENT_CLEAVE));
                if (t == 150) phase = DONE;
            }
            break;
        }
        return;
    }
#endif
#ifdef HLE_TEST_SKILL_DMG
    // Damage per use (with HLE_TEST_FREEZE_BOSS): every 200 frames the Guardian is healed and
    // the player set 30 in front of it, locked on, then uses HLE_TEST_SKILL_DMG once
    // (0: the three-swing combo, 1: Moon Blade Dance, 2: Halfmoon Slash, 3: Crescent Cleave)
    {
        extern float deltaTime;
        static int start = -1;
        Boss *boss = boss_get_instance();
        if (start < 0) {
            if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) start = frame;
            else if (frame > 200 && frame % 30 == 0) { pressed->a = 1; in->btn.a = 1; }
            return;
        }
#ifndef HLE_SDMG_CYCLE
#define HLE_SDMG_CYCLE 200      // frames a cast (Full Moon Dance needs about 300)
#endif
        int t = (frame - start) % HLE_SDMG_CYCLE, cyc = (frame - start) / HLE_SDMG_CYCLE;
        static float hp0; static int t0;
        if (t == 0) debugf("[sdmgcyc] cast %d starts at %lu ms (VI frame ~%lu)\n", cyc, (unsigned long)get_ticks_ms(), (unsigned long)(get_ticks_ms() * 60 / 1000));
        if (t == 0) {
#ifdef HLE_SDMG_TOWER     // in a Tower run holding every skill (at Lv 1; HLE_SKILL_LV: one at a level and form)
            { extern void tower_begin_run(uint32_t); extern bool tower_active(void); if (!tower_active()) tower_begin_run(1); }
#ifdef HLE_SKILL_LV       // <skill 1-3>,<lv 1-5>,<form 1 A, 2 B>; lv 0: each cast the next of Lv 1-4, then 5 A, 5 B
            { extern void tower_test_skill_lv(int, int, int); static const int L[3] = { HLE_SKILL_LV };
              if (!L[1]) { int c = cyc % 6; tower_test_skill_lv(L[0] - 1, c < 4 ? c + 1 : 5, c == 5 ? 2 : 1); } }
#endif
#ifdef HLE_SDMG_STAGGERED   // the boss held down, with Executioner, 3 Breaker pacts and Eclipse: its bonuses capped at +100%
            { extern void tower_test_stagger_build(void); tower_test_stagger_build(); }
#endif
#endif
            boss->health = boss->maxHealth; hp0 = boss->health;
            float a = atan2f(character.pos[0] - boss->pos[0], character.pos[2] - boss->pos[2]);
#ifndef HLE_SDMG_DIST
#define HLE_SDMG_DIST 30.0f     // from the boss
#endif
            character.pos[0] = boss->pos[0] + sinf(a) * HLE_SDMG_DIST; character.pos[2] = boss->pos[2] + cosf(a) * HLE_SDMG_DIST;
            character.stamina = character.maxStamina;
#ifdef HLE_TEST_LOW_STAMINA     // a sliver of stamina: the first action still goes
            character.stamina = 1.0f;
#endif
            boss->poise = 0.0f; boss->staggerT = 0.0f;
        }
#ifdef HLE_SDMG_STAGGERED
        boss->staggerT = 3.0f; boss->poise = 0.0f;
#endif
#ifdef HLE_SDMG_FT        // test aid: the frame time through each cast (10-190 frames into its cycle): mean and worst
        static uint32_t ftPrev, ftSum, ftWorst, ftN;
        { uint32_t now = get_ticks_us(); if (t > 10 && t <= HLE_SDMG_CYCLE - 10) { uint32_t d = now - ftPrev; ftSum += d; ftN++; if (d > ftWorst) ftWorst = d; } ftPrev = now;
          if (t == HLE_SDMG_CYCLE - 9 && ftN) { debugf("[sdmgft] cast %d: mean %u us, worst %u us over %u frames\n", cyc, (unsigned)(ftSum / ftN), (unsigned)ftWorst, (unsigned)ftN); ftSum = ftWorst = ftN = 0; } }
#endif
        if (t == 2 && cyc == 0) pressed->z = 1;
#ifdef HLE_DUMMY_SKIN   // in this skin (the cloth through a skill)
        if (t == 2 && cyc == 0) character_skin_set(HLE_DUMMY_SKIN);
#endif
        in->btn.z = 1;
        if (t == 10) t0 = frame;
#ifdef HLE_MBD_XSTILL   // one Moon Blade X held still in the air (flying +z, as seen from behind at first), the camera circling it HLE_MBD_XSTILL away
        if (cyc == 0 && t >= 10) {
            static float xp[3];
            if (t == 10) {
                xp[0] = camTarget.v[0]; xp[1] = camTarget.v[1] + 40.0f; xp[2] = camTarget.v[2];
                skill_fx_spawn_wave(SKILL_WAVE_X, xp, (float[3]){ 0.0f, 0.0f, 0.001f }, 0.0f, 0, 0.0f, 40.0f, false);
                debugf("[xstill] at %.0f %.0f %.0f, frame %d\n", (double)xp[0], (double)xp[1], (double)xp[2], frame);
            }
            float a = (t - 10) * (6.2832f / 360.0f);    // one turn in 360 frames, from behind
            cameraLockOnActive = false;
            customCamPos = (T3DVec3){{ xp[0] - sinf(a) * HLE_MBD_XSTILL, xp[1], xp[2] - cosf(a) * HLE_MBD_XSTILL }};
            customCamTarget = (T3DVec3){{ xp[0], xp[1], xp[2] }};
            camera_mode_smooth(CAMERA_CUSTOM, 0.0f);
            if (t == 10 + 90 || t == 10 + 180 || t == 10 + 360) debugf("[xstill] turn %d at frame %d\n", (t - 10) / 90, frame);
            return;
        }
#endif
#if HLE_TEST_SKILL_DMG == 0
        if (t == 10 || t == 28 || t == 46) { pressed->b = 1; in->btn.b = 1; }
#elif HLE_TEST_SKILL_DMG == 1
        if (t == 10) pressed->d_up = 1;
#ifdef HLE_MBD_CLOSECAM     // (HLE_MBD_CAMYAW radians, default 1.2)
#ifndef HLE_MBD_CAMYAW
#define HLE_MBD_CAMYAW 1.2f
#endif
        // a still camera HLE_MBD_CLOSECAM from the side of the waves' path, from the cast (unlocked: they aim along the camera's old facing)
        {
            static T3DVec3 p, tg;
            if (t == 11 && cyc == 0) {
                // the follow camera here, turned HLE_MBD_CAMYAW round its target, HLE_MBD_CLOSECAM nearer
                float ox = camPos.v[0] - camTarget.v[0], oz = camPos.v[2] - camTarget.v[2], c = cosf(HLE_MBD_CAMYAW), sn = sinf(HLE_MBD_CAMYAW);
                p  = (T3DVec3){{ camTarget.v[0] + (ox * c - oz * sn) * HLE_MBD_CLOSECAM, 45.0f, camTarget.v[2] + (ox * sn + oz * c) * HLE_MBD_CLOSECAM }};
                tg = (T3DVec3){{ camTarget.v[0], 32.0f, camTarget.v[2] }};
                debugf("[mbdcam] cam %.0f %.0f %.0f -> %.0f %.0f %.0f (follow %.0f %.0f %.0f)\n", (double)p.v[0], (double)p.v[1], (double)p.v[2], (double)tg.v[0], (double)tg.v[1], (double)tg.v[2], (double)camPos.v[0], (double)camPos.v[1], (double)camPos.v[2]);
            }
            if (t >= 11 && cyc == 0) { cameraLockOnActive = false; customCamPos = p; customCamTarget = tg; camera_mode_smooth(CAMERA_CUSTOM, 0.0f); }
        }
#endif
#ifdef HLE_MBD_KD   // a heavy hit HLE_MBD_KD frames into the dance (knocked down mid-air)
        if (t == 10 + HLE_MBD_KD && cyc == 0) { debugf("[mbdkd] hit at frame %d, y %.1f\n", frame, (double)character.pos[1]); character_apply_damage(25.0f); }
        if (cyc == 0 && t > 10 && t < 120) debugf("[mbdkd] f %d state %d y %.1f\n", frame, character_get_state(), (double)character.pos[1]);
#endif
#elif HLE_TEST_SKILL_DMG == 2
        if (t == 10) pressed->d_right = 1;
#else
        if (t == 10) pressed->d_down = 1;
#endif
#ifdef HLE_SDMG_RAISE     // the boss held this high in the air (its feet above the floor): out of the wave's reach?
        { static float y0; if (cyc == 0 && t == 0) y0 = boss->pos[1]; boss->pos[1] = y0 + (float)HLE_SDMG_RAISE; }
#endif
        static int busyEnd;
        static float poisePeak;
        if (t == 11) { busyEnd = 0; poisePeak = 0.0f; }
        if (boss->poise > poisePeak) poisePeak = boss->poise;
        if (t > 11 && !busyEnd && character_get_state() == CHAR_STATE_NORMAL) busyEnd = frame;
        if (t == HLE_SDMG_CYCLE - 5) { extern int tower_skill_level(int), tower_skill_form(int); int k = HLE_TEST_SKILL_DMG - 1;
            debugf("[sdmg] skill %d Lv %d form %d: %.1f damage, busy %.2f s, stagger meter %.2f (its peak %.3f)%s\n", HLE_TEST_SKILL_DMG, k >= 0 ? tower_skill_level(k) : 0, k >= 0 ? tower_skill_form(k) : 0,
                   (double)(hp0 - boss->health), (double)((busyEnd - t0) * deltaTime), (double)boss->poise, (double)poisePeak, boss->staggerT > 0.0f ? " (staggered)" : ""); }
        return;
    }
#endif
#ifdef HLE_TEST_SKILLS
    // A (not Start, it pauses) until the fight starts, then each skill once:
    // Halfmoon, lock on, Cleave, Moon Blade Dance jumping left
    static int start = -1;
    if (start < 0) {
        Boss *boss = boss_get_instance();
        if (frame > 200 && boss && boss->mode == BOSS_MODE_COMBAT && character_get_state() == CHAR_STATE_NORMAL) {
            start = frame;
            debugf("[skill] fight starts at frame %d\n", frame);
        } else if (frame > 200 && frame % 30 == 0) {
            pressed->a = 1; in->btn.a = 1;
        }
        return;
    }
    int t = frame - start;
#if defined(HLE_TEST_FREEZE_BOSS) && !defined(HLE_SKILLS_BEHIND)    // HLE_SKILLS_BEHIND: the camera stays behind the knight (README GIFs)
    if (t < 40) in->btn.c_left = 1;     // orbit the camera to a side view
#endif
#ifdef HLE_SKILLS_BEHIND    // the follow camera, settled, held still from just before the first skill
    {
        static T3DVec3 pos, target;
        if (t == 120) {     // pulled back and aimed higher: Moon Blade Dance's leap stays in frame
            target = (T3DVec3){{ character.pos[0], character.pos[1] + 40.0f, character.pos[2] }};
            for (int k = 0; k < 3; k++) pos.v[k] = character.pos[k] + (camPos.v[k] - character.pos[k]) * 1.4f;
        }
        if (t >= 120) { cameraLockOnActive = false; customCamPos = pos; customCamTarget = target; camera_mode_smooth(CAMERA_CUSTOM, 0.0f); }
    }
#endif
    if (t == 150) pressed->d_right = 1;
#ifndef HLE_HIDE_BOSS   // (nothing to lock on to, and the leap straight up: the camera stays put)
    if (t == 160) { pressed->z = 1; in->btn.z = 1; }
#endif
    if (t == 360) pressed->d_down = 1;
#ifndef HLE_HIDE_BOSS
    if (t > 610 && t < 710) in->stick_x = -80;
#endif
    if (t == 620) pressed->d_up = 1;
    if (t == 150 || t == 360 || t == 620) debugf("[skill] press at frame %d\n", frame);
    return;
#endif
    if (frame > 200 && frame < 2000 && frame % 60 == 0) {
        pressed->a = 1; in->btn.a = 1;
        pressed->start = 1; in->btn.start = 1;
    }
    if (frame > 2400 && frame < 2600) in->stick_y = 80;   // walk forward a bit
    if (frame > 2600 && frame % 45 == 0) { pressed->b = 1; in->btn.b = 1; }   // attack
}
#endif
#if defined(HLE_LOG_HITVOL) || defined(HLE_LOG_HITGAP)
#include <t3d/t3dmodel.h>
#include <t3d/t3dskeleton.h>
#include "utilities/game_math.h"
// test aid: a skinned mesh's vertices in the world (which: 0 the body, swords left out; 1 the swords only)
int hle_mesh_points(const T3DModel *m, const T3DSkeleton *sk, const T3DMat4FP *M, int which, float (*out)[3], int max)
{
    int n = 0;
    if (!m || !sk || !M) return 0;
    T3DModelIter it = t3d_model_iter_create(m, T3D_CHUNK_TYPE_OBJECT);
    while (t3d_model_iter_next(&it)) {
        const T3DObject *ob = it.object;
        const char *mn = ob->material && ob->material->name ? ob->material->name : "";
        bool sw = strstr(mn, "sword") != NULL;
        if (strstr(mn, "a_black") || sw != (which == 1)) continue;
        for (int p = 0; p < ob->numParts; p++) {
            const T3DObjectPart *pt = &ob->parts[p];
            if (pt->matrixIdx >= sk->skeletonRef->boneCount) continue;
            const T3DMat4FP *B = &sk->boneMatricesFP[pt->matrixIdx];
            for (int v = 0; v < pt->vertLoadCount && n < max; v++) {
                const int16_t *q = (v & 1) ? pt->vert[v >> 1].posB : pt->vert[v >> 1].posA;
                float l[3] = { q[0], q[1], q[2] }, mm[3];
                mat4fp_mul_point_f32_row3_colbasis(B, l, mm);
                mat4fp_mul_point_f32_row3_colbasis(M, mm, out[n++]);
            }
        }
    }
    return n;
}
// test aid: a blade as drawn: the segment between its two furthest vertices, and how far its other vertices stand off it
bool hle_points_blade(const float (*p)[3], int n, float a[3], float b[3], float *hw)
{
    if (n < 2) return false;
    int i0 = 0, i1 = 1; float best = -1.0f;
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) {
        float dx = p[i][0] - p[j][0], dy = p[i][1] - p[j][1], dz = p[i][2] - p[j][2], d = dx * dx + dy * dy + dz * dz;
        if (d > best) { best = d; i0 = i; i1 = j; }
    }
    memcpy(a, p[i0], 12); memcpy(b, p[i1], 12);
    extern float hle_pt_seg(const float q[3], const float a[3], const float b[3]);
    // its half-width: the median vertex's distance off the segment (the guard's few, much wider, left out)
    enum { NW = 256 }; float d[NW]; int m = 0;
    for (int i = 0; i < n && m < NW; i++) { float v = hle_pt_seg(p[i], a, b), t = v; int j = m++; while (j > 0 && d[j - 1] > t) { d[j] = d[j - 1]; j--; } d[j] = t; }
    *hw = d[m / 2];
#ifdef HLE_LOG_HITGAP
    { static int said; if (said++ < 6) { float l = sqrtf((a[0]-b[0])*(a[0]-b[0]) + (a[1]-b[1])*(a[1]-b[1]) + (a[2]-b[2])*(a[2]-b[2]));
        debugf("[hgblade] %d vertices, %.1f long, off its line: p25 %.1f p50 %.1f p75 %.1f max %.1f\n", n, (double)l, (double)d[m / 4], (double)d[m / 2], (double)d[m * 3 / 4], (double)d[m - 1]); } }
#endif
    return true;
}
float hle_pt_seg(const float q[3], const float a[3], const float b[3])     // test aid: a point's distance from a segment
{
    float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, aq[3] = { q[0] - a[0], q[1] - a[1], q[2] - a[2] };
    float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2], t = l2 > 1e-6f ? (aq[0] * ab[0] + aq[1] * ab[1] + aq[2] * ab[2]) / l2 : 0.0f;
    t = fminf(1.0f, fmaxf(0.0f, t));
    float d[3] = { aq[0] - ab[0] * t, aq[1] - ab[1] * t, aq[2] - ab[2] * t };
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}
#ifdef HLE_LOG_JUMPBODY
// test aid: each player swing while the Guardian/Unbound leaps (combo attack, flip attack): did it land, and did the drawn blade touch the drawn body
void hle_jb_swing(int idx, float t, float t0, float t1, bool hit, const float base[3], const float tip[3])
{
    static int lastIdx = -1, st; static float lastT = 1e9f, gap = 1e9f, bodyLo, bodyHi, capLo, capHi, bladeLo, bladeHi, at; static bool any, wasHit, hitLimb; static float hitGap, hitBodyLo, hitCapLo, hitBladeHi;
    Boss *bb = boss_get_instance();
    bool jump = bb && bb->phaseIndex <= 2 && bb->mode == BOSS_MODE_COMBAT && (bb->state == BOSS_STATE_COMBO_ATTACK || bb->state == BOSS_STATE_FLIP_ATTACK);
    bool fresh = idx != lastIdx || t < lastT;
    if ((fresh || idx < 0) && any) {
        debugf("[jbswing] boss %d st %d t %.2f: %s | drawn blade to drawn body %.1f | body %.0f..%.0f, blade %.0f..%.0f, capsule %.0f..%.0f (over the floor, at the closest)", bb ? bb->phaseIndex : 0, st, (double)at,
               wasHit ? "HIT" : "miss", (double)gap, (double)bodyLo, (double)bodyHi, (double)bladeLo, (double)bladeHi, (double)capLo, (double)capHi);
        if (wasHit) debugf(" | at the hit: gap %.1f, body from %.0f, capsule from %.0f, blade up to %.0f%s", (double)hitGap, (double)hitBodyLo, (double)hitCapLo, (double)hitBladeHi, hitLimb ? ", a limb" : "");
        debugf("\n");
        any = false;
    }
    if (fresh) { gap = 1e9f; wasHit = false; }
    lastIdx = idx; lastT = t;
    if (idx < 0 || !jump || t <= t0 || t >= t1) return;
    if (!any && hit) return;     // it landed before the leap began
    enum { NP = 1400 };
    static float pts[NP][3], sw[64][3];
    extern T3DModel *characterModel;
    int n = hle_mesh_points(bb->model, bb->skeleton, bb->modelMat, 0, pts, NP);
    int ns = hle_mesh_points(characterModel, character.skeleton, character.modelMat, 1, sw, 64);
    float a[3], b[3], hw;
    if (!hle_points_blade((const float (*)[3])sw, ns, a, b, &hw)) { memcpy(a, base, 12); memcpy(b, tip, 12); hw = 1.0f; }
    float g = 1e9f, lo = 1e9f, hi = -1e9f, fl = character.pos[1];
    for (int i = 0; i < n; i++) { g = fminf(g, hle_pt_seg(pts[i], a, b) - hw); lo = fminf(lo, pts[i][1]); hi = fmaxf(hi, pts[i][1]); }
    {   // every frame of a swing's live part in the leap: the sword's hit test against the drawn blade meeting the drawn body
        extern bool charWeaponCollision; static int nf, both, air, missed, n0;
        nf++; if (charWeaponCollision && g <= 1.5f) both++; else if (charWeaponCollision && g > 3.0f) air++; else if (!charWeaponCollision && g <= 1.5f) missed++;
        if (nf - n0 >= 60) { n0 = nf; debugf("[jbframes] %d frames: the sword's test meets it and the drawn blade touches the body %d | meets it with the blade >3 off the body (air) %d | the blade touches the body, the test misses %d\n", nf, both, air, missed); }
    }
    if (hit && !wasHit) {     // the frame it landed
        extern bool hle_limb_only;
        hitGap = g; hitLimb = hle_limb_only; hitBodyLo = lo - fl;
        hitCapLo = bb->pos[1] + bb->capsuleCollider.localCapA.v[1] - bb->capsuleCollider.radius - fl;
        hitBladeHi = fmaxf(a[1], b[1]) - fl;
    }
    if (g < gap) {
        gap = g; st = bb->state; at = bb->stateTimer; bodyLo = lo - fl; bodyHi = hi - fl;
        bladeLo = fminf(a[1], b[1]) - fl; bladeHi = fmaxf(a[1], b[1]) - fl;
        capLo = bb->pos[1] + bb->capsuleCollider.localCapA.v[1] - bb->capsuleCollider.radius - fl;
        capHi = bb->pos[1] + bb->capsuleCollider.localCapB.v[1] + bb->capsuleCollider.radius - fl;
    }
    any = true;
    if (hit) wasHit = true;
}
#endif
bool hle_body_yspan(const T3DModel *m, const T3DSkeleton *sk, const T3DMat4FP *M, float out[2])     // test aid: the drawn body's lowest and highest skinned vertex (world y; swords left out)
{
    if (!m || !sk || !M) return false;
    out[0] = 1e9f; out[1] = -1e9f;
    T3DModelIter it = t3d_model_iter_create(m, T3D_CHUNK_TYPE_OBJECT);
    while (t3d_model_iter_next(&it)) {
        const T3DObject *ob = it.object;
        const char *mn = ob->material && ob->material->name ? ob->material->name : "";
        if (strstr(mn, "sword") || strstr(mn, "a_black")) continue;
        for (int p = 0; p < ob->numParts; p++) {
            const T3DObjectPart *pt = &ob->parts[p];
            if (pt->matrixIdx >= sk->skeletonRef->boneCount) continue;
            const T3DMat4FP *B = &sk->boneMatricesFP[pt->matrixIdx];
            for (int v = 0; v < pt->vertLoadCount; v++) {
                const int16_t *q = (v & 1) ? pt->vert[v >> 1].posB : pt->vert[v >> 1].posA;
                float l[3] = { q[0], q[1], q[2] }, mm[3], w[3];
                mat4fp_mul_point_f32_row3_colbasis(B, l, mm);
                mat4fp_mul_point_f32_row3_colbasis(M, mm, w);
                out[0] = fminf(out[0], w[1]); out[1] = fmaxf(out[1], w[1]);
            }
        }
    }
    return out[1] > out[0];
}
/** Every skinned vertex (swords and the summoned swords left out) in world space, from o: the
 *  widest (horizontal) below h above o of the legs, torso, arms and head; the top; the furthest
 *  in front of yaw (facing (-sin, cos)) and to its side */
bool hle_silhouette(const T3DModel *m, const T3DSkeleton *sk, const T3DMat4FP *M, const float o[3], float yaw, float h, float out[7])
{
    if (!m || !sk || !M) return false;
    float fx = -fm_sinf(yaw), fz = fm_cosf(yaw);
    for (int i = 0; i < 7; i++) out[i] = 0.0f;
    T3DModelIter it = t3d_model_iter_create(m, T3D_CHUNK_TYPE_OBJECT);
    while (t3d_model_iter_next(&it)) {
        const T3DObject *ob = it.object;
        const char *mn = ob->material && ob->material->name ? ob->material->name : "";
        static int said;
        if ((!strcmp(mn, "sword") || !strcmp(mn, "z_sword1")) && said < 2) {     // the blade's ends, in the hand's space (the hit segments are set from these)
            said++;
            for (int p = 0; p < ob->numParts; p++) {
                const T3DObjectPart *pt = &ob->parts[p];
                int lo = 0, hi = 0; const int16_t *L = NULL, *H = NULL;
                for (int v = 0; v < pt->vertLoadCount; v++) {
                    const int16_t *q = (v & 1) ? pt->vert[v >> 1].posB : pt->vert[v >> 1].posA;
                    if (!L || q[0] < lo) { lo = q[0]; L = q; }
                    if (!H || q[0] > hi) { hi = q[0]; H = q; }
                }
                if (L) debugf("[hitvol] %s part %d bone %d: x from %d,%d,%d to %d,%d,%d\n", mn, p, pt->matrixIdx, L[0], L[1], L[2], H[0], H[1], H[2]);
            }
        }
        if (strstr(mn, "sword") || strstr(mn, "a_black")) continue;
        for (int p = 0; p < ob->numParts; p++) {
            const T3DObjectPart *pt = &ob->parts[p];
            if (pt->matrixIdx >= sk->skeletonRef->boneCount) continue;
            const char *bn = sk->skeletonRef->bones[pt->matrixIdx].name;
            int c = (strstr(bn, "Leg") || strstr(bn, "Foot")) ? 0 : (strstr(bn, "Arm") || strstr(bn, "Hand") || strstr(bn, "Finger") || strstr(bn, "Thumb")) ? 2 : strstr(bn, "Head") ? 3 : 1;
            const T3DMat4FP *B = &sk->boneMatricesFP[pt->matrixIdx];
            for (int v = 0; v < pt->vertLoadCount; v++) {
                const T3DVertPacked *vp = &pt->vert[v >> 1];
                const int16_t *q = (v & 1) ? vp->posB : vp->posA;
                float l[3] = { q[0], q[1], q[2] }, mm[3], w[3];
                mat4fp_mul_point_f32_row3_colbasis(B, l, mm);
                mat4fp_mul_point_f32_row3_colbasis(M, mm, w);
                float dx = w[0] - o[0], dz = w[2] - o[2], dy = w[1] - o[1], r = sqrtf(dx * dx + dz * dz);
                if (dy < h) out[c] = fmaxf(out[c], r);
                out[4] = fmaxf(out[4], dy);
                out[5] = fmaxf(out[5], dx * fx + dz * fz); out[6] = fmaxf(out[6], fabsf(dx * fz - dz * fx));
            }
        }
    }
    return true;
}
#endif
#ifdef HLE_LOG_HITGAP
// Test aid: each blow on the player, the gap between what was drawn when it landed (a blade, fist,
// body, fire, beam...) and the player's drawn body (every skinned vertex, the sword left out),
// against the same thing's gap to the player's hit capsule, and what a smaller capsule would change.
// [hg] each blow as it lands; [hgw] each blow's live window as it ends: whether it landed and, for the
// hurtboxes tried (v0 as it is, 8 round; r6; r5; r6 and r5 with the top lowered while rolling or down),
// whether its volume reached it. A volume that doesn't use the player's capsule (a distance from his
// middle) is "fixed": every hurtbox gives the same answer
#include "character.h"
#include "game/boss/boss.h"
extern T3DModel *characterModel;
extern float hle_pt_seg(const float q[3], const float a[3], const float b[3]);
extern int hle_mesh_points(const T3DModel *m, const T3DSkeleton *sk, const T3DMat4FP *M, int which, float (*out)[3], int max);
extern bool hle_points_blade(const float (*p)[3], int n, float a[3], float b[3], float *hw);
int hle_hg_frame;
#ifndef HLE_HG_LOW_TOP
#define HLE_HG_LOW_TOP 14.0f    // the capsule's top over the floor while rolling or down, in the what-ifs
#endif
enum { HG_NP = 800, HG_NV = 5, HG_SLOTS = 96 };
static float hgP[HG_NP][3]; static int hgN, hgAt = -1;
static float hgMesh, hgCap; static const char *hgWhat;
static const char *hgPartN[6]; static float hgPartG[6]; static int hgParts;     // each drawn part's own gap
static void hg_part(const char *what, float g) { for (int i = 0; i < hgParts; i++) if (hgPartN[i] == what) { hgPartG[i] = fminf(hgPartG[i], g); return; } if (hgParts < 6) { hgPartN[hgParts] = what; hgPartG[hgParts++] = g; } }
static void hg_player(void)
{
    if (hgAt == hle_hg_frame) return;
    hgAt = hle_hg_frame;
    hgN = hle_mesh_points(characterModel, character.skeleton, character.modelMat, 0, hgP, HG_NP);
    static int said;     // how far apart its vertices are (a gap between two meshes' vertices reads up to about half of each mesh's spacing over the surfaces' own)
    if (said++ == 600) {
        static float nn[HG_NP]; int m = 0;
        for (int i = 0; i < hgN; i++) { float b2 = 1e18f; for (int j = 0; j < hgN; j++) if (j != i) { float dx = hgP[i][0]-hgP[j][0], dy = hgP[i][1]-hgP[j][1], dz = hgP[i][2]-hgP[j][2], d2 = dx*dx+dy*dy+dz*dz; if (d2 > 1e-4f && d2 < b2) b2 = d2; } nn[m++] = sqrtf(b2); }
        for (int i = 1; i < m; i++) { float v = nn[i]; int j = i - 1; while (j >= 0 && nn[j] > v) { nn[j + 1] = nn[j]; j--; } nn[j + 1] = v; }
        debugf("[hgmesh] the player: %d vertices, nearest neighbour p50 %.1f p90 %.1f max %.1f\n", hgN, (double)nn[m / 2], (double)nn[m * 9 / 10], (double)nn[m - 1]);
    }
}
static float hg_segseg(const float p1[3], const float q1[3], const float p2[3], const float q2[3])     // between two segments
{
    float d1[3], d2[3], r[3];
    for (int i = 0; i < 3; i++) { d1[i] = q1[i] - p1[i]; d2[i] = q2[i] - p2[i]; r[i] = p1[i] - p2[i]; }
    float a = d1[0]*d1[0] + d1[1]*d1[1] + d1[2]*d1[2], e = d2[0]*d2[0] + d2[1]*d2[1] + d2[2]*d2[2], f = d2[0]*r[0] + d2[1]*r[1] + d2[2]*r[2];
    float s = 0.0f, t = 0.0f;
    if (a < 1e-6f && e < 1e-6f) { } 
    else if (a < 1e-6f) t = fminf(1.0f, fmaxf(0.0f, f / e));
    else {
        float c = d1[0]*r[0] + d1[1]*r[1] + d1[2]*r[2];
        if (e < 1e-6f) s = fminf(1.0f, fmaxf(0.0f, -c / a));
        else {
            float b = d1[0]*d2[0] + d1[1]*d2[1] + d1[2]*d2[2], den = a * e - b * b;
            s = den > 1e-6f ? fminf(1.0f, fmaxf(0.0f, (b * f - c * e) / den)) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) { t = 0.0f; s = fminf(1.0f, fmaxf(0.0f, -c / a)); }
            else if (t > 1.0f) { t = 1.0f; s = fminf(1.0f, fmaxf(0.0f, (b - c) / a)); }
        }
    }
    float x[3]; for (int i = 0; i < 3; i++) x[i] = p1[i] + d1[i] * s - (p2[i] + d2[i] * t);
    return sqrtf(x[0]*x[0] + x[1]*x[1] + x[2]*x[2]);
}
static bool hg_low(void) { CharacterState st = character_get_state(); return st == CHAR_STATE_ROLLING || st == CHAR_STATE_KNOCKDOWN; }
// The player's capsule: v 0 as the game has it; 1/2 radius 6/5 with the same top (20); 3/4 those with
// the top at HLE_HG_LOW_TOP while rolling or down
static float hg_axis(int v, float a[3], float b[3])
{
    const float *p = character.pos;
    if (v == 0) {
        for (int i = 0; i < 3; i++) { a[i] = p[i] + character.capsuleCollider.localCapA.v[i]; b[i] = p[i] + character.capsuleCollider.localCapB.v[i]; }
        return character.capsuleCollider.radius;
    }
    float r = (v == 1 || v == 3) ? 6.0f : 5.0f, top = (v >= 3 && hg_low()) ? HLE_HG_LOW_TOP : 20.0f;
    a[0] = b[0] = p[0]; a[2] = b[2] = p[2];
    b[1] = p[1] + top - r; a[1] = fminf(p[1] + 4.0f, b[1]);
    return r;
}
typedef struct { const char *name; const void *key; int last, hits; float m[HG_NV], mesh, cap, top; int st; bool fixed; } HgWin;
static const void *hgKey;
void hle_hg_key(const void *k) { hgKey = k; }     // the next call's blow is this one (an effect among several of one name)
static HgWin hgW[HG_SLOTS];
static void hg_flush(HgWin *w)
{
    Boss *bb = boss_get_instance();
    debugf("[hgw] boss %d %s | %s | reach", bb ? bb->phaseIndex : 0, w->name, w->hits ? "LANDED" : "missed");
    for (int v = 0; v < HG_NV; v++) debugf(" %.1f", (double)w->m[v]);
    debugf(" | hurtboxes it reaches:");
    for (int v = 0; v < HG_NV; v++) debugf(" %d", w->m[v] <= 0.0f);
    if (w->hits) debugf(" | drawn gap %.1f, to the capsule %.1f, player state %d top %.1f", (double)w->mesh, (double)w->cap, w->st, (double)w->top);
    debugf("%s\n", w->fixed ? " | fixed" : "");
    w->name = NULL;
}
void hle_hg_tick(void)     // a window not seen for 4 frames is over
{
#ifdef HLE_HG_POSE     // the drawn body's top and reach off the axis, every 6th frame out of the normal stance (rolls, knockdowns)
    if (hle_hg_frame % 6 == 0 && character.skeleton && character_get_state() != CHAR_STATE_NORMAL) {
        hg_player(); float top = -1e9f, out = 0.0f;
        for (int i = 0; i < hgN; i++) { float dx = hgP[i][0] - character.pos[0], dz = hgP[i][2] - character.pos[2]; top = fmaxf(top, hgP[i][1] - character.pos[1]); out = fmaxf(out, sqrtf(dx * dx + dz * dz)); }
        debugf("[pose] f%d st %d top %.1f out %.1f\n", hle_hg_frame, character_get_state(), (double)top, (double)out);
    }
#endif
    for (int i = 0; i < HG_SLOTS; i++) if (hgW[i].name && hle_hg_frame - hgW[i].last > 4) hg_flush(&hgW[i]);
}
static HgWin *hg_win(const char *name)
{
    HgWin *free = NULL; const void *key = hgKey; hgKey = NULL;
    for (int i = 0; i < HG_SLOTS; i++) { if (hgW[i].name == name && hgW[i].key == key) return &hgW[i]; if (!hgW[i].name && !free) free = &hgW[i]; }
    if (!free) return NULL;
    memset(free, 0, sizeof *free);
    free->name = name; free->key = key;
    for (int v = 0; v < HG_NV; v++) free->m[v] = 1e9f;
    return free;
}
// A blow's volume this frame: the capsule a-b (a ball when a == b), radius r, tested against the player's capsule
void hle_hg_vol(const char *name, const float a[3], const float b[3], float r)
{
    HgWin *w = hg_win(name); if (!w) return;
    w->last = hle_hg_frame;
    for (int v = 0; v < HG_NV; v++) { float pa[3], pb[3], pr = hg_axis(v, pa, pb); w->m[v] = fminf(w->m[v], hg_segseg(a, b, pa, pb) - r - pr); }
}
// A blow whose volume is a distance from the player's middle (or a floor area): inside or not, any capsule
void hle_hg_fixed(const char *name, bool inside)
{
    HgWin *w = hg_win(name); if (!w) return;
    w->last = hle_hg_frame; w->fixed = true;
    for (int v = 0; v < HG_NV; v++) w->m[v] = fminf(w->m[v], inside ? -1.0f : 1.0f);
}
// What is drawn as the blow lands, added up from its parts: segments (balls when a == b) of radius r,
void hle_hg_begin(void) { hg_player(); hgMesh = hgCap = 1e9f; hgWhat = ""; hgParts = 0; }
void hle_hg_seg(const char *what, const float a[3], const float b[3], float r)
{
    float pa[3], pb[3], pr = hg_axis(0, pa, pb), g = 1e9f;
    for (int i = 0; i < hgN; i++) g = fminf(g, hle_pt_seg(hgP[i], a, b));
    g -= r;
    hg_part(what, g);
    if (g < hgMesh) { hgMesh = g; hgWhat = what; }
    hgCap = fminf(hgCap, hg_segseg(a, b, pa, pb) - r - pr);
}
// a skinned mesh's vertices (which 0: the body; 1: the swords), as points: the nearest pair
void hle_hg_mesh(const char *what, const T3DModel *m, const T3DSkeleton *sk, const T3DMat4FP *M, int which)
{
    enum { NB = 1400 };
    static float q[NB][3];
    int n = hle_mesh_points(m, sk, M, which, q, NB);
    { static int said[2]; if (said[which & 1]++ == 3) {
        static float nn[NB]; int c = 0;
        for (int i = 0; i < n; i++) { float b2 = 1e18f; for (int j = 0; j < n; j++) if (j != i) { float dx = q[i][0]-q[j][0], dy = q[i][1]-q[j][1], dz = q[i][2]-q[j][2], d2 = dx*dx+dy*dy+dz*dz; if (d2 > 1e-4f && d2 < b2) b2 = d2; } nn[c++] = sqrtf(b2); }
        for (int i = 1; i < c; i++) { float v = nn[i]; int j = i - 1; while (j >= 0 && nn[j] > v) { nn[j + 1] = nn[j]; j--; } nn[j + 1] = v; }
        if (c) debugf("[hgmesh] %s (%s): %d vertices, nearest neighbour p50 %.1f p90 %.1f max %.1f\n", what, which ? "swords" : "body", c, (double)nn[c / 2], (double)nn[c * 9 / 10], (double)nn[c - 1]); } }
    float pa[3], pb[3], pr = hg_axis(0, pa, pb), g2 = 1e18f, c = 1e9f, out = 0.0f;
    for (int j = 0; j < hgN; j++) out = fmaxf(out, hle_pt_seg(hgP[j], pa, pb));     // the player's drawn body reaches this far off his axis
    for (int i = 0; i < n; i++) {
        float da = hle_pt_seg(q[i], pa, pb);
        c = fminf(c, da - pr);
        if (da - out > 0.0f && (da - out) * (da - out) >= g2) continue;     // (no nearer than that to any of his vertices)
        for (int j = 0; j < hgN; j++) {
            float dx = q[i][0] - hgP[j][0], dy = q[i][1] - hgP[j][1], dz = q[i][2] - hgP[j][2], d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < g2) g2 = d2;
        }
    }
    float g = sqrtf(g2);
    hg_part(what, g);
    if (g < hgMesh) { hgMesh = g; hgWhat = what; }
    hgCap = fminf(hgCap, c);
}
// a gap worked out by the caller (Shadowborne's drawn shapes): to the mesh, to the capsule
void hle_hg_gaps(const char *what, float mesh, float cap) { hg_part(what, mesh); if (mesh < hgMesh) { hgMesh = mesh; hgWhat = what; } hgCap = fminf(hgCap, cap); }
float hle_hg_mesh_seg(const float a[3], const float b[3])     // the player's drawn body from a segment
{
    hg_player();
    float g = 1e9f;
    for (int i = 0; i < hgN; i++) g = fminf(g, hle_pt_seg(hgP[i], a, b));
    return g;
}
void hle_hg_hit(const char *name)
{
    float top = -1e9f;
    for (int i = 0; i < hgN; i++) top = fmaxf(top, hgP[i][1] - character.pos[1]);
    Boss *bb = boss_get_instance();
    debugf("[hg] boss %d %s | drawn %s gap %.1f | to the capsule %.1f | player state %d top %.1f | f%d | parts", bb ? bb->phaseIndex : 0, name, hgWhat, (double)hgMesh, (double)hgCap, character_get_state(), (double)top, hle_hg_frame);
    for (int i = 0; i < hgParts; i++) debugf(" %s %.1f", hgPartN[i], (double)hgPartG[i]);
    debugf("\n");
    HgWin *w = hg_win(name); if (!w) return;
    if (!w->last) { w->last = hle_hg_frame; }
    if (!w->hits) { w->mesh = hgMesh; w->cap = hgCap; w->st = character_get_state(); w->top = top; }
    w->hits++;
}
#endif
#ifdef HLE_LOG_HITGAP
static const char *const hgG[2][20] = {
    { "G 0", "G 1", "G 2", "G 3", "G 4", "G 5", "G 6", "G lunge (blade)", "G power jump (blade)", "G combo (blade)", "G combo starter (blade)", "G tracking slam (blade)", "G flip (blade)", "G lunge starter (blade)", "G stomp (blade)", "G attack1 (blade)", "G 16", "G 17", "G 18", "G 19" },
    { "U 0", "U 1", "U 2", "U 3", "U 4", "U 5", "U 6", "U lunge (blade)", "U power jump (blade)", "U combo (blade)", "U combo starter (blade)", "U tracking slam (blade)", "U flip (blade)", "U lunge starter (blade)", "U stomp (blade)", "U attack1 (blade)", "U barrage (blade)", "U sweep (blade)", "U 18", "U 19" } };
const char *hle_hg_gname(const Boss *b) { return hgG[b->phaseIndex == 2][(unsigned)b->state < 20 ? b->state : 19]; }
const char *hle_hg_gsphere; int hle_hg_gsphereAt;     // this frame's ball (power jump, stomp, flip burst), if that's what landed
// test aid: the Guardian's sword blade (z_sword1 and its decal past the guard, and the flared point) as its plane
// sampled every 0.75 (tools: /tmp/gb), in the Hand-Right bone's space: the mesh has vertices only at the
// guard and the point, so its vertices read the middle of the blade as far as 5 off
static const int16_t HG_GBLADE[][3] = {
    {-781,-74,-76}, {-777,-63,-76}, {-773,-51,-77}, {-770,-71,-75}, {-770,-50,-77}, {-770,-40,-77}, {-769,-62,-76}, {-766,-29,-78}, {-762,-18,-79}, {-759,-68,-74},
    {-759,-25,-78}, {-759,-13,-79}, {-758,-61,-74}, {-758,-49,-76}, {-758,-37,-77}, {-758,-6,-79}, {-754,5,-80}, {-751,16,-80}, {-748,-65,-73}, {-748,11,-80},
    {-748,23,-81}, {-747,-25,-77}, {-747,-13,-78}, {-747,-1,-79}, {-747,27,-81}, {-746,-61,-73}, {-746,-49,-74}, {-746,-37,-76}, {-743,39,-82}, {-739,50,-82},
    {-737,-62,-73}, {-737,35,-81}, {-737,47,-82}, {-736,0,-78}, {-736,12,-79}, {-736,24,-80}, {-735,-24,-76}, {-735,-12,-77}, {-735,61,-83}, {-734,-60,-72},
    {-734,-48,-73}, {-734,-36,-74}, {-732,73,-83}, {-728,84,-84}, {-726,-59,-72}, {-726,72,-83}, {-726,84,-84}, {-725,36,-80}, {-725,48,-81}, {-725,60,-82},
    {-724,0,-77}, {-724,12,-78}, {-724,24,-79}, {-724,95,-85}, {-723,-24,-74}, {-723,-12,-76}, {-722,-48,-72}, {-722,-36,-73}, {-715,-56,-71}, {-714,60,-81},
    {-714,72,-82}, {-714,84,-83}, {-714,91,-83}, {-713,36,-79}, {-713,48,-80}, {-712,1,-76}, {-712,12,-77}, {-712,24,-78}, {-711,-35,-72}, {-711,-23,-73},
    {-711,-11,-74}, {-710,-47,-71}, {-704,-53,-70}, {-703,88,-82}, {-702,61,-80}, {-702,73,-81}, {-702,85,-82}, {-701,37,-78}, {-701,49,-79}, {-700,1,-74},
    {-700,13,-76}, {-700,25,-77}, {-699,-35,-71}, {-699,-23,-72}, {-699,-11,-73}, {-698,-47,-70}, {-693,-50,-70}, {-693,84,-81}, {-690,61,-79}, {-690,73,-80},
    {-689,25,-76}, {-689,37,-77}, {-689,49,-78}, {-688,1,-73}, {-688,13,-74}, {-687,-34,-70}, {-687,-22,-71}, {-687,-10,-72}, {-686,-46,-69}, {-682,-47,-69},
    {-682,81,-80}, {-678,62,-78}, {-678,74,-79}, {-677,26,-74}, {-677,38,-76}, {-677,50,-77}, {-676,2,-72}, {-676,14,-73}, {-675,-34,-69}, {-675,-22,-70},
    {-675,-10,-71}, {-672,77,-78}, {-671,-44,-68}, {-666,62,-77}, {-666,74,-78}, {-665,26,-73}, {-665,38,-74}, {-665,50,-76}, {-664,2,-71}, {-664,14,-72},
    {-663,-33,-68}, {-663,-21,-69}, {-663,-10,-70}, {-662,74,-77}, {-660,-40,-67}, {-654,63,-76}, {-653,27,-72}, {-653,39,-73}, {-653,51,-74}, {-652,-9,-69},
    {-652,3,-70}, {-652,15,-71}, {-651,-33,-67}, {-651,-21,-68}, {-651,70,-76}, {-649,-37,-67}, {-642,63,-74}, {-641,27,-71}, {-641,39,-72}, {-641,51,-73},
    {-641,67,-75}, {-640,-9,-68}, {-640,3,-69}, {-640,14,-70}, {-640,15,-70}, {-639,-32,-66}, {-639,-21,-67}, {-638,-34,-66}, {-632,21,-70}, {-631,7,-69},
    {-630,52,-72}, {-630,63,-73}, {-629,28,-70}, {-629,40,-71}, {-628,-8,-67}, {-628,4,-68}, {-628,12,-69}, {-628,16,-69}, {-628,24,-70}, {-627,-31,-65},
    {-627,-20,-66}, {-624,29,-70}, {-622,0,-67}, {-620,59,-72}, {-618,52,-71}, {-617,28,-69}, {-617,40,-70}, {-616,-28,-64}, {-616,-8,-66}, {-616,4,-67},
    {-616,13,-68}, {-616,16,-68}, {-616,25,-69}, {-615,-20,-65}, {-615,1,-67}, {-615,37,-70}, {-613,-8,-66}, {-610,56,-71}, {-607,45,-70}, {-606,53,-70},
    {-605,-25,-64}, {-605,17,-67}, {-605,29,-68}, {-605,37,-69}, {-605,41,-69}, {-604,-15,-64}, {-604,-7,-65}, {-604,1,-66}, {-604,5,-66}, {-604,13,-67},
    {-604,25,-68}, {-603,-19,-64}, {-603,-11,-65}, {-599,52,-70}, {-597,51,-69}, {-596,39,-68}, {-595,-22,-63}, {-595,28,-67}, {-594,-22,-63}, {-594,-13,-64},
    {-594,-3,-65}, {-594,6,-65}, {-594,16,-66}, {-593,38,-68}, {-593,50,-69}, {-592,2,-65}, {-592,14,-66}, {-592,26,-67}, {-591,-10,-64}, {-587,53,-69},
    {-582,-22,-62}, {-581,38,-67}, {-581,50,-68}, {-580,2,-64}, {-580,14,-65}, {-580,26,-66}, {-579,-10,-63}, {-576,53,-68}, {-570,-21,-61}, {-569,39,-66},
    {-569,51,-67}, {-568,3,-63}, {-568,15,-64}, {-568,27,-65}, {-567,-9,-62}, {-564,53,-67}, {-558,-21,-60}, {-557,27,-64}, {-557,39,-65}, {-557,51,-66},
    {-556,3,-62}, {-556,15,-63}, {-555,-9,-61}, {-552,54,-66}, {-547,-20,-59}, {-545,28,-63}, {-545,39,-64}, {-545,51,-65}, {-544,4,-61}, {-544,16,-62},
    {-543,-8,-60}, {-540,54,-64}, {-535,-19,-58}, {-533,28,-62}, {-533,40,-63}, {-533,52,-64}, {-532,4,-60}, {-532,16,-61}, {-531,-8,-58}, {-528,54,-63},
    {-523,-19,-57}, {-521,28,-61}, {-521,40,-62}, {-521,52,-63}, {-520,-7,-57}, {-520,5,-58}, {-520,17,-60}, {-516,55,-62}, {-511,-18,-56}, {-509,29,-60},
    {-509,41,-61}, {-509,53,-62}, {-508,-7,-56}, {-508,5,-57}, {-508,17,-58}, {-504,55,-61}, {-500,-18,-55}, {-498,53,-61}, {-497,29,-58}, {-497,41,-60},
    {-496,-6,-55}, {-496,6,-56}, {-496,17,-57}, {-493,55,-60}, {-488,-17,-54}, {-486,54,-60}, {-485,30,-57}, {-485,42,-58}, {-484,-6,-54}, {-484,6,-55},
    {-484,18,-56}, {-481,56,-59}, {-476,-17,-53}, {-474,54,-58}, {-473,18,-55}, {-473,30,-56}, {-473,42,-57}, {-472,-5,-53}, {-472,6,-54}, {-469,56,-58},
    {-464,-16,-52}, {-462,55,-57}, {-461,19,-54}, {-461,31,-55}, {-461,43,-56}, {-460,-5,-52}, {-460,7,-53}, {-457,56,-57}, {-453,-15,-51}, {-450,55,-56},
    {-449,19,-53}, {-449,31,-54}, {-449,43,-55}, {-448,-5,-51}, {-448,7,-52}, {-445,57,-56}, {-441,-15,-50}, {-438,56,-55}, {-437,20,-52}, {-437,32,-53},
    {-437,44,-54}, {-436,-4,-50}, {-436,8,-51}, {-433,57,-55}, {-429,-14,-49}, {-426,56,-54}, {-425,20,-51}, {-425,32,-52}, {-425,44,-53}, {-424,-4,-49},
    {-424,8,-50}, {-421,57,-54}, {-417,-14,-48}, {-414,45,-52}, {-414,57,-53}, {-413,21,-50}, {-413,33,-51}, {-412,-3,-48}, {-412,9,-49}, {-410,58,-53},
    {-405,-13,-47}, {-402,45,-51}, {-402,57,-52}, {-401,21,-49}, {-401,33,-50}, {-400,-3,-47}, {-400,9,-48}, {-398,58,-52}, {-394,-13,-46}, {-390,46,-50},
    {-390,58,-51}, {-389,10,-47}, {-389,22,-48}, {-389,34,-49}, {-388,-2,-46}, {-386,58,-51}, {-382,-12,-45}, {-378,46,-49}, {-378,58,-50}, {-377,10,-46},
    {-377,22,-47}, {-377,34,-48}, {-376,-2,-45}, {-374,59,-50}, {-370,-11,-44}, {-366,47,-48}, {-366,58,-49}, {-365,11,-45}, {-365,23,-46}, {-365,35,-47},
    {-364,-1,-44}, {-362,59,-49}, {-358,-11,-43}, {-354,47,-47}, {-354,59,-48}, {-353,11,-44}, {-353,23,-45}, {-353,35,-46}, {-352,-1,-43}, {-350,59,-48},
    {-347,-10,-42}, {-342,47,-46}, {-342,59,-47}, {-341,12,-43}, {-341,24,-44}, {-341,36,-45}, {-340,0,-42}, {-338,60,-47}, {-335,-10,-41}, {-330,36,-44},
    {-330,48,-45}, {-330,60,-46}, {-329,12,-42}, {-329,24,-43}, {-328,0,-41}, {-327,60,-46}, {-323,-9,-39}, {-318,36,-43}, {-318,48,-44}, {-317,13,-41},
    {-317,25,-42}, {-316,1,-40}, {-315,60,-45}, {-311,-9,-38}, {-306,37,-42}, {-306,49,-43}, {-305,1,-39}, {-305,13,-40}, {-305,25,-41}, {-303,61,-44},
    {-299,-8,-37}, {-294,37,-41}, {-294,49,-42}, {-293,2,-38}, {-293,13,-39}, {-293,25,-40}, {-291,61,-43}, {-288,-7,-36}, {-282,38,-40}, {-282,50,-41},
    {-281,2,-37}, {-281,14,-38}, {-281,26,-39}, {-279,61,-41}, {-276,-7,-35}, {-270,38,-39}, {-270,50,-40}, {-269,2,-36}, {-269,14,-37}, {-269,26,-38},
    {-267,62,-40}, {-264,-6,-34}, {-258,39,-38}, {-258,51,-39}, {-257,3,-35}, {-257,15,-36}, {-257,27,-37}, {-255,62,-39}, {-252,-6,-33}, {-246,27,-36},
    {-246,39,-37}, {-246,51,-38}, {-245,3,-33}, {-245,15,-35}, {-244,62,-38}, {-241,-5,-32}, {-234,28,-35}, {-234,40,-36}, {-234,52,-37}, {-233,4,-32},
    {-233,16,-33}, {-232,63,-37}, {-229,-5,-31}, {-222,28,-33}, {-222,40,-35}, {-222,52,-36}, {-221,4,-31}, {-221,16,-32}, {-220,63,-36}, {-217,-4,-30},
    {-210,29,-32}, {-210,41,-33}, {-210,53,-34}, {-209,5,-30}, {-209,17,-31}, {-208,63,-35}, {-205,-3,-29}, {-199,53,-33}, {-198,29,-31}, {-198,41,-32},
    {-197,5,-29}, {-197,17,-30}, {-196,64,-34}, {-193,-3,-28}, {-187,54,-32}, {-186,30,-30}, {-186,42,-31}, {-185,6,-28}, {-185,18,-29}, {-184,64,-33},
    {-182,-2,-27}, {-175,54,-31}, {-174,30,-29}, {-174,42,-30}, {-173,6,-27}, {-173,18,-28}, {-172,64,-32}, {-170,-2,-26}, {-163,54,-30}, {-162,19,-27},
    {-162,31,-28}, {-162,43,-29}, {-161,7,-26}, {-161,65,-31}, {-158,-1,-25}, {-151,55,-29}, {-150,19,-26}, {-150,31,-27}, {-150,43,-28}, {-149,7,-25},
    {-149,65,-30}, {-146,-1,-24}, {-139,55,-28}, {-138,20,-25}, {-138,32,-26}, {-138,43,-27}, {-137,8,-24}, {-137,65,-29}, {-135,0,-23}, {-127,56,-27},
    {-126,20,-24}, {-126,32,-25}, {-126,44,-26}, {-125,8,-23}, {-125,66,-28}, {-123,1,-22}, {-115,44,-25}, {-115,56,-26}, {-114,21,-23}, {-114,32,-24},
    {-113,9,-22}, {-113,55,-26}, {-113,66,-27}, {-112,23,-23}, {-112,34,-24}, {-112,45,-25}, {-111,1,-21}, {-111,2,-21}, {-111,12,-22}
};
static void hg_gblade(const Boss *b)
{
    if (b->handRightBoneIndex < 0) return;
    const T3DMat4FP *B = &((T3DSkeleton *)b->skeleton)->boneMatricesFP[b->handRightBoneIndex];
    float pa[3], pb[3], pr = hg_axis(0, pa, pb), g2 = 1e18f, c = 1e9f;
    for (unsigned i = 0; i < sizeof HG_GBLADE / sizeof HG_GBLADE[0]; i++) {
        float l[3] = { HG_GBLADE[i][0], HG_GBLADE[i][1], HG_GBLADE[i][2] }, m[3], w[3];
        mat4fp_mul_point_f32_row3_colbasis(B, l, m); mat4fp_mul_point_f32_row3_colbasis((const T3DMat4FP *)b->modelMat, m, w);
        c = fminf(c, hle_pt_seg(w, pa, pb) - pr);
        for (int j = 0; j < hgN; j++) { float dx = w[0] - hgP[j][0], dy = w[1] - hgP[j][1], dz = w[2] - hgP[j][2], d2 = dx*dx + dy*dy + dz*dz; if (d2 < g2) g2 = d2; }
    }
    float g = sqrtf(g2);
    hg_part("blade", g);
    if (g < hgMesh) { hgMesh = g; hgWhat = "blade"; }
    hgCap = fminf(hgCap, c);
}
void hle_hg_ghit(void)      // the Guardian or Unbound landed a blow: the blade and the body as drawn
{
    Boss *b = boss_get_instance(); if (!b) return;
    hle_hg_begin();
    hg_gblade(b);     // the sword's blade, its plane sampled densely (its mesh has vertices only at the guard and the point)
    hle_hg_mesh("swords", b->model, b->skeleton, b->modelMat, 1);
    hle_hg_mesh("body", b->model, b->skeleton, b->modelMat, 0);
    hle_hg_hit(hle_hg_gsphere && hle_hg_gsphereAt == hle_hg_frame ? hle_hg_gsphere : hle_hg_gname(b));
}
#endif
#ifdef HLE_LOG_HITGAP
#include "utilities/simple_collision_utility.h"
static float hg_obb_d(const SCU_OBB *o, const float q[3], bool xz)     // a point's distance from a box turned by yaw about y
{
    float c = cosf(o->yaw), s = sinf(o->yaw), dx = q[0] - o->center[0], dy = q[1] - o->center[1], dz = q[2] - o->center[2];
    float lx = dx * c + dz * s, lz = -dx * s + dz * c;     // (either turn: the box is symmetric)
    float ex = fmaxf(0.0f, fabsf(lx) - o->half[0]), ey = xz ? 0.0f : fmaxf(0.0f, fabsf(dy) - o->half[1]), ez = fmaxf(0.0f, fabsf(lz) - o->half[2]);
    return sqrtf(ex * ex + ey * ey + ez * ez);
}
void hle_hg_obb_vol(const char *name, const SCU_OBB *o)     // a box on the floor tested in xz against the player's radius
{
    HgWin *w = hg_win(name); if (!w) return;
    w->last = hle_hg_frame;
    for (int v = 0; v < HG_NV; v++) { float pa[3], pb[3], pr = hg_axis(v, pa, pb); w->m[v] = fminf(w->m[v], hg_obb_d(o, pa, true) - pr); }
}
void hle_hg_obb(const char *what, const SCU_OBB *o)     // a box as drawn
{
    float pa[3], pb[3], pr = hg_axis(0, pa, pb), g = 1e9f, c = 1e9f;
    for (int i = 0; i < hgN; i++) g = fminf(g, hg_obb_d(o, hgP[i], false));
    for (int k = 0; k <= 8; k++) { float q[3]; for (int i = 0; i < 3; i++) q[i] = pa[i] + (pb[i] - pa[i]) * k / 8.0f; c = fminf(c, hg_obb_d(o, q, false) - pr); }
    hg_part(what, g);
    if (g < hgMesh) { hgMesh = g; hgWhat = what; }
    hgCap = fminf(hgCap, c);
}
#endif
#ifdef HLE_LOG_HITGAP
void hle_hg_vol_xz(const char *name, const float c[3], float R)     // a wave on the floor: the player's middle within R + his radius of c
{
    HgWin *w = hg_win(name); if (!w) return;
    w->last = hle_hg_frame;
    for (int v = 0; v < HG_NV; v++) { float pa[3], pb[3], pr = hg_axis(v, pa, pb), dx = pa[0] - c[0], dz = pa[2] - c[2]; w->m[v] = fminf(w->m[v], sqrtf(dx * dx + dz * dz) - R - pr); }
}
void hle_hg_ring(const char *what, const float c[3], float R, float yLo, float yHi)     // a ring on the floor as drawn: out to R, from yLo to yHi
{
    float pa[3], pb[3], pr = hg_axis(0, pa, pb), g = 1e9f;
    for (int i = 0; i < hgN; i++) {
        float dx = hgP[i][0] - c[0], dz = hgP[i][2] - c[2], ex = sqrtf(dx * dx + dz * dz) - R, ey = hgP[i][1] > yHi ? hgP[i][1] - yHi : hgP[i][1] < yLo ? yLo - hgP[i][1] : 0.0f;
        g = fminf(g, ex > 0.0f ? sqrtf(ex * ex + ey * ey) : ey > 0.0f ? ey : ex);
    }
    float dx = pa[0] - c[0], dz = pa[2] - c[2], cap = sqrtf(dx * dx + dz * dz) - R - pr;
    hg_part(what, g);
    if (g < hgMesh) { hgMesh = g; hgWhat = what; }
    hgCap = fminf(hgCap, cap);
}
int hle_hg_boss_state(void)
{
    extern int hle_fk_state(void), hle_sun_state(void), hle_dg_state(void);
    Boss *b = boss_get_instance(); if (!b) return -1;
    return b->phaseIndex == 3 ? hle_fk_state() : b->phaseIndex == 4 ? hle_sun_state() : b->phaseIndex == 5 ? hle_dg_state() : (int)b->state;
}
const char *hle_hg_fxname(int shape, bool arc, int st)     // a hostile effect, by its boss, its shape and the boss's move when it was cast
{
    static const char *const S[7] = { "X", "blade", "crescent", "ember", "beam", "shot", "wave" };
    static const char *const B[6] = { "?", "G", "U", "FW", "Sun", "SB" };
    static char buf[6][7][32][28];
    Boss *b = boss_get_instance(); int p = b && b->phaseIndex >= 1 && b->phaseIndex <= 5 ? b->phaseIndex : 0;
    int k = arc ? 6 : (shape >= 0 && shape < 6 ? shape : 5), m = st >= 0 && st < 32 ? st : 31;
    if (!buf[p][k][m][0]) snprintf(buf[p][k][m], sizeof buf[p][k][m], "%s %s (move %d)", B[p], S[k], st);
    return buf[p][k][m];
}
#endif
#ifdef HLE_LOG_HITGAP
void hle_hg_sbvol(const char *name, float ax)     // Shadowborne's own test: what is drawn within 6 (PLAYER_R) of an axis 4..16 over the feet; ax its distance
{
    static const float R[HG_NV] = { 6.0f, 6.0f, 5.0f, 6.0f, 5.0f };
    HgWin *w = hg_win(name); if (!w) return;
    w->last = hle_hg_frame;
    for (int v = 0; v < HG_NV; v++) w->m[v] = fminf(w->m[v], ax - R[v]);
}
int hle_hg_player_pts(const float (**p)[3]) { hg_player(); *p = (const float (*)[3])hgP; return hgN; }
float hle_hg_r(int v) { float a[3], b[3]; return hg_axis(v, a, b); }
void hle_hg_m(const char *name, const float m[HG_NV], bool fixed)     // a reach worked out by the caller for each hurtbox
{
    HgWin *w = hg_win(name); if (!w) return;
    w->last = hle_hg_frame; w->fixed = w->fixed || fixed;
    for (int v = 0; v < HG_NV; v++) w->m[v] = fminf(w->m[v], m[v]);
}
#endif
#ifdef HLE_LOG_HITGAP
void hle_hg_ring2(const char *what, const float c[3], float Rin, float Rout, float yLo, float yHi)     // a band Rin..Rout round c as drawn, yLo..yHi
{
    float pa[3], pb[3], pr = hg_axis(0, pa, pb), g = 1e9f;
    for (int i = 0; i < hgN; i++) {
        float dx = hgP[i][0] - c[0], dz = hgP[i][2] - c[2], d = sqrtf(dx * dx + dz * dz), ex = fmaxf(Rin - d, d - Rout), ey = hgP[i][1] > yHi ? hgP[i][1] - yHi : hgP[i][1] < yLo ? yLo - hgP[i][1] : 0.0f;
        g = fminf(g, ex > 0.0f ? sqrtf(ex * ex + ey * ey) : ey > 0.0f ? ey : ex);
    }
    float dx = pa[0] - c[0], dz = pa[2] - c[2], d = sqrtf(dx * dx + dz * dz);
    hg_part(what, g);
    if (g < hgMesh) { hgMesh = g; hgWhat = what; }
    hgCap = fminf(hgCap, fmaxf(d - pr - Rout, Rin - (d + pr)));
}
#endif
