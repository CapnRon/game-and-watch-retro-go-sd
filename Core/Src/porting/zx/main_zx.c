#include <odroid_system.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "appid.h"
#include "common.h"
#include "gw_buttons.h"
#include "gw_lcd.h"
#include "gw_linker.h"
#include "gw_malloc.h"
#include "lzma.h"
#include "main.h"
#include "main_zx.h"
#include "odroid_overlay.h"
#include "rg_storage.h"
#include "rg_utils.h"
#include "zx-roms.h"
#include "zx_keymaps.h"

#define SPEAKER_PIN 0

static inline void vram_set_dirty_bitmap(uint16_t addr) {(void) addr;}
static inline void vram_set_dirty_attr(uint16_t addr) {(void) addr;}
static inline void vram_force_dirty(void) {}

/* zx2040 Z80 shares symbol names with SMS Plus GX; rename in this TU only. */
#define z80_init zx2040_z80_init
#define z80_reset zx2040_z80_reset
#define z80_tick zx2040_z80_tick
#define z80_prefetch zx2040_z80_prefetch

#define CHIPS_IMPL
#include "chips_common.h"
#include "mem.h"
#include "z80.h"
#include "kbd.h"
#include "clk.h"
#include "zx.h"

#define ZX_FPS 50
#define ZX_AUDIO_SAMPLE_RATE 22050
#define ZX_AUDIO_BUFFER_LENGTH (ZX_AUDIO_SAMPLE_RATE / ZX_FPS)
#define ZX_ROM_SIZE (16 * 1024)
#define ZX_ROM_UNPACK_BUFFER_SIZE (192 * 1024)
#define ZX_KEYMAP_MAX_BYTES (3 * 100)
#define ZX_KEYMAP_LINE_MAX_BYTES 128
#define ZX_FRAME_USEC 20000
#define ZX_AUDIO_FRAME_BITS 4375
#define ZX_AUDIO_RING_BITS (AUDIOBUF_LEN * 32)
#define ZX_AUDIO_GAIN 4096
#define ZX_SAVE_MAGIC 0x585A4757u
#define ZX_SAVE_VERSION 2u
#define ZX_DEFAULT_SCANLINE_PERIOD 150

enum {
    ZX_PAD_LEFT = 0,
    ZX_PAD_RIGHT,
    ZX_PAD_UP,
    ZX_PAD_DOWN,
    ZX_PAD_A,
    ZX_PAD_B,
    ZX_PAD_FIRE,
    ZX_PAD_GAME,
    ZX_PAD_TIME,
};

#define ZX_KEMPSTONE_FIRE  0xFF
#define ZX_KEMPSTONE_LEFT  0xFE
#define ZX_KEMPSTONE_RIGHT 0xFD
#define ZX_KEMPSTONE_DOWN  0xFC
#define ZX_KEMPSTONE_UP    0xFB

#define ZX_PRESS_AT_TICK   0xFE
#define ZX_RELEASE_AT_TICK 0xFD
#define ZX_KEY_END         0xFF
#define ZX_KEY_EXT         0x80

typedef struct {
    z80_t cpu;
    zx_type_t type;
    zx_joystick_type_t joystick_type;
    bool memory_paging_disabled;
    uint8_t kbd_joymask;
    uint8_t joy_joymask;
    uint32_t tick_count;
    uint8_t last_mem_config;
    uint8_t last_fe_out;
    uint8_t blink_counter;
    uint8_t border_color;
    int frame_scan_lines;
    int top_border_scanlines;
    int scanline_period;
    int scanline_counter;
    int scanline_y;
    int beeper_state;
    int int_counter;
    uint32_t display_ram_bank;
    kbd_t kbd;
    uint64_t pins;
    uint64_t freq_hz;
    bool valid;
    uint8_t ram[3][0x4000];
} zx_machine_state_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t show_border;
    uint32_t scanline_period;
    char profile_name[32];
    uint8_t keymap[ZX_KEYMAP_MAX_BYTES];
    zx_machine_state_t snapshot;
} zx_save_blob_t;

static zx_t zx;
static uint8_t zx_rom_buffer[ZX_ROM_UNPACK_BUFFER_SIZE];
static uint16_t zx_palette[16];
static uint32_t zx_tick_counter;
static uint8_t zx_keymap[ZX_KEYMAP_MAX_BYTES];
static char zx_profile_name[32];
static uint8_t zx_framebuffer[256 * 192];
static zx_save_blob_t zx_save_buffer;
static uint32_t zx_show_border = 0;
static int32_t zx_audio_dc_level = 0;
static bool zx_audio_dc_primed = false;
static uint32_t zx_audio_last_write_pos = 0;
static bool zx_audio_write_pos_primed = false;
static uint8_t zx_audio_prime_frames = 0;
static uint8_t zx_audio_start_frames = 0;
static bool zx_audio_started = false;
static char border_name[8];
static char scanline_name[8];
static char profile_name[32];
static char key_name[12];

struct zx_key_info {
    uint8_t key_id;
    const char *name;
    bool auto_release;
};

