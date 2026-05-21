/*
 * G&W video platform — scanline-based blit into the launcher's LCD buffer.
 *
 * EarthBound renders one scanline at a time (256 px wide BGR565), so we
 * memcpy each scanline directly into lcd_get_active_buffer() at row
 * (y + Y_OFFSET), starting at column X_OFFSET. The launcher's 320x240 LCD
 * has 32 px of black bars on each side and 8 px top/bottom around the
 * 256x224 SNES viewport.
 *
 * platform_video_end_frame() does NOT call lcd_swap() here — frame
 * presentation is handled centrally from platform_timer_frame_end() so
 * the swap is synchronized with audio DMA cadence.
 */

#include <string.h>

#include "platform/platform.h"
#include "gw_lcd.h"

#define EB_VIEWPORT_WIDTH  256
#define EB_VIEWPORT_HEIGHT 224
#define X_OFFSET ((GW_LCD_WIDTH  - EB_VIEWPORT_WIDTH)  / 2)
#define Y_OFFSET ((GW_LCD_HEIGHT - EB_VIEWPORT_HEIGHT) / 2)

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
    uint16_t *fb = lcd_get_active_buffer();
    memcpy(&fb[(y + Y_OFFSET) * GW_LCD_WIDTH + X_OFFSET],
           pixels,
           EB_VIEWPORT_WIDTH * sizeof(pixel_t));
}

pixel_t *platform_video_get_framebuffer(void)
{
    return (pixel_t *)lcd_get_active_buffer();
}

void platform_video_end_frame(void) {}

void platform_video_set_vsync(bool enabled)
{
    (void)enabled;
}

extern void ppu_render_frame(void);

void platform_render_frame(scanline_stamp_cb_t fps_overlay_cb)
{
    platform_video_begin_frame();
    ppu_render_frame();
    if (fps_overlay_cb) {
        uint16_t *fb = lcd_get_active_buffer();
        for (int y = 0; y < EB_VIEWPORT_HEIGHT; y++) {
            fps_overlay_cb(y, &fb[(y + Y_OFFSET) * GW_LCD_WIDTH + X_OFFSET]);
        }
    }
    platform_video_end_frame();
}
