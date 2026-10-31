#ifndef AUDIO_CONTROLLER_H
#define AUDIO_CONTROLLER_H

#include <libdragon.h>
#include <stdbool.h>

// Mixer / Channel Layout
#define MIXER_NUM_CHANNELS   16
#define CHANNEL_MUSIC        0      // stereo: also takes channel 1
#define SFX_CH_FIRST         2
#define SFX_CH_LAST          (MIXER_NUM_CHANNELS - 1)

typedef struct MultiSfx {
    int   sfxIndex;     // scene-local sfx index
    float triggerTime;  // seconds into the attack
    bool  played;
} MultiSfx;

// Init / Shutdown
void audio_initialize(void);
void audio_controller_free(void);

// Music
void audio_play_music(const char *path, bool loop);
void audio_stop_music(void);
bool audio_is_music_playing(void);

void audio_pause_music(void);
void audio_resume_music(void);

void audio_stop_music_fade(float durationSec);
void audio_update_fade(float dt);

// Scene SFX Loading
#define AUDIO_SCENE_MAX_SFX  80

void audio_scene_load_paths(const char *const *paths, int count);
void audio_scene_unload_sfx(void);
// Holds these sounds in memory for good (played from there, not streamed): call at boot
void audio_hold_paths(const char *const *paths, int count);
// The scene's sounds at these indices play from their held copies (loaded now if not yet)
void audio_scene_hold_sfx(const int *indices, int count);

// Play a scene-local SFX index: the channel it plays on (-1 if none)
int audio_play_scene_sfx_dist(
    int sceneSfxIndex,
    float baseVolume,
    float distance
);
// A sound still playing on `ch` (audio_play_scene_sfx_dist's) heard from `distance` now (a moving source)
bool audio_set_scene_sfx_dist(int ch, int sceneSfxIndex, float distance);
void audio_stop_scene_sfx(int ch, int sceneSfxIndex);   // stop one sound early (if it is still that sound)

void audio_stop_all_sfx(void);

// Volumes
void audio_set_master_volume(int volume);
void audio_set_music_volume(int volume);
void audio_set_sfx_volume(int volume);

int  audio_get_master_volume(void);
int  audio_get_music_volume(void);
int  audio_get_sfx_volume(void);

void audio_adjust_master_volume(int direction);
void audio_adjust_music_volume(int direction);
void audio_adjust_sfx_volume(int direction);

// Mute
void audio_set_mute(bool muted);
void audio_toggle_mute(void);
bool audio_is_muted(void);

// Stereo mode
void audio_set_stereo_mode(bool stereo);
void audio_toggle_stereo_mode(void);
bool audio_get_stereo_mode(void);

// Settings load guard
void audio_set_loading_mode(bool loading);

void audio_update(float dt);

#endif