#include <libdragon.h>
#include "gpu_frame.h"
#include "general_utility.h"
#include <stdio.h>
#include <string.h>

#include "menu_controller.h"
#include "../utilities/joypad_utility.h"
#include "audio_controller.h"
#include "../scenes/guardian/guardian_scene.h"
#include "../utilities/save_controller.h"
#include <stdlib.h>
#include "../character/character.h"
#include "../character/character_skin.h"
#include "../fx/skill_fx.h"
#include "../game/tower/tower.h"
#include "../utilities/ui_pages.h"
#define rdpq_text_printf ui_textf     // (what a menu lays out is tracked: its box fits it, ui_pages.h)

#include "../utilities/globals.h"
#include "../utilities/video_layout.h"
#include "camera_controller.h"

// Menu state
// The Relics list: row 0 is None, row 1 + id each relic, then Back. The Skins list: one row a skin, then Back
static const char *skin_lock_hint(int k)
{
    return k == SKIN_CRIMSON_CLOAK ? "Earned when every foe lies still"
         : k == SKIN_TEAL_CLOAK ? "It waits high within the Tower"
         : k == SKIN_NIGHTFARER ? "Its price is paid in embers" : "";
}

static int s_listPer;        // a paged list's rows a page (list_page), 0 when it fits
static MenuState currentMenu = MENU_MAIN;
static MenuState parentMenu = MENU_MAIN;
static int selectedOption = 0;
static int parentSelectedOption = 0;
static bool menuActive = false;
static bool musicWasPaused = false;
static bool menuIsTitleMenu = false;
static GameState menuReturnState = GAME_STATE_PLAYING;

// Scene/UI action output.
// The menu controller does not switch scenes directly.
// Consumers call menu_controller_consume_action().
static MenuAction pendingAction = MENU_ACTION_NONE;

// Settings hub "return to" target (since parentMenu is reused for submenus).
static MenuState settingsHubReturnMenu = MENU_MAIN;
static int       settingsHubReturnSelectedOption = 0;


// Controls menu icons (prefer black-outline set for readability on bright backgrounds)
static sprite_t *iconA = NULL;
static sprite_t *iconB = NULL;
static sprite_t *iconZ = NULL;
static sprite_t *iconStart = NULL;
static sprite_t *iconStick = NULL;
static sprite_t *iconCLeft = NULL;
static sprite_t *iconDpad = NULL;
// The Controls page's icons (26 KB of CI8 sprites) only while it's up: they were held all the time, a fight's
// low point included. A is shared with the button prompts and stays
static void controls_icons(bool want)
{
    if (want == (iconB != NULL)) return;
    if (want) {
        iconB = sprite_load("rom:/buttons/B.sprite");
        iconZ = sprite_load("rom:/buttons/Z.sprite");
        iconStart = sprite_load("rom:/buttons/Start.sprite");
        iconStick = sprite_load("rom:/buttons/StickTexture.sprite");
        iconCLeft = sprite_load("rom:/buttons/CLeft.sprite");
        iconDpad = sprite_load("rom:/buttons/Dpad.sprite");
        return;
    }
    rspq_wait();        // (the last frame drew them)
    sprite_t **all[] = { &iconB, &iconZ, &iconStart, &iconStick, &iconCLeft, &iconDpad };
    for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) { if (*all[i]) sprite_free(*all[i]); *all[i] = NULL; }
}

// Input handling for edge detection
static bool lastStartPressed = false;
static bool lastUpPressed = false;
static bool lastDownPressed = false;
static bool lastLeftPressed = false;
static bool lastRightPressed = false;
static bool lastAPressed = false;
static bool lastBPressed = false;
static bool lastRPressed = false;      // Z: Details (no layout moves it)
static bool lastStickUp = false;
static bool lastStickDown = false;
static bool lastStickLeft = false;
static bool lastStickRight = false;

// Overscan calibration overlay (inside Video settings)
static bool overscanCalibrating = false;
static int8_t overscanPrevX = 0;
static int8_t overscanPrevY = 0;

// The Stats menu: the run's numbers, then its build, as one paged list (ui_pages: Left/Right turn); on a Build
// page Up/Down move a row cursor (-1: the Back row at the foot) and A opens the row's Details
static UiPager runPager;
static int runRow = -1;
static UiDoc *s_statsDoc;     // (these two on the heap while the Stats menu is open: none during a fight)
// the menus' box for the Stats pages and the Codex, inside the title-safe area (the overscan setting's, never wider than
// the fixed 7.5% one: the default and smallest settings keep it where it was, a larger one moves it in, its pages shorter)
static int run_mx(void) { int m = ui_safe_margin_x(); return m < TITLE_SAFE_MARGIN_X ? TITLE_SAFE_MARGIN_X : m; }
static int run_my(void) { int m = ui_safe_margin_y(); return m < TITLE_SAFE_MARGIN_Y ? TITLE_SAFE_MARGIN_Y : m; }
#define RUN_TX (run_mx() + 6)                           // 30 at 7.5%
#define RUN_TW (SCREEN_WIDTH - 2 * RUN_TX)
#define RUN_CY (run_my() + 16)                          // the corners' baseline (34)
#define RUN_BODY (RUN_CY + 22)                          // the first line's (56)
#define RUN_BODY_END (SCREEN_HEIGHT - run_my() - 22)    // the last line's (200: over the Back row)
typedef struct { int statsPages, buildPages, n, perPage; char left[TOWER_BUILD_ROWS][40], right[TOWER_BUILD_ROWS][12]; int style[TOWER_BUILD_ROWS], card[TOWER_BUILD_ROWS]; } RunPages;
static RunPages *s_run;
static void run_pages_free(void) { free(s_statsDoc); free(s_run); s_statsDoc = NULL; s_run = NULL; }
static void run_pages_build(void)
{
    if (!s_statsDoc) s_statsDoc = malloc(sizeof *s_statsDoc);
    if (!s_run) s_run = malloc(sizeof *s_run);
    ui_doc_clear(s_statsDoc);
    tower_stats_doc(s_statsDoc);
    int pitch = ui_line_pitch();
    s_run->statsPages = ui_doc_layout(s_statsDoc, RUN_TW, RUN_BODY_END - RUN_BODY + pitch);
    s_run->n = tower_build_rows(s_run->left, s_run->right, s_run->style, s_run->card, TOWER_BUILD_ROWS);
    s_run->perPage = (RUN_BODY_END - RUN_BODY - 8) / pitch + 1;     // (8: the gap under the skills)
    s_run->buildPages = s_run->n > 0 ? (s_run->n + s_run->perPage - 1) / s_run->perPage : 1;
    runPager.pages = (int8_t)(s_run->statsPages + s_run->buildPages);
    if (runPager.page >= runPager.pages) runPager.page = runPager.pages - 1;
}
// The Codex: its topics, paged as Stats is (Left/Right turn; Up/Down along the rows and the Back row at the foot;
// A opens a topic in the Details panel). From the Tower menu or the pause menu (a climb's)
static UiPager codexPager;
static int codexRow = 0;
static MenuState codexReturn = MENU_TOWER;
static int codexReturnOption = 0;
static int codex_per_page(void) { return (RUN_BODY_END - RUN_BODY) / ui_line_pitch() + 1; }
static int codex_rows_on_page(void) { int n = tower_codex_count() - codexPager.page * codex_per_page(); return n < codex_per_page() ? n : codex_per_page(); }
static void codex_open_menu(MenuState from, int option)
{
    codexReturn = from; codexReturnOption = option;
    currentMenu = MENU_CODEX;
    ui_pager_reset(&codexPager, (tower_codex_count() + codex_per_page() - 1) / codex_per_page());
    codexRow = 0;
}
static bool main_row_hidden(int i) { return (i == MENU_MAIN_RUN || i == MENU_MAIN_CODEX) && !tower_active(); }    // (Tower runs only)
// Settings from a pause menu: no Skills or Skin rows (the user's call: they're the title's; Sunforged's toggle with the skins)
static bool settings_row_hidden(int i) { return (i == MENU_SETTINGS_SKILLS || i == MENU_SETTINGS_SKIN) && settingsHubReturnMenu != MENU_TITLE; }
static bool row_hidden(int i) { return currentMenu == MENU_MAIN ? main_row_hidden(i) : currentMenu == MENU_SETTINGS && settings_row_hidden(i); }
static int run_rows_on_page(void)
{
    int b = runPager.page - s_run->statsPages;
    if (b < 0) return 0;
    int n = s_run->n - b * s_run->perPage;
    return n < s_run->perPage ? n : s_run->perPage;
}

// Menu text
static const char* mainMenuOptions[MENU_MAIN_COUNT] = {
    "Resume",
    "Stats",
    "Restart",
    "Settings",
    "Controls",
    "Codex",
    "Main Menu",
};

static const char* titleMenuOptions[MENU_TITLE_COUNT] = {
    [MENU_TITLE_STORY]  = "Story",
    [MENU_TITLE_BONUS]     = "Bonus",
    [MENU_TITLE_TOWER]     = "Tower",
    [MENU_TITLE_LOAD_GAME] = "Load Game",
    [MENU_TITLE_SETTINGS]  = "Settings",
    [MENU_TITLE_CREDITS]   = "Credits",
};

static const char* settingsMenuOptions[MENU_SETTINGS_COUNT] = {
    "Audio",
    "Video",
    "Controls",
    "Skills: %s",
    "Skin: %s",           // A: the Skins list
    "Back",
};

static const char* audioMenuOptions[MENU_AUDIO_COUNT] = {
    "Master Volume: %d",
    "Music Volume: %d",
    "SFX Volume: %d",
    "Mute All: %s",
    "Audio Mode: %s",
    "Rumble: %s",
    "Back",
};

static const char* videoMenuOptions[MENU_VIDEO_COUNT] = {
    "Aspect: %s",
    "Calibrate Overscan",
    "Back",
};

static const char* towerMenuOptions[MENU_TOWER_COUNT] = {
    "Climb",
    "Shrine",
    "Offerings",
    "Codex",
    "Heat: %d",
    "Back",
};


static const char* bonusMenuOptions[MENU_BONUS_COUNT] = {
    "Fallen Warrior",
    "The Shackled Sun",
    "Shadowborne",
    "Back",
};

static const char* loadGameMenuOptions[MENU_LOAD_GAME_COUNT] = {
    "Save 1",
    "Save 2",
    "Save 3",
    "Back",
};

static int8_t clamp_overscan_x(int v)
{
    if (v < UI_OVERSCAN_MIN) v = UI_OVERSCAN_MIN;
    int maxX = (SCREEN_WIDTH / 2) - 2;
    if (v > maxX) v = maxX;
    return (int8_t)v;
}

static int8_t clamp_overscan_y(int v)
{
    if (v < UI_OVERSCAN_MIN) v = UI_OVERSCAN_MIN;
    int maxY = (SCREEN_HEIGHT / 2) - 2;
    if (v > maxY) v = maxY;
    return (int8_t)v;
}