static const struct zx_key_info zx_keyboard[] = {
    {'0', "0", true}, {'1', "1", true}, {'2', "2", true}, {'3', "3", true}, {'4', "4", true},
    {'5', "5", true}, {'6', "6", true}, {'7', "7", true}, {'8', "8", true}, {'9', "9", true},
    {'a', "A", true}, {'b', "B", true}, {'c', "C", true}, {'d', "D", true}, {'e', "E", true},
    {'f', "F", true}, {'g', "G", true}, {'h', "H", true}, {'i', "I", true}, {'j', "J", true},
    {'k', "K", true}, {'l', "L", true}, {'m', "M", true}, {'n', "N", true}, {'o', "O", true},
    {'p', "P", true}, {'q', "Q", true}, {'r', "R", true}, {'s', "S", true}, {'t', "T", true},
    {'u', "U", true}, {'v', "V", true}, {'w', "W", true}, {'x', "X", true}, {'y', "Y", true},
    {'z', "Z", true}, {' ', "Space", true}, {0x0D, "Enter", true},
};

#define ZX_RELEASE_KEY_DELAY 5
static int zx_selected_key_index = 0;
static const struct zx_key_info *zx_pressed_key = NULL;
static const struct zx_key_info *zx_release_key = NULL;
static int zx_release_key_delay = ZX_RELEASE_KEY_DELAY;
static bool zx_update_border_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat);
static bool zx_update_scanline_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat);
static bool zx_update_keyboard_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat);
static odroid_dialog_choice_t zx_options[] = {
    {100, "Border", border_name, 1, &zx_update_border_cb},
    {101, "Scanline", scanline_name, 1, &zx_update_scanline_cb},
    {102, "Profile", profile_name, -1, NULL},
    {103, "Press Key", key_name, 1, &zx_update_keyboard_cb},
    ODROID_DIALOG_CHOICE_LAST
};

static int zx_button_pressed(odroid_gamepad_state_t *joystick, uint8_t button_id) {
    switch (button_id) {
        case ZX_PAD_LEFT: return joystick->values[ODROID_INPUT_LEFT];
        case ZX_PAD_RIGHT: return joystick->values[ODROID_INPUT_RIGHT];
        case ZX_PAD_UP: return joystick->values[ODROID_INPUT_UP];
        case ZX_PAD_DOWN: return joystick->values[ODROID_INPUT_DOWN];
        case ZX_PAD_A: return joystick->values[ODROID_INPUT_A];
        case ZX_PAD_B: return joystick->values[ODROID_INPUT_B];
        case ZX_PAD_FIRE:
            return joystick->values[ODROID_INPUT_A] || joystick->values[ODROID_INPUT_B];
        case ZX_PAD_GAME: return joystick->values[ODROID_INPUT_START];
        case ZX_PAD_TIME: return joystick->values[ODROID_INPUT_SELECT];
        default:
            return 0;
    }
}

static void zx_release_all_keys(void) {
    for (int i = 0; i < 256; ++i) {
        zx_key_up(&zx, i);
    }
}

static bool zx_is_virtual_key_down(uint8_t key_id) {
    if (zx_pressed_key && zx_pressed_key->key_id == key_id) {
        return true;
    }
    if (zx_release_key && zx_release_key->key_id == key_id) {
        return true;
    }
    return false;
}

static int zx_keymap_descr_to_row(char *p, uint8_t *map) {
    char buf[16];
    int idx = 0;

    while (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && idx < (int) sizeof(buf) - 1) {
        buf[idx++] = *p++;
    }
    buf[idx] = 0;

    int ext = 0;
    int atframe = 0;
    int pos = 0;
    for (int j = 0; j < 3; j++) {
        if (j == 0) {
            if (!ext && buf[0] == 'x') {
                ext = 1;
                pos++;
            } else if (!atframe && buf[0] == '@') {
                atframe = 1;
                map[0] = ZX_PRESS_AT_TICK;
                map[3] = ZX_RELEASE_AT_TICK;
                pos++;
            }
        }

        if (j == 1 && atframe) {
            char *frame = buf + pos;
            while (buf[pos] && buf[pos] != ':') {
                pos++;
            }
            if (buf[pos] == 0) {
                return 0;
            }
            buf[pos] = 0;
            pos++;
            map[j] = atoi(frame);
        } else if ((j == 0 && !atframe) || (j == 1 && ext)) {
            uint8_t pin;
            switch (buf[pos]) {
                case 'l': pin = ZX_PAD_LEFT; break;
                case 'r': pin = ZX_PAD_RIGHT; break;
                case 'u': pin = ZX_PAD_UP; break;
                case 'd': pin = ZX_PAD_DOWN; break;
                case 'a': pin = ZX_PAD_A; break;
                case 'b': pin = ZX_PAD_B; break;
                case 'f': pin = ZX_PAD_FIRE; break;
                case 'g': pin = ZX_PAD_GAME; break;
                case 't': pin = ZX_PAD_TIME; break;
                default: return 0;
            }
            if (ext && j == 0) {
                pin |= ZX_KEY_EXT;
            }
            map[j] = pin;
            pos++;
        } else if (j == 2 || (j == 1 && !ext)) {
            if (j == 2 && buf[pos] == 0) {
                break;
            }

            if (buf[pos] == '|') {
                pos++;
                switch (buf[pos]) {
                    case 'l': map[j] = ZX_KEMPSTONE_LEFT; break;
                    case 'r': map[j] = ZX_KEMPSTONE_RIGHT; break;
                    case 'u': map[j] = ZX_KEMPSTONE_UP; break;
                    case 'd': map[j] = ZX_KEMPSTONE_DOWN; break;
                    case 'f': map[j] = ZX_KEMPSTONE_FIRE; break;
                    default: return 0;
                }
            } else {
                if (buf[pos] == '~') {
                    buf[pos] = ' ';
                }
                if (buf[pos] == '^') {
                    map[j] = 0x0D;
                } else {
                map[j] = buf[pos];
                }
            }
            pos++;

            if (atframe) {
                map[5] = map[2];
                map[4] = map[1] + atoi(buf + pos);
            }
        }
    }

    return atframe ? 6 : 3;
}

