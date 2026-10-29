#include <libdragon.h>

#include "game_time.h"

static float s_freezeT, s_slowT, s_slowScale = 1.0f;     // real seconds left

void game_time_hitstop(float seconds) { if (seconds > s_freezeT) s_freezeT = seconds; }
void game_time_slowmo(float seconds, float scale) { s_slowT = seconds; s_slowScale = scale; }
void game_time_clear_effects(void) { s_freezeT = s_slowT = 0.0f; }

double nowS = 0.0;
float gameTime = 0.0f;
float lastTime = 0.0f;
float deltaTime = 0.0f;
int deltaFrames;    // TV frames the last step covered (1 at full speed)

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

    // Calculate the time difference between this frame and the last frame, in whole TV frames:
    // a frame is shown for whole refreshes, so the scene moves on by exactly what it is shown for.
    // Frames start unevenly (the CPU waits on the RSP or a framebuffer by turns) and the scene
    // judders if it moves on by the raw time. The rest carries over, so it keeps to real time
    static float carry;
    float period = 1.0f / display_get_refresh_rate();
    float raw = nowS - lastTime + carry;
    int frames = (int)(raw / period + 0.5f);
    if (frames < 1) frames = 1;
    deltaTime = frames * period;
    deltaFrames = frames;
    carry = raw - deltaTime;
    if (carry > period) carry = period; else if (carry < -period) carry = -period;   // after a hitch (a load)

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