static int get_menu_option_count(MenuState menu)
{
    switch (menu) {
        case MENU_MAIN:      return MENU_MAIN_COUNT;
        case MENU_TITLE:     return MENU_TITLE_COUNT;
        case MENU_SETTINGS:  return MENU_SETTINGS_COUNT;
        case MENU_AUDIO:     return MENU_AUDIO_COUNT;
        case MENU_VIDEO:     return MENU_VIDEO_COUNT;
        case MENU_LOAD_GAME: return MENU_LOAD_GAME_COUNT;
        case MENU_BONUS:     return MENU_BONUS_COUNT;
        case MENU_TOWER:     return MENU_TOWER_COUNT;
        case MENU_SHRINE:    return MENU_SHRINE_COUNT;
        case MENU_RELICS:    return tower_relic_total() + 2;    // None, the relics, Back
        case MENU_UNLOCKS:   return tower_pack_count() + 1;     // the packs, Back
        case MENU_OFFERINGS: return tower_offer_count() + 1;    // the Offerings, Back
        case MENU_CODEX:     return 1;      // (its corners and rows: codexPager, codexRow)
        case MENU_SKINS:     return SKIN_COUNT + 2;   // the skins, Sunforged, Back
        case MENU_CONTROLS:  return 3;    // Layout, Camera speed, Back
        case MENU_RUN:       return 1;      // (its corners and rows: runPager, runRow)
        case MENU_CREDITS:   return 1;
        default:             return 1;
    }
}

static void overscan_apply_and_save(void)
{
    uiOverscanX = clamp_overscan_x((int)uiOverscanX);
    uiOverscanY = clamp_overscan_y((int)uiOverscanY);
    (void)save_controller_save_settings();
}

static void draw_overscan_corner_markers(void)
{
    const int left   = ui_safe_margin_x();
    const int right  = SCREEN_WIDTH - ui_safe_margin_x();
    const int top    = ui_safe_margin_y();
    const int bottom = SCREEN_HEIGHT - ui_safe_margin_y();

    const int len = 14;
    const int t = 2;

    rdpq_sync_pipe();
    rdpq_set_mode_standard();
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);

    rdpq_set_prim_color(RGBA32(0, 0, 0, 120));
    rdpq_fill_rectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

    rdpq_set_prim_color(RGBA32(80, 255, 80, 255));

    rdpq_fill_rectangle(left, top, left + len, top + t);
    rdpq_fill_rectangle(left, top, left + t, top + len);

    rdpq_fill_rectangle(right - len, top, right, top + t);
    rdpq_fill_rectangle(right - t, top, right, top + len);

    rdpq_fill_rectangle(left, bottom - t, left + len, bottom);
    rdpq_fill_rectangle(left, bottom - len, left + t, bottom);

    rdpq_fill_rectangle(right - len, bottom - t, right, bottom);
    rdpq_fill_rectangle(right - t, bottom - len, right, bottom);

    // its words on the menus' box (a fight's boss title showed through them)
    ui_box(left + 20, top + 4, right - 20, top + 42);
    rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
    const int textY = top + 18;

    rdpq_text_printf(&(rdpq_textparms_t){
        .align = ALIGN_CENTER,
        .width = SCREEN_WIDTH,
        .wrap = WRAP_WORD,
    }, FONT_UNBALANCED, 0, textY, "Overscan Calibration");

    rdpq_text_printf(&(rdpq_textparms_t){
        .align = ALIGN_CENTER,
        .width = SCREEN_WIDTH,
        .wrap = WRAP_WORD,
    }, FONT_UNBALANCED, 0, textY + 16, "Fit the corners to the screen");

    rdpq_text_printf(&(rdpq_textparms_t){
        .align = ALIGN_CENTER,
        .width = SCREEN_WIDTH,
        .wrap = WRAP_WORD,
    }, FONT_UNBALANCED, 0, textY + 34, "X: %d   Y: %d", (int)uiOverscanX, (int)uiOverscanY);
    // what A and B do there, as words (no button letters: A takes, B goes back)
    rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = SCREEN_WIDTH, .style_id = FONT_STYLE_GREY }, FONT_UNBALANCED, 0, bottom - 8, "Enter: save   Back: cancel");
}

void menu_controller_init(void)
{
    currentMenu = MENU_MAIN;
    parentMenu = MENU_MAIN;
    selectedOption = 0;
    parentSelectedOption = 0;
    menuActive = false;
    musicWasPaused = false;
    menuIsTitleMenu = false;
    menuReturnState = GAME_STATE_PLAYING;
    pendingAction = MENU_ACTION_NONE;

    settingsHubReturnMenu = MENU_MAIN;
    settingsHubReturnSelectedOption = 0;

    overscanCalibrating = false;
    overscanPrevX = 0;
    overscanPrevY = 0;

    lastStartPressed = false;
    lastUpPressed = false;
    lastDownPressed = false;
    lastLeftPressed = false;
    lastRightPressed = false;
    lastAPressed = false;
    lastBPressed = false;
    lastStickUp = false;
    lastStickDown = false;
    lastStickLeft = false;
    lastStickRight = false;


    iconA = sprite_load_shared("rom:/buttons/A.sprite");
    // (the Controls page's other icons load with it: controls_icons)
}

MenuAction menu_controller_consume_action(void)
{
    MenuAction action = pendingAction;
    pendingAction = MENU_ACTION_NONE;
    return action;
}

// A purchase with Embers (the Shrine's upgrades, the Nightfarer, Relics, Unlocks, an Offering set aside): a confirm first
// (ui_confirm_open: its effect, Buy / Back, the cursor on Back); too few Embers (or owned, at its max, not sold): nothing
// opens (its line of help says why, red). Its Buy runs buy_take on the row still under the menu's cursor
static void buy_ask(const char *name, int cost, const char *effect)
{
    if (cost < 0) return;
    if (tower_embers() < cost) {
#ifdef HLE_AUTOPLAY
        debugf("[buy] %s for %d: refused, Embers %d (no confirm)\n", name, cost, tower_embers());
#endif
        return;
    }
    UiPanel *p = ui_details();
    UiDoc *d = ui_panel_doc(p);
    char t[64];
    ui_doc_clear(d);
    ui_doc_text(d, FONT_STYLE_WHITE, "%s", effect);
    ui_doc_gap(d);
    ui_doc_text(d, FONT_STYLE_GREY, TOWER_EMBERS " %d -> %d", tower_embers(), tower_embers() - cost);
    snprintf(t, sizeof t, "Buy %s for %d Embers?", name, cost);
    ui_confirm_open(p, t, FONT_STYLE_GOLD, "Buy");
    p->owner = UI_OWNER_MENU;
#ifdef HLE_AUTOPLAY
    debugf("[buy] confirm: %s (menu %d row %d, Embers %d)\n", t, currentMenu, selectedOption, tower_embers());
#endif
}
static void buy_take(void)
{
    int i = selectedOption, e = tower_embers();
    bool ok = false;
    if (currentMenu == MENU_SHRINE && i < MENU_SHRINE_UPGRADES) ok = tower_upgrade_buy((UpgradeId)i);
    else if (currentMenu == MENU_SHRINE && i == MENU_SHRINE_NIGHTFARER) ok = tower_nightfarer_buy();
    else if (currentMenu == MENU_RELICS) ok = tower_relic_select(i - 1);
    else if (currentMenu == MENU_UNLOCKS) ok = tower_pack_buy(i);
    else if (currentMenu == MENU_OFFERINGS) ok = !tower_offer_set(i) && tower_offer_toggle(i);
    if (ok) save_controller_save_settings();
#ifdef HLE_AUTOPLAY
    debugf("[buy] bought (menu %d row %d): %s, Embers %d -> %d\n", currentMenu, i, ok ? "yes" : "NO", e, tower_embers());
#endif
    (void)e;
}

