#include <libdragon.h>
#include <eepromfs.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "save_controller.h"
#include "camera_controller.h"
#include "audio_controller.h"
#include "joypad_utility.h"
#include "globals.h"
#include "video_controller.h"
#include "../character/character.h"
#include "../character/character_skin.h"

/*
 * EEPROMFS notes:
 * - Files always exist at the size specified during eepfs_init
 * - eepfs_verify_signature validates only filesystem layout, not file contents
 * - It's still on us to validate magic/version/checksum
 */

// Keep stable across builds.
#define SAVE_FILE_NAME "/pandemonium_save.dat"

#define SAVE_MAGIC   0x50414E44u  // "PAND", short for our game name "Pandemonium"
#define SAVE_VERSION 1u

// Default to 3 slots (Save 1..3). Increase later once a UI exists.
#define SAVE_SLOT_COUNT 3

// Debounce auto-saving (audio sliders) to avoid hitches + EEPROM wear.
#define SAVE_DEBOUNCE_S 0.50

typedef struct SaveBlob {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved0;
    SaveData slots[SAVE_SLOT_COUNT];
    uint32_t crc32; // CRC32 over slots[] only
} SaveBlob;

_Static_assert((sizeof(SaveBlob) % 8) == 0, "SaveBlob size should be a multiple of 8 bytes for EEPROM blocks");

static bool s_initialized = false;
static bool s_eeprom_available = false;
static int  s_active_slot = 0;

static SaveBlob s_blob;
static bool s_dirty = false;
static double s_last_dirty_time_s = 0.0;
static uint32_t s_play_session_counter = 0;

static uint32_t calculate_crc32(const void *data, size_t size) {
    static const uint32_t CRC32_POLY = 0xEDB88320u;
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t *bytes = (const uint8_t*)data;

    for (size_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (CRC32_POLY & -(int)(crc & 1u));
        }
    }

    return ~crc;
}

