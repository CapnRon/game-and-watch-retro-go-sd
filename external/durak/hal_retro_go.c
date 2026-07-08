#include "hal.h"
#include <odroid_audio.h>
#include <odroid_display.h>
#include <odroid_input.h>
#include <odroid_overlay.h>
#include <odroid_system.h>
#include "common.h"
#include "gw_audio.h"
#include "gw_lcd.h"
#include "main.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// Вбудовані ассети (генеруються convert.py -> assets.c).
// Формат: 8-біт індекси кольору (1 байт/піксель) + окрема палітра RGB565.
// ---------------------------------------------------------------------------
extern const unsigned char  asset_title_idx[42 * 62];
extern const unsigned short asset_title_pal[];
extern const unsigned char  asset_cards_idx[1442 * 62];
extern const unsigned short asset_cards_pal[];
extern const unsigned char  asset_font_idx[128 * 64];
extern const unsigned short asset_font_pal[];
extern const unsigned char  asset_faces_idx[642 * 34];
extern const unsigned short asset_faces_pal[];

struct HalTexture {
    const uint8_t*  indices;  // 1 байт/піксель - індекс у палітрі
    const uint16_t* palette;  // RGB565, palette[i] == 0x0000 => прозорий
    int stride_w;
    int stride_h;
    int src_off_x;
    int src_off_y;
    int width;
    int height;
};

// Реєстр "ім'я файлу -> вбудований масив", щоб main.c міг і надалі
// викликати hal_load_texture("cards.png") як і в SDL2-версії.
static const struct {
    const char* name;
    const uint8_t* idx;
    const uint16_t* pal;
    int w, h;
    int sx, sy;
    int cw, ch;
} asset_table[] = {
    // title/cards/faces were exported with a 1px transparent border all around.
    { "title.png", asset_title_idx, asset_title_pal, 42,   62, 1, 1, 40,   60 },
    { "cards.png", asset_cards_idx, asset_cards_pal, 1442, 62, 1, 1, 1440, 60 },
    { "font.png",  asset_font_idx,  asset_font_pal,  128,  64, 0, 0, 128,  64 },
    { "faces.png", asset_faces_idx, asset_faces_pal, 642,  34, 1, 1, 640,  32 },
};
#define ASSET_TABLE_COUNT (sizeof(asset_table) / sizeof(asset_table[0]))

// ---------------------------------------------------------------------------
// Кнопки
// ---------------------------------------------------------------------------
static uint32_t btn_state = 0;
static uint32_t btn_old_state = 0;
static bool pause_was_held = false;
static bool pause_macro_activated = false;
static int pause_last_key = -1;

// Audio path aligned with the emulator cores: one frame writes one DMA half-buffer.
#define DURAK_SAMPLE_RATE 22050
#define DURAK_FPS         60
#define DURAK_AUDIO_SAMPLES_PER_FRAME (DURAK_SAMPLE_RATE / DURAK_FPS)

static bool audio_started = false;
static int sfx_freq = 0;
static uint32_t sfx_remaining_ms = 0;
static uint32_t sfx_phase_sample = 0;

static void durak_audio_submit_frame(uint32_t frame_ms)
{
    if (!audio_started) {
        return;
    }

    if (common_emu_sound_loop_is_muted()) {
        common_emu_sound_sync(false);
        return;
    }

    int16_t *sound_buffer = audio_get_active_buffer();
    uint16_t sound_buffer_length = audio_get_buffer_length();
    int32_t volume_factor = common_emu_sound_get_volume();

    for (uint16_t i = 0; i < sound_buffer_length; i++) {
        int32_t sample = 0;
        if (sfx_remaining_ms > 0 && sfx_freq > 0) {
            uint32_t period = (uint32_t)DURAK_SAMPLE_RATE / (uint32_t)sfx_freq;
            if (period < 2) period = 2;
            uint32_t pos_in_period = sfx_phase_sample % period;
            sample = (pos_in_period < (period / 2)) ? 1800 : -1800;
            sfx_phase_sample++;
        }
        sound_buffer[i] = (int16_t)((sample * volume_factor) >> 8);
    }

    if (sfx_remaining_ms > 0) {
        if (sfx_remaining_ms > frame_ms) {
            sfx_remaining_ms -= frame_ms;
        } else {
            sfx_remaining_ms = 0;
            sfx_freq = 0;
            sfx_phase_sample = 0;
        }
    }

    common_emu_sound_sync(false);
}

