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
 * The one case end_frame does block is when a frame finishes within the same
 * refresh as the previous present (render < ~16.7ms, i.e. already faster than
 * the panel): it waits for the prior swap to latch before queueing the next,
 * so the pipeline never holds two un-latched presents. That keeps presents
 * strictly one-per-refresh and FIFO-ordered, which is what makes the
 * round-robin buffer selection in end_frame race-free (an earlier version that
 * skipped this wait could orphan a pending buffer and flash it mid-draw).
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
#include "snes/ppu.h"
#include "gw_lcd.h"
#include "common.h"

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
#ifdef EB_PPU_PROFILE
    uint64_t ef_t0 = platform_timer_ticks();
#endif
    uint16_t *drawn = tb_buf[tb_draw];

    /* Draw the in-game overlay (volume/brightness bars, save/load icon, …) onto
     * the buffer we're about to present. EB manages its own triple-buffer and
     * never updates the launcher's active_framebuffer, so the plain
     * common_ingame_overlay() would draw into the wrong buffer — pass the actual
     * draw target explicitly. Must run before the cache flush below so the
     * overlay pixels are flushed too. */
    common_ingame_overlay_to((pixel_t *)drawn);

    /* eb_fb3 is cacheable — flush the scanlines we just wrote so the LTDC,
     * which reads RAM directly, sees them. fb1/fb2 are non-cacheable (RAM_UC),
     * so their writes already reached RAM. */
    if (drawn == eb_fb3) {
        SCB_CleanDCache_by_Addr((uint32_t *)eb_fb3, sizeof(eb_fb3));
    }

    /* Don't queue a new present while the previous one is still waiting for its
     * vblank latch. When two frames complete within a single 60Hz refresh
     * (light scenes, menus, title screens — render < ~16.7ms), the second
     * lcd_present_at_vblank() would overwrite the still-pending address and
     * orphan a buffer; the round-robin below could then hand that buffer back
     * as the next draw target right as the vblank scans it out — a 1-frame
     * tear/black flash. Waiting here bounds the pipeline to exactly one present
     * per refresh in FIFO order, which is what makes the rotation below
     * provably race-free. It only blocks when we're already ahead of the
     * refresh, so the ~25fps overworld path — where the prior present latched a
     * full refresh ago — never waits. */
    lcd_sleep_while_swap_pending();

    /* Queue this fully-drawn buffer for a tear-free swap at the next vblank. */
    lcd_present_at_vblank(drawn);

    /* Next draw target: strict round-robin. With at most one present pending
     * (guaranteed by the wait above) the buffers are in a known state — buf
     * [tb_draw] is now pending and buf[tb_draw-1] is displayed — so
     * buf[(tb_draw+1)%3] is always the third, free buffer. No read of the live
     * displayed address, so no select-vs-vblank race. */
    tb_draw = (tb_draw + 1) % 3;

#ifdef EB_PPU_PROFILE
    /* Black-frame hunt + stall detection (a long gap between presents = a frame
     * the loop blocked on, e.g. asset/music load — correlate with audio
     * underruns). With the FIFO-bounded rotation above the displayed-buffer
     * collision check should never fire; it stays as a regression tripwire. */
    {
        if ((void *)tb_buf[tb_draw] == lcd_get_displayed_buffer())
            printf("VIDEO: drawing into the displayed buffer (%p) — tear/black\n",
                   (void *)tb_buf[tb_draw]);

        static uint64_t last_present;
        uint64_t t = platform_timer_ticks();
        if (last_present) {
            uint32_t gap_ms = (uint32_t)((t - last_present) /
                                         (platform_timer_ticks_per_sec() / 1000));
            if (gap_ms > 100)
                printf("VIDEO: %lu ms since last present (stall)\n",
                       (unsigned long)gap_ms);
        }
        last_present = t;
    }
#endif

#ifdef EB_PPU_PROFILE
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

/* Scanline sink that writes into the launcher's *active* framebuffer rather
 * than EB's private triple-buffer rotation. fb1/fb2 are non-cacheable (RAM_UC),
 * so no cache maintenance is needed after the copy. */
static void eb_scanline_to_active(int y, const pixel_t *pixels)
{
    if (y < 0 || y >= EB_VIEWPORT_HEIGHT) {
        return;
    }
    uint16_t *fb = (uint16_t *)lcd_get_active_buffer();
    memcpy(&fb[y * GW_LCD_WIDTH], pixels, EB_VIEWPORT_WIDTH * sizeof(pixel_t));
}

/* Re-render the current PPU frame into the launcher's active framebuffer, for
 * the retro-go pause-menu background. The overlay code clears the active buffer,
 * calls this via the repaint hook, then darkens it and draws the dialog on top
 * (see odroid_overlay.c). This mirrors zelda3's DrawPpuFrame repaint: EB's own
 * end_frame presents through a private triple-buffer and never touches the
 * launcher's active buffer, so without an on-demand re-render the menu would
 * show the just-cleared black background. The game loop is frozen inside the
 * modal menu so the PPU state is stable, and ppu_render_frame only reads it —
 * safe to call on every menu redraw. */
void eb_video_repaint_active(void)
{
    ppu_render_frame(eb_scanline_to_active);
}

/* Savestate (de)compressor scratch — lend the idle third framebuffer.
 *
 * Saves/loads run at the root boundary while the game is paused: rendering is
 * stopped and the pause UI presents through the launcher's fb1/fb2, so eb_fb3
 * (EB's private triple-buffer member, never the LTDC-active buffer here) holds a
 * stale game frame nobody is reading. state_dump.c borrows it as the tamp LZ
 * window + working struct + compressed-I/O staging for the duration of the op;
 * the next normal end_frame repaints it on resume. It's cacheable (RAM_EMU) and
 * CPU-only during compression (no DMA), so no cache maintenance is needed while
 * it's on loan. 150 KiB — vastly more than the few hundred bytes tamp needs at
 * window_bits=8, with the slack going to chunkier SD staging. */
void *platform_savestate_scratch(size_t *out_bytes)
{
    if (out_bytes)
        *out_bytes = sizeof(eb_fb3);
    return eb_fb3;
}
