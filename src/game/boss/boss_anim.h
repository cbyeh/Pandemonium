#ifndef BOSS_ANIM_H
#define BOSS_ANIM_H

#include "boss.h"

// Animation module - ONLY place that can touch t3d_anim_attach, t3d_skeleton_reset, blend flags, current/prev anim
// This module owns all animation state and tiny3d animation structs

void boss_anim_init(Boss* boss);
void boss_anim_request(Boss* boss, BossAnimState target, float start_time, bool force_restart, BossAnimPriority priority);
void boss_anim_update(Boss* boss);
// Hold `anim` at t0, blended toward its pose at t1 by k (0..1), while the aerial barrage lasts
void boss_anim_breathe(Boss* boss, int anim, float t0, float t1, float k);

#endif // BOSS_ANIM_H


