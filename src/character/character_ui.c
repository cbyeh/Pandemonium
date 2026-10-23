#include "character_ui.h"
#include "gpu_frame.h"

#include <libdragon.h>

#include "character.h"
#include "../utilities/game_time.h"
#include "../utilities/globals.h"

#include "../utilities/video_layout.h"

#define CHARACTER_UI_TRAIL_HOLD_TIME 0.35f
#define CHARACTER_UI_TRAIL_DECAY_RATE 0.6f

typedef struct {
    float lossTrail;
    float gainTrail;
    float lastRatio;
    float lossHold;
    float gainHold;
} CharacterUiBarTrailState;

static sprite_t* s_keySprite = NULL;
static sprite_t* s_skillMoonBladeSprite = NULL;
static sprite_t* s_skillHalfmoonSprite = NULL;
static sprite_t* s_skillCleaveSprite = NULL;

static surface_t s_keySurf = {0};
static surface_t s_skillMoonBladeSurf = {0};
static surface_t s_skillHalfmoonSurf = {0};
static surface_t s_skillCleaveSurf = {0};

static sprite_t* s_healthBottleSprite = NULL;
static surface_t s_healthBottleSurf = {0};

static float s_playerUiIntro = 1.0f;

static CharacterUiBarTrailState s_playerHealthBarState  = {1.0f, 1.0f, 1.0f, 0.0f, 0.0f};
static CharacterUiBarTrailState s_playerStaminaBarState = {1.0f, 1.0f, 1.0f, 0.0f, 0.0f};