static void menu_controller_update_for_state(GameState state, bool cutsceneActive)
{
    if (state == GAME_STATE_DEAD) {
        return;
    }

    if (state == GAME_STATE_TITLE_TRANSITION) {
        if (menuActive && menuIsTitleMenu) {
            menuActive = false;
        }
        return;
    }

    if (
        state != GAME_STATE_PLAYING &&
        state != GAME_STATE_MENU &&
        state != GAME_STATE_TITLE &&
        state != GAME_STATE_VICTORY
    ) {
        return;
    }

    if (state == GAME_STATE_TITLE) {
        menuIsTitleMenu = true;

        if (!menuActive) {
            menuActive = true;
            currentMenu = MENU_TITLE;
            parentMenu = MENU_TITLE;
            parentSelectedOption = 0;
            selectedOption = 0;
        }
    }

    bool startPressed = btn.start;
    bool startJustPressed = startPressed && !lastStartPressed;
    lastStartPressed = startPressed;

    if (startJustPressed && state != GAME_STATE_TITLE) {
        if (!cutsceneActive && state != GAME_STATE_TITLE_TRANSITION) {
            menu_controller_toggle();
        }
        return;
    }

    if (!menuActive) {
        return;
    }

    bool upPressed = btn.d_up;
    bool downPressed = btn.d_down;
    bool leftPressed = btn.d_left;
    bool rightPressed = btn.d_right;
    bool aPressed = btn.a;
    bool bPressed = btn.b;

    // The stick steers every menu as the D-pad does (one step a push)
    const int stickThreshold = 40;
    // only its main direction: a push up or down that leans sideways is not also left or right
    bool vert = abs(joypad.stick_y) >= abs(joypad.stick_x);
    bool stickUp = vert && joypad.stick_y > stickThreshold, stickDown = vert && joypad.stick_y < -stickThreshold;
    bool stickLeft = !vert && joypad.stick_x < -stickThreshold, stickRight = !vert && joypad.stick_x > stickThreshold;
    bool upJustPressed = (upPressed && !lastUpPressed) || (stickUp && !lastStickUp);
    bool downJustPressed = (downPressed && !lastDownPressed) || (stickDown && !lastStickDown);
    bool leftJustPressed = (leftPressed && !lastLeftPressed) || (stickLeft && !lastStickLeft);
    bool rightJustPressed = (rightPressed && !lastRightPressed) || (stickRight && !lastStickRight);
    lastStickUp = stickUp; lastStickDown = stickDown; lastStickLeft = stickLeft; lastStickRight = stickRight;
    bool aJustPressed = aPressed && !lastAPressed;
    bool bJustPressed = bPressed && !lastBPressed;
    bool rJustPressed = btn.z && !lastRPressed;
    lastRPressed = btn.z;

    lastUpPressed = upPressed;
    lastDownPressed = downPressed;
    lastLeftPressed = leftPressed;
    lastRightPressed = rightPressed;
    lastAPressed = aPressed;
    lastBPressed = bPressed;

    if (state == GAME_STATE_TITLE && startJustPressed) {
        aJustPressed = true;
    }

    // a Details panel opened here (A on a Build row, a Codex topic) or a purchase's confirm takes every press until it closes
    UiPanel *dp = ui_details();
    if (dp->open && dp->owner == UI_OWNER_MENU) {
        if (ui_panel_input(dp, upJustPressed, downJustPressed, leftJustPressed, rightJustPressed, aJustPressed, bJustPressed, rJustPressed) == UI_PANEL_ENTER) buy_take();
#ifdef HLE_AUTOPLAY
        else if (!dp->open && dp->confirm) debugf("[buy] closed: nothing bought, Embers %d, menu %d row %d\n", tower_embers(), currentMenu, selectedOption);
#endif
        return;
    }
    if (!(currentMenu == MENU_VIDEO && overscanCalibrating)) {
        int maxOptions = get_menu_option_count(currentMenu);

        if (upJustPressed) {
            selectedOption--;
            if (selectedOption < 0) {
                selectedOption = maxOptions - 1;
            }
            while (row_hidden(selectedOption)) selectedOption--;   // (the Tower's rows, the pause Settings' skins)
        }

        if (downJustPressed) {
            selectedOption++;
            if (selectedOption >= maxOptions) {
                selectedOption = 0;
            }
            while (row_hidden(selectedOption)) selectedOption++;
        }
    }

    bool pagedList = currentMenu == MENU_SHRINE || currentMenu == MENU_RELICS || currentMenu == MENU_SKINS || currentMenu == MENU_UNLOCKS || currentMenu == MENU_OFFERINGS;
    if (pagedList && s_listPer && (leftJustPressed || rightJustPressed)) {
        int n = get_menu_option_count(currentMenu), k = (selectedOption / s_listPer + (rightJustPressed ? 1 : -1)) * s_listPer;
        if (k >= 0 && k < n) selectedOption = k;
    }
    if (currentMenu == MENU_MAIN) {
        if (bJustPressed) {
            menu_controller_close();
            return;
        }

        if (aJustPressed) {
            switch (selectedOption) {
                case MENU_MAIN_RESUME:
                    menu_controller_close();
                    break;

                case MENU_MAIN_RUN:
                    currentMenu = MENU_RUN;
                    run_pages_build();
                    ui_pager_reset(&runPager, runPager.pages);     // page 0, on Back
                    runRow = -1;
                    break;

                case MENU_MAIN_RESTART:
                    menu_controller_close();

                    /*
                     * First pass:
                     * restart is still guardian-scene-owned.
                     * Later, this can become pendingAction = MENU_ACTION_RESTART_GAME.
                     */
                    scene_restart();
                    break;

                case MENU_MAIN_SETTINGS:
                    parentMenu = MENU_MAIN;
                    parentSelectedOption = MENU_MAIN_SETTINGS;
                    settingsHubReturnMenu = parentMenu;
                    settingsHubReturnSelectedOption = parentSelectedOption;
                    currentMenu = MENU_SETTINGS;
                    selectedOption = 0;
                    break;

                case MENU_MAIN_CONTROLS:
                    parentMenu = MENU_MAIN;
                    parentSelectedOption = MENU_MAIN_CONTROLS;
                    currentMenu = MENU_CONTROLS;
                    selectedOption = 0;
                    break;

                case MENU_MAIN_CODEX:
                    codex_open_menu(MENU_MAIN, MENU_MAIN_CODEX);
                    break;

                case MENU_MAIN_QUIT:
                    menu_controller_close();
                    pendingAction = MENU_ACTION_QUIT_TO_TITLE;
                    break;

                default:
                    break;
            }
        }
    } else if (currentMenu == MENU_TITLE) {
        if (aJustPressed) {
            switch (selectedOption) {
                case MENU_TITLE_STORY: {
                    int lastPlayedSlot = save_controller_get_last_played_slot();
                    int slotToLoad = (lastPlayedSlot >= 0) ? lastPlayedSlot : 0;

                    save_controller_set_active_slot(slotToLoad);
                    save_controller_update_last_played_timestamp();

                    scene_set_bonus_fight(BONUS_NONE);
                    scene_set_tower(false);
                    menu_controller_close();
                    pendingAction = MENU_ACTION_TITLE_START_GAME;
                    break;
                }

                case MENU_TITLE_TOWER: {
                    // The Tower's Embers and upgrades belong to the last played slot
                    int lastPlayedSlot = save_controller_get_last_played_slot();
                    save_controller_set_active_slot((lastPlayedSlot >= 0) ? lastPlayedSlot : 0);
                    parentMenu = MENU_TITLE;
                    parentSelectedOption = MENU_TITLE_TOWER;
                    currentMenu = MENU_TOWER;
                    selectedOption = 0;
                    break;
                }

                case MENU_TITLE_BONUS:
                    parentMenu = MENU_TITLE;
                    parentSelectedOption = MENU_TITLE_BONUS;
                    currentMenu = MENU_BONUS;
                    selectedOption = 0;
                    break;

                case MENU_TITLE_LOAD_GAME:
                    parentMenu = MENU_TITLE;
                    parentSelectedOption = MENU_TITLE_LOAD_GAME;
                    currentMenu = MENU_LOAD_GAME;
                    selectedOption = 0;
                    break;

                case MENU_TITLE_SETTINGS:
                    parentMenu = MENU_TITLE;
                    parentSelectedOption = MENU_TITLE_SETTINGS;
                    settingsHubReturnMenu = parentMenu;
                    settingsHubReturnSelectedOption = parentSelectedOption;
                    currentMenu = MENU_SETTINGS;
                    selectedOption = 0;
                    break;

                case MENU_TITLE_CREDITS:
                    parentMenu = MENU_TITLE;
                    parentSelectedOption = MENU_TITLE_CREDITS;
                    currentMenu = MENU_CREDITS;
                    selectedOption = 0;
                    break;

                default:
                    break;
            }
        }
    } else if (currentMenu == MENU_SETTINGS) {
        if (aJustPressed && selectedOption == MENU_SETTINGS_SKIN) {
            currentMenu = MENU_SKINS;
            selectedOption = character_skin_get();
            return;
        }
        if ((leftJustPressed || rightJustPressed) && selectedOption == MENU_SETTINGS_SKILLS && settingsHubReturnMenu == MENU_TITLE) {
            character_set_skills_enabled(!character_skills_enabled());     // as A does, like every other toggle
            (void)save_controller_save_settings();
        }
        if (aJustPressed) {
            switch (selectedOption) {
                case MENU_SETTINGS_AUDIO:
                    parentMenu = MENU_SETTINGS;
                    parentSelectedOption = MENU_SETTINGS_AUDIO;
                    currentMenu = MENU_AUDIO;
                    selectedOption = 0;
                    break;

                case MENU_SETTINGS_VIDEO:
                    parentMenu = MENU_SETTINGS;
                    parentSelectedOption = MENU_SETTINGS_VIDEO;
                    currentMenu = MENU_VIDEO;
                    selectedOption = 0;
                    break;

                case MENU_SETTINGS_CONTROLS:
                    parentMenu = MENU_SETTINGS;
                    parentSelectedOption = MENU_SETTINGS_CONTROLS;
                    currentMenu = MENU_CONTROLS;
                    selectedOption = 0;
                    break;

                case MENU_SETTINGS_SKILLS:
                    // Only from the title screen: no switching mid-battle
                    if (settingsHubReturnMenu == MENU_TITLE) {
                        character_set_skills_enabled(!character_skills_enabled());
                        (void)save_controller_save_settings();
                    }
                    break;

                case MENU_SETTINGS_BACK:
                    currentMenu = settingsHubReturnMenu;
                    selectedOption = settingsHubReturnSelectedOption;
                    break;

                default:
                    break;
            }
        }

        if (bJustPressed) {
            currentMenu = settingsHubReturnMenu;
            selectedOption = settingsHubReturnSelectedOption;
        }
    } else if (currentMenu == MENU_AUDIO) {
        if (leftJustPressed || rightJustPressed) {
            int direction = rightJustPressed ? 1 : -1;

            switch (selectedOption) {
                case MENU_AUDIO_MASTER_VOLUME:
                    audio_adjust_master_volume(direction);
                    break;

                case MENU_AUDIO_MUSIC_VOLUME:
                    audio_adjust_music_volume(direction);
                    break;

                case MENU_AUDIO_SFX_VOLUME:
                    audio_adjust_sfx_volume(direction);
                    break;

                case MENU_AUDIO_MUTE_TOGGLE:
                    audio_toggle_mute();
                    break;

                case MENU_AUDIO_STEREO_MODE:
                    audio_toggle_stereo_mode();
                    break;

                case MENU_AUDIO_RUMBLE_TOGGLE:
                    joypad_set_rumble_enabled(!joypad_is_rumble_enabled());
                    (void)save_controller_save_settings();
                    break;

                default:
                    break;
            }
        }

        if (aJustPressed) {
            switch (selectedOption) {
                case MENU_AUDIO_MUTE_TOGGLE:
                    audio_toggle_mute();
                    break;

                case MENU_AUDIO_STEREO_MODE:
                    audio_toggle_stereo_mode();
                    break;

                case MENU_AUDIO_RUMBLE_TOGGLE:
                    joypad_set_rumble_enabled(!joypad_is_rumble_enabled());
                    (void)save_controller_save_settings();
                    break;

                case MENU_AUDIO_BACK:
                    currentMenu = parentMenu;
                    selectedOption = parentSelectedOption;
                    break;

                default:
                    break;
            }
        }

        if (bJustPressed) {
            currentMenu = parentMenu;
            selectedOption = parentSelectedOption;
        }
    } else if (currentMenu == MENU_VIDEO) {
        if (overscanCalibrating) {
            if (leftJustPressed) {
                uiOverscanX = clamp_overscan_x((int)uiOverscanX - 1);
                overscan_apply_and_save();
            }

            if (rightJustPressed) {
                uiOverscanX = clamp_overscan_x((int)uiOverscanX + 1);
                overscan_apply_and_save();
            }

            if (upJustPressed) {
                uiOverscanY = clamp_overscan_y((int)uiOverscanY + 1);
                overscan_apply_and_save();
            }

            if (downJustPressed) {
                uiOverscanY = clamp_overscan_y((int)uiOverscanY - 1);
                overscan_apply_and_save();
            }

            if (aJustPressed) {
                overscanCalibrating = false;
            } else if (bJustPressed) {
                uiOverscanX = overscanPrevX;
                uiOverscanY = overscanPrevY;
                overscan_apply_and_save();
                overscanCalibrating = false;
            }

            return;
        }

        if (leftJustPressed || rightJustPressed) {
            switch (selectedOption) {
                case MENU_VIDEO_ASPECT:
                    hdAspect = !hdAspect;
                    (void)save_controller_save_settings();
                    break;

                default:
                    break;
            }
        }

        if (aJustPressed) {
            switch (selectedOption) {
                case MENU_VIDEO_ASPECT:
                    hdAspect = !hdAspect;
                    (void)save_controller_save_settings();
                    break;

                case MENU_VIDEO_UI_OVERSCAN_CALIBRATE:
                    overscanPrevX = uiOverscanX;
                    overscanPrevY = uiOverscanY;
                    overscanCalibrating = true;
                    break;

                case MENU_VIDEO_BACK:
                    currentMenu = parentMenu;
                    selectedOption = parentSelectedOption;
                    break;

                default:
                    break;
            }
        }

        if (bJustPressed) {
            currentMenu = parentMenu;
            selectedOption = parentSelectedOption;
        }
    } else if (currentMenu == MENU_CONTROLS) {
        if (bJustPressed || (aJustPressed && selectedOption == 2)) {
            currentMenu = parentMenu;
            selectedOption = parentSelectedOption;
        } else if (selectedOption < 2 && (aJustPressed || leftJustPressed || rightJustPressed)) {
            int d = leftJustPressed ? -1 : 1;
            if (selectedOption == 0) joypad_set_layout((joypad_get_layout() + d + LAYOUT_COUNT) % LAYOUT_COUNT);
            else camera_set_speed((camera_get_speed() - 1 + d + 5) % 5 + 1);
            (void)save_controller_save_settings();
        }
    } else if (currentMenu == MENU_RUN) {
        // Left/Right turn the pages (Stats, then Build), B closes from any page; on a Build page Up/Down walk its rows and
        // the Back row at the foot (-1), A opens a row's Details; on a Stats page the Back row is the only one
        run_pages_build();
        int rows = run_rows_on_page();
        if (runRow >= rows) runRow = rows - 1;
        if (rows && downJustPressed) runRow = runRow + 1 < rows ? runRow + 1 : -1;
        else if (rows && upJustPressed) runRow = runRow < 0 ? rows - 1 : runRow - 1;
        int at = (runPager.page - s_run->statsPages) * s_run->perPage + runRow;
        if (runRow >= 0 && aJustPressed && !bJustPressed) { if (s_run->card[at] >= 0) tower_details_card(s_run->card[at]); }
        else {
            int r = ui_pager_input(&runPager, leftJustPressed, rightJustPressed, aJustPressed && runRow < 0, bJustPressed);
            if (r == UI_PAGER_BACK) { currentMenu = MENU_MAIN; selectedOption = MENU_MAIN_RUN; run_pages_free(); }
            else if (r == UI_PAGER_TURNED) runRow = run_rows_on_page() ? 0 : -1;
        }
    } else if (currentMenu == MENU_CREDITS) {
        if (aJustPressed || bJustPressed) {
            currentMenu = parentMenu;
            selectedOption = parentSelectedOption;
        }
    } else if (currentMenu == MENU_LOAD_GAME) {
        if (aJustPressed) {
            if (selectedOption >= MENU_LOAD_GAME_SLOT_1 && selectedOption <= MENU_LOAD_GAME_SLOT_3) {
                // Only loads the file (its skin, settings and progress): back to the title
                save_controller_set_active_slot(selectedOption);
                save_controller_update_last_played_timestamp();
                currentMenu = parentMenu;
                selectedOption = parentSelectedOption;
            } else if (selectedOption == MENU_LOAD_GAME_BACK) {
                currentMenu = parentMenu;
                selectedOption = parentSelectedOption;
            }
        }

        if (bJustPressed) {
            currentMenu = parentMenu;
            selectedOption = parentSelectedOption;
        }
    } else if (currentMenu == MENU_BONUS) {
        if (aJustPressed && selectedOption != MENU_BONUS_BACK) {
            // Bonus fights use the last played slot for settings and run stats
            int lastPlayedSlot = save_controller_get_last_played_slot();
            save_controller_set_active_slot((lastPlayedSlot >= 0) ? lastPlayedSlot : 0);

            scene_set_bonus_fight(selectedOption == MENU_BONUS_SHACKLED_SUN ? BONUS_SHACKLED_SUN :
                                  selectedOption == MENU_BONUS_SHADOWBORN ? BONUS_SHADOWBORN : BONUS_FALLEN_KNIGHT);
            scene_set_tower(false);
            menu_controller_close();
            pendingAction = MENU_ACTION_TITLE_START_GAME;
        } else if (bJustPressed || (aJustPressed && selectedOption == MENU_BONUS_BACK)) {
            currentMenu = parentMenu;
            selectedOption = parentSelectedOption;
        }
    } else if (currentMenu == MENU_TOWER) {
        if (aJustPressed && selectedOption == MENU_TOWER_CLIMB) {
            scene_set_bonus_fight(BONUS_NONE);
            scene_set_tower(true);
            menu_controller_close();
            pendingAction = MENU_ACTION_TITLE_START_GAME;
        } else if (aJustPressed && selectedOption == MENU_TOWER_SHRINE) {
            currentMenu = MENU_SHRINE;
            selectedOption = 0;
        } else if (aJustPressed && selectedOption == MENU_TOWER_OFFERINGS) {
            currentMenu = MENU_OFFERINGS;
            selectedOption = 0;
        } else if (aJustPressed && selectedOption == MENU_TOWER_CODEX) {
            codex_open_menu(MENU_TOWER, MENU_TOWER_CODEX);
        } else if (selectedOption == MENU_TOWER_HEAT && (aJustPressed || leftJustPressed || rightJustPressed)) {
            int h = tower_heat() + (leftJustPressed ? -1 : 1);
            tower_heat_set(aJustPressed && h > 10 ? 0 : h);      // (A alone reaches every step; saved with the Tower's bytes)
        } else if (bJustPressed || (aJustPressed && selectedOption == MENU_TOWER_BACK)) {
            currentMenu = MENU_TITLE;
            selectedOption = MENU_TITLE_TOWER;
        }
    } else if (currentMenu == MENU_SHRINE) {
        bool back = bJustPressed || (aJustPressed && selectedOption == MENU_SHRINE_COUNT - 1);
        if (back) {
            currentMenu = MENU_TOWER;
            selectedOption = MENU_TOWER_SHRINE;
        } else if (selectedOption == MENU_SHRINE_RELIC && aJustPressed) {
            currentMenu = MENU_RELICS;
            selectedOption = tower_relic_equipped() + 1;      // on the one worn (None: row 0)
        } else if (selectedOption == MENU_SHRINE_NIGHTFARER && aJustPressed) {
            buy_ask("Nightfarer", tower_nightfarer_cost(), "A tracker's winged helm and blue coat (a skin)");
        } else if (selectedOption == MENU_SHRINE_PACKS && aJustPressed) {
            currentMenu = MENU_UNLOCKS;
            selectedOption = 0;
        } else if (aJustPressed && selectedOption < MENU_SHRINE_UPGRADES) {
            buy_ask(tower_upgrade_name((UpgradeId)selectedOption), tower_upgrade_cost((UpgradeId)selectedOption), tower_upgrade_desc((UpgradeId)selectedOption));
        }
    } else if (currentMenu == MENU_RELICS) {
        int back = tower_relic_total() + 1;
        if (bJustPressed || (aJustPressed && selectedOption == back)) {
            currentMenu = MENU_SHRINE;
            selectedOption = MENU_SHRINE_RELIC;
        } else if (aJustPressed && selectedOption - 1 != tower_relic_equipped()) {
            int k = selectedOption - 1;
            if (k >= 0 && !tower_relic_owned(k)) buy_ask(tower_relic_name(k), tower_relic_cost(k), tower_relic_desc(k));     // (bought, it's worn)
            else if (tower_relic_select(k)) save_controller_save_settings();     // wear an owned one, or None
        }
    } else if (currentMenu == MENU_UNLOCKS) {
        int back = tower_pack_count();
        if (bJustPressed || (aJustPressed && selectedOption == back)) {
            currentMenu = MENU_SHRINE;
            selectedOption = MENU_SHRINE_PACKS;
        } else if (aJustPressed) {
            buy_ask(tower_pack_name(selectedOption), tower_pack_cost(selectedOption), tower_pack_desc(selectedOption));
        }
    } else if (currentMenu == MENU_OFFERINGS) {
        int back = tower_offer_count();
        if (bJustPressed || (aJustPressed && selectedOption == back)) {
            currentMenu = MENU_TOWER;
            selectedOption = MENU_TOWER_OFFERINGS;
        } else if (aJustPressed && tower_offer_set(selectedOption)) {      // take it back: a refund, at once (nothing spent)
            if (tower_offer_toggle(selectedOption)) save_controller_save_settings();
        } else if (aJustPressed && !tower_offer_why(selectedOption)[0]) {     // set it aside: Embers spent, so asked first
            buy_ask(tower_offer_name(selectedOption), tower_offer_price(selectedOption), tower_offer_brief(selectedOption));
        }
    } else if (currentMenu == MENU_CODEX) {
        // the page's topics and the Back row at its foot (-1): Up/Down along them, A opens a topic (or takes Back),
        // Left/Right turn the pages (the cursor on the same row, or the last), B back from anywhere
        int rows = codex_rows_on_page();
        if (bJustPressed || (aJustPressed && codexRow < 0)) { currentMenu = codexReturn; selectedOption = codexReturnOption; return; }
        if (downJustPressed) codexRow = codexRow + 1 < rows ? codexRow + 1 : -1;
        else if (upJustPressed) codexRow = codexRow < 0 ? rows - 1 : codexRow - 1;
        else if (aJustPressed && tower_codex_open_ok(codexPager.page * codex_per_page() + codexRow)) tower_codex_open(codexPager.page * codex_per_page() + codexRow);
        else if (ui_pager_input(&codexPager, leftJustPressed, rightJustPressed, false, false) == UI_PAGER_TURNED && codexRow >= codex_rows_on_page()) codexRow = codex_rows_on_page() - 1;
    } else if (currentMenu == MENU_SKINS) {
        if (bJustPressed || (aJustPressed && selectedOption == SKIN_COUNT + 1)) {
            currentMenu = MENU_SETTINGS;
            selectedOption = MENU_SETTINGS_SKIN;
        } else if (aJustPressed && selectedOption == SKIN_COUNT) {     // Sunforged: on/off once floor 100 is reached
            if (save_controller_sunforged_unlocked()) { skill_fx_set_palette(!skill_fx_palette()); (void)save_controller_save_settings(); }
#ifdef HLE_AUTOPLAY
            debugf("[sunforged] Skins row: unlocked %d, now %s\n", save_controller_sunforged_unlocked(), skill_fx_palette() ? "on" : "off");
#endif
        } else if (aJustPressed && save_controller_skin_unlocked(selectedOption) && selectedOption != character_skin_get()) {
            character_skin_set(selectedOption);
            (void)save_controller_save_settings();
        }
    }
}

