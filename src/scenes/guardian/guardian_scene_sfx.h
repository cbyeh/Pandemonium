#ifndef SCENE_SFX_H
#define SCENE_SFX_H

typedef enum Scene1Sfx
{
    SCENE1_SFX_TITLE_WALK = 0,

    SCENE1_SFX_BOSS_SWING1, 
    SCENE1_SFX_BOSS_SWING2, 
    SCENE1_SFX_BOSS_SWING3, 
    SCENE1_SFX_BOSS_SWING4, 

    SCENE1_SFX_BOSS_SMASH1, 
    SCENE1_SFX_BOSS_SMASH2, 
    SCENE1_SFX_BOSS_SMASH3, 

    SCENE1_SFX_BOSS_LUNGE, 

    SCENE1_SFX_BOSS_LAND1, 
    SCENE1_SFX_BOSS_LAND2,

    SCENE1_SFX_BOSS_STEP1,

    SCENE1_SFX_DRAGON_ROAR,     // CC0, "Deep Monster Roar" by trazzz123 (OpenGameArt)

    // CC0, "80 CC0 RPG SFX" by rubberduck (OpenGameArt)
    SCENE1_SFX_SPELL_WAVE,
    SCENE1_SFX_SPELL_CAST,
    SCENE1_SFX_BLADE_SLASH,
    SCENE1_SFX_HEAL,
    SCENE1_SFX_FIRE,
    SCENE1_SFX_DARK_BEAM,
    SCENE1_SFX_DARK_NOVA,
    SCENE1_SFX_ROCK_BLAST,
    SCENE1_SFX_CHAIN1,
    SCENE1_SFX_CHAIN2,
    // The Bonus bosses' copies of the knight's skill sounds, at the Guardian's level (hostile/)
    SCENE1_SFX_SPELL_WAVE_BOSS,
    SCENE1_SFX_SPELL_CAST_BOSS,
    SCENE1_SFX_BLADE_SLASH_BOSS,
    SCENE1_SFX_HEAL_BOSS,
    SCENE1_SFX_SWING_BOSS,      // the knight's swing, for the Fallen Warrior

    // Character SFX
    SCENE1_SFX_CHAR_SWING1,
    
    SCENE1_SFX_CHAR_ATTACK_HIT1,
    SCENE1_SFX_CHAR_ATTACK_HIT2,
    SCENE1_SFX_CHAR_ATTACK_HIT3,
    SCENE1_SFX_CHAR_ATTACK_HIT4,
    SCENE1_SFX_CHAR_ATTACK_HIT5,
    SCENE1_SFX_CHAR_ATTACK_HIT6,

    SCENE1_SFX_CHAR_FOOTSTEP_RUN1,
    SCENE1_SFX_CHAR_FOOTSTEP_RUN2,
    SCENE1_SFX_CHAR_FOOTSTEP_RUN3,
    SCENE1_SFX_CHAR_FOOTSTEP_RUN4,

    SCENE1_SFX_CHAR_FOOTSTEP_WALK1,
    SCENE1_SFX_CHAR_FOOTSTEP_WALK2,
    SCENE1_SFX_CHAR_FOOTSTEP_WALK3,
    SCENE1_SFX_CHAR_FOOTSTEP_WALK4,

    SCENE1_SFX_CHAR_UMPH,
    SCENE1_SFX_ROLL_SWISH,      // generated: band-passed pink noise with a swell
    SCENE1_SFX_ARMOR_CLANK,     // CC0, rubberduck (metal_03)
    SCENE1_SFX_WAR_CRY,         // the grunt, deeper, with an echo

    SCENE1_SFX_COUNT
} Scene1Sfx;

// Every Bonus boss sound plays at this (times its own mix): the Guardian's level at a fight's
// usual range. The Guardian's sounds fall off with distance (0.72 near, ~0.6 at 40 away); the
// Bonus bosses' play as if near (their bodies are big), and their spell, fire, beam and chain
// sounds are the hostile/ copies (tools/boss_sfx_levels.py), as loud as the Guardian's own
#define BONUS_SFX_VOL 0.85f // TODO: Change this from scene to a entity specific later.

#endif