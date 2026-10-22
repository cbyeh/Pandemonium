#ifndef SAVE_CONTROLLER_H
#define SAVE_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Save data stored per slot in EEPROM.
 * Notes:
 * - Slot selection UI doesn't exist yet; default slot is 0 ("Save 1").
 * - This struct is serialized directly to EEPROM; keep it stable and versioned.
 */
typedef struct {
    // Header / validation
    uint32_t magic;        // 'PAND'
    uint16_t version;      // Save format version
    uint8_t slot_index;    // 0..SAVE_SLOT_COUNT-1
    uint8_t reserved0;

    // Tracked stats/settings (per slot)
    uint32_t run_count;
    uint32_t best_boss_time_ms; // 0 => no recorded clear yet
    uint32_t last_played_timestamp; // Timestamp of last play (0 = never played)

    // Audio settings (0-10)
    int8_t masterVolume;
    int8_t musicVolume;
    int8_t sfxVolume;
    uint8_t globalMute;
    uint8_t stereoMode; // 1 = stereo, 0 = mono

    // Validation
    uint32_t checksum; // FNV-1a over all bytes before this field

    // Pad to a fixed size on EEPROM (see SAVE_SLOT_SIZE_BYTES in .c)
    uint8_t _pad[28];
} SaveData;

void save_controller_init(void);
void save_controller_update(void);

// Save availability / status
bool save_controller_is_enabled(void); // true if EEPROM + EEPROMFS initialized

// Save slot selection (default is 0 / "Save 1")
bool save_controller_set_active_slot(int slot);
int  save_controller_get_active_slot(void);

// Audio settings
bool save_controller_load_settings(void);
bool save_controller_save_settings(void);

// Tower / Shrine progress, kept in the slot's spare bytes (old saves read as zeros)
typedef struct {
    uint16_t embers;
    uint8_t  levels[5];      // Shrine upgrade levels 0..7
    uint8_t  bestFloor;
    uint8_t  weaponsOwned;   // bit per weapon
    uint8_t  weapon;         // equipped
    uint8_t  flags;          // TOWER_FLAG_*; bits 1-2: the equipped relic (1-3, 0 none)
    uint8_t  relics[3];      // two effects a relic (high, low nibble); 0: an empty slot
} TowerSave;
#define TOWER_FLAG_WYLDER 0x01   // the Wylder skin, bought at the Shrine
void save_controller_get_tower(TowerSave *out);
bool save_controller_set_tower(const TowerSave *in);

// Progress shown on the Load Game cards (any slot's data; NULL reads as nothing)
int  save_progress_tower_best_floor(const SaveData *d);
int  save_progress_tower_embers(const SaveData *d);
int  save_progress_bonus_mask(const SaveData *d);      // bit per Bonus boss beaten: Fallen Warrior, Shackled Sun, Shadowborne
// The skins earned (of SAVE_SKINS_TO_EARN: the Crimson and Teal cloaks, Wylder; the rest are open from the start)
#define SAVE_SKINS_TO_EARN 3
int  save_progress_skins_unlocked(const SaveData *d);
bool save_progress_skin_unlocked(const SaveData *d, int skin);
// The active slot
void save_controller_mark_bonus_beaten(int bonusIndex);   // 0..2
bool save_controller_skin_unlocked(int skin);

// Run + boss stats
bool save_controller_increment_run_count(void);
bool save_controller_record_boss_clear_time_ms(uint32_t clear_time_ms);
uint32_t save_controller_get_run_count(void);
uint32_t save_controller_get_best_boss_time_ms(void);

// Get save data from any slot (for load game menu, etc)
const SaveData* save_controller_get_slot_data(int slot);
uint32_t save_controller_get_slot_run_count(int slot);
uint32_t save_controller_get_slot_best_boss_time_ms(int slot);
uint32_t save_controller_get_slot_last_played_timestamp(int slot);

// Find last played save slot (-1 if no saves have been played)
int save_controller_get_last_played_slot(void);

// Update timestamp when a save is played
bool save_controller_update_last_played_timestamp(void);

// Clear a save slot (reset to empty/defaults)
bool save_controller_clear_slot(int slot);
void save_controller_free(void);

#endif