static float character_ui_clampf(float x, float lo, float hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static void character_ui_reset_bar_trail(CharacterUiBarTrailState *s, float ratio)
{
    if (!s) return;

    ratio = character_ui_clampf(ratio, 0.0f, 1.0f);

    s->lossTrail = ratio;
    s->gainTrail = ratio;
    s->lastRatio = ratio;
    s->lossHold = 0.0f;
    s->gainHold = 0.0f;
}

static void character_ui_update_bar_trails(float ratio, CharacterUiBarTrailState *s)
{
    if (!s) return;

    ratio = character_ui_clampf(ratio, 0.0f, 1.0f);

    if (ratio < s->lastRatio) {
        // Just lost health/stamina.
        // Refresh loss hold and snap gain trail forward so stale healing
        // highlight does not reappear behind new damage/use.
        s->lossHold = CHARACTER_UI_TRAIL_HOLD_TIME;

        if (s->gainTrail < ratio) {
            s->gainTrail = ratio;
        }
    } else if (ratio > s->lastRatio) {
        // Just gained health/stamina.
        // Refresh gain hold and snap loss trail up to current.
        s->gainHold = CHARACTER_UI_TRAIL_HOLD_TIME;

        if (s->lossTrail < ratio) {
            s->lossTrail = ratio;
        }
    }

    s->lastRatio = ratio;

    // Clamp trails to valid sides of the current ratio.
    if (s->lossTrail < ratio) {
        s->lossTrail = ratio;
    }

    if (s->gainTrail > ratio) {
        s->gainTrail = ratio;
    }

    if (s->lossHold > 0.0f) {
        s->lossHold -= deltaTime;

        if (s->lossHold < 0.0f) {
            s->lossHold = 0.0f;
        }
    } else if (s->lossTrail > ratio) {
        s->lossTrail -= CHARACTER_UI_TRAIL_DECAY_RATE * deltaTime;

        if (s->lossTrail < ratio) {
            s->lossTrail = ratio;
        }
    }

    if (s->gainHold > 0.0f) {
        s->gainHold -= deltaTime;

        if (s->gainHold < 0.0f) {
            s->gainHold = 0.0f;
        }
    } else if (s->gainTrail < ratio) {
        s->gainTrail += CHARACTER_UI_TRAIL_DECAY_RATE * deltaTime;

        if (s->gainTrail > ratio) {
            s->gainTrail = ratio;
        }
    }
}

static void character_ui_load_sprite(sprite_t **sprite, surface_t *surface, const char *path)
{
    if (!sprite || !surface || !path) return;
    if (*sprite) return;

    *sprite = sprite_load(path);

    if (*sprite) {
        *surface = sprite_get_pixels(*sprite);
    } else {
        *surface = (surface_t){0};
    }
}

static rspq_block_t *s_keysBlock;     // the D-pad keys as last drawn (character_ui_draw_c_buttons)
static unsigned s_keysLook = ~0u;

static void character_ui_free_sprite(sprite_t **sprite, surface_t *surface)
{
    if (!sprite || !surface) return;

    if (*sprite) {
        sprite_free(*sprite);
        *sprite = NULL;
    }

    *surface = (surface_t){0};
}

void character_ui_init(void)
{
    s_playerUiIntro = 1.0f;

    character_ui_reset_bar_trail(&s_playerHealthBarState, 1.0f);
    character_ui_reset_bar_trail(&s_playerStaminaBarState, 1.0f);

    // D-pad keys (blank) with the skill icons and the potion drawn over them.
    s_keysLook = ~0u;   // recorded again with these
    character_ui_load_sprite(&s_keySprite, &s_keySurf, "rom:/ui/skills/dpad_key.rgba16.sprite");
    character_ui_load_sprite(&s_skillMoonBladeSprite, &s_skillMoonBladeSurf, "rom:/ui/skills/skill_moonblade.rgba16.sprite");
    character_ui_load_sprite(&s_skillHalfmoonSprite, &s_skillHalfmoonSurf, "rom:/ui/skills/skill_halfmoon.rgba16.sprite");
    character_ui_load_sprite(&s_skillCleaveSprite, &s_skillCleaveSurf, "rom:/ui/skills/skill_cleave.rgba16.sprite");

    // Health potion bottle icon.
    character_ui_load_sprite(&s_healthBottleSprite, &s_healthBottleSurf, "rom:/healthBottle.ia8.sprite");
}

void character_ui_cleanup(void)
{
    if (s_keysBlock) { gpu_wait(); rspq_block_free(s_keysBlock); s_keysBlock = NULL; }   // it draws the sprites below
    character_ui_free_sprite(&s_keySprite, &s_keySurf);
    character_ui_free_sprite(&s_skillMoonBladeSprite, &s_skillMoonBladeSurf);
    character_ui_free_sprite(&s_skillHalfmoonSprite, &s_skillHalfmoonSurf);
    character_ui_free_sprite(&s_skillCleaveSprite, &s_skillCleaveSurf);

    character_ui_free_sprite(&s_healthBottleSprite, &s_healthBottleSurf);

    s_playerUiIntro = 1.0f;

    character_ui_reset_bar_trail(&s_playerHealthBarState, 1.0f);
    character_ui_reset_bar_trail(&s_playerStaminaBarState, 1.0f);
}

void character_ui_reset(void)
{
    s_playerUiIntro = 1.0f;

    character_ui_reset_bar_trail(&s_playerHealthBarState, 1.0f);
    character_ui_reset_bar_trail(&s_playerStaminaBarState, 1.0f);
}

void character_ui_set_intro(float progress)
{
    s_playerUiIntro = character_ui_clampf(progress, 0.0f, 1.0f);
}

void character_ui_draw_health_bar(const char *name, float ratio, float flash)
{
    (void)name;

    ratio = character_ui_clampf(ratio, 0.0f, 1.0f);
    flash = character_ui_clampf(flash, 0.0f, 1.0f);

    rdpq_set_mode_standard();

#ifdef RDPQ_FOG_DISABLED
    rdpq_mode_fog(RDPQ_FOG_DISABLED);
#else
    rdpq_mode_fog(0);
#endif

    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);

    const int marginX = ui_safe_margin_x();
    const int marginY = ui_safe_margin_y();

    const int barWidth = 120;
    const int barHeight = 10;

    float p = s_playerUiIntro;
    int slideDist = 40;
    int yOffset = (int)((1.0f - p) * (float)slideDist);

    int left = marginX;
    int top = marginY + 4 - yOffset;
    int bottom = top + barHeight;
    int right = left + barWidth;

    // Background.
    rdpq_set_prim_color(RGBA32(35, 35, 35, 160));
    rdpq_fill_rectangle(left, top, right, bottom);

    // Inner empty region.
    rdpq_set_prim_color(RGBA32(10, 10, 10, 190));
    rdpq_fill_rectangle(left + 1, top + 1, right - 1, bottom - 1);

    character_ui_update_bar_trails(ratio, &s_playerHealthBarState);

    int red = 200 + (int)(55.0f * flash);
    int green = 35 + (int)(45.0f * flash);
    int blue = 35 + (int)(45.0f * flash);

    int fillRight = left + 2 + (int)((float)(barWidth - 4) * ratio);
    int lossRight = left + 2 + (int)((float)(barWidth - 4) * s_playerHealthBarState.lossTrail);
    int gainLeft = left + 2 + (int)((float)(barWidth - 4) * s_playerHealthBarState.gainTrail);

    // Recent-damage segment.
    if (lossRight > fillRight) {
        rdpq_set_prim_color(RGBA32(230, 200, 60, 220));
        rdpq_fill_rectangle(fillRight, top + 2, lossRight, bottom - 2);
    }

    // Health fill.
    if (fillRight > left + 2) {
        rdpq_set_prim_color(RGBA32(red, green, blue, 220));
        rdpq_fill_rectangle(left + 2, top + 2, fillRight, bottom - 2);
    }

    // Recent-heal highlight.
    if (gainLeft < fillRight) {
        rdpq_set_prim_color(RGBA32(240, 240, 240, 220));
        rdpq_fill_rectangle(gainLeft, top + 2, fillRight, bottom - 2);
    }

    // Light frame.
    rdpq_set_prim_color(RGBA32(210, 210, 210, 180));
    rdpq_fill_rectangle(left, top, right, top + 1);
    rdpq_fill_rectangle(left, bottom - 1, right, bottom);
    rdpq_fill_rectangle(left, top, left + 1, bottom);
    rdpq_fill_rectangle(right - 1, top, right, bottom);
}