// the lists with 12 px icons (their sprites held while one is open)
static bool list_icons_menu(int m) { return m == MENU_CODEX || m == MENU_SHRINE || m == MENU_RELICS || m == MENU_UNLOCKS || m == MENU_OFFERINGS; }
void menu_controller_update(void)
{
    if (currentMenu != MENU_CONTROLS || !menuActive) controls_icons(false);
    if (!tower_active() && (currentMenu != MENU_CODEX || !menuActive)) tower_icons_free();     // (the emblems: a climb's, or the Codex's)
    if (!menuActive || !list_icons_menu(currentMenu)) tower_list_icons_free();
    tower_money_idle();      // (the gold and Embers icons, freed once no screen draws them)
    menu_controller_update_for_state(
        scene_get_game_state(),
        scene_is_cutscene_active()
    );
}

void menu_controller_update_title(void)
{
    if (currentMenu != MENU_CONTROLS || !menuActive) controls_icons(false);
    if (!tower_active() && (currentMenu != MENU_CODEX || !menuActive)) tower_icons_free();     // (the emblems: a climb's, or the Codex's)
    if (!menuActive || !list_icons_menu(currentMenu)) tower_list_icons_free();
    tower_money_idle();      // (the gold and Embers icons, freed once no screen draws them)
    menu_controller_update_for_state(
        GAME_STATE_TITLE,
        false
    );
}

