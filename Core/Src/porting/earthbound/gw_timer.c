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

/* IIR-filtered frame period in ms, scaled by (1 << FPS_IIR_SHIFT). 60 FPS →
 * 16.67 ms → steady-state period_acc ≈ 267. Shift=4 (factor 1/16) matches
 * the smoothing upstream uses for its logic/render accumulators. */
#define FPS_IIR_SHIFT 4
static uint32_t period_acc;
static uint32_t last_fps_tick;

bool platform_timer_init(void)
{
    period_acc = 0;
    last_fps_tick = 0;
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

void platform_timer_update_fps(void)
{
    uint32_t now = HAL_GetTick();
    if (last_fps_tick != 0) {
        uint32_t period = now - last_fps_tick;
        period_acc = period_acc - (period_acc >> FPS_IIR_SHIFT) + period;
    }
    last_fps_tick = now;
}

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
    /* fps = 1000ms / period_ms; tenths = fps × 10 = 10000 / period_ms.
     * period_acc is scaled by (1<<FPS_IIR_SHIFT), so we keep that scaling
     * on the numerator to preserve precision. */
    if (period_acc == 0) return 600;
    return (10000u << FPS_IIR_SHIFT) / period_acc;
}
