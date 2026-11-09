#ifndef CHARACTER_SKIN_H
#define CHARACTER_SKIN_H

#include <stdbool.h>
#include <t3d/t3d.h>
#include <t3d/t3dmodel.h>
#include <t3d/t3dskeleton.h>

// Cosmetic skins: recolours of the knight's materials plus optional cloth
// (a verlet cloak or hair) and a procedural hood. Saved with the settings.
typedef enum {
    SKIN_KNIGHT,
    SKIN_TEAL_CLOAK,
    SKIN_CRIMSON_CLOAK,
    SKIN_UMBER_CLOAK,
    SKIN_WOLF_KNIGHT,
    SKIN_BLACK_SWORDSMAN,
    SKIN_SCARLET_VALKYRIE,
    SKIN_NIGHTFARER,
    SKIN_COUNT
} CharacterSkinId;

const char* character_skin_name(int id);
int  character_skin_get(void);
void character_skin_set(int id);              // re-records the knight's draw block
void character_skin_set_blade(bool set, bool glow, uint8_t r, uint8_t g, uint8_t b, uint8_t er, uint8_t eg, uint8_t eb);   // the equipped sword's colour over the skin's (weapon.c), its slab's edges on the Black Swordsman

// Used by character.c
rspq_block_t* character_skin_record(const T3DModel *model, const T3DSkeleton *skel);
void character_skin_draw_extras(const T3DSkeleton *skel, const T3DMat4FP *modelMat, const float pos[3], float ground, float dt);   // ground: the floor the cloth stays above
void character_skin_reset_cloth(void);
void character_skin_bind_block(rspq_block_t **block);

#endif
