#include <libdragon.h>
#include <string.h>
#include <stdbool.h>

#include "audio_controller.h"
#include "save_controller.h"
#include "globals.h"

/* ============================================================================
 * Internal helpers / state
 * ============================================================================
 */

static wav64_t currentMusic;
static bool musicPlaying = false;
static char currentMusicPath[256] = "";
static bool currentMusicLoop = false;

// Volume control (0-10 scale)
static int masterVolume = 10;
static int musicVolume  = 6;
static int sfxVolume    = 10;

static bool globalMute        = false;
static bool isLoadingSettings = false;
static bool pauseMuted        = false;
static bool stereoMode        = true;

// Music fade-out state
static bool  musicFadingOut    = false;
static float musicFadeT        = 0.0f;
static float musicFadeDuration = 0.0f;
static float musicFadeStartVol = 0.0f;

// Headroom cap: prevents per-channel SFX from hitting full-scale (helps avoid bus clipping)
static float sfxNearGain      = 0.72f;  // was 0.60
static float sfxMaxChannelVol = 0.75f;  // was 0.65

static inline float clamp01(float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

static inline float cap_sfx(float v) {
    v = clamp01(v);
    if (v > sfxMaxChannelVol) v = sfxMaxChannelVol;
    return v;
}

// Convert 0-10 scale to 0.0-1.0 float
static float volume_to_float(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 10) volume = 10;
    return (float)volume / 10.0f;
}

// Apply master volume and mute to a specific volume
static float apply_volume_settings(int specificVolume) {
    if (globalMute) return 0.0f;
    return volume_to_float(masterVolume) * volume_to_float(specificVolume);
}

// Apply stereo/mono mode to volume values
static void apply_stereo_volume(int ch, float volume) {
    if (stereoMode) {
        mixer_ch_set_vol(ch, volume, volume);
    } else {
        mixer_ch_set_vol(ch, volume, volume);
    }
}

/* ============================================================================
 * Scene SFX Cache
 * ============================================================================
 */

static wav64_t sceneWavs[AUDIO_SCENE_MAX_SFX];
static wav64_t *sceneHeld[AUDIO_SCENE_MAX_SFX];    // a copy held in memory (audio_scene_hold_sfx), or NULL
static const char *scenePaths[AUDIO_SCENE_MAX_SFX];
static bool    sceneLoaded[AUDIO_SCENE_MAX_SFX];
static int     sceneCount = 0;

/* ============================================================================
 * Dynamic SFX channel slots
 * ============================================================================
 */

typedef struct {
    int   ch;
    bool  inUse;

    int   sceneIndex;
    float baseVolMul;
    float distance; // last provided distance
} SfxSlot;

static SfxSlot sfxSlots[MIXER_NUM_CHANNELS];

// Distance attenuation parameters (tune to world scale)
static float sfxMinDist = 10.0f;   // full volume at/inside this
static float sfxMaxDist = 200.0f;  // silent at/after this
static float sfxMinGain = 0.0f;    // floor gain

static inline bool ch_is_sfx_eligible(int ch)
{
    return (ch >= SFX_CH_FIRST && ch <= SFX_CH_LAST);
}

// Linear falloff (cheap)
static float sfx_distance_gain(float d)
{
    if (d <= sfxMinDist) return sfxNearGain;
    if (d >= sfxMaxDist) return sfxMinGain;

    float t = (d - sfxMinDist) / (sfxMaxDist - sfxMinDist);
    t = clamp01(t);

    // linear falloff from nearGain down to minGain
    float g = sfxNearGain + (sfxMinGain - sfxNearGain) * t;
    if (g < sfxMinGain) g = sfxMinGain;
    return g;
}

static void sfx_slots_init(void)
{
    for (int i = 0; i < MIXER_NUM_CHANNELS; i++) {
        sfxSlots[i].ch = i;
        sfxSlots[i].inUse = false;
        sfxSlots[i].sceneIndex = -1;
        sfxSlots[i].baseVolMul = 1.0f;
        sfxSlots[i].distance = 0.0f;
    }
}

