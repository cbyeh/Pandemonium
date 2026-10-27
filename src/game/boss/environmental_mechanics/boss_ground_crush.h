#ifndef BOSS_GROUND_CRUSH_H
#define BOSS_GROUND_CRUSH_H

void boss_ground_crush_init(void);
void boss_ground_crush_reset(void);
void boss_ground_crush_update(float dt);
void boss_ground_crush_draw(void);
void boss_ground_crush_cleanup(void);

void boss_ground_crush_spawn(float x, float y, float z);
// `size` scales the decal (1 = the Guardian's slam)
void boss_ground_crush_spawn_sized(float x, float y, float z, float size);
// One of four crack shapes (not the last one), turned to `yaw`: the Fallen Warrior's slams
void boss_ground_crush_spawn_varied(float x, float y, float z, float size, float yaw);

#endif