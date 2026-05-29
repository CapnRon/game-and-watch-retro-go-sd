/*
 * G&W video platform — triple-buffered, scanline-based blit to the LCD.
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
 * Triple buffering: the LTDC scans out one buffer while the CPU renders the
 * next, and a third stays free so the CPU never stalls on vblank. Plain
 * double-buffered vsync (swap + wait_for_vblank) quantizes the frame rate to
 * the 60Hz refresh — a ~35ms frame rounds up to 50ms (20fps). With a third
 * buffer the CPU always has a free target, so end_frame queues the finished
 * buffer for a tear-free vblank swap and returns immediately; the frame rate
 * becomes the true render rate (~28fps), and real-time pacing is still handled
 * by platform_timer_sleep_until() in host_process_frame().
 *
 * Buffer cacheability: fb1/fb2 are the launcher's buffers in the non-cacheable
 * RAM_UC region (LTDC-coherent with no maintenance). The third buffer lives in
 * EB's overlay BSS (RAM_EMU, cacheable), so we flush its cache lines before
 * handing it to the LTDC. Rendering into a cacheable buffer is also a touch
 * faster than the uncached fb1/fb2.
 */

#include <string.h>
#include <stdio.h>

#include "platform/platform.h"
#include "gw_lcd.h"

/* Third framebuffer — cacheable, in EB's overlay BSS (RAM_EMU). 32-byte
 * aligned and a whole number of cache lines so SCB_CleanDCache_by_Addr cleans
 * exactly this region. */
static uint16_t eb_fb3[GW_LCD_WIDTH * GW_LCD_HEIGHT] __attribute__((aligned(32)));

static uint16_t *tb_buf[3];
static int tb_draw = -1;   /* index of the buffer the CPU is currently drawing */

static void tb_ensure_init(void)
{
    if (tb_draw >= 0) return;
    tb_buf[0] = framebuffer1;   /* RAM_UC, non-cacheable */
    tb_buf[1] = framebuffer2;   /* RAM_UC, non-cacheable */
    tb_buf[2] = eb_fb3;         /* RAM_EMU, cacheable */
    tb_draw = 0;
}

bool platform_video_init(void)
{
    tb_ensure_init();
    return true;
}

void platform_video_shutdown(void) {}

void platform_video_begin_frame(void)
{
    tb_ensure_init();
}

void platform_video_send_scanline(int y, const pixel_t *pixels)
{
    if (y < 0 || y >= EB_VIEWPORT_HEIGHT) {
        return;
    }
    uint16_t *dst = &tb_buf[tb_draw][y * GW_LCD_WIDTH];
    memcpy(dst, pixels, EB_VIEWPORT_WIDTH * sizeof(pixel_t));
}

pixel_t *platform_video_get_framebuffer(void)
{
    tb_ensure_init();
    return (pixel_t *)tb_buf[tb_draw];
}

void platform_video_end_frame(void)
{
#ifdef PPU_PROFILE
    uint64_t ef_t0 = platform_timer_ticks();
#endif
    uint16_t *drawn = tb_buf[tb_draw];

    /* eb_fb3 is cacheable — flush the scanlines we just wrote so the LTDC,
     * which reads RAM directly, sees them. fb1/fb2 are non-cacheable (RAM_UC),
     * so their writes already reached RAM. */
    if (drawn == eb_fb3) {
        SCB_CleanDCache_by_Addr((uint32_t *)eb_fb3, sizeof(eb_fb3));
    }

    /* Queue this buffer for a tear-free swap at the next vblank — non-blocking. */
    lcd_present_at_vblank(drawn);

    /* Choose the next draw buffer: any buffer that is neither the one we just
     * queued (now pending) nor the one currently displayed. With 3 buffers and
     * at most 2 busy, one is always free — so the CPU never waits on vblank. */
    void *disp = lcd_get_displayed_buffer();
    int next = (tb_draw + 1) % 3;   /* fallback (e.g. nothing displayed yet) */
    for (int i = 0; i < 3; i++) {
        if (tb_buf[i] != drawn && (void *)tb_buf[i] != disp) {
            next = i;
            break;
        }
    }
    tb_draw = next;

#ifdef PPU_PROFILE
    /* Confirm the vsync stall is gone: end_frame should now be ~0 (cache clean
     * + non-blocking present), not the ~15ms vblank wait of the old path. */
    {
        static uint32_t acc, frames;
        acc += (uint32_t)(platform_timer_ticks() - ef_t0);
        if (++frames >= 60) {
            uint32_t div = (uint32_t)(platform_timer_ticks_per_sec() / 10000);
            if (div == 0) div = 1;
            printf("ENDFRAME/60fr: present+clean=%lu (0.1ms avg/frame)\n",
                   (unsigned long)(acc / frames / div));
            acc = frames = 0;
        }
    }
#endif
}

void platform_video_set_vsync(bool enabled)
{
    (void)enabled;
}

/* platform_render_frame() is provided by src/platform/platform_render.c
 * (the upstream single-core default). It calls begin_frame, ppu_render_frame
 * with platform_video_send_scanline as the scanline callback, and end_frame.
 * The G&W is single-core so we use that default verbatim. */