static uint32_t fnv1a32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t*)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint32_t)p[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t compute_slot_checksum(const SaveData *d) {
    const size_t len = offsetof(SaveData, checksum);
    return fnv1a32(d, len);
}

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int8_t saved_overscan_x(const SaveData *d)
{
    if (!d) return UI_OVERSCAN_DEFAULT_X;
    // Stored in padding to keep save size stable across versions.
    int8_t v = (int8_t)d->_pad[0];
    int maxX = (SCREEN_WIDTH / 2) - 2;
    return (int8_t)clampi((int)v, UI_OVERSCAN_MIN, maxX);
}

static int8_t saved_overscan_y(const SaveData *d)
{
    if (!d) return UI_OVERSCAN_DEFAULT_Y;
    int8_t v = (int8_t)d->_pad[1];
    int maxY = (SCREEN_HEIGHT / 2) - 2;
    return (int8_t)clampi((int)v, UI_OVERSCAN_MIN, maxY);
}

static void save_set_overscan(SaveData *d, int8_t x, int8_t y)
{
    if (!d) return;
    int maxX = (SCREEN_WIDTH / 2) - 2;
    int maxY = (SCREEN_HEIGHT / 2) - 2;
    x = (int8_t)clampi((int)x, UI_OVERSCAN_MIN, maxX);
    y = (int8_t)clampi((int)y, UI_OVERSCAN_MIN, maxY);

    d->_pad[0] = (uint8_t)x;
    d->_pad[1] = (uint8_t)y;
}

static bool saved_rumble_enabled(const SaveData *d)
{
    if (!d) return true;
    // Stored in padding to keep save size stable across versions.
    return d->_pad[2] != 0;
}

static void save_set_rumble_enabled(SaveData *d, bool enabled)
{
    if (!d) return;
    d->_pad[2] = enabled ? 1 : 0;
}

static bool saved_hd_aspect(const SaveData *d)
{
    if (!d) return false;
    // Stored in padding to keep save size stable across versions.
    return d->_pad[3] != 0;
}

static void save_set_hd_aspect(SaveData *d, bool enabled)
{
    if (!d) return;
    d->_pad[3] = enabled ? 1 : 0;
}

// Stored inverted so saves from before the skills existed read as enabled.
static bool saved_skills_enabled(const SaveData *d)
{
    return !d || d->_pad[4] == 0;
}

static void save_set_skills_enabled(SaveData *d, bool enabled)
{
    if (!d) return;
    d->_pad[4] = enabled ? 0 : 1;
}

// Old saves hold 0: the stock Knight.
static int saved_skin(const SaveData *d)
{
    return (d && d->_pad[5] < SKIN_COUNT) ? d->_pad[5] : SKIN_KNIGHT;
}

static void save_defaults_for_slot(SaveData *d, int slot) {
    memset(d, 0, sizeof(*d));

    d->magic = SAVE_MAGIC;
    d->version = (uint16_t)SAVE_VERSION;
    d->slot_index = (uint8_t)slot;

    // Defaults: take current audio state (whatever audio_controller initialized to)
    d->masterVolume = (int8_t)audio_get_master_volume();
    d->musicVolume  = (int8_t)audio_get_music_volume();
    d->sfxVolume    = (int8_t)audio_get_sfx_volume();
    d->globalMute   = (uint8_t)(audio_is_muted() ? 1 : 0);
    d->stereoMode   = (uint8_t)(audio_get_stereo_mode() ? 1 : 0);

    // UI overscan defaults: minimum extra padding (widest UI in calibrated range)
    save_set_overscan(d, UI_OVERSCAN_DEFAULT_X, UI_OVERSCAN_DEFAULT_Y);
    save_set_rumble_enabled(d, true);
    save_set_hd_aspect(d, false);

    d->checksum = compute_slot_checksum(d);
}

static bool validate_slot(const SaveData *d, int expected_slot) {
    if (!d) return false;
    if (d->magic != SAVE_MAGIC) return false;
    if (d->version != (uint16_t)SAVE_VERSION) return false;
    if ((int)d->slot_index != expected_slot) return false;

    if (d->masterVolume < 0 || d->masterVolume > 10) return false;
    if (d->musicVolume  < 0 || d->musicVolume  > 10) return false;
    if (d->sfxVolume    < 0 || d->sfxVolume    > 10) return false;

    return d->checksum == compute_slot_checksum(d);
}

static void blob_seed_defaults(void) {
    memset(&s_blob, 0, sizeof(s_blob));
    s_blob.magic = SAVE_MAGIC;
    s_blob.version = (uint16_t)SAVE_VERSION;

    for (int i = 0; i < SAVE_SLOT_COUNT; i++) {
        save_defaults_for_slot(&s_blob.slots[i], i);
    }

    s_blob.crc32 = calculate_crc32(s_blob.slots, sizeof(s_blob.slots));
}

static bool blob_is_valid(const SaveBlob *b) {
    if (!b) return false;
    if (b->magic != SAVE_MAGIC) return false;
    if (b->version != (uint16_t)SAVE_VERSION) return false;
    if (b->crc32 != calculate_crc32(b->slots, sizeof(b->slots))) return false;

    for (int i = 0; i < SAVE_SLOT_COUNT; i++) {
        if (!validate_slot(&b->slots[i], i)) return false;
    }
    return true;
}

static bool blob_write_now(void) {
    if (!s_initialized) return false;

    // Refresh per-slot checksums (cheap)
    for (int i = 0; i < SAVE_SLOT_COUNT; i++) {
        s_blob.slots[i].magic = SAVE_MAGIC;
        s_blob.slots[i].version = (uint16_t)SAVE_VERSION;
        s_blob.slots[i].slot_index = (uint8_t)i;
        s_blob.slots[i].checksum = compute_slot_checksum(&s_blob.slots[i]);
    }

    s_blob.magic = SAVE_MAGIC;
    s_blob.version = (uint16_t)SAVE_VERSION;
    s_blob.crc32 = calculate_crc32(s_blob.slots, sizeof(s_blob.slots));

    int r = eepfs_write(SAVE_FILE_NAME, &s_blob, sizeof(s_blob));
    if (r == EEPFS_ESUCCESS) {
        s_dirty = false;
        return true;
    }
    debugf("EEPROMFS write failed (%d)\n", r);
    return false;
}

static bool blob_read_or_seed_defaults(void) {
    if (!s_initialized) return false;

    // Layout mismatch / brand new EEPROM / another game: wipe + seed defaults
#if defined(HLE_TEST_SKIN_SAVE) || defined(HLE_TEST_SAVE_FLOW)
    { uint8_t b0[8]; eeprom_read(0, b0); debugf("[save] eeprom type %d, block 0: %02x %02x %02x %02x %02x %02x %02x %02x\n", eeprom_present(),
                                                b0[0], b0[1], b0[2], b0[3], b0[4], b0[5], b0[6], b0[7]); }
#endif
    if (!eepfs_verify_signature()) {
        debugf("EEPROMFS signature mismatch: wiping + seeding defaults\n");
        eepfs_wipe();
        blob_seed_defaults();
        return blob_write_now();
    }

    SaveBlob b;
    memset(&b, 0, sizeof(b));
    int r = eepfs_read(SAVE_FILE_NAME, &b, sizeof(b));
    if (r != EEPFS_ESUCCESS) {
        debugf("EEPROMFS read failed (%d): seeding defaults\n", r);
        blob_seed_defaults();
        return blob_write_now();
    }

    if (!blob_is_valid(&b)) {
        debugf("Save blob invalid: wiping + seeding defaults\n");
        eepfs_wipe();
        blob_seed_defaults();
        return blob_write_now();
    }

    s_blob = b;
    return true;
}

void save_controller_init(void) {
    eeprom_type_t t = eeprom_present();
    s_eeprom_available = (t != EEPROM_NONE);
    s_initialized = false;

    if (!s_eeprom_available) {
        debugf("EEPROM not present (saves disabled)\n");
        return;
    }

    static const eepfs_entry_t entries[] = {
        { SAVE_FILE_NAME, sizeof(SaveBlob) },
    };

    int r = eepfs_init(entries, 1);
    if (r != EEPFS_ESUCCESS) {
        debugf("EEPROMFS init failed (%d) (saves disabled)\n", r);
        return;
    }

    s_initialized = true;
    debugf("EEPROMFS initialized. Using %d save slots.\n", SAVE_SLOT_COUNT);

    // Load blob or seed defaults, then select the last played slot (its skin and settings
    // are the ones that were chosen last; Save 1 if none has been played)
    (void)blob_read_or_seed_defaults();
    // carry the play order on from the last boot, or a newly played slot would sort below older ones
    for (int i = 0; i < SAVE_SLOT_COUNT; i++)
        if (s_blob.slots[i].last_played_timestamp > s_play_session_counter) s_play_session_counter = s_blob.slots[i].last_played_timestamp;
    int last = save_controller_get_last_played_slot();
    (void)save_controller_set_active_slot(last >= 0 ? last : 0);
}

void save_controller_update(void) {
    if (!s_initialized) return;
    if (!s_dirty) return;

    // Provided by your game_time module
    extern double nowS;
    if ((nowS - s_last_dirty_time_s) < SAVE_DEBOUNCE_S) return;

    (void)blob_write_now();
}

bool save_controller_is_enabled(void) {
    return s_initialized;
}

int save_controller_get_active_slot(void) {
    return s_active_slot;
}

bool save_controller_set_active_slot(int slot) {
    if (slot < 0) slot = 0;
    if (slot >= SAVE_SLOT_COUNT) slot = SAVE_SLOT_COUNT - 1;
    s_active_slot = slot;

    if (!s_initialized) {
        // Keep defaults in memory even if EEPROM isn't available
        blob_seed_defaults();
        return false;
    }

    // Persist pending (debounced) setting changes first: the re-read below would drop them
    if (s_dirty) (void)blob_write_now();

    // Ensure we have a valid blob loaded
    (void)blob_read_or_seed_defaults();

    // If just this slot is invalid, reset it and persist.
    if (!validate_slot(&s_blob.slots[s_active_slot], s_active_slot)) {
        debugf("Save slot %d invalid: resetting to defaults\n", s_active_slot);
        save_defaults_for_slot(&s_blob.slots[s_active_slot], s_active_slot);
        (void)blob_write_now();
    }

    (void)save_controller_load_settings();
    return true;
}

bool save_controller_load_settings(void) {
    if (!s_initialized) return false;

    const SaveData *d = &s_blob.slots[s_active_slot];

    audio_set_loading_mode(true);
    audio_set_master_volume((int)d->masterVolume);
    audio_set_music_volume((int)d->musicVolume);
    audio_set_sfx_volume((int)d->sfxVolume);
    audio_set_mute(d->globalMute != 0);
    audio_set_stereo_mode(d->stereoMode != 0);
    audio_set_loading_mode(false);

    // UI overscan (applied to edge-anchored UI via globals.h helpers)
    uiOverscanX = saved_overscan_x(d);
    uiOverscanY = saved_overscan_y(d);

    joypad_set_rumble_enabled(saved_rumble_enabled(d));
    hdAspect = saved_hd_aspect(d);
    character_set_skills_enabled(saved_skills_enabled(d));
    // a skin this slot hasn't unlocked (an older save, before the unlocks) falls back to the Knight
    character_skin_set(save_progress_skin_unlocked(d, saved_skin(d)) ? saved_skin(d) : SKIN_KNIGHT);
#ifdef WIP_TEST_EMBERS   // testing aid for the WIP builds (the Makefile): every slot starts with at least this many Embers
    { TowerSave t; save_controller_get_tower(&t); if (t.embers < WIP_TEST_EMBERS) { t.embers = WIP_TEST_EMBERS; save_controller_set_tower(&t); }
#ifdef HLE_AUTOPLAY
      { TowerSave u; save_controller_get_tower(&u); debugf("[save] Embers on boot: %d\n", u.embers); }
#endif
    }
#endif
    joypad_set_layout(d->_pad[20]);                         // 0 (older saves): classic
    camera_set_speed(d->_pad[21] ? d->_pad[21] : 3);        // 0 (older saves): the original speed
    return true;
}

bool save_controller_save_settings(void) {
    if (!s_initialized) return false;

    SaveData *d = &s_blob.slots[s_active_slot];
    d->masterVolume = (int8_t)audio_get_master_volume();
    d->musicVolume  = (int8_t)audio_get_music_volume();
    d->sfxVolume    = (int8_t)audio_get_sfx_volume();
    d->globalMute   = (uint8_t)(audio_is_muted() ? 1 : 0);
    d->stereoMode   = (uint8_t)(audio_get_stereo_mode() ? 1 : 0);

    // Persist current UI overscan values
    save_set_overscan(d, uiOverscanX, uiOverscanY);
    save_set_rumble_enabled(d, joypad_is_rumble_enabled());
    save_set_hd_aspect(d, hdAspect);
    save_set_skills_enabled(d, character_skills_enabled());
    d->_pad[5] = (uint8_t)character_skin_get();
    d->_pad[6] = 0;                                         // was the Hood setting: cleared for reuse
    d->_pad[20] = (uint8_t)joypad_get_layout();
    d->_pad[21] = (uint8_t)camera_get_speed();
    // the slot just changed is the one reopened on boot (Save 1 on a fresh EEPROM)
    if (d->last_played_timestamp == 0 || d->last_played_timestamp != s_play_session_counter) d->last_played_timestamp = ++s_play_session_counter;

    d->checksum     = compute_slot_checksum(d);

    // Debounced flush
    extern double nowS;
    s_last_dirty_time_s = nowS;
    s_dirty = true;
    return true;
}

bool save_controller_increment_run_count(void) {
    if (!s_initialized) {
        debugf("Run count increment skipped (saves disabled)\n");
        return false;
    }

    SaveData *d = &s_blob.slots[s_active_slot];
    d->run_count += 1;
    d->checksum = compute_slot_checksum(d);
    return blob_write_now();
}

// _pad[7..8] embers, [9..10] five 3-bit levels, [11] best floor, [12] weapons owned, [13] weapon, [15] Bonus bosses beaten
void save_controller_get_tower(TowerSave *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_initialized) return;
    const uint8_t *p = s_blob.slots[s_active_slot]._pad;
    out->embers = (uint16_t)(p[7] | (p[8] << 8));
    uint16_t lv = (uint16_t)(p[9] | (p[10] << 8));
    for (int i = 0; i < 5; i++) out->levels[i] = (lv >> (i * 3)) & 7;
    out->bestFloor = p[11];
    out->weaponsOwned = p[12];
    out->weapon = p[13];
    out->flags = p[19];      // (14 held the old skills: stale on older saves)
    for (int i = 0; i < 3; i++) out->relics[i] = p[16 + i];
}

