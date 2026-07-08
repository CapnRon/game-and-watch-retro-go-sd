#include "hal.h"
#include "game.h"
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <stdio.h>
#include <string.h> 

// PLATFORM_RETRO_GO має визначатись у Makefile/збірці для консолі
// (наприклад -DPLATFORM_RETRO_GO). На retro-go немає SDL, тому весь
// SDL-специфічний код фонової музики нижче для цієї платформи вимкнено.
#ifndef PLATFORM_RETRO_GO
#ifdef _WIN32
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif
#define HAS_SDL_BG_MUSIC 1
#endif

#undef main 

#define CARD_WIDTH  40
#define CARD_HEIGHT 60
#define ANIM_FRAMES 20 
#define MAX_ANIMS   32

typedef struct {
    int freq;      
    int duration;  
} ToneNote;

// =========================================================
// МЕЛОДІЯ ГОЛОВНОГО МЕНЮ: Your Love Is My Drug 8-Bit [Slowed]
// =========================================================
static const ToneNote menu_music[] = {
    {392, 350}, {440, 350}, {523, 700}, {494, 350}, {440, 350},
    {392, 350}, {349, 350}, {392, 700}, {0, 150},
    {392, 350}, {440, 350}, {523, 700}, {587, 350}, {523, 350},
    {494, 700}, {440, 700}, {0, 150},
    {523, 400}, {523, 200}, {523, 400}, {494, 400}, {440, 400},
    {392, 400}, {392, 200}, {392, 400}, {440, 400}, {494, 400},
    {523, 400}, {523, 200}, {523, 400}, {587, 400}, {523, 400},
    {494, 600}, {440, 600}, {392, 800}, {0, 300},
    {440, 350}, {440, 350}, {440, 350}, {392, 350}, {440, 700},
    {494, 350}, {494, 350}, {494, 350}, {440, 350}, {494, 700},
    {523, 350}, {523, 350}, {587, 350}, {523, 350}, {494, 700},
    {440, 1000}, {0, 500} 
};
#define MENU_NOTE_COUNT (sizeof(menu_music) / sizeof(ToneNote))

#ifdef HAS_SDL_BG_MUSIC
static int audio_sample_index = 0;
static int current_freq = 0;

void audio_callback(void* userdata, Uint8* stream, int len) {
    Sint16* buffer = (Sint16*)stream;
    int samples_count = len / 2;
    for (int i = 0; i < samples_count; i++) {
        if (current_freq > 0) {
            int t = 44100 / current_freq;
            if (t > 0) buffer[i] = ((audio_sample_index / (t / 2)) % 2) ? 250 : -250;
            else buffer[i] = 0;
            audio_sample_index++;
        } else {
            buffer[i] = 0;
            audio_sample_index = 0;
        }
    }
}

void init_bg_music(void) {
    SDL_AudioSpec wanted;
    wanted.freq = 44100;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 1;
    wanted.samples = 1024;
    wanted.callback = audio_callback;
    wanted.userdata = NULL;
    if (SDL_OpenAudio(&wanted, NULL) >= 0) SDL_PauseAudio(0);
}
#else
// На retro-go фоновий трек поки не програється через відсутність
// callback-аудіо в цьому HAL - лише SFX через hal_play_sound().
// Якщо захочете фонову музику на консолі, її треба буде програвати
// нота-за-нотою з ігрового циклу (за лічильником кадрів), а не callback'ом.
void init_bg_music(void) {}
#endif

typedef struct {
    bool active;
    Card card;
    float cx, cy;       
    float tx, ty;       
    float dx, dy;       
    int frames_left;
    bool hide_card; 
} VisualAnim;

void draw_card(HalTexture* cards_tex, Card card, int dx, int dy) {
    int card_index = (card.rank - 6) * 4 + card.suit;
    int sx = card_index * CARD_WIDTH;
    int sy = 0;
    hal_draw_sprite(cards_tex, sx, sy, CARD_WIDTH, CARD_HEIGHT, dx, dy);
}

