/*
 * EarthBound retro-go entry point.
 *
 * Unlike zelda3/main_zelda3.c, this file does NOT drive the per-frame
 * game loop. Upstream's earthbound_main() (port/gw_retro_go/main.c, renamed
 * from `main` via earthbound_redefines) contains the entire game loop and
 * calls platform_*() hooks each frame. Those hooks are implemented in
 * sibling gw_*.c files.
 *
 * Responsibilities here:
 *   1. Cache the rodata blob from SD into the extflash cache.
 *   2. Run PatchCodeRodataOffset to fix up pointer tables (which the build
 *      placed in .noreloc → RAM via EB_NORELOC and EBASSET_TABLE_ATTR).
 *   3. Initialize odroid_system (appid, audio sample rate, savestate hooks).
 *   4. Hand control to upstream.
 */

#include <odroid_system.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "main.h"
#include "gw_lcd.h"
#include "gw_linker.h"
#include "gw_buttons.h"
#include "gw_malloc.h"
#include "gw_ofw.h"
#include "gw_sleep.h"

#include "stm32h7xx_hal.h"

#include "common.h"
#include "rom_manager.h"
#include "appid.h"

#include "main_earthbound.h"

#include "game_main.h"   /* host_request_capture/load/status — native savestate engine */

#pragma GCC optimize("Ofast")

/* Matches the SPC700/DSP native rate. eb_audio_pump produces 534
 * stereo samples per game frame, which is the half-buffer SAI consumes
 * per 16.69 ms at 32 kHz mono — frame and DMA cadence stay aligned. */
#define EB_AUDIO_SAMPLE_RATE   (32000)

/* Shared with gw_input.c — the platform layer reads from this each frame. */
odroid_gamepad_state_t eb_joystick;

/* Linker-defined start of .rodata_earthbound. Used as the patcher's base —
 * NOT 0xCAFE0000 (that's .rodata_zelda3's base). Other ports' rodata regions
 * may precede EB's, so the EB base is layout-dependent and must be a symbol. */
extern void *__rodata_earthbound_start__[];

/* PatchCodeRodataOffset — walks RAM_EMU looking for pointers that fall inside
 * .rodata_earthbound's linker VMA range and rewrites them to the actual
 * runtime address where odroid_overlay_cache_file_in_flash placed the rodata
 * blob. Skips the main_earthbound.o code window (between
 * _EARTHBOUND_MAIN_CODE_*) because that code's rodata lives in
 * .overlay_earthbound itself, not in .rodata_earthbound.
 *
 * NON-OBVIOUS: this scans RAM_EMU ONLY. .rodata_earthbound loads to a runtime
 * address that VARIES per boot, so every literal-pool pointer into it must be
 * rewritten here or it keeps the 0xCAFF.. *link* address and faults on first
 * use (symptom: BSOD with PC inside newlib _vfiprintf_r — a garbage printf
 * format-string pointer). Therefore if you ever relocate an EarthBound .o's
 * code/data OUT of .overlay_earthbound (e.g. into ITCM), it is no longer
 * scanned and you must run PatchRodataRange() over the new location too, with
 * the SAME base/offset. (Tried & reverted: ppu_render.o → ITCM — link/runtime
 * worked once patched, but gave no FPS gain; EB render is compute-bound.) */
static void PatchRodataRange(uint32_t *ptr, uint32_t *end,
                             uint32_t rodata_base, int32_t offset,
                             uint32_t rodata_length, bool skip_main_code)
{
    while (ptr < end) {
        bool in_skip_window = skip_main_code &&
            (ptr >= (uint32_t *)&_EARTHBOUND_MAIN_CODE_START) &&
            (ptr <= (uint32_t *)&_EARTHBOUND_MAIN_CODE_END);
        if (!in_skip_window) {
            uint32_t value = *ptr;
            if ((value >= rodata_base) && (value < rodata_base + rodata_length)) {
                *ptr = value + offset;
                wdog_refresh();
            }
        }
        ptr++;
    }
}