static void zx_load_profile_name(const char *line, const char *prefix) {
    const size_t prefix_len = strlen(prefix);
    const char *start = line + prefix_len;
    const char *end = start;

    while (*end && *end != '\r' && *end != '\n') {
        end++;
    }

    size_t len = (size_t) (end - start);
    if (len >= sizeof(zx_profile_name)) {
        len = sizeof(zx_profile_name) - 1;
    }

    memcpy(zx_profile_name, start, len);
    zx_profile_name[len] = 0;
}

static bool zx_load_sidecar_keymap(const uint8_t *data, uint32_t size) {
    uint8_t *map = zx_keymap;
    uint32_t offset = 0;
    int line = 1;

    memset(zx_keymap, 0, sizeof(zx_keymap));
    strcpy(zx_profile_name, "Custom");
    zx.scanline_period = ZX_DEFAULT_SCANLINE_PERIOD;

    while (offset < size) {
        char linebuf[ZX_KEYMAP_LINE_MAX_BYTES];
        uint32_t start = offset;
        uint32_t len = 0;
        char *text;

        while (offset < size && data[offset] != '\n' && data[offset] != '\r') {
            if (len + 1 < sizeof(linebuf)) {
                linebuf[len++] = (char) data[offset];
            }
            offset++;
        }
        linebuf[len] = 0;

        while (offset < size && (data[offset] == '\n' || data[offset] == '\r')) {
            offset++;
        }

        text = linebuf;
        while (*text == ' ' || *text == '\t') {
            text++;
        }

        if (*text == 0 || *text == '#') {
            line++;
            continue;
        }

        if (!memcmp(text, "PROFILE:", 8)) {
            zx_load_profile_name(text, "PROFILE:");
            line++;
            continue;
        }

        if (!memcmp(text, "MATCH:", 6)) {
            zx_load_profile_name(text, "MATCH:");
            line++;
            continue;
        }

        if (!memcmp(text, "SCANLINE-PERIOD:", 16)) {
            int period = atoi(text + 16);
            if (period > 0 && period < 500) {
                zx.scanline_period = period;
            }
            line++;
            continue;
        }

        if (!memcmp(text, "DEFAULT:", 8) || !memcmp(text, "#START", 6) || !memcmp(text, "#END", 4)) {
            line++;
            continue;
        }

        int used_bytes = zx_keymap_descr_to_row(text, map);
        if (used_bytes == 0) {
            printf("ZX sidecar keymap syntax error on line %d\n", line);
            printf("ZX sidecar source offset %u\n", (unsigned int) start);
            *map = ZX_KEY_END;
            strcpy(zx_profile_name, "Invalid");
            return false;
        }
        map += used_bytes;
        if (map > zx_keymap + sizeof(zx_keymap) - 3) {
            map -= 3;
            *map = ZX_KEY_END;
            printf("ZX sidecar keymap too large\n");
            strcpy(zx_profile_name, "Invalid");
            return false;
        }

        line++;
    }

    *map = ZX_KEY_END;
    return true;
}

static bool zx_try_load_sidecar_keymap_from_path(const char *game_path) {
    char cfg_path[RG_PATH_MAX];
    char *dot;
    FILE *file;
    uint8_t buf[4096];
    size_t size;

    if (!game_path || !game_path[0]) {
        return false;
    }

    strncpy(cfg_path, game_path, sizeof(cfg_path) - 1);
    cfg_path[sizeof(cfg_path) - 1] = '\0';
    dot = strrchr(cfg_path, '.');
    if (!dot) {
        return false;
    }
    snprintf(dot, sizeof(cfg_path) - (size_t)(dot - cfg_path), ".cfg");

    file = fopen(cfg_path, "rb");
    if (!file) {
        return false;
    }

    size = fread(buf, 1, sizeof(buf), file);
    fclose(file);
    if (size == 0) {
        return false;
    }

    if (zx_load_sidecar_keymap(buf, (uint32_t)size)) {
        return true;
    }

    printf("ZX sidecar keymap invalid in %s, falling back to built-in profiles\n", cfg_path);
    return false;
}

