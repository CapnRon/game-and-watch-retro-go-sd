/*
 * G&W input platform — translates retro-go's odroid_gamepad_state_t into
 * SNES PAD_* bits.
 *
 * Two G&W hardware variants:
 *   Mario unit  (6 buttons): UP/DOWN/LEFT/RIGHT + A + B
 *   Zelda unit (12 buttons): + X + Y + START + SELECT + GAME + TIME
 *
 * Detected at runtime via get_ofw_is_mario(). The Mario unit chord-encodes
 * the missing buttons using the GAME (volume) modifier — same scheme as
 * the zelda3 port.
 */

#include "platform/platform.h"
#include "pad.h"
#include "odroid_input.h"
#include "gw_ofw.h"

/* Defined in main_earthbound.c; populated by us each frame. */
extern odroid_gamepad_state_t eb_joystick;

bool platform_headless = false;
bool platform_skip_intro = false;
int platform_max_frames = 0;

static uint16_t pad_state;
static uint16_t pad_prev;
static uint16_t aux_state;
static bool quit_requested;

bool platform_input_init(void)
{
    pad_state = 0;
    pad_prev = 0;
    aux_state = 0;
    quit_requested = false;
    return true;
}

void platform_input_shutdown(void) {}

void platform_input_poll(void)
{
    odroid_input_read_gamepad(&eb_joystick);

    pad_prev = pad_state;

    uint16_t pad = 0;
    if (eb_joystick.values[ODROID_INPUT_UP])    pad |= PAD_UP;
    if (eb_joystick.values[ODROID_INPUT_DOWN])  pad |= PAD_DOWN;
    if (eb_joystick.values[ODROID_INPUT_LEFT])  pad |= PAD_LEFT;
    if (eb_joystick.values[ODROID_INPUT_RIGHT]) pad |= PAD_RIGHT;
    if (eb_joystick.values[ODROID_INPUT_A])     pad |= PAD_A;
    if (eb_joystick.values[ODROID_INPUT_B])     pad |= PAD_B;

    bool game_mod = eb_joystick.values[ODROID_INPUT_START];
    if (!get_ofw_is_mario()) {
        if (eb_joystick.values[ODROID_INPUT_SELECT]) pad |= PAD_X;
        if (eb_joystick.values[ODROID_INPUT_Y])      pad |= PAD_Y;
        if (eb_joystick.values[ODROID_INPUT_X])      pad |= PAD_START;
    } else {
        if (game_mod && eb_joystick.values[ODROID_INPUT_B]) pad |= PAD_X;
        if (eb_joystick.values[ODROID_INPUT_SELECT])        pad |= PAD_Y;
        if (game_mod && eb_joystick.values[ODROID_INPUT_A]) pad |= PAD_START;
    }

    pad_state = pad;
}

uint16_t platform_input_get_pad(void)
{
    return pad_state;
}

uint16_t platform_input_get_pad_new(void)
{
    return pad_state & ~pad_prev;
}

uint16_t platform_input_get_aux(void)
{
    return aux_state;
}

bool platform_input_quit_requested(void)
{
    return quit_requested;
}

void platform_request_quit(void)
{
    quit_requested = true;
}
