#ifndef HAL_H
#define HAL_H

#include <stdbool.h>
#include <stdint.h>

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

typedef enum {
    BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
    BTN_A, BTN_B, BTN_START, BTN_SELECT, BTN_QUIT,
    BTN_COUNT
} Button;

typedef enum {
    SND_CARD,
    SND_DEAL,
    SND_ERROR,
    SND_FLIP,
    SND_SELECT,
    SND_SHUFFLE,
    SND_TAKE,
    SND_COUNT
} SoundFx;

typedef struct HalTexture HalTexture;

bool hal_init(void);
void hal_shutdown(void);
void hal_update(void);

bool hal_is_button_pressed(Button btn);
bool hal_is_button_held(Button btn);

void hal_clear_screen(unsigned int hex_color);
void hal_present(void);
void hal_delay(unsigned int ms);

HalTexture* hal_load_texture(const char* filename);
void hal_destroy_texture(HalTexture* texture);
void hal_draw_texture(HalTexture* texture, int x, int y);
void hal_draw_sprite(HalTexture* texture, int sx, int sy, int sw, int sh, int dx, int dy);
void hal_draw_sprite_rotated(HalTexture* texture, int sx, int sy, int sw, int sh, int dx, int dy, double angle);

void hal_play_sound(SoundFx snd);

#endif // HAL_H