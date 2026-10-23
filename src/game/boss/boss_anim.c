/*
 * boss_anim.c
 * 
 * ONLY file allowed to call:
 * - t3d_anim_attach
 * - t3d_skeleton_reset
 * - Direct writes to currentAnimation, previousAnimation, blend vars
 * 
 * This module owns all animation state and tiny3d animation structs.
 */

#include "boss_anim.h"
#include "boss.h"

#include <libdragon.h>
#include <t3d/t3dskeleton.h>
#include <t3d/t3danim.h>
#include <math.h>
#include <string.h>

#include "game_time.h"


void boss_anim_init(Boss* boss) {
    if (!boss) return;
    
    // Stop all animations first to ensure clean state
    if (boss->animations) {
        T3DAnim** anims = (T3DAnim**)boss->animations;
        for (int i = 0; i < boss->animationCount; i++) {
            if (anims[i]) {
                t3d_anim_set_playing(anims[i], false);
                t3d_anim_set_time(anims[i], 0.0f);
            }
        }
    }
    
    // Reset skeletons
    if (boss->skeleton) {
        t3d_skeleton_reset((T3DSkeleton*)boss->skeleton);
    }
    if (boss->skeletonBlend) {
        t3d_skeleton_reset((T3DSkeleton*)boss->skeletonBlend);
    }
    
    // Initialize animation state
    boss->currentAnimation = BOSS_ANIM_KNEEL;
    boss->previousAnimation = -1;
    boss->currentAnimState = BOSS_ANIM_KNEEL;
    boss->currentPriority = BOSS_ANIM_PRIORITY_NORMAL;
    boss->lockFrames = 0;
    boss->blendFactor = 0.0f;
    boss->blendDuration = 0.5f;
    boss->blendTimer = 0.0f;
    boss->isBlending = false;
    
    // Set up idle animation properly
    if (boss->animations && boss->animationCount > 0 && boss->skeleton) {
        T3DAnim** anims = (T3DAnim**)boss->animations;
        T3DSkeleton* skeleton = (T3DSkeleton*)boss->skeleton;
        if (anims[0]) {
            t3d_anim_attach(anims[BOSS_ANIM_KNEEL], skeleton);
            t3d_anim_set_playing(anims[BOSS_ANIM_KNEEL], true);
            t3d_anim_set_time(anims[BOSS_ANIM_KNEEL], 0.0f);
        }
    }
}

// The aerial barrage's breathing pose (boss_anim_breathe)
static T3DSkeleton s_breathSkel;
static bool s_breathReady;
static float s_breathK = -1.0f;     // < 0: not breathing
#ifndef CRUCIFIX_SWORD_DOWN
#define CRUCIFIX_SWORD_DOWN 0.35f   // radians, about the sword hand's own z
#endif

