/*
 * G&W input platform — translates retro-go's odroid_gamepad_state_t into
 * SNES PAD_* bits.
 *
 * EarthBound's button map:
 *   - SNES L (auto-check)  on G&W A
 *   - SNES B (cancel/dash) on G&W B
 *   - SNES X (town map)    on G&W TIME
 *
 * SNES A is unmapped: L's auto-check interacts with the nearest object
 * regardless of facing, so it strictly supersedes A in normal play.
 * SNES Y, Select, Start, and R are unmapped — not needed for the buttons
 * available on a Mario unit.
 *
 * The same mapping works on both Mario and Zelda variants, so there's no
 * get_ofw_is_mario() branch. G&W PAUSE/SET (ODROID_INPUT_VOLUME) is left
 * alone for retro-go's launcher chord (save states, exit, etc.).
 */

#include "platform/platform.h"
#include "pad.h"
#include "odroid_input.h"

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
    if (eb_joystick.values[ODROID_INPUT_UP])     pad |= PAD_UP;
    if (eb_joystick.values[ODROID_INPUT_DOWN])   pad |= PAD_DOWN;
    if (eb_joystick.values[ODROID_INPUT_LEFT])   pad |= PAD_LEFT;
    if (eb_joystick.values[ODROID_INPUT_RIGHT])  pad |= PAD_RIGHT;
    if (eb_joystick.values[ODROID_INPUT_A])      pad |= PAD_L;
    if (eb_joystick.values[ODROID_INPUT_B])      pad |= PAD_B;
    if (eb_joystick.values[ODROID_INPUT_SELECT]) pad |= PAD_X;

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