// Beaten (the story cleared, a Bonus boss slain on this save): gold text and a small
// gold diamond after it
static void draw_menu_line(int x, int y, int w, int h, rdpq_align_t align, const char *text, bool beaten)
{
    rdpq_textparms_t tp = { .align = align, .width = w, .height = h, .wrap = WRAP_WORD,
                            .style_id = beaten ? FONT_STYLE_GOLD : FONT_STYLE_WHITE };
    int nb = strlen(text);
    rdpq_paragraph_t *pg = rdpq_paragraph_build(&tp, FONT_UNBALANCED, text, &nb);
    ui_track(x + (int)pg->bbox.x0, y + (int)pg->bbox.y0, x + (int)pg->bbox.x1 + 1, y + (int)pg->bbox.y1 + 1);
    rdpq_paragraph_render(pg, x, y);
    int mx = x + (int)pg->bbox.x1 + 5, my = y + (int)((pg->bbox.y0 + pg->bbox.y1) * 0.5f);
    rdpq_paragraph_free(pg);
    if (!beaten) return;
    static const uint8_t ROW[7] = { 1, 3, 5, 7, 5, 3, 1 };
    rdpq_sync_pipe();
    rdpq_set_mode_standard();
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_set_prim_color(RGBA32(255, 200, 90, 255));
    for (int r = 0; r < 7; r++) rdpq_fill_rectangle(mx + 3 - ROW[r] / 2, my - 3 + r, mx + 3 - ROW[r] / 2 + ROW[r], my - 2 + r);
}

// The standard box (ui_pages.h: THE MENU STANDARD) over r (fit to its content: menu_controller_draw), its title on the top row
static bool s_fitOn; static UiRect s_fitBox;     // the box ui_fit gave this frame (else the box UI_PAD round r)
static UiRect menu_box(UiRect r, const char *title)
{
    if (s_fitOn) ui_box_exact(s_fitBox.x0, s_fitBox.y0, s_fitBox.x1, s_fitBox.y1);
    else ui_box(r.x0, r.y0, r.x1, r.y1);
    if (title) rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = r.x1 - r.x0 }, FONT_UNBALANCED, r.x0, r.y0 + UI_TITLE_DY, "%s", title);
    return r;
}
#define MB_X(r)     ((r).x0 + 8)
#define MB_W(r)     ((r).x1 - (r).x0 - 16)
#define MB_BODY(r)  ((r).y0 + UI_TITLE_DY + 24)          // the first row's baseline
#define MB_FOOT(r)  ((r).y1 - UI_FOOT_DY)                 // the footer's
#define MB_HELP(r)  (MB_FOOT(r) - 15)                     // a row's line of help, over the footer
// a centred list's cursor: its widest row's label and 12 each side (measured in the frame's first pass: s_rowMax), at least 96
static int s_rowW = 176, s_rowMax;
#define ROW_MIN 96
#define ROW_W s_rowW
// a centred row's label at its baseline y: tracked with its cursor's room (the cursor's own width isn't known yet when measuring)
static void row_fit(int x, int w, int y, const char *label)
{
    if (!ui_measuring()) return;
    int lw = ui_text_width(label), cx = x + w / 2;
    if (lw > s_rowMax) s_rowMax = lw;
    if (lw < ROW_MIN - 24) lw = ROW_MIN - 24;
    ui_track(cx - lw / 2 - 12, y - 12, cx + lw / 2 + 12, y + 4);
}
// a list row with its 12 px icon at its left (the Tower's lists: tower_list_icon by the name icon; NULL none), its text after it
static void menu_row_icon(UiRect r, int y, bool sel, int style, const char *left, const char *right, const char *icon)
{
    int x = MB_X(r), w = MB_W(r);
    if (sel) ui_select_box(x - 4, y - 12, x + w + 4, y + 4);
    if (icon && !ui_measuring()) tower_list_icon(icon, x, y - 10);     // (2 px clear of the box above and below)
    const int in = 16;      // (every row's text, None and Back too, in one column after the icons)
    rdpq_text_printf(&(rdpq_textparms_t){ .width = w - UI_RIGHT_W - in, .style_id = style }, FONT_UNBALANCED, x + in, y, "%s", left);
    if (right && right[0]) tower_money_text(x + w - UI_RIGHT_W - 40, y, UI_RIGHT_W + 40, ALIGN_RIGHT, style, right);     // (a price: its Embers icon)
}
// a list's row: centred (right NULL), or a name left and its state right; y its baseline
static void menu_row(UiRect r, int y, bool sel, int style, const char *left, const char *right)
{
    int x = MB_X(r), w = MB_W(r);
    if (!right) {
        row_fit(x, w, y, left);
        if (sel && !ui_measuring()) ui_select_box(x + (w - ROW_W) / 2, y - 12, x + (w + ROW_W) / 2, y + 4);
        rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = w, .style_id = style }, FONT_UNBALANCED, x, y, "%s", left);
        return;
    }
    if (sel) ui_select_box(x - 4, y - 12, x + w + 4, y + 4);
    rdpq_text_printf(&(rdpq_textparms_t){ .width = w - UI_RIGHT_W, .style_id = style }, FONT_UNBALANCED, x, y, "%s", left);
    if (right[0]) tower_money_text(x + w - UI_RIGHT_W - 40, y, UI_RIGHT_W + 40, ALIGN_RIGHT, style, right);
}
// a row's line of help over the footer (one line: the box ends under it; its room kept when a row has none)
static void menu_help(UiRect r, int style, const char *s)
{
    ui_track_foot(true);
    ui_track(MB_X(r) + MB_W(r) / 2, MB_HELP(r) - 10, MB_X(r) + MB_W(r) / 2, MB_HELP(r) + 3);
    if (s && s[0]) rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = MB_W(r), .style_id = style }, FONT_UNBALANCED, MB_X(r), MB_HELP(r), "%s", s);
    ui_track_foot(false);
}
// A list longer than the box holds (the Shrine's at a large overscan setting): a page of its rows at a time, the
// cursor's; Up/Down run on across them (past a page's end, the next), Left/Right turn a page (s_listPer), "1/2" in the footer
static int list_page(UiRect r, int y0, int pitch, int rows)
{
    UiRect sr = ui_safe_rect();     // (by the safe rect, as measured: the fit box holds as many, and both passes agree)
    int per = (MB_HELP(sr) - 14 - (y0 - r.y0 + sr.y0)) / pitch + 1;
    if (per < 3) per = 3;
    if (per >= rows) { s_listPer = 0; return rows; }
    s_listPer = per;
    char n[24]; snprintf(n, sizeof n, "%d/%d", selectedOption / per + 1, (rows + per - 1) / per);
    ui_track_foot(true);
    rdpq_text_printf(&(rdpq_textparms_t){ .style_id = FONT_STYLE_GREY }, FONT_UNBALANCED, MB_X(r), MB_FOOT(r), "%s", n);
    ui_track_foot(false);
    return per;
}
// a paged screen's Back row at the foot (A takes it when it's under the cursor)
static void menu_back_row(UiRect r, bool on) { ui_track_foot(true); ui_row(MB_X(r), MB_W(r), MB_FOOT(r), on, "Back"); ui_track_foot(false); }

// The Tower lists' title row: Embers left (gold), the title centred unless that would touch them (then just right of them)
static void tower_menu_title(UiRect r, const char *title)
{
    char e[24]; snprintf(e, sizeof e, TOWER_EMBERS " %d", tower_embers());
    int y = r.y0 + UI_TITLE_DY, x = MB_X(r), w = MB_W(r);
    tower_money_text(x, y, w, ALIGN_LEFT, FONT_STYLE_GOLD, e);
    int tw = ui_text_width(title), tx = x + (w - tw) / 2, after = x + 16 + ui_text_width(e + 1) + 10;     // (its icon: 16)
    if (tx < after) tx = after;
    rdpq_text_printf(&(rdpq_textparms_t){ .width = tw + 8 }, FONT_UNBALANCED, tx, y, "%s", title);
}

static void draw_icon_line(sprite_t *icon, int x, int rowBaselineY, int lineH, int w, const char *text)
{
    const int gap = 8;
    const int srcW = (icon && icon->width > 0) ? icon->width : lineH;
    const int srcH = (icon && icon->height > 0) ? icon->height : lineH;
    const int box = lineH;
    const int rowTop = rowBaselineY - lineH;

    float s = 1.0f;
    if (srcW > 0 && srcH > 0) {
        float sx = (float)box / (float)srcW;
        float sy = (float)box / (float)srcH;
        s = (sx < sy) ? sx : sy;
    }

    const int iconW = (int)(srcW * s);
    const int iconH = (int)(srcH * s);
    const int iconX = x + (box - iconW) / 2;
    const int iconY = rowTop + (box - iconH) / 2;
    ui_track(x, rowTop, x + box, rowTop + box);     // (the icon: inside the fit box too)

    rdpq_sync_pipe();
    rdpq_set_mode_standard();
    rdpq_mode_alphacompare(0);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);

    if (icon) {
        rdpq_sprite_blit(icon, iconX, iconY, &(rdpq_blitparms_t){
            .scale_x = s,
            .scale_y = s,
        });
    }

    rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
    rdpq_text_printf(&(rdpq_textparms_t){
        .align = ALIGN_LEFT,
        .width = w,
        .wrap = WRAP_WORD,
    }, FONT_UNBALANCED, x + box + gap, rowBaselineY, "%s", text ? text : "");
}

