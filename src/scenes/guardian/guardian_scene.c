#include <libdragon.h>
#include "gpu_frame.h"
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>
#include <t3d/t3dmodel.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "guardian_scene.h"
#include "guardian_scene_sfx.h"
#include "scene_controller.h"

#include "audio_controller.h"

#include "camera_controller.h"

#include "joypad_utility.h"
#include "general_utility.h"
#include "game_lighting.h"
#include "game_time.h"

#include "globals.h"
#include "../../utilities/button_prompt_utility.h"

#include "../../managers/cutscene_manager.h"
#include "../../managers/cutscene_manager_internal.h"
#include "guardian_scene_context.h"

#include "character.h"
#include "character_ui.h"
#include "../../game/boss/boss.h"
#include "../../game/boss/boss_ai.h"
#include "../../game/boss/boss_render.h"
#include "../../game/boss/boss_ui.h"
#include "../../game/boss/environmental_mechanics/boss_ground_crush.h"
#include "../../controllers/dialog_controller.h"
#include "display_utility.h"
#include "../../controllers/menu_controller.h"
#include "save_controller.h"
#include "collision_system.h"
#include "letterbox_utility.h"
#include "../../fx/sword_trail.h"
#include "../../fx/screen_shake.h"

// TODO: This should not be declared in the header file, as it is only used externally (temp)
#include "dev.h"
#include "debug_draw.h"
#include "utilities/simple_collision_utility.h"


#include "multi_sword_attacks.h" // TODO: call only from boss
#include "fx/lightning_fx.h"
#include "systems/particle_system.h"
#include "fx/dust_particles_fx.h"
#include "fx/blood_particles_fx.h"
#include "fx/skill_fx.h"
#include "game/fallen_knight/fallen_knight.h"
#include "game/sun_boss/sun_boss.h"
#include "game/dragon_boss/dragon_boss.h"
#include "game/tower/tower.h"
#include "guardian_ending.h"
//#include "boulder_hazard.h" // close-range ground-boulder hazard

// Forward declaration: scene_init() and scene_restart() start the Guardian intro.
void scene_init_cutscene(void);

T3DModel* mapModel;
rspq_block_t* mapDpl;
T3DMat4FP* mapMatrix;

T3DModel* sunshaftsModel;
rspq_block_t* sunshaftsDpl;
T3DMat4FP* sunshaftsMatrix;

T3DModel* pillarsModel;
rspq_block_t* pillarsDpl;
T3DMat4FP* pillarsMatrix;

T3DModel* pillarsFrontModel;
rspq_block_t* pillarsFrontDpl;
T3DMat4FP* pillarsFrontMatrix;

T3DModel* chainsModel;
rspq_block_t* chainsDpl;
T3DMat4FP* chainsMatrix;

T3DModel* fogDoorModel;
rspq_block_t* fogDoorDpl;
T3DMat4FP* fogDoorMatrix;
ScrollParams fogScrollParams = {
    .xSpeed = 0.0f,
    .ySpeed = 10.0f,
    .scale  = 64
};

T3DModel* windowsModel;
rspq_block_t* windowsDpl;
T3DMat4FP* windowsMatrix;

T3DModel* roomLedgeModel;
rspq_block_t* roomLedgeDpl;
T3DMat4FP* roomLedgeMatrix;

T3DModel* roomFloorModel;
rspq_block_t* roomFloorDpl;
T3DMat4FP* roomFloorMatrix;

T3DModel* floorGlowModel;
rspq_block_t* floorGlowDpl;
T3DMat4FP* floorGlowMatrix;
ScrollParams floorGlowScrollParams = {
    .xSpeed = 0.0f,
    .ySpeed = 10.0f,
    .scale  = 64
};

//======== PHASE 2 ========

// static T3DModel* bossChainsModel;
// static rspq_block_t* bossChainsDpl;
// static T3DMat4FP* bossChainsMatrix;
// static T3DSkeleton* bossChainsSkeleton;
// static T3DAnim** bossChainsAnimations = NULL;
// static int currentBossChainsAnimation = 0;
// static bool bossChainsVisible = true;

T3DModel* bossChainsGlowModel;
rspq_block_t* bossChainsGlowDpl;
T3DMat4FP* bossChainsGlowMatrix;
ScrollParams bossChainsGlowScrollParams = {
    .xSpeed = 0.0f,
    .ySpeed = 20.0f,
    .scale  = 64
};

//==========================

// Guardian room vertical offset
static float roomY = -1.0f;

// Cutscene/screen transition state shared with Guardian cutscene context.
static bool screenTransition = false;

// Character yaw used when gameplay begins.
// The old title-facing yaw was +90 degrees; gameplay faces the opposite direction.
static const float GUARDIAN_CHARACTER_YAW = T3D_PI * 1.5f;


// ------------------------------------------------------------
// Video trigger AABB at world origin
// ------------------------------------------------------------
static float videoTrigMin[3] = { 502.7f, 0.0f,  -25.0f };
static float videoTrigMax[3] = { 552.7f, 120.0f, 25.0f };

static bool videoTrigFired = false;
static bool videoTrigHitThisFrame = false;
static bool videoPendingPlay = false;

static VideoPrerollState videoPreroll = VIDEO_PREROLL_NONE;
static float videoPrerollTimer = 0.0f;

// tweakable
static const float VIDEO_BLACK_HOLD_S = 0.5f;
static const float VIDEO_FADE_SPEED   = 200.0f; // same scale you already use
static bool bossDeathMusicFadeStarted = false;

// Post-boss interaction ("restored") state
static bool bossPostDefeatTalkDone = false;
static bool bossWasDead = false; // Tracks death transition for one-time post-death cleanup

// Per-slot save stats: track one "run" (boss attempt) start time, and record clear time at death transition.
static bool s_bossRunActive = false;
static double s_bossRunStartS = 0.0;
static double s_bossRunPlayS;       // the story's clear time: fighting only (no cutscenes, no pause menu)

// Post-boss interaction distances (XZ)
static const float POST_BOSS_PROMPT_DIST  = 140.0f;   // show A prompt and allow talk when inside this range

// ------------------------------------------------------------
// Cutscene music
// ------------------------------------------------------------
static bool s_pendingBossLoopMusic = false;
static const char *s_bossLoopMusicPath = "rom:/audio/music/boss_phase1-looping-22k.wav64";
static const char *s_boss2LoopMusicPath = "rom:/audio/music/boss_phase2_cutscene2-22k.wav64";   // Guardian Unbound
static const char *s_boss3LoopMusicPath = "rom:/audio/music/whispers_of_the_abyss-22k.wav64";    // Fallen Warrior
static BonusFight s_bonusFight = BONUS_NONE;   // set by the title Bonus menu
static bool  s_tower;             // set by the title Tower menu: the next scene_init climbs
static bool  s_towerDraft;        // choosing a boon between floors
static float s_towerDeadT;        // the floor's boss has lain dead this long
static bool  s_towerLastL, s_towerLastR, s_towerLastA;
static void scene_tower_begin_run(void);
static int   s_bonusBeat;                     // bonus intro/outro script step
static float s_bonusEndTimer;

// Require the character to be facing the boss for post-boss interaction.
// The third-person camera orbits independently of the character's body yaw,
// so checking the character's facing direction matches what the player sees:
// turning away with the left stick correctly hides the prompt and disables A.
static bool scene_character_facing_boss_xz(const Boss *boss, float minDot)
{
    if (!boss) return false;

    // Character forward (XZ) — matches character.c convention: (-sin(yaw), cos(yaw))
    float yaw = character.rot[1];
    float fwdX = -sinf(yaw);
    float fwdZ =  cosf(yaw);

    // Direction from character to boss (XZ)
    float toX = boss->pos[0] - character.pos[0];
    float toZ = boss->pos[2] - character.pos[2];
    float toLen = sqrtf(toX*toX + toZ*toZ);
    if (toLen < 0.001f) return true; // standing on top of it
    toX /= toLen;
    toZ /= toLen;

    float dot = fwdX * toX + fwdZ * toZ;
    return dot >= minDot;
}

// Within prompt distance (XZ). Controls visibility of the "A" prompt above the boss.
static bool scene_post_boss_in_range(const Boss *boss)
{
    if (!boss) return false;
    float dx = boss->pos[0] - character.pos[0];
    float dz = boss->pos[2] - character.pos[2];
    float d = sqrtf(dx*dx + dz*dz);
    return d <= POST_BOSS_PROMPT_DIST;
}

// In range AND facing the boss. Required for the A press to actually start the dialog.
static bool scene_post_boss_interact_allowed(const Boss *boss)
{
    if (!scene_post_boss_in_range(boss)) return false;
    return scene_character_facing_boss_xz(boss, 0.5f); // ~60° cone in front
}

// Boss title fade control (shown during intro, fades out when fight starts)
static float bossTitleFade = 0.0f;
static float bossTitleFadeSpeed = 1.8f;

// Victory title card ("Enemy restored") timing/state
static float victoryTitleTimer = 0.0f;
static bool victoryTitleDone = false;
static const float VICTORY_TITLE_FADEIN_S  = 0.75f;
static const float VICTORY_TITLE_HOLD_S    = 2.00f;
static const float VICTORY_TITLE_FADEOUT_S = 0.90f;

// progress for boss/player UIs
static float bossUiIntro = 1.0f;
static float playerUiIntro = 1.0f;
static float uiIntroSpeed = 1.5f;

// (cutsceneState, cutsceneTimer, cutsceneCameraTimer, cutsceneCamPosStart/End
// live in cutscene_manager.c — accessed via the internal header above.)
static Boss* g_boss = NULL;  // Boss instance pointer

// Game state management
static GameState gameState = GAME_STATE_PLAYING;
static bool lastMenuActive = false;

// Death screen restart lockout (prevents rapid A-mash from instantly restarting)
static float deathRestartLockoutTimer = 0.0f;
static const float DEATH_RESTART_LOCKOUT_S = 2.0f;

// Input state tracking
static bool lastAPressed = false;
static bool lastStartPressed = false;
static bool lastZPressed = false;
static bool lastLPressed = false;
static bool lastInteractAHeld = false;
static bool lastCLeftHeld = false;
static bool lastCRightHeld = false;

// Z-target cycling / tap-toggle behavior:
// - Tap Z toggles lock-on on/off
// - Hold Z keeps lock-on active and allows cycling targets with C-left/C-right
static float s_zHoldTimer = 0.0f;
static bool  s_zHoldConsumed = false;     // true if we used Z-hold for cycling this hold
static bool  s_zActivatedOnPress = false; // true if we turned lock-on ON on Z press
static const float Z_TAP_TOGGLE_MAX_S = 0.22f;

typedef enum {
    LOCK_TARGET_LOWERLEG_LEFT = 0,
    LOCK_TARGET_LOWERLEG_RIGHT,
    LOCK_TARGET_WAIST,
    LOCK_TARGET_COUNT
} LockTargetId;

// Current lock target selection (cycled via held-Z + C-left/C-right)
static int s_lockTargetIndex = LOCK_TARGET_WAIST;

// Victory title card background ("Enemy restored")
static sprite_t* victoryTitleBgSprite = NULL;
static surface_t victoryTitleBgSurf = {0};