bool save_controller_set_tower(const TowerSave *in)
{
    if (!s_initialized) return false;
    SaveData *d = &s_blob.slots[s_active_slot];
    uint8_t *p = d->_pad;
    p[7] = in->embers & 0xFF; p[8] = in->embers >> 8;
    uint16_t lv = 0;
    for (int i = 0; i < 5; i++) lv |= (uint16_t)((in->levels[i] & 7) << (i * 3));
    p[9] = lv & 0xFF; p[10] = lv >> 8;
    p[11] = in->bestFloor;
    p[12] = in->weaponsOwned;
    p[13] = in->weapon;
    p[19] = in->flags;
    for (int i = 0; i < 3; i++) p[16 + i] = in->relics[i];
    d->checksum = compute_slot_checksum(d);
    extern double nowS;
    s_last_dirty_time_s = nowS;
    s_dirty = true;
    return true;
}

int save_progress_tower_best_floor(const SaveData *d) { return d ? d->_pad[11] : 0; }
int save_progress_tower_embers(const SaveData *d) { return d ? (d->_pad[7] | (d->_pad[8] << 8)) : 0; }
int save_progress_bonus_mask(const SaveData *d) { return d ? (d->_pad[15] & 7) : 0; }

// The Knight, the Umber cloak and the three cameo skins from the start; the Crimson
// cloak for beating the story (Guardian, then Unbound: a clear time is recorded); the
// Teal cloak for two circles of the Tower (every boss twice: floor 10)
bool save_progress_skin_unlocked(const SaveData *d, int skin)
{
    if (skin == SKIN_CRIMSON_CLOAK) return d && d->best_boss_time_ms > 0;
    if (skin == SKIN_TEAL_CLOAK) return save_progress_tower_best_floor(d) >= 10;
    if (skin == SKIN_NIGHTFARER) return d && (d->_pad[19] & TOWER_FLAG_NIGHTFARER);   // bought at the Shrine
    return skin >= 0 && skin < SKIN_COUNT;
}