static void zx_select_keymap(void) {
    if (zx_try_load_sidecar_keymap_from_path(ACTIVE_FILE->path)) {
        return;
    }

    int got_match = 0;
    uint8_t *map = zx_keymap;
    char *text = (char *) zx_keymaps_text;
    int line = 1;

    memset(zx_keymap, 0, sizeof(zx_keymap));
    strcpy(zx_profile_name, "Default Kempston");
    zx.scanline_period = ZX_DEFAULT_SCANLINE_PERIOD;

    while (1) {
        if (*text == '#' || *text == '\r' || *text == '\n' || *text == ' ' || *text == '\t') {
            if (got_match) {
                *map = ZX_KEY_END;
                return;
            }
            goto next_line;
        }

        if ((!memcmp(text, "MATCH:", 6) && !got_match) ||
            (!memcmp(text, "AND-MATCH", 9) && got_match)) {
            char *end = strchr(text, '\n');
            char *pattern;
            unsigned int pattern_len;
            int found = 0;

            if (*(end - 1) == '\r') {
                end--;
            }

            pattern = strchr(text, ':') + 1;
            pattern_len = (unsigned int) (end - pattern);

            for (uint32_t i = 0; i < 49152 - pattern_len; i++) {
                uint8_t *ram = (uint8_t *) zx.ram;
                if (ram[i] == (uint8_t) pattern[0] && !memcmp(ram + i, pattern, pattern_len)) {
                    found = 1;
                    break;
                }
            }

            got_match = found;
            if (got_match && !memcmp(text, "MATCH:", 6)) {
                zx_load_profile_name(text, "MATCH:");
            }
            goto next_line;
        }

        if (!memcmp(text, "DEFAULT:", 8)) {
            got_match = 1;
            strcpy(zx_profile_name, "Default Kempston");
            goto next_line;
        }

        if (!got_match) {
            goto next_line;
        }

        if (!memcmp(text, "SCANLINE-PERIOD:", 16)) {
            int period = atoi(text + 16);
            if (period > 0 && period < 500) {
                zx.scanline_period = period;
            }
            goto next_line;
        }

        int used_bytes = zx_keymap_descr_to_row(text, map);
        if (used_bytes == 0) {
            printf("ZX keymap syntax error on line %d\n", line);
            *map = ZX_KEY_END;
            strcpy(zx_profile_name, "Invalid");
            return;
        }
        map += used_bytes;
        if (map > zx_keymap + sizeof(zx_keymap) - 3) {
            map -= 3;
            *map = ZX_KEY_END;
            return;
        }

next_line:
        line++;
        if (!memcmp(text, "#END", 4)) {
            *map = ZX_KEY_END;
            return;
        }
        text = strchr(text, '\n');
        if (!text) {
            *map = ZX_KEY_END;
            return;
        }
        text++;
    }
}

static void zx_handle_key_press(odroid_gamepad_state_t *joystick, const uint8_t *keymap, uint32_t ticks) {
    uint64_t put_down[4] = {0, 0, 0, 0};

#define PUT_DOWN_SET(code) put_down[(code) >> 6] |= (1ULL << ((code) & 63))
#define PUT_DOWN_GET(code) (put_down[(code) >> 6] & (1ULL << ((code) & 63)))

    for (int j = 0;; j += 3) {
        if (keymap[j] == ZX_KEY_END) {
            break;
        } else if (keymap[j] == ZX_PRESS_AT_TICK || keymap[j] == ZX_RELEASE_AT_TICK) {
            if (keymap[j + 1] != ticks) {
                continue;
            }
            if (keymap[j] == ZX_PRESS_AT_TICK) {
                zx_key_down(&zx, keymap[j + 2]);
            } else {
                zx_key_up(&zx, keymap[j + 2]);
            }
        } else if (!(keymap[j] & ZX_KEY_EXT)) {
            if (zx_button_pressed(joystick, keymap[j])) {
                if (keymap[j + 1]) {
                    PUT_DOWN_SET(keymap[j + 1]);
                    zx_key_down(&zx, keymap[j + 1]);
                }
                if (keymap[j + 2]) {
                    PUT_DOWN_SET(keymap[j + 2]);
                    zx_key_down(&zx, keymap[j + 2]);
                }
            } else {
                if (keymap[j + 1] && !PUT_DOWN_GET(keymap[j + 1])) {
                    zx_key_up(&zx, keymap[j + 1]);
                }
                if (keymap[j + 2] && !PUT_DOWN_GET(keymap[j + 2])) {
                    zx_key_up(&zx, keymap[j + 2]);
                }
            }
        } else {
            if (zx_button_pressed(joystick, keymap[j] & 0x7f) &&
                zx_button_pressed(joystick, keymap[j + 1])) {
                PUT_DOWN_SET(keymap[j + 2]);
                zx_key_down(&zx, keymap[j + 2]);
                return;
            } else if (keymap[j + 2] && !PUT_DOWN_GET(keymap[j + 2])) {
                zx_key_up(&zx, keymap[j + 2]);
            }
        }
    }
}

