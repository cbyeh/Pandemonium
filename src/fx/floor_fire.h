#ifndef FLOOR_FIRE_H
#define FLOOR_FIRE_H

// A Smouldering Elite's burning floor (docs/design/TOWER_ROGUELIKE.md section 11): where its slams land,
// a patch of Shadowborne's ground flames burns 3 s (a ring round a body 2 s), hurting where it is drawn (6 a second felt). The
// flames are the attack itself: no warning before them, no glow
// A slam landed here (nothing unless the fight is Smouldering): a flame on it and a ring `ring` out (9: a blade's bite;
// a body that lands on the floor, round its edge: the flame where it lies is under it)
void floor_fire_slam(float x, float y, float z, float ring);
void floor_fire_update(float dt);
void floor_fire_draw(void);
void floor_fire_free(void);
int  floor_fire_live(void);     // flames burning (a test aid's count)
bool floor_fire_near(float x, float z, float r);    // a flame burning within r of (x, z) (its drawn edge)

#endif
