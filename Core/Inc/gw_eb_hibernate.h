#ifndef _GW_EB_HIBERNATE_H_
#define _GW_EB_HIBERNATE_H_

/*
 * EarthBound STANDBY hibernation (sleep / resume).
 *
 * EarthBound on the G&W is a NATIVE port: the game's "where am I" lives on the
 * host C call stack, so a normal data-only emulator save-state is impossible.
 * Instead we core-dump the mutable RAM (the live RAM_EMU range + the in-use C
 * stack) to SD at a frame boundary, fully power down (STANDBY), and on the next
 * cold boot restore that RAM and longjmp back into the exact frame.
 *
 * This only works because the firmware is a single fixed-address build: every
 * saved return address / pointer is valid when reloaded by the IDENTICAL
 * binary. The snapshot is stamped with a build hash and rejected on mismatch.
 *
 * Flow:
 *   save    - POWER press sets hibernate_requested (gw_input.c); the EB loop
 *             calls eb_hibernate() at a frame boundary (game_main.c).
 *   wake    - a value in RTC backup register DR1 (survives STANDDBY) makes the
 *             launcher auto-launch EarthBound, which calls eb_hibernate_restore.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <setjmp.h>

/* Set by the input layer when POWER is pressed; consumed by the EB loop. */
extern volatile bool hibernate_requested;

/* Save side. Called from the EarthBound main loop at a frame boundary. On a
 * successful snapshot this enters STANDBY and never returns; it returns (with
 * hibernate_requested cleared) on the resume path and on any failure. */
void eb_hibernate(void);

/* Restore side. True when a STANDBY-surviving "hibernation pending" marker is
 * present in the RTC backup domain. */
bool eb_hibernate_pending(void);

/* Restore side. Called from app_main_earthbound after the normal EB setup
 * (rodata cached + odroid_system_init) when eb_hibernate_pending() is true.
 * eb_rodata/eb_rodata_len are THIS boot's cached-rodata address/length, used to
 * re-fixup the restored .noreloc pointer tables. Never returns on success
 * (longjmp into the resumed game); returns to fall through to a fresh start on
 * any failure. */
void eb_hibernate_restore(uint8_t *eb_rodata, uint32_t eb_rodata_len);

/* Record THIS session's cached-rodata address/length so the save side can stamp
 * them into the snapshot. Called from app_main_earthbound. */
void eb_hibernate_set_rodata(uint8_t *eb_rodata, uint32_t eb_rodata_len);

/* ---- retro-go savestate slots ----
 *
 * A savestate is the SAME RAM_EMU + C-stack core-dump as sleep/resume, just
 * written to the retro-go slot path instead of the .hib file (a native port has
 * no data-only state). The only wrinkle: the retro-go pause menu calls the
 * save/load hooks from deep inside common_emu_input_loop(), below the frame
 * boundary, so a snapshot taken there must resume into the game loop rather than
 * back into the menu. We solve that with a per-frame "resume anchor" captured
 * above the menu stack. */

/* Per-frame quiescent resume anchor. platform_input_poll() MUST do, at its very
 * top (above the pause-menu stack it can later descend into):
 *     if (setjmp(*eb_savestate_frame_jb()) != 0) return;   // resumed
 *     eb_savestate_arm_frame(<current SP>);
 * so a savestate taken from the menu resumes cleanly into host_process_frame(). */
jmp_buf *eb_savestate_frame_jb(void);
void eb_savestate_arm_frame(uint32_t sp);

/* Save the running game to `path` (a retro-go slot/temp file). Synchronous; the
 * game keeps running afterwards. Returns false if no frame anchor is armed yet
 * or the write fails. */
bool eb_savestate_save(const char *path);

/* Load a savestate slot. The live menu stack can't be overwritten in place, so
 * this stages `path` and reboots into the shared cold-boot restore (like a
 * hibernation wake). Never returns on success; returns false only when the slot
 * is missing or the staging write fails. */
bool eb_savestate_load(const char *path);

/* Restore a snapshot directly from `path` at startup (for the launcher's
 * load_state arg — already a fresh boot, so no reboot needed). Never returns on
 * success; returns to fall through to a fresh start on any failure. */
void eb_savestate_restore_path(const char *path, uint8_t *eb_rodata, uint32_t eb_rodata_len);

#ifdef __cplusplus
}
#endif

#endif /* _GW_EB_HIBERNATE_H_ */