_Static_assert(SCENE1_SFX_COUNT <= AUDIO_SCENE_MAX_SFX, "more sounds than audio_scene_load_paths loads: raise AUDIO_SCENE_MAX_SFX");
static const char *SCENE1_SFX_PATHS[SCENE1_SFX_COUNT] = {
    [SCENE1_SFX_TITLE_WALK]  = "rom:/audio/sfx/title_screen_walk_effect-22k.wav64",

    [SCENE1_SFX_BOSS_SWING1] = "rom:/audio/sfx/boss/boss_swing1_22k.wav64",
    [SCENE1_SFX_BOSS_SWING2] = "rom:/audio/sfx/boss/boss_swing2_22k.wav64",
    [SCENE1_SFX_BOSS_SWING3] = "rom:/audio/sfx/boss/boss_swing3_22k.wav64",
    [SCENE1_SFX_BOSS_SWING4] = "rom:/audio/sfx/boss/boss_swing4_22k.wav64",

    [SCENE1_SFX_BOSS_SMASH1] = "rom:/audio/sfx/boss/boss_smash1_22k.wav64",
    [SCENE1_SFX_BOSS_SMASH2] = "rom:/audio/sfx/boss/boss_smash2_22k.wav64",
    [SCENE1_SFX_BOSS_SMASH3] = "rom:/audio/sfx/boss/boss_smash3_22k.wav64",

    [SCENE1_SFX_BOSS_LUNGE]  = "rom:/audio/sfx/boss/boss_lunge_attack_22k.wav64",
    [SCENE1_SFX_BOSS_LAND1]  = "rom:/audio/sfx/boss/boss_land1_22k.wav64",
    [SCENE1_SFX_BOSS_LAND2]  = "rom:/audio/sfx/boss/boss_land2_22k.wav64",

    [SCENE1_SFX_BOSS_STEP1]  = "rom:/audio/sfx/boss/boss_step1_22k.wav64",
    [SCENE1_SFX_DRAGON_ROAR] = "rom:/audio/sfx/boss/dragon_roar_22k.wav64",
    [SCENE1_SFX_DRAGON_GROWL] = "rom:/audio/sfx/hostile/dragon_growl_22k.wav64",
    [SCENE1_SFX_SPELL_CAST]  = "rom:/audio/sfx/fx/spell_cast_22k.wav64",
    [SCENE1_SFX_BLADE_SLASH] = "rom:/audio/sfx/fx/halfmoon_slash_22k.wav64",    // Halfmoon Slash (tools/halfmoon_sfx.py)
    [SCENE1_SFX_HEAL]        = "rom:/audio/sfx/fx/heal_chime_22k.wav64",
    // the Bonus bosses' own, at the Guardian's level (tools/boss_sfx_levels.py)
    [SCENE1_SFX_FIRE]        = "rom:/audio/sfx/hostile/fire_burst_22k.wav64",
    [SCENE1_SFX_DARK_BEAM]   = "rom:/audio/sfx/hostile/dark_beam_22k.wav64",
    [SCENE1_SFX_DARK_NOVA]   = "rom:/audio/sfx/hostile/dark_nova_22k.wav64",
    [SCENE1_SFX_ROCK_BLAST]  = "rom:/audio/sfx/hostile/rock_blast_22k.wav64",
    [SCENE1_SFX_CHAIN1]      = "rom:/audio/sfx/hostile/chain1_22k.wav64",
    [SCENE1_SFX_CHAIN2]      = "rom:/audio/sfx/hostile/chain2_22k.wav64",
    [SCENE1_SFX_SPELL_WAVE_BOSS]  = "rom:/audio/sfx/hostile/spell_wave_22k.wav64",
    [SCENE1_SFX_SPELL_CAST_BOSS]  = "rom:/audio/sfx/hostile/spell_cast_22k.wav64",
    [SCENE1_SFX_BLADE_SLASH_BOSS] = "rom:/audio/sfx/hostile/halfmoon_slash_22k.wav64",
    [SCENE1_SFX_HOLY_BOSS]        = "rom:/audio/sfx/hostile/holy_charge_22k.wav64",    // tools/holy_slam_sfx.py
    [SCENE1_SFX_SLAM_BOSS]        = "rom:/audio/sfx/hostile/slam_impact_22k.wav64",
    [SCENE1_SFX_SWING_BOSS]       = "rom:/audio/sfx/hostile/char_swing1_22k.wav64",

    // Character SFX
    [SCENE1_SFX_CHAR_SWING1]         = "rom:/audio/sfx/character/char_swing1_22k.wav64",
    [SCENE1_SFX_CHAR_ATTACK_HIT1]    = "rom:/audio/sfx/character/char_attack_hit1_22k.wav64",
    [SCENE1_SFX_CHAR_ATTACK_HIT2]    = "rom:/audio/sfx/character/char_attack_hit2_22k.wav64",
    [SCENE1_SFX_CHAR_ATTACK_HIT3]    = "rom:/audio/sfx/character/char_attack_hit3_22k.wav64",
    [SCENE1_SFX_CHAR_ATTACK_HIT4]    = "rom:/audio/sfx/character/char_attack_hit4_22k.wav64",
    [SCENE1_SFX_CHAR_ATTACK_HIT5]    = "rom:/audio/sfx/character/char_attack_hit5_22k.wav64",
    [SCENE1_SFX_CHAR_ATTACK_HIT6]    = "rom:/audio/sfx/character/char_attack_hit6_22k.wav64",
    [SCENE1_SFX_CHAR_FOOTSTEP_RUN1]  = "rom:/audio/sfx/character/char_footstep_run1_22k.wav64",
    [SCENE1_SFX_CHAR_FOOTSTEP_RUN2]  = "rom:/audio/sfx/character/char_footstep_run2_22k.wav64",
    [SCENE1_SFX_CHAR_FOOTSTEP_RUN3]  = "rom:/audio/sfx/character/char_footstep_run3_22k.wav64",
    [SCENE1_SFX_CHAR_FOOTSTEP_RUN4]  = "rom:/audio/sfx/character/char_footstep_run4_22k.wav64",
    [SCENE1_SFX_CHAR_FOOTSTEP_WALK1] = "rom:/audio/sfx/character/char_footstep_walk1_22k.wav64",
    [SCENE1_SFX_CHAR_FOOTSTEP_WALK2] = "rom:/audio/sfx/character/char_footstep_walk2_22k.wav64",
    [SCENE1_SFX_CHAR_FOOTSTEP_WALK3] = "rom:/audio/sfx/character/char_footstep_walk3_22k.wav64",
    [SCENE1_SFX_CHAR_FOOTSTEP_WALK4] = "rom:/audio/sfx/character/char_footstep_walk4_22k.wav64",

    [SCENE1_SFX_CHAR_UMPH]           = "rom:/audio/sfx/character/umph_22k.wav64",
    [SCENE1_SFX_ROLL_SWISH] = "rom:/audio/sfx/character/roll_swish_22k.wav64",
    [SCENE1_SFX_ARMOR_CLANK] = "rom:/audio/sfx/character/armor_clank_22k.wav64",
    [SCENE1_SFX_WAR_CRY] = "rom:/audio/sfx/hostile/war_cry_22k.wav64",
    [SCENE1_SFX_FIRE_SUN]    = "rom:/audio/sfx/hostile/fire_sun_22k.wav64",
    [SCENE1_SFX_FIRE_DRAGON] = "rom:/audio/sfx/hostile/fire_dragon_22k.wav64",
    [SCENE1_SFX_BREATH]      = "rom:/audio/sfx/hostile/dragon_breath_22k.wav64",
    [SCENE1_SFX_WING_FLAP]   = "rom:/audio/sfx/hostile/wing_flap_22k.wav64",
    [SCENE1_SFX_WING_FLYBY]  = "rom:/audio/sfx/hostile/wing_flyby_22k.wav64",
    [SCENE1_SFX_MBD_X]       = "rom:/audio/sfx/fx/mbd_x_22k.wav64",
    [SCENE1_SFX_CLEAVE_BEAM] = "rom:/audio/sfx/fx/cleave_beam_22k.wav64",
    [SCENE1_SFX_FW_SLICE]    = "rom:/audio/sfx/hostile/fw_slice_22k.wav64",
    [SCENE1_SFX_FW_ROLL]     = "rom:/audio/sfx/hostile/fw_roll_22k.wav64",
    [SCENE1_SFX_FW_STEP1]    = "rom:/audio/sfx/hostile/fw_step1_22k.wav64",
    [SCENE1_SFX_FW_STEP2]    = "rom:/audio/sfx/hostile/fw_step2_22k.wav64",
    [SCENE1_SFX_FW_STEP3]    = "rom:/audio/sfx/hostile/fw_step3_22k.wav64",
    [SCENE1_SFX_FW_STEP4]    = "rom:/audio/sfx/hostile/fw_step4_22k.wav64",
    [SCENE1_SFX_FW_HURT]     = "rom:/audio/sfx/hostile/fw_hurt_22k.wav64",
    [SCENE1_SFX_SUN_CAST]    = "rom:/audio/sfx/hostile/sun_cast_22k.wav64",
    [SCENE1_SFX_SUN_SWELL]   = "rom:/audio/sfx/hostile/sun_swell_22k.wav64",
    [SCENE1_SFX_SUN_BURST]   = "rom:/audio/sfx/hostile/sun_burst_22k.wav64",
    [SCENE1_SFX_SUN_RUSH]    = "rom:/audio/sfx/hostile/sun_rush_22k.wav64",
    [SCENE1_SFX_SUN_ECLIPSE] = "rom:/audio/sfx/hostile/sun_eclipse_22k.wav64",
    [SCENE1_SFX_SUN_BREAK]   = "rom:/audio/sfx/hostile/sun_break_22k.wav64",
    [SCENE1_SFX_SUN_LASH]    = "rom:/audio/sfx/hostile/sun_lash_22k.wav64",
    [SCENE1_SFX_SUN_SWEEP]   = "rom:/audio/sfx/hostile/sun_sweep_22k.wav64",
    [SCENE1_SFX_SUN_RAIN]    = "rom:/audio/sfx/hostile/sun_rain_22k.wav64",
    [SCENE1_SFX_DRAGON_RISE] = "rom:/audio/sfx/hostile/dragon_rise_22k.wav64",
    [SCENE1_SFX_DRAGON_STOMP] = "rom:/audio/sfx/hostile/dragon_stomp_22k.wav64",
};

// The sounds heard all through a fight, held in memory (about 230 KB): the bosses' step, the
// knight's swing, its hits and running steps, and the smashes, swing and fire Shadowborne
// uses most (its fight was the one still short of 60 fps). The game runs in 4 MB
static const int s_heldSfx[] = {
    SCENE1_SFX_BOSS_STEP1, SCENE1_SFX_CHAR_SWING1,
    SCENE1_SFX_BOSS_SMASH1, SCENE1_SFX_BOSS_SMASH2, SCENE1_SFX_BOSS_SWING3,
    SCENE1_SFX_CHAR_ATTACK_HIT1, SCENE1_SFX_CHAR_ATTACK_HIT2, SCENE1_SFX_CHAR_ATTACK_HIT3,
    SCENE1_SFX_CHAR_ATTACK_HIT4, SCENE1_SFX_CHAR_ATTACK_HIT5, SCENE1_SFX_CHAR_ATTACK_HIT6,
    SCENE1_SFX_CHAR_FOOTSTEP_RUN1, SCENE1_SFX_CHAR_FOOTSTEP_RUN2, SCENE1_SFX_CHAR_FOOTSTEP_RUN3, SCENE1_SFX_CHAR_FOOTSTEP_RUN4,
};

// Loads the held sounds once, at boot (main): loading them as a fight started stalled it 220 ms
void scene_preload_sounds(void)
{
    const char *p[sizeof s_heldSfx / sizeof s_heldSfx[0]];
    for (unsigned k = 0; k < sizeof s_heldSfx / sizeof s_heldSfx[0]; k++) p[k] = SCENE1_SFX_PATHS[s_heldSfx[k]];
    audio_hold_paths(p, sizeof p / sizeof p[0]);
}

static void scene_begin_video_preroll(void)
{
    if (videoTrigFired) return;

    videoTrigFired = true;
    videoTrigHitThisFrame = true;

    // start fade-to-black using your existing priming mechanism
    startScreenFade = true;              // primes fadeBlackAlpha inside display_utility
    videoPreroll = VIDEO_PREROLL_FADING_TO_BLACK;
    videoPrerollTimer = 0.0f;
}

static void scene_update_video_trigger(void)
{
    videoTrigHitThisFrame = false;
    if (videoTrigFired) return;

    float capA[3], capB[3], r;
    collision_get_character_capsule_world(capA, capB, &r);

    if (scu_capsule_vs_rect_f(capA, capB, r, videoTrigMin, videoTrigMax)) {
        scene_begin_video_preroll();
    }
}

static void scene_update_video_preroll(void)
{
    if (videoPreroll == VIDEO_PREROLL_NONE) return;

    videoPrerollTimer += deltaTime;

    if (videoPreroll == VIDEO_PREROLL_FADING_TO_BLACK) {
        if (videoPrerollTimer >= 3.0f) {
            videoPreroll = VIDEO_PREROLL_BLACK_HOLD;
            videoPrerollTimer = 0.0f;
        }
        return;
    }

    // VIDEO_PREROLL_BLACK_HOLD
    if (videoPrerollTimer >= VIDEO_BLACK_HOLD_S) {

        // IMPORTANT: ensure audio controller stops touching CHANNEL_MUSIC
        // (and doesn't later mixer_ch_stop it mid-video)
        // The ending plays live in the hall (it used to be a video)
        videoPreroll = VIDEO_PREROLL_NONE;
        audio_set_music_volume(10);
        cutsceneState = CUTSCENE_ENDING;
        scene_init_cutscene();
    }
}

#ifdef HLE_LOG_FPS
static void log_model(const char *name, const T3DModel *m)   // test aid: what a model costs to draw
{
    T3DModelIter it = t3d_model_iter_create(m, T3D_CHUNK_TYPE_OBJECT);
    int objs = 0, tris = 0;
    int verts = 0;
    while (t3d_model_iter_next(&it)) { objs++; tris += it.object->triCount; for (int p = 0; p < it.object->numParts; p++) verts += it.object->parts[p].vertLoadCount; }
    debugf("[model] %s: %d objects, %d triangles, %d vertex loads, BVH %s\n", name, objs, tris, verts, t3d_model_bvh_get(m) ? "yes" : "no");
}
#endif
void scene_load_environment(void)
{
    // ===== LOAD MAP =====
    mapModel = t3d_model_load("rom:/boss_room/room.t3dm");
    gpu_log_model("mapModel", mapModel);
    gpu_block_begin();
        t3d_model_draw(mapModel);
    mapDpl = gpu_block_end();

    mapMatrix = malloc_uncached(sizeof(T3DMat4FP));
    t3d_mat4fp_from_srt_euler(
        mapMatrix,
        (float[3]){MODEL_SCALE, MODEL_SCALE, MODEL_SCALE},
        (float[3]){0.0f, 0.0f, 0.0f},
        (float[3]){0.0f, roomY, 0.0f}
    );

    // ===== LOAD PILLARS =====
    pillarsModel = t3d_model_load("rom:/boss_room/pillars.t3dm");
    gpu_log_model("pillarsModel", pillarsModel);
    gpu_block_begin();
        t3d_model_draw(pillarsModel);
    pillarsDpl = gpu_block_end();

    pillarsMatrix = malloc_uncached(sizeof(T3DMat4FP));
    t3d_mat4fp_from_srt_euler(
        pillarsMatrix,
        (float[3]){MODEL_SCALE, MODEL_SCALE, MODEL_SCALE},
        (float[3]){0.0f, 0.0f, 0.0f},
        (float[3]){0.0f, roomY, 0.0f}
    );

    pillarsFrontModel = t3d_model_load("rom:/boss_room/pillars_front.t3dm");
    gpu_log_model("pillarsFrontModel", pillarsFrontModel);
    gpu_block_begin();
        t3d_model_draw(pillarsFrontModel);
    pillarsFrontDpl = gpu_block_end();

    pillarsFrontMatrix = malloc_uncached(sizeof(T3DMat4FP));
    t3d_mat4fp_from_srt_euler(
        pillarsFrontMatrix,
        (float[3]){MODEL_SCALE, MODEL_SCALE, MODEL_SCALE},
        (float[3]){0.0f, 0.0f, 0.0f},
        (float[3]){0.0f, roomY, 0.0f}
    );

    // ===== LOAD LEDGE =====
    roomLedgeModel = t3d_model_load("rom:/boss_room/room_ledge_walls.t3dm");
    gpu_log_model("roomLedgeModel", roomLedgeModel);
    gpu_block_begin();
        t3d_model_draw(roomLedgeModel);
    roomLedgeDpl = gpu_block_end();

    roomLedgeMatrix = malloc_uncached(sizeof(T3DMat4FP));
    t3d_mat4fp_from_srt_euler(
        roomLedgeMatrix,
        (float[3]){MODEL_SCALE, MODEL_SCALE, MODEL_SCALE},
        (float[3]){0.0f, 0.0f, 0.0f},
        (float[3]){0.0f, roomY, 0.0f}
    );

    // ===== LOAD WINDOWS =====
    windowsModel = t3d_model_load("rom:/boss_room/windows.t3dm");
    gpu_log_model("windowsModel", windowsModel);
    gpu_block_begin();
        t3d_model_draw(windowsModel);
    windowsDpl = gpu_block_end();

    windowsMatrix = malloc_uncached(sizeof(T3DMat4FP));
    t3d_mat4fp_from_srt_euler(
        windowsMatrix,
        (float[3]){MODEL_SCALE, MODEL_SCALE, MODEL_SCALE},
        (float[3]){0.0f, 0.0f, 0.0f},
        (float[3]){0.0f, roomY, 0.0f}
    );

#ifdef HLE_LOG_FPS
    log_model("room", mapModel); log_model("pillars", pillarsModel); log_model("pillars_front", pillarsFrontModel);
    log_model("ledge", roomLedgeModel); log_model("windows", windowsModel);
#endif
    // ===== LOAD PERSISTENT CEILING CHAINS =====
    chainsModel = t3d_model_load("rom:/boss_room/ceiling_chains.t3dm");
    gpu_log_model("chainsModel", chainsModel);
    gpu_block_begin();
        t3d_model_draw(chainsModel);
    chainsDpl = gpu_block_end();

    chainsMatrix = malloc_uncached(sizeof(T3DMat4FP));
    t3d_mat4fp_from_srt_euler(
        chainsMatrix,
        (float[3]){MODEL_SCALE, MODEL_SCALE, MODEL_SCALE},
        (float[3]){0.0f, 0.0f, 0.0f},
        (float[3]){0.0f, roomY, 0.0f}
    );

    // ===== LOAD SUN SHAFTS =====
    sunshaftsModel = t3d_model_load("rom:/boss_room/sunshafts.t3dm");
    gpu_log_model("sunshaftsModel", sunshaftsModel);
    gpu_block_begin();
        t3d_model_draw(sunshaftsModel);
    sunshaftsDpl = gpu_block_end();

    sunshaftsMatrix = malloc_uncached(sizeof(T3DMat4FP));
    t3d_mat4fp_from_srt_euler(
        sunshaftsMatrix,
        (float[3]){MODEL_SCALE, MODEL_SCALE, MODEL_SCALE},
        (float[3]){0.0f, 0.0f, 0.0f},
        (float[3]){0.0f, roomY, 0.0f}
    );

    // ===== LOAD FOG DOOR =====
    fogDoorModel = t3d_model_load("rom:/boss_room/fog.t3dm");
    gpu_log_model("fogDoorModel", fogDoorModel);
    gpu_block_begin();
        t3d_model_draw(fogDoorModel);
    fogDoorDpl = gpu_block_end();

    fogDoorMatrix = malloc_uncached(sizeof(T3DMat4FP));
    t3d_mat4fp_from_srt_euler(
        fogDoorMatrix,
        (float[3]){MODEL_SCALE, MODEL_SCALE, MODEL_SCALE},
        (float[3]){0.0f, 0.0f, 0.0f},
        (float[3]){0.0f, roomY, 0.0f}
    );

    // ===== LOAD FLOOR GLOW =====
    floorGlowModel = t3d_model_load("rom:/boss_room/floor_glow.t3dm");
    gpu_log_model("floorGlowModel", floorGlowModel);
    gpu_block_begin();
        t3d_model_draw(floorGlowModel);
    floorGlowDpl = gpu_block_end();

    floorGlowMatrix = malloc_uncached(sizeof(T3DMat4FP));
    t3d_mat4fp_from_srt_euler(
        floorGlowMatrix,
        (float[3]){MODEL_SCALE, MODEL_SCALE, MODEL_SCALE},
        (float[3]){0.0f, 0.0f, 0.0f},
        (float[3]){0.0f, roomY, 0.0f}
    );

    // ===== LOAD FLOOR =====
    roomFloorModel = t3d_model_load("rom:/boss_room/floor.t3dm");
    gpu_log_model("roomFloorModel", roomFloorModel);
    gpu_block_begin();
        t3d_model_draw(roomFloorModel);
    roomFloorDpl = gpu_block_end();
#ifdef HLE_LOG_FPS
    log_model("floor", roomFloorModel);
    { extern T3DModel* characterModel; log_model("knight", characterModel); }
#endif

    roomFloorMatrix = malloc_uncached(sizeof(T3DMat4FP));
    t3d_mat4fp_from_srt_euler(
        roomFloorMatrix,
        (float[3]){MODEL_SCALE, MODEL_SCALE, MODEL_SCALE},
        (float[3]){0.0f, 0.0f, 0.0f},
        (float[3]){0.0f, roomY, 0.0f}
    );

    // ===== LOAD BOSS CHAINS GLOW =====
    // Keep this here only if it is visible after the phase-2 cutscene / during gameplay.
    // If it is phase-2-cutscene-only, move it into cutscene_guardian_phase2.c instead.
    bossChainsGlowModel = t3d_model_load("rom:/boss/boss_chain_glow.t3dm");
    gpu_log_model("bossChainsGlowModel", bossChainsGlowModel);
    gpu_block_begin();
        t3d_model_draw(bossChainsGlowModel);
    bossChainsGlowDpl = gpu_block_end();

    bossChainsGlowMatrix = malloc_uncached(sizeof(T3DMat4FP));
    t3d_mat4fp_from_srt_euler(
        bossChainsGlowMatrix,
        (float[3]){MODEL_SCALE, MODEL_SCALE, MODEL_SCALE},
        (float[3]){0.0f, 0.0f, 0.0f},
        (float[3]){0.0f, roomY, 0.0f}
    );

    // Global/system-level effect. Keep loaded if it is used outside one cutscene,
    // or move it later if it is phase-2-only.
    lightning_fx_system_init("rom:/boss/boss_back_sword_lightning2.t3dm");
}