static void sfx_slot_release(SfxSlot *slot)
{
    if (!slot) return;
    mixer_ch_stop(slot->ch);
    slot->inUse = false;
    slot->sceneIndex = -1;
}

static void sfx_reap_finished(void)
{
    for (int ch = 0; ch < MIXER_NUM_CHANNELS; ch++) {
        if (!ch_is_sfx_eligible(ch)) continue;

        SfxSlot *slot = &sfxSlots[ch];
        if (!slot->inUse) continue;

        if (!mixer_ch_playing(ch)) {
            sfx_slot_release(slot);
        }
    }
}

static SfxSlot* sfx_find_free_slot(void)
{
    // reclaim finished first
    sfx_reap_finished();

    for (int ch = 0; ch < MIXER_NUM_CHANNELS; ch++) {
        if (!ch_is_sfx_eligible(ch)) continue;

        SfxSlot *slot = &sfxSlots[ch];
        if (!slot->inUse) return slot;
    }
    return NULL;
}

static void sfx_update_volumes(void)
{
    if (globalMute || pauseMuted) return;

    float sfxBase = apply_volume_settings(sfxVolume);

    for (int ch = 0; ch < MIXER_NUM_CHANNELS; ch++) {
        if (!ch_is_sfx_eligible(ch)) continue;

        SfxSlot *slot = &sfxSlots[ch];
        if (!slot->inUse) continue;
        if (!mixer_ch_playing(ch)) continue;

        float gain = sfx_distance_gain(slot->distance);
        float v = sfxBase * slot->baseVolMul * gain;

        v = cap_sfx(v);
        apply_stereo_volume(ch, v);
    }
}

static void audio_refresh_all_channel_volumes(void)
{
    // Music
    if (!pauseMuted && !musicFadingOut) {
        float mv = musicPlaying ? apply_volume_settings(musicVolume) : 0.0f;
        apply_stereo_volume(CHANNEL_MUSIC, mv);
    }

    // SFX
    if (!pauseMuted) {
        sfx_update_volumes();
    } else {
        apply_stereo_volume(CHANNEL_MUSIC, 0.0f);

        for (int ch = 0; ch < MIXER_NUM_CHANNELS; ch++) {
            if (!ch_is_sfx_eligible(ch)) continue;
            if (sfxSlots[ch].inUse) apply_stereo_volume(ch, 0.0f);
        }
    }
}

/* ============================================================================
 * Scene SFX API
 * ============================================================================
 */

void audio_scene_unload_sfx(void)
{
    audio_stop_all_sfx();

    for (int i = 0; i < sceneCount; i++) {
        if (sceneLoaded[i]) {
            wav64_close(&sceneWavs[i]);
            sceneHeld[i] = NULL;     // the held copy stays (audio_hold_paths)
            sceneLoaded[i] = false;
        }
    }

    sceneCount = 0;
}

void audio_scene_load_paths(const char *const *paths, int count)
{
    audio_scene_unload_sfx();

    if (!paths || count <= 0) return;

    if (count > AUDIO_SCENE_MAX_SFX)
        count = AUDIO_SCENE_MAX_SFX;

    sceneCount = count;

    for (int i = 0; i < sceneCount; i++) {
        sceneLoaded[i] = false;

        const char *p = paths[i];
        if (!p) continue;

        wav64_open(&sceneWavs[i], p);
        scenePaths[i] = p;
        wav64_set_loop(&sceneWavs[i], false);
        sceneLoaded[i] = true;
    }
}

// Sounds held in memory (still compressed): the ones played all through a fight aren't streamed
// from the cartridge as they play (the mixer's reads and bookkeeping for a streamed sound cost
// the CPU about 0.3 ms a frame each, and slowed the RSP). Loaded once, at boot (audio_hold_paths):
// loading them took 220 ms, a stall every time a fight started. Only a few: 4 MB of RAM
static struct { const char *path; wav64_t *w; } s_held[24];
static int s_nHeld;

