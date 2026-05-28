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
#include <string.h>

#include "main.h"
#include "gw_lcd.h"
#include "gw_linker.h"
#include "gw_buttons.h"
#include "gw_malloc.h"
#include "gw_ofw.h"

#include "stm32h7xx_hal.h"

#include "common.h"
#include "rom_manager.h"
#include "appid.h"

#include "main_earthbound.h"

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
 * .overlay_earthbound itself, not in .rodata_earthbound. */
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

static void *Screenshot(void)
{
    lcd_wait_for_vblank();
    lcd_clear_active_buffer();
    /* TODO: render the current PPU frame into the active buffer once the
     * platform_video_send_scanline implementation in gw_video.c is wired up.
     * For now this is just a black frame to keep the launcher's cover-capture
     * hook from crashing. */
    return lcd_get_active_buffer();
}

/* TODO: real implementations once title screen boots. EarthBound's save
 * format is a flat 7680-byte buffer (SAVE_FILE_SIZE in src/save.c), so the
 * retro-go "savestate" and "SRAM" concepts collapse to the same content
 * here. First-boot goal is title-screen-only; saves are post-MVP. */
static bool eb_LoadState(char *savePathName)
{
    (void)savePathName;
    return false;
}

static bool eb_SaveState(char *savePathName)
{
    (void)savePathName;
    return true;
}

static void eb_SramSave(void)
{
    /* TODO: write the 7680-byte EarthBound save buffer to ODROID_PATH_SAVE_SRAM. */
}

extern int earthbound_main(void);

int app_main_earthbound(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)load_state;
    (void)start_paused;
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
     * location of the rodata blob. */
    PatchCodeRodataOffset(eb_rodata, eb_rodata_length);

    odroid_system_init(APPID_EARTHBOUND, EB_AUDIO_SAMPLE_RATE);
    odroid_system_emu_init(&eb_LoadState, &eb_SaveState, &Screenshot,
                           NULL, NULL, &eb_SramSave);

    /* Hand off to upstream — earthbound_main() runs the game loop forever. */
    earthbound_main();
    __builtin_unreachable();
}