void scene_init(void)
{
    joypad_rumble_stop();

    // ==== Sounds ====

    audio_scene_load_paths(SCENE1_SFX_PATHS, SCENE1_SFX_COUNT);
    audio_scene_hold_sfx(s_heldSfx, sizeof s_heldSfx / sizeof s_heldSfx[0]);

    // ==== Camera ====

    cameraState = CAMERA_CUSTOM;
    lastCameraState = CAMERA_CUSTOM;

    // ==== Lighting ====
    game_lighting_initialize();
    colorAmbient[2] = 0xFF;
    colorAmbient[1] = 0xFF;
    colorAmbient[0] = 0xFF;
    colorAmbient[3] = 255;

    // Currently not using dir lights so ignore
    // colorDir[2] = 0xFF;
    // colorDir[1] = 0xFF;
    // colorDir[0] = 0xFF;
    // colorDir[3] = 0xFF;
    // lightDirVec = (T3DVec3){{-0.9833f, 0.1790f, -0.0318f}};
    // t3d_vec3_norm(&lightDirVec);

    scene_load_environment();

    g_boss = boss_spawn();
    if (!g_boss) {
        // Handle error
        return;
    }

    // The scene owns when the boss is commanded into/out of gameplay,
    // but the boss owns the actual active/cinematic/combat mode internally.
    boss_deactivate(g_boss);

    boss_ground_crush_init();

    // reset character
    character_reset();
    character_reset_button_state();

    // Initialize dialog controller
    dialog_controller_init();

    button_prompt_init();

    // Load victory title background (used for "Enemy restored")
    victoryTitleBgSprite = sprite_load("rom:/dialog-gradient.ia8.sprite");
    if (victoryTitleBgSprite) {
        victoryTitleBgSurf = sprite_get_pixels(victoryTitleBgSprite);
    }

    particle_system_init();
    dust_particles_fx_init();
    blood_particles_fx_init();
    skill_fx_init();

    character_ui_init();
    boss_ui_init();

    // Initialize and show letterbox bars for intro
    letterbox_init();
    letterbox_show(false);  // Show immediately without animation

    collision_init();


    dust_particles_fx_reset();
    blood_particles_fx_reset();

    msa_init();

    // A new run from the title: clear per-run state left by the previous one (phase
    // triggers, end-portal trigger, ...). Without this, a second run in the same
    // session skipped phase 2 and the end portal never fired.
    scene_reset();

    // Guardian scene normal startup: entering the scene starts the phase-1 intro
    // (or a Bonus fight's intro).
    gameState = GAME_STATE_PLAYING;
    if (s_tower) {
        audio_stop_all_sfx();
        scene_tower_begin_run();
        return;
    }
    cutsceneState = s_bonusFight ? CUTSCENE_BONUS_INTRO : CUTSCENE_PHASE1_INTRO;
    cutsceneTimer = 0.0f;
    cutsceneCameraTimer = 0.0f;
    scene_init_cutscene();
    audio_stop_all_sfx();

    // DEBUG: uncomment to start the fight in phase 2
    // if (g_boss) g_boss->phaseIndex = 2;
    // phase2CutsceneTriggered = true;
}


static inline int scene_lockon_bone_index_for_target(LockTargetId target)
{
    if (!g_boss) return -1;
    switch (target) {
        case LOCK_TARGET_LOWERLEG_LEFT:  return g_boss->lowerLegLeftBoneIndex;
        case LOCK_TARGET_LOWERLEG_RIGHT: return g_boss->lowerLegRightBoneIndex;
        case LOCK_TARGET_WAIST:          return g_boss->waistBoneIndex;
        default:                         return -1;
    }
}

static inline bool scene_lockon_target_available(LockTargetId target)
{
    return scene_lockon_bone_index_for_target(target) >= 0;
}

static void scene_cycle_lock_target(int dir)
{
    if (LOCK_TARGET_COUNT <= 0) return;
    if (dir == 0) return;

    // Try to skip unavailable bones, but always terminate (max LOCK_TARGET_COUNT iterations).
    int idx = s_lockTargetIndex;
    for (int i = 0; i < LOCK_TARGET_COUNT; i++) {
        idx = (idx + dir) % LOCK_TARGET_COUNT;
        if (idx < 0) idx += LOCK_TARGET_COUNT;
        if (scene_lockon_target_available((LockTargetId)idx)) {
            s_lockTargetIndex = idx;
            return;
        }
    }
    // If nothing is available, keep current selection.
}

// Lock-on focus point for targeting.
static T3DVec3 get_boss_lock_focus_point(void)
{
    if (!g_boss) {
        return (T3DVec3){{0.0f, 0.0f, 0.0f}};
    }

    // 1) Selected target (if available)
    T3DVec3 worldPos;
    int bone = scene_lockon_bone_index_for_target((LockTargetId)s_lockTargetIndex);
    if (boss_get_bone_world_pos(g_boss, bone, &worldPos)) {
        return worldPos;
    }

    // Ultimate fallback: boss position with a small lift so the marker is not at the feet.
    return boss_get_fallback_lock_focus_point(g_boss);
}

void scene_reset(void)
{
    // Runtime state reset (no allocations / no frees)
    cutsceneState = CUTSCENE_NONE;
    cutsceneTimer = 0.0f;
    cutsceneCameraTimer = 0.0f;
    skipButtonVisible = false;
    lastCutsceneAPressed = false;
    // Note: skipButtonVisible is also used for title transition, so we reset it here
    if (g_boss) {
        boss_deactivate(g_boss);
    }
    phase2CutsceneTriggered = false;
    bossWasDead = false;
    boss_ui_set_form(1);
    s_bonusEndTimer = 0.0f;
    fallen_knight_free();
    sun_boss_free();
    dragon_boss_free();
    gameState = GAME_STATE_PLAYING;
    lastMenuActive = false;
    lastAPressed = false;
    lastStartPressed = false;
    lastZPressed = false;
    cameraLockOnActive = false;
    lastCLeftHeld = false;
    lastCRightHeld = false;
    s_zHoldTimer = 0.0f;
    s_zHoldConsumed = false;
    s_zActivatedOnPress = false;
    s_lockTargetIndex = LOCK_TARGET_WAIST;
    videoTrigFired = false;
    videoPendingPlay = false;

    videoPreroll = VIDEO_PREROLL_NONE;
    videoPrerollTimer = 0.0f;
    bossDeathMusicFadeStarted = false;
    videoTrigFired = false;
    videoPendingPlay = false;
    // Cutscene fade/transition state
    screenTransition = false;
    // UI state
    bossTitleFade = 0.0f;
    bossUiIntro = 1.0f;
    playerUiIntro = 1.0f;
    boss_ui_reset();
    character_ui_reset();
    boss_ui_set_intro(bossUiIntro);
    character_ui_set_intro(playerUiIntro);

    // Victory end-card state
    victoryTitleTimer = 0.0f;
    victoryTitleDone = false;

    // Death end-card state
    deathRestartLockoutTimer = 0.0f;

    // Post-boss interaction state
    bossPostDefeatTalkDone = false;
    bossPostDefeatDialogStep = 0;

    s_pendingBossLoopMusic = false;

    // Clear cutscene/attack effects that latch globally. Without these, a
    // mid-phase-2 death leaks state into the new run: leftover MSA swords keep
    // falling during phase 1, the BNW lightning ring stays armed, and the
    // shackled-sun screen shake never clears.
    msa_init();
    lightning_fx_system_ring_enable(false);
    screen_shake_set_shake_mag(0.0f);

    // Reset letterbox to show state for intro
    letterbox_show(false);

    // Reset run timer state (a new run will be started when we enter gameplay again)
    s_bossRunActive = false;
    s_bossRunStartS = 0.0;

    dust_particles_fx_reset();
    blood_particles_fx_reset();
    skill_fx_reset();
    boss_ground_crush_reset();
}

static void scene_sync_input_edge_state(void)
{
    // Sync last-pressed to the current button state so held buttons don't cause "just pressed"
    // events immediately after restart.
    lastAPressed = btn.a;
    lastStartPressed = btn.start;
    // Use held state for Z so our lock-on toggle edge detection is reliable.
    lastZPressed = joypad.btn.z;
    // Use held-state for L since btn.* is "pressed this frame" in libdragon.
    lastLPressed = joypad.btn.l;
    // Use held state for interact-A edge detection (prevents missing presses).
    lastInteractAHeld = joypad.btn.a;
    lastCLeftHeld = joypad.btn.c_left;
    lastCRightHeld = joypad.btn.c_right;
    s_zHoldTimer = 0.0f;
    s_zHoldConsumed = false;
    s_zActivatedOnPress = false;
    lastCutsceneAPressed = btn.a;
}

static inline float scene_dist_xz(float ax, float az, float bx, float bz) {
    float dx = ax - bx;
    float dz = az - bz;
    return sqrtf(dx*dx + dz*dz);
}

bool scene_is_cutscene_active(void) {
    return cutsceneState != CUTSCENE_NONE;
}

bool scene_is_boss_active(void) {
    return boss_is_active(g_boss);
}

GameState scene_get_game_state(void) {
    return gameState;
}

void scene_set_game_state(GameState state) {
    if (state == gameState) return;

    GameState prev = gameState;
    gameState = state;

    // Death: lock out restart input for a short duration so late gameplay inputs don't
    // instantly skip the death screen straight back to title.
    if (state == GAME_STATE_DEAD && prev != GAME_STATE_DEAD) {
        deathRestartLockoutTimer = 0.0f;
    } else if (prev == GAME_STATE_DEAD && state != GAME_STATE_DEAD) {
        deathRestartLockoutTimer = 0.0f;
    }

    // Reset the victory title card whenever we enter or leave victory
    if (state == GAME_STATE_VICTORY && prev != GAME_STATE_VICTORY) {
        victoryTitleTimer = 0.0f;
        victoryTitleDone = false;
    } else if (prev == GAME_STATE_VICTORY && state != GAME_STATE_VICTORY) {
        victoryTitleTimer = 0.0f;
        victoryTitleDone = false;
    }
}

bool scene_is_menu_active(void) {
    return gameState == GAME_STATE_MENU;
}

void scene_restart(void)
{
    debugf("RESTART: Starting Guardian scene restart sequence\n");

    // 1) Stop running systems first (prevents update-on-freed state)
    audio_stop_all_sfx();
    audio_stop_music();
    dialog_controller_reset();

    // 2) Reset input edge-tracking (prevents phantom presses)
    character_reset_button_state();

    // 3) Reset gameplay entities (logic state)
    if (g_boss) boss_reset(g_boss);
    character_reset();

    // 4) Reset camera / lock-on and Guardian runtime flags
    camera_reset();
    camera_mode(CAMERA_CUSTOM);
    scene_reset();

    // 5) Re-enter Guardian intro runtime state (NO allocations / NO frees)
    gameState = GAME_STATE_PLAYING;
    if (s_tower) {     // a Tower run restarts from floor 1
        scene_tower_begin_run();
        scene_sync_input_edge_state();
        return;
    }
    cutsceneState = s_bonusFight ? CUTSCENE_BONUS_INTRO : CUTSCENE_PHASE1_INTRO;
    cutsceneTimer = 0.0f;
    cutsceneCameraTimer = 0.0f;
    scene_init_cutscene();
    audio_stop_all_sfx();

    scene_sync_input_edge_state();

    debugf("RESTART: Guardian soft reset complete. cameraState=%d speaking=%s\n",
           cameraState, dialog_controller_speaking() ? "true" : "false");
}

