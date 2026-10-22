#ifndef GAME_TIME_H
#define GAME_TIME_H

extern float gameTime;
extern float deltaTime;
extern double nowS;

void game_time_init(void);
void game_time_reset(void);
void game_time_update(void);
// Combat feel: game time nearly stops for `seconds` of real time (hits), or runs at `scale` (a perfect dodge)
void game_time_hitstop(float seconds);
void game_time_slowmo(float seconds, float scale);
void game_time_clear_effects(void);

#endif