static void zx_handle_virtual_key_press(void) {
    if (zx_pressed_key != NULL) {
        zx_key_down(&zx, zx_pressed_key->key_id);
        if (zx_pressed_key->auto_release) {
            zx_release_key = zx_pressed_key;
            zx_release_key_delay = ZX_RELEASE_KEY_DELAY;
        }
        zx_pressed_key = NULL;
    } else if (zx_release_key != NULL) {
        if (zx_release_key_delay == 0) {
            zx_key_up(&zx, zx_release_key->key_id);
            zx_release_key = NULL;
            zx_release_key_delay = ZX_RELEASE_KEY_DELAY;
        } else {
            zx_release_key_delay--;
        }
    }
}

static inline uint16_t zx_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void zx_init_palette(void) {
    static const uint8_t palette[16][3] = {
        {0x00, 0x00, 0x00}, {0x00, 0x00, 0xD8}, {0xD8, 0x00, 0x00}, {0xD8, 0x00, 0xD8},
        {0x00, 0xD8, 0x00}, {0x00, 0xD8, 0xD8}, {0xD8, 0xD8, 0x00}, {0xD8, 0xD8, 0xD8},
        {0x00, 0x00, 0x00}, {0x00, 0x00, 0xFF}, {0xFF, 0x00, 0x00}, {0xFF, 0x00, 0xFF},
        {0x00, 0xFF, 0x00}, {0x00, 0xFF, 0xFF}, {0xFF, 0xFF, 0x00}, {0xFF, 0xFF, 0xFF},
    };

    for (int i = 0; i < 16; ++i) {
        zx_palette[i] = zx_rgb565(palette[i][0], palette[i][1], palette[i][2]);
    }
}

static inline uint8_t zx_get_pixel(uint32_t x, uint32_t y, int blink) {
    uint8_t *vmem = zx.ram[zx.display_ram_bank];
    uint32_t byte_index = ((y & 0xC0) << 5) | ((y & 0x07) << 8) | ((y & 0x38) << 2) | (x >> 3);
    uint8_t pixel_byte = vmem[byte_index];
    uint8_t attr = vmem[0x1800 + ((y >> 3) << 5) + (x >> 3)];
    uint8_t ink = attr & 7;
    uint8_t paper = (attr >> 3) & 7;

    if (attr & 0x40) {
        ink += 8;
        paper += 8;
    }
    if ((attr & 0x80) && blink) {
        uint8_t tmp = ink;
        ink = paper;
        paper = tmp;
    }

    return (pixel_byte & (0x80 >> (x & 7))) ? ink : paper;
}

static void zx_render_active_area(void) {
    uint16_t *dst = lcd_get_active_buffer();
    int blink = (zx.blink_counter & 0x10) != 0;

    for (int y = 0; y < 192; ++y) {
        for (int x = 0; x < 256; ++x) {
            zx_framebuffer[y * 256 + x] = zx_get_pixel((uint32_t) x, (uint32_t) y, blink);
        }
    }

    for (int y = 0; y < 240; ++y) {
        int src_y = (y * 192) / 240;
        for (int x = 0; x < 320; ++x) {
            int src_x = (x * 256) / 320;
            dst[y * GW_LCD_WIDTH + x] = zx_palette[zx_framebuffer[src_y * 256 + src_x]];
        }
    }
}

static void zx_render_with_border(void) {
    uint16_t *dst = lcd_get_active_buffer();
    int blink = (zx.blink_counter & 0x10) != 0;
    uint16_t border = zx_palette[zx.border_color & 7];

    for (int y = 0; y < 240; ++y) {
        int src_y = y + 8;
        for (int x = 0; x < 320; ++x) {
            if (src_y < 32 || src_y >= 224 || x < 32 || x >= 288) {
                dst[y * GW_LCD_WIDTH + x] = border;
            } else {
                dst[y * GW_LCD_WIDTH + x] = zx_palette[zx_get_pixel((uint32_t) (x - 32), (uint32_t) (src_y - 32), blink)];
            }
        }
    }
}

static void zx_blit(void) {
    if (zx_show_border) {
        zx_render_with_border();
    } else {
        zx_render_active_area();
    }
    common_ingame_overlay();
}

static int zx_get_audio_bit(uint32_t absolute_bit) {
    uint32_t absolute = absolute_bit % ZX_AUDIO_RING_BITS;
    uint32_t word = absolute / 32;
    uint32_t bit = absolute & 31;
    return (zx.audiobuf[word] >> bit) & 1u;
}

static void zx_audio_reset_stream_state(void) {
    zx_audio_dc_level = 0;
    zx_audio_dc_primed = false;
    zx_audio_last_write_pos = (zx.audiobuf_byte * 32 + zx.audiobuf_bit) % ZX_AUDIO_RING_BITS;
    zx_audio_write_pos_primed = false;
}