static wav64_t *held_find(const char *path)
{
    for (int k = 0; k < s_nHeld; k++) if (!strcmp(s_held[k].path, path)) return s_held[k].w;
    return NULL;
}

void audio_hold_paths(const char *const *paths, int count)
{
    for (int k = 0; k < count && s_nHeld < (int)(sizeof s_held / sizeof s_held[0]); k++) {
        if (!paths[k] || held_find(paths[k])) continue;
        wav64_t *w = wav64_load(paths[k], &(wav64_loadparms_t){ .streaming_mode = WAV64_STREAMING_NONE });
        wav64_set_loop(w, false);
        s_held[s_nHeld].path = paths[k]; s_held[s_nHeld].w = w; s_nHeld++;
    }
}

void audio_scene_hold_sfx(const int *indices, int count)
{
    for (int k = 0; k < count; k++) {
        int i = indices[k];
        if (i < 0 || i >= sceneCount || !sceneLoaded[i]) continue;
        if (!held_find(scenePaths[i])) audio_hold_paths(&scenePaths[i], 1);    // not preloaded: now, then kept
        sceneHeld[i] = held_find(scenePaths[i]);
    }
}

/* ============================================================================
 * Public API
 * ============================================================================
 */

void audio_set_loading_mode(bool loading) {
    isLoadingSettings = loading;
}

void audio_initialize(void)
{
    audio_init(22050, 4);
    mixer_init(MIXER_NUM_CHANNELS);
    wav64_init_compression(1);

    mixer_ch_set_limits(CHANNEL_MUSIC, 0, 22050, 0);

    sceneCount = 0;
    for (int i = 0; i < AUDIO_SCENE_MAX_SFX; i++) {
        sceneLoaded[i] = false;
    }

    sfx_slots_init();
}

static void audio_reset_fade(void)
{
    musicFadingOut = false;
    musicFadeT = 0.0f;
    musicFadeDuration = 0.0f;
    musicFadeStartVol = 0.0f;
}

void audio_stop_music(void)
{
    if (musicFadingOut)
        audio_reset_fade();

    if (musicPlaying) {
        mixer_ch_stop(CHANNEL_MUSIC);
        wav64_close(&currentMusic);
        musicPlaying = false;
    }
}

void audio_play_music(const char *path, bool loop)
{
#ifdef HLE_NO_MUSIC   // test aid: the music's cost (silent)
    return;
#endif
    if (musicFadingOut)
        audio_reset_fade();

    if (musicPlaying)
        audio_stop_music();

#ifdef HLE_AUTOPLAY
    debugf("[music] %s%s\n", path, loop ? " (loop)" : "");
#endif
    strncpy(currentMusicPath, path, sizeof(currentMusicPath) - 1);
    currentMusicPath[sizeof(currentMusicPath) - 1] = '\0';
    currentMusicLoop = loop;

    wav64_open(&currentMusic, path);
    // A track with its own loop (tools/music_loop.py) keeps it; others loop whole
    if (!loop || currentMusic.wave.loop_len == 0)
        wav64_set_loop(&currentMusic, loop);
#ifdef HLE_AUTOPLAY
    debugf("[music] len %d loop %d..%d\n", (int)currentMusic.wave.len,
           currentMusic.wave.loop_len ? (int)((currentMusic.wave.loop_end ? currentMusic.wave.loop_end : currentMusic.wave.len) - currentMusic.wave.loop_len) : -1,
           (int)(currentMusic.wave.loop_end ? currentMusic.wave.loop_end : currentMusic.wave.len));
#endif

    mixer_ch_set_freq(CHANNEL_MUSIC, currentMusic.wave.frequency);

    float v = apply_volume_settings(musicVolume);
    if (pauseMuted) v = 0.0f;
    apply_stereo_volume(CHANNEL_MUSIC, v);

    wav64_play(&currentMusic, CHANNEL_MUSIC);
#ifdef HLE_TEST_PITCH   // test aid: the music resampled (the HLE mixer's resample path)
    mixer_ch_set_freq(CHANNEL_MUSIC, currentMusic.wave.frequency * 0.73f);
#endif
    musicPlaying = true;
}