static void scene_place_character(void)
{
    character.pos[0] = -320.43f;
    character.pos[1] = 4.0f;
    character.pos[2] = 0.0f;

    character.scale[0] = MODEL_SCALE * 0.5f;
    character.scale[1] = MODEL_SCALE * 0.5f;
    character.scale[2] = MODEL_SCALE * 0.5f;

    // Face towards boss
    character.rot[1] = GUARDIAN_CHARACTER_YAW;

    character_update_position();
    character_set_state(CHAR_STATE_NORMAL);
    character_set_velocity_xz(0.0f, 0.0f);
}

void scene_init_playing(bool skippedCutscene)
{
    game_time_clear_effects();
    scene_place_character();

    // Skip dialog and cutscene
    dialog_controller_stop_speaking();

    cutsceneState = CUTSCENE_NONE;
    cutsceneTimer = 0.0f;
    cutsceneCameraTimer = 0.0f;
    cutsceneDialogActive = false;
    skipButtonVisible = false;
    lastCutsceneAPressed = false;

    if (g_boss) {
        boss_activate_combat(g_boss);
    }

    // Starting a new run (boss attempt).
    // Default save slot is 0 ("Save 1") until we have a UI to pick slots.
    (void)save_controller_increment_run_count();
    s_bossRunActive = true;
    s_bossRunStartS = nowS;
    s_bossRunPlayS = 0.0;

    // ---- Music handoff behavior ----
    // If the cutscene was skipped, slam immediately into looping music.
    // If not skipped, let the current (non-looping) cutscene track finish naturally,
    // then start the loop once it's done.
    if (skippedCutscene) {
        s_pendingBossLoopMusic = false;  // we are starting it now
        audio_stop_music();
        audio_play_music(s_bossLoopMusicPath, true);
    } else {
        // Do NOT stop the current music. Just arm the handoff.
        s_pendingBossLoopMusic = true;
    }

    // Hide letterbox bars with animation
    letterbox_hide();

    // Return camera control to the player
    camera_mode_smooth(CAMERA_CHARACTER, 1.0f);
    cameraLockOnActive = true;

    // Start boss title fully visible so it slides up on gameplay start
    bossTitleFade = 1.0f;

    // Reset UI intro animations (they will slide/fade into view)
    bossUiIntro = 0.0f;
    playerUiIntro = 0.0f;
    boss_ui_set_intro(bossUiIntro);
    character_ui_set_intro(playerUiIntro);

#if DEBUG_BOULDER_ATTACK_FIRST
    // Debug: immediately open phase 1 with the boulder attack so the hazard
    // can be observed without waiting for the AI to naturally select it.
    if (g_boss) {
        g_boss->state                    = BOSS_STATE_ATTACK1;
        g_boss->stateTimer               = 0.0f;
        g_boss->attack1Cooldown          = 6.0f;
        g_boss->attackCooldown           = 1.0f;
        g_boss->isAttacking              = true;
        g_boss->attackAnimTimer          = 0.0f;
        g_boss->animationTransitionTimer = 0.0f;
        g_boss->currentAttackHasHit      = false;
        g_boss->velX                     = 0.0f;
        g_boss->velZ                     = 0.0f;
        g_boss->currentAttackName        = "Attack1";
        g_boss->attackNameDisplayTimer   = 2.0f;
        g_boss->currentAttackId          = BOSS_ATTACK_ATTACK1;
    }
#endif
}

void scene_dev_warp_to_fight(void)
{
    gameState = GAME_STATE_PLAYING;
    cutscene_manager_reset();
    scene_init_playing(true);
}

void scene_dev_warp_to_pre_phase2(void)
{
    scene_dev_warp_to_fight();
    // Drop boss to just above the phase-2 trigger (40% of maxHealth) so the next
    // hit kicks off the phase 2 cutscene chain.
    if (g_boss) {
        g_boss->health = g_boss->maxHealth * 0.42f;
    }
}

// End of phase2 cutscene teardown
static void scene_finish_phase2_cutscene(void)
{
    lightning_fx_system_ring_enable(false);
    screen_shake_set_shake_mag(0.0f);
    joypad_rumble_stop();
    dialog_controller_stop_speaking();
    cutsceneDialogActive = false;

    if (g_boss) {
        boss_activate_combat(g_boss);

        g_boss->phaseIndex = 2;
        g_boss->name = "Guardian Unbound";
        g_boss->health = g_boss->maxHealth;
        g_boss->handAttackColliderActive = false;
        g_boss->sphereAttackColliderActive = false;
        g_boss->velX = 0.0f;
        g_boss->velZ = 0.0f;

        // Phase 2 opens with the aerial sword barrage.
        boss_ai_start_aerial_sword_barrage(g_boss);
    }

    // Phase 2 cutscene music was started non-looping; arm the boss loop to take over.
    s_pendingBossLoopMusic = true;

    // Long camera blend so the cinematic angle hold
    camera_mode_smooth(CAMERA_CHARACTER, 5.0f);
    cameraLockOnActive = true;

    cutsceneTimer = 0.0f;
    cutsceneCameraTimer = 0.0f;
    cutsceneState = CUTSCENE_NONE;
    skipButtonVisible = false;

    // A new boss: replay the name title and grow a fresh health bar
    boss_ui_set_form(2);
    bossTitleFade = 1.0f;
    bossUiIntro = 0.0f;
    boss_ui_set_intro(0.0f);
    boss_ui_snap_health_trail(1.0f);

    character_reset_button_state();
    scene_sync_input_edge_state();
}

// Temporary scene context until memory management is added and cutscenes become their own scenes.
static SceneContext sceneContext;
static void scene_update_context(void)
{
    sceneContext.boss = g_boss;

    sceneContext.roomY = roomY;

    sceneContext.gameState = &gameState;
    sceneContext.screenTransition = &screenTransition;

    sceneContext.windowsModel = windowsModel;
    sceneContext.windowsDpl = windowsDpl;
    sceneContext.windowsMatrix = windowsMatrix;

    sceneContext.mapModel = mapModel;
    sceneContext.mapDpl = mapDpl;
    sceneContext.mapMatrix = mapMatrix;

    sceneContext.roomFloorModel = roomFloorModel;
    sceneContext.roomFloorDpl = roomFloorDpl;
    sceneContext.roomFloorMatrix = roomFloorMatrix;

    sceneContext.roomLedgeModel = roomLedgeModel;
    sceneContext.roomLedgeDpl = roomLedgeDpl;
    sceneContext.roomLedgeMatrix = roomLedgeMatrix;

    sceneContext.pillarsModel = pillarsModel;
    sceneContext.pillarsDpl = pillarsDpl;
    sceneContext.pillarsMatrix = pillarsMatrix;

    sceneContext.pillarsFrontModel = pillarsFrontModel;
    sceneContext.pillarsFrontDpl = pillarsFrontDpl;
    sceneContext.pillarsFrontMatrix = pillarsFrontMatrix;

    sceneContext.sunshaftsModel = sunshaftsModel;
    sceneContext.sunshaftsDpl = sunshaftsDpl;
    sceneContext.sunshaftsMatrix = sunshaftsMatrix;

    sceneContext.chainsModel = chainsModel;
    sceneContext.chainsDpl = chainsDpl;
    sceneContext.chainsMatrix = chainsMatrix;

    sceneContext.bossChainsGlowModel = bossChainsGlowModel;
    sceneContext.bossChainsGlowDpl = bossChainsGlowDpl;
    sceneContext.bossChainsGlowMatrix = bossChainsGlowMatrix;
    sceneContext.bossChainsGlowScrollParams = &bossChainsGlowScrollParams;

    sceneContext.init_playing = scene_init_playing;
    sceneContext.finish_phase2_cutscene = scene_finish_phase2_cutscene;
}

// ---- Title-menu Bonus fights ----

void scene_set_bonus_fight(BonusFight fight) { s_bonusFight = fight; }

#define BONUS_SPAWN_X (-160.0f)         // the boss comes down between the player and the hall
#define BONUS_GATHER_S 3.5f             // the light gathers this long before the descent
#define BONUS_LAND_S   2.0f             // the descent plays this long before the next line
#define BONUS_HOLD_S   1.4f             // a line stays this long once typed

// An intro shot relative to the spawn point (the player stands at x -160): the camera
// drifts from p0 to p1 looking at tg until the next cut. p0[1] == 0: no cut, hold the last.
typedef struct { float p0[3], p1[3], tg[3]; } BonusShot;

// Each Bonus fight: its intro lines (the boss comes down before line `dropAt`), its
// outro lines, and the boss it turns g_boss into. Lines follow each other as soon as
// the previous one has been typed and held, so the timing follows the text. Each line
// cuts to its own shot (lines before the drop only until the light gathers), with the
// gather and descent shots in between.
typedef struct {
    const char *intro[6], *outro[4];
    int dropAt;
    BonusShot line[6], gather, drop;
    void (*begin)(Boss*), (*skip)(Boss*);
    int phase;
    const char *music;
    float camScale;     // the outro camera stands this much further off (0: 1, a boss of human size)
} BonusDef;

static const BonusDef BONUS_DEFS[] = {
    [BONUS_FALLEN_KNIGHT] = {
        .intro = {
            "^Another knight...~\n^another oath,~ ^sworn\nto a dying sun.",
            "^I knelt where you stand.~\n^I spoke the words\nyou spoke.",
            "^I wore this steel~\n^until it wore me.",
            ">Rise, then...~ ^and learn\nwhat your oath is worth!",
        },
        .outro = {
            "^Ah...~ ^so this is how\nthe oath ends.",
            "^Not in glory...~\n<but on my knees.",
            "^Carry it, knight...~\n<better than I did.",
        },
        .dropAt = 3,
        .line = {
            { { -70, 20, -60 }, { -95, 18, 45 }, { -160, 14, 0 } },      // watched from where it will land
            { { -138, 4, 22 }, { -136, 14, 24 }, { -160, 8, 0 } },       // up from the floor "where you stand"
            { { -170, 16, -70 }, { -160, 16, -62 }, { -135, 14, 0 } },   // the knight's profile, the empty hall ahead
            { { -58, 18, 16 }, { -34, 17, 8 }, { 0, 20, 0 } },           // pushing in on it: the challenge
        },
        .gather = { { -50, 6, 40 }, { -55, 22, 48 }, { 0, 70, 0 } },
        .drop   = { { -42, 4, 18 }, { -36, 6, 12 }, { 0, 22, 0 } },      // low, looking up as it lands
        .begin = fallen_knight_begin, .skip = fallen_knight_skip_intro, .phase = FALLEN_KNIGHT_PHASE,
        .music = "rom:/audio/music/whispers_of_the_abyss-22k.wav64",  // "Whispers of the Abyss" by AlkaKrab
    },
    [BONUS_SHACKLED_SUN] = {
        .intro = {
            "^Little knight...~\n^you have walked beneath\nme all your life.",
            "^I warmed your fields.~\n^I gilded your king's\nthrone.",
            "^And for my light~\n^he forged these chains.",
            "^Now my Guardian lies\nbroken~ ^at your feet...",
            ">So look upon me,~\n^thief of dawns...~ <and\nBURN.",
        },
        .outro = {
            "^You think this...~\n^is victory?",
            "^Every dawn you see~\n^will be my corpse,\n<rising.",
            ">Freeze in the dark,~\n^knight...~ <without me.",
        },
        .dropAt = 4,
        .line = {
            { { -120, 170, 30 }, { -140, 140, 16 }, { -160, 4, 0 } },    // from above: "walked beneath me"
            { { -100, 14, 30 }, { -125, 16, 20 }, { -160, 14, 0 } },     // closing on the knight, the lit hall behind
            { { -200, 10, 30 }, { -190, 10, 34 }, { -120, 160, 0 } },    // up into the dark where the chains hang
            { { -135, 4, 20 }, { -138, 4, 14 }, { -160, 4, 0 } },        // down at the knight's feet
            { { -80, 26, 4 }, { -46, 24, 2 }, { 0, 24, 0 } },            // pushing in on its face: BURN
        },
        .gather = { { -200, 6, -10 }, { -195, 4, -6 }, { 0, 90, 0 } },     // behind the knight, looking up
        .drop   = { { -100, 20, -70 }, { -90, 22, -60 }, { 0, 40, 0 } },
        .begin = sun_boss_begin, .skip = sun_boss_skip_intro, .phase = SUN_BOSS_PHASE,
        .music = "rom:/audio/music/veil_of_eternal_nightfall-22k.wav64",    // "Veil of Eternal Nightfall" by AlkaKrab
    },
    [BONUS_SHADOWBORN] = {
        .intro = {
            "^Before your king,~ ^before\nhis Sun...~ ^there was the\ndark.",
            "^I was set to guard it.~\n^I have been eating it\never since.",
            "^It eats me back.~\n<It is winning.",
            ">Stand still, little one...~\n<let me feed.",
        },
        .outro = {
            "^So... warm...~\n^I had forgotten.",
            "^Leave me the dark,~\n^knight.~ <It is all\nI ever was.",
        },
        .dropAt = 3,
        .line = {
            { { 240, 50, -80 }, { 180, 44, -60 }, { -160, 10, 0 } },     // from the far dark, the knight small
            { { -110, 12, -90 }, { -110, 14, 90 }, { -180, 12, 0 } },    // tracking across the knight
            { { -200, 22, -16 }, { -194, 20, -12 }, { 0, 30, 0 } },      // over the knight's shoulder, the hall empty
            { { -150, 10, 50 }, { -125, 14, 36 }, { 0, 70, 0 } },        // low, closing on its head: "let me feed"
        },
        .gather = { { -135, 4, 30 }, { -140, 5, 34 }, { -160, 30, 0 } },  // low on the knight, the dark above
        .drop   = { { -40, 30, -200 }, { -60, 40, -170 }, { 0, 60, 0 } },    // side-on, wide for its wings
        .begin = dragon_boss_begin, .skip = dragon_boss_skip_intro, .phase = DRAGON_BOSS_PHASE,
        .music = "rom:/audio/music/shadowforge_convergence-22k.wav64",    // "Shadowforge Convergence" by AlkaKrab
        .camScale = 2.6f,
    },
};

// The current script's schedule: when each line starts, the descent, and the end
static float s_bonusLineT[6], s_bonusDropT, s_bonusEndT;
static int s_bonusLine;

static void scene_bonus_schedule(const char *const *lines, int max, int dropAt, float t)
{
    s_bonusDropT = -1.0f;
    for (int i = 0; i < max && lines[i]; i++) {
        if (i == dropAt) { s_bonusDropT = t; t += BONUS_LAND_S; }
        s_bonusLineT[i] = t;
        t += dialog_controller_type_time(lines[i]) + BONUS_HOLD_S;
    }
    s_bonusEndT = t;
    s_bonusLine = 0;
}

static void scene_bonus_speak(const char *const *lines, int max, float t)
{
    if (s_bonusLine < max && lines[s_bonusLine] && t >= s_bonusLineT[s_bonusLine]) {
        cutscene_manager_begin_dialog(lines[s_bonusLine], 0.0f);
        s_bonusLine++;
    }
}

static void scene_bonus_begin_boss(void)
{
    const BonusDef *d = &BONUS_DEFS[s_bonusFight];
    d->begin(g_boss);
    boss_ui_set_form(d->phase);
}