static void menu_body(UiRect r0);
void menu_controller_draw(void)
{
    if (!menuActive) {
        return;
    }
    // Details (R) alone over the scene: the menus' black is translucent, the menu's rows would show through it
    if (ui_details()->open && ui_details()->owner == UI_OWNER_MENU) { ui_panel_draw(ui_details()); return; }

    if (currentMenu == MENU_TITLE) {
        rdpq_sync_pipe();
        rdpq_set_mode_standard();
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_combiner(RDPQ_COMBINER_FLAT);

        const int menuX = ui_safe_margin_x() + 4;
        int y = 40;
        const int menuW = 140;
        const int lineHeight = 16;

        rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
        rdpq_text_printf(&(rdpq_textparms_t){
            .align = ALIGN_LEFT,
            .width = menuW,
            .wrap = WRAP_WORD,
        }, FONT_UNBALANCED, menuX, y, "Pandemonium");

        y += (lineHeight * 2) + 6;

        for (int i = 0; i < MENU_TITLE_COUNT; i++) {
            if (i == selectedOption) {
                ui_select_box(menuX - 6, y - lineHeight + 2, menuX + (menuW * 3) / 4 + 6, y + 6);
                rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
            } else {
                rdpq_set_prim_color(RGBA32(220, 220, 220, 255));
            }

            bool beaten = i == MENU_TITLE_STORY && save_controller_get_best_boss_time_ms() > 0;
            draw_menu_line(menuX, y, menuW, 0, ALIGN_LEFT, titleMenuOptions[i], beaten);

            y += lineHeight + 6;
        }

        return;
    }

    if (currentMenu == MENU_VIDEO && overscanCalibrating) {
        draw_overscan_corner_markers();
        return;
    }

    // the box fit to its content (ui_fit: one even padding): measured first (nothing reaches the screen), then drawn in
    // its rect; Stats/Build and the Codex keep the safe rect (their pages fill it)
    UiRect r = ui_safe_rect();
    if (currentMenu != MENU_RUN && currentMenu != MENU_CODEX) {
        s_rowMax = 0;
        ui_measure_begin(); menu_body(r); ui_measure_end();
        // three widths (the user's call): the short centred menus narrow, the lists wide; as tall as their content
        bool narrow = currentMenu == MENU_MAIN || currentMenu == MENU_SETTINGS || currentMenu == MENU_AUDIO || currentMenu == MENU_VIDEO
                   || currentMenu == MENU_CONTROLS || currentMenu == MENU_BONUS || currentMenu == MENU_TOWER || currentMenu == MENU_CREDITS;
        r = ui_fit(r, narrow ? UI_NARROW : UI_WIDE, &s_fitBox); s_fitOn = true;
        s_rowW = s_rowMax ? s_rowMax + 24 : 176;
        if (s_rowW < ROW_MIN) s_rowW = ROW_MIN;
        s_rowW = UI_NARROW - 2 * UI_EVEN;       // (a narrow box's cursor its width less the padding)
    }
    menu_body(r);
    s_fitOn = false;
    if (ui_details()->open && ui_details()->owner == UI_OWNER_MENU) ui_panel_draw(ui_details());     // Details (R), over the menu
}