void boss_anim_request(Boss* boss, BossAnimState target, float start_time, bool force_restart, BossAnimPriority priority) {
    if (!boss || !boss->animations) return;
    
    // Check if locked (higher priority can interrupt)
    if (boss->lockFrames > 0 && priority <= boss->currentPriority) {
        // Lower or equal priority request ignored while locked
        return;
    }
    // Check if we need to change animation
    bool needsChange = false;
    if (boss->currentAnimState != target) {
        needsChange = true;
    } else if (force_restart) {
        needsChange = true;
    }
    
    if (!needsChange) {
        return;  // Already playing the requested animation
    }
    
    // If we're already blending, complete the blend first
    if (boss->isBlending) {
        // Force complete the blend
        boss->blendFactor = 1.0f;
        boss->isBlending = false;
        boss->blendTimer = 0.0f;
        
        // Stop previous animation
        T3DAnim** anims = (T3DAnim**)boss->animations;
        if (boss->previousAnimation >= 0 && boss->previousAnimation < boss->animationCount) {
            t3d_anim_set_playing(anims[boss->previousAnimation], false);
        }
    }
    
    // Store previous animation for blending
    boss->previousAnimation = boss->currentAnimation;
    
    // Start blending if we have a valid previous animation
    if (boss->previousAnimation >= 0 && boss->previousAnimation < boss->animationCount) {
        T3DAnim** anims = (T3DAnim**)boss->animations;
        T3DSkeleton* skeletonBlend = (T3DSkeleton*)boss->skeletonBlend;
        
        // Save the current animation time before switching
        float savedTime = 0.0f;
        if (boss->currentAnimation >= 0 && boss->currentAnimation < boss->animationCount && 
            anims[boss->currentAnimation]) {
            savedTime = anims[boss->currentAnimation]->time;
        }
        
        // Set up blend skeleton to preserve current visual state
        t3d_skeleton_reset(skeletonBlend);
        t3d_anim_attach(anims[boss->previousAnimation], skeletonBlend);
        t3d_anim_set_playing(anims[boss->previousAnimation], true);
        t3d_anim_set_time(anims[boss->previousAnimation], savedTime);
        
        // Start blending
        boss->isBlending = true;
        boss->blendFactor = 0.0f;
        boss->blendTimer = 0.0f;
    }
    
    // Start new animation on main skeleton
    boss->currentAnimation = (int)target;
    boss->currentAnimState = target;
    boss->currentPriority = priority;
    
    if (boss->currentAnimation >= 0 && boss->currentAnimation < boss->animationCount) {
        T3DAnim** anims = (T3DAnim**)boss->animations;
        T3DSkeleton* skeleton = (T3DSkeleton*)boss->skeleton;
        
        // Reset main skeleton and attach new animation
        t3d_skeleton_reset(skeleton);
        t3d_anim_attach(anims[boss->currentAnimation], skeleton);
        t3d_anim_set_playing(anims[boss->currentAnimation], true);
        t3d_anim_set_speed(anims[boss->currentAnimation], 1.0f);    // boss_anim_breathe holds its clip
        t3d_anim_set_time(anims[boss->currentAnimation], start_time);
        s_breathK = -1.0f;
    }
    
    // Set lock frames based on priority (critical animations lock longer)
    if (priority >= BOSS_ANIM_PRIORITY_HIGH) {
        boss->lockFrames = 10;  // Lock for 10 frames (prevents rapid interrupts)
    } else {
        boss->lockFrames = 3;   // Normal lock
    }
}

// The aerial barrage's levitation pose breathes between two poses of one clip, the far one
// cached once (seeking the streamed clip every frame cost the frame 10 ms)

void boss_anim_breathe(Boss* boss, int anim, float t0, float t1, float k)
{
    T3DAnim* pose = ((T3DAnim**)boss->animations)[anim];
    T3DSkeleton* skel = (T3DSkeleton*)boss->skeleton;
    if (s_breathK < 0.0f) {
        if (!s_breathReady) { s_breathSkel = t3d_skeleton_clone(skel, false); s_breathReady = true; }
        t3d_anim_attach(pose, &s_breathSkel);
        t3d_anim_set_time(pose, t1);
        t3d_anim_update(pose, 0.0f);
        t3d_anim_attach(pose, skel);
        t3d_anim_set_time(pose, t0);
        t3d_anim_set_speed(pose, 0.0f);     // held at t0; boss_anim_request restores the speed
    }
    s_breathK = k;
}