static void scene_bonus_start_fight(void)
{
    const BonusDef *d = &BONUS_DEFS[s_bonusFight];
    cutscene_manager_clear_dialog();
    if (g_boss->phaseIndex != d->phase) scene_bonus_begin_boss();
    d->skip(g_boss);
    scene_init_playing(false);
    // The boss theme starts with the fight; its intro had the Guardian's cutscene music
    audio_stop_music();
    audio_play_music(d->music, true);
    s_pendingBossLoopMusic = false;
    character_reset_button_state();
    scene_sync_input_edge_state();
}

// ---- The Tower: floors back to back, no cutscenes, a boon draft between them ----


void scene_set_tower(bool on) { s_tower = on; }

static void scene_tower_start_floor(void)
{
    TowerBoss tb = tower_floor_boss();
#ifdef HLE_AUTOPLAY
    debugf("[tower] floor %d: boss %d\n", tower_floor(), tb);
#endif
    // A clean hall: nothing from the last floor's fight may linger
    gpu_wait();
    skill_fx_reset();
    msa_clear_all();
    boss_ground_crush_reset();
    lightning_fx_system_ring_enable(false);
    fallen_knight_free();
    sun_boss_free();
    dragon_boss_free();
    screen_shake_set_shake_mag(0.0f);

    boss_reset(g_boss);
    g_boss->pos[0] = BONUS_SPAWN_X;
    g_boss->pos[2] = 0.0f;
    phase2CutsceneTriggered = true;   // Guardian Unbound is a floor of its own, not a transformation
    s_bonusFight = BONUS_NONE;
    s_towerDeadT = 0.0f;
    bossWasDead = false;
    bossDeathMusicFadeStarted = false;

    scene_place_character();          // health and potions carry over
    cutsceneState = CUTSCENE_NONE;
    letterbox_hide();

    const char *music = s_bossLoopMusicPath;
    int form = 1;
    if (tb == TOWER_GUARDIAN || tb == TOWER_UNBOUND) {
        boss_activate_combat(g_boss);
        if (tb == TOWER_UNBOUND) {
            g_boss->phaseIndex = 2;
            g_boss->name = "Guardian Unbound";
            boss_ai_start_aerial_sword_barrage(g_boss);
            music = s_boss2LoopMusicPath;
            form = 2;
        }
    } else {
        s_bonusFight = tb == TOWER_FALLEN_KNIGHT ? BONUS_FALLEN_KNIGHT : tb == TOWER_SHACKLED_SUN ? BONUS_SHACKLED_SUN : BONUS_SHADOWBORN;
        const BonusDef *d = &BONUS_DEFS[s_bonusFight];
        d->begin(g_boss);
        d->skip(g_boss);
        music = d->music;
        form = d->phase;
    }
    boss_set_full_health(g_boss);     // sized for this floor of the climb

    boss_ui_set_form(form);
    bossTitleFade = 1.0f;
    bossUiIntro = 0.0f;
    boss_ui_set_intro(0.0f);
    boss_ui_snap_health_trail(1.0f);
    s_pendingBossLoopMusic = false;
    audio_stop_music();
    audio_play_music(music, true);
    camera_mode_smooth(CAMERA_CHARACTER, 1.0f);
    cameraLockOnActive = true;
    character_reset_button_state();
    scene_sync_input_edge_state();
}

static void scene_tower_begin_run(void)
{
    tower_begin_run((uint32_t)TICKS_READ() | 1u);
    character_reset();                // Tower potions and health (the run is active now)
    dialog_controller_stop_speaking();
    s_towerDraft = false;
    s_bossRunActive = false;
    gameState = GAME_STATE_PLAYING;
    bossUiIntro = playerUiIntro = 0.0f;
    character_ui_set_intro(0.0f);
    scene_tower_start_floor();
}

// Between floors: the draft; once a boon is taken, the next floor
static void scene_tower_draft_update(void)
{
    bool l = btn.d_left || joypad.stick_x < -40, r = btn.d_right || joypad.stick_x > 40, a = btn.a;
    bool u = btn.d_up || joypad.stick_y > 40, d = btn.d_down || joypad.stick_y < -40;
    static bool lastU, lastD;
    bool done = tower_draft_update(l && !s_towerLastL, r && !s_towerLastR, u && !lastU, d && !lastD, a && !s_towerLastA);
    s_towerLastL = l; s_towerLastR = r; s_towerLastA = a; lastU = u; lastD = d;
    if (!done) return;
    s_towerDraft = false;
    tower_floor_restore();
    tower_next_floor();
    scene_tower_start_floor();
}

// An intro shot of the spawn point: positions and target are relative to it, scaled to the boss
static void scene_bonus_shot(const BonusShot *s)
{
    cutscene_manager_set_camera_shot(&sceneContext,
        (T3DVec3){{ BONUS_SPAWN_X + s->p0[0], s->p0[1], s->p0[2] }}, (T3DVec3){{ BONUS_SPAWN_X + s->p1[0], s->p1[1], s->p1[2] }},
        (T3DVec3){{ BONUS_SPAWN_X + s->tg[0], s->tg[1], s->tg[2] }});
}

// The intro's cuts, in order: a shot per line, the gather and the descent
static const BonusShot *s_cutShot[10];
static float s_cutT[10];
static int s_cutN, s_cut;

static void scene_bonus_plan_cuts(const BonusDef *d)
{
    float gatherT = s_bonusDropT - BONUS_GATHER_S;
    s_cutN = 0;
    for (int i = 0; i < 6 && d->intro[i]; i++) {
        if (i == d->dropAt) {
            s_cutT[s_cutN] = gatherT; s_cutShot[s_cutN++] = &d->gather;
            s_cutT[s_cutN] = s_bonusDropT; s_cutShot[s_cutN++] = &d->drop;
        }
        float t = i == 0 ? 0.0f : s_bonusLineT[i];
        if (d->line[i].p0[1] > 0.0f && (i == 0 || i >= d->dropAt || t < gatherT)) {
            s_cutT[s_cutN] = t; s_cutShot[s_cutN++] = &d->line[i];
        }
    }
    scene_bonus_shot(s_cutShot[0]);
    s_cut = 1;
}

static void scene_bonus_intro_update(bool aJustPressed)
{
    const BonusDef *d = &BONUS_DEFS[s_bonusFight];
    float t = cutsceneTimer;
    cutscene_manager_update_dialog();

    if (aJustPressed) {
        if (skipButtonVisible) { scene_bonus_start_fight(); return; }
        skipButtonVisible = true;
    }
    scene_bonus_speak(d->intro, 6, t);

    if (s_bonusBeat == 0 && t >= s_bonusDropT) {     // it comes down from the dark
        scene_bonus_begin_boss();
        s_bonusBeat = 1;
    }
    if (s_cut < s_cutN && t >= s_cutT[s_cut]) {
#ifdef HLE_AUTOPLAY
        debugf("[cut] %d of %d at %.2f s (planned %.2f), ms %lu\n", s_cut, s_cutN, (double)t, (double)s_cutT[s_cut], (unsigned long)get_ticks_ms());
#endif
        scene_bonus_shot(s_cutShot[s_cut++]);
    }
    cutscene_manager_update_camera((s_cut < s_cutN ? s_cutT[s_cut] : s_bonusEndT) - s_cutT[s_cut - 1]);
    if (s_bonusBeat) {
        boss_update(g_boss);
        if (t >= s_bonusEndT) scene_bonus_start_fight();
    }
}

static void scene_bonus_outro_update(bool aJustPressed)
{
    const BonusDef *d = &BONUS_DEFS[s_bonusFight];
    float t = cutsceneTimer;
    cutscene_manager_update_dialog();
    scene_bonus_speak(d->outro, 4, t);
    if (t >= s_bonusEndT + 1.0f || (aJustPressed && t > 1.0f)) scene_controller_switch_to_title();
}

void scene_init_cutscene(void)
{
    scene_update_context();

    skipButtonVisible = false;

    if (cutscene_manager_handles_guardian_cutscene(cutsceneState)) {
        cutscene_manager_enter(&sceneContext, cutsceneState);
        return;
    }

    switch (cutsceneState) {
        case CUTSCENE_BONUS_INTRO: {
            // An empty hall: the boss descends at its dropT
            scene_place_character();
            if (g_boss) {
                g_boss->visible = false;
                g_boss->pos[0] = BONUS_SPAWN_X;
                g_boss->pos[2] = 0.0f;
            }
            s_bonusBeat = 0;
            scene_bonus_schedule(BONUS_DEFS[s_bonusFight].intro, 6, BONUS_DEFS[s_bonusFight].dropAt, 0.8f);
            cutsceneTimer = 0.0f;
            audio_stop_music();
            audio_play_music("rom:/audio/music/boss_phase1_cutscene1-22k.wav64", false);   // as the Guardian's intro; the theme waits for the fight
            scene_bonus_plan_cuts(&BONUS_DEFS[s_bonusFight]);
        } break;

        case CUTSCENE_ENDING: {
            if (g_boss) g_boss->visible = false;
            cameraLockOnActive = false;
            letterbox_show(false);
            ending_begin();
        } break;

        case CUTSCENE_BONUS_OUTRO: {
            // The boss lies defeated; the camera turns to it
            s_bonusBeat = 0;
            cutsceneTimer = 0.0f;
            character_set_velocity_xz(0.0f, 0.0f);
            letterbox_show(true);
            customCamPos = camPos;
            customCamTarget = (T3DVec3){{ g_boss->pos[0], g_boss->pos[1] + 14.0f, g_boss->pos[2] }};
            float k = BONUS_DEFS[s_bonusFight].camScale;
            if (k > 1.0f) {     // a big body: stand off, on the player's side of it
                float dx = character.pos[0] - g_boss->pos[0], dz = character.pos[2] - g_boss->pos[2];
                float d = fmaxf(sqrtf(dx * dx + dz * dz), 1.0f);
                customCamPos = (T3DVec3){{ g_boss->pos[0] + dx / d * 60.0f * k, g_boss->pos[1] + 28.0f * k,
                                           g_boss->pos[2] + dz / d * 60.0f * k }};
            }
            camera_mode_smooth(CAMERA_CUSTOM, 1.0f);
            scene_bonus_schedule(BONUS_DEFS[s_bonusFight].outro, 4, -1, 0.0f);
            audio_stop_music();
            audio_play_music("rom:/audio/music/boss_phase1_cutscene1-22k.wav64", false);
        } break;

        case CUTSCENE_POST_BOSS_RESTORED: {
            // Post-boss dialog: keep the current gameplay camera position,
            // but retarget it to face the boss.
            cutsceneDialogActive = true;
            bossPostDefeatDialogStep = 0;
            skipButtonVisible = false;
            lastCutsceneAPressed = false;

            // Freeze character motion for the duration of this dialog so we don't coast afterward.
            character_set_velocity_xz(0.0f, 0.0f);

            // Keep current camera position and smoothly rotate to face the boss.
            customCamPos = camPos;
            customCamTarget = get_boss_lock_focus_point();
            camera_mode_smooth(CAMERA_CUSTOM, 0.25f);

            // Start the current chat's first line.
            // bossPostDefeatChatIndex persists across cutscene entries.
            const PostBossChat *chat = cutscene_manager_get_post_boss_chat(bossPostDefeatChatIndex);
            dialog_controller_speak(chat->line1, 0, chat->holdSec, false, true);
        } break;

        default:
            break;
    }
}

static void scene_update_post_boss_cutscene_input(void)
{
    /*
     * Guardian phase cutscene skip is owned by cutscene_manager_skip().
     * This remains only for the scene-owned post-boss restored cutscene.
     */
    if (cutsceneState == CUTSCENE_POST_BOSS_RESTORED) {
        skipButtonVisible = false;
        lastCutsceneAPressed = btn.a;
        return;
    }

    lastCutsceneAPressed = btn.a;
}

void scene_cutscene_update(void)
{
    scene_update_context();

    if (cutscene_manager_handles_guardian_cutscene(cutsceneState)) {
        cutscene_manager_update(&sceneContext, deltaTime);
        return;
    }

    switch (cutsceneState) {
        case CUTSCENE_POST_BOSS_RESTORED: {
            // Post-boss dialog runs while gameplay continues to animate without player input.
            character_update_cinematic();

            if (boss_is_active(g_boss)) {
                boss_update(g_boss);
            }

            collision_update();

            // Allow A/Start to advance immediately to the next line or end.
            if (dialog_controller_speaking() && (btn.a || btn.start)) {
                dialog_controller_skip();
            }

            dialog_controller_update();

            if (!dialog_controller_speaking()) {
                const PostBossChat *chat = cutscene_manager_get_post_boss_chat(bossPostDefeatChatIndex);

                if (bossPostDefeatDialogStep == 0 && chat->line2) {
                    bossPostDefeatDialogStep = 1;

                    dialog_controller_speak(
                        chat->line2,
                        0,
                        chat->holdSec,
                        false,
                        true
                    );
                } else {
                    int count = cutscene_manager_post_boss_chat_count();

                    if (bossPostDefeatChatIndex < count - 1) {
                        bossPostDefeatChatIndex++;
                    }

                    cutsceneDialogActive = false;
                    cutsceneState = CUTSCENE_NONE;
                    cutsceneTimer = 0.0f;
                    cutsceneCameraTimer = 0.0f;
                    skipButtonVisible = false;

                    camera_mode_smooth(CAMERA_CHARACTER, 0.8f);

                    character_set_velocity_xz(0.0f, 0.0f);

                    character_reset_button_state();
                    scene_sync_input_edge_state();
                    return;
                }
            }
        } break;

        case CUTSCENE_ENDING: {
            bool aJustPressed = (btn.a || btn.start) && !lastCutsceneAPressed;
            lastCutsceneAPressed = btn.a || btn.start;
            if (aJustPressed && !skipButtonVisible) skipButtonVisible = true;
            else if (aJustPressed || !ending_update(deltaTime)) {
                cutscene_manager_clear_dialog();
                screen_shake_set_shake_mag(0.0f);
                scene_controller_switch_to_title();
            }
            return;
        }

        case CUTSCENE_BONUS_INTRO:
        case CUTSCENE_BONUS_OUTRO: {
            bool aJustPressed = btn.a && !lastCutsceneAPressed;
            lastCutsceneAPressed = btn.a;
            cutsceneTimer += deltaTime;
            cutsceneCameraTimer += deltaTime;
            character_update_cinematic();
            skill_fx_update(deltaTime);
            if (cutsceneState == CUTSCENE_BONUS_INTRO) {
                scene_bonus_intro_update(aJustPressed);
            } else {
                if (boss_is_active(g_boss)) boss_update(g_boss);
                scene_bonus_outro_update(aJustPressed);
            }
            return;
        }

        default:
            break;
    }

    scene_update_post_boss_cutscene_input();
}

