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
#include "../game/tower/tower.h"
#include "../utilities/ui_pages.h"

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

// Pause menu background
static sprite_t *pauseMenuBg = NULL;
static surface_t pauseMenuBgSurf = {0};

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
static bool lastRPressed = false;      // R: Details (no layout moves it)
static bool lastStickUp = false;
static bool lastStickDown = false;
static bool lastStickLeft = false;
static bool lastStickRight = false;

// Overscan calibration overlay (inside Video settings)
static bool overscanCalibrating = false;
static int8_t overscanPrevX = 0;
static int8_t overscanPrevY = 0;

// The Stats menu: the run's numbers, then its build, as one paged list (ui_pages: the corners); on a Build
// page Up/Down move a row cursor (-1: the corners) and A or R opens the row's Details
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
#define RUN_BODY_END (SCREEN_HEIGHT - run_my() - 18)    // the last line's (204)
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
// The Codex: its topics, paged as Stats is (the corners; Up/Down along the rows, Down past a page's last to the next;
// A or R opens a topic in the Details panel). From the Tower menu or the pause menu (a climb's)
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
        case MENU_SKINS:     return SKIN_COUNT + 1;
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
    }, FONT_UNBALANCED, 0, textY + 16, "D-PAD: adjust   A: save   B: cancel");

    rdpq_text_printf(&(rdpq_textparms_t){
        .align = ALIGN_CENTER,
        .width = SCREEN_WIDTH,
        .wrap = WRAP_WORD,
    }, FONT_UNBALANCED, 0, textY + 34, "X: %d   Y: %d", (int)uiOverscanX, (int)uiOverscanY);
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

    pauseMenuBg = sprite_load_shared("rom:/dialog_vertical.ia8.sprite");     // (the dialog box's too)
    if (pauseMenuBg) {
        pauseMenuBgSurf = sprite_get_pixels(pauseMenuBg);
    }

    iconA = sprite_load_shared("rom:/buttons/A.sprite");
    // (the Controls page's other icons load with it: controls_icons)
}

MenuAction menu_controller_consume_action(void)
{
    MenuAction action = pendingAction;
    pendingAction = MENU_ACTION_NONE;
    return action;
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
    bool rJustPressed = btn.r && !lastRPressed;
    lastRPressed = btn.r;

    lastUpPressed = upPressed;
    lastDownPressed = downPressed;
    lastLeftPressed = leftPressed;
    lastRightPressed = rightPressed;
    lastAPressed = aPressed;
    lastBPressed = bPressed;

    if (state == GAME_STATE_TITLE && startJustPressed) {
        aJustPressed = true;
    }

    // a Details panel opened here (R on the Build page or the Shrine) takes every press until it closes
    UiPanel *dp = ui_details();
    if (dp->open && dp->owner == UI_OWNER_MENU) {
        ui_panel_input(dp, upJustPressed, downJustPressed, leftJustPressed, rightJustPressed, aJustPressed, bJustPressed, rJustPressed);
        return;
    }
    if (rJustPressed) {     // R: what the row under the cursor does in full
        if (currentMenu == MENU_SHRINE) {
            if (selectedOption < MENU_SHRINE_UPGRADES) tower_details_shrine(TOWER_SHRINE_UPGRADE, selectedOption);
            else if (selectedOption == MENU_SHRINE_RELIC) tower_details_shrine(TOWER_SHRINE_RELIC, tower_relic_equipped());
            else if (selectedOption == MENU_SHRINE_NIGHTFARER) tower_details_shrine(TOWER_SHRINE_NIGHTFARER, 0);
        } else if (currentMenu == MENU_RELICS && selectedOption <= tower_relic_total()) tower_details_shrine(TOWER_SHRINE_RELIC, selectedOption - 1);
        else if (currentMenu == MENU_UNLOCKS && selectedOption < tower_pack_count()) tower_details_shrine(TOWER_SHRINE_UNLOCK, selectedOption);
        else if (currentMenu == MENU_OFFERINGS && selectedOption < tower_offer_count()) tower_details_shrine(TOWER_SHRINE_OFFERING, selectedOption);
        if (dp->open) return;
    }

    if (!(currentMenu == MENU_VIDEO && overscanCalibrating)) {
        int maxOptions = get_menu_option_count(currentMenu);

        if (upJustPressed) {
            selectedOption--;
            if (selectedOption < 0) {
                selectedOption = maxOptions - 1;
            }
            while (currentMenu == MENU_MAIN && main_row_hidden(selectedOption)) selectedOption--;   // Tower only
        }

        if (downJustPressed) {
            selectedOption++;
            if (selectedOption >= maxOptions) {
                selectedOption = 0;
            }
            while (currentMenu == MENU_MAIN && main_row_hidden(selectedOption)) selectedOption++;
        }
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
                    ui_pager_reset(&runPager, runPager.pages);     // page 0, on Next
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
        // the pages' buttons in the title row's corners (Back | Next >, < Prev | Next >, < Prev): Left/Right pick
        // a corner, A takes it, B closes from any page. On a Build page Up/Down walk its rows; A or R: the row's Details
        run_pages_build();
        int rows = run_rows_on_page();
        if (runRow >= rows) runRow = rows - 1;
        if (rows && downJustPressed) runRow = runRow + 1 < rows ? runRow + 1 : runRow;
        else if (rows && upJustPressed) runRow = runRow >= 0 ? runRow - 1 : -1;
        int at = (runPager.page - s_run->statsPages) * s_run->perPage + runRow;
        if (runRow >= 0 && !bJustPressed && !leftJustPressed && !rightJustPressed) {
            if ((aJustPressed || rJustPressed) && s_run->card[at] >= 0) tower_details_card(s_run->card[at]);
        } else {
            if (leftJustPressed || rightJustPressed) runRow = -1;
            int r = ui_pager_input(&runPager, leftJustPressed, rightJustPressed, aJustPressed, bJustPressed);
            if (r == UI_PAGER_BACK) { currentMenu = MENU_MAIN; selectedOption = MENU_MAIN_RUN; run_pages_free(); }
            else if (r == UI_PAGER_TURNED) runRow = -1;
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
            if (tower_nightfarer_buy()) save_controller_save_settings();
        } else if (selectedOption == MENU_SHRINE_PACKS && aJustPressed) {
            currentMenu = MENU_UNLOCKS;
            selectedOption = 0;
        } else if (aJustPressed && selectedOption < MENU_SHRINE_UPGRADES) {
            if (tower_upgrade_buy((UpgradeId)selectedOption)) save_controller_save_settings();
        }
    } else if (currentMenu == MENU_RELICS) {
        int back = tower_relic_total() + 1;
        if (bJustPressed || (aJustPressed && selectedOption == back)) {
            currentMenu = MENU_SHRINE;
            selectedOption = MENU_SHRINE_RELIC;
        } else if (aJustPressed && selectedOption - 1 != tower_relic_equipped()) {
            if (tower_relic_select(selectedOption - 1)) save_controller_save_settings();     // buy and equip, equip, or None
        }
    } else if (currentMenu == MENU_UNLOCKS) {
        int back = tower_pack_count();
        if (bJustPressed || (aJustPressed && selectedOption == back)) {
            currentMenu = MENU_SHRINE;
            selectedOption = MENU_SHRINE_PACKS;
        } else if (aJustPressed && tower_pack_buy(selectedOption)) {
            save_controller_save_settings();
        }
    } else if (currentMenu == MENU_OFFERINGS) {
        int back = tower_offer_count();
        if (bJustPressed || (aJustPressed && selectedOption == back)) {
            currentMenu = MENU_TOWER;
            selectedOption = MENU_TOWER_OFFERINGS;
        } else if (aJustPressed && tower_offer_toggle(selectedOption)) {      // set it aside, or take it back
            save_controller_save_settings();
        }
    } else if (currentMenu == MENU_CODEX) {
        // the corners (row -1) and the topics: Up/Down along them (Down past a page's last: the next page's first), A or R opens one;
        // Left/Right go to the corners and pick one, A takes it; B back from anywhere
        int rows = codex_rows_on_page(), per = codex_per_page();
        if (bJustPressed) { currentMenu = codexReturn; selectedOption = codexReturnOption; return; }
        if (downJustPressed) {
            if (codexRow + 1 < rows) codexRow++;
            else if (codexPager.page + 1 < codexPager.pages) { codexPager.page++; codexRow = 0; codexPager.sel = codexPager.page + 1 < codexPager.pages; }
        } else if (upJustPressed) codexRow = codexRow >= 0 ? codexRow - 1 : -1;
        else if (codexRow >= 0 && (leftJustPressed || rightJustPressed)) { codexRow = -1; codexPager.sel = rightJustPressed && codexPager.page + 1 < codexPager.pages; }
        else if (codexRow >= 0 && (aJustPressed || rJustPressed)) tower_codex_open(codexPager.page * per + codexRow);
        else if (codexRow < 0) {
            int r = ui_pager_input(&codexPager, leftJustPressed, rightJustPressed, aJustPressed, false);
            if (r == UI_PAGER_BACK) { currentMenu = codexReturn; selectedOption = codexReturnOption; }
            else if (r == UI_PAGER_TURNED) codexRow = -1;     // (on the corners, as Stats: A again turns on)
        }
    } else if (currentMenu == MENU_SKINS) {
        if (bJustPressed || (aJustPressed && selectedOption == SKIN_COUNT)) {
            currentMenu = MENU_SETTINGS;
            selectedOption = MENU_SETTINGS_SKIN;
        } else if (aJustPressed && save_controller_skin_unlocked(selectedOption) && selectedOption != character_skin_get()) {
            character_skin_set(selectedOption);
            (void)save_controller_save_settings();
        }
    }
}