bool audio_is_music_playing(void)
{
    if (!musicPlaying) return false;
    if (pauseMuted) return false;
    return mixer_ch_playing(CHANNEL_MUSIC);
}

void audio_pause_music(void)
{
    if (pauseMuted) return;

    apply_stereo_volume(CHANNEL_MUSIC, 0.0f);

    for (int ch = 0; ch < MIXER_NUM_CHANNELS; ch++) {
        if (!ch_is_sfx_eligible(ch)) continue;
        if (sfxSlots[ch].inUse) {
            apply_stereo_volume(ch, 0.0f);
        }
    }

    pauseMuted = true;
}

void audio_resume_music(void)
{
    if (!pauseMuted) return;

    pauseMuted = false;
    audio_refresh_all_channel_volumes();
}

/* ============================================================================
 * Music fade-out
 * ============================================================================
 */

void audio_update_fade(float dt)
{
#ifdef HLE_LOG_MUSICPOS   // test aid: the music's play position every 2 s (shows the loop's jump back)
    static float logT;
    if (musicPlaying && (logT += dt) >= 2.0f) {
        logT = 0.0f;
        debugf("[musicpos] %lu %.2f s\n", (unsigned long)get_ticks_ms(), mixer_ch_get_pos(CHANNEL_MUSIC) / currentMusic.wave.frequency);
    }
#endif
    if (!musicFadingOut) return;

    musicFadeT += dt;
    float t = (musicFadeDuration > 0.0f) ? (musicFadeT / musicFadeDuration) : 1.0f;
    if (t > 1.0f) t = 1.0f;

    float v = musicFadeStartVol * (1.0f - t);
    apply_stereo_volume(CHANNEL_MUSIC, v);

    if (t >= 1.0f) {
        audio_stop_music();
    }
}

void audio_stop_music_fade(float durationSec)
{
    if (!musicPlaying) return;

    if (durationSec <= 0.0f) {
        audio_stop_music();
        return;
    }

    pauseMuted = false;

    musicFadingOut = true;
    musicFadeT = 0.0f;
    musicFadeDuration = durationSec;

    musicFadeStartVol = apply_volume_settings(musicVolume);

    apply_stereo_volume(CHANNEL_MUSIC, musicFadeStartVol);
}

/* ============================================================================
 * SFX: dynamic + distance-aware
 * ============================================================================
 */

int audio_play_scene_sfx_dist(int sceneSfxIndex, float baseVolume, float distance)
{
    if (globalMute || pauseMuted) return -1;

    if (sceneSfxIndex < 0 || sceneSfxIndex >= sceneCount) return -1;
    if (!sceneLoaded[sceneSfxIndex]) return -1;

    baseVolume = clamp01(baseVolume);
    if (distance < 0.0f) distance = 0.0f;

    SfxSlot *slot = sfx_find_free_slot();
#ifdef HLE_LOG_SFX
    debugf("[sfx] %lu %d d %.0f%s\n", (unsigned long)get_ticks_ms(), sceneSfxIndex, (double)distance, slot ? "" : " (no free channel)");
#endif
    if (!slot) return -1;

    slot->inUse = true;
    slot->sceneIndex = sceneSfxIndex;
    slot->baseVolMul = baseVolume;
    slot->distance = distance;

    float gain = sfx_distance_gain(distance);
    float finalVol = apply_volume_settings(sfxVolume) * baseVolume * gain;

    finalVol = cap_sfx(finalVol);
    apply_stereo_volume(slot->ch, finalVol);

    wav64_play(sceneHeld[sceneSfxIndex] ? sceneHeld[sceneSfxIndex] : &sceneWavs[sceneSfxIndex], slot->ch);
    return slot->ch;
}

