/*
 * G&W audio platform — minimal stub for the title-screen MVP.
 *
 * Audio is intentionally disabled for first-boot bring-up (ENABLE_AUDIO=OFF
 * at the libgame CMake level, but here we still need to provide the
 * platform_audio_* symbols so the build links). When audio is re-enabled,
 * route through the launcher's SAI peripheral via audio_get_active_buffer
 * + audio_start_playing(), matching the zelda3/smw cadence (16 kHz, ~534
 * samples/frame at 30 fps).
 *
 * G&W is single-threaded, so the audio_lock/unlock pair is a no-op.
 */

#include "platform/platform.h"

bool platform_audio_init(void)
{
    return true;
}

void platform_audio_shutdown(void) {}

void platform_audio_lock(void) {}

void platform_audio_unlock(void) {}