static void PatchCodeRodataOffset(uint8_t *rodata, uint32_t rodata_length)
{
    uint32_t rodata_base = (uint32_t)__rodata_earthbound_start__;
    int32_t offset = (uint32_t)rodata - rodata_base;

    printf("eb rodata = %p base = 0x%08lX offset = 0x%08lX length = %lu\n",
           rodata, rodata_base, offset, (unsigned long)rodata_length);

    /* RAM_EMU: overlay code + bss. Skip the main_earthbound.o code window
     * because that code's local rodata lives in .overlay_earthbound itself
     * (not in .rodata_earthbound) and patching its instructions would risk
     * corrupting code bytes that coincidentally match the rodata VMA range. */
    PatchRodataRange((uint32_t *)__RAM_EMU_START__,
                     (uint32_t *)&_OVERLAY_EARTHBOUND_BSS_END,
                     rodata_base, offset, rodata_length, /*skip_main_code=*/true);
}

/* Defined in gw_video.c: re-render the current (frozen) PPU frame into the
 * launcher's active framebuffer. Used for the pause-menu background and here. */
extern void eb_video_repaint_active(void);

static void *Screenshot(void)
{
    /* Render the live (frozen) PPU frame into the launcher's active buffer so
     * retro-go's cover/menu capture sees the running game rather than black.
     * Same read-only path the pause-menu background uses; the caller has the
     * game paused, so the PPU state is stable. */
    lcd_wait_for_vblank();
    lcd_clear_active_buffer();
    eb_video_repaint_active();
    return lcd_get_active_buffer();
}

/* retro-go savestate slots now ride the upstream native savestate engine
 * (state_dump.c): a structured snapshot of the game-state structs, written/read
 * through our platform_savestate_* backend (gw_savestate.c). Both hooks run from
 * deep inside common_emu_input_loop() — one level below the root-loop boundary —
 * so they REQUEST the action rather than perform it: host_request_capture/load()
 * set a pending flag, the loop free-runs any in-flight blocking helper to the
 * root, and host_root_boundary() (game_main.c) does the torn-safe save / in-place
 * load. No reboot, no per-frame stack anchor. We point the backend at the
 * launcher's chosen slot path first via eb_savestate_set_base(). */
static bool eb_LoadState(char *savePathName)
{
    eb_savestate_set_base(savePathName);
    host_request_load();   /* serviced in-place at the next root boundary */
    return true;
}

static bool eb_SaveState(char *savePathName)
{
    eb_savestate_set_base(savePathName);
    host_request_capture();  /* torn-safe write at the next root boundary */
    return true;
}

/* EarthBound writes its 7680-byte save buffer straight to SD whenever the
 * player saves in-game (save_game() -> platform_save_write; see gw_save.c), so
 * there is no battery-backed SRAM image held in RAM to flush. retro-go calls
 * this hook on exit/sleep; for EB it is intentionally a no-op. */
static void eb_SramSave(void)
{
}

extern int earthbound_main(void);

/* ---- STANDBY sleep / resume ------------------------------------------------
 *
 * "Sleep" is a native savestate written to a private slot plus a full
 * power-down; "resume" (below, in app_main_earthbound) is a native in-process
 * load queued at cold boot. This replaces the old gw_eb_hibernate.c glue: the
 * capture is requested from the per-frame input poll (gw_input.c -> here, so
 * blocking helpers unwind to the root boundary), and the power-down happens at
 * earthbound_main()'s root boundary via platform_root_boundary() (the generic
 * host root-boundary hook; STANDBY is its only consumer today).
 */
#define EB_SLEEP_PATH     "/saves/EarthBound.sleep"    /* private; not a user slot */
#define EB_HIB_RTC_MAGIC  0x45424948u                  /* 'HIBE' in RTC_BKP_DR1, survives STANDBY */
#define EB_HIB_RTC_REG    RTC_BKP_DR1

/* Set true by gw_input.c when POWER is pressed; one-shot per sleep attempt. */
static volatile bool s_standby_requested;

void eb_request_standby(void)
{
    if (s_standby_requested)
        return;   /* request already in flight — don't restart the capture */
    /* Point the savestate backend at the private sleep snapshot, then request a
     * torn-safe capture. host_request_capture() flags the in-flight blocking
     * helpers to free-run to the root boundary, where the engine writes it. */
    eb_savestate_set_base(EB_SLEEP_PATH);
    host_request_capture();
    s_standby_requested = true;
}