static void hal_take_screenshot(void)
{
#if SD_CARD == 1
    uint8_t bmp_header[66] = {
        0x42, 0x4D,             // BM
        0x42, 0x58, 0x02, 0x00, // 66 + 320*240*2 = 153666
        0x00, 0x00,
        0x00, 0x00,
        0x42, 0x00, 0x00, 0x00, // pixel data offset
        0x28, 0x00, 0x00, 0x00, // DIB size
        0x40, 0x01, 0x00, 0x00, // width 320
        0xF0, 0x00, 0x00, 0x00, // height 240
        0x01, 0x00,
        0x10, 0x00,             // 16bpp
        0x03, 0x00, 0x00, 0x00, // BI_BITFIELDS
        0x00, 0x58, 0x02, 0x00, // image size
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0xF8, 0x00, 0x00, // R mask
        0xE0, 0x07, 0x00, 0x00, // G mask
        0x1F, 0x00, 0x00, 0x00  // B mask
    };

    rg_storage_mkdir(ODROID_BASE_PATH_SCREENSHOTS);
    char *file_path = odroid_system_get_path(ODROID_PATH_USER_SCREENSHOT, odroid_system_get_app()->romPath);
    FILE *file = fopen(file_path, "wb");
    if (!file) {
        free(file_path);
        return;
    }

    fwrite(bmp_header, 1, sizeof(bmp_header), file);
    lcd_sleep_while_swap_pending();
    uint8_t *data = (uint8_t*)lcd_get_inactive_buffer();
    for (int y = 239; y >= 0; y--) {
        fwrite(&data[y * 320 * 2], 1, 320 * 2, file);
    }
    fclose(file);
    free(file_path);
#endif
}

// We map our local Button enum to stable bit positions.
#define BTN_BIT_UP     (1u << 0)
#define BTN_BIT_DOWN   (1u << 1)
#define BTN_BIT_LEFT   (1u << 2)
#define BTN_BIT_RIGHT  (1u << 3)
#define BTN_BIT_A      (1u << 4)
#define BTN_BIT_B      (1u << 5)
#define BTN_BIT_START  (1u << 6)
#define BTN_BIT_SELECT (1u << 7)
#define BTN_BIT_QUIT   (1u << 8)

static uint32_t map_button_bit(Button btn) {
    switch (btn) {
        case BTN_UP:     return BTN_BIT_UP;
        case BTN_DOWN:   return BTN_BIT_DOWN;
        case BTN_LEFT:   return BTN_BIT_LEFT;
        case BTN_RIGHT:  return BTN_BIT_RIGHT;
        case BTN_A:      return BTN_BIT_A;
        case BTN_B:      return BTN_BIT_B;
        case BTN_START:  return BTN_BIT_START;
        case BTN_SELECT: return BTN_BIT_SELECT;
        case BTN_QUIT:   return BTN_BIT_QUIT;
        default:         return 0;
    }
}

// ---------------------------------------------------------------------------
// Звук
// ---------------------------------------------------------------------------
void hal_play_sound(SoundFx snd) {
    switch (snd) {
        case SND_CARD:    sfx_freq = 880; sfx_remaining_ms = 50;  break;
        case SND_DEAL:    sfx_freq = 440; sfx_remaining_ms = 100; break;
        case SND_ERROR:   sfx_freq = 200; sfx_remaining_ms = 200; break;
        case SND_FLIP:    sfx_freq = 600; sfx_remaining_ms = 100; break;
        case SND_SELECT:  sfx_freq = 700; sfx_remaining_ms = 50;  break;
        case SND_SHUFFLE: sfx_freq = 300; sfx_remaining_ms = 150; break;
        case SND_TAKE:    sfx_freq = 500; sfx_remaining_ms = 150; break;
        default:
            sfx_freq = 0;
            sfx_remaining_ms = 0;
            break;
    }
    sfx_phase_sample = 0;
}

// ---------------------------------------------------------------------------
// Життєвий цикл / вхід
// ---------------------------------------------------------------------------
bool hal_init(void)
{
    if (!audio_started) {
        audio_start_playing(DURAK_AUDIO_SAMPLES_PER_FRAME);
        audio_started = true;
    }
    return true;
}

void hal_shutdown(void)
{
    if (audio_started) {
        audio_stop_playing();
        audio_started = false;
    }
}

