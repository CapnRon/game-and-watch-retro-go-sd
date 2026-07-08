#include <odroid_system.h>
#include <stdint.h>

#include "appid.h"
#include "common.h"
#include "gw_linker.h"
#include "gw_lcd.h"
#include "gw_malloc.h"

// Implemented inside external/durak.
int durak_run(void);

int app_main_durak(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
  (void)load_state;
  (void)save_slot;
  (void)start_paused;

  // Reserve overlay heap right after the durak BSS.
  ram_start = (uint32_t)&_OVERLAY_DURAK_BSS_END;

  odroid_system_init(APPID_HOMEBREW, 22050);
  odroid_system_emu_init(NULL, NULL, NULL, NULL, NULL, NULL);

  if (start_paused) {
    common_emu_state.pause_after_frames = 2;
  } else {
    common_emu_state.pause_after_frames = 0;
  }
  common_emu_state.frame_time_10us = (uint16_t)(100000 / 60 + 0.5f);

  lcd_clear_buffers();

  return durak_run();
}