void audio_set_scene_sfx_dist(int ch, int sceneSfxIndex, float distance)
{
    if (ch < 0 || ch >= MIXER_NUM_CHANNELS || !ch_is_sfx_eligible(ch)) return;
    SfxSlot *slot = &sfxSlots[ch];
    if (!slot->inUse || slot->sceneIndex != sceneSfxIndex) return;     // finished, the channel since reused
    slot->distance = fmaxf(distance, 0.0f);     // (sfx_update_volumes applies it)
}

void audio_stop_scene_sfx(int ch, int sceneSfxIndex)
{
    if (ch < 0 || ch >= MIXER_NUM_CHANNELS || !ch_is_sfx_eligible(ch)) return;
    SfxSlot *slot = &sfxSlots[ch];
    if (!slot->inUse || slot->sceneIndex != sceneSfxIndex) return;     // finished, the channel since reused
    mixer_ch_stop(ch);
    slot->inUse = false;
}

void audio_stop_all_sfx(void)
{
    for (int ch = 0; ch < MIXER_NUM_CHANNELS; ch++) {
        if (!ch_is_sfx_eligible(ch)) continue;
        sfx_slot_release(&sfxSlots[ch]);
    }
}

/* ============================================================================
 * Per-frame update
 * ============================================================================
 */

void audio_update(float dt)
{
    audio_update_fade(dt);
    sfx_reap_finished();
    sfx_update_volumes();
}

/* ============================================================================
 * Shutdown
 * ============================================================================
 */

void audio_controller_free(void)
{
    audio_stop_music();
    audio_stop_all_sfx();
    audio_scene_unload_sfx();
}

/* ============================================================================
 * Volume controls (0–10)
 * ============================================================================
 */

void audio_set_master_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 10) volume = 10;
    masterVolume = volume;

    if (!pauseMuted) {
        audio_refresh_all_channel_volumes();
    }

    if (!isLoadingSettings) {
        save_controller_save_settings();
    }
}

void audio_set_music_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 10) volume = 10;
    musicVolume = volume;

    if (musicPlaying && !pauseMuted && !musicFadingOut) {
        float mv = apply_volume_settings(musicVolume);
        apply_stereo_volume(CHANNEL_MUSIC, mv);
    }

    if (!isLoadingSettings) {
        save_controller_save_settings();
    }
}

void audio_set_sfx_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 10) volume = 10;
    sfxVolume = volume;

    if (!pauseMuted) {
        sfx_update_volumes();
    }

    if (!isLoadingSettings) {
        save_controller_save_settings();
    }
}

int audio_get_master_volume(void) { return masterVolume; }
int audio_get_music_volume(void)  { return musicVolume; }
int audio_get_sfx_volume(void)    { return sfxVolume; }

void audio_adjust_master_volume(int direction) { audio_set_master_volume(masterVolume + direction); }
void audio_adjust_music_volume(int direction)  { audio_set_music_volume(musicVolume + direction); }
void audio_adjust_sfx_volume(int direction)    { audio_set_sfx_volume(sfxVolume + direction); }

/* ============================================================================
 * Mute
 * ============================================================================
 */

void audio_set_mute(bool muted)
{
    globalMute = muted;

    if (!pauseMuted) {
        audio_refresh_all_channel_volumes();
    }

    if (!isLoadingSettings) {
        save_controller_save_settings();
    }
}

void audio_toggle_mute(void) {
    audio_set_mute(!globalMute);
}

bool audio_is_muted(void) {
    return globalMute;
}

/* ============================================================================
 * Stereo mode
 * ============================================================================
 */

void audio_set_stereo_mode(bool stereo)
{
    stereoMode = stereo;

    if (!pauseMuted) {
        audio_refresh_all_channel_volumes();
    }

    if (!isLoadingSettings) {
        save_controller_save_settings();
    }
}

void audio_toggle_stereo_mode(void) {
    audio_set_stereo_mode(!stereoMode);
}

bool audio_get_stereo_mode(void) {
    return stereoMode;
}