static void zx_capture_machine_state(zx_machine_state_t *dst) {
    dst->cpu = zx.cpu;
    dst->type = zx.type;
    dst->joystick_type = zx.joystick_type;
    dst->memory_paging_disabled = zx.memory_paging_disabled;
    dst->kbd_joymask = zx.kbd_joymask;
    dst->joy_joymask = zx.joy_joymask;
    dst->tick_count = zx.tick_count;
    dst->last_mem_config = zx.last_mem_config;
    dst->last_fe_out = zx.last_fe_out;
    dst->blink_counter = zx.blink_counter;
    dst->border_color = zx.border_color;
    dst->frame_scan_lines = zx.frame_scan_lines;
    dst->top_border_scanlines = zx.top_border_scanlines;
    dst->scanline_period = zx.scanline_period;
    dst->scanline_counter = zx.scanline_counter;
    dst->scanline_y = zx.scanline_y;
    dst->beeper_state = zx.beeper_state;
    dst->int_counter = zx.int_counter;
    dst->display_ram_bank = zx.display_ram_bank;
    dst->kbd = zx.kbd;
    dst->pins = zx.pins;
    dst->freq_hz = zx.freq_hz;
    dst->valid = zx.valid;
    memcpy(dst->ram, zx.ram, sizeof(dst->ram));
}

static void zx_restore_machine_state(const zx_machine_state_t *src) {
    zx.cpu = src->cpu;
    zx.type = src->type;
    zx.joystick_type = src->joystick_type;
    zx.memory_paging_disabled = src->memory_paging_disabled;
    zx.kbd_joymask = src->kbd_joymask;
    zx.joy_joymask = src->joy_joymask;
    zx.tick_count = src->tick_count;
    zx.last_mem_config = src->last_mem_config;
    zx.last_fe_out = src->last_fe_out;
    zx.blink_counter = src->blink_counter;
    zx.border_color = src->border_color;
    zx.frame_scan_lines = src->frame_scan_lines;
    zx.top_border_scanlines = src->top_border_scanlines;
    zx.scanline_period = src->scanline_period;
    zx.scanline_counter = src->scanline_counter;
    zx.scanline_y = src->scanline_y;
    zx.beeper_state = src->beeper_state;
    zx.int_counter = src->int_counter;
    zx.display_ram_bank = src->display_ram_bank;
    zx.kbd = src->kbd;
    zx.pins = src->pins;
    zx.freq_hz = src->freq_hz;
    zx.valid = src->valid;
    memcpy(zx.ram, src->ram, sizeof(zx.ram));

    memset(zx.audiobuf, 0, sizeof(zx.audiobuf));
    zx.audiobuf_byte = 0;
    zx.audiobuf_bit = 0;
    zx.audiobuf_notify = 0;
    _zx_init_memory_map(&zx);
}

static void zx_submit_audio(void) {
    uint32_t write_pos = (zx.audiobuf_byte * 32 + zx.audiobuf_bit) % ZX_AUDIO_RING_BITS;

    if (common_emu_sound_loop_is_muted()) {
        zx_audio_reset_stream_state();
        zx_audio_last_write_pos = write_pos;
        zx_audio_write_pos_primed = true;
        return;
    }

    int16_t *sound_buffer = audio_get_active_buffer();
    uint16_t sound_buffer_length = audio_get_buffer_length();
    int32_t factor = common_emu_sound_get_volume();
    uint32_t produced_bits;

    if (zx_audio_write_pos_primed) {
        produced_bits = (write_pos + ZX_AUDIO_RING_BITS - zx_audio_last_write_pos) % ZX_AUDIO_RING_BITS;
        if (produced_bits == 0) {
            produced_bits = ZX_AUDIO_FRAME_BITS;
        }
    } else {
        produced_bits = ZX_AUDIO_FRAME_BITS;
        zx_audio_write_pos_primed = true;
    }
    zx_audio_last_write_pos = write_pos;

    uint32_t start_pos = (write_pos + ZX_AUDIO_RING_BITS - produced_bits) % ZX_AUDIO_RING_BITS;

    for (uint32_t i = 0; i < sound_buffer_length; ++i) {
        uint32_t first = (i * produced_bits) / sound_buffer_length;
        uint32_t last = ((i + 1) * produced_bits) / sound_buffer_length;
        uint32_t count = last - first;
        int32_t sum = 0;

        if (count == 0) {
            count = 1;
            last = first + 1;
        }

        for (uint32_t j = first; j < last; ++j) {
            sum += zx_get_audio_bit(start_pos + j) ? 1 : -1;
        }
        int32_t raw_sample = (sum * ZX_AUDIO_GAIN) / (int32_t) count;

        // The ZX beeper is a 1-bit signal. After reconstruction it can carry a
        // large DC component when the line sits high or low for a while, which
        // this audio path does not tolerate well. Track and remove that slowly
        // varying offset so steady levels decay back to silence.
        if (!zx_audio_dc_primed) {
            zx_audio_dc_level = raw_sample;
            zx_audio_dc_primed = true;
        } else {
            zx_audio_dc_level += (raw_sample - zx_audio_dc_level) >> 5;
        }

        sound_buffer[i] = (int16_t) (((raw_sample - zx_audio_dc_level) * factor) >> 8);
    }
}

static bool zx_save_state_to_ptr(uint8_t *dest) {
    zx_save_blob_t *blob = (zx_save_blob_t *) dest;

    if (!dest) {
        return false;
    }

    memset(blob, 0, sizeof(*blob));
    blob->magic = ZX_SAVE_MAGIC;
    blob->version = ZX_SAVE_VERSION;
    blob->show_border = zx_show_border;
    blob->scanline_period = (uint32_t) zx.scanline_period;
    memcpy(blob->profile_name, zx_profile_name, sizeof(blob->profile_name));
    memcpy(blob->keymap, zx_keymap, sizeof(blob->keymap));
    zx_capture_machine_state(&blob->snapshot);
    return true;
}

