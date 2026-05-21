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

#define EB_AUDIO_SAMPLE_RATE   (16000)

/* Shared with gw_input.c — the platform layer reads from this each frame. */
odroid_gamepad_state_t eb_joystick;

/* PatchCodeRodataOffset — identical to zelda3's, just with EARTHBOUND
 * symbols. Walks the RAM_EMU region looking for pointers that fall inside
 * RODATA_BASE..RODATA_BASE+ro_size and adjusts them to point at the actual
 * extflash location where odroid_overlay_cache_file_in_flash placed the
 * rodata blob.
 *
 * Skips the main_earthbound.o code window (between _EARTHBOUND_MAIN_CODE_*)
 * because that code lives in the overlay but doesn't reference the offset
 * RODATA_BASE — its rodata is in .overlay_earthbound itself, not the
 * separate .rodata_earthbound region.
 */
#define RODATA_BASE 0xCAFE0000
static void PatchCodeRodataOffset(uint8_t *rodata, uint32_t rodata_length)
{
    uint32_t *ptr = (uint32_t *)__RAM_EMU_START__;
    uint32_t *end = (uint32_t *)&_OVERLAY_EARTHBOUND_BSS_END;

    int32_t offset = (uint32_t)rodata - RODATA_BASE;

    printf("eb rodata = %p offset = 0x%08lX\n", rodata, offset);
    while (ptr < end) {
        if ((ptr < (uint32_t *)&_EARTHBOUND_MAIN_CODE_START) ||
            (ptr > (uint32_t *)&_EARTHBOUND_MAIN_CODE_END)) {
            uint32_t value = *ptr;
            if ((value >= RODATA_BASE) && (value < RODATA_BASE + rodata_length)) {
                *ptr = value + offset;
                wdog_refresh();
            }
        }
        ptr++;
    }
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