void hal_update(void) {
    btn_old_state = btn_state;

    odroid_gamepad_state_t joystick;
    odroid_input_read_gamepad(&joystick);

    btn_state = 0;
    if (joystick.values[ODROID_INPUT_UP])     btn_state |= BTN_BIT_UP;
    if (joystick.values[ODROID_INPUT_DOWN])   btn_state |= BTN_BIT_DOWN;
    if (joystick.values[ODROID_INPUT_LEFT])   btn_state |= BTN_BIT_LEFT;
    if (joystick.values[ODROID_INPUT_RIGHT])  btn_state |= BTN_BIT_RIGHT;
    if (joystick.values[ODROID_INPUT_A])      btn_state |= BTN_BIT_A;
    if (joystick.values[ODROID_INPUT_B])      btn_state |= BTN_BIT_B;
    if (joystick.values[ODROID_INPUT_START])  btn_state |= BTN_BIT_START;
    if (joystick.values[ODROID_INPUT_SELECT]) btn_state |= BTN_BIT_SELECT;
    // BTN_QUIT is mapped to the physical "power" button.
    if (joystick.values[ODROID_INPUT_POWER])  btn_state |= BTN_BIT_QUIT;

    // Retro-Go pause button: full pause menu + quick actions.
    bool pause_held = joystick.values[ODROID_INPUT_VOLUME];
    if (pause_held) {
        if (!pause_was_held) {
            pause_macro_activated = false;
            pause_last_key = -1;
        }

        if (pause_last_key < 0) {
            if (joystick.values[ODROID_INPUT_START]) {
                hal_take_screenshot();
                pause_last_key = ODROID_INPUT_START;
                pause_macro_activated = true;
            } else if (joystick.values[ODROID_INPUT_LEFT]) {
                int8_t level = odroid_audio_volume_get();
                if (level > ODROID_AUDIO_VOLUME_MIN) odroid_audio_volume_set(--level);
                pause_last_key = ODROID_INPUT_LEFT;
                pause_macro_activated = true;
            } else if (joystick.values[ODROID_INPUT_RIGHT]) {
                int8_t level = odroid_audio_volume_get();
                if (level < ODROID_AUDIO_VOLUME_MAX) odroid_audio_volume_set(++level);
                pause_last_key = ODROID_INPUT_RIGHT;
                pause_macro_activated = true;
            } else if (joystick.values[ODROID_INPUT_UP]) {
                int8_t level = odroid_display_get_backlight();
                if (level < ODROID_BACKLIGHT_LEVEL_COUNT - 1) odroid_display_set_backlight(++level);
                pause_last_key = ODROID_INPUT_UP;
                pause_macro_activated = true;
            } else if (joystick.values[ODROID_INPUT_DOWN]) {
                int8_t level = odroid_display_get_backlight();
                if (level > 0) odroid_display_set_backlight(--level);
                pause_last_key = ODROID_INPUT_DOWN;
                pause_macro_activated = true;
            }
        }

        if (pause_last_key >= 0 && !joystick.values[pause_last_key]) {
            pause_last_key = -1;
        }

        // Consume inputs while pause is held so gameplay doesn't react.
        btn_state = 0;
    } else if (pause_was_held && !pause_macro_activated) {
        // Pause released with no macro: open full Retro-Go in-game menu.
        odroid_overlay_game_menu(NULL, NULL, 0);
    }

    pause_was_held = pause_held;

    wdog_refresh();
}

bool hal_is_button_pressed(Button btn) {
    uint32_t bit = map_button_bit(btn);
    return (btn_state & bit) && !(btn_old_state & bit);
}

bool hal_is_button_held(Button btn) {
    uint32_t bit = map_button_bit(btn);
    return (btn_state & bit) != 0;
}

// ---------------------------------------------------------------------------
// Відео
// ---------------------------------------------------------------------------
void hal_clear_screen(unsigned int hex_color) {
    uint8_t r = (hex_color >> 16) & 0xFF;
    uint8_t g = (hex_color >> 8) & 0xFF;
    uint8_t b = hex_color & 0xFF;
    uint16_t color565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

    uint16_t* fb = (uint16_t *)lcd_get_active_buffer();
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) fb[i] = color565;
}