void platform_root_boundary(void)
{
    if (!s_standby_requested)
        return;

    switch (host_capture_status()) {
    case HOST_CAPTURE_COMMITTED:
        /* Durably on the card. Mark EarthBound as the startup app so the launcher
         * auto-relaunches it on wake, arm the STANDBY-surviving marker, and power
         * down. Never returns; we come back via cold boot + the resume path. */
        odroid_settings_StartupFile_set(ACTIVE_FILE);
        odroid_settings_commit();
        HAL_RTCEx_BKUPWrite(&hrtc, EB_HIB_RTC_REG, EB_HIB_RTC_MAGIC);
        GW_EnterDeepSleep(true, NULL, NULL);
        s_standby_requested = false;   /* unreachable; keep state sane */
        break;
    case HOST_CAPTURE_FAILED:
        /* Capture abandoned (e.g. an indefinite input-wait never unwound) or slot
         * I/O failed. Abort sleep and keep running; the prior snapshot, if any, is
         * left intact by the engine's ping-pong write. */
        s_standby_requested = false;
        break;
    default: /* HOST_CAPTURE_PENDING — still unwinding to the root boundary */
        break;
    }
}

int app_main_earthbound(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)save_slot;

    printf("EarthBound start\n");
    ram_start = (uint32_t)&_OVERLAY_EARTHBOUND_BSS_END;

    /* Wipe the launcher's last frame out of both framebuffers — otherwise
     * the retro-go menu shows through any pixels EB hasn't written yet
     * (force-blank frames, transitions, etc.). */
    lcd_clear_buffers();

    /* Cache the rodata blob (3 MB of INCBIN'd assets + compiled const tables)
     * from SD into the extflash round-robin cache. */
    uint32_t eb_rodata_length = 0;
    uint8_t *eb_rodata = odroid_overlay_cache_file_in_flash(
        "/roms/homebrew/earthbound.ro", &eb_rodata_length, false);
    if (eb_rodata == NULL) {
        printf("Missing /roms/homebrew/earthbound.ro file\n");
    }

    /* Fix up RAM pointer tables (.noreloc) to point at the actual extflash
     * location of the rodata blob. Native savestate serializes game-state structs
     * (not raw RAM), and load applies on top of this freshly-patched boot, so no
     * rodata re-stamping is needed on resume — this one pass is sufficient. */
    PatchCodeRodataOffset(eb_rodata, eb_rodata_length);

    odroid_system_init(APPID_EARTHBOUND, EB_AUDIO_SAMPLE_RATE);
    odroid_system_emu_init(&eb_LoadState, &eb_SaveState, &Screenshot,
                           NULL, NULL, &eb_SramSave);

    /* Start paused into the in-game retro-go menu (same as zelda3/smw). The
     * launcher requests this on power-on resume (rg_main.c: emulator_start(...,
     * start_paused=true)) so the device comes back paused over the loaded state.
     * pause_after_frames is decremented in common_emu_input_loop() — which
     * gw_input.c's platform_input_poll() calls every frame — and trips
     * pause_pressed, opening the pause menu. Audio is muted so the 2 lead-in
     * frames are silent; the game menu unmutes on exit (odroid_overlay_game_menu).
     * The native savestate load queued below is applied at the first root-loop
     * boundary, so by the time the menu opens it darkens the loaded frame. */
    if (start_paused) {
        common_emu_state.pause_after_frames = 2;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }

    /* Queue a native savestate load to be serviced at earthbound_main()'s first
     * root-loop boundary (it free-runs in-process; no reboot). Two cold-boot
     * sources, both pointing the backend at their slot before requesting:
     *   1. STANDBY sleep resume — the RTC wake marker points the backend at the
     *      private sleep snapshot (marker cleared first, so a crash mid-load
     *      yields a clean normal launch on the next boot rather than a loop).
     *   2. Launcher "load slot at startup" — the chosen ODROID_PATH_SAVE_STATE.
     * On a missing/invalid slot the load silently fails at the root boundary and
     * the game falls through to a normal fresh start. */
    if (HAL_RTCEx_BKUPRead(&hrtc, EB_HIB_RTC_REG) == EB_HIB_RTC_MAGIC) {
        HAL_RTCEx_BKUPWrite(&hrtc, EB_HIB_RTC_REG, 0);
        eb_savestate_set_base(EB_SLEEP_PATH);
        host_request_load();
    } else if (load_state) {
        char *p = odroid_system_get_path(ODROID_PATH_SAVE_STATE, ACTIVE_FILE->path);
        if (p) {
            eb_savestate_set_base(p);
            host_request_load();
            free(p);
        }
    }

    /* Hand off to upstream — earthbound_main() runs the game loop forever. */
    earthbound_main();
    __builtin_unreachable();
}