static bool zx_load_state_from_ptr(const uint8_t *src) {
    const zx_save_blob_t *blob = (const zx_save_blob_t *) src;

    if (blob->magic != ZX_SAVE_MAGIC || blob->version != ZX_SAVE_VERSION) {
        return false;
    }
    zx_restore_machine_state(&blob->snapshot);

    zx_show_border = blob->show_border ? 1u : 0u;
    zx.scanline_period = (int) blob->scanline_period;
    memcpy(zx_profile_name, blob->profile_name, sizeof(zx_profile_name));
    memcpy(zx_keymap, blob->keymap, sizeof(zx_keymap));
    return true;
}

static bool zx_system_LoadState(const char *pathName) {
    FILE *file;

    if (!pathName) {
        return false;
    }

    file = fopen(pathName, "rb");
    if (!file) {
        return false;
    }

    if (fread(&zx_save_buffer, 1, sizeof(zx_save_buffer), file) != sizeof(zx_save_buffer)) {
        fclose(file);
        return false;
    }
    fclose(file);

    return zx_load_state_from_ptr((const uint8_t *)&zx_save_buffer);
}

static bool zx_system_SaveState(const char *pathName) {
    FILE *file;

    if (!pathName || !zx_save_state_to_ptr((uint8_t *)&zx_save_buffer)) {
        return false;
    }

    if (!rg_storage_mkdir(rg_dirname(pathName))) {
        return false;
    }

    file = fopen(pathName, "wb");
    if (!file) {
        return false;
    }

    if (fwrite(&zx_save_buffer, 1, sizeof(zx_save_buffer), file) != sizeof(zx_save_buffer)) {
        fclose(file);
        return false;
    }
    fclose(file);
    return true;
}

static bool zx_update_border_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat) {
    (void) repeat;
    if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT) {
        zx_show_border ^= 1u;
    }
    strcpy(option->value, zx_show_border ? "On" : "Off");
    return event == ODROID_DIALOG_ENTER;
}

static bool zx_update_scanline_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat) {
    (void) repeat;
    if (event == ODROID_DIALOG_PREV && zx.scanline_period > 60) {
        zx.scanline_period -= 5;
    }
    if (event == ODROID_DIALOG_NEXT && zx.scanline_period < 300) {
        zx.scanline_period += 5;
    }
    snprintf(option->value, 8, "%d", zx.scanline_period);
    return event == ODROID_DIALOG_ENTER;
}

static bool zx_update_keyboard_cb(odroid_dialog_choice_t *option, odroid_dialog_event_t event, uint32_t repeat) {
    (void) repeat;
    const int max_index = (int) (sizeof(zx_keyboard) / sizeof(zx_keyboard[0])) - 1;

    if (event == ODROID_DIALOG_PREV) {
        zx_selected_key_index = zx_selected_key_index > 0 ? zx_selected_key_index - 1 : max_index;
    }
    if (event == ODROID_DIALOG_NEXT) {
        zx_selected_key_index = zx_selected_key_index < max_index ? zx_selected_key_index + 1 : 0;
    }

    if (zx_is_virtual_key_down(zx_keyboard[zx_selected_key_index].key_id)) {
        option->value[0] = '*';
        strncpy(option->value + 1, zx_keyboard[zx_selected_key_index].name, sizeof(key_name) - 2);
        option->value[sizeof(key_name) - 1] = 0;
    } else {
        strncpy(option->value, zx_keyboard[zx_selected_key_index].name, sizeof(key_name) - 1);
        option->value[sizeof(key_name) - 1] = 0;
    }

    if (event == ODROID_DIALOG_ENTER) {
        zx_pressed_key = &zx_keyboard[zx_selected_key_index];
    }
    return event == ODROID_DIALOG_ENTER;
}

static size_t zx_get_game_data(uint8_t **data) {
    uint32_t size = ACTIVE_FILE->size;

    *data = NULL;
    if (!ACTIVE_FILE->path[0] || size == 0) {
        return 0;
    }

    ram_start = (uint32_t)&_OVERLAY_ZX_BSS_END;
    if (size > ram_get_free_size()) {
        *data = odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &size, false);
    } else {
        *data = ram_malloc(size);
        if (*data != NULL) {
            if (odroid_overlay_cache_file_in_ram(ACTIVE_FILE->path, *data) != size) {
                *data = odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &size, false);
            }
        } else {
            *data = odroid_overlay_cache_file_in_flash(ACTIVE_FILE->path, &size, false);
        }
    }

    if (*data == NULL || size == 0) {
        printf("ZX failed to load %s\n", ACTIVE_FILE->path);
        return 0;
    }

    if (ACTIVE_FILE->ext && strcasecmp(ACTIVE_FILE->ext, "lzma") == 0) {
        size_t out_len = lzma_inflate(zx_rom_buffer, sizeof(zx_rom_buffer), *data, size);
        *data = zx_rom_buffer;
        return out_len;
    }

    return size;
}

