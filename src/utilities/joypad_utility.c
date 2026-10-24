#include <libdragon.h>

#include "joypad_utility.h"
#include "game_time.h"

joypad_inputs_t joypad;
joypad_buttons_t btn;
joypad_buttons_t rel;

static double rumbleStopTimeS = 0.0;
static bool rumbleEnabled = true;

static void drive_rumble(bool active)
{
    if (joypad_get_rumble_supported(JOYPAD_PORT_1)) {
        joypad_set_rumble_active(JOYPAD_PORT_1, active);
    }
}

void joypad_utility_init(void)
{
    joypad_init();
    joypad = joypad_get_inputs(JOYPAD_PORT_1);
    btn = joypad_get_buttons_pressed(JOYPAD_PORT_1);
    rel = joypad_get_buttons_released(JOYPAD_PORT_1);
    rumbleStopTimeS = 0.0;
    rumbleEnabled = true;

    drive_rumble(false);
}

static int s_layout = LAYOUT_CLASSIC;
static bool s_gameplay;

void joypad_set_layout(int layout) { s_layout = (layout >= 0 && layout < LAYOUT_COUNT) ? layout : LAYOUT_CLASSIC; }
int  joypad_get_layout(void) { return s_layout; }
void joypad_set_gameplay(bool on)
{
#ifdef HLE_AUTOPLAY
    if (on != s_gameplay) debugf("[pad] gameplay %d (layout %d)\n", on, s_layout);
#endif
    s_gameplay = on;
}
const char* joypad_skill_button(int dir)
{
    static const char *D[3] = { "D-up", "D-right", "D-down" }, *C[3] = { "C-up", "C-right", "C-down" };
    if (dir < 0 || dir > 2) return "";
    return s_layout == LAYOUT_SKILLS_ON_C ? C[dir] : D[dir];
}

const char* joypad_layout_name(int layout)
{
    static const char *N[LAYOUT_COUNT] = { "Classic", "A/B Swapped", "Skills on C" };
    return N[(layout >= 0 && layout < LAYOUT_COUNT) ? layout : 0];
}

// The game reads the classic buttons: a layout swaps the physical ones into them
static void remap(joypad_buttons_t *b)
{
    joypad_buttons_t o = *b;
    if (s_layout == LAYOUT_SWAPPED) { b->a = o.b; b->b = o.a; }
    if (s_layout == LAYOUT_SKILLS_ON_C) {
        b->d_up = o.c_up; b->d_down = o.c_down; b->d_left = o.c_left; b->d_right = o.c_right;
        b->c_up = o.d_up; b->c_down = o.d_down; b->c_left = o.d_left; b->c_right = o.d_right;
    }
}

#ifdef HLE_AUTOPLAY
void joypad_debug_remap(joypad_buttons_t *b) { remap(b); }    // test aid
#endif

void joypad_update(void)
{
    joypad_poll();
    joypad = joypad_get_inputs(JOYPAD_PORT_1);
    btn = joypad_get_buttons_pressed(JOYPAD_PORT_1);
    rel = joypad_get_buttons_released(JOYPAD_PORT_1);
    if (s_gameplay && s_layout != LAYOUT_CLASSIC) {
        joypad_buttons_t held = joypad.btn;     // a packed member: remapped through a copy
        remap(&held); joypad.btn = held;
        remap(&btn); remap(&rel);
    }
#ifdef HLE_AUTOPLAY
    extern void hle_autoplay(joypad_inputs_t *in, joypad_buttons_t *pressed);
    hle_autoplay(&joypad, &btn);
#endif
    if(joypad.stick_x < 10 && joypad.stick_x > -10)joypad.stick_x = 0;
    if(joypad.stick_y < 10 && joypad.stick_y > -10)joypad.stick_y = 0;

    bool shouldRumble = rumbleEnabled && (nowS < rumbleStopTimeS);

    // drive the desired state EVERY frame so the controller and our internal state never diverge
    // matches the per-frame rumble pump used in other N64 titles.
    drive_rumble(shouldRumble);

    if (!shouldRumble) {
        rumbleStopTimeS = 0.0;
    }
}

void joypad_rumble_pulse_seconds(float seconds)
{
    if (seconds <= 0.0f) return;
    if (!rumbleEnabled) return;

    double stopAt = nowS + (double)seconds;
    if (stopAt > rumbleStopTimeS) {
        rumbleStopTimeS = stopAt;
    }
}

void joypad_rumble_stop(void)
{
    rumbleStopTimeS = 0.0;
    drive_rumble(false);
}

void joypad_set_rumble_enabled(bool enabled)
{
    rumbleEnabled = enabled;

    if (!enabled) {
        joypad_rumble_stop();
    }
}

bool joypad_is_rumble_enabled(void)
{
    return rumbleEnabled;
}