#ifdef HLE_LOG_FPS   // test aid: CPU ms of the update's parts
static uint32_t s_up[6], s_upN;
#define UPROF(i, x) do { uint32_t _t = TICKS_READ(); x; s_up[i] += TICKS_READ() - _t; } while (0)
#else
#define UPROF(i, x) x
#endif
void scene_update(void) {
#ifdef HLE_LOG_FPS
    if (++s_upN == 120) { debugf("[update] character %.2f position %.2f skills %.2f boss %.2f collision %.2f ms\n", TICKS_TO_US(s_up[0] / 120) / 1000.0, TICKS_TO_US(s_up[1] / 120) / 1000.0,
        TICKS_TO_US(s_up[2] / 120) / 1000.0, TICKS_TO_US(s_up[3] / 120) / 1000.0, TICKS_TO_US(s_up[4] / 120) / 1000.0); memset(s_up, 0, sizeof s_up); s_upN = 0; }
#endif
    // The button layout applies while the player is in control (not in menus, cutscenes,
    // the Tower's draft or the death screen, which read A and B as they are)
    joypad_set_gameplay(gameState == GAME_STATE_PLAYING && !menu_controller_is_active() && cutsceneState == CUTSCENE_NONE
                        && !s_towerDraft && character_get_state() != CHAR_STATE_DEAD);

    if (menu_controller_consume_action() == MENU_ACTION_QUIT_TO_TITLE) {
        cutscene_manager_clear_dialog();
        screen_shake_set_shake_mag(0.0f);
        scene_controller_switch_to_title();
        return;
    }

    if (gameState == GAME_STATE_VIDEO) {
        return;
    }

    audio_update_fade(deltaTime);

    if (s_pendingBossLoopMusic) {
        if (!audio_is_music_playing() && g_boss->health > 0) {
            s_pendingBossLoopMusic = false;
            audio_play_music((g_boss->phaseIndex == FALLEN_KNIGHT_PHASE) ? s_boss3LoopMusicPath :
                             (g_boss->phaseIndex == 2) ? s_boss2LoopMusicPath : s_bossLoopMusicPath, true);
        }
    }

    if (g_boss && g_boss->health <= 0.0f && gameState != GAME_STATE_VIDEO) {
        if(!bossDeathMusicFadeStarted){
            bossDeathMusicFadeStarted = true;
            audio_stop_music_fade(2);
        }
        if (!s_bonusFight && !s_tower) scene_update_video_trigger();   // bonus fights and the Tower have no ending portal
    }

    scene_update_video_preroll();  // always safe; it early-outs

    // Update all scrolling textures
    scroll_update();
    // Check if pause menu was just closed - if so, reset character button state
    // NOTE: during victory, the pause menu overlays without switching GAME_STATE to MENU.
    bool pauseMenuBlocking = scene_is_menu_active() || menu_controller_is_pause_menu_active();
    if (lastMenuActive && !pauseMenuBlocking) {
        // Menu was just closed - reset character button state to prevent false "just pressed"
        character_reset_button_state();
    }
    lastMenuActive = pauseMenuBlocking;

    // If player is dead, disable player control but keep boss/UI moving.
    // Death is owned by the character now; the scene only reads the character state.
    if (character_get_state() == CHAR_STATE_DEAD) {
        deathRestartLockoutTimer += deltaTime;

        // Still update the character so end-state animations, like Death, can play.
        character_update_cinematic();

        // Keep boss AI updating so it continues moving during end screen.
        if (boss_is_active(g_boss)) {
            boss_update(g_boss);
        }

        letterbox_update();

        if (deathRestartLockoutTimer >= DEATH_RESTART_LOCKOUT_S && btn.a) {
            scene_controller_switch_to_title();
        }
        return;
    }

    // Don't update game logic when pause menu is active (including victory overlay case)
    if (pauseMenuBlocking) {
        return;
    }

    if (s_towerDraft) {     // the fight is over; the hall waits while a boon is chosen
        scene_tower_draft_update();
        return;
    }

    // Debug hotkey: L-trigger skips to boss defeated (dead + fully stopped)
    // if (DEV_MODE)
    // {
    //     bool lHeld = joypad.btn.l;
    //     bool lJustPressed = lHeld && !lastLPressed;
    //     lastLPressed = lHeld;
    //     if (lJustPressed && boss_is_active(g_boss)) {
    //         scene_debug_force_boss_defeated();
    //     }
    // }

    if(cutsceneState == CUTSCENE_NONE) // Normal gameplay
    {
        // Post-boss interaction trigger: after defeat, when Z-targeted and close enough,
        // start the dialog/camera only when player presses A (held-edge).
        // IMPORTANT: check this BEFORE character_update so A doesn't also trigger a roll.
        if (!s_tower && boss_is_interactable(g_boss) && g_boss->state == BOSS_STATE_DEAD) {
            bool aHeld = joypad.btn.a;
            bool aJustPressed = aHeld && !lastInteractAHeld;
            lastInteractAHeld = aHeld;

            // Match the on-screen prompt behavior: if the "A" prompt is visible, A should talk.
            // Do NOT require Z-targeting; post-fight we untarget by default, but still allow re-targeting.
            if (scene_post_boss_interact_allowed(g_boss) && aJustPressed) {
                cutsceneState = CUTSCENE_POST_BOSS_RESTORED;
                cutsceneTimer = 0.0f;
                cutsceneCameraTimer = 0.0f;
                scene_init_cutscene();
                return;
            }
        } else {
            // Keep interact edge-tracker in sync when not eligible
            lastInteractAHeld = joypad.btn.a;
        }

        UPROF(0, character_update());
        if (s_tower) tower_tick(deltaTime);
        // Update character transform after constraint
        UPROF(1, character_update_position());
        UPROF(2, skill_fx_update(deltaTime));

        boss_ground_crush_update(deltaTime);

        if (boss_is_active(g_boss)) {
            UPROF(3, boss_update(g_boss));
            // Boss death no longer forces GAME_STATE_VICTORY.
            // The boss will play its collapse and remain still; the player can keep moving.

            // Second fight: when the Guardian falls the first time, it rises again as
            // Guardian Unbound (phase-2 cutscene). Refill its health in this same frame
            // so the ending (music fade, video trigger, clear time) does not start.
            if (s_tower && g_boss->state == BOSS_STATE_DEAD) {
                s_towerDeadT += deltaTime;
                if (s_towerDeadT >= 2.0f) {     // let the death play out, then the draft
                    tower_floor_cleared();
                    s_towerDraft = true;
                    s_towerLastL = s_towerLastR = s_towerLastA = true;   // no carried-over press
                    return;
                }
            }
#if PHASE_2_ENABLED
            if (!s_tower && s_bonusFight && g_boss->state == BOSS_STATE_DEAD) {
                if (s_bonusEndTimer == 0.0f) save_controller_mark_bonus_beaten(s_bonusFight - BONUS_FALLEN_KNIGHT);
                s_bonusEndTimer += deltaTime;
                if (s_bonusEndTimer >= 2.5f) {     // let the death play out, then the outro
                    cutsceneState = CUTSCENE_BONUS_OUTRO;
                    scene_init_cutscene();
                    return;
                }
            }
            if (!phase2CutsceneTriggered && g_boss->phaseIndex == 1 && g_boss->health <= 0.0f) {
                phase2CutsceneTriggered = true;
                g_boss->health = g_boss->maxHealth;
                g_boss->state = BOSS_STATE_INTRO;
                g_boss->mode = BOSS_MODE_CINEMATIC;
                cameraLockOnActive = false;
                skill_fx_reset();
                cutsceneTimer = 0.0f;
                cutsceneCameraTimer = 0.0f;
                cutsceneState = CUTSCENE_PHASE2_INTRO;
                g_boss->damageFlashTimer = 0.0f;
                scene_init_cutscene();
                return;
            }
#endif
        }

        UPROF(4, collision_update());


        //dialog_controller_update();

        // Update boss title fade-out when fight has started
        if (bossTitleFade > 0.0f) {
            bossTitleFade -= deltaTime / bossTitleFadeSpeed;
            if (bossTitleFade < 0.0f) bossTitleFade = 0.0f;
        }

        // Progress UI intro animations
        // Boss health bar should start growing only after boss title is gone
        if (bossTitleFade <= 0.0f && bossUiIntro < 1.0f) {
            bossUiIntro += deltaTime / uiIntroSpeed;
            if (bossUiIntro > 1.0f) bossUiIntro = 1.0f;
            boss_ui_set_intro(bossUiIntro);
        }
        if (playerUiIntro < 1.0f) {
            playerUiIntro += deltaTime / uiIntroSpeed;
            if (playerUiIntro > 1.0f) playerUiIntro = 1.0f;
            character_ui_set_intro(playerUiIntro);
        }
    }
    else // Cutscene
    {
        scene_cutscene_update();
    }

    // Update letterbox animation
    letterbox_update();

    // Advance victory title animation timer (but still allow full gameplay update above)
    if (gameState == GAME_STATE_VICTORY && !victoryTitleDone) {
        const float total = VICTORY_TITLE_FADEIN_S + VICTORY_TITLE_HOLD_S + VICTORY_TITLE_FADEOUT_S;
        victoryTitleTimer += deltaTime;
        if (victoryTitleTimer >= total) {
            victoryTitleTimer = total;
            victoryTitleDone = true;
        }
    }

    // Post-boss cleanup: once the boss becomes dead, clear Z-targeting so the player is untargeted
    // after the fight. The player can still re-target by pressing Z as normal.
    {   // the clear time counts only fighting: not the phase-2 reveal or other cutscenes, not the pause menu
        static double last;
        double d = last > 0.0 ? nowS - last : 0.0; last = nowS;
        if (d > 0.25) d = 0.25;     // a stall (a load, or the frame after the pause menu): not fighting time
        if (s_bossRunActive && cutsceneState == CUTSCENE_NONE && gameState == GAME_STATE_PLAYING && !menu_controller_is_active())
            s_bossRunPlayS += d;
    }
    if (boss_is_active(g_boss)) {
        bool bossDeadNow = (g_boss->state == BOSS_STATE_DEAD);
        if (bossDeadNow && !bossWasDead) {
            // Record fastest clear time per save slot (once, on death edge).
            if (s_bossRunActive && s_bossRunStartS > 0.0 && !s_bonusFight && !s_tower) {
                double dt = s_bossRunPlayS;
#ifdef HLE_AUTOPLAY
                debugf("[clear] fighting %.1f s of %.1f s since the fight began\n", dt, nowS - s_bossRunStartS);
#endif
                if (dt > 0.0) {
                    uint32_t ms = (uint32_t)(dt * 1000.0);
                    (void)save_controller_record_boss_clear_time_ms(ms);
                }
            }
            s_bossRunActive = false;

            cameraLockOnActive = false;     // keep cameraLockBlend: the camera hands off from where the lock-on left it, as on a Z tap
            // Sync edge detectors so we don't immediately re-toggle due to a held button
            lastZPressed = joypad.btn.z;
            lastInteractAHeld = joypad.btn.a;
            lastCLeftHeld = joypad.btn.c_left;
            lastCRightHeld = joypad.btn.c_right;
            s_zHoldTimer = 0.0f;
            s_zHoldConsumed = false;
            s_zActivatedOnPress = false;
        }
        bossWasDead = bossDeadNow;
    } else {
        bossWasDead = false;
    }

    // Z-target:
    // - Tap Z toggles lock-on on/off
    // - Hold Z keeps lock-on active and allows cycling targets with C-left/C-right
    bool lockonAllowed = cutsceneState == CUTSCENE_NONE && boss_is_active(g_boss);
    bool zHeld = joypad.btn.z;
    bool zJustPressed = zHeld && !lastZPressed;
    bool zJustReleased = !zHeld && lastZPressed;
    bool cLeftHeld = joypad.btn.c_left;
    bool cRightHeld = joypad.btn.c_right;
    bool cLeftJustPressed = cLeftHeld && !lastCLeftHeld;
    bool cRightJustPressed = cRightHeld && !lastCRightHeld;

    // D-left is the health potion (the other D-pad directions are skills, see character.c).
    // Avoid consuming during cutscenes / dialog sequences.
    if (cutsceneState == CUTSCENE_NONE && btn.d_left) {
        (void)character_try_use_health_potion();
    }

    // If lock-on is not allowed, force it off.
    if (!lockonAllowed) {
        cameraLockOnActive = false;
    }

    if (zJustPressed) {
        s_zHoldTimer = 0.0f;
        s_zHoldConsumed = false;
        s_zActivatedOnPress = false;

        // If we're currently NOT locked-on, pressing Z should immediately lock on.
        if (lockonAllowed && !cameraLockOnActive) {
            cameraLockOnActive = true;
            s_zActivatedOnPress = true;
        }
    }

    if (zHeld) {
        s_zHoldTimer += deltaTime;
    }

    if (lockonAllowed) {
        // While holding Z, allow cycling targets without changing lock-on active state.
        if (zHeld) {
            if (cLeftJustPressed) {
                scene_cycle_lock_target(-1);
                s_zHoldConsumed = true;
            } else if (cRightJustPressed) {
                scene_cycle_lock_target(1);
                s_zHoldConsumed = true;
            }
        }

        // Short tap-release toggles OFF when already locked-on.
        // Holding Z (or cycling) will not untarget.
        if (zJustReleased) {
            if (cameraLockOnActive && !s_zActivatedOnPress && !s_zHoldConsumed && s_zHoldTimer <= Z_TAP_TOGGLE_MAX_S) {
                cameraLockOnActive = false;
            }
            s_zActivatedOnPress = false;
        }

        // Update target position when lock-on is active
        if (cameraLockOnActive) {
            cameraLockOnTarget = get_boss_lock_focus_point();
        }
    }

    if (cutsceneState == CUTSCENE_NONE) {
        msa_update(deltaTime); // multi sword attack (Guardian Unbound)
    }
    //boulder_hazard_update(deltaTime); // close-range ground boulders

    lastZPressed = zHeld;
    lastCLeftHeld = cLeftHeld;
    lastCRightHeld = cRightHeld;
}

void scene_fixed_update(void)
{
}

// Draws a small lock-on marker over the boss when Z-targeting is active.
static void draw_lockon_indicator(T3DViewport *viewport)
{
    bool visible = cameraLockOnActive && !scene_is_cutscene_active() && boss_is_active(g_boss);
    T3DVec3 worldPos = get_boss_lock_focus_point();

    boss_ui_draw_lockon_marker(viewport, &worldPos, visible);
}

// The hall with the knight and the boss (cutscenes that keep the gameplay view).
static void scene_draw_hall(void)
{
    rdpq_sync_pipe();
    rdpq_mode_zbuf(false, false);

    // No-depth environment first.
    t3d_matrix_push_pos(1);
        if (windowsMatrix && windowsDpl) {
            t3d_matrix_set(windowsMatrix, true);
            rspq_block_run(windowsDpl);
        }

        if (mapMatrix && mapDpl) {
            t3d_matrix_set(mapMatrix, true);
            rspq_block_run(mapDpl);
        }
    t3d_matrix_pop(1);

    // Floor.
    rdpq_sync_pipe();
    rdpq_mode_zbuf(true, true);

    t3d_matrix_push_pos(1);
        if (roomFloorMatrix && roomFloorDpl) {
            t3d_matrix_set(roomFloorMatrix, true);
            rspq_block_run(roomFloorDpl);
        }
    t3d_matrix_pop(1);

    // Shadows.
    rdpq_sync_pipe();
    rdpq_mode_zbuf(false, false);

    t3d_matrix_push_pos(1);
        character_draw_shadow();

        if (g_boss) {
            boss_draw_shadow(g_boss);
        }
    t3d_matrix_pop(1);

    // Room pieces.
    rdpq_sync_pipe();
    rdpq_mode_zbuf(true, true);

    t3d_matrix_push_pos(1);
        if (roomLedgeMatrix && roomLedgeDpl) {
            t3d_matrix_set(roomLedgeMatrix, true);
            rspq_block_run(roomLedgeDpl);
        }

        if (pillarsMatrix && pillarsDpl) {
            t3d_matrix_set(pillarsMatrix, true);
            rspq_block_run(pillarsDpl);
        }

        if (pillarsFrontMatrix && pillarsFrontDpl) {
            t3d_matrix_set(pillarsFrontMatrix, true);
            rspq_block_run(pillarsFrontDpl);
        }
    t3d_matrix_pop(1);

    // Characters.
    t3d_matrix_push_pos(1);
        character_draw();

        if (g_boss) {
            boss_draw(g_boss);
        }
    t3d_matrix_pop(1);

    // Persistent gameplay chains only.
    // cinematicChains moved into cutscene_guardian_phase1.c and should not be referenced here.
    t3d_matrix_push_pos(1);
        if (chainsMatrix && chainsDpl) {
            t3d_matrix_set(chainsMatrix, true);
            rspq_block_run(chainsDpl);
        }
    t3d_matrix_pop(1);
}