void character_ui_draw_stamina_bar(float ratio)
{
    ratio = character_ui_clampf(ratio, 0.0f, 1.0f);

    rdpq_set_mode_standard();

#ifdef RDPQ_FOG_DISABLED
    rdpq_mode_fog(RDPQ_FOG_DISABLED);
#else
    rdpq_mode_fog(0);
#endif

    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);

    const int marginX = ui_safe_margin_x();
    const int marginY = ui_safe_margin_y();

    const int healthBarWidth = 120;
    const int healthBarHeight = 10;

    const int barWidth = (healthBarWidth * 3) / 4;
    const int barHeight = 6;

    float p = s_playerUiIntro;
    int slideDist = 40;
    int yOffset = (int)((1.0f - p) * (float)slideDist);

    // Sit flush against the bottom of the health bar so the borders share a line.
    int healthBarBottom = marginY + 4 + healthBarHeight - yOffset;

    int left = marginX;
    int top = healthBarBottom;
    int bottom = top + barHeight;
    int right = left + barWidth;

    // Background.
    rdpq_set_prim_color(RGBA32(35, 35, 35, 160));
    rdpq_fill_rectangle(left, top, right, bottom);

    // Inner empty region.
    rdpq_set_prim_color(RGBA32(10, 10, 10, 190));
    rdpq_fill_rectangle(left + 1, top + 1, right - 1, bottom - 1);

    character_ui_update_bar_trails(ratio, &s_playerStaminaBarState);

    int fillWidth = (int)((float)(barWidth - 4) * ratio);
    int lossWidth = (int)((float)(barWidth - 4) * s_playerStaminaBarState.lossTrail);
    int gainWidth = (int)((float)(barWidth - 4) * s_playerStaminaBarState.gainTrail);

    // Recently-used segment.
    if (lossWidth > fillWidth) {
        rdpq_set_prim_color(RGBA32(230, 200, 60, 220));
        rdpq_fill_rectangle(left + 2 + fillWidth, top + 2, left + 2 + lossWidth, bottom - 2);
    }

    // Stamina fill.
    // Require at least 2px before rendering so sub-pixel residue does not leave
    // a sliver when the bar is empty.
    if (fillWidth >= 2) {
        rdpq_set_prim_color(RGBA32(60, 200, 80, 220));
        rdpq_fill_rectangle(left + 2, top + 2, left + 2 + fillWidth, bottom - 2);
    }

    // Recently-regenerated highlight.
    if (gainWidth < fillWidth) {
        rdpq_set_prim_color(RGBA32(240, 240, 240, 220));
        rdpq_fill_rectangle(left + 2 + gainWidth, top + 2, left + 2 + fillWidth, bottom - 2);
    }

    // Light frame.
    rdpq_set_prim_color(RGBA32(210, 210, 210, 180));
    rdpq_fill_rectangle(left, top, right, top + 1);
    rdpq_fill_rectangle(left, bottom - 1, right, bottom);
    rdpq_fill_rectangle(left, top, left + 1, bottom);
    rdpq_fill_rectangle(right - 1, top, right, bottom);
}

