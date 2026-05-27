/*
 * G&W input platform — translates retro-go's odroid_gamepad_state_t into
 * SNES PAD_* bits, and runs the retro-go macro/menu loop each frame.
 *
 * EarthBound's button map:
 *   - SNES L (auto-check)  on G&W A
 *   - SNES B (cancel/dash) on G&W B
 *   - SNES X (town map)    on G&W GAME
 *   - FPS overlay toggle   on G&W TIME (via AUX_FPS_TOGGLE → upstream)
 *   - G&W PAUSE/SET        retro-go menu + chord modifier (save state, etc.)
 *
 * SNES A is unmapped: L's auto-check interacts with the nearest object
 * regardless of facing, so it strictly supersedes A in normal play.
 * SNES Y, Select, and R are unmapped — not needed for the buttons available
 * on a Mario unit.
 *
 * The same mapping works on both Mario and Zelda variants, so there's no
 * get_ofw_is_mario() branch.
 *
 * common_emu_input_loop() runs after the gamepad read and before the SNES
 * translation. When PAUSE/SET-based chords fire (save state, volume,
 * brightness, …), common_emu_input_loop zeros the joystick so the consumed
 * inputs don't leak into the SNES PAD_* bits or the aux bitmask.
 *
 * The aux bitmask drives upstream's debug hotkeys (game_main.c). We only
 * expose AUX_FPS_TOGGLE on TIME; the multi-line FPS/logic/render/idle
 * overlay it triggers is drawn by upstream via a scanline-stamp callback.
 * game_main.c computes the rising edge itself (aux_new = aux & ~aux_prev),
 * so we just report the held state.
 */

#include "platform/platform.h"
#include "pad.h"
#include "common.h"
#include "odroid_input.h"
#include "odroid_overlay.h"

/* Defined in main_earthbound.c; populated by us each frame. */
extern odroid_gamepad_state_t eb_joystick;

bool platform_headless = false;
bool platform_skip_intro = false;
int platform_max_frames = 0;

static uint16_t pad_state;
static uint16_t pad_prev;
static uint16_t aux_state;
static bool quit_requested;

/* No game-specific options yet — the standard retro-go pause-menu entries
 * (volume, brightness, save/load, exit, …) are all that's exposed. */
static odroid_dialog_choice_t eb_game_options[] = {
    ODROID_DIALOG_CHOICE_LAST,
};

/* Repaint hook for the pause menu's background. EB doesn't expose a "render
 * the current PPU state on demand" API the way zelda3's DrawPpuFrame does,
 * so menu backgrounds stay as whatever was last in the LCD buffer. The next
 * platform_video_send_scanline pass after menu dismissal repaints the game. */
static void eb_repaint(void) {}

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

    /* Must run BEFORE the PAD_* translation: when a PAUSE/SET chord fires,
     * common_emu_input_loop memsets the joystick to 0 to keep the consumed
     * inputs from being interpreted as SNES button presses. */
    common_emu_input_loop(&eb_joystick, eb_game_options, &eb_repaint);

    pad_prev = pad_state;

    uint16_t pad = 0;
    if (eb_joystick.values[ODROID_INPUT_UP])     pad |= PAD_UP;
    if (eb_joystick.values[ODROID_INPUT_DOWN])   pad |= PAD_DOWN;
    if (eb_joystick.values[ODROID_INPUT_LEFT])   pad |= PAD_LEFT;
    if (eb_joystick.values[ODROID_INPUT_RIGHT])  pad |= PAD_RIGHT;
    if (eb_joystick.values[ODROID_INPUT_A])      pad |= PAD_L;
    if (eb_joystick.values[ODROID_INPUT_B])      pad |= PAD_B;
    if (eb_joystick.values[ODROID_INPUT_START])  pad |= PAD_X;

    /* TIME → AUX_FPS_TOGGLE. PAUSE/SET+TIME (speedup chord) was already
     * consumed by common_emu_input_loop above, so TIME only reaches here
     * when pressed standalone. */
    aux_state = eb_joystick.values[ODROID_INPUT_SELECT] ? AUX_FPS_TOGGLE : 0;

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