static void menu_body(UiRect r0)
{
    static const char *const TITLE[] = {
        [MENU_MAIN] = "PAUSE MENU", [MENU_SETTINGS] = "SETTINGS", [MENU_AUDIO] = "AUDIO", [MENU_VIDEO] = "VIDEO", [MENU_CONTROLS] = "CONTROLS",
        [MENU_CREDITS] = "CREDITS", [MENU_LOAD_GAME] = "LOAD GAME", [MENU_BONUS] = "BONUS", [MENU_TOWER] = "TOWER", [MENU_SKINS] = "SKINS",
    };
    bool tower = currentMenu == MENU_SHRINE || currentMenu == MENU_RELICS || currentMenu == MENU_UNLOCKS || currentMenu == MENU_OFFERINGS;
    bool pager = currentMenu == MENU_RUN || currentMenu == MENU_CODEX;
    UiRect r = menu_box(r0, tower || pager ? NULL : TITLE[currentMenu]);
    const int x = MB_X(r), w = MB_W(r), P = 16;
    int y = MB_BODY(r);

    if (currentMenu == MENU_MAIN) {
        for (int i = 0; i < MENU_MAIN_COUNT; i++) {
            if (main_row_hidden(i)) continue;     // Tower runs only
            menu_row(r, y, i == selectedOption, FONT_STYLE_WHITE, mainMenuOptions[i], NULL);
            y += tower_active() ? P + 2 : P + 4;
        }
    } else if (currentMenu == MENU_SETTINGS) {
        y += 8;
        for (int i = 0; i < MENU_SETTINGS_COUNT; i++) {
            if (settings_row_hidden(i)) continue;     // (from a pause menu)
            char optionText[64];
            bool locked = i == MENU_SETTINGS_SKILLS && settingsHubReturnMenu != MENU_TITLE;
            const char *arg = locked ? (character_skills_enabled() ? "ON (title only)" : "OFF (title only)")
                                     : (character_skills_enabled() ? "ON" : "OFF");
            if (i == MENU_SETTINGS_SKIN) arg = character_skin_name(character_skin_get());
            snprintf(optionText, sizeof(optionText), settingsMenuOptions[i], arg);
            menu_row(r, y, i == selectedOption, locked ? FONT_STYLE_GREY : FONT_STYLE_WHITE, optionText, NULL);
            y += P + 4;
        }
    } else if (currentMenu == MENU_BONUS) {
        rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = w, .style_id = FONT_STYLE_GREY }, FONT_UNBALANCED, x, y, "Test your might");
        y += 2 * P + 4;
        const SaveData *sd = save_controller_get_slot_data(save_controller_get_active_slot());
        for (int i = 0; i < MENU_BONUS_COUNT; i++) {
            bool beaten = i < 3 && (save_progress_bonus_mask(sd) & (1 << i));
            row_fit(x, w, y, bonusMenuOptions[i]);
            if (i == selectedOption && !ui_measuring()) ui_select_box(x + (w - ROW_W) / 2, y - 12, x + (w + ROW_W) / 2, y + 4);
            draw_menu_line(x, y, w, 0, ALIGN_CENTER, bonusMenuOptions[i], beaten);
            y += P + 4;
        }
    } else if (currentMenu == MENU_TOWER) {
        UiRect sr = ui_safe_rect();
        if (sr.y1 - sr.y0 >= 190 + P + 2) {     // (its two lines of flavour where they fit: the Heat row took their room at the default overscan)
            rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = w, .style_id = FONT_STYLE_GREY }, FONT_UNBALANCED, x, y - 4, "Boss after boss\nEver higher");
            y += 2 * P + 2;
        }
        { char e[48]; snprintf(e, sizeof e, "Best floor %d   " TOWER_EMBERS " %d", tower_best_floor(), tower_embers()); tower_money_text(x, y, w, ALIGN_CENTER, FONT_STYLE_GOLD, e); }
        y += P;
        // the floor-100 title; else what waits set aside for the next climb
        const char *badge = tower_title_badge(), *offers = tower_offers_line();
        if (badge[0]) rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = w, .style_id = FONT_STYLE_RED }, FONT_UNBALANCED, x, y, "%s", badge);
        else if (offers[0]) rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = w, .style_id = FONT_STYLE_GREY }, FONT_UNBALANCED, x, y, "%s", offers);
        y += P + 8;
        for (int i = 0; i < MENU_TOWER_COUNT; i++, y += P + 2) {
            char heat[24];
            if (i == MENU_TOWER_HEAT) {
                int h = tower_heat();
                if (h) snprintf(heat, sizeof heat, "Heat: %d (+%d%%)", h, 5 * h);     // (tower.c HEAT_STEP) else snprintf(heat, sizeof heat, "Heat: off");
            }
            menu_row(r, y, i == selectedOption, i == MENU_TOWER_HEAT && tower_heat() ? FONT_STYLE_RED : FONT_STYLE_WHITE, i == MENU_TOWER_HEAT ? heat : towerMenuOptions[i], NULL);
        }
    } else if (currentMenu == MENU_SHRINE) {
        tower_menu_title(r, "SHRINE");
        int per = list_page(r, y, P, MENU_SHRINE_COUNT), first = selectedOption / per * per;
        for (int i = first; i < MENU_SHRINE_COUNT && i < first + per; i++, y += P) {
            char name[40], right[24] = "";
            if (i < MENU_SHRINE_UPGRADES) {
                int c = tower_upgrade_cost((UpgradeId)i);
                snprintf(name, sizeof name, "%s %d/%d", tower_upgrade_name((UpgradeId)i), tower_upgrade_level((UpgradeId)i), UPG_MAX_LEVEL);
                if (c < 0) snprintf(right, sizeof right, "max"); else snprintf(right, sizeof right, "%d" TOWER_EMBERS, c);
            } else if (i == MENU_SHRINE_RELIC) {
                snprintf(name, sizeof name, "Relics");
                snprintf(right, sizeof right, "%s", tower_relic_name(tower_relic_equipped()));      // the one worn
            } else if (i == MENU_SHRINE_NIGHTFARER) {
                snprintf(name, sizeof name, "Nightfarer (skin)");
                if (tower_nightfarer_cost() < 0) snprintf(right, sizeof right, "owned"); else snprintf(right, sizeof right, "%d" TOWER_EMBERS, tower_nightfarer_cost());
            } else if (i == MENU_SHRINE_PACKS) {
                int own = 0;
                for (int k = 0; k < tower_pack_count(); k++) own += tower_pack_owned(k);
                snprintf(name, sizeof name, "Unlocks");      // the Boon Packs, then the skills' B forms
                snprintf(right, sizeof right, "%d/%d", own, tower_pack_count());
            } else snprintf(name, sizeof name, "Back");
            // bought (the Nightfarer) or maxed: greyed out
            bool done = (i == MENU_SHRINE_NIGHTFARER && tower_nightfarer_cost() < 0) || (i < MENU_SHRINE_UPGRADES && tower_upgrade_cost((UpgradeId)i) < 0);
            menu_row_icon(r, y, i == selectedOption, done ? FONT_STYLE_GREY : FONT_STYLE_WHITE, name, right,
                          i < MENU_SHRINE_UPGRADES ? tower_upgrade_name((UpgradeId)i) : i == MENU_SHRINE_COUNT - 1 ? NULL : name);
        }
        // what the selected one does
        const char *desc = selectedOption < MENU_SHRINE_UPGRADES ? tower_upgrade_desc((UpgradeId)selectedOption)
                         : selectedOption == MENU_SHRINE_RELIC ? "Choose the relic you climb with"
                         : selectedOption == MENU_SHRINE_NIGHTFARER ? "A tracker's winged helm and blue coat"
                         : selectedOption == MENU_SHRINE_PACKS ? "New boons and skill forms"
                         : "Skills are learned in the climb";
        // too few Embers for it: why A buys nothing, red (as the Offerings' refusals)
        int c = selectedOption < MENU_SHRINE_UPGRADES ? tower_upgrade_cost((UpgradeId)selectedOption) : selectedOption == MENU_SHRINE_NIGHTFARER ? tower_nightfarer_cost() : -1;
        bool poor = c >= 0 && tower_embers() < c;
        menu_help(r, poor ? FONT_STYLE_RED : FONT_STYLE_GREY, poor ? "Not enough Embers" : desc);
    } else if (currentMenu == MENU_RELICS || currentMenu == MENU_SKINS || currentMenu == MENU_UNLOCKS) {
        // a list: name on the left, its state on the right, what the hovered one does below
        bool relics = currentMenu == MENU_RELICS, packs = currentMenu == MENU_UNLOCKS;
        if (relics || packs) tower_menu_title(r, relics ? "RELICS" : "UNLOCKS");
        int rows = get_menu_option_count(currentMenu);
        int per = list_page(r, y, packs ? P - 2 : P, rows), first = selectedOption / per * per;
        for (int i = first; i < rows && i < first + per; i++, y += packs ? P - 2 : P) {
            const char *name, *right = "";
            char cost[12];
            bool grey = false;
            if (i == rows - 1) name = "Back";
            else if (packs) {
                name = tower_pack_name(i);
                if (tower_pack_owned(i)) right = "owned";
                else { snprintf(cost, sizeof cost, "%d" TOWER_EMBERS, tower_pack_cost(i)); right = cost; grey = tower_embers() < tower_pack_cost(i); }
            } else if (relics) {
                int k = i - 1;      // row 0: None
                name = tower_relic_name(k);
                if (tower_relic_equipped() == k) right = "equipped";
                else if (k >= 0 && !tower_relic_owned(k) && tower_relic_cost(k) == -2) {      // a milestone's, not sold
                    snprintf(cost, sizeof cost, "floor %d", tower_relic_floor(k)); right = cost; grey = true;
                } else if (k >= 0 && !tower_relic_owned(k)) {
                    snprintf(cost, sizeof cost, "%d" TOWER_EMBERS, tower_relic_cost(k)); right = cost;
                    grey = tower_embers() < tower_relic_cost(k);       // can't afford it yet
                }
            } else if (i == SKIN_COUNT) {      // Sunforged, the floor-100 reward: the skills' colours
                name = "Sunforged skills";
                if (!save_controller_sunforged_unlocked()) { right = "locked"; grey = true; }
                else right = skill_fx_palette() ? "on" : "off";
            } else {
                name = character_skin_name(i);
                if (!save_controller_skin_unlocked(i)) { right = "locked"; grey = true; }
                else if (character_skin_get() == i) right = "worn";
            }
            if (relics || packs) menu_row_icon(r, y, i == selectedOption, grey ? FONT_STYLE_GREY : FONT_STYLE_WHITE, name, right, i == rows - 1 || (relics && i == 0) ? NULL : name);
            else menu_row(r, y, i == selectedOption, grey ? FONT_STYLE_GREY : FONT_STYLE_WHITE, name, right);
        }
        const char *desc = selectedOption == rows - 1 ? ""
                         : packs ? tower_pack_brief(selectedOption)
                         : relics ? (selectedOption == 0 ? "Climb without a relic" : tower_relic_desc(selectedOption - 1))
                         : selectedOption == SKIN_COUNT ? (save_controller_sunforged_unlocked() ? "Your skills in gold and white" : "It burns at the Tower's summit")
                         : save_controller_skin_unlocked(selectedOption) ? "" : skin_lock_hint(selectedOption);
        int c = selectedOption == rows - 1 ? -1 : packs ? tower_pack_cost(selectedOption) : relics && selectedOption > 0 ? tower_relic_cost(selectedOption - 1) : -1;
        bool poor = c >= 0 && tower_embers() < c;
        menu_help(r, poor ? FONT_STYLE_RED : FONT_STYLE_GREY, poor ? "Not enough Embers" : desc);
    } else if (currentMenu == MENU_OFFERINGS) {
        // the Offerings: name, its price now (or set, or why not), then Back; what's set aside over them; the hovered one's line below
        tower_menu_title(r, "OFFERINGS");
        int rows = get_menu_option_count(currentMenu), held = tower_offers_held();
        const char *line = tower_offers_line();
        rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = w, .style_id = line[0] ? FONT_STYLE_GOLD : FONT_STYLE_GREY },
                         FONT_UNBALANCED, x, y - 4, "%s", line[0] ? line : "For the next climb: two at most");
        y += P;
        int per = list_page(r, y, P - 1, rows), first = selectedOption / per * per;
        for (int i = first; i < rows && i < first + per; i++, y += P - 1) {
            char right[16] = "";
            bool grey = false, set = false;
            const char *name = i == rows - 1 ? "Back" : tower_offer_name(i);
            if (i < rows - 1) {
                set = tower_offer_set(i);
                const char *why = tower_offer_lock(i);
                int c = tower_offer_price(i);
                if (set) snprintf(right, sizeof right, "set");
                else if (why[0]) { snprintf(right, sizeof right, "locked"); grey = true; }
                else { snprintf(right, sizeof right, "%d" TOWER_EMBERS, c); grey = held >= 2 || tower_embers() < c; }
            }
            menu_row_icon(r, y, i == selectedOption, set ? FONT_STYLE_GOLD : grey ? FONT_STYLE_GREY : FONT_STYLE_WHITE, name, right, i == rows - 1 ? NULL : name);
        }
        // why A can't set it aside in red, as the Shop's refusals; else what it does
        const char *why = selectedOption < rows - 1 ? tower_offer_why(selectedOption) : "";
        menu_help(r, why[0] ? FONT_STYLE_RED : FONT_STYLE_GREY, selectedOption < rows - 1 ? (why[0] ? why : tower_offer_brief(selectedOption)) : "");
    } else if (currentMenu == MENU_CODEX) {
        // the topics, paged (ui_pages), the corners on the title row
        ui_pager_draw(&codexPager, RUN_TX, RUN_CY, RUN_TW, "CODEX");
        int pitch = ui_line_pitch(), first = codexPager.page * codex_per_page();
        for (int k = 0; k < codex_rows_on_page(); k++) {
            int i = first + k, ry = RUN_BODY + k * pitch;
            bool open = tower_codex_open_ok(i);
            if (k == codexRow) ui_select_box(RUN_TX - 4, ry - 12, RUN_TX + RUN_TW + 4, ry + 4);
            if (open && !ui_measuring()) tower_list_icon(tower_codex_title(i), RUN_TX, ry - 10);     // (its 12 px icon; "???" none)
            rdpq_text_printf(&(rdpq_textparms_t){ .width = RUN_TW - UI_RIGHT_W - 16, .style_id = open ? FONT_STYLE_WHITE : FONT_STYLE_GREY }, FONT_UNBALANCED, RUN_TX + 16, ry, "%s", open ? tower_codex_title(i) : "???");
            if (!open) {
                char fl[16]; snprintf(fl, sizeof fl, "floor %d", tower_codex_floor(i));
                rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_RIGHT, .width = UI_RIGHT_W, .style_id = FONT_STYLE_GREY }, FONT_UNBALANCED, RUN_TX + RUN_TW - UI_RIGHT_W, ry, "%s", fl);
            }
        }
        menu_back_row(r, codexRow < 0);
    } else if (currentMenu == MENU_AUDIO) {
        y += 8;
        for (int i = 0; i < MENU_AUDIO_COUNT; i++, y += P + 2) {
            char optionText[64];
            const char *fmt = audioMenuOptions[i];
            if (i == MENU_AUDIO_MASTER_VOLUME) snprintf(optionText, sizeof optionText, fmt, audio_get_master_volume());
            else if (i == MENU_AUDIO_MUSIC_VOLUME) snprintf(optionText, sizeof optionText, fmt, audio_get_music_volume());
            else if (i == MENU_AUDIO_SFX_VOLUME) snprintf(optionText, sizeof optionText, fmt, audio_get_sfx_volume());
            else if (i == MENU_AUDIO_MUTE_TOGGLE) snprintf(optionText, sizeof optionText, fmt, audio_is_muted() ? "ON" : "OFF");
            else if (i == MENU_AUDIO_STEREO_MODE) snprintf(optionText, sizeof optionText, fmt, audio_get_stereo_mode() ? "Stereo" : "Mono");
            else if (i == MENU_AUDIO_RUMBLE_TOGGLE) {
                bool on = joypad_is_rumble_enabled();
                if (!(joypad_is_connected(JOYPAD_PORT_1) && joypad_get_rumble_supported(JOYPAD_PORT_1))) snprintf(optionText, sizeof optionText, on ? "Rumble: ON (No Pak)" : "Rumble: OFF (No Pak)");
                else snprintf(optionText, sizeof optionText, fmt, on ? "ON" : "OFF");
            } else snprintf(optionText, sizeof optionText, "%s", fmt);
            menu_row(r, y, i == selectedOption, FONT_STYLE_WHITE, optionText, NULL);
        }
    } else if (currentMenu == MENU_VIDEO) {
        y += 8;
        for (int i = 0; i < MENU_VIDEO_COUNT; i++, y += P + 2) {
            char optionText[64];
            if (i == MENU_VIDEO_ASPECT) snprintf(optionText, sizeof optionText, videoMenuOptions[i], hdAspect ? "16:9" : "4:3");
            else snprintf(optionText, sizeof optionText, "%s", videoMenuOptions[i]);
            menu_row(r, y, i == selectedOption, FONT_STYLE_WHITE, optionText, NULL);
        }
    } else if (currentMenu == MENU_CONTROLS) {
        controls_icons(true);
        // the two settings (Left/Right or A to change) and Back, then what each button does in that layout (its pitch
        // as the box holds it: 14, less at a large overscan setting)
        for (int i = 0; i < 3; i++, y += P) {
            char line[40];
            if (i == 0) snprintf(line, sizeof line, "Layout: %s", joypad_layout_name(joypad_get_layout()));
            else if (i == 1) snprintf(line, sizeof line, "Camera speed: %d", camera_get_speed());
            else snprintf(line, sizeof line, "Back");
            menu_row(r, y, i == selectedOption, FONT_STYLE_WHITE, line, NULL);
        }
        int lh = (MB_FOOT(r) - y) / 7;
        if (lh > 14) lh = 14;
        const int listW = 150, listX = x + (w - listW) / 2 + 12;
        int lay = joypad_get_layout();
        y -= 4;
        draw_icon_line(iconStick, listX, y += lh, lh, listW, "Move");
        draw_icon_line(iconA, listX, y += lh, lh, listW, lay == LAYOUT_SWAPPED ? "Attack" : "Dodge / Interact");
        draw_icon_line(iconB, listX, y += lh, lh, listW, lay == LAYOUT_SWAPPED ? "Dodge / Interact" : "Attack");
        draw_icon_line(iconZ, listX, y += lh, lh, listW, "Target");
        draw_icon_line(iconStart, listX, y += lh, lh, listW, "Pause Menu");
        draw_icon_line(iconCLeft, listX, y += lh, lh, listW, lay == LAYOUT_SKILLS_ON_C ? "Skills / Potion" : "Move Camera");
        draw_icon_line(iconDpad, listX, y += lh, lh, listW, lay == LAYOUT_SKILLS_ON_C ? "Move Camera" : "Skills / Potion");
    } else if (currentMenu == MENU_RUN) {
        // Stats then Build, paged (ui_pages), the corners on the title row
        run_pages_build();
        bool build = runPager.page >= s_run->statsPages;
        ui_pager_draw(&runPager, RUN_TX, RUN_CY, RUN_TW, build ? "BUILD" : "STATS");
        if (!build) ui_doc_draw(s_statsDoc, runPager.page, RUN_TX, RUN_BODY, RUN_TW);
        else {
            // the 3 skills, the 5 boon slots (each in its family's colour, its level on the right; a duo's two boons
            // under it), then the rivals locked out; the row under the cursor boxed (A or R: its Details)
            int pitch = ui_line_pitch(), first = (runPager.page - s_run->statsPages) * s_run->perPage;
            for (int k = 0; k < run_rows_on_page(); k++) {
                int i = first + k, ry = RUN_BODY + k * pitch + (i >= 3 && first == 0 ? 8 : 0);     // a gap between the skills and the slots
                if (k == runRow) ui_select_box(RUN_TX - 4, ry - 12, RUN_TX + RUN_TW + 4, ry + 4);
                bool noRight;     // its emblem (or a skill's icon, a padlock, a duo's two) left, its pips right (tower_build_row_marks)
                int in = tower_build_row_marks(i, RUN_TX, ry, RUN_TW, &noRight);
                rdpq_text_printf(&(rdpq_textparms_t){ .width = RUN_TW - UI_RIGHT_W - in, .style_id = s_run->style[i] }, FONT_UNBALANCED, RUN_TX + in, ry, "%s", s_run->left[i]);
                if (s_run->right[i][0] && !noRight)
                    rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_RIGHT, .width = UI_RIGHT_W, .style_id = s_run->style[i] }, FONT_UNBALANCED, RUN_TX + RUN_TW - UI_RIGHT_W, ry, "%s", s_run->right[i]);
            }
        }
        menu_back_row(r, runRow < 0);
    } else if (currentMenu == MENU_CREDITS) {
        static const char *const NAMES[] = { "FROMSCRATCH", "BoxingBruin", "HelloNewman", "Aethrone" };
        y += 8;
        for (int i = 0; i < 4; i++, y += P + (i == 1 ? 8 : 4))
            rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = w, .style_id = i ? FONT_STYLE_WHITE : FONT_STYLE_GOLD }, FONT_UNBALANCED, x, y, "%s", NAMES[i]);
        menu_row(r, y + 12, true, FONT_STYLE_WHITE, "Back", NULL);
    } else if (currentMenu == MENU_LOAD_GAME) {
        int boxY = r.y0 + UI_TITLE_DY + 10, boxH = (MB_FOOT(r) - 16 - boxY) / 3 - 4;     // (40 at the default overscan, less at a large one)
        if (boxH > 40) boxH = 40;
        const int gap = 4, bl = x, br = x + w;
        for (int slot = 0; slot < 3; slot++, boxY += boxH + gap) {
            const SaveData *saveData = save_controller_get_slot_data(slot);
            bool isSelected = slot == selectedOption, isLastPlayed = slot == save_controller_get_last_played_slot();
            // each slot a card: the cursor's frame white (as the selection box's), the last played gold, the others the box's
            rdpq_sync_pipe(); rdpq_set_mode_standard(); rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY); rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
            ui_track(bl, boxY, br, boxY + boxH);
            if (isSelected) { rdpq_set_prim_color(RGBA32(0, 0, 0, 140)); rdpq_fill_rectangle(bl, boxY, br, boxY + boxH); }     // (the selection box's black)
            rdpq_set_prim_color(isSelected ? RGBA32(255, 255, 255, 255) : isLastPlayed ? RGBA32(255, 215, 90, 255) : RGBA32(110, 100, 90, 255));
            rdpq_fill_rectangle(bl, boxY, br, boxY + 1);
            rdpq_fill_rectangle(bl, boxY + boxH - 1, br, boxY + boxH);
            rdpq_fill_rectangle(bl, boxY, bl + 1, boxY + boxH);
            rdpq_fill_rectangle(br - 1, boxY, br, boxY + boxH);
            rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = w, .style_id = isLastPlayed ? FONT_STYLE_GOLD : FONT_STYLE_WHITE }, FONT_UNBALANCED, bl, boxY + 12, "%s", loadGameMenuOptions[slot]);
            // used once played or set up (a skin or a setting changed stamps it, even before a fight)
            bool used = saveData && (saveData->last_played_timestamp > 0 || saveData->run_count > 0 || saveData->best_boss_time_ms > 0 ||
                                     save_progress_tower_best_floor(saveData) > 0 || save_progress_bonus_mask(saveData));
            if (used) {
                // the slot's progress: the story, the Tower, the Bonus bosses, the skins
                char l1[48], l2[48], story[16] = "-";
                uint32_t ms = saveData->best_boss_time_ms;
                if (ms) snprintf(story, sizeof story, "%lu:%02lu", (unsigned long)(ms / 60000), (unsigned long)(ms / 1000 % 60));
                int bm = save_progress_bonus_mask(saveData);
                snprintf(l1, sizeof l1, "Story %s   Tower %d", story, save_progress_tower_best_floor(saveData));
                snprintf(l2, sizeof l2, "Bonus %d/3   Skins %d/%d", (bm & 1) + ((bm >> 1) & 1) + ((bm >> 2) & 1),
                         save_progress_skins_unlocked(saveData), SAVE_SKINS_TO_EARN);
                rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = w, .style_id = FONT_STYLE_GREY }, FONT_UNBALANCED, bl, boxY + boxH / 2 + 3, "%s", l1);
                rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = w, .style_id = FONT_STYLE_GREY }, FONT_UNBALANCED, bl, boxY + boxH - 4, "%s", l2);
            } else rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = w, .style_id = FONT_STYLE_GREY }, FONT_UNBALANCED, bl, boxY + boxH - 10, "Empty");
        }
        menu_row(r, boxY + 12, selectedOption == MENU_LOAD_GAME_BACK, FONT_STYLE_WHITE, loadGameMenuOptions[MENU_LOAD_GAME_BACK], NULL);
    }
}