void draw_card_rotated(HalTexture* cards_tex, Card card, int dx, int dy, double angle) {
    int card_index = (card.rank - 6) * 4 + card.suit;
    int sx = card_index * CARD_WIDTH;
    int sy = 0;
    hal_draw_sprite_rotated(cards_tex, sx, sy, CARD_WIDTH, CARD_HEIGHT, dx, dy, angle);
}

void draw_text(HalTexture* font_tex, const char* text, int x, int y) {
    for (int i = 0; text[i] != '\0'; i++) {
        int ascii = (unsigned char)text[i];
        int sx = (ascii % 16) * 8;
        int sy = (ascii / 16) * 8;
        hal_draw_sprite(font_tex, sx, sy, 8, 8, x + (i * 8), y);
    }
}

void draw_face(HalTexture* faces_tex, int face_idx, int x, int y) {
    if (!faces_tex) return;
    int sx = face_idx * 32;
    int sy = 0;
    hal_draw_sprite(faces_tex, sx, sy, 32, 32, x, y);
}

int durak_run(void) {
    srand((unsigned int)time(NULL));
    if (!hal_init()) return 1;

    init_bg_music();

    HalTexture* card_back = hal_load_texture("title.png");
    HalTexture* cards_tex = hal_load_texture("cards.png");
    HalTexture* font_tex = hal_load_texture("font.png");             
    HalTexture* faces_tex = hal_load_texture("faces.png"); 
    // Завантаження mainscreen.png повністю видалено для економії пам'яті!

    GameState game;
    game_init_global(&game); 

    int selected_card_idx = 0;
    unsigned int bg_color = 0x1B4D3E; 
    
    VisualAnim anims[MAX_ANIMS] = {0};
    int anim_phase = 0;           
    bool phase_took = false;      
    bool hide_table = false;      
    int single_target_pair_idx = -1;

    int ai_delay = 0; 
    int current_note_idx = 0;
    int music_timer_ms = 0;
    bool in_menu = true; 
    int menu_frame_counter = 0; // Лічильник кадрів для блимання тексту

    while (!hal_is_button_held(BTN_QUIT)) {
        hal_update();

        if (in_menu) {
            if (music_timer_ms <= 0) {
                ToneNote n = menu_music[current_note_idx];
#ifdef HAS_SDL_BG_MUSIC
                current_freq = n.freq; 
#endif
                music_timer_ms = n.duration;
                current_note_idx = (current_note_idx + 1) % MENU_NOTE_COUNT;
            } else {
                music_timer_ms -= 16; 
            }
            menu_frame_counter++; // Оновлюємо лічильник кадрів меню
        } else {
#ifdef HAS_SDL_BG_MUSIC
            current_freq = 0; 
#endif
        }

        if (in_menu) {
            if (hal_is_button_pressed(BTN_START) || hal_is_button_pressed(BTN_A)) {
                in_menu = false; 
                game_init(&game);
                game_shuffle_deck(&game);
                game_deal_cards(&game);
                
                hal_play_sound(SND_SHUFFLE);
                selected_card_idx = 0;
                ai_delay = 10;
                anim_phase = 0;
                hide_table = false;
                for (int i = 0; i < MAX_ANIMS; i++) anims[i].active = false;
            }

            hal_clear_screen(0x000000); 
            
            // Малюємо декоративні карти по центру для антуражу
            hal_draw_texture(card_back, 140, 30); 
            Card logo1 = {SUIT_HEARTS, RANK_A};
            Card logo2 = {SUIT_SPADES, RANK_K};
            draw_card(cards_tex, logo1, 115, 45);
            draw_card(cards_tex, logo2, 165, 45);

            // Виводимо текстове меню
            if (font_tex) {
                // Головний заголовок
                const char* title = "- D U R E N -";
                int title_w = strlen(title) * 8;
                draw_text(font_tex, title, 160 - (title_w / 2), 125);

                // Блимаючий надпис (кожні ~30 кадрів)
                if ((menu_frame_counter / 30) % 2 == 0) {
                    const char* prompt = "PRESS START";
                    int prompt_w = strlen(prompt) * 8;
                    draw_text(font_tex, prompt, 160 - (prompt_w / 2), 165);
                }

                // Копірайт внизу екрану
                const char* bottom_text = "2026 G&W EDITION";
                int bottom_w = strlen(bottom_text) * 8;
                draw_text(font_tex, bottom_text, 160 - (bottom_w / 2), 220);
            }

            hal_present();
            hal_delay(16);
            continue; 
        }

        bool is_animating = false;
        if (anim_phase > 0) {
            is_animating = true;
        } else {
            for (int i = 0; i < MAX_ANIMS; i++) {
                if (anims[i].active) { is_animating = true; break; }
            }
        }

        int p_count = game.players[0].card_count;
        bool need_defense = (game.table_pair_count > 0 && game.table_defense[game.table_pair_count - 1].rank < 6);

        bool any_anim_running = false;
        for (int i = 0; i < MAX_ANIMS; i++) {
            if (anims[i].active) {
                anims[i].cx += anims[i].dx;
                anims[i].cy += anims[i].dy;
                anims[i].frames_left--;
                if (anims[i].frames_left <= 0) {
                    anims[i].active = false;
                    if (anim_phase == 0 && i == 0) {
                        if (single_target_pair_idx == -1) {
                            game.table_attack[game.table_pair_count] = anims[0].card;
                            game.table_pair_count++;
                        } else {
                            game.table_defense[single_target_pair_idx] = anims[0].card;
                        }
                        game_check_status(&game);
                    }
                } else {
                    any_anim_running = true;
                }
            }
        }

        if (!any_anim_running && anim_phase > 0) {
            if (anim_phase == 1) {
                int temp_deck_top = game.deck.top_index;
                int temp_p0_count = game.players[0].card_count;
                int temp_p1_count = game.players[1].card_count;

                if (phase_took) {
                    int def_idx = game.is_player_turn ? 1 : 0;
                    for (int k = 0; k < game.table_pair_count; k++) {
                        if (game.table_attack[k].rank >= 6) { if(def_idx == 0) temp_p0_count++; else temp_p1_count++; }
                        if (game.table_defense[k].rank >= 6) { if(def_idx == 0) temp_p0_count++; else temp_p1_count++; }
                    }
                }

                for (int k = 0; k < MAX_ANIMS; k++) anims[k].active = false;

                int anim_idx = 0;
                while (temp_p0_count < 6 && temp_deck_top < DECK_SIZE) {
                    anims[anim_idx].active = true;
                    anims[anim_idx].cx = 250; anims[anim_idx].cy = 110; 
                    anims[anim_idx].tx = (float)(10 + (temp_p0_count * 25));     
                    anims[anim_idx].ty = 160;
                    anims[anim_idx].frames_left = ANIM_FRAMES;
                    anims[anim_idx].dx = (anims[anim_idx].tx - anims[anim_idx].cx) / ANIM_FRAMES;
                    anims[anim_idx].dy = (anims[anim_idx].ty - anims[anim_idx].cy) / ANIM_FRAMES;
                    anims[anim_idx].hide_card = true; 
                    anim_idx++; temp_p0_count++; temp_deck_top++;
                }

                while (temp_p1_count < 6 && temp_deck_top < DECK_SIZE) {
                    anims[anim_idx].active = true;
                    anims[anim_idx].cx = 250; anims[anim_idx].cy = 110;
                    anims[anim_idx].tx = (float)(10 + (temp_p1_count * 20));     
                    anims[anim_idx].ty = 10;
                    anims[anim_idx].frames_left = ANIM_FRAMES;
                    anims[anim_idx].dx = (anims[anim_idx].tx - anims[anim_idx].cx) / ANIM_FRAMES;
                    anims[anim_idx].dy = (anims[anim_idx].ty - anims[anim_idx].cy) / ANIM_FRAMES;
                    anims[anim_idx].hide_card = true;
                    anim_idx++; temp_p1_count++; temp_deck_top++;
                }

                if (anim_idx > 0) {
                    anim_phase = 2; 
                } else {
                    game_end_turn(&game, phase_took);
                    hide_table = false; anim_phase = 0; selected_card_idx = 0; ai_delay = 15;
                }
            } 
            else if (anim_phase == 2) {
                game_end_turn(&game, phase_took);
                hide_table = false;
                anim_phase = 0;
                selected_card_idx = 0;
                ai_delay = 15;
            }
        }

        if (!is_animating && game.result == RESULT_NONE) {
            if (ai_delay > 0) {
                ai_delay--;
            } else {
                if (!game.is_player_turn && !need_defense) {
                    int ai_idx = game_ai_choose_attack(&game);
                    if (ai_idx != -1) {
                        hal_play_sound(SND_CARD);
                        for(int k=0; k<MAX_ANIMS; k++) anims[k].active = false;
                        anims[0].active = true; anims[0].card = game.players[1].hand[ai_idx];
                        single_target_pair_idx = -1;
                        anims[0].cx = 140.0f; anims[0].cy = -20.0f;
                        anims[0].tx = (float)(10 + (game.table_pair_count * 35)); anims[0].ty = 80.0f;
                        anims[0].frames_left = ANIM_FRAMES;
                        anims[0].dx = (anims[0].tx - anims[0].cx) / ANIM_FRAMES; anims[0].dy = (anims[0].ty - anims[0].cy) / ANIM_FRAMES;
                        anims[0].hide_card = false;
                        
                        for (int i = ai_idx; i < game.players[1].card_count - 1; i++) game.players[1].hand[i] = game.players[1].hand[i + 1];
                        game.players[1].card_count--;
                        ai_delay = 25; 
                    } else if (game.table_pair_count > 0) {
                        anim_phase = 1; phase_took = false; hide_table = true;
                        int anim_idx = 0;
                        for (int k = 0; k < game.table_pair_count; k++) {
                            anims[anim_idx].active = true; anims[anim_idx].card = game.table_attack[k];
                            anims[anim_idx].cx = (float)(10 + (k * 35)); anims[anim_idx].cy = 80.0f;
                            anims[anim_idx].tx = -60.0f; anims[anim_idx].ty = 100.0f; 
                            anims[anim_idx].frames_left = ANIM_FRAMES;
                            anims[anim_idx].dx = (anims[anim_idx].tx - anims[anim_idx].cx) / ANIM_FRAMES; anims[anim_idx].dy = (anims[anim_idx].ty - anims[anim_idx].cy) / ANIM_FRAMES;
                            anims[anim_idx].hide_card = false; anim_idx++;

                            if (game.table_defense[k].rank >= 6) {
                                anims[anim_idx].active = true; anims[anim_idx].card = game.table_defense[k];
                                anims[anim_idx].cx = (float)(10 + (k * 35) + 10); anims[anim_idx].cy = 95.0f;
                                anims[anim_idx].tx = -60.0f; anims[anim_idx].ty = 100.0f;
                                anims[anim_idx].frames_left = ANIM_FRAMES;
                                anims[anim_idx].dx = (anims[anim_idx].tx - anims[anim_idx].cx) / ANIM_FRAMES; anims[anim_idx].dy = (anims[anim_idx].ty - anims[anim_idx].cy) / ANIM_FRAMES;
                                anims[anim_idx].hide_card = false; anim_idx++;
                            }
                        }
                        hal_play_sound(SND_SHUFFLE);
                    }
                }
                else if (game.is_player_turn && need_defense) {
                    int ai_idx = game_ai_choose_defend(&game, game.table_attack[game.table_pair_count - 1]);
                    if (ai_idx != -1) {
                        hal_play_sound(SND_CARD);
                        for(int k=0; k<MAX_ANIMS; k++) anims[k].active = false;
                        anims[0].active = true; anims[0].card = game.players[1].hand[ai_idx];
                        single_target_pair_idx = game.table_pair_count - 1;
                        anims[0].cx = 140.0f; anims[0].cy = -20.0f;
                        anims[0].tx = (float)(10 + ((game.table_pair_count - 1) * 35) + 10); anims[0].ty = 95.0f;
                        anims[0].frames_left = ANIM_FRAMES;
                        anims[0].dx = (anims[0].tx - anims[0].cx) / ANIM_FRAMES; anims[0].dy = (anims[0].ty - anims[0].cy) / ANIM_FRAMES;
                        anims[0].hide_card = false;
                        
                        for (int i = ai_idx; i < game.players[1].card_count - 1; i++) game.players[1].hand[i] = game.players[1].hand[i + 1];
                        game.players[1].card_count--;
                        ai_delay = 25;
                    } else {
                        anim_phase = 1; phase_took = true; hide_table = true;
                        int anim_idx = 0;
                        for (int k = 0; k < game.table_pair_count; k++) {
                            anims[anim_idx].active = true; anims[anim_idx].card = game.table_attack[k];
                            anims[anim_idx].cx = (float)(10 + (k * 35)); anims[anim_idx].cy = 80.0f;
                            anims[anim_idx].tx = 100.0f; anims[anim_idx].ty = -50.0f; 
                            anims[anim_idx].frames_left = ANIM_FRAMES;
                            anims[anim_idx].dx = (anims[anim_idx].tx - anims[anim_idx].cx) / ANIM_FRAMES; anims[anim_idx].dy = (anims[anim_idx].ty - anims[anim_idx].cy) / ANIM_FRAMES;
                            anims[anim_idx].hide_card = false; anim_idx++;

                            if (game.table_defense[k].rank >= 6) {
                                anims[anim_idx].active = true; anims[anim_idx].card = game.table_defense[k];
                                anims[anim_idx].cx = (float)(10 + (k * 35) + 10); anims[anim_idx].cy = 95.0f;
                                anims[anim_idx].tx = 100.0f; anims[anim_idx].ty = -50.0f;
                                anims[anim_idx].frames_left = ANIM_FRAMES;
                                anims[anim_idx].dx = (anims[anim_idx].tx - anims[anim_idx].cx) / ANIM_FRAMES; anims[anim_idx].dy = (anims[anim_idx].ty - anims[anim_idx].cy) / ANIM_FRAMES;
                                anims[anim_idx].hide_card = false; anim_idx++;
                            }
                        }
                        hal_play_sound(SND_TAKE);
                    }
                }
            }
        }

        if (!is_animating) {
            if (game.player_money <= 0) {
                if (hal_is_button_pressed(BTN_START) || hal_is_button_pressed(BTN_A)) {
                    game_init_global(&game);
                    in_menu = true;
                    current_note_idx = 0; music_timer_ms = 0;
                }
            }
            else if (game.result != RESULT_NONE) {
                if (hal_is_button_pressed(BTN_START) || hal_is_button_pressed(BTN_A)) {
                    if (game.player_money < 50) {
                        game_init_global(&game); 
                        in_menu = true; 
                        current_note_idx = 0; music_timer_ms = 0;
                    } else {
                        game_init(&game);
                        game_shuffle_deck(&game);
                        game_deal_cards(&game);
                        hal_play_sound(SND_SHUFFLE);
                        selected_card_idx = 0;
                        ai_delay = 10;
                        anim_phase = 0;
                        hide_table = false;
                    }
                }
            } 
            else {
                if (p_count > 0) {
                    if (hal_is_button_pressed(BTN_LEFT))  selected_card_idx = (selected_card_idx - 1 + p_count) % p_count;
                    if (hal_is_button_pressed(BTN_RIGHT)) selected_card_idx = (selected_card_idx + 1) % p_count;
                    
                    if (hal_is_button_pressed(BTN_A)) {
                        Card selected_card = game.players[0].hand[selected_card_idx];
                        if (game.is_player_turn && !need_defense) {
                            if (game_can_attack_with(&game, selected_card)) {
                                hal_play_sound(SND_CARD); 
                                for(int k=0; k<MAX_ANIMS; k++) anims[k].active = false;
                                anims[0].active = true; anims[0].card = selected_card;
                                single_target_pair_idx = -1;
                                int p_offset_x = (p_count > 1) ? (220 / (p_count - 1)) : CARD_WIDTH;
                                if (p_offset_x > CARD_WIDTH) p_offset_x = CARD_WIDTH;
                                anims[0].cx = (float)(10 + (selected_card_idx * p_offset_x)); anims[0].cy = 145.0f;
                                anims[0].tx = (float)(10 + (game.table_pair_count * 35)); anims[0].ty = 80.0f;
                                anims[0].frames_left = ANIM_FRAMES;
                                anims[0].dx = (anims[0].tx - anims[0].cx) / ANIM_FRAMES; anims[0].dy = (anims[0].ty - anims[0].cy) / ANIM_FRAMES;
                                anims[0].hide_card = false;

                                Player* p = &game.players[0];
                                for (int i = selected_card_idx; i < p->card_count - 1; i++) p->hand[i] = p->hand[i + 1];
                                p->card_count--;
                                if (selected_card_idx >= p->card_count && p->card_count > 0) selected_card_idx = p->card_count - 1;
                            } else hal_play_sound(SND_ERROR);
                        } else if (!game.is_player_turn && need_defense) {
                            if (game_can_defend_with(&game, game.table_attack[game.table_pair_count - 1], selected_card)) {
                                hal_play_sound(SND_CARD); 
                                for(int k=0; k<MAX_ANIMS; k++) anims[k].active = false;
                                anims[0].active = true; anims[0].card = selected_card;
                                single_target_pair_idx = game.table_pair_count - 1;
                                int p_offset_x = (p_count > 1) ? (220 / (p_count - 1)) : CARD_WIDTH;
                                if (p_offset_x > CARD_WIDTH) p_offset_x = CARD_WIDTH;
                                anims[0].cx = (float)(10 + (selected_card_idx * p_offset_x)); anims[0].cy = 145.0f;
                                anims[0].tx = (float)(10 + ((game.table_pair_count - 1) * 35) + 10); anims[0].ty = 95.0f;
                                anims[0].frames_left = ANIM_FRAMES;
                                anims[0].dx = (anims[0].tx - anims[0].cx) / ANIM_FRAMES; anims[0].dy = (anims[0].ty - anims[0].cy) / ANIM_FRAMES;
                                anims[0].hide_card = false;

                                Player* p = &game.players[0];
                                for (int i = selected_card_idx; i < p->card_count - 1; i++) p->hand[i] = p->hand[i + 1];
                                p->card_count--;
                                if (selected_card_idx >= p->card_count && p->card_count > 0) selected_card_idx = p->card_count - 1;
                            } else hal_play_sound(SND_ERROR);
                        } else hal_play_sound(SND_ERROR);
                    }
                }
                if (hal_is_button_pressed(BTN_SELECT)) {
                    if (game.table_pair_count > 0) {
                        if (game.is_player_turn && !need_defense) {
                            anim_phase = 1; phase_took = false; hide_table = true;
                            int anim_idx = 0;
                            for (int k = 0; k < game.table_pair_count; k++) {
                                anims[anim_idx].active = true; anims[anim_idx].card = game.table_attack[k];
                                anims[anim_idx].cx = (float)(10 + (k * 35)); anims[anim_idx].cy = 80.0f;
                                anims[anim_idx].tx = -60.0f; anims[anim_idx].ty = 100.0f;
                                anims[anim_idx].frames_left = ANIM_FRAMES;
                                anims[anim_idx].dx = (anims[anim_idx].tx - anims[anim_idx].cx) / ANIM_FRAMES; anims[anim_idx].dy = (anims[anim_idx].ty - anims[anim_idx].cy) / ANIM_FRAMES;
                                anims[anim_idx].hide_card = false; anim_idx++;

                                if (game.table_defense[k].rank >= 6) {
                                    anims[anim_idx].active = true; anims[anim_idx].card = game.table_defense[k];
                                    anims[anim_idx].cx = (float)(10 + (k * 35) + 10); anims[anim_idx].cy = 95.0f;
                                    anims[anim_idx].tx = -60.0f; anims[anim_idx].ty = 100.0f;
                                    anims[anim_idx].frames_left = ANIM_FRAMES;
                                    anims[anim_idx].dx = (anims[anim_idx].tx - anims[anim_idx].cx) / ANIM_FRAMES; anims[anim_idx].dy = (anims[anim_idx].ty - anims[anim_idx].cy) / ANIM_FRAMES;
                                    anims[anim_idx].hide_card = false; anim_idx++;
                                }
                            }
                            hal_play_sound(SND_SHUFFLE);
                            selected_card_idx = 0;
                        } else if (!game.is_player_turn && need_defense) {
                            anim_phase = 1; phase_took = true; hide_table = true;
                            int anim_idx = 0;
                            for (int k = 0; k < game.table_pair_count; k++) {
                                anims[anim_idx].active = true; anims[anim_idx].card = game.table_attack[k];
                                anims[anim_idx].cx = (float)(10 + (k * 35)); anims[anim_idx].cy = 80.0f;
                                anims[anim_idx].tx = 140.0f; anims[anim_idx].ty = 250.0f; 
                                anims[anim_idx].frames_left = ANIM_FRAMES;
                                anims[anim_idx].dx = (anims[anim_idx].tx - anims[anim_idx].cx) / ANIM_FRAMES; anims[anim_idx].dy = (anims[anim_idx].ty - anims[anim_idx].cy) / ANIM_FRAMES;
                                anims[anim_idx].hide_card = false; anim_idx++;

                                if (game.table_defense[k].rank >= 6) {
                                    anims[anim_idx].active = true; anims[anim_idx].card = game.table_defense[k];
                                    anims[anim_idx].cx = (float)(10 + (k * 35) + 10); anims[anim_idx].cy = 95.0f;
                                    anims[anim_idx].tx = 140.0f; anims[anim_idx].ty = 250.0f;
                                    anims[anim_idx].frames_left = ANIM_FRAMES;
                                    anims[anim_idx].dx = (anims[anim_idx].tx - anims[anim_idx].cx) / ANIM_FRAMES; anims[anim_idx].dy = (anims[anim_idx].ty - anims[anim_idx].cy) / ANIM_FRAMES;
                                    anims[anim_idx].hide_card = false; anim_idx++;
                                }
                            }
                            hal_play_sound(SND_TAKE);
                            selected_card_idx = 0;
                        } else hal_play_sound(SND_ERROR);
                    }
                }
            }
        }

        unsigned int current_bg = bg_color;
        if (game.result != RESULT_NONE) {
            if (game.player_money < 50) current_bg = 0x550000; 
            else if (game.result == RESULT_WIN) current_bg = 0x1D551D; 
            else if (game.result == RESULT_LOSE) current_bg = 0x553311; 
            else current_bg = 0x224455; 
        }
        hal_clear_screen(current_bg);

        if (game.player_money >= 500) hal_draw_texture(card_back, 270, 40);

        int ai_count = game.players[1].card_count;
        int ai_start_x = 10;
        int ai_offset_x = (ai_count > 1) ? (180 / (ai_count - 1)) : CARD_WIDTH;
        if (ai_offset_x > CARD_WIDTH) ai_offset_x = CARD_WIDTH;
        for (int i = 0; i < ai_count; i++) hal_draw_texture(card_back, ai_start_x + (i * ai_offset_x), 10);

        int cards_left = DECK_SIZE - game.deck.top_index;
        if (cards_left > 0) {
            draw_card_rotated(cards_tex, game.trump_card, 250, 110, 90.0);
            if (cards_left > 1) hal_draw_texture(card_back, 260, 100);
        }

        if (!hide_table) {
            for (int i = 0; i < game.table_pair_count; i++) {
                int table_x = 10 + (i * 35);
                draw_card(cards_tex, game.table_attack[i], table_x, 80);
                if (game.table_defense[i].rank >= 6) draw_card(cards_tex, game.table_defense[i], table_x + 10, 95);
            }
        }

        for (int i = 0; i < MAX_ANIMS; i++) {
            if (anims[i].active) {
                if (anims[i].hide_card) {
                    hal_draw_texture(card_back, (int)anims[i].cx, (int)anims[i].cy);
                } else {
                    draw_card(cards_tex, anims[i].card, (int)anims[i].cx, (int)anims[i].cy);
                }
            }
        }

        p_count = game.players[0].card_count; 
        int p_start_x = 10;
        int p_offset_x = (p_count > 1) ? (220 / (p_count - 1)) : CARD_WIDTH;
        if (p_offset_x > CARD_WIDTH) p_offset_x = CARD_WIDTH;
        for (int i = 0; i < p_count; i++) {
            int card_x = p_start_x + (i * p_offset_x);
            int card_y = (i == selected_card_idx && p_count > 0 && game.result == RESULT_NONE && !is_animating) ? 145 : 160;
            draw_card(cards_tex, game.players[0].hand[i], card_x, card_y);
        }

        if (game.result != RESULT_NONE) {
            if (game.player_money < 50) {
                hal_draw_texture(card_back, 110, 90); hal_draw_texture(card_back, 140, 90);
            } else if (game.result == RESULT_WIN) {
                Card vic_card = {SUIT_HEARTS, RANK_A}; draw_card(cards_tex, vic_card, 140, 90);
            } else if (game.result == RESULT_LOSE) {
                Card lose_card = {SUIT_SPADES, RANK_6}; draw_card(cards_tex, lose_card, 140, 90);
            }
        }

        if (font_tex) {
            if (game.current_opponent.name[0] != '\0') {
                char opp_str[32];
                sprintf(opp_str, "%s LVL%d", game.current_opponent.name, game.current_opponent.level);
                int text_width = strlen(opp_str) * 8;
                int opp_x = 320 - text_width - 10; 
                draw_text(font_tex, opp_str, opp_x, 10); 
                
                if (faces_tex) {
                    int face_idx = (game.current_opponent.level - 1) % 20; 
                    int face_x = opp_x + (text_width / 2) - 16; 
                    int face_y = 24; 
                    draw_face(faces_tex, face_idx, face_x, face_y);
                }
            }

            char money_str[32];
            sprintf(money_str, "CASH: $%d", game.player_money);
            int money_x = 320 - (strlen(money_str) * 8) - 10;
            draw_text(font_tex, money_str, money_x, 222);

            char deck_str[16];
            sprintf(deck_str, "DECK:%d", cards_left);
            draw_text(font_tex, deck_str, 250, 85);

            if (game.player_money <= 0) draw_text(font_tex, "GAME OVER", 120, 120);
        }

        hal_present();
        hal_delay(16);
    }

#ifdef HAS_SDL_BG_MUSIC
    SDL_CloseAudio(); 
#endif
    hal_destroy_texture(card_back); 
    hal_destroy_texture(cards_tex);
    if (font_tex) hal_destroy_texture(font_tex);                           
    if (faces_tex) hal_destroy_texture(faces_tex); 

    hal_shutdown();
    return 0;
}