void scene_draw_cutscene(T3DViewport *viewport)
{
    scene_update_context();

    if (cutscene_manager_handles_guardian_cutscene(cutsceneState)) {
        cutscene_manager_draw(&sceneContext, viewport);
        return;
    }

    switch (cutsceneState) {
        case CUTSCENE_ENDING: {
            if (ending_room_visible()) scene_draw_hall();
            ending_draw_world();
            ending_draw_overlay();
        } break;

        case CUTSCENE_BONUS_INTRO:
        case CUTSCENE_BONUS_OUTRO:
        case CUTSCENE_POST_BOSS_RESTORED: {
            /*
             * Render the normal scene while the post-boss dialog is active.
             * This avoids a black screen because this cutscene is camera/dialog only.
             */

            scene_draw_hall();

            skill_fx_draw();

            // 2D dialog overlay.
            if (cutsceneDialogActive) {
                int height = 70;
                int width = 220;
                int x = (SCREEN_WIDTH - width) / 2;
                int y = 240 - height - 10;

                dialog_controller_draw(false, x, y, width, height);
            }
        } break;

        default:
            break;
    }
}

static void scene_draw_video_preroll_fade(void)
{
    if (videoPreroll != VIDEO_PREROLL_NONE) {
        display_utility_solid_black_transition(false, VIDEO_FADE_SPEED);
    }
}

static void scene_draw_video_trigger(T3DViewport *vp)
{
    if (!debugDraw || !vp) return;

    T3DVec3 mn = {{ videoTrigMin[0], videoTrigMin[1], videoTrigMin[2] }};
    T3DVec3 mx = {{ videoTrigMax[0], videoTrigMax[1], videoTrigMax[2] }};

    uint16_t color = DEBUG_COLORS[2];               // normal (green-ish)
    if (videoTrigHitThisFrame) color = DEBUG_COLORS[0]; // hit (red)
    if (videoTrigFired)        color = DEBUG_COLORS[5]; // fired (cyan)

    debug_draw_aabb(vp, &mn, &mx, color);
}

#ifdef HLE_LOG_FPS
static uint32_t s_prof[8];
#endif
#ifdef HLE_PROF_SERIAL   // test aid: each stage recorded, then run alone: its own RSP+RDP time
static uint32_t s_gpu[7];
#define PROF_SERIAL_WAIT(i) { s_gpu[i] += gpu_prof_end(); if (i < 6) gpu_prof_begin(); }
#endif
// The hall, culled to the camera: each object recorded once with its material (its userBlock),
// and each frame only those in the view frustum run. The RSP's vertex work bounds the frame, and
// much of the hall is behind the camera; setting the materials live cost the CPU 2 ms (and its
// commands slowed the RSP). Their own material each: the hall's objects hardly share one. The
// hall models are all drawn at MODEL_SCALE, raised by roomY
#ifdef HLE_PROF_SERIAL   // test aid: each hall model's own GPU time
static uint32_t s_envGpu[8]; static const T3DModel *s_envModel[8];
static void draw_culled_impl(T3DModel *m, const T3DViewport *vp);
static void draw_culled(T3DModel *m, const T3DViewport *vp)
{
    s_gpu[0] += gpu_prof_end(); gpu_prof_begin();
    draw_culled_impl(m, vp);
    uint32_t t = gpu_prof_end(); gpu_prof_begin();
    int k = 0; while (k < 7 && s_envModel[k] && s_envModel[k] != m) k++;
    s_envModel[k] = m; s_envGpu[k] += t;
    static int n; if (++n == 6 * 120) { debugf("[envgpu]"); for (int i = 0; i < 8 && s_envModel[i]; i++) { debugf(" %.2f", TICKS_TO_US(s_envGpu[i] / 120) / 1000.0); s_envGpu[i] = 0; } debugf("\n"); n = 0; }
}
#else
static void draw_culled_impl(T3DModel *m, const T3DViewport *vp);
#define draw_culled draw_culled_impl
#endif
static void draw_culled_impl(T3DModel *m, const T3DViewport *vp)
{
    T3DModelIter it = t3d_model_iter_create(m, T3D_CHUNK_TYPE_OBJECT);
    while (t3d_model_iter_next(&it)) {
        T3DObject *o = it.object;
        T3DVec3 mn = {{ o->aabbMin[0] * MODEL_SCALE, o->aabbMin[1] * MODEL_SCALE + roomY, o->aabbMin[2] * MODEL_SCALE }};
        T3DVec3 mx = {{ o->aabbMax[0] * MODEL_SCALE, o->aabbMax[1] * MODEL_SCALE + roomY, o->aabbMax[2] * MODEL_SCALE }};
        if (!t3d_frustum_vs_aabb(&vp->viewFrustum, &mn, &mx)) continue;
        if (!o->userBlock) {
            gpu_block_begin();
            t3d_model_draw_material(o->material, NULL);     // from scratch: the block runs after anything
            t3d_model_draw_object(o, NULL);
            o->userBlock = gpu_block_end();
        }
        rspq_block_run(o->userBlock);
    }
}

