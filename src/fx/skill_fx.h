#ifndef SKILL_FX_H
#define SKILL_FX_H

#include <stdbool.h>

// Player skill effects (D-pad skills): sword waves and ground blades that hit
// the boss, plus purely visual light pillars and ground bursts. Drawn as
// alpha-blended world-space geometry, like the sword trail.

typedef enum {
    SKILL_WAVE_X,       // Moon Blade Dance: two crossed crescents
    SKILL_WAVE_BLADE,   // Crescent Cleave: tall, slightly curved light blade rising out of the ground
    SKILL_WAVE_CRESCENT,    // Moonlight Splitter (the Fallen Warrior): one flying crescent, tilted
    SKILL_WAVE_ORB,     // the Shackled Sun's embers: a small fire star
    SKILL_WAVE_SLICES,  // the Fallen Warrior's Moon Blade: two long thin parallel slashes, apart (the player's is a solid X)
} SkillWaveShape;

void skill_fx_init(void);
void skill_fx_reset(void);
void skill_fx_update(float dt);
void skill_fx_draw(void);

// Halfmoon Slash: a 180-degree crescent that grows out to `radius` in front of
// `pos` (facing `yaw`) and hits the boss once if it is inside the half-disc.
void skill_fx_spawn_arc(const float pos[3], float yaw, float radius, float damage);
/** The stagger the next spawned effect's hits deal (a share of the meter; by default the damage's). */
void skill_fx_next_poise(float poise);
/** The next spawned wave's size (1 = normal) and charge time (it holds still, harmless and growing, then flies). */
void skill_fx_next_shape(float scale, float delay);
/** How far round the next arc reaches each side of its yaw (radians; by default a half-disc, pi/2). */
void skill_fx_next_arc_half(float half);
struct Boss; bool skill_arc_reaches(const struct Boss *b, float arcY);   // Halfmoon's wave reaches the boss (not up in the air)

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
void skill_fx_spawn_pillar_hostile(const float pos[3]);
void skill_fx_spawn_arc_hostile(const float pos[3], float yaw, float radius, float damage);
void skill_fx_spawn_wave_hostile(SkillWaveShape shape, const float pos[3], const float vel[3],
                                 float damage, int hits, float hitInterval, float life, bool pillarOnEnd);
// One crescent flying along `vel`, its blade turned `roll` from level (one hit)
void skill_fx_spawn_crescent_hostile(const float pos[3], const float vel[3], float roll, float damage, float life);

// An ember flying along `vel` (one hit); `homing` > 0 turns it toward the player (rad/s, roughly)
// until it is close, then it flies straight: dodge it late
void skill_fx_spawn_orb_hostile(const float pos[3], const float vel[3], float damage, float life, float homing);

// Removes the bosses' effects, keeping the player's.
void skill_fx_clear_hostile(void);

#endif
