#ifndef MENU_CONTROLLER_H
#define MENU_CONTROLLER_H

typedef enum {
    // Pause menu root (in-game)
    MENU_MAIN,

    // Title menu root (title screen)
    MENU_TITLE,

    // Shared submenus / screens
    MENU_SETTINGS,
    MENU_AUDIO,
    MENU_VIDEO,
    MENU_CONTROLS,
    MENU_CREDITS,
    MENU_LOAD_GAME,
    MENU_BONUS,
    MENU_TOWER,
    MENU_SHRINE,
    MENU_RUN,           // the Tower run's boons, pacts and totals (from the pause menu)
    MENU_RELICS,        // Shrine > Relics: every relic, bought and equipped here
    MENU_SKINS,         // Settings > Skin: every skin, worn from here
    MENU_UNLOCKS,       // Shrine > Unlocks: the families' new boons (Boon Packs) and the skills' B forms, bought here
    MENU_OFFERINGS,     // Tower > Offerings: gifts for the next climb, set aside here
    MENU_CODEX,         // Tower > Codex and the pause menu's (in a climb): a page a topic, paged (ui_pages)
    MENU_ARMORY,        // Shrine > Armory: the swords (weapon.c), bought and equipped here
} MenuState;

typedef enum {
    MENU_MAIN_RESUME,
    MENU_MAIN_RUN,      // Tower runs only
    MENU_MAIN_RESTART,
    MENU_MAIN_SETTINGS,
    MENU_MAIN_CODEX,    // Tower runs only (Controls: in Settings)
    MENU_MAIN_QUIT,
    MENU_MAIN_COUNT
} MainMenuOption;

typedef enum {
    MENU_TITLE_STORY,
    MENU_TITLE_BONUS,
    MENU_TITLE_TOWER,
    MENU_TITLE_LOAD_GAME,
    MENU_TITLE_SETTINGS,
    MENU_TITLE_CREDITS,
    MENU_TITLE_COUNT
} TitleMenuOption;

typedef enum {
    MENU_SETTINGS_AUDIO,
    MENU_SETTINGS_VIDEO,
    MENU_SETTINGS_CONTROLS,
    MENU_SETTINGS_SKILLS,
    MENU_SETTINGS_SKIN,
    MENU_SETTINGS_SWORD,    // A: the Armory (title only, as the skins)
    MENU_SETTINGS_BACK,
    MENU_SETTINGS_COUNT
} SettingsMenuOption;

typedef enum {
    MENU_AUDIO_MASTER_VOLUME,
    MENU_AUDIO_MUSIC_VOLUME,
    MENU_AUDIO_SFX_VOLUME,
    MENU_AUDIO_MUTE_TOGGLE,
    MENU_AUDIO_STEREO_MODE,
    MENU_AUDIO_RUMBLE_TOGGLE,
    MENU_AUDIO_BACK,
    MENU_AUDIO_COUNT
} AudioMenuOption;

typedef enum {
    MENU_VIDEO_ASPECT,
    MENU_VIDEO_UI_OVERSCAN_CALIBRATE,
    MENU_VIDEO_BACK,
    MENU_VIDEO_COUNT
} VideoMenuOption;

typedef enum {
    MENU_LOAD_GAME_SLOT_1,
    MENU_LOAD_GAME_SLOT_2,
    MENU_LOAD_GAME_SLOT_3,
    MENU_LOAD_GAME_BACK,
    MENU_LOAD_GAME_COUNT
} LoadGameMenuOption;

typedef enum {
    MENU_BONUS_FALLEN_KNIGHT,
    MENU_BONUS_SHACKLED_SUN,
    MENU_BONUS_SHADOWBORN,
    MENU_BONUS_BACK,
    MENU_BONUS_COUNT
} BonusMenuOption;

typedef enum {
    MENU_TOWER_CLIMB,
    MENU_TOWER_SHRINE,
    MENU_TOWER_OFFERINGS,
    MENU_TOWER_CODEX,
    MENU_TOWER_HEAT,        // the Heat dial: Left/Right (A steps it up, past the top back to 0)
    MENU_TOWER_BACK,
    MENU_TOWER_COUNT
} TowerMenuOption;

// The Shrine: the five upgrades, Back
#define MENU_SHRINE_UPGRADES 5
#define MENU_SHRINE_RELIC    (MENU_SHRINE_UPGRADES)       // A: the Relics list
#define MENU_SHRINE_NIGHTFARER   (MENU_SHRINE_UPGRADES + 1)   // A: buy the Nightfarer skin
#define MENU_SHRINE_PACKS    (MENU_SHRINE_UPGRADES + 2)   // A: the Boon Packs list (the unlock ladder)
#define MENU_SHRINE_ARMORY   (MENU_SHRINE_UPGRADES + 3)   // A: the swords (weapon.c)
#define MENU_SHRINE_COUNT    (MENU_SHRINE_UPGRADES + 5)

typedef enum {
    MENU_ACTION_NONE = 0,

    // Emitted when the title menu selects Continue/Play or a save slot.
    // The title scene consumes this and starts its transition.
    MENU_ACTION_TITLE_START_GAME,

    // Reserved for later cleanup. For now restart can still call scene_restart()
    // directly from the pause menu if you kept that first-pass behavior.
    MENU_ACTION_RESTART_GAME,

    // Pause menu > Main Menu: the Guardian scene consumes this at the top of its
    // update and returns to the title (not from inside the menu, mid-scene-update).
    MENU_ACTION_QUIT_TO_TITLE,
} MenuAction;

void menu_controller_init(void);

/*
 * Guardian / in-game update path.
 * Reads scene_get_game_state() internally for pause/menu behavior.
 */
void menu_controller_update(void);

/*
 * Title-screen update path.
 * Forces title-menu behavior without asking the guardian scene for GameState.
 */
void menu_controller_update_title(void);

/*
 * Returns and clears the latest one-shot menu action.
 */
MenuAction menu_controller_consume_action(void);

void menu_controller_draw(void);
void menu_controller_free(void);

bool menu_controller_is_active(void);
bool menu_controller_is_title_submenu_active(void);
bool menu_controller_is_pause_menu_active(void);

void menu_controller_toggle(void);
void menu_controller_close(void);
void menu_controller_open_pause(int option);     // the pause menu, its cursor on this row (the Tower map's B: Stats)

#endif