int save_progress_skins_unlocked(const SaveData *d)
{
    return save_progress_skin_unlocked(d, SKIN_CRIMSON_CLOAK) + save_progress_skin_unlocked(d, SKIN_TEAL_CLOAK)
         + save_progress_skin_unlocked(d, SKIN_NIGHTFARER);
}

bool save_controller_skin_unlocked(int skin)
{
    return save_progress_skin_unlocked(s_initialized ? &s_blob.slots[s_active_slot] : NULL, skin);
}

void save_controller_mark_bonus_beaten(int bonusIndex)
{
    if (!s_initialized || bonusIndex < 0 || bonusIndex > 2) return;
    SaveData *d = &s_blob.slots[s_active_slot];
    if (d->_pad[15] & (1 << bonusIndex)) return;
    d->_pad[15] |= (uint8_t)(1 << bonusIndex);
    d->checksum = compute_slot_checksum(d);
    extern double nowS;
    s_last_dirty_time_s = nowS;
    s_dirty = true;
}

bool save_controller_record_boss_clear_time_ms(uint32_t clear_time_ms) {
    if (!s_initialized) return false;
    if (clear_time_ms == 0) return false;

    SaveData *d = &s_blob.slots[s_active_slot];
    if (d->best_boss_time_ms == 0 || clear_time_ms < d->best_boss_time_ms) {
        d->best_boss_time_ms = clear_time_ms;
        d->checksum = compute_slot_checksum(d);
        return blob_write_now();
    }
    return true;
}