void menu_controller_update(void)
{
    if (currentMenu != MENU_CONTROLS || !menuActive) controls_icons(false);
    menu_controller_update_for_state(
        scene_get_game_state(),
        scene_is_cutscene_active()
    );
}

void menu_controller_update_title(void)
{
    if (currentMenu != MENU_CONTROLS || !menuActive) controls_icons(false);
    menu_controller_update_for_state(
        GAME_STATE_TITLE,
        false
    );
}

static void draw_pause_menu_background(int x, int y, int dialogWidth, int dialogHeight)
{
    if (!pauseMenuBg || pauseMenuBgSurf.width <= 0 || pauseMenuBgSurf.height <= 0) {
        rdpq_set_prim_color(RGBA32(40, 35, 30, 180));
        rdpq_fill_rectangle(x, y, x + dialogWidth, y + dialogHeight);
        return;
    }

    int imgW = pauseMenuBgSurf.width;
    int imgH = pauseMenuBgSurf.height;

    const float maxScaleX = (float)dialogWidth / (float)imgW;
    const float maxScaleY = (float)dialogHeight / (float)imgH;
    const float fitScale = (maxScaleX < maxScaleY) ? maxScaleX : maxScaleY;

    const float heightShrink = 0.98f;
    const float baseScale = fitScale * heightShrink;

    const float extraXStretch = 1.15f;
    float scaleX = baseScale * extraXStretch;
    if (scaleX > maxScaleX) scaleX = maxScaleX;

    const float scaleY = baseScale;

    const int drawW = (int)(imgW * scaleX);
    const int drawH = (int)(imgH * scaleY);
    const int drawX = x + (dialogWidth - drawW) / 2;
    const int drawY = y + (dialogHeight - drawH) / 2;

    rdpq_mode_alphacompare(1);
    rdpq_tex_blit(&pauseMenuBgSurf, drawX, drawY, &(rdpq_blitparms_t){
        .scale_x = scaleX,
        .scale_y = scaleY,
    });
}

static void draw_menu_selection_highlight(int x, int y, int w, int h)
{
    const int padX = 6;
    const int padY = 2;
    const int x0 = x - padX;
    const int y0 = y - padY;
    const int x1 = x + w + padX;
    const int y1 = y + h + padY;

    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);

    rdpq_set_prim_color(RGBA32(0, 0, 0, 140));
    rdpq_fill_rectangle(x0, y0, x1, y1);

    rdpq_set_prim_color(RGBA32(255, 255, 255, 70));
    rdpq_fill_rectangle(x0, y0, x1, y0 + 1);
    rdpq_fill_rectangle(x0, y1 - 1, x1, y1);
    rdpq_fill_rectangle(x0, y0, x0 + 1, y1);
    rdpq_fill_rectangle(x1 - 1, y0, x1, y1);
}

static void draw_menu_selection_highlight_baseline(int x, int baselineY, int w, int h)
{
    const int padX = 6;
    const int padY = 2;
    const int x0 = x - padX;
    const int y0 = (baselineY - h) - padY + 2;
    const int x1 = x + w + padX;
    const int y1 = baselineY + padY + 4;

    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);

    rdpq_set_prim_color(RGBA32(0, 0, 0, 140));
    rdpq_fill_rectangle(x0, y0, x1, y1);
}

static void draw_menu_selection_highlight_centered(int contentX, int y, int contentW, int lineH, int maxW)
{
    int w = maxW;
    if (w > contentW) w = contentW;

    int x = contentX + (contentW - w) / 2;
    draw_menu_selection_highlight(x, y, w, lineH);
}