void hal_present(void) {
    lcd_swap();
    lcd_wait_for_vblank();
}
void hal_delay(unsigned int ms)
{
    // Cadence is driven by the DMA audio ring buffer (same pattern as other cores).
    durak_audio_submit_frame(ms ? ms : 16u);
}

HalTexture* hal_load_texture(const char* filename) {
    for (size_t i = 0; i < ASSET_TABLE_COUNT; i++) {
        if (strcmp(asset_table[i].name, filename) == 0) {
            HalTexture* tex = malloc(sizeof(HalTexture));
            if (!tex) return NULL;
            tex->indices = asset_table[i].idx;
            tex->palette = asset_table[i].pal;
            tex->stride_w = asset_table[i].w;
            tex->stride_h = asset_table[i].h;
            tex->src_off_x = asset_table[i].sx;
            tex->src_off_y = asset_table[i].sy;
            tex->width = asset_table[i].cw;
            tex->height = asset_table[i].ch;
            return tex;
        }
    }
    return NULL; // ассет не знайдено в таблиці - перевірте назву файлу
}

void hal_destroy_texture(HalTexture* texture) {
    // Самі дані - const-масиви у флеші, звільняти їх не треба,
    // видаляємо лише обгортку.
    free(texture);
}

void hal_draw_texture(HalTexture* texture, int x, int y) {
    if (!texture) return;
    hal_draw_sprite(texture, 0, 0, texture->width, texture->height, x, y);
}

// Блітер із палітрою: індекс -> колір, 0x0000 у палітрі = прозорий (color-key)
void hal_draw_sprite(HalTexture* texture, int sx, int sy, int sw, int sh, int dx, int dy) {
    if (!texture || !texture->indices) return;
    if (sw <= 0 || sh <= 0) return;
    if (sx < 0 || sy < 0) return;
    if (sx + sw > texture->width)  sw = texture->width - sx;
    if (sy + sh > texture->height) sh = texture->height - sy;
    if (sw <= 0 || sh <= 0) return;

    uint16_t* fb = (uint16_t *)lcd_get_active_buffer();
    const uint8_t* idx = texture->indices;
    const uint16_t* pal = texture->palette;

    for (int y = 0; y < sh; y++) {
        int fy = dy + y;
        if (fy < 0 || fy >= SCREEN_HEIGHT) continue;
        int src_row = (texture->src_off_y + sy + y) * texture->stride_w;
        int dst_row = fy * SCREEN_WIDTH;

        for (int x = 0; x < sw; x++) {
            int fx = dx + x;
            if (fx < 0 || fx >= SCREEN_WIDTH) continue;

            uint16_t color = pal[idx[src_row + texture->src_off_x + sx + x]];
            if (color == 0x0000) continue; // прозорий піксель - пропускаємо

            fb[dst_row + fx] = color;
        }
    }
}

// Обертання методом найближчого сусіда навколо центру спрайту.
void hal_draw_sprite_rotated(HalTexture* texture, int sx, int sy, int sw, int sh,
                              int dx, int dy, double angle) {
    if (!texture || !texture->indices) return;
    if (sw <= 0 || sh <= 0) return;
    if (sx < 0 || sy < 0) return;
    if (sx + sw > texture->width || sy + sh > texture->height) return;

    uint16_t* fb = (uint16_t *)lcd_get_active_buffer();
    const uint8_t* idx = texture->indices;
    const uint16_t* pal = texture->palette;

    double rad = -angle * (M_PI / 180.0);
    double cs = cos(rad), sn = sin(rad);
    int cx = sw / 2, cy = sh / 2;

    int half = (int)(sqrt((double)(sw * sw + sh * sh)) / 2) + 1;

    for (int y = -half; y <= half; y++) {
        int fy = dy + cy + y;
        if (fy < 0 || fy >= SCREEN_HEIGHT) continue;

        for (int x = -half; x <= half; x++) {
            int fx = dx + cx + x;
            if (fx < 0 || fx >= SCREEN_WIDTH) continue;

            int srcx = (int)round(x * cs - y * sn) + cx;
            int srcy = (int)round(x * sn + y * cs) + cy;
            if (srcx < 0 || srcx >= sw || srcy < 0 || srcy >= sh) continue;

            uint16_t color = pal[idx[(texture->src_off_y + sy + srcy) * texture->stride_w +
                                     (texture->src_off_x + sx + srcx)]];
            if (color == 0x0000) continue;

            fb[fy * SCREEN_WIDTH + fx] = color;
        }
    }
}
