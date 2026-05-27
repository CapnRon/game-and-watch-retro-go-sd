/*
 * G&W video platform — scanline-based blit into the launcher's LCD buffer.
 *
 * The EB src/ build (Makefile.common: C_DEFS_EARTHBOUND) sets:
 *   - EB_VIEWPORT_WIDTH=320 EB_VIEWPORT_HEIGHT=240 — PPU outputs scanlines sized
 *     to the LCD; the 256x224 SNES content is centered by EB itself, with
 *     the margins filled by BG fill color / black.
 *   - EB_PIXEL_RGB565=1 — pixel_t is built as RGB565 instead of the default
 *     BGR565, matching the LTDC layer format (main.c:831). The swap happens
 *     once per palette entry per frame (256 ops) inside ppu_prepare_palette,
 *     not per pixel — so this file is a plain memcpy.
 *
 * platform_video_end_frame() performs the LTDC swap. host_process_frame()
 * only invokes platform_timer_frame_end() on the fast-forward path, so the
 * present cannot live there — the SDL port (sdl2_video.c) does the same
 * thing: end_frame is the present, frame_end is just timing.
 */

#include <string.h>

#include "platform/platform.h"
#include "gw_lcd.h"

bool platform_video_init(void)
{
    return true;
}

void platform_video_shutdown(void) {}

void platform_video_begin_frame(void) {}

void platform_video_send_scanline(int y, const pixel_t *pixels)
{
    if (y < 0 || y >= EB_VIEWPORT_HEIGHT) {
        return;
    }
    uint16_t *dst = &((uint16_t *)lcd_get_active_buffer())[y * GW_LCD_WIDTH];
    memcpy(dst, pixels, EB_VIEWPORT_WIDTH * sizeof(pixel_t));
}

pixel_t *platform_video_get_framebuffer(void)
{
    return (pixel_t *)lcd_get_active_buffer();
}

void platform_video_end_frame(void)
{
    /* Schedule LTDC to switch to the buffer we just rendered (takes effect
     * at next vblank), then wait for that vblank so the next frame's writes
     * don't land in the buffer LTDC is still displaying. */
    lcd_swap();
    lcd_wait_for_vblank();
}

void platform_video_set_vsync(bool enabled)
{
    (void)enabled;
}

/* platform_render_frame() is provided by src/platform/platform_render.c
 * (the upstream single-core default). It calls begin_frame, ppu_render_frame
 * with platform_video_send_scanline as the scanline callback, and end_frame.
 * The G&W is single-core so we use that default verbatim. */