void boss_anim_update(Boss* boss) {
    if (!boss || !boss->skeleton || !boss->animations || !boss->skeletonBlend) return;
    // Safety check: ensure deltaTime is valid (not zero, negative, or denormal)
    // Use a minimum threshold to prevent denormal floating point values
    const float MIN_DELTA_TIME = 0.0001f;
    const float MAX_DELTA_TIME = 1.0f;  // Cap at 1 second to prevent huge jumps
    // Check for invalid values: zero, negative, too large, or NaN/Inf
    if (deltaTime < MIN_DELTA_TIME || deltaTime > MAX_DELTA_TIME || deltaTime != deltaTime) {
        // Invalid deltaTime - use a safe default (60 FPS)
        deltaTime = 1.0f / 60.0f;
    }
    
    // Update attack animation timer
    // This manages the isAttacking flag for attacks that use it (like tracking slam, power jump, flip attack)
    if (boss->isAttacking) {
        boss->attackAnimTimer += deltaTime;
        // Use longer duration for attacks that need to complete their animation
        float attackDuration = 0.9f; // Default duration
        if (boss->state == BOSS_STATE_TRACKING_SLAM) {
            attackDuration = 5.0f;
        } else if (boss->state == BOSS_STATE_POWER_JUMP) {
            attackDuration = 6.0f; // Allow power jump animation to complete and blend properly
        } else if (boss->state == BOSS_STATE_FLIP_ATTACK) {
            attackDuration = 7.5f; // Allow flip attack animation to complete
        }
        if (boss->attackAnimTimer >= attackDuration) {
            boss->isAttacking = false;
            boss->attackAnimTimer = 0.0f;
        }
    }
    
    // Safety check: ensure main skeleton has an animation attached
    // If currentAnimation is invalid, attach idle animation
    if (boss->currentAnimation < 0 || boss->currentAnimation >= boss->animationCount) {
        T3DAnim** anims = (T3DAnim**)boss->animations;
        T3DSkeleton* skeleton = (T3DSkeleton*)boss->skeleton;
        if (anims && anims[BOSS_ANIM_IDLE] && skeleton) {
            t3d_skeleton_reset(skeleton);
            t3d_anim_attach(anims[BOSS_ANIM_IDLE], skeleton);
            t3d_anim_set_playing(anims[BOSS_ANIM_IDLE], true);
            boss->currentAnimation = BOSS_ANIM_IDLE;
            boss->currentAnimState = BOSS_ANIM_IDLE;
        } else {
            return;  // Can't proceed without valid animation
        }
    }
    
    // Decrement lock frames
    if (boss->lockFrames > 0) {
        boss->lockFrames--;
    }
    
    T3DAnim** anims = (T3DAnim**)boss->animations;
    // Update animation
    t3d_anim_update(anims[boss->currentAnimation], deltaTime);

    // Update blending
    if (boss->isBlending) {
        boss->blendTimer += deltaTime;
        
        // Safety check: ensure blendDuration is valid to prevent division by zero or denormal results
        // Use a minimum threshold (0.001f) to prevent denormal floating point values
        const float MIN_BLEND_DURATION = 0.001f;
        if (boss->blendDuration < MIN_BLEND_DURATION) {
            // Invalid blend duration - disable blending immediately
            boss->blendFactor = 1.0f;
            boss->isBlending = false;
            boss->blendTimer = 0.0f;
            boss->blendDuration = 0.5f;  // Reset to default value
            
            // Stop previous animation
            if (boss->previousAnimation >= 0 && boss->previousAnimation < boss->animationCount) {
                t3d_anim_set_playing(anims[boss->previousAnimation], false);
            }
        } else {
            // Clamp negative timer to 0
            if (boss->blendTimer < 0.0f) {
                boss->blendTimer = 0.0f;
                boss->blendFactor = 0.0f;
            } else if (boss->blendTimer >= boss->blendDuration) {
                // Blend complete
                boss->blendFactor = 1.0f;
                boss->isBlending = false;
                boss->blendTimer = 0.0f;
                
                // Stop previous animation
                if (boss->previousAnimation >= 0 && boss->previousAnimation < boss->animationCount) {
                    t3d_anim_set_playing(anims[boss->previousAnimation], false);
                }
            } else {
                // Interpolate blend factor (safe division - blendDuration is guaranteed >= MIN_BLEND_DURATION)
                boss->blendFactor = boss->blendTimer / boss->blendDuration;
            }
        }
    }
    
    // Blend skeletons if blending is active
    if (boss->isBlending && boss->skeletonBlend) {
        T3DAnim** anims = (T3DAnim**)boss->animations;
        bool canBlend = (boss->previousAnimation >= 0 && 
                        boss->previousAnimation < boss->animationCount && 
                        anims[boss->previousAnimation] != NULL &&
                        anims[boss->previousAnimation]->isPlaying &&
                        boss->blendFactor >= 0.0f && 
                        boss->blendFactor <= 1.0f &&
                        boss->blendTimer >= 0.0f);
        
        if (canBlend) {
            T3DSkeleton* skeleton = (T3DSkeleton*)boss->skeleton;
            T3DSkeleton* skeletonBlend = (T3DSkeleton*)boss->skeletonBlend;
            
            // Blend skeletons: blend from skeletonBlend (old) to skeleton (new), store result in skeleton
            t3d_skeleton_blend(skeletonBlend, skeleton, skeleton, boss->blendFactor);
        } else {
            // Not safe to blend - disable blending
            boss->isBlending = false;
            boss->blendFactor = 0.0f;
            boss->blendTimer = 0.0f;
            
            if (boss->previousAnimation >= 0 && boss->previousAnimation < boss->animationCount &&
                anims[boss->previousAnimation]) {
                t3d_anim_set_playing(anims[boss->previousAnimation], false);
            }
        }
    }
    
    if (s_breathK >= 0.0f) {
        if (boss->state == BOSS_STATE_AERIAL_SWORD_BARRAGE && boss->currentAnimation == BOSS_ANIM_PHASE2_REVEAL)
            t3d_skeleton_blend((T3DSkeleton*)boss->skeleton, (T3DSkeleton*)boss->skeleton, &s_breathSkel, s_breathK);
        else s_breathK = -1.0f;
    }
    // Guardian Unbound, hanging in the air (the barrage, or up high outside its jumps, as when
    // the fight opens): a crucifix, arms spread wide and legs together, the sword angled down.
    // The bind pose is the model's T-pose, so the spine, arms and legs ease to it (bones 2-4,
    // 6-21); the weight eases in and out (smoothstep), so it never snaps
    {
        static float crossT;
        bool jump = boss->state == BOSS_STATE_POWER_JUMP || boss->state == BOSS_STATE_FLIP_ATTACK || boss->state == BOSS_STATE_TRACKING_SLAM;
        bool hang = boss->phaseIndex == 2 && boss->state != BOSS_STATE_DEAD &&
                    (boss->state == BOSS_STATE_AERIAL_SWORD_BARRAGE || (boss->pos[1] > 30.0f && !jump));
        crossT += (hang ? 1.0f : -1.0f) * deltaTime * 1.6f;
        crossT = fminf(1.0f, fmaxf(0.0f, crossT));
        float w = crossT * crossT * (3.0f - 2.0f * crossT);
        if (w > 0.0f) {
            T3DSkeleton *sk = (T3DSkeleton*)boss->skeleton;
            for (int i = 2; i <= 21; i++) {
                if (i == 5) continue;       // the head keeps the clip's bow
                t3d_quat_nlerp(&sk->bones[i].rotation, &sk->bones[i].rotation, &sk->skeletonRef->bones[i].rotation, w);
                sk->bones[i].hasChanged = true;
            }
            // the sword hand turned so the blade points a little down
            T3DQuat down, h = sk->bones[13].rotation;
            t3d_quat_from_rotation(&down, (float[3]){ 0.0f, 0.0f, 1.0f }, CRUCIFIX_SWORD_DOWN * w);
            t3d_quat_mul(&sk->bones[13].rotation, &h, &down);
        }
    }

    // Update main skeleton (ONLY the main skeleton, never the blend skeleton)
    // Only update if we have a valid current animation
    // We ensure the animation is attached in boss_anim_request, so we can safely update here
    if (boss->currentAnimation >= 0 && boss->currentAnimation < boss->animationCount) {
        T3DAnim** anims = (T3DAnim**)boss->animations;
        T3DSkeleton* skeleton = (T3DSkeleton*)boss->skeleton;
        if (anims && anims[boss->currentAnimation] && skeleton) {
            // Update the skeleton - animation should already be attached from boss_anim_request
            // If not attached, this will cause issues, but boss_anim_request should always attach it
            t3d_skeleton_update(skeleton);
        }
    }
}


