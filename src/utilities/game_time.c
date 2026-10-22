#include <libdragon.h>

#include "game_time.h"

static float TIME_SPEED = 1.0f;
static float s_freezeT, s_slowT, s_slowScale = 1.0f;     // real seconds left

void game_time_hitstop(float seconds) { if (seconds > s_freezeT) s_freezeT = seconds; }
void game_time_slowmo(float seconds, float scale) { s_slowT = seconds; s_slowScale = scale; }
void game_time_clear_effects(void) { s_freezeT = s_slowT = 0.0f; }

double nowS = 0.0;
float gameTime = 0.0f;
float lastTime = 0.0f;
float deltaTime = 0.0f;

static float get_time_s() 
{
    return (float)((double)get_ticks_us() / 1000000.0);
}

void game_time_init(void) 
{
    lastTime = get_time_s() - (1.0f / 60.0f);
}
  
void game_time_update(void)
{
    // Get current time in seconds
    nowS = get_time_s();

    // Calculate the time difference between this frame and the last frame
    deltaTime = nowS - lastTime;

    // Hit-stop, then slow motion: game time crawls for a moment of real time (never 0:
    // some systems treat a zero step as a bad frame)
    float real = deltaTime;
    if (s_freezeT > 0.0f) { s_freezeT -= real; deltaTime *= 0.05f; }
    else if (s_slowT > 0.0f) { s_slowT -= real; deltaTime *= s_slowScale; }

    // Update game time with the adjusted deltaTime
    gameTime += deltaTime;

    // Update lastTime for the next frame
    lastTime = nowS;
}

void game_time_reset(void)
{
    gameTime = 0.0f;
    lastTime = 0.0;
    deltaTime = 0.0f;
}
