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
#include "boss_render.h"

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
static float s_blendDur = 0.4f;     // the current crossfade (boss->blendDuration paces the AI, not this)
// The crucifix pose (boss_anim_update), radians about each bone's own axes
#ifndef CX_SPINE1
#define CX_SPINE1      0.10f
#define CX_HIPS        0.08f
#define CX_SPINE2      0.08f
#define CX_NECK_BOW    0.40f
#define CX_NECK_TILT   -0.25f
#define CX_HEAD_BOW    0.50f
#define CX_HEAD_TILT   -0.40f
#define CX_ARM_RAISE_L -1.10f
#define CX_ARM_RAISE_R 1.10f
#define CX_ELBOW       0.30f
#define CX_WRIST_L     0.30f
#define CX_LEG_IN_L    0.06f
#define CX_LEG_IN_R    -0.03f
#define CX_THIGH       0.04f
#define CX_KNEE_L      0.08f
#define CX_KNEE_R      0.05f
#define CX_FOOT        0.40f
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
    
    // Crossfade from the pose on screen (frozen), so a clip that cuts in, even mid-blend
    // (a stagger breaking a swing), eases over instead of snapping
    T3DAnim** anims = (T3DAnim**)boss->animations;
    if (boss->previousAnimation >= 0 && boss->previousAnimation < boss->animationCount)
        t3d_anim_set_playing(anims[boss->previousAnimation], false);
    boss->previousAnimation = boss->currentAnimation;
    if (boss->previousAnimation >= 0 && boss->previousAnimation < boss->animationCount && boss->skeletonBlend) {
        t3d_skeleton_blend((T3DSkeleton*)boss->skeletonBlend, (T3DSkeleton*)boss->skeleton, (T3DSkeleton*)boss->skeleton, 0.0f);
        boss->isBlending = true;
        boss->blendFactor = 0.0f;
        boss->blendTimer = 0.0f;
        // swings short (the weapon collider rides the hand), a stagger or a fall a little longer
        s_blendDur = priority == BOSS_ANIM_PRIORITY_CRITICAL ? 0.35f : priority >= BOSS_ANIM_PRIORITY_HIGH ? 0.2f : 0.4f;
        if (boss->currentAnimState == BOSS_ANIM_KNEEL && boss->state == BOSS_STATE_RECOVER) s_blendDur = 0.8f;   // up off the knee, unhurried
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
        if (s_blendDur < MIN_BLEND_DURATION) {
            // Invalid blend duration - disable blending immediately
            boss->blendFactor = 1.0f;
            boss->isBlending = false;
            boss->blendTimer = 0.0f;
            
            // Stop previous animation
            if (boss->previousAnimation >= 0 && boss->previousAnimation < boss->animationCount) {
                t3d_anim_set_playing(anims[boss->previousAnimation], false);
            }
        } else {
            // Clamp negative timer to 0
            if (boss->blendTimer < 0.0f) {
                boss->blendTimer = 0.0f;
                boss->blendFactor = 0.0f;
            } else if (boss->blendTimer >= s_blendDur) {
                // Blend complete
                boss->blendFactor = 1.0f;
                boss->isBlending = false;
                boss->blendTimer = 0.0f;
                
                // Stop previous animation
                if (boss->previousAnimation >= 0 && boss->previousAnimation < boss->animationCount) {
                    t3d_anim_set_playing(anims[boss->previousAnimation], false);
                }
            } else {
                // Interpolate blend factor (s_blendDur >= MIN_BLEND_DURATION)
                boss->blendFactor = boss->blendTimer / s_blendDur;
            }
        }
    }
    
    // Blend skeletons if blending is active
    if (boss->isBlending && boss->skeletonBlend) {
        T3DAnim** anims = (T3DAnim**)boss->animations;
        bool canBlend = (boss->previousAnimation >= 0 && 
                        boss->previousAnimation < boss->animationCount && 
                        anims[boss->previousAnimation] != NULL &&
                        boss->blendFactor >= 0.0f && 
                        boss->blendFactor <= 1.0f &&
                        boss->blendTimer >= 0.0f);
        
        if (canBlend) {
            T3DSkeleton* skeleton = (T3DSkeleton*)boss->skeleton;
            T3DSkeleton* skeletonBlend = (T3DSkeleton*)boss->skeletonBlend;
            
            // Blend from skeletonBlend (the old pose) to skeleton (the new clip), into skeleton
            float k = boss->blendFactor;
            t3d_skeleton_blend(skeleton, skeletonBlend, skeleton, k * k * (3.0f - 2.0f * k));   // eased in and out
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
    // the fight opens): crucified, arms spread and raised a little as if hung by the hands,
    // the elbows giving, the head bowed and fallen to one shoulder, the body slack, the legs
    // near straight, feet together and pointed, the sword floating level over the head
    // (boss_render); it breathes slowly. Each bone is set
    // from the bind pose (the model's T-pose) turned about its own axes; the weight eases in
    // and out (smoothstep), so it never snaps
    {
        static float crossT, breathT;
        bool jump = boss->state == BOSS_STATE_POWER_JUMP || boss->state == BOSS_STATE_FLIP_ATTACK || boss->state == BOSS_STATE_TRACKING_SLAM;
        bool hang = boss->phaseIndex == 2 && boss->state != BOSS_STATE_DEAD &&
                    (boss->state == BOSS_STATE_AERIAL_SWORD_BARRAGE || (boss->pos[1] > 30.0f && !jump));
#ifdef HLE_TEST_CRUCIFIX
        hang = boss->phaseIndex == 2;       // test aid: held for a look
#endif
        crossT += (hang ? 1.0f : -1.0f) * deltaTime * 1.6f;
        crossT = fminf(1.0f, fmaxf(0.0f, crossT));
        float w = crossT * crossT * (3.0f - 2.0f * crossT);
        boss_render_set_sword_float(w);     // the sword leaves the hand for over the head
        if (w > 0.0f) {
            breathT += deltaTime;
            float br = sinf(breathT * 1.7f);       // a slow breath, about every 3.7 s
            static const struct { int8_t bone, axis; float a, breath; } CROSS[] = {
                {  2, 2,  CX_SPINE1,   0.03f },  // the body slack, swaying with the breath
                {  3, 0,  CX_HIPS,     0.00f },
                {  3, 2,  CX_SPINE2,   0.03f },
                {  4, 2,  CX_NECK_BOW, 0.02f },  // the head fallen forward and to its right
                {  4, 0,  CX_NECK_TILT, 0.00f },
                {  5, 2,  CX_HEAD_BOW, 0.00f },
                {  5, 0,  CX_HEAD_TILT, 0.00f },
                {  6, 0,  CX_ARM_RAISE_L, 0.03f },   // arms raised a little: hung by the hands
                { 11, 0,  CX_ARM_RAISE_R, -0.03f },
                {  7, 2,  CX_ELBOW,    0.00f },
                { 12, 2,  CX_ELBOW,    0.00f },
                {  8, 0,  CX_WRIST_L,  0.00f },
                { 16, 0,  CX_LEG_IN_L, 0.00f },     // legs together, one knee bent more
                { 19, 0,  CX_LEG_IN_R, 0.00f },
                { 16, 2,  CX_THIGH,    0.00f },
                { 19, 2,  CX_THIGH,    0.00f },
                { 17, 2,  CX_KNEE_L,   0.00f },
                { 20, 2,  CX_KNEE_R,   0.00f },
                { 18, 2,  CX_FOOT,     0.00f },     // feet pointed down
                { 21, 2,  CX_FOOT,     0.00f },
            };
            T3DSkeleton *sk = (T3DSkeleton*)boss->skeleton;
            for (int i = 2; i <= 21; i++) {
                t3d_quat_nlerp(&sk->bones[i].rotation, &sk->bones[i].rotation, &sk->skeletonRef->bones[i].rotation, w);
                sk->bones[i].hasChanged = true;
            }
            for (unsigned i = 0; i < sizeof CROSS / sizeof CROSS[0]; i++) {
                float axis[3] = { 0.0f, 0.0f, 0.0f };
                axis[(int)CROSS[i].axis] = 1.0f;
                T3DQuat q, r = sk->bones[CROSS[i].bone].rotation;
                t3d_quat_from_rotation(&q, axis, (CROSS[i].a + CROSS[i].breath * br) * w);
                t3d_quat_mul(&sk->bones[CROSS[i].bone].rotation, &r, &q);
            }
        }
    }

#ifdef HLE_LOG_ANIMJUMP     // test aid: the largest turn of any bone in one frame, peaked over the 30 frames after a clip change
    {
        static T3DQuat prev[64]; static int since = 999, lastAnim = -1; static float peak;
        T3DSkeleton *sk = (T3DSkeleton*)boss->skeleton;
        if (boss->currentAnimation != lastAnim || (boss->blendTimer == 0.0f && boss->isBlending)) { if (since < 30) debugf("[ajump] clip %d peak %.3f (cut short)\n", lastAnim, (double)peak); since = 0; peak = 0.0f; lastAnim = boss->currentAnimation; }
        float m = 0.0f;
        for (int i = 0; i < sk->skeletonRef->boneCount && i < 64; i++) {
            T3DQuat *q = &sk->bones[i].rotation;
            float d = fabsf(q->v[0]*prev[i].v[0] + q->v[1]*prev[i].v[1] + q->v[2]*prev[i].v[2] + q->v[3]*prev[i].v[3]);
            float a = 2.0f * acosf(fminf(1.0f, d)); if (a > m) m = a; prev[i] = *q;
        }
        if (since < 30) { if (m > peak) peak = m; if (++since == 30) debugf("[ajump] clip %d state %d peak %.3f rad/frame\n", lastAnim, boss->state, (double)peak); }
    }
#endif
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