static const uint8_t *zx_get_bios_data(void) {
    static uint8_t bios_buffer[ZX_ROM_SIZE];
    static bool initialized = false;

    if (!initialized) {
        FILE *file = fopen("/bios/zx/spectrum.rom", "rb");
        initialized = true;
        if (file && fread(bios_buffer, 1, ZX_ROM_SIZE, file) == ZX_ROM_SIZE) {
            fclose(file);
            return bios_buffer;
        }
        if (file) {
            fclose(file);
        }
        memcpy(bios_buffer, dump_amstrad_zx48k_bin, ZX_ROM_SIZE);
    }

    return bios_buffer;
}

__attribute__((noinline))
static void app_main_zx_impl(uint8_t load_state, uint8_t start_paused, int8_t save_slot) {
    const uint8_t *bios_data;
    uint8_t *game_data;
    size_t game_size;
    odroid_gamepad_state_t joystick;

    bios_data = zx_get_bios_data();

    if (!bios_data) {
        printf("ZX BIOS missing\n");
        return;
    }

    if (start_paused) {
        common_emu_state.pause_after_frames = 2;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }

    common_emu_state.frame_time_10us = (uint16_t) (100000 / ZX_FPS + 0.5f);
    lcd_set_refresh_rate(ZX_FPS);
    lcd_clear_buffers();

    wdog_refresh();
    odroid_system_init(APPID_ZX, ZX_AUDIO_SAMPLE_RATE);
    wdog_refresh();

    odroid_system_emu_init(&zx_system_LoadState, &zx_system_SaveState, NULL, NULL, NULL, NULL);
    wdog_refresh();

    zx_init_palette();
    wdog_refresh();

    zx_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.type = ZX_TYPE_48K;
    desc.joystick_type = ZX_JOYSTICKTYPE_KEMPSTON;
    desc.roms.zx48k.ptr = (void *) bios_data;
    desc.roms.zx48k.size = ZX_ROM_SIZE;

    wdog_refresh();
    zx_init(&zx, &desc);
    zx.scanline_period = ZX_DEFAULT_SCANLINE_PERIOD;
    zx_show_border = 0;
    wdog_refresh();

    game_size = zx_get_game_data(&game_data);
    wdog_refresh();
    if (game_data == NULL || game_size == 0) {
        printf("ZX failed to load game data for %s\n", ACTIVE_FILE->path);
        return;
    }
    if (!zx_quickload(&zx, (chips_range_t) {.ptr = game_data, .size = game_size})) {
        printf("ZX quickload failed for %s\n", ACTIVE_FILE->name);
        return;
    }
    wdog_refresh();

    zx_select_keymap();
    wdog_refresh();

    zx_release_all_keys();
    zx_pressed_key = NULL;
    zx_release_key = NULL;
    zx_release_key_delay = ZX_RELEASE_KEY_DELAY;
    wdog_refresh();

    zx_audio_reset_stream_state();
    zx_audio_prime_frames = 0;
    zx_audio_start_frames = 2;
    zx_audio_started = false;
    wdog_refresh();

    if (load_state) {
        odroid_system_emu_load_state(save_slot);
        wdog_refresh();
    }

    common_emu_frame_loop_reset();
    while (1) {
        wdog_refresh();

        bool draw_frame = true;
        odroid_input_read_gamepad(&joystick);
        common_emu_input_loop(&joystick, zx_options, &zx_blit);
        common_emu_input_loop_handle_turbo(&joystick);

        zx_handle_key_press(&joystick, zx_keymap, zx_tick_counter);
        zx_handle_virtual_key_press();
        zx_exec(&zx, ZX_FRAME_USEC);

        strcpy(border_name, zx_show_border ? "On" : "Off");
        snprintf(scanline_name, sizeof(scanline_name), "%d", zx.scanline_period);
        snprintf(profile_name, sizeof(profile_name), "%.31s", zx_profile_name);
        zx_update_keyboard_cb(&zx_options[3], ODROID_DIALOG_INIT, 0);

        if (draw_frame) {
            zx_blit();
            lcd_swap();
            lcd_wait_for_vblank();
        }

        if (!zx_audio_started) {
            if (zx_audio_start_frames > 0) {
                zx_audio_start_frames--;
            }
            if (zx_audio_start_frames == 0) {
                zx_audio_reset_stream_state();
                audio_start_playing(ZX_AUDIO_BUFFER_LENGTH);
                zx_audio_prime_frames = 2;
                zx_audio_started = true;
            }
        }

        if (zx_audio_started) {
            // Prime the stream at startup so both DMA halves contain fresh ZX
            // audio before steady-state pacing takes over.
            if (zx_audio_prime_frames > 0) {
                zx_submit_audio();
                memcpy(audio_get_inactive_buffer(), audio_get_active_buffer(), audio_get_buffer_size());
                zx_audio_prime_frames--;
            } else {
                zx_submit_audio();
            }

            common_emu_sound_sync(false);
        }
        zx_tick_counter++;
    }
}

__attribute__((noinline))
void app_main_zx(uint8_t load_state, uint8_t start_paused, int8_t save_slot) {
    wdog_refresh();
    app_main_zx_impl(load_state, start_paused, save_slot);
}
