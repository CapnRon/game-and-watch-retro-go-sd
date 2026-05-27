/*
 * G&W audio platform — drives the lakesnes SPC700/DSP emulator into the SAI DMA.
 *
 * Cadence (matches the SDL port's SAMPLES_PER_FRAME=534): the SPC700's DSP
 * produces ~533.33 samples per NTSC frame at its native 32 kHz rate, so we
 * configure the SAI for 32 kHz mono and pull 534 stereo samples per game
 * frame. The DMA full buffer is 2*534 = 1068 int16s; half-complete interrupts
 * fire every 16.69 ms, which lines up with the 60 fps game frame.
 *
 * Stereo→mono downmix: SAI is configured SAI_MONOMODE (main.c::MX_SAI1_Init),
 * and the device has a single speaker. We average L+R into the active DMA
 * half each frame.
 *
 * Threading: single-threaded MCU, so audio_lock/unlock are no-ops. The
 * SAI DMA only writes the dma_state field from ISR; the rest of the
 * audiobuffer_dma writes are exclusive to the main loop.
 *
 * Pump site: eb_audio_pump() is called from gw_timer.c::platform_timer_frame_start
 * once per frame (including frame-skipped ones), so the DMA never underruns
 * even when the renderer falls behind.
 */

#include <string.h>

#include "platform/platform.h"
#include "game/audio.h"

#include "common.h"
#include "gw_audio.h"
#include "odroid_audio.h"

#define EB_AUDIO_SAMPLES_PER_FRAME 534  /* 32000 / 60 ≈ 533.33 */

/* Stereo scratch — audio_generate_samples writes L,R,L,R interleaved. */
static int16_t eb_stereo_scratch[EB_AUDIO_SAMPLES_PER_FRAME * 2];

bool platform_audio_init(void)
{
    audio_init();
    audio_start_playing(EB_AUDIO_SAMPLES_PER_FRAME);
    return true;
}

void platform_audio_shutdown(void)
{
    audio_stop_playing();
    audio_shutdown();
}

void platform_audio_lock(void) {}
void platform_audio_unlock(void) {}

void eb_audio_pump(void)
{
    if (common_emu_sound_loop_is_muted()) {
        /* common_emu_sound_loop_is_muted already zeroed the active half. */
        return;
    }

    audio_generate_samples(eb_stereo_scratch, EB_AUDIO_SAMPLES_PER_FRAME);

    int16_t  factor    = common_emu_sound_get_volume();
    int16_t *dst       = audio_get_active_buffer();
    uint16_t dst_count = audio_get_buffer_length();
    uint16_t src_count = (dst_count < EB_AUDIO_SAMPLES_PER_FRAME)
                         ? dst_count
                         : EB_AUDIO_SAMPLES_PER_FRAME;

    for (uint16_t i = 0; i < src_count; i++) {
        int32_t mono = ((int32_t)eb_stereo_scratch[i * 2] +
                        (int32_t)eb_stereo_scratch[i * 2 + 1]) >> 1;
        dst[i] = (int16_t)((mono * factor) >> 8);
    }
}