void menu_controller_free(void)
{
    gpu_wait();


    if (iconA) {
        sprite_free_shared(iconA);
        iconA = NULL;
    }

    controls_icons(false);
}

#ifdef HLE_AUTOPLAY
int menu_controller_test_state(int *option) { if (option) *option = selectedOption; return currentMenu; }     // test aid
// the cursor (a pager's row: -1 its corners, *corner which), the Back row (-1: a pager's Back corner); 100 the overscan screen
int menu_controller_test_nav(int *row, int *back, int *corner)
{
    *corner = 0;
    if (currentMenu == MENU_VIDEO && overscanCalibrating) { *row = 0; *back = -2; return 100; }
    if (currentMenu == MENU_RUN) { *row = runRow; *back = -1; *corner = runPager.page; return currentMenu; }     // (a pager's Back row -1; *corner its page)
    if (currentMenu == MENU_CODEX) { *row = codexRow; *back = -1; *corner = codexPager.page; return currentMenu; }
    *row = selectedOption;
    *back = currentMenu == MENU_MAIN ? MENU_MAIN_RESUME : currentMenu == MENU_TITLE ? -3 : get_menu_option_count(currentMenu) - 1;
    return currentMenu;
}
int menu_controller_test_run(int *page, int *pages, int *row) { *page = runPager.page; *pages = runPager.pages; *row = runRow; return runRow < 0; }     // (1: on Back)
int menu_controller_test_pages(int *per) { *per = currentMenu == MENU_CODEX ? codex_per_page() : currentMenu == MENU_RUN ? 0 : s_listPer; return currentMenu == MENU_RUN ? runPager.pages : currentMenu == MENU_CODEX ? codexPager.pages : s_listPer ? (get_menu_option_count(currentMenu) + s_listPer - 1) / s_listPer : 1; }
#endif
bool menu_controller_is_active(void)
{
    return menuActive;
}

bool menu_controller_is_title_submenu_active(void)
{
    return menuActive && menuIsTitleMenu && currentMenu != MENU_TITLE;
}

bool menu_controller_is_pause_menu_active(void)
{
    return menuActive && !menuIsTitleMenu;
}

void menu_controller_toggle(void)
{
    menuActive = !menuActive;
    if (ui_details()->owner == UI_OWNER_MENU) ui_panel_close(ui_details());
    run_pages_free();

    if (menuActive) {
        joypad_rumble_stop();
        // the press that opened it (Start, or B on the Tower's map) isn't a press in it
        lastAPressed = btn.a; lastBPressed = btn.b; lastRPressed = btn.z;
        lastUpPressed = btn.d_up; lastDownPressed = btn.d_down; lastLeftPressed = btn.d_left; lastRightPressed = btn.d_right;

        GameState state = scene_get_game_state();

        menuIsTitleMenu = state == GAME_STATE_TITLE;
        parentMenu = menuIsTitleMenu ? MENU_TITLE : MENU_MAIN;
        parentSelectedOption = 0;

        if (menuIsTitleMenu) {
            currentMenu = MENU_TITLE;
        } else {
            menuReturnState = state;

            if (state != GAME_STATE_VICTORY) {
                scene_set_game_state(GAME_STATE_MENU);
            }

            currentMenu = MENU_MAIN;
        }

        selectedOption = 0;

        if (!menuIsTitleMenu) {
            musicWasPaused = !audio_is_music_playing();
            if (!musicWasPaused) {
                audio_pause_music();
            }
        }
    } else {
        joypad_rumble_stop();

        overscanCalibrating = false;

        if (!menuIsTitleMenu) {
            scene_set_game_state(menuReturnState);

            if (!musicWasPaused) {
                audio_resume_music();
            }
        }
    }
}

void menu_controller_open_pause(int option)
{
    if (!menuActive) menu_controller_toggle();
    currentMenu = MENU_MAIN;
    selectedOption = option;
}

void menu_controller_close(void)
{
    if (ui_details()->owner == UI_OWNER_MENU) ui_panel_close(ui_details());
    run_pages_free();
    menuActive = false;
    joypad_rumble_stop();
    overscanCalibrating = false;

    if (!menuIsTitleMenu) {
        scene_set_game_state(menuReturnState);

        if (!musicWasPaused) {
            audio_resume_music();
        }
    }
}