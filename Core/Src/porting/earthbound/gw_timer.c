/*
 * G&W timer platform — frame pacing via HAL_GetTick (1 ms resolution).
 *
 * Frame presentation lives in gw_video.c::platform_video_end_frame(); this
 * file only does timing. host_process_frame() only calls frame_end() on the
 * fast-forward path, so a swap here would never fire in the normal path.
 *
 * Audio is pumped from platform_timer_frame_start (called at the bottom of
 * host_process_frame, every frame, including frame-skipped ones) so the
 * SAI DMA never underruns when the renderer falls behind.
 */

#include "platform/platform.h"
#include "stm32h7xx_hal.h"
#include "gw_lcd.h"

extern void wdog_refresh(void);
extern void eb_audio_pump(void);

static uint32_t fps_tenths = 600;

bool platform_timer_init(void)
{
    return true;
}

void platform_timer_shutdown(void) {}

void platform_timer_frame_start(void)
{
    /* Called once per frame from host_process_frame() regardless of frame
     * skip / fast-forward / sleep state. wdog_refresh() lives here (not just
     * inside platform_timer_sleep_until's while-loop) because once frame work
     * exceeds frame_period the sleep call returns instantly with no refresh,
     * and we'd WWDG-reset within a second. */
    wdog_refresh();

    /* Refill the inactive half of the SAI DMA buffer with one frame of
     * APU samples. Runs every frame so skipped renders still keep audio
     * flowing — the DMA cadence is locked to wall-clock, not draw-rate. */
    eb_audio_pump();
}

void platform_timer_frame_end(void)
{
    /* Fast-forward path only — see file header. Frame present is in
     * platform_video_end_frame(); here we just keep the watchdog happy. */
    wdog_refresh();
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
