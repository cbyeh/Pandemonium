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

    // The knight's skills and the Bonus bosses' spells (the Dark Fantasy Soulslike pack; the chains CC0, rubberduck)
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
    SCENE1_SFX_HOLY_BOSS,
    SCENE1_SFX_SLAM_BOSS,       // the Fallen Warrior's Holy Slam and triangle slam eruption
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
    SCENE1_SFX_ROLL_SWISH,
    SCENE1_SFX_ARMOR_CLANK,
    SCENE1_SFX_WAR_CRY,         // the grunt, deeper, with an echo
    SCENE1_SFX_DRAGON_GROWL,    // the roar slowed, low-passed and rumbling (Shadowborne's pacing)

    // Each Bonus boss's fire its own (SCENE1_SFX_FIRE: the Fallen Warrior's), and Shadowborne's wings
    SCENE1_SFX_FIRE_SUN,
    SCENE1_SFX_FIRE_DRAGON,     // Shadowborne's fireballs and fire rain
    SCENE1_SFX_BREATH,          // Shadowborne's breath: one long torrent, heard from the stream
    SCENE1_SFX_WING_FLAP,       // a beat of its wings, in the air
    SCENE1_SFX_WING_FLYBY,      // a pass overhead

    // The knight's skills' own, and the Fallen Warrior's: the knight's sounds, deeper and heavier
    SCENE1_SFX_MBD_X,           // Moon Blade Dance: each X
    SCENE1_SFX_CLEAVE_BEAM,     // Crescent Cleave: each blade
    SCENE1_SFX_FW_SLICE,        // the Fallen Warrior's Moon Blade: each slice
    SCENE1_SFX_FW_ROLL,
    SCENE1_SFX_FW_STEP1,
    SCENE1_SFX_FW_STEP2,
    SCENE1_SFX_FW_STEP3,
    SCENE1_SFX_FW_STEP4,
    SCENE1_SFX_FW_HURT,         // its grunt as it staggers
    // The Shackled Sun's own
    // (sounds that play together from one place are mixed into one: tools/pack_sfx.py)
    SCENE1_SFX_SUN_CAST,        // its cast (the Guardian's swing 2, from its attack)
    SCENE1_SFX_SUN_SWELL,       // Flare's and Corona's charge
    SCENE1_SFX_SUN_BURST,       // their ring of fire: the boom and the fire
    SCENE1_SFX_SUN_RUSH,        // a ram or dive (its cast and the rush), heard from it as it moves
    SCENE1_SFX_SUN_ECLIPSE,     // the eclipse and its roar
    SCENE1_SFX_SUN_BREAK,       // breaking free: the boom, its roar, the chains
    SCENE1_SFX_SUN_LASH,        // the lash's and sweep's wind-ups: its cast and a chain
    SCENE1_SFX_SUN_SWEEP,
    SCENE1_SFX_SUN_RAIN,        // the rain's tell: its cast and both chains
    SCENE1_SFX_DRAGON_RISE,     // Shadowborne's hover: its cast and roar
    SCENE1_SFX_DRAGON_STOMP,    // its stomp: the boom and its footfall
    SCENE1_SFX_BEAM_HUM,        // its beam while it's on, heard from the beam (dark_beam is its start)

    SCENE1_SFX_COUNT
} Scene1Sfx;

// Every Bonus boss sound plays at this (times its own mix), with the Guardian's distance
// falloff (0.72 near, ~0.6 at 40 away); their spell, fire, beam and chain sounds are the
// hostile/ copies (tools/boss_sfx_levels.py), as loud as the Guardian's own
#define BONUS_SFX_VOL 1.0f   // the Bonus bosses as the Guardian: his volume and distance falloff (their copies levelled to his loudness)

#endif