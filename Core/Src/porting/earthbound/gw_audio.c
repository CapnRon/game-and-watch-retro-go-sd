/*
 * G&W audio platform — drives the lakesnes SPC700/DSP emulator into the SAI DMA
 * through a decoupling ring buffer.
 *
 * Native rate: the SPC700 DSP produces ~533.33 samples per NTSC frame at 32 kHz
 * (dsp_getSamples resamples to exactly 534). The SAI is configured for 32 kHz
 * mono (odroid_audio_init(32000)), so one game frame == one SAI half-buffer.
 *
 * Why a ring buffer (not a frame-pumped 2-half buffer):
 *
 *   EarthBound renders well below 60 fps and relies on frame-skip to keep game
 *   logic + audio running at 60 Hz. With the old design the pump wrote one
 *   frame into the active DMA half every frame, but the loop only *waited* for
 *   a DMA boundary on rendered frames. On a skip frame the pump ran immediately
 *   with the DMA state unchanged, overwriting the half it had just filled — so
 *   the previous frame's audio was discarded before it played. With ~45% of
 *   frames skipped, a large fraction of generated audio never reached the
 *   speaker → discontinuous, crunchy output. On top of that, a 2-half (~33 ms)
 *   buffer has no slack to ride out a ~26 ms render during which no audio is
 *   produced.
 *
 *   The ring fixes both. The producer (eb_audio_pump, once per game frame)
 *   pushes exactly one frame per call and never overwrites unconsumed data. The
 *   consumer (the SAI Tx ISR, via audio_set_dma_refill_callback) pops one frame
 *   per DMA half into the just-freed half — so the buffer stays fed even during
 *   a long render, and every produced frame plays exactly once in order. The
 *   ring depth provides the jitter slack the 2-half buffer lacked.
 *
 * Pacing: the game loop is paced by ring back-pressure (eb_audio_pump blocks
 * until a slot frees) instead of common_emu_sound_sync(). Because the ISR drains
 * the ring at the fixed SAI rate, blocking on a free slot phase-locks audio
 * generation to consumption — strictly better than the old half-buffer wait,
 * and it removes the skip-frame coupling entirely (see gw_timer.c).
 *
 * Concurrency: single producer (main loop, writes head), single consumer (SAI
 * ISR, writes tail). 32-bit aligned accesses are atomic on the M7; a DMB orders
 * the ring writes before the head publish. No lock needed.
 */

#include <string.h>

#include "platform/platform.h"
#include "game/audio.h"

#include "common.h"
#include "gw_audio.h"
#include "odroid_audio.h"
#include "porting/common.h"

extern void wdog_refresh(void);

#define EB_AUDIO_SAMPLES_PER_FRAME 534  /* 32000 / 60 ≈ 533.33 */

/* Ring depth in frames. MUST be a power of two (index masking). Steady-state
 * audio latency ≈ EB_RING_FRAMES × 16.69 ms because back-pressure keeps the
 * ring near full. 8 frames (~133 ms) gives generous slack to ride consecutive
 * long renders; drop to 4 (~67 ms) if SFX feel laggy and crunch stays gone. */
#define EB_RING_FRAMES 8
#define EB_RING_MASK   (EB_RING_FRAMES - 1)

/* Stereo scratch — audio_generate_samples writes L,R,L,R interleaved. */
static int16_t eb_stereo_scratch[EB_AUDIO_SAMPLES_PER_FRAME * 2];

/* Ring of fully-mixed (mono, volume-applied) frames. */
static int16_t eb_ring[EB_RING_FRAMES][EB_AUDIO_SAMPLES_PER_FRAME];
static volatile uint16_t eb_ring_head;  /* producer (main loop) writes */
static volatile uint16_t eb_ring_tail;  /* consumer (SAI ISR) writes */

static inline uint16_t eb_ring_count(void)
{
    return (uint16_t)(eb_ring_head - eb_ring_tail);
}

/* SAI Tx ISR refill: pop one frame into the just-freed DMA half. Kept tiny —
 * a single memcpy. On underrun (ring empty) emit silence; with adequate ring
 * depth this should not happen outside startup. */
static void eb_audio_dma_refill(int16_t *dst, uint16_t samples)
{
    uint16_t n = (samples < EB_AUDIO_SAMPLES_PER_FRAME)
                 ? samples : EB_AUDIO_SAMPLES_PER_FRAME;

    if (eb_ring_head == eb_ring_tail) {
        memset(dst, 0, samples * sizeof(int16_t));
        return;
    }

    memcpy(dst, eb_ring[eb_ring_tail & EB_RING_MASK], n * sizeof(int16_t));
    if (samples > n)
        memset(dst + n, 0, (samples - n) * sizeof(int16_t));

    eb_ring_tail++;
}

bool platform_audio_init(void)
{
    eb_ring_head = 0;
    eb_ring_tail = 0;
    audio_init();
    audio_set_dma_refill_callback(eb_audio_dma_refill);
    audio_start_playing(EB_AUDIO_SAMPLES_PER_FRAME);
    return true;
}

void platform_audio_shutdown(void)
{
    audio_set_dma_refill_callback(NULL);
    audio_stop_playing();
    audio_shutdown();
}

void platform_audio_lock(void) {}
void platform_audio_unlock(void) {}

void eb_audio_pump(void)
{
    /* Back-pressure: wait for a free ring slot. This is the frame loop's pacing
     * clock — the SAI ISR frees one slot every ~16.69 ms, so blocking here locks
     * generation to playback. The wait is bounded by one DMA half (well under
     * the WWDG timeout); refresh the watchdog each spin in case a preceding
     * render ran long. */
    while (eb_ring_count() >= EB_RING_FRAMES) {
        wdog_refresh();
        cpumon_sleep();  /* __WFI; wakes on the SAI ISR that frees a slot */
    }

    int16_t *dst = eb_ring[eb_ring_head & EB_RING_MASK];

    if (common_emu_sound_loop_is_muted()) {
        memset(dst, 0, EB_AUDIO_SAMPLES_PER_FRAME * sizeof(int16_t));
    } else {
        audio_generate_samples(eb_stereo_scratch, EB_AUDIO_SAMPLES_PER_FRAME);

        int16_t factor = common_emu_sound_get_volume();
        for (uint16_t i = 0; i < EB_AUDIO_SAMPLES_PER_FRAME; i++) {
            int32_t mono = ((int32_t)eb_stereo_scratch[i * 2] +
                            (int32_t)eb_stereo_scratch[i * 2 + 1]) >> 1;
            dst[i] = (int16_t)((mono * factor) >> 8);
        }
    }

    /* Publish the frame: ensure the ring writes land before the head advance
     * the ISR observes. */
    __DMB();
    eb_ring_head++;
}