static void character_ui_draw_icon(sprite_t *sprite, const surface_t *surf, int x, int y, float px)
{
    if (!sprite || surf->width <= 0) return;
    float scale = px / (float)((surf->width > surf->height) ? surf->width : surf->height);
    rdpq_sprite_blit(sprite, x, y, &(rdpq_blitparms_t){
        .scale_x = scale, .scale_y = scale, .cx = surf->width / 2, .cy = surf->height / 2,
    });
}

/** Skill slot: icon, darkened from the top while on cooldown, plus seconds left. */
/** The cooldown over a skill's icon: darkened from the top, plus seconds left. */
static void character_ui_draw_skill_cooldown(CharacterSkill skill, int x, int y, int keyPx)
{
    float ratio = character_get_skill_cooldown_ratio(skill);
    bool affordable = character_can_afford_skill(skill);
    if (ratio <= 0.0f && affordable) return;

    // Visible key face is ~80% of the sprite
    int half = (int)(keyPx * 0.40f);
    int top = y - half;
    int shadeH = (ratio > 0.0f) ? (int)(2 * half * ratio + 0.5f) : 2 * half;
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
    rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
    rdpq_set_prim_color(RGBA32(0, 0, 0, ratio > 0.0f ? 170 : 110));
    rdpq_fill_rectangle(x - half, top, x + half, top + shadeH);

    float secs = character_get_skill_cooldown(skill);
    if (secs > 0.0f) {
        rdpq_text_printf(&(rdpq_textparms_t){ .width = 2 * half, .align = ALIGN_CENTER },
                         FONT_UNBALANCED, x - half, y + 4, "%d", (int)ceilf(secs));
    }
}

