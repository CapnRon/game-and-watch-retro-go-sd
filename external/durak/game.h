#ifndef GAME_H
#define GAME_H

#include <stdint.h>
#include <stdbool.h>

#define DECK_SIZE   36
#define MAX_HAND    36 
#define MAX_TABLE   6  
#define MAX_AI_NAME 16

typedef enum { SUIT_SPADES, SUIT_CLUBS, SUIT_DIAMONDS, SUIT_HEARTS, SUIT_COUNT } Suit;
typedef enum { RANK_6 = 6, RANK_7, RANK_8, RANK_9, RANK_10, RANK_J, RANK_Q, RANK_K, RANK_A } Rank;
typedef enum { RESULT_NONE, RESULT_WIN, RESULT_LOSE, RESULT_DRAW } GameResult;

typedef struct { uint8_t suit; uint8_t rank; } Card;
typedef struct { Card cards[DECK_SIZE]; int top_index; } Deck;
typedef struct { Card hand[MAX_HAND]; int card_count; bool is_ai; } Player;

typedef struct { char name[MAX_AI_NAME]; int level; } Opponent;

typedef struct {
    Deck deck;
    Player players[2]; 
    Suit trump_suit;   
    Card trump_card;   
    bool is_player_turn; 
    Card table_attack[MAX_TABLE];  
    Card table_defense[MAX_TABLE]; 
    int table_pair_count;          
    GameResult result;          
    int player_money;
    int current_level;
    Opponent current_opponent;
} GameState;

void game_init_global(GameState* state);
void game_init(GameState* state);
void game_set_opponent(GameState* state, int level);
void game_check_status(GameState* state);

// =========================================================
// ВИПРАВЛЕННЯ: Додані прототипи відсутніх функцій
// =========================================================
void game_shuffle_deck(GameState* state);
void game_deal_cards(GameState* state);
int game_ai_choose_attack(GameState* state);
int game_ai_choose_defend(GameState* state, Card attack_card);
bool game_can_attack_with(GameState* state, Card card);
bool game_can_defend_with(GameState* state, Card attack_card, Card defend_card);
void game_end_turn(GameState* state, bool took);

#endif