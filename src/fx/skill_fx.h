#ifndef SKILL_FX_H
#define SKILL_FX_H

#include <stdbool.h>

// Player skill effects (D-pad skills): sword waves and ground blades that hit
// the boss, plus purely visual light pillars and ground bursts. Drawn as
// alpha-blended world-space geometry, like the sword trail.

typedef enum {
    SKILL_WAVE_X,       // Moon Blade Dance: two crossed crescents
    SKILL_WAVE_BLADE,   // Crescent Cleave: tall, slightly curved light blade rising out of the ground
} SkillWaveShape;

void skill_fx_init(void);
void skill_fx_reset(void);
void skill_fx_update(float dt);
void skill_fx_draw(void);

// Halfmoon Slash: a 180-degree crescent that grows out to `radius` in front of
// `pos` (facing `yaw`) and hits the boss once if it is inside the half-disc.
void skill_fx_spawn_arc(const float pos[3], float yaw, float radius, float damage);

// Moving wave. It damages the boss every `hitInterval` seconds while touching
// it, at most `hits` times; with `pillarOnEnd` it ends in a light pillar.
void skill_fx_spawn_wave(SkillWaveShape shape, const float pos[3], const float vel[3],
                         float damage, int hits, float hitInterval, float life, bool pillarOnEnd);

void skill_fx_spawn_pillar(const float pos[3]);

// Swirling orange/violet funnel hanging down from `top` (the ending's vortex).
void skill_fx_draw_vortex(const float top[3], float depth, float radius, float time);
void skill_fx_spawn_burst(const float pos[3], float yaw);

// Hostile versions (the Fallen Knight): same shapes in a crimson palette, hitting
// the player instead of the boss.
void skill_fx_spawn_burst_hostile(const float pos[3], float yaw);
void skill_fx_spawn_arc_hostile(const float pos[3], float yaw, float radius, float damage);
void skill_fx_spawn_wave_hostile(SkillWaveShape shape, const float pos[3], const float vel[3],
                                 float damage, int hits, float hitInterval, float life, bool pillarOnEnd);

#endif
