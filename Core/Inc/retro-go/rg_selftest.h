#pragma once

/* Runs the SD -> DTCM -> PSRAM -> execute data-path self-test.
 * Triggered at cold boot when /selftest.flag exists on the SD card.
 * Results go to the persistent log buffer (printf). */
void psram_selftest_run(void);