void scene_draw(T3DViewport *viewport)
{
#ifdef HLE_LOG_FPS
    s_prof[0] = TICKS_READ();
#endif

    if(gameState == GAME_STATE_VIDEO)
        return;

    t3d_frame_start();

    if(!DITHER_ENABLED && !debugDraw)
    {
        rdpq_mode_dithering(DITHER_NONE_BAYER);
    }

    t3d_viewport_attach(viewport);
    g_viewFrustum = &viewport->viewFrustum;
#ifdef HLE_PROF_NORDP   // test aid: nothing rasterized, so the GPU time left is the RSP's
    rdpq_set_scissor(0, 0, 1, 1);
#endif

#ifdef HLE_TEST_COVER
    {   // test aid: only the knight and the boss on magenta (cover art cutouts)
        t3d_screen_clear_color(RGBA32(255, 0, 255, 0xFF));
        t3d_screen_clear_depth();
        t3d_fog_set_enabled(false);
        t3d_light_set_ambient(colorAmbient);
        t3d_matrix_push_pos(1);
            character_draw();
            if (g_boss) boss_draw(g_boss);
        t3d_matrix_pop(1);
        return;
    }
#endif

    // Fog
    color_t fogColor = (color_t){0, 0, 0, 0xFF};

    rdpq_mode_fog(RDPQ_FOG_STANDARD);
    rdpq_set_fog_color(fogColor);

    t3d_screen_clear_color(RGBA32(0, 0, 0, 0xFF));
    t3d_screen_clear_depth();

    if(cutscene_manager_handles_guardian_cutscene(cutsceneState)){
        cutscene_manager_draw_fog();
    }else{
        t3d_fog_set_range(450.0f, 800.0f);
    }
    t3d_fog_set_enabled(true);

    // Lighting
    t3d_light_set_ambient(colorAmbient);

    if(cutsceneState != CUTSCENE_NONE)
    {
        scene_draw_cutscene(viewport);
        // Draw letterbox bars during cutscenes
        letterbox_draw();
        // Draw skip indicator on top of letterbox bars
        cutscene_manager_draw_skip_overlay(skipButtonVisible);
        return;
    }
    // ===== DRAW 3D =====
#ifdef HLE_PROF_SERIAL
    gpu_prof_begin();
#endif

    rdpq_mode_zbuf(false, false);

    // Draw no depth environment first
    t3d_matrix_push_pos(1);

    t3d_matrix_set(windowsMatrix, true);
    draw_culled(windowsModel, viewport);

    t3d_matrix_set(mapMatrix, true);
    draw_culled(mapModel, viewport);

    //Draw depth environment
    rdpq_mode_zbuf(true, true);

    t3d_matrix_set(roomFloorMatrix, true);
    draw_culled(roomFloorModel, viewport);

#ifdef HLE_PROF_SERIAL
    PROF_SERIAL_WAIT(0);
#endif
#ifdef HLE_LOG_FPS
    s_prof[1] = TICKS_READ();
#endif

    rdpq_mode_zbuf(false, false);

    // projection effects
    boss_ground_crush_draw();
    // blob shadows
#ifndef HLE_SHOT_CLEAN
    character_draw_shadow();
#endif
    if (g_boss) {
        boss_draw_shadow(g_boss);
    }

    rdpq_mode_zbuf(true, true);


    t3d_matrix_set(roomLedgeMatrix, true);
    draw_culled(roomLedgeModel, viewport);

    t3d_matrix_set(pillarsMatrix, true);
    draw_culled(pillarsModel, viewport);

    t3d_matrix_set(pillarsFrontMatrix, true);
    draw_culled(pillarsFrontModel, viewport);

#ifdef HLE_PROF_SERIAL
    PROF_SERIAL_WAIT(1);
#endif
#ifdef HLE_LOG_FPS
    s_prof[2] = TICKS_READ();
#endif

    rdpq_mode_zbuf(false, false);

    // floor glow
    if(g_boss->health <= 0)
    {
        t3d_matrix_set(floorGlowMatrix, true);
        // Create a struct to pass the scrolling parameters to the tile callback
        t3d_model_draw_custom(floorGlowModel, (T3DModelDrawConf){
            .userData = &floorGlowScrollParams,
            .tileCb = tile_scroll,
        });
    }


    rdpq_mode_zbuf(true, true);

    // Draw characters
#if !defined(HLE_SKIP_KNIGHT) && !defined(HLE_SHOT_CLEAN)
    character_draw();
#endif

#ifdef HLE_PROF_BOSS    // test aid: the RSP drained first, so the boss's draw time is the CPU's alone
    { static uint32_t w, n; uint32_t t = TICKS_READ(); gpu_wait(); w += TICKS_READ() - t;
      if (++n == 120) { debugf("[bossprof] RSP behind by %.2f ms before the boss\n", TICKS_TO_US(w / n) / 1000.0); w = n = 0; } }
#endif
#ifdef HLE_PROF_SERIAL
    PROF_SERIAL_WAIT(2);
#endif
#ifdef HLE_LOG_FPS
    s_prof[3] = TICKS_READ();
#endif
#ifndef HLE_SKIP_BOSS    // test aid: a stage's real cost, measured by its absence
    if (g_boss) {
        boss_draw(g_boss);
    }
#endif

#ifdef HLE_PROF_SERIAL   // test aid: the summoned swords alone (their worst frame too)
    { static uint32_t acc, worst, n; s_gpu[3] += gpu_prof_end(); gpu_prof_begin();
      msa_draw_visuals(viewport);
      uint32_t t = gpu_prof_end(); gpu_prof_begin(); acc += t; if (t > worst) worst = t;
      if (++n == 120) { debugf("[msagpu] %.2f ms, worst %.2f\n", TICKS_TO_US(acc / n) / 1000.0, TICKS_TO_US(worst) / 1000.0); acc = worst = n = 0; } }
#else
    msa_draw_visuals(viewport); // multi sword attack (Guardian Unbound)
#endif

#ifdef HLE_PROF_SERIAL
    PROF_SERIAL_WAIT(3);
#endif
#ifdef HLE_LOG_FPS
    s_prof[4] = TICKS_READ();
#endif
    //boulder_hazard_draw(viewport); // close-range ground boulders

    // Fog door (transparent): depth test ON, depth write OFF so it can be drawn late.
    rdpq_mode_zbuf(true, false);

    if (fogDoorMatrix && fogDoorModel) {
        t3d_matrix_set(fogDoorMatrix, true);
        t3d_model_draw_custom(fogDoorModel, (T3DModelDrawConf){
            .userData = &fogScrollParams,
            .tileCb = tile_scroll,
        });
    }

    rdpq_mode_zbuf(true, true);

    t3d_matrix_set(chainsMatrix, true);
    rspq_block_run(chainsDpl);

    t3d_matrix_pop(1);

#ifdef HLE_PROF_SERIAL
    PROF_SERIAL_WAIT(4);
#endif
#ifdef HLE_LOG_FPS
    s_prof[5] = TICKS_READ();
#endif

    // Screen-space ribbon trails, drawn right after 3D so they feel "in world"
    sword_trail_draw_all(viewport);

    // Dust puffs (boss landings/impacts)
    dust_particles_fx_update(deltaTime);
    dust_particles_fx_draw(viewport);

    // Blood splatters from boss hits (drawn after dust so red reads on top of puffs)
    blood_particles_fx_update(deltaTime);
    blood_particles_fx_draw(viewport);

    // Player skill waves, arches, pillars and bursts
    skill_fx_draw();

#ifdef HLE_PROF_SERIAL
    PROF_SERIAL_WAIT(5);
#endif
#ifdef HLE_LOG_FPS
    s_prof[6] = TICKS_READ();
#endif

    // ===== DRAW 2D =====
    // Post-boss interaction prompt ("A") above the defeated boss when close enough to interact
    {
        T3DVec3 postBossPromptPos;
        bool postBossPromptVisible =
            !scene_is_cutscene_active()
            && !s_tower     // no talk in the Tower: the draft follows the fall
            && boss_is_interactable(g_boss)
            && g_boss
            && g_boss->state == BOSS_STATE_DEAD
            && scene_post_boss_in_range(g_boss);

        if (!boss_get_head_world_pos(g_boss, &postBossPromptPos)) {
            postBossPromptPos = get_boss_lock_focus_point();
        }

#ifdef HLE_TEST_TOWER_RUN      // test aid: the defeated boss's "A" prompt, in range
        if (g_boss && g_boss->state == BOSS_STATE_DEAD && boss_is_interactable(g_boss) && scene_post_boss_in_range(g_boss))
            debugf("[prompt] in range, shown %d\n", postBossPromptVisible);
#endif
        boss_ui_draw_post_boss_a_prompt(viewport, &postBossPromptPos, postBossPromptVisible);
    }

    // Overlay lock-on marker above the boss.
    draw_lockon_indicator(viewport);

    scene_draw_video_trigger(viewport);

    bool cutsceneActive = scene_is_cutscene_active();
    bool isDead = character_get_state() == CHAR_STATE_DEAD;
    bool isVictory = gameState == GAME_STATE_VICTORY;
    bool isEndScreen = isDead || isVictory;

    // Draw letterbox bars (they handle their own visibility and animation)
    letterbox_draw();

    // Draw UI elements after 3D rendering is complete.
    // Keep player UI visible during victory; hide it during death, cutscenes and the pause menu
    // (its pages, the Tower's run list, are drawn over the scene)
#if defined(HLE_SHOT_CLEAN) || defined(HLE_NO_HUD)      // test aid: no HUD (README shots)
    if (false) {
#else
    if (!cutsceneActive && !isDead && !menu_controller_is_active() && !s_towerDraft) {
#endif
        // Boss UI/debug is owned by boss_render/boss_ui now.
        // Keep scene responsible only for draw order and visibility gating.
#ifdef HLE_PROF_SERIAL   // test aid: the HUD's parts, each run alone
        static uint32_t s_ui[3]; static int s_uiN;
        s_gpu[6] += gpu_prof_end(); gpu_prof_begin();
#endif
        if (!isVictory && boss_is_active(g_boss) && bossTitleFade <= 0.0f && g_boss->maxHealth > 0.0f) {
            boss_draw_ui(g_boss, viewport);
        }
#ifdef HLE_PROF_SERIAL
        s_ui[0] += gpu_prof_end(); gpu_prof_begin();
        { extern void character_ui_draw_health_bar(const char *, float, float); extern void character_ui_draw_stamina_bar(float);
          character_ui_draw_health_bar("Player", 1.0f, 0.0f); character_ui_draw_stamina_bar(1.0f); }
        s_ui[1] += gpu_prof_end(); gpu_prof_begin();
        character_ui_draw_c_buttons();
        s_ui[2] += gpu_prof_end(); gpu_prof_begin();
        if (++s_uiN == 120) { debugf("[uigpu] boss %.2f bars %.2f buttons %.2f\n", TICKS_TO_US(s_ui[0] / 120) / 1000.0, TICKS_TO_US(s_ui[1] / 120) / 1000.0, TICKS_TO_US(s_ui[2] / 120) / 1000.0); s_ui[0] = s_ui[1] = s_ui[2] = 0; s_uiN = 0; }
#else
        character_ui_draw();
#endif
        if (s_tower) tower_hud_draw();
    }
    if (s_towerDraft && !isDead) tower_draft_draw();

    // Slide-up overlay for boss title after fight starts (no fading)
    if (!cutsceneActive && !isEndScreen && bossTitleFade > 0.0f && g_boss) {
        rdpq_sync_pipe();
        rdpq_set_mode_standard();
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_combiner(RDPQ_COMBINER_FLAT);

        int barWidth = 195;
        int barHeight = 23;
        int barTop = 35;
        int barLeft = (SCREEN_WIDTH - barWidth) / 2;
        int slideDistance = 120; // pixels to move upward until off-screen
        float t = 1.0f - bossTitleFade; // 0 -> 1 as we slide up

        // Compute current Y positions while sliding upward
        int currentBarTop = barTop - (int)(slideDistance * t);
        int currentTextY = 50 - (int)(slideDistance * t);

        // Draw centered black bar behind the title (constant alpha)
        int barAlpha = 120;
        rdpq_set_prim_color(RGBA32(0, 0, 0, barAlpha));
        rdpq_fill_rectangle(barLeft, currentBarTop, barLeft + barWidth, currentBarTop + barHeight);

        // Draw centered title text (constant intensity while sliding up)
        rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
        rdpq_text_printf(&(rdpq_textparms_t){
            .align = ALIGN_CENTER,
            .width = SCREEN_WIDTH,
        }, FONT_UNBALANCED, 0, currentTextY, "%s", g_boss->name);
    }

    if (isEndScreen) {
        rdpq_sync_pipe();
        rdpq_set_mode_standard();
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_combiner(RDPQ_COMBINER_FLAT);

        if (isDead) {
            // Full-screen overlay with prompt to restart
            rdpq_set_prim_color(RGBA32(0, 0, 0, 140));
            rdpq_fill_rectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
            rdpq_set_prim_color(RGBA32(255, 255, 255, 255));

            rdpq_text_printf(&(rdpq_textparms_t){
                .align = ALIGN_CENTER,
                .width = SCREEN_WIDTH,
            }, FONT_UNBALANCED, 0, SCREEN_HEIGHT / 2 - 12, "%s", "You Died");
            if (s_tower) {
                rdpq_text_printf(&(rdpq_textparms_t){ .align = ALIGN_CENTER, .width = SCREEN_WIDTH, .style_id = FONT_STYLE_GOLD },
                                 FONT_UNBALANCED, 0, SCREEN_HEIGHT / 2 + 2, "%s", tower_death_summary());
            }

            // Delay restart prompt so rapid gameplay A-mashing doesn't instantly restart.
            if (deathRestartLockoutTimer >= DEATH_RESTART_LOCKOUT_S) {
                const char *label = "Restart";
                const int gap = 6;
                const int y = (SCREEN_HEIGHT / 2) + (s_tower ? 34 : 20);    // below the Tower's two summary lines

                // Source sprite is 64×64; scale down to match other UI button sizes.
                const float kTargetPx = 20.0f;
                int buttonW = button_prompt_a_icon_width(kTargetPx);
                int buttonH = button_prompt_a_icon_height(kTargetPx);
                int x0 = (SCREEN_WIDTH / 2) - 44;

                if (button_prompt_has_a_button()) {
                    int buttonX = x0;
                    int buttonY = y - (buttonH / 2) - 6;
                    button_prompt_draw_a_icon(buttonX, buttonY, kTargetPx);
                }

                // Text baseline aligned to match other UI codepaths (roughly icon vertical center)
                int textX = x0 + buttonW + gap;
                rdpq_set_prim_color(RGBA32(255, 255, 255, 255));
                rdpq_text_printf(&(rdpq_textparms_t){
                    .align = ALIGN_LEFT,
                    .width = SCREEN_WIDTH - textX,
                }, FONT_UNBALANCED, textX, y, "%s", label);
            }

            scene_draw_video_preroll_fade();
            return;
        }

        // Victory: Dark Souls–style title fade ("Enemy restored")
        {
            const float fadeIn = VICTORY_TITLE_FADEIN_S;
            const float hold = VICTORY_TITLE_HOLD_S;
            const float fadeOut = VICTORY_TITLE_FADEOUT_S;
            const float total = fadeIn + hold + fadeOut;

            float t = victoryTitleTimer;
            if (t < 0.0f) t = 0.0f;
            if (t > total) t = total;

            float a01 = 0.0f;
            if (t < fadeIn) {
                a01 = (fadeIn > 0.0f) ? (t / fadeIn) : 1.0f;
            } else if (t < fadeIn + hold) {
                a01 = 1.0f;
            } else {
                float u = (fadeOut > 0.0f) ? ((t - (fadeIn + hold)) / fadeOut) : 1.0f;
                if (u < 0.0f) u = 0.0f;
                if (u > 1.0f) u = 1.0f;
                a01 = 1.0f - u;
            }

            uint8_t textA = (uint8_t)fmaxf(0.0f, fminf(255.0f, a01 * 255.0f));
            uint8_t barA  = (uint8_t)fmaxf(0.0f, fminf(200.0f, a01 * 140.0f));

            if (!victoryTitleDone) {
                if (textA > 0) {
                    int barWidth = 250;
                    int barHeight = 28;
                    int barLeft = (SCREEN_WIDTH - barWidth) / 2;
                    int barTop = (SCREEN_HEIGHT / 2) - (barHeight / 2) - 2;

                    // Background: use dialog gradient sprite (fallback to solid bar if missing)
                    if (victoryTitleBgSprite) {
                        const int src_w = victoryTitleBgSurf.width;
                        const int src_h = victoryTitleBgSurf.height;
                        const float sx = (src_w > 0) ? ((float)barWidth  / (float)src_w) : 1.0f;
                        const float sy = (src_h > 0) ? ((float)barHeight / (float)src_h) : 1.0f;

                        rdpq_sync_pipe();
                        rdpq_set_mode_standard();
                        rdpq_mode_alphacompare(1);
                        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
                        rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
                        rdpq_set_prim_color(RGBA32(255, 255, 255, barA));
                        rdpq_tex_blit(&victoryTitleBgSurf, barLeft, barTop, &(rdpq_blitparms_t){
                            .scale_x = sx,
                            .scale_y = sy,
                        });
                    } else {
                        rdpq_set_prim_color(RGBA32(0, 0, 0, barA));
                        rdpq_fill_rectangle(barLeft, barTop, barLeft + barWidth, barTop + barHeight);
                    }

                    // Ensure a simple combiner for text after the sprite blit path.
                    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
                    rdpq_set_prim_color(RGBA32(255, 255, 255, textA));
                    rdpq_text_printf(&(rdpq_textparms_t){
                        .align = ALIGN_CENTER,
                        .width = SCREEN_WIDTH,
                    }, FONT_UNBALANCED, 0, SCREEN_HEIGHT / 2, "%s", "Enemy restored");
                }
            }
        }

        scene_draw_video_preroll_fade();
        return;
    }

    // Draw dialog on top of everything
    int height = 70;
    int width = 220;
    int x = (SCREEN_WIDTH - width) / 2;
    // bottom positioning
    if(cutsceneDialogActive)
    {
        int y = 240 - height - 10;
        dialog_controller_draw(false, x, y, width, height);
    }


    if(screenTransition)
    {
        if(cutsceneState == CUTSCENE_PHASE1_INTRO){
            display_utility_solid_black_transition(true, 100.0f);
        }
        else if(cutsceneState == CUTSCENE_PHASE1_FEAR){
            display_utility_solid_black_transition(false, 200.0f);
        }
    }

    // Video preroll fade-to-black must draw over every non-video path.
    scene_draw_video_preroll_fade();


    //msa_draw_debug(viewport);
#ifdef HLE_LOG_FPS
    {   // test aid: CPU ms per draw stage (submission blocks when the RSP queue is full)
        static const char *N[7] = { "env", "shadow+room", "knight", "boss+msa", "door+chains", "fx", "2d" };
        static uint32_t acc[8], n;
#ifdef HLE_PROF_SERIAL
        PROF_SERIAL_WAIT(6);
#endif
        s_prof[7] = TICKS_READ();
        for (int i = 0; i < 7; i++) acc[i] += s_prof[i + 1] - s_prof[i];
        if (++n == 120) {
            debugf("[draw]");
            for (int i = 0; i < 7; i++) debugf(" %s %.2f", N[i], TICKS_TO_US(acc[i] / n) / 1000.0);
            debugf("\n");
#ifdef HLE_PROF_SERIAL
            debugf("[gpu] ");
            for (int i = 0; i < 7; i++) { debugf(" %s %.2f", N[i], TICKS_TO_US(s_gpu[i] / n) / 1000.0); s_gpu[i] = 0; }
            debugf("\n");
#endif
            memset(acc, 0, sizeof acc); n = 0;
        }
    }
#endif
}

void scene_delete_environment(void)
{
    // --- DPLs ---
    if (mapDpl)        { rspq_block_free(mapDpl);        mapDpl = NULL; }
    if (pillarsDpl)    { rspq_block_free(pillarsDpl);    pillarsDpl = NULL; }
    if (pillarsFrontDpl) { rspq_block_free(pillarsFrontDpl); pillarsFrontDpl = NULL; }
    if (roomLedgeDpl)  { rspq_block_free(roomLedgeDpl);  roomLedgeDpl = NULL; }
    if (windowsDpl)    { rspq_block_free(windowsDpl);    windowsDpl = NULL; }
    if (chainsDpl)     { rspq_block_free(chainsDpl);     chainsDpl = NULL; }
    if (sunshaftsDpl)  { rspq_block_free(sunshaftsDpl);  sunshaftsDpl = NULL; }
    if (fogDoorDpl)    { rspq_block_free(fogDoorDpl);    fogDoorDpl = NULL; }
    if (roomFloorDpl)  { rspq_block_free(roomFloorDpl);  roomFloorDpl = NULL; }

    // --- Models ---
    if (mapModel)       { t3d_model_free(mapModel);       mapModel = NULL; }
    if (pillarsModel)   { t3d_model_free(pillarsModel);   pillarsModel = NULL; }
    if (pillarsFrontModel) { t3d_model_free(pillarsFrontModel); pillarsFrontModel = NULL; }
    if (roomLedgeModel) { t3d_model_free(roomLedgeModel); roomLedgeModel = NULL; }
    if (windowsModel)   { t3d_model_free(windowsModel);   windowsModel = NULL; }
    if (chainsModel)    { t3d_model_free(chainsModel);    chainsModel = NULL; }
    if (sunshaftsModel) { t3d_model_free(sunshaftsModel); sunshaftsModel = NULL; }
    if (fogDoorModel)   { t3d_model_free(fogDoorModel);   fogDoorModel = NULL; }
    if (roomFloorModel) { t3d_model_free(roomFloorModel); roomFloorModel = NULL; }

    // --- Matrices (malloc_uncached) ---
    if (mapMatrix)       { free_uncached(mapMatrix);       mapMatrix = NULL; }
    if (pillarsMatrix)   { free_uncached(pillarsMatrix);   pillarsMatrix = NULL; }
    if (pillarsFrontMatrix) { free_uncached(pillarsFrontMatrix); pillarsFrontMatrix = NULL; }
    if (roomLedgeMatrix) { free_uncached(roomLedgeMatrix); roomLedgeMatrix = NULL; }
    if (windowsMatrix)   { free_uncached(windowsMatrix);   windowsMatrix = NULL; }
    if (chainsMatrix)    { free_uncached(chainsMatrix);    chainsMatrix = NULL; }
    if (sunshaftsMatrix) { free_uncached(sunshaftsMatrix); sunshaftsMatrix = NULL; }
    if (fogDoorMatrix)   { free_uncached(fogDoorMatrix);   fogDoorMatrix = NULL; }
    if (roomFloorMatrix) { free_uncached(roomFloorMatrix); roomFloorMatrix = NULL; }
}

#ifdef HLE_TEST_ENDING
void scene_test_start_ending(void)     // test aid: straight to the ending
{
    cutsceneState = CUTSCENE_ENDING;
    scene_init_cutscene();
}
#endif

void scene_cleanup(void)
{
    gpu_wait();
    if (s_tower) { tower_end_run(); s_tower = false; s_towerDraft = false; }
    ending_free();
    sun_boss_free();
    dragon_boss_free();
    //collision_mesh_cleanup();
    scene_delete_environment();
    boss_ground_crush_cleanup();
    camera_reset();

    /*
     * Character lifetime is owned by main.c for this transitional pass.
     * Do not delete/free it during scene transitions.
     */
    if (g_boss) {
        boss_free(g_boss);
        g_boss = NULL;
    }

    blood_particles_fx_cleanup();
    dust_particles_fx_cleanup();
    particle_system_cleanup();
    msa_shutdown();
    lightning_fx_system_shutdown();
    fallen_knight_free();

    dialog_controller_free();
    audio_scene_unload_sfx();
    cutscene_manager_cleanup();
    button_prompt_cleanup();

    if (victoryTitleBgSprite) {
        sprite_free(victoryTitleBgSprite);
        victoryTitleBgSprite = NULL;
        surface_free(&victoryTitleBgSurf);
    }

    character_ui_cleanup();
    boss_ui_cleanup();
}