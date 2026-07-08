#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "hal.h"

struct HalTexture {
    SDL_Texture* sdl_tex;
    int w, h;
};

static SDL_Window* window = NULL;
static SDL_Renderer* renderer = NULL;
static bool btn_state[BTN_COUNT] = {false};
static bool btn_old_state[BTN_COUNT] = {false};

// =========================================================
// ГЕНЕРАТОР ЗВУКУ (8-бітні "біпалки" без WAV файлів)
// =========================================================
static SDL_AudioDeviceID sfx_device = 0;
static int sfx_freq = 0;
static int sfx_samples_left = 0;
static int sfx_sample_index = 0;

static void sfx_audio_callback(void* userdata, Uint8* stream, int len) {
    Sint16* buffer = (Sint16*)stream;
    int samples_count = len / 2; 

    for (int i = 0; i < samples_count; i++) {
        if (sfx_samples_left > 0 && sfx_freq > 0) {
            int period = 44100 / sfx_freq;
            if (period > 0) {
                // Генерація прямокутної хвилі (Square Wave)
                buffer[i] = ((sfx_sample_index / (period / 2)) % 2) ? 3000 : -3000;
            } else {
                buffer[i] = 0;
            }
            sfx_sample_index++;
            sfx_samples_left--;
        } else {
            buffer[i] = 0; // Тиша
        }
    }
}

void hal_play_sound(SoundFx snd) {
    if (sfx_device == 0) return;

    int freq = 0;
    int duration_ms = 0;

    // Частоти підібрані один-в-один під залізо Game & Watch
    switch (snd) {
        case SND_CARD:    freq = 880; duration_ms = 50;  break;
        case SND_DEAL:    freq = 440; duration_ms = 100; break;
        case SND_ERROR:   freq = 200; duration_ms = 200; break;
        case SND_FLIP:    freq = 600; duration_ms = 100; break;
        case SND_SELECT:  freq = 700; duration_ms = 50;  break;
        case SND_SHUFFLE: freq = 300; duration_ms = 150; break;
        case SND_TAKE:    freq = 500; duration_ms = 150; break;
        default: return;
    }

    SDL_LockAudioDevice(sfx_device);
    sfx_freq = freq;
    sfx_samples_left = (44100 * duration_ms) / 1000; 
    sfx_sample_index = 0;
    SDL_UnlockAudioDevice(sfx_device);
}
// =========================================================

static int map_key(SDL_Keycode key) {
    switch (key) {
        case SDLK_UP:     return BTN_UP;    case SDLK_DOWN:   return BTN_DOWN;
        case SDLK_LEFT:   return BTN_LEFT;  case SDLK_RIGHT:  return BTN_RIGHT;
        case SDLK_z:      return BTN_A;     case SDLK_x:      return BTN_B;      
        case SDLK_RETURN: return BTN_START; case SDLK_LSHIFT: return BTN_SELECT; 
        case SDLK_ESCAPE: return BTN_QUIT;  default:          return -1;
    }
}

bool hal_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) return false;
    if (!(IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG)) return false;

    window = SDL_CreateWindow("Durak G&W (8-bit Audio)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 
                              SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_SHOWN);
    if (!window) return false;

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) return false;

    SDL_RenderSetLogicalSize(renderer, SCREEN_WIDTH, SCREEN_HEIGHT);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    // Ініціалізація аудіо-генератора
    SDL_AudioSpec wanted_spec;
    SDL_zero(wanted_spec);
    wanted_spec.freq = 44100;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = 1; 
    wanted_spec.samples = 512; 
    wanted_spec.callback = sfx_audio_callback;

    sfx_device = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, NULL, 0);
    if (sfx_device > 0) {
        SDL_PauseAudioDevice(sfx_device, 0); 
    }

    return true;
}

void hal_shutdown(void) {
    if (sfx_device > 0) SDL_CloseAudioDevice(sfx_device);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    IMG_Quit(); 
    SDL_Quit();
}

void hal_update(void) {
    SDL_Event event;
    memcpy(btn_old_state, btn_state, sizeof(btn_state));
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) btn_state[BTN_QUIT] = true;
        else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
            int btn = map_key(event.key.keysym.sym);
            if (btn >= 0) btn_state[btn] = (event.type == SDL_KEYDOWN);
        }
    }
}

bool hal_is_button_pressed(Button btn) { return btn_state[btn] && !btn_old_state[btn]; }
bool hal_is_button_held(Button btn) { return btn_state[btn]; }

void hal_clear_screen(unsigned int hex_color) {
    Uint8 r = (hex_color >> 16) & 0xFF; 
    Uint8 g = (hex_color >> 8) & 0xFF; 
    Uint8 b = hex_color & 0xFF;
    SDL_SetRenderDrawColor(renderer, r, g, b, 255); 
    SDL_RenderClear(renderer);
}

void hal_present(void) { SDL_RenderPresent(renderer); }
void hal_delay(unsigned int ms) { SDL_Delay(ms); }

HalTexture* hal_load_texture(const char* filename) {
    SDL_Texture* sdl_tex = IMG_LoadTexture(renderer, filename);
    if (!sdl_tex) return NULL;
    HalTexture* texture = malloc(sizeof(HalTexture));
    if (!texture) return NULL;
    texture->sdl_tex = sdl_tex;
    SDL_QueryTexture(sdl_tex, NULL, NULL, &texture->w, &texture->h);
    return texture;
}

void hal_destroy_texture(HalTexture* texture) { 
    if (texture) { 
        if (texture->sdl_tex) SDL_DestroyTexture(texture->sdl_tex); 
        free(texture); 
    } 
}

void hal_draw_texture(HalTexture* texture, int x, int y) {
    if (!texture || !texture->sdl_tex) return;
    SDL_Rect dst = { x, y, texture->w, texture->h };
    SDL_RenderCopy(renderer, texture->sdl_tex, NULL, &dst);
}

void hal_draw_sprite(HalTexture* texture, int sx, int sy, int sw, int sh, int dx, int dy) {
    if (!texture || !texture->sdl_tex) return;
    SDL_Rect src = { sx, sy, sw, sh }; 
    SDL_Rect dst = { dx, dy, sw, sh };
    SDL_RenderCopy(renderer, texture->sdl_tex, &src, &dst);
}

void hal_draw_sprite_rotated(HalTexture* texture, int sx, int sy, int sw, int sh, int dx, int dy, double angle) {
    if (!texture || !texture->sdl_tex) return;
    SDL_Rect src = { sx, sy, sw, sh }; 
    SDL_Rect dst = { dx, dy, sw, sh };
    SDL_RenderCopyEx(renderer, texture->sdl_tex, &src, &dst, angle, NULL, SDL_FLIP_NONE);
}