void character_ui_draw_c_buttons(void)
{
    // Bottom-left D-pad diamond: skills on up/right/down, health potion on left.
    // This should be called by scene after the 3D/world pass, with the rest of
    // the 2D UI.
    if (!s_keySprite) return;

    // Target on-screen key size. Source sprites are 64x64.
    const int keyPx = 20;
    const int marginX = ui_safe_margin_x();
    const int marginY = ui_safe_margin_y();

    // Diamond spacing: the sprites have transparent padding, so this keeps the
    // visible key edges close together.
    int spacing = (int)(keyPx * 0.9f);
    int centerX = marginX + spacing + keyPx / 2;
    // Sits above the boss health bar + name plate row (bottom centre), which it
    // would otherwise overlap on the right key
    const int bossRowH = 26;
    int centerY = SCREEN_HEIGHT - marginY - bossRowH - spacing - keyPx / 2;

    int leftX  = centerX - spacing, leftY  = centerY;
    int rightX = centerX + spacing, rightY = centerY;
    int upX    = centerX,           upY    = centerY - spacing;
    int downX  = centerX,           downY  = centerY + spacing;

    // All four keys always show, so the potion reads as D-Left; a skill not usable (the Skills
    // setting, or not yet learned in a Tower run) is a dimmed blank key
    const struct { CharacterSkill s; sprite_t *spr; const surface_t *surf; int x, y; } keys[] = {
        { SKILL_MOON_BLADE_DANCE, s_skillMoonBladeSprite, &s_skillMoonBladeSurf, upX, upY },
        { SKILL_HALFMOON_SLASH,   s_skillHalfmoonSprite,  &s_skillHalfmoonSurf,  rightX, rightY },
        { SKILL_CRESCENT_CLEAVE,  s_skillCleaveSprite,    &s_skillCleaveSurf,    downX, downY },
    };
    int potionCount = character_get_health_potion_count();
    unsigned look = potionCount > 0 ? 8 : 0;
    for (int i = 0; i < 3; i++) if (character_skill_usable(keys[i].s)) look |= 1u << i;

    // The keys, icons and bottle change only with that: recorded once for it (64 px sprites
    // scaled down, each with its own mode, cost the RSP half a millisecond a frame). The
    // cooldowns go on top each frame (nothing of one key overlaps another)
    look |= (unsigned)marginX << 8 | (unsigned)marginY << 16;     // where it goes
    if (look != s_keysLook || !s_keysBlock) {
        if (s_keysBlock) { gpu_wait(); rspq_block_free(s_keysBlock); }
        gpu_block_begin();
        // the keys, then what goes on them, a mode each (no two keys overlap)
        rdpq_set_mode_standard();
        rdpq_mode_alphacompare(0);
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_filter(FILTER_BILINEAR);
        character_ui_draw_icon(s_keySprite, &s_keySurf, leftX, leftY, keyPx);
        for (int i = 0; i < 3; i++) character_ui_draw_icon(s_keySprite, &s_keySurf, keys[i].x, keys[i].y, keyPx);
        rdpq_mode_alphacompare(1);
        for (int i = 0; i < 3; i++)
            if (look & (1u << i)) character_ui_draw_icon(keys[i].spr, keys[i].surf, keys[i].x, keys[i].y, keyPx * 0.75f);
        if ((look & 7) != 7) {
            int half = (int)(keyPx * 0.40f);
            rdpq_set_mode_standard();
            rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
            rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
            rdpq_set_prim_color(RGBA32(0, 0, 0, 140));
            for (int i = 0; i < 3; i++)
                if (!(look & (1u << i))) rdpq_fill_rectangle(keys[i].x - half, keys[i].y - half, keys[i].x + half, keys[i].y + half);
        }
        // Only show the potion bottle + count while the player has potions.
        if ((look & 8) && s_healthBottleSprite && s_healthBottleSurf.width > 0 && s_healthBottleSurf.height > 0) {
            // Tint the IA8 bottle sprite red so it contrasts against the grey key.
            rdpq_set_mode_standard();
            rdpq_mode_alphacompare(1);
            rdpq_mode_combiner(RDPQ_COMBINER_TEX_FLAT);
            rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
            rdpq_mode_filter(FILTER_BILINEAR);
            rdpq_set_prim_color(RGBA32(220, 50, 50, 255));
            character_ui_draw_icon(s_healthBottleSprite, &s_healthBottleSurf, leftX, leftY, keyPx * 0.65f);
        }
        s_keysBlock = gpu_block_end();
        s_keysLook = look;
    }
    rspq_block_run(s_keysBlock);
    for (int i = 0; i < 3; i++)
        if (look & (1u << i)) character_ui_draw_skill_cooldown(keys[i].s, keys[i].x, keys[i].y, keyPx);

    if (potionCount <= 0) {
        return;
    }

    // Potion count, in the gap at the diamond's centre.
    {
        const int textX = leftX + keyPx / 2 - 1;
        const int textY = leftY + 4;

        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_set_prim_color(RGBA32(255, 255, 255, 255));

        rdpq_text_printf(NULL, FONT_UNBALANCED, textX, textY, "%d", potionCount);
    }
}

void character_ui_draw(void)
{
    float hp = character_get_health();
    float maxHp = character_get_max_health();
    float stamina = character_get_stamina();
    float maxStamina = character_get_max_stamina();

    float hpRatio = (maxHp > 0.0f) ? (hp / maxHp) : 0.0f;
    float staminaRatio = (maxStamina > 0.0f) ? (stamina / maxStamina) : 0.0f;

    // If your character module does not expose a damage flash value yet,
    // keep this at 0.0f. Later this should come from character visual feedback.
    float damageFlash = 0.0f;

    character_ui_draw_health_bar("Player", hpRatio, damageFlash);
    character_ui_draw_stamina_bar(staminaRatio);
    character_ui_draw_c_buttons();
}