uint32_t save_controller_get_run_count(void) {
    if (!s_initialized) return 0;
    return s_blob.slots[s_active_slot].run_count;
}

uint32_t save_controller_get_best_boss_time_ms(void) {
    if (!s_initialized) return 0;
    return s_blob.slots[s_active_slot].best_boss_time_ms;
}

const SaveData* save_controller_get_slot_data(int slot) {
    if (!s_initialized) return NULL;
    if (slot < 0 || slot >= SAVE_SLOT_COUNT) return NULL;
    return &s_blob.slots[slot];
}

uint32_t save_controller_get_slot_run_count(int slot) {
    if (!s_initialized) return 0;
    if (slot < 0 || slot >= SAVE_SLOT_COUNT) return 0;
    return s_blob.slots[slot].run_count;
}

uint32_t save_controller_get_slot_best_boss_time_ms(int slot) {
    if (!s_initialized) return 0;
    if (slot < 0 || slot >= SAVE_SLOT_COUNT) return 0;
    return s_blob.slots[slot].best_boss_time_ms;
}

uint32_t save_controller_get_slot_last_played_timestamp(int slot) {
    if (!s_initialized) return 0;
    if (slot < 0 || slot >= SAVE_SLOT_COUNT) return 0;
    return s_blob.slots[slot].last_played_timestamp;
}

