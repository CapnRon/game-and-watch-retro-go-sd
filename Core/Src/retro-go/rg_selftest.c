/* PSRAM data-path self-test v5 — real production cache flow + triage.
 *
 * v4 (kept, unchanged below) proved:
 *   - production 256B 0x02 writes land intact (per-page indirect readback)
 *   - with XSPI NCS-boundary (CSBOUND=10, main.c) the MM window reads clean
 *
 * But zelda3 STILL HardFaults during the "Caching game" progress bar, with a
 * corrupted stack return slot whose value looks like packed RGB565 colors —
 * i.e. a fill loop writing at a bad framebuffer pointer. v5 adds:
 *   ST-RC  : run the EXACT production cache entry (odroid_overlay_cache_file_
 *            in_flash: nested progress_cb + real progress bar + lcd_swap) for
 *            both zelda3 files, then verify PSRAM window vs fresh SD stream
 *   ST-FB  : log the framebuffer pointer globals before/after
 *   end    : drop the cache table so the user's launch re-caches live,
 *            instrumented by the buffer canaries in circular_flash_write and
 *            the bad-pointer guard in lcd_get_active_buffer.
 *
 * Triggered at cold boot while /selftest.flag is on the SD card.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <odroid_system.h>
#include "odroid_settings.h"
#include "main.h"
#include "gw_flash.h"
#include "gw_flash_alloc.h"
#include "gw_linker.h"
#include "gw_lcd.h"
#include "odroid_overlay.h"
#include "rg_storage.h"
#include "rg_selftest.h"

#define ST_RO_FILE    "/roms/homebrew/zelda3.ro"
#define ST_ASSET_FILE "/roms/homebrew/zelda3_assets.dat"
#define ST_SCRATCH    0x100000u
#define ST_WIN_BASE   ((uintptr_t)&__EXTFLASH_BASE__) /* 0x90000000 */
#define ST_CHUNK      0x4000u

static uint8_t  st_buf[ST_CHUNK] __attribute__((aligned(4)));

static void st_log_fb(const char *tag)
{
    printf("ST-FB %s: f1=%08lx f2=%08lx act=%lu\n", tag,
           (unsigned long)(uintptr_t)framebuffer1,
           (unsigned long)(uintptr_t)framebuffer2,
           (unsigned long)active_framebuffer);
}

/* Run the real production cache path for `path` (progress bar and all),
 * then verify the cached PSRAM contents against a fresh SD stream. */
static void st_real_cache(const char *path)
{
    uint32_t size = 0;
    uint8_t *p = odroid_overlay_cache_file_in_flash(path, &size, false);
    printf("ST-RC %s: ptr=%08lx size=0x%lx\n", path,
           (unsigned long)(uintptr_t)p, (unsigned long)size);
    st_log_fb("post-cache");
    if (!p)
        return;

    uint32_t bad = 0, first = 0, off = 0;
    const volatile uint8_t *w = (const volatile uint8_t *)(uintptr_t)p;
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n;
        while ((n = fread(st_buf, 1, ST_CHUNK, f)) > 0) {
            for (uint32_t x = 0; x < (uint32_t)n; x++)
                if (w[off + x] != st_buf[x]) {
                    if (!first)
                        first = off + x;
                    bad++;
                }
            off += (uint32_t)n;
            wdog_refresh();
        }
        fclose(f);
    }
    printf("ST-RC verify: size=0x%lx bad=%lu first=0x%lx %s\n",
           (unsigned long)off, (unsigned long)bad, (unsigned long)first,
           bad ? "FAIL" : "OK");
}

/* ----------------------------------------------------------------- main - */
void psram_selftest_run(void)
{
    uint8_t oc = odroid_settings_cpu_oc_level_get();
    SystemClock_Config(oc);

    printf("ST: ==== PSRAM selftest v5 oc=%lu ====\n", (unsigned long)oc);
    st_log_fb("start");

    /* What did the previous session leave on the SD? */
    flash_alloc_dump_metadata_state();

    /* PSRAM status register (RDSR 0x05) — burst length / mode bits */
    OSPI_DisableMemoryMappedMode();
    uint8_t sr = 0;
    OSPI_PsramReadStatus(&sr);
    printf("ST: PSRAM SR=0x%02x\n", (unsigned)sr);

    /* ---------------- v4: scratch-band write/read verification ---------- */
    FILE *f = fopen(ST_RO_FILE, "rb");
    if (!f) {
        printf("ST: cannot open %s\n", ST_RO_FILE);
        return;
    }

    uint8_t ff[256];
    memset(ff, 0xFF, 256);
    uint8_t chunk[0x1000] __attribute__((aligned(4))); /* 4KB: the user stack
        * is only 24 KB — keep this frame small (see circular_flash_write) */
    uint8_t rb[256];

    uint32_t off = 0, nchunk = 0;
    uint32_t badpg = 0, badb = 0, firstoff = 0;
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        uint32_t len = (uint32_t)n;
        /* 0xFF fill (production erase contract) */
        for (uint32_t a = 0; a < len; a += 256)
            OSPI_Program(ST_SCRATCH + off + a, ff, 256);
        /* data page by page with immediate indirect readback verify */
        for (uint32_t a = 0; a < len; a += 256) {
            uint32_t pl = (len - a > 256) ? 256u : (len - a);
            OSPI_Program(ST_SCRATCH + off + a, chunk + a, pl);
            OSPI_IndirectRead(ST_SCRATCH + off + a, rb, pl);
            uint32_t cb = 0;
            for (uint32_t x = 0; x < pl; x++)
                if (rb[x] != chunk[a + x])
                    cb++;
            if (cb) {
                badpg++;
                badb += cb;
                if (!firstoff)
                    firstoff = off + a;
            }
        }
        off += len;
        nchunk++;
        wdog_refresh();
    }
    fclose(f);
    printf("ST-V4 indirect-perpage size=0x%lx badpages=%lu badbytes=%lu first=0x%lx\n",
           (unsigned long)off, (unsigned long)badpg, (unsigned long)badb,
           (unsigned long)firstoff);

    /* contrast: read the whole region through the MM window vs fresh SD read */
    OSPI_EnableMemoryMappedMode();
    const volatile uint8_t *w = (const volatile uint8_t *)(ST_WIN_BASE + ST_SCRATCH);
    f = fopen(ST_RO_FILE, "rb");
    uint32_t wbad = 0, wfirst = 0, woff = 0;
    if (f) {
        while ((n = fread(st_buf, 1, ST_CHUNK, f)) > 0) {
            for (uint32_t x = 0; x < (uint32_t)n; x++)
                if (w[woff + x] != st_buf[x]) {
                    if (!wfirst)
                        wfirst = woff + x;
                    wbad++;
                }
            woff += (uint32_t)n;
            wdog_refresh();
        }
        fclose(f);
    }
    printf("ST-V4 window-verify  size=0x%lx bad=%lu first=0x%lx\n",
           (unsigned long)woff, (unsigned long)wbad, (unsigned long)wfirst);

    /* ---------------- v5: real production cache flow -------------------- */
    st_real_cache(ST_RO_FILE);
    st_real_cache(ST_ASSET_FILE);
    st_log_fb("end");

    /* Drop the cache table so the user's game launch re-caches live with the
     * canary/guard instrumentation in place (a cache hit would skip the
     * progress-bar path entirely). */
    flash_alloc_discard_stale_cache();
    flash_alloc_dump_metadata_state(); /* must say "file absent" */
    printf("ST: cache table dropped (launch re-caches live)\n");

    /* leave MM mode ON (normal post-boot state) */
    printf("ST: ==== done ====\n");
}