// Beaten (the story cleared, a Bonus boss slain on this save): gold text and a small
// gold diamond after it
static void draw_menu_line(int x, int y, int w, int h, rdpq_align_t align, const char *text, bool beaten)
{
    rdpq_textparms_t tp = { .align = align, .width = w, .height = h, .wrap = WRAP_WORD,
                            .style_id = beaten ? FONT_STYLE_GOLD : FONT_STYLE_WHITE };
    int nb = strlen(text);
    rdpq_paragraph_t *pg = rdpq_paragraph_build(&tp, FONT_UNBALANCED, text, &nb);
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

// The Shrine's title row: Embers left (5 digits), the title centred unless that would touch them (then just right of them)
static void tower_menu_title(int x, int y, int w, int h, const char *title)
{
    char e[24]; snprintf(e, sizeof e, "Embers %d", tower_embers());
    int tw = ui_text_width(title), tx = x + (w - tw) / 2, after = x + 4 + ui_text_width(e) + 10;
    if (tx < after) tx = after;
    rdpq_text_printf(&(rdpq_textparms_t){ .width = tw + 8, .height = h }, FONT_UNBALANCED, tx, y, "%s", title);
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

void menu_controller_draw(void)
{
    if (!menuActive) {
        return;
    }

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
                draw_menu_selection_highlight_baseline(menuX, y, (menuW * 3) / 4, lineHeight);
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

    if (currentMenu == MENU_LOAD_GAME) {
        rdpq_sync_pipe();
        rdpq_set_mode_standard();
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);

        const int dialogMarginX = 0;
        int dialogWidth = SCREEN_WIDTH - (dialogMarginX * 2);
        int dialogHeight = 230;

        dialogWidth = 260;

        int dialogX = (SCREEN_WIDTH - dialogWidth) / 2;
        int dialogY = (SCREEN_HEIGHT - dialogHeight) / 2;

        draw_pause_menu_background(dialogX, dialogY, dialogWidth, dialogHeight);

        rdpq_set_prim_color(RGBA32(0, 0, 0, 90));
        rdpq_fill_rectangle(dialogX, dialogY, dialogX + dialogWidth, dialogY + dialogHeight);

        rdpq_set_prim_color(RGBA32(180, 160, 120, 255));
        rdpq_fill_rectangle(dialogX, dialogY, dialogX + dialogWidth, dialogY + 3);
        rdpq_fill_rectangle(dialogX, dialogY + dialogHeight - 3, dialogX + dialogWidth, dialogY + dialogHeight);
        rdpq_fill_rectangle(dialogX, dialogY, dialogX + 3, dialogY + dialogHeight);
        rdpq_fill_rectangle(dialogX + dialogWidth - 3, dialogY, dialogX + dialogWidth, dialogY + dialogHeight);

        rdpq_set_prim_color(RGBA32(220, 200, 160, 255));
        rdpq_fill_rectangle(dialogX + 3, dialogY + 3, dialogX + dialogWidth - 3, dialogY + 4);
        rdpq_fill_rectangle(dialogX + 3, dialogY + 3, dialogX + 4, dialogY + dialogHeight - 3);

        const int paddingX = 15;
        const int paddingY = 20;
        const int contentX = dialogX + paddingX;
        const int contentY = dialogY + paddingY;
        const int contentW = dialogWidth - (paddingX * 2);
        const int titleYOffset = 8;

        rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
        rdpq_text_printf(&(rdpq_textparms_t){
            .align = ALIGN_CENTER,
            .width = contentW,
            .wrap = WRAP_WORD,
        }, FONT_UNBALANCED, contentX, contentY + titleYOffset, "LOAD GAME");

        int boxY = contentY + 34;
        const int boxHeight = 44;
        const int boxSpacing = 5;
        const int slotInnerPadX = 8;
        const int slotWidth = contentW;
        const int boxLeft = contentX + (contentW - slotWidth) / 2;
        const int boxRight = boxLeft + slotWidth;
        const int boxWidth = boxRight - boxLeft;

        for (int slot = 0; slot < 3; slot++) {
            const SaveData *saveData = save_controller_get_slot_data(slot);
            int lastPlayedSlot = save_controller_get_last_played_slot();
            bool isLastPlayed = slot == lastPlayedSlot;
            bool isSelected = slot == selectedOption;

            if (isSelected) {
                rdpq_sync_pipe();
                rdpq_set_mode_standard();
                rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
                rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
                rdpq_set_prim_color(RGBA32(0, 0, 0, 100));
                rdpq_fill_rectangle(boxLeft, boxY, boxRight, boxY + boxHeight);
            }

            rdpq_sync_pipe();
            rdpq_set_mode_standard();
            rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
            rdpq_set_prim_color(RGBA32(40, 35, 30, 200));
            rdpq_fill_rectangle(boxLeft, boxY, boxRight, boxY + boxHeight);

            rdpq_sync_pipe();
            rdpq_set_mode_standard();
            rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
            rdpq_mode_combiner(RDPQ_COMBINER_FLAT);

            if (isSelected) {
                rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
            } else if (isLastPlayed) {
                rdpq_set_prim_color(RGBA32(255, 255, 0, 255));
            } else {
                rdpq_set_prim_color(RGBA32(180, 160, 120, 255));
            }

            rdpq_fill_rectangle(boxLeft, boxY, boxRight, boxY + 1);
            rdpq_fill_rectangle(boxLeft, boxY + boxHeight - 1, boxRight, boxY + boxHeight);
            rdpq_fill_rectangle(boxLeft, boxY, boxLeft + 1, boxY + boxHeight);
            rdpq_fill_rectangle(boxRight - 1, boxY, boxRight, boxY + boxHeight);

            rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
            rdpq_text_printf(&(rdpq_textparms_t){
                .align = ALIGN_CENTER,
                .width = boxWidth - (slotInnerPadX * 2),
                .wrap = WRAP_WORD,
            }, FONT_UNBALANCED, boxLeft + slotInnerPadX, boxY + 11, "%s", loadGameMenuOptions[slot]);

            // used once played or set up (a skin or a setting changed stamps it, even before a fight)
            bool used = saveData && (saveData->last_played_timestamp > 0 || saveData->run_count > 0 || saveData->best_boss_time_ms > 0 ||
                                     save_progress_tower_best_floor(saveData) > 0 || save_progress_bonus_mask(saveData));
            if (used) {
                // The slot's progress: the story, the Tower, the Bonus bosses, the skins
                char l1[48], l2[48], story[16] = "-";
                uint32_t ms = saveData->best_boss_time_ms;
                if (ms) snprintf(story, sizeof story, "%lu:%02lu", (unsigned long)(ms / 60000), (unsigned long)(ms / 1000 % 60));
                int bm = save_progress_bonus_mask(saveData);
                snprintf(l1, sizeof l1, "Story %s   Tower %d", story, save_progress_tower_best_floor(saveData));
                snprintf(l2, sizeof l2, "Bonus %d/3   Skins %d/%d", (bm & 1) + ((bm >> 1) & 1) + ((bm >> 2) & 1),
                         save_progress_skins_unlocked(saveData), SAVE_SKINS_TO_EARN);
                rdpq_set_prim_color(RGBA32(200, 200, 200, 255));
                rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = boxWidth - (slotInnerPadX * 2) },
                                 FONT_UNBALANCED, boxLeft + slotInnerPadX, boxY + 24, "%s", l1);
                rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = boxWidth - (slotInnerPadX * 2) },
                                 FONT_UNBALANCED, boxLeft + slotInnerPadX, boxY + 36, "%s", l2);
            } else {
                rdpq_set_prim_color(RGBA32(150, 150, 150, 255));

                rdpq_text_printf(&(rdpq_textparms_t){
                    .align = ALIGN_CENTER,
                    .width = boxWidth - (slotInnerPadX * 2),
                    .wrap = WRAP_WORD,
                }, FONT_UNBALANCED, boxLeft + slotInnerPadX, boxY + 28, "Empty");
            }

            boxY += boxHeight + boxSpacing;
        }

        int backY = boxY + 8;
        if (selectedOption == MENU_LOAD_GAME_BACK) {
            const int highlightH = 16;
            const int highlightY = backY - (highlightH / 2) - 2;
            draw_menu_selection_highlight_centered(contentX, highlightY, contentW, highlightH, 103);
            rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
        } else {
            rdpq_set_prim_color(RGBA32(220, 220, 220, 255));
        }

        const int backButtonWidth = contentW / 2;
        const int backButtonX = contentX + (contentW - backButtonWidth) / 2;

        rdpq_text_printf(&(rdpq_textparms_t){
            .align = ALIGN_CENTER,
            .width = backButtonWidth,
            .wrap = WRAP_WORD,
        }, FONT_UNBALANCED, backButtonX, backY, "%s", loadGameMenuOptions[MENU_LOAD_GAME_BACK]);

        return;
    }

    if (currentMenu == MENU_VIDEO && overscanCalibrating) {
        draw_overscan_corner_markers();
        return;
    }

    const int dialogMarginX = 0;
    int dialogWidth = SCREEN_WIDTH - (dialogMarginX * 2);
    int dialogHeight = 200;

    if (currentMenu == MENU_SETTINGS || currentMenu == MENU_AUDIO || currentMenu == MENU_VIDEO) {
        dialogHeight = 230;
    } else if (currentMenu == MENU_CONTROLS || currentMenu == MENU_CREDITS || currentMenu == MENU_SHRINE || currentMenu == MENU_RUN ||
               currentMenu == MENU_RELICS || currentMenu == MENU_SKINS || currentMenu == MENU_UNLOCKS || currentMenu == MENU_OFFERINGS ||
               currentMenu == MENU_CODEX || currentMenu == MENU_TOWER || (currentMenu == MENU_MAIN && tower_active())) {
        dialogHeight = 230;     // (the Tower's menu: five rows and its records; a climb's pause menu: seven rows)
    }

    if (currentMenu == MENU_CONTROLS) {
        dialogWidth = 320;
    } else if (
        currentMenu == MENU_SETTINGS ||
        currentMenu == MENU_AUDIO ||
        currentMenu == MENU_VIDEO ||
        currentMenu == MENU_CREDITS ||
        currentMenu == MENU_BONUS ||
        currentMenu == MENU_TOWER
    ) {
        dialogWidth = 260;
    } else if (currentMenu == MENU_SHRINE || currentMenu == MENU_RUN || currentMenu == MENU_RELICS || currentMenu == MENU_SKINS || currentMenu == MENU_UNLOCKS ||
               currentMenu == MENU_OFFERINGS || currentMenu == MENU_CODEX) {
        dialogWidth = 300;
    }

    if (currentMenu == MENU_RUN || currentMenu == MENU_CODEX) {     // (framing their box: 300 x 230 at the 7.5% area, as the others)
        dialogWidth = SCREEN_WIDTH - 2 * (RUN_TX - 20);
        dialogHeight = SCREEN_HEIGHT - 2 * (RUN_CY - 29);
    }

    rdpq_sync_pipe();
    rdpq_set_mode_standard();
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);

    int x = (SCREEN_WIDTH - dialogWidth) / 2;
    int y = (SCREEN_HEIGHT - dialogHeight) / 2;

    draw_pause_menu_background(x, y, dialogWidth, dialogHeight);

    // the Tower's menus darker: their rows sit over the title's bright fire or the hall's light (the dialog's art doesn't show on Delta's renderer)
    bool towerMenu = currentMenu == MENU_TOWER || currentMenu == MENU_SHRINE || currentMenu == MENU_RELICS || currentMenu == MENU_UNLOCKS || currentMenu == MENU_RUN ||
                     currentMenu == MENU_OFFERINGS || currentMenu == MENU_CODEX;
    if (towerMenu) { rdpq_mode_alphacompare(0); rdpq_mode_combiner(RDPQ_COMBINER_FLAT); }     // (a flat fill: the art's blit left a texture combiner)
    rdpq_set_prim_color(RGBA32(0, 0, 0, towerMenu ? 200 : 90));
    rdpq_fill_rectangle(x, y, x + dialogWidth, y + dialogHeight);

    rdpq_set_prim_color(RGBA32(180, 160, 120, 255));
    rdpq_fill_rectangle(x, y, x + dialogWidth, y + 3);
    rdpq_fill_rectangle(x, y + dialogHeight - 3, x + dialogWidth, y + dialogHeight);
    rdpq_fill_rectangle(x, y, x + 3, y + dialogHeight);
    rdpq_fill_rectangle(x + dialogWidth - 3, y, x + dialogWidth, y + dialogHeight);

    rdpq_set_prim_color(RGBA32(220, 200, 160, 255));
    rdpq_fill_rectangle(x + 3, y + 3, x + dialogWidth - 3, y + 4);
    rdpq_fill_rectangle(x + 3, y + 3, x + 4, y + dialogHeight - 3);

    const int paddingX = 30;
    const int paddingY = 20;
    const int contentX = x + paddingX;
    const int contentY = y + paddingY;
    const int contentW = dialogWidth - (paddingX * 2);
    const int contentH = dialogHeight - (paddingY * 2);

    x = contentX;
    y = contentY;

    int lineHeight = 16;
    const int titleYOffset = 8;

    if (currentMenu == MENU_MAIN) {
        rdpq_text_printf(&(rdpq_textparms_t){
            .align = ALIGN_CENTER,
            .width = contentW,
            .height = contentH,
            .wrap = WRAP_WORD,
        }, FONT_UNBALANCED, x, y + titleYOffset, "PAUSE MENU");

        y += (lineHeight * 3) + titleYOffset;

        for (int i = 0; i < MENU_MAIN_COUNT; i++) {
            if (main_row_hidden(i)) continue;     // Tower runs only
            if (i == selectedOption) {
                draw_menu_selection_highlight_centered(x, y, contentW, lineHeight, 90);
                rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
            } else {
                rdpq_set_prim_color(RGBA32(220, 220, 220, 255));
            }

            rdpq_text_printf(&(rdpq_textparms_t){
                .align = ALIGN_CENTER,
                .width = contentW,
                .height = contentH,
                .wrap = WRAP_WORD,
            }, FONT_UNBALANCED, x, y, "%s", mainMenuOptions[i]);

            y += lineHeight + (tower_active() ? 2 : 4);     // (a climb's seven rows: tighter, in its taller box)
        }

        rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
    } else if (currentMenu == MENU_SETTINGS) {
        rdpq_text_printf(&(rdpq_textparms_t){
            .align = ALIGN_CENTER,
            .width = contentW,
            .height = contentH,
            .wrap = WRAP_WORD,
        }, FONT_UNBALANCED, x, y + titleYOffset, "SETTINGS");

        y += (lineHeight * 3) + titleYOffset;

        for (int i = 0; i < MENU_SETTINGS_COUNT; i++) {
            char optionText[64];
            bool locked = i == MENU_SETTINGS_SKILLS && settingsHubReturnMenu != MENU_TITLE;
            const char *arg = locked ? (character_skills_enabled() ? "ON (title only)" : "OFF (title only)")
                                     : (character_skills_enabled() ? "ON" : "OFF");
            if (i == MENU_SETTINGS_SKIN) arg = character_skin_name(character_skin_get());
            snprintf(optionText, sizeof(optionText), settingsMenuOptions[i] ? settingsMenuOptions[i] : "", arg);

            if (i == selectedOption) {
                draw_menu_selection_highlight_centered(x, y, contentW, lineHeight, contentW / 2);
                rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
            } else {
                rdpq_set_prim_color(RGBA32(220, 220, 220, 255));
            }
            rdpq_text_printf(&(rdpq_textparms_t){
                .align = ALIGN_CENTER,
                .width = contentW,
                .height = contentH,
                .wrap = WRAP_WORD,
                .style_id = locked ? FONT_STYLE_GREY : FONT_STYLE_WHITE,   // greyed out
            }, FONT_UNBALANCED, x, y, "%s", optionText);

            y += lineHeight + 2;
        }

        rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
    } else if (currentMenu == MENU_BONUS) {
        rdpq_text_printf(&(rdpq_textparms_t){
            .align = ALIGN_CENTER,
            .width = contentW,
            .height = contentH,
            .wrap = WRAP_WORD,
        }, FONT_UNBALANCED, x, y + titleYOffset, "BONUS");
        rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = contentW, .height = contentH, .style_id = FONT_STYLE_GREY },
                         FONT_UNBALANCED, x, y + titleYOffset + lineHeight + 2, "Test your might");

        y += (lineHeight * 3) + titleYOffset;

        for (int i = 0; i < MENU_BONUS_COUNT; i++) {
            if (i == selectedOption) {
                draw_menu_selection_highlight_centered(x, y, contentW, lineHeight, contentW / 2);
                rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
            } else {
                rdpq_set_prim_color(RGBA32(220, 220, 220, 255));
            }

            const SaveData *sd = save_controller_get_slot_data(save_controller_get_active_slot());
            bool beaten = i < 3 && (save_progress_bonus_mask(sd) & (1 << i));
            draw_menu_line(x, y, contentW, contentH, ALIGN_CENTER, bonusMenuOptions[i], beaten);

            y += lineHeight + 2;
        }

        rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
    } else if (currentMenu == MENU_TOWER) {
        rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = contentW, .height = contentH },
                         FONT_UNBALANCED, x, y + titleYOffset, "TOWER");
        y += lineHeight * 2 + 4;     // (tight: its five rows and two records stay in the title-safe area)
        rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = contentW, .style_id = FONT_STYLE_GREY },
                         FONT_UNBALANCED, x, y, "Boss after boss\nEver higher");
        y += lineHeight * 2 + 4;
        rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = contentW, .style_id = FONT_STYLE_GOLD },
                         FONT_UNBALANCED, x, y, "Best floor %d   Embers %d", tower_best_floor(), tower_embers());
        y += lineHeight;
        // the floor-100 title; else what waits set aside for the next climb
        const char *badge = tower_title_badge(), *offers = tower_offers_line();
        if (badge[0]) rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = contentW, .style_id = FONT_STYLE_RED }, FONT_UNBALANCED, x, y, "%s", badge);
        else if (offers[0]) rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = contentW, .style_id = FONT_STYLE_GREY }, FONT_UNBALANCED, x, y, "%s", offers);
        y += lineHeight + 2;
        for (int i = 0; i < MENU_TOWER_COUNT; i++) {
            if (i == selectedOption) {
                draw_menu_selection_highlight_centered(x, y, contentW, lineHeight, contentW / 2);
                rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
            } else {
                rdpq_set_prim_color(RGBA32(220, 220, 220, 255));
            }
            rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = contentW, .height = contentH },
                             FONT_UNBALANCED, x, y, "%s", towerMenuOptions[i]);
            y += lineHeight + 2;
        }
        rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
    } else if (currentMenu == MENU_SHRINE) {
        tower_menu_title(x, y + titleYOffset, contentW, contentH, "SHRINE");
        // Embers on the title's row, left (the rows and the description below need the room); R's hint right
        rdpq_text_printf(&(rdpq_textparms_t){ .width = contentW, .height = contentH, .style_id = FONT_STYLE_GOLD },
                         FONT_UNBALANCED, x + 4, y + titleYOffset, "Embers %d", tower_embers());
        if (selectedOption != MENU_SHRINE_PACKS && selectedOption < MENU_SHRINE_COUNT - 1)
            rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_RIGHT, .width = contentW - 4, .height = contentH, .style_id = FONT_STYLE_GREY }, FONT_UNBALANCED, x, y + titleYOffset, "R Details");
        y += lineHeight + titleYOffset + 4;     // (its description under the nine rows stays in the title-safe area: it was at 224)
        for (int i = 0; i < MENU_SHRINE_COUNT; i++) {
            char name[40], right[24];
            right[0] = 0;
            if (i < MENU_SHRINE_UPGRADES) {
                int c = tower_upgrade_cost((UpgradeId)i);
                snprintf(name, sizeof name, "%s %d/%d", tower_upgrade_name((UpgradeId)i), tower_upgrade_level((UpgradeId)i), UPG_MAX_LEVEL);
                if (c < 0) snprintf(right, sizeof right, "max"); else snprintf(right, sizeof right, "%d", c);
            } else if (i == MENU_SHRINE_RELIC) {
                snprintf(name, sizeof name, "Relics");
                snprintf(right, sizeof right, "%s", tower_relic_name(tower_relic_equipped()));      // the one worn
            } else if (i == MENU_SHRINE_NIGHTFARER) {
                snprintf(name, sizeof name, "Nightfarer (skin)");
                if (tower_nightfarer_cost() < 0) snprintf(right, sizeof right, "owned"); else snprintf(right, sizeof right, "%d", tower_nightfarer_cost());
            } else if (i == MENU_SHRINE_PACKS) {
                int own = 0;
                for (int k = 0; k < tower_pack_count(); k++) own += tower_pack_owned(k);
                snprintf(name, sizeof name, "Unlocks");      // the Boon Packs, then the skills' B forms
                snprintf(right, sizeof right, "%d/%d", own, tower_pack_count());
            } else {
                snprintf(name, sizeof name, "Back");
            }
            if (i == selectedOption) {
                draw_menu_selection_highlight_centered(x, y, contentW, lineHeight, contentW - 8);
                rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
            } else {
                rdpq_set_prim_color(RGBA32(220, 220, 220, 255));
            }
            // A text box from y, as the highlight expects (a bare y would be the baseline: a line low).
            // Bought (the Nightfarer) or maxed: greyed out
            bool done = (i == MENU_SHRINE_NIGHTFARER && tower_nightfarer_cost() < 0) || (i < MENU_SHRINE_UPGRADES && tower_upgrade_cost((UpgradeId)i) < 0);
            int style = done ? FONT_STYLE_GREY : FONT_STYLE_WHITE;
            rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_LEFT, .width = contentW - 16, .height = contentH, .style_id = style },
                             FONT_UNBALANCED, x + 8, y, "%s", name);
            if (right[0])
                rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_RIGHT, .width = contentW - 16, .height = contentH, .style_id = style },
                                 FONT_UNBALANCED, x + 8, y, "%s", right);
            y += lineHeight + 1;
        }
        // What the selected one does
        const char *desc = selectedOption < MENU_SHRINE_UPGRADES ? tower_upgrade_desc((UpgradeId)selectedOption)
                         : selectedOption == MENU_SHRINE_RELIC ? "Choose the relic you climb with"
                         : selectedOption == MENU_SHRINE_NIGHTFARER ? "A tracker's winged helm and blue coat"
                         : selectedOption == MENU_SHRINE_PACKS ? "New boons and skill forms"
                         : "Skills are learned in the climb";
        rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = contentW, .height = contentH, .wrap = WRAP_WORD, .style_id = FONT_STYLE_GREY },
                         FONT_UNBALANCED, x, y + 6, "%s", desc);
        rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
    } else if (currentMenu == MENU_RELICS || currentMenu == MENU_SKINS || currentMenu == MENU_UNLOCKS) {
        // A list to scroll: name on the left, its state on the right, what the hovered one does below
        bool relics = currentMenu == MENU_RELICS, packs = currentMenu == MENU_UNLOCKS;
        if (relics || packs) tower_menu_title(x, y + titleYOffset, contentW, contentH, relics ? "RELICS" : "UNLOCKS");
        else rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = contentW, .height = contentH }, FONT_UNBALANCED, x, y + titleYOffset, "SKINS");
        if (relics || packs)
            rdpq_text_printf(&(rdpq_textparms_t){ .width = contentW, .height = contentH, .style_id = FONT_STYLE_GOLD },
                             FONT_UNBALANCED, x + 4, y + titleYOffset, "Embers %d", tower_embers());
        if ((relics || packs) && selectedOption < get_menu_option_count(currentMenu) - 1)
            rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_RIGHT, .width = contentW - 4, .height = contentH, .style_id = FONT_STYLE_GREY }, FONT_UNBALANCED, x, y + titleYOffset, "R Details");
        y += (packs || relics ? lineHeight : lineHeight * 2) + titleYOffset;     // (the Unlocks' ten rows: tighter, so what it holds fits under them; a relic's two effects can take two lines)
        int rows = get_menu_option_count(currentMenu);
        for (int i = 0; i < rows; i++) {
            const char *name, *right = "";
            char cost[12];
            bool grey = false;
            if (i == rows - 1) name = "Back";
            else if (packs) {
                name = tower_pack_name(i);
                if (tower_pack_owned(i)) right = "owned";
                else { snprintf(cost, sizeof cost, "%d", tower_pack_cost(i)); right = cost; grey = tower_embers() < tower_pack_cost(i); }
            } else if (relics) {
                int r = i - 1;      // row 0: None
                name = tower_relic_name(r);
                if (tower_relic_equipped() == r) right = "equipped";
                else if (r >= 0 && !tower_relic_owned(r) && tower_relic_cost(r) == -2) {      // a milestone's, not sold
                    snprintf(cost, sizeof cost, "floor %d", tower_relic_floor(r)); right = cost; grey = true;
                } else if (r >= 0 && !tower_relic_owned(r)) {
                    snprintf(cost, sizeof cost, "%d", tower_relic_cost(r)); right = cost;
                    grey = tower_embers() < tower_relic_cost(r);       // can't afford it yet
                }
            } else {
                name = character_skin_name(i);
                if (!save_controller_skin_unlocked(i)) { right = "locked"; grey = true; }
                else if (character_skin_get() == i) right = "worn";
            }
            if (i == selectedOption) {
                draw_menu_selection_highlight_centered(x, y, contentW, lineHeight, contentW - 8);
                rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
            } else {
                rdpq_set_prim_color(RGBA32(220, 220, 220, 255));
            }
            int style = grey ? FONT_STYLE_GREY : FONT_STYLE_WHITE;
            rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_LEFT, .width = contentW - 16, .height = contentH, .style_id = style },
                             FONT_UNBALANCED, x + 8, y, "%s", name);
            if (right[0])
                rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_RIGHT, .width = contentW - 16, .height = contentH, .style_id = style },
                                 FONT_UNBALANCED, x + 8, y, "%s", right);
            y += lineHeight + (packs ? -1 : 1);
        }
        const char *desc = selectedOption == rows - 1 ? ""
                         : packs ? tower_pack_brief(selectedOption)
                         : relics ? (selectedOption == 0 ? "Climb without a relic" : tower_relic_desc(selectedOption - 1))
                         : save_controller_skin_unlocked(selectedOption) ? "" : skin_lock_hint(selectedOption);
        rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = contentW, .height = contentH, .wrap = WRAP_WORD, .style_id = FONT_STYLE_GREY },
                         FONT_UNBALANCED, x, y + 6, "%s", desc);
        rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
    } else if (currentMenu == MENU_OFFERINGS) {
        // the Offerings: name, its price now (or set, or why not), then Back; what's set aside under the title; the hovered one's line below
        tower_menu_title(x, y + titleYOffset, contentW, contentH, "OFFERINGS");
        rdpq_text_printf(&(rdpq_textparms_t){ .width = contentW, .height = contentH, .style_id = FONT_STYLE_GOLD },
                         FONT_UNBALANCED, x + 4, y + titleYOffset, "Embers %d", tower_embers());
        int rows = get_menu_option_count(currentMenu), held = tower_offers_held();
        if (selectedOption < rows - 1)
            rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_RIGHT, .width = contentW - 4, .height = contentH, .style_id = FONT_STYLE_GREY }, FONT_UNBALANCED, x, y + titleYOffset, "R Details");
        y += lineHeight + titleYOffset;
        const char *line = tower_offers_line();
        char head[64];
        if (line[0]) snprintf(head, sizeof head, "%s", line);       // (the widest pair is 192 px of 240: no room for a label)
        else snprintf(head, sizeof head, "For the next climb: two at most");
        rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = contentW, .height = contentH, .style_id = line[0] ? FONT_STYLE_GOLD : FONT_STYLE_GREY },
                         FONT_UNBALANCED, x, y, "%s", head);
        y += lineHeight + 2;
        for (int i = 0; i < rows; i++) {
            char right[16] = "";
            bool grey = false, set = false;
            const char *name = i == rows - 1 ? "Back" : tower_offer_name(i);
            if (i < rows - 1) {
                set = tower_offer_set(i);
                const char *why = tower_offer_lock(i);
                int c = tower_offer_price(i);
                if (set) snprintf(right, sizeof right, "set");
                else if (why[0]) { snprintf(right, sizeof right, "locked"); grey = true; }
                else { snprintf(right, sizeof right, "%d", c); grey = held >= 2 || tower_embers() < c; }
            }
            if (i == selectedOption) {
                draw_menu_selection_highlight_centered(x, y, contentW, lineHeight, contentW - 8);
                rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
            } else rdpq_set_prim_color(RGBA32(220, 220, 220, 255));
            int style = set ? FONT_STYLE_GOLD : grey ? FONT_STYLE_GREY : FONT_STYLE_WHITE;
            rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_LEFT, .width = contentW - 16, .height = contentH, .style_id = style }, FONT_UNBALANCED, x + 8, y, "%s", name);
            if (right[0]) rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_RIGHT, .width = contentW - 16, .height = contentH, .style_id = style }, FONT_UNBALANCED, x + 8, y, "%s", right);
            y += lineHeight - 1;
        }
        // (one line: two would leave the title-safe area)
        // (why A can't set it aside in red, as the Shop's refusals; else what it does)
        const char *desc = "", *why = selectedOption < rows - 1 ? tower_offer_why(selectedOption) : "";
        if (selectedOption < rows - 1) desc = why[0] ? why : tower_offer_brief(selectedOption);
        rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = contentW, .height = contentH, .wrap = WRAP_WORD, .style_id = why[0] ? FONT_STYLE_RED : FONT_STYLE_GREY },
                         FONT_UNBALANCED, x, y + 6, "%s", desc);
        rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
    } else if (currentMenu == MENU_CODEX) {
        // the topics, paged (ui_pages) on a dark box inside the title-safe area, as Stats
        rdpq_sync_pipe(); rdpq_set_mode_standard(); rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY); rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
        rdpq_set_prim_color(RGBA32(24, 18, 14, 235));
        rdpq_fill_rectangle(RUN_TX - 8, RUN_CY - 16, RUN_TX + RUN_TW + 8, RUN_BODY_END + 20);
        ui_pager_draw(&codexPager, RUN_TX, RUN_CY, RUN_TW, "CODEX");
        int pitch = ui_line_pitch(), first = codexPager.page * codex_per_page();
        for (int k = 0; k < codex_rows_on_page(); k++) {
            int i = first + k, ry = RUN_BODY + k * pitch;
            bool open = tower_codex_open_ok(i);
            if (k == codexRow) ui_select_box(RUN_TX - 4, ry - 12, RUN_TX + RUN_TW + 4, ry + 4);
            rdpq_text_printf(&(rdpq_textparms_t){ .width = RUN_TW - UI_RIGHT_W, .style_id = open ? FONT_STYLE_WHITE : FONT_STYLE_GREY }, FONT_UNBALANCED, RUN_TX, ry, "%s", open ? tower_codex_title(i) : "???");
            if (!open) {
                char r[16]; snprintf(r, sizeof r, "floor %d", tower_codex_floor(i));
                rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_RIGHT, .width = UI_RIGHT_W, .style_id = FONT_STYLE_GREY }, FONT_UNBALANCED, RUN_TX + RUN_TW - UI_RIGHT_W, ry, "%s", r);
            }
        }
        ui_hint(RUN_TX + RUN_TW, RUN_BODY_END + 14, codexRow >= 0 ? "A or R: read" : "Down: a topic");
    } else if (currentMenu == MENU_AUDIO) {
        rdpq_text_printf(&(rdpq_textparms_t){
            .align = ALIGN_CENTER,
            .width = contentW,
            .height = contentH,
            .wrap = WRAP_WORD,
        }, FONT_UNBALANCED, x, y + titleYOffset, "AUDIO");

        y += (lineHeight * 3) + titleYOffset;

        for (int i = 0; i < MENU_AUDIO_COUNT; i++) {
            char optionText[64];
            const char *fmt = audioMenuOptions[i] ? audioMenuOptions[i] : "";

            if (i == MENU_AUDIO_MASTER_VOLUME) {
                snprintf(optionText, sizeof(optionText), fmt, audio_get_master_volume());
            } else if (i == MENU_AUDIO_MUSIC_VOLUME) {
                snprintf(optionText, sizeof(optionText), fmt, audio_get_music_volume());
            } else if (i == MENU_AUDIO_SFX_VOLUME) {
                snprintf(optionText, sizeof(optionText), fmt, audio_get_sfx_volume());
            } else if (i == MENU_AUDIO_MUTE_TOGGLE) {
                snprintf(optionText, sizeof(optionText), fmt, audio_is_muted() ? "ON" : "OFF");
            } else if (i == MENU_AUDIO_STEREO_MODE) {
                snprintf(optionText, sizeof(optionText), fmt, audio_get_stereo_mode() ? "Stereo" : "Mono");
            } else if (i == MENU_AUDIO_RUMBLE_TOGGLE) {
                bool rumbleEnabled = joypad_is_rumble_enabled();
                bool rumbleAvailable = joypad_is_connected(JOYPAD_PORT_1) && joypad_get_rumble_supported(JOYPAD_PORT_1);

                if (!rumbleAvailable) {
                    snprintf(optionText, sizeof(optionText), rumbleEnabled ? "Rumble: ON (No Pak)" : "Rumble: OFF (No Pak)");
                } else {
                    snprintf(optionText, sizeof(optionText), fmt, rumbleEnabled ? "ON" : "OFF");
                }
            } else {
                snprintf(optionText, sizeof(optionText), "%s", fmt);
            }

            if (i == selectedOption) {
                draw_menu_selection_highlight_centered(x, y, contentW, lineHeight, contentW / 2);
                rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
            } else {
                rdpq_set_prim_color(RGBA32(220, 220, 220, 255));
            }

            rdpq_text_printf(&(rdpq_textparms_t){
                .align = ALIGN_CENTER,
                .width = contentW,
                .height = contentH,
                .wrap = WRAP_WORD,
            }, FONT_UNBALANCED, x, y, "%s", optionText);

            y += lineHeight + 2;
        }

        rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
    } else if (currentMenu == MENU_VIDEO) {
        rdpq_text_printf(&(rdpq_textparms_t){
            .align = ALIGN_CENTER,
            .width = contentW,
            .height = contentH,
            .wrap = WRAP_WORD,
        }, FONT_UNBALANCED, x, y + titleYOffset, "VIDEO");

        y += (lineHeight * 3) + titleYOffset;

        for (int i = 0; i < MENU_VIDEO_COUNT; i++) {
            char optionText[64];
            const char *fmt = videoMenuOptions[i] ? videoMenuOptions[i] : "";

            if (i == MENU_VIDEO_ASPECT) {
                snprintf(optionText, sizeof(optionText), fmt, hdAspect ? "16:9" : "4:3");
            } else if (i == MENU_VIDEO_UI_OVERSCAN_CALIBRATE) {
                snprintf(optionText, sizeof(optionText), "%s", "Calibrate Overscan");
            } else {
                snprintf(optionText, sizeof(optionText), "%s", fmt);
            }

            if (i == selectedOption) {
                draw_menu_selection_highlight_centered(x, y, contentW, lineHeight, contentW / 2);
                rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
            } else {
                rdpq_set_prim_color(RGBA32(220, 220, 220, 255));
            }

            rdpq_text_printf(&(rdpq_textparms_t){
                .align = ALIGN_CENTER,
                .width = contentW,
                .height = contentH,
                .wrap = WRAP_WORD,
            }, FONT_UNBALANCED, x, y, "%s", optionText);

            y += lineHeight + 2;
        }

        rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
    } else if (currentMenu == MENU_CONTROLS) {
        controls_icons(true);
        rdpq_text_printf(&(rdpq_textparms_t){
            .align = ALIGN_CENTER,
            .width = contentW,
            .height = contentH,
            .wrap = WRAP_WORD,
        }, FONT_UNBALANCED, x, y + titleYOffset, "CONTROLS");

        y += lineHeight + 4 + titleYOffset;     // (tighter than the other menus: the Back row under the button list fits)

        // The two settings (Left/Right or A to change), then what each button does in that layout
        for (int i = 0; i < 2; i++) {
            char line[40];
            if (i == 0) snprintf(line, sizeof line, "Layout: %s", joypad_layout_name(joypad_get_layout()));
            else snprintf(line, sizeof line, "Camera speed: %d", camera_get_speed());
            if (i == selectedOption) draw_menu_selection_highlight_centered(x, y, contentW, lineHeight, contentW - 20);
            rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = contentW, .height = contentH },
                             FONT_UNBALANCED, x, y, "%s", line);
            y += lineHeight + 2;
        }
        y += lineHeight + 4;

        const int listW = 150;
        const int listX = x + (contentW - listW) / 2 + 12;
        int lay = joypad_get_layout();
        const int step = lineHeight;

        draw_icon_line(iconStick, listX, y, lineHeight, listW, "Move");
        y += step;
        draw_icon_line(iconA, listX, y, lineHeight, listW, lay == LAYOUT_SWAPPED ? "Attack" : "Dodge / Interact");
        y += step;
        draw_icon_line(iconB, listX, y, lineHeight, listW, lay == LAYOUT_SWAPPED ? "Dodge / Interact" : "Attack");
        y += step;
        draw_icon_line(iconZ, listX, y, lineHeight, listW, "Target");
        y += step;
        draw_icon_line(iconStart, listX, y, lineHeight, listW, "Pause Menu");
        y += step;
        draw_icon_line(iconCLeft, listX, y, lineHeight, listW, lay == LAYOUT_SKILLS_ON_C ? "Skills / Potion" : "Move Camera");
        y += step;
        draw_icon_line(iconDpad, listX, y, lineHeight, listW, lay == LAYOUT_SKILLS_ON_C ? "Move Camera" : "Skills / Potion");
        y += 6;     // (the list's y is its baseline; the Back row's, its top)
        if (selectedOption == 2) draw_menu_selection_highlight_centered(x, y, contentW, lineHeight, contentW - 20);
        rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = contentW, .height = contentH }, FONT_UNBALANCED, x, y, "Back");
    } else if (currentMenu == MENU_RUN) {
        // Stats then Build, paged (ui_pages), inside the title-safe area, on a dark box (the dialog's art lets the hall's light through)
        run_pages_build();
        rdpq_sync_pipe(); rdpq_set_mode_standard(); rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY); rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
        rdpq_set_prim_color(RGBA32(24, 18, 14, 235));
        rdpq_fill_rectangle(RUN_TX - 8, RUN_CY - 16, RUN_TX + RUN_TW + 8, RUN_BODY_END + 20);
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
                rdpq_text_printf(&(rdpq_textparms_t){ .width = RUN_TW - UI_RIGHT_W, .style_id = s_run->style[i] }, FONT_UNBALANCED, RUN_TX, ry, "%s", s_run->left[i]);
                if (s_run->right[i][0])
                    rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_RIGHT, .width = UI_RIGHT_W, .style_id = s_run->style[i] }, FONT_UNBALANCED, RUN_TX + RUN_TW - UI_RIGHT_W, ry, "%s", s_run->right[i]);
            }
            ui_hint(RUN_TX + RUN_TW, RUN_BODY_END + 14, runRow >= 0 ? "A or R: Details" : "Down: a row");
        }
    } else if (currentMenu == MENU_CREDITS) {
        rdpq_text_printf(&(rdpq_textparms_t){
            .align = ALIGN_CENTER,
            .width = contentW,
            .height = contentH,
            .wrap = WRAP_WORD,
        }, FONT_UNBALANCED, x, y + titleYOffset, "CREDITS");

        y += (lineHeight * 3) + titleYOffset;

        rdpq_set_prim_color(RGBA32(255, 255, 255, 255));

        rdpq_text_printf(&(rdpq_textparms_t){
            .align = ALIGN_CENTER,
            .width = contentW,
            .height = contentH,
            .wrap = WRAP_WORD,
        }, FONT_UNBALANCED, x, y, "FROMSCRATCH");

        y += lineHeight + 8;

        rdpq_text_printf(&(rdpq_textparms_t){
            .align = ALIGN_CENTER,
            .width = contentW,
            .height = contentH,
            .wrap = WRAP_WORD,
        }, FONT_UNBALANCED, x, y, "BoxingBruin");

        y += lineHeight + 6;

        rdpq_text_printf(&(rdpq_textparms_t){
            .align = ALIGN_CENTER,
            .width = contentW,
            .height = contentH,
            .wrap = WRAP_WORD,
        }, FONT_UNBALANCED, x, y, "HelloNewman");

        y += lineHeight + 6;

        rdpq_text_printf(&(rdpq_textparms_t){
            .align = ALIGN_CENTER,
            .width = contentW,
            .height = contentH,
            .wrap = WRAP_WORD,
        }, FONT_UNBALANCED, x, y, "Aethrone");
    }
    if (ui_details()->open && ui_details()->owner == UI_OWNER_MENU) ui_panel_draw(ui_details());     // Details (R), over the menu
}

void menu_controller_free(void)
{
    gpu_wait();

    if (pauseMenuBg) {
        sprite_free_shared(pauseMenuBg);
        pauseMenuBg = NULL;
        surface_free(&pauseMenuBgSurf);
    }

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
    if (currentMenu == MENU_RUN) { *row = runRow; *back = -1; *corner = runPager.sel; return currentMenu; }
    if (currentMenu == MENU_CODEX) { *row = codexRow; *back = -1; *corner = codexPager.sel; return currentMenu; }
    *row = selectedOption;
    *back = currentMenu == MENU_MAIN ? MENU_MAIN_RESUME : currentMenu == MENU_TITLE ? -3 : get_menu_option_count(currentMenu) - 1;
    return currentMenu;
}
int menu_controller_test_run(int *page, int *pages, int *row) { *page = runPager.page; *pages = runPager.pages; *row = runRow; return runPager.sel; }
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
        lastAPressed = btn.a; lastBPressed = btn.b; lastRPressed = btn.r;
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