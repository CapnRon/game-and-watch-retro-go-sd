/*
 * EarthBound STANDBY hibernation (sleep / resume) — see gw_eb_hibernate.h.
 *
 * This translation unit is INTENTIONALLY part of the always-resident firmware
 * (C_SOURCES, internal flash) and NOT of the EarthBound overlay. The restore
 * path overwrites the whole live RAM_EMU range from the snapshot, so the code
 * that does it must not live in the region being overwritten. The save path is
 * called from the EB loop but also runs from flash.
 *
 * Snapshot file layout: [header][RAM_EMU bytes][stack bytes]
 *   - RAM_EMU bytes : [__RAM_EMU_START__, current_ram_pointer)  (EB code + bss +
 *     every bump allocation, incl. the lakesnes APU/DSP audio state).
 *   - stack bytes   : [saved_SP, _estack)  (the in-use C stack at the frame
 *     boundary — game frames + wait_for_vblank + eb_hibernate).
 * Launcher DTCM (.data/.bss/heap/FatFs scratch) is NOT captured: it is freshly
 * and identically re-initialised on the cold boot, and the only DTCM state the
 * resumed game frames depend on is the stack, which we do capture.
 */

#include "main.h"
#include "gw_eb_hibernate.h"
#include "gw_malloc.h"
#include "gw_linker.h"
#include "gw_lcd.h"
#include "gw_audio.h"
#include "gw_sleep.h"
#include "gw_sdcard.h"
#include "ff.h"

#include <odroid_system.h>
#include "rom_manager.h"

#include <setjmp.h>
#include <string.h>
#include <stdio.h>

/* ---- linker symbols not exposed via a header ---- */
extern uint32_t _estack;               /* top of the DTCM C stack */
extern uint32_t __eb_hib_stack_top__;  /* top of the ITCM scratch stack */
extern uint32_t __RAM_EMU_END__;
extern uint32_t _sidata;               /* LMA end of the firmware flash image */

/* ---- EB platform hooks (RAM_EMU code; safe to call after I-cache invalidate) ---- */
extern bool platform_timer_init(void);
extern bool platform_video_init(void);
extern void platform_audio_rearm(void);

#define EB_HIB_PATH          "/saves/EarthBound.hib"
#define EB_HIB_FILE_MAGIC    0x32484245u  /* 'EBH2' — snapshot format v2; bumped
                                           * when the build-identity hash moved
                                           * from GIT_TAG to a content hash, so
                                           * every pre-v2 snapshot is rejected
                                           * outright by the magic check. */
#define EB_HIB_RTC_MAGIC     0x45424948u  /* 'HIBE' — in RTC_BKP_DR1, survives STANDBY */
#define EB_HIB_RTC_REG       RTC_BKP_DR1

/* Bound on the captured C-stack size. Must fit the RAM_UC staging buffer
 * (framebuffer1 = 320*240*2 = 150 KB) with room to spare; the real EB stack at
 * a frame boundary is a few KB. Aborting above this is safer than a bad dump. */
#define EB_HIB_MAX_STACK     (64u * 1024u)
#define EB_HIB_CHUNK         (32u * 1024u)

typedef struct {
    uint32_t magic;
    uint32_t build_hash;
    uint32_t saved_sp;        /* base of the captured stack region */
    uint32_t stack_size;      /* _estack - saved_sp */
    uint32_t ram_emu_size;    /* always the whole RAM_EMU pool (__RAM_EMU_END__ - __RAM_EMU_START__) */
    uint32_t rodata_orig;     /* runtime address of earthbound.ro at save time */
    uint32_t rodata_len;
    jmp_buf  jb;              /* setjmp context to resume into */
} eb_hib_header_t;

/* Globals live in DTCM .bss (low addresses) — outside the high-address stack
 * region the restore path overwrites, so they stay valid across the swap. */
volatile bool hibernate_requested = false;
static jmp_buf       s_save_jb;
static jmp_buf       s_restore_jb;
static eb_hib_header_t s_hdr;
static FIL           s_fil;
static uint32_t      s_rodata_addr;
static uint32_t      s_rodata_len;

static uint32_t eb_hib_fnv(uint32_t h, const uint8_t *p, uint32_t n)
{
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
}

/* Identity of the EXACT binary a snapshot belongs to. A snapshot can only be
 * resumed by a byte-identical firmware AND EarthBound overlay, because it holds
 * raw stack return addresses and a jmp_buf PC pointing into both. GIT_TAG is
 * NOT enough — a dirty tree keeps the same `git describe` string across
 * rebuilds, so an incompatible stale snapshot would pass the check and BSOD.
 * So we fingerprint the actual loaded code from its two IMMUTABLE sources:
 *   (1) the firmware internal-flash image [__INTFLASH__, _sidata) — read-only in
 *       flash; catches any firmware change (including this engine);
 *   (2) the EarthBound overlay binary on SD (ACTIVE_FILE) — hashed from the
 *       file, NOT the RAM_EMU copy, because the in-RAM image's .data is mutated
 *       and its .noreloc tables are runtime-patched, so RAM bytes differ
 *       between save and restore even for the same build.
 * Any change to either flips the hash; the snapshot is then rejected and we
 * fall through to a clean fresh boot. Must be called BEFORE opening the .hib
 * file (FatFs is FF_FS_TINY — avoid two open handles at once). */
static uint32_t eb_hib_build_hash(void)
{
    /* Stack-local read buffer. On the save path this function's frame lies
     * below the captured SP, so the buffer is never part of the snapshot; and
     * keeping it off DTCM .bss matters because DTCM is nearly full. */
    uint8_t buf[1024];
    uint32_t h = 2166136261u;

    /* (1) Firmware flash image — hashed in place (memory-mapped), strided so
     * the watchdog is fed across the ~200 KB region. */
    const uint8_t *fw = (const uint8_t *)&__INTFLASH__;
    uint32_t fw_len = (uint32_t)&_sidata - (uint32_t)&__INTFLASH__;
    for (uint32_t off = 0; off < fw_len; off += 65536u) {
        uint32_t n = fw_len - off;
        if (n > 65536u) n = 65536u;
        h = eb_hib_fnv(h, fw + off, n);
        wdog_refresh();
    }

    h ^= 0x9e3779b9u;  /* domain separator between the two regions */

    /* (2) EarthBound overlay binary on SD. */
    FIL f;
    if (ACTIVE_FILE && f_open(&f, ACTIVE_FILE->path, FA_READ) == FR_OK) {
        UINT br;
        do {
            if (f_read(&f, buf, sizeof(buf), &br) != FR_OK) { br = 0; break; }
            h = eb_hib_fnv(h, buf, br);
            wdog_refresh();
        } while (br == sizeof(buf));
        f_close(&f);
    } else {
        /* Unknown EB image — fold a sentinel so this can never collide with a
         * fully-hashed identity. */
        h = eb_hib_fnv(h, (const uint8_t *)"NO_EB_BIN", 9);
    }

    return h;
}

/* ---- chunked SD I/O with watchdog refresh (a full dump is ~hundreds of KB) ---- */

static bool eb_hib_write_all(const void *buf, uint32_t size)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (size) {
        UINT n = (size > EB_HIB_CHUNK) ? EB_HIB_CHUNK : size;
        UINT bw = 0;
        if (f_write(&s_fil, p, n, &bw) != FR_OK || bw != n)
            return false;
        p += n;
        size -= n;
        wdog_refresh();
    }
    return true;
}

static bool eb_hib_read_all(void *buf, uint32_t size)
{
    uint8_t *p = (uint8_t *)buf;
    while (size) {
        UINT n = (size > EB_HIB_CHUNK) ? EB_HIB_CHUNK : size;
        UINT br = 0;
        if (f_read(&s_fil, p, n, &br) != FR_OK || br != n)
            return false;
        p += n;
        size -= n;
        wdog_refresh();
    }
    return true;
}

void eb_hibernate_set_rodata(uint8_t *eb_rodata, uint32_t eb_rodata_len)
{
    s_rodata_addr = (uint32_t)eb_rodata;
    s_rodata_len = eb_rodata_len;
}

bool eb_hibernate_pending(void)
{
    return HAL_RTCEx_BKUPRead(&hrtc, EB_HIB_RTC_REG) == EB_HIB_RTC_MAGIC;
}

/* ============================ SAVE ============================ */

void eb_hibernate(void)
{
    if (setjmp(s_save_jb) != 0) {
        /* Resume lands here (longjmp from the restore path). */
        hibernate_requested = false;
        return;
    }

    uint32_t saved_sp;
    __asm volatile("mov %0, sp" : "=r"(saved_sp));
    saved_sp &= ~0x7u;

    uint32_t estack = (uint32_t)&_estack;
    uint32_t stack_size = estack - saved_sp;

    /* Save the ENTIRE RAM_EMU pool. The bump allocator's high-water mark sits
     * within ~6 KB of the top (EB static .bss ~648 KB + lakesnes audio ~70 KB
     * of the 724 KB pool), so tracking it to trim the dump isn't worth the
     * machinery. And EB only bump-allocates at init (lakesnes audio; EB src
     * never mallocs — see eb_alloc.h), so there is no post-resume allocation
     * that could read a stale bump pointer — nothing to restore on the far
     * side. NOTE: this invariant breaks if any code starts ram_malloc()'ing
     * during gameplay; then the bump pointer would need saving/restoring. */
    uint32_t ram_emu_start = (uint32_t)__RAM_EMU_START__;
    uint32_t ram_emu_size = (uint32_t)&__RAM_EMU_END__ - ram_emu_start;

    if (stack_size > EB_HIB_MAX_STACK) {
        /* Refuse rather than write a bad snapshot; game keeps running. */
        printf("[EB hib] refused: stack=%lu too deep\n", (unsigned long)stack_size);
        hibernate_requested = false;
        return;
    }

    /* Freeze the RAM_EMU audio ring vs. the SAI ISR so it isn't dumped torn. */
    audio_stop_playing();
    /* Don't hand a partially-presented frame to the panel during the dump. */
    lcd_sleep_while_swap_pending();
    /* Push cached writes to physical RAM before FatFs reads them. */
    SCB_CleanDCache();

    memset(&s_hdr, 0, sizeof(s_hdr));
    s_hdr.magic = EB_HIB_FILE_MAGIC;
    s_hdr.build_hash = eb_hib_build_hash();
    s_hdr.saved_sp = saved_sp;
    s_hdr.stack_size = stack_size;
    s_hdr.ram_emu_size = ram_emu_size;
    s_hdr.rodata_orig = s_rodata_addr;
    s_hdr.rodata_len = s_rodata_len;
    memcpy(s_hdr.jb, s_save_jb, sizeof(jmp_buf));

    f_mkdir("/saves");  /* harmless if it already exists */

    bool ok = (f_open(&s_fil, EB_HIB_PATH, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK);
    if (ok) {
        UINT bw = 0;
        ok = (f_write(&s_fil, &s_hdr, sizeof(s_hdr), &bw) == FR_OK && bw == sizeof(s_hdr));
        ok = ok && eb_hib_write_all((const void *)ram_emu_start, ram_emu_size);
        ok = ok && eb_hib_write_all((const void *)saved_sp, stack_size);
        f_close(&s_fil);
    }

    if (!ok) {
        printf("[EB hib] snapshot write failed\n");
        f_unlink(EB_HIB_PATH);
        platform_audio_rearm();  /* un-mute: we stopped the SAI above */
        hibernate_requested = false;
        return;
    }

    /* Remember EarthBound as the startup app so the launcher auto-launches it on
     * wake, and arm the STANDBY-surviving restore flag. */
    odroid_settings_StartupFile_set(ACTIVE_FILE);
    odroid_settings_commit();
    HAL_RTCEx_BKUPWrite(&hrtc, EB_HIB_RTC_REG, EB_HIB_RTC_MAGIC);

    /* Full power-down. Never returns; we come back via a cold boot + restore. */
    GW_EnterDeepSleep(true, NULL, NULL);

    /* Unreachable, but keep state sane if STANDBY somehow fell through. */
    hibernate_requested = false;
}

/* ============================ RESTORE ============================ */

/* Re-fixup the restored .noreloc pointer tables when this boot cached the
 * earthbound.ro blob at a different address than the save-time boot. Mirrors
 * PatchRodataRange() in main_earthbound.c but rebased on the runtime address
 * (the snapshot's pointers are already runtime-resolved, not link-VMA). */
static void eb_hib_patch_rodata(uint32_t rodata_orig, int32_t delta, uint32_t len)
{
    uint32_t *ptr = (uint32_t *)__RAM_EMU_START__;
    uint32_t *end = (uint32_t *)_OVERLAY_EARTHBOUND_BSS_END;
    uint32_t *skip_lo = (uint32_t *)_EARTHBOUND_MAIN_CODE_START;
    uint32_t *skip_hi = (uint32_t *)_EARTHBOUND_MAIN_CODE_END;

    while (ptr < end) {
        if (!(ptr >= skip_lo && ptr <= skip_hi)) {
            uint32_t v = *ptr;
            if (v >= rodata_orig && v < rodata_orig + len) {
                *ptr = (uint32_t)((int32_t)v + delta);
                wdog_refresh();
            }
        }
        ptr++;
    }
}

/* Final hand-off, running on the ITCM scratch stack (see eb_hib_switch_and_jump)
 * so overwriting the DTCM stack region is safe. Non-static + used so the inline
 * asm `bl` resolves; never returns. */
__attribute__((noinline, used))
void eb_hib_restore_and_jump(const void *staging, uint32_t dst_sp,
                             uint32_t len, void *jb)
{
    memcpy((void *)dst_sp, staging, len);  /* repopulate the suspended stack */
    __DSB();
    __ISB();
    __enable_irq();
    longjmp(*(jmp_buf *)jb, 1);             /* SP <- saved_sp, PC -> setjmp site */
    __builtin_unreachable();
}

static void eb_hib_switch_and_jump(const void *staging, uint32_t dst_sp,
                                   uint32_t len, void *jb)
{
    register uint32_t r_top asm("r4") = (uint32_t)&__eb_hib_stack_top__;
    register uint32_t r_stg asm("r5") = (uint32_t)staging;
    register uint32_t r_dsp asm("r6") = dst_sp;
    register uint32_t r_len asm("r7") = len;
    register uint32_t r_jb  asm("r8") = (uint32_t)jb;

    __asm volatile(
        "mov sp, %[top]      \n\t"  /* abandon the DTCM stack for ITCM scratch */
        "mov r0, %[stg]      \n\t"
        "mov r1, %[dsp]      \n\t"
        "mov r2, %[len]      \n\t"
        "mov r3, %[jb]       \n\t"
        "bl  eb_hib_restore_and_jump\n\t"
        :
        : [top]"r"(r_top), [stg]"r"(r_stg), [dsp]"r"(r_dsp),
          [len]"r"(r_len), [jb]"r"(r_jb)
        : "r0", "r1", "r2", "r3", "lr", "memory");
    __builtin_unreachable();
}

void eb_hibernate_restore(uint8_t *eb_rodata, uint32_t eb_rodata_len)
{
    (void)eb_rodata_len;

    /* Clear the flag first: if the restore crashes, the next boot is a clean,
     * normal launch rather than a restore loop. */
    HAL_RTCEx_BKUPWrite(&hrtc, EB_HIB_RTC_REG, 0);

    if (!fs_mounted)
        return;

    /* Identity of the running binary — computed BEFORE opening the snapshot
     * file because eb_hib_build_hash() opens ACTIVE_FILE and FatFs is
     * FF_FS_TINY (one shared window; keep to a single open handle). */
    uint32_t cur_hash = eb_hib_build_hash();

    if (f_open(&s_fil, EB_HIB_PATH, FA_READ) != FR_OK)
        return;

    UINT br = 0;
    if (f_read(&s_fil, &s_hdr, sizeof(s_hdr), &br) != FR_OK || br != sizeof(s_hdr)) {
        f_close(&s_fil);
        return;
    }

    uint32_t ram_emu_start = (uint32_t)__RAM_EMU_START__;
    uint32_t ram_emu_cap = (uint32_t)&__RAM_EMU_END__ - ram_emu_start;
    uint32_t estack = (uint32_t)&_estack;

    /* Reject anything that isn't an exact-build, internally-consistent
     * snapshot. The build-hash check is the primary guard against resuming into
     * a different binary (which would BSOD); the structural checks catch a
     * corrupt/truncated header even if the hash somehow matched. On ANY
     * rejection we fall through to a clean fresh boot — hibernation never
     * touches the player's committed in-game SRAM save, so only the
     * (unrestorable) sleep state is lost, never real progress. */
    bool ok = s_hdr.magic == EB_HIB_FILE_MAGIC
           && s_hdr.build_hash == cur_hash
           && s_hdr.stack_size != 0
           && s_hdr.stack_size <= EB_HIB_MAX_STACK
           && s_hdr.saved_sp < estack
           && s_hdr.saved_sp >= estack - EB_HIB_MAX_STACK
           && s_hdr.stack_size == estack - s_hdr.saved_sp
           && s_hdr.ram_emu_size == ram_emu_cap;  /* always the whole pool */
    if (!ok) {
        printf("[EB hib] snapshot rejected (magic/build/consistency)\n");
        f_close(&s_fil);
        f_unlink(EB_HIB_PATH);
        return;
    }

    /* 1) Restore the live RAM_EMU range (EB code + bss + allocations). */
    if (!eb_hib_read_all((void *)ram_emu_start, s_hdr.ram_emu_size)) {
        f_close(&s_fil);
        return;
    }

    /* 2) Stage the saved stack image in RAM_UC (framebuffer1, unused now). */
    void *staging = (void *)framebuffer1;
    if (!eb_hib_read_all(staging, s_hdr.stack_size)) {
        f_close(&s_fil);
        return;
    }
    f_close(&s_fil);

    /* (The RAM_EMU bump pointer is not restored: EB only allocates at init, not
     * during gameplay, so nothing reads it after resume — see the save side.) */

    /* 3) Re-anchor rodata pointers if the blob landed elsewhere this boot. */
    int32_t delta = (int32_t)((uint32_t)eb_rodata - s_hdr.rodata_orig);
    if (delta != 0 && s_hdr.rodata_len != 0)
        eb_hib_patch_rodata(s_hdr.rodata_orig, delta, s_hdr.rodata_len);

    /* 4) Make restored RAM_EMU coherent for both data reads and code execution
     * (it was filled by CPU stores into the cached region, and holds the EB
     * code we are about to run). */
    SCB_CleanInvalidateDCache();
    SCB_InvalidateICache();
    __DSB();
    __ISB();

    /* 5) Re-arm peripherals the skipped earthbound_main()/platform_*_init would
     * have set up. These touch the now-restored RAM_EMU glue state and arm
     * hardware; none of them re-allocate. */
    platform_timer_init();   /* hw DWT->CYCCNT was reset by the cold boot */
    platform_video_init();   /* tb_buf/tb_draw already restored; idempotent */
    platform_audio_rearm();  /* SAI DMA + ISR callback; keeps the restored APU */

    /* Hold the resume context in DTCM .bss, off the stack we are about to
     * overwrite. */
    memcpy(s_restore_jb, s_hdr.jb, sizeof(jmp_buf));

    /* 6) Overwrite the suspended stack and longjmp back into the game. The
     * critical region (SP switch + memcpy + longjmp) runs with interrupts off
     * and is only microseconds long. */
    __disable_irq();
    eb_hib_switch_and_jump(staging, s_hdr.saved_sp, s_hdr.stack_size, s_restore_jb);
    __builtin_unreachable();
}
