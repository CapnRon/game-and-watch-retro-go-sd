/*
 * G&W timer platform — frame pacing via HAL_GetTick (1 ms resolution).
 *
 * frame_end() refreshes the watchdog, waits for the LCD vblank, then
 * swaps buffers. The launcher's common_emu_sound_sync() is intentionally
 * not called here yet — audio is disabled in the MVP.
 */

#include "platform/platform.h"
#include "stm32h7xx_hal.h"
#include "gw_lcd.h"

extern void wdog_refresh(void);

static uint64_t frame_counter;
static uint32_t fps_tenths = 600;

bool platform_timer_init(void)
{
    frame_counter = 0;
    return true;
}

void platform_timer_shutdown(void) {}

void platform_timer_frame_start(void) {}

void platform_timer_frame_end(void)
{
    wdog_refresh();
    lcd_wait_for_vblank();
    lcd_swap();
    frame_counter++;
}

void platform_timer_update_fps(void) {}

void platform_timer_sleep_until(uint64_t deadline)
{
    while (platform_timer_ticks() < deadline) {
        wdog_refresh();
    }
}

uint64_t platform_timer_ticks(void)
{
    return HAL_GetTick();
}

uint64_t platform_timer_ticks_per_sec(void)
{
    return 1000;
}

uint32_t platform_timer_get_fps_tenths(void)
{
    return fps_tenths;
}