int save_controller_get_last_played_slot(void) {
    if (!s_initialized) return -1;
    
    int lastPlayedSlot = -1;
    uint32_t lastPlayedTime = 0;
    
    for (int i = 0; i < SAVE_SLOT_COUNT; i++) {
        if (s_blob.slots[i].last_played_timestamp > lastPlayedTime) {
            lastPlayedTime = s_blob.slots[i].last_played_timestamp;
            lastPlayedSlot = i;
        }
    }
    
    return lastPlayedSlot;
}

bool save_controller_update_last_played_timestamp(void) {
    if (!s_initialized) return false;
    
    // Use a simple incrementing counter to track last played
    // In a real game, you might use actual timestamps
    SaveData *d = &s_blob.slots[s_active_slot];
    d->last_played_timestamp = ++s_play_session_counter;
    d->checksum = compute_slot_checksum(d);
    return blob_write_now();
}

bool save_controller_clear_slot(int slot) {
    if (!s_initialized) return false;
    if (slot < 0 || slot >= SAVE_SLOT_COUNT) return false;
    
    // Reset slot to defaults (empty)
    save_defaults_for_slot(&s_blob.slots[slot], slot);
    return blob_write_now();
}

void save_controller_free(void) {
    if (s_initialized && s_dirty) {
        (void)blob_write_now();
    }
    s_dirty = false;
    s_initialized = false;
    s_eeprom_available = false;

    // Optional: clean up the FS table. Not strictly required.
    (void)eepfs_close();
}