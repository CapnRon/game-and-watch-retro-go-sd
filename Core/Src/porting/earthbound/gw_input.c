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
#include "gw_eb_hibernate.h"

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

/* Repaint hook for the pause menu's background: re-render the current (frozen)
 * PPU frame into the launcher's active framebuffer so the menu's darken + dialog
 * land over the live game rather than a black background. Implemented in
 * gw_video.c, which owns the framebuffer/scanline plumbing. */
extern void eb_video_repaint_active(void);
static void eb_repaint(void) { eb_video_repaint_active(); }

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
    /* Quiescent resume anchor for retro-go savestates. The pause-menu save/load
     * hooks fire from deep inside common_emu_input_loop() below — but a restored
     * savestate must resume into the game loop, not back into the menu. So we
     * record the restore context HERE, above the menu stack, every frame. On a
     * loaded-state cold boot the restore longjmps back to this setjmp; we just
     * return into host_process_frame(), which runs the resumed frame. (The
     * captured stack region [SP, _estack) is the game's call chain; the menu it
     * later descends into lives at lower addresses and is never part of it.) */
    if (setjmp(*eb_savestate_frame_jb()) != 0) {
        return;
    }
    {
        uint32_t sp;
        __asm volatile("mov %0, sp" : "=r"(sp));
        eb_savestate_arm_frame(sp & ~0x7u);
    }

    odroid_input_read_gamepad(&eb_joystick);

    /* Standalone POWER → STANDBY hibernation. Intercept it here, BEFORE
     * common_emu_input_loop, and consume the bit so the common layer's own
     * (no-op-for-EB) power/sleep path doesn't also fire. The actual snapshot is
     * taken at the next frame boundary in wait_for_vblank(), where the stack
     * holds only game frames. POWER+PAUSE/SET stays with the common layer. */
    if (eb_joystick.values[ODROID_INPUT_POWER] &&
        !eb_joystick.values[ODROID_INPUT_VOLUME]) {
        hibernate_requested = true;
        eb_joystick.values[ODROID_INPUT_POWER] = 0;
    }

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
    /* STANDBY hibernation is taken here rather than via an edit to upstream's
     * wait_for_vblank(): this port-specific quit hook is already polled at every
     * frame boundary (and from the deep battle/text/menu loops that call it
     * directly), which is exactly where we want a quiescent snapshot point.
     * platform_input_poll() sets the flag when POWER is pressed; eb_hibernate()
     * snapshots RAM to SD and enters STANDBY (never returns), or returns here on
     * the resume/failure path with the flag already cleared. */
    if (hibernate_requested)
        eb_hibernate();

    return quit_requested;
}

void platform_request_quit(void)
{
    quit_requested = true;
}
