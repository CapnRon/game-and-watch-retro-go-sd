#include "game.h"
#include <stdlib.h>
#include <string.h>

static const char* ai_names[] = {
    "Alek", "Bob", "Carl", "Dave", "Erik", "Fred", "Greg", "Hank", "Ivan", "Jack",
    "Karl", "Liam", "Mike", "Nick", "Oleg", "Paul", "Quin", "Rick", "Stan", "Mnrouch"
};

void game_set_opponent(GameState* state, int level) {
    state->current_level = level;
    int idx = (level - 1) % 20;
    strncpy(state->current_opponent.name, ai_names[idx], MAX_AI_NAME);
    state->current_opponent.level = level;
}

void game_init_global(GameState* state) {
    memset(state, 0, sizeof(GameState));
    state->player_money = 500;
    game_set_opponent(state, 1);
    state->result = RESULT_NONE;
}

void game_check_status(GameState* state) {
    if (state->result != RESULT_NONE) return; 
    int cards_left = DECK_SIZE - state->deck.top_index;
    
    if (cards_left == 0 && state->players[0].card_count == 0 && state->players[1].card_count == 0) {
        state->result = RESULT_DRAW;
    } else if (cards_left == 0 && state->players[0].card_count == 0) {
        state->result = RESULT_WIN;
        state->player_money += 100; // Нагорода за перемогу
        game_set_opponent(state, state->current_level + 1); // Наступний суперник
    } else if (cards_left == 0 && state->players[1].card_count == 0) {
        state->result = RESULT_LOSE;
        state->player_money -= 50; 
    }
}

void game_init(GameState* state) {
    state->deck.top_index = 0;
    int idx = 0;
    for (int r = RANK_6; r <= RANK_A; r++) {
        for (int s = 0; s < SUIT_COUNT; s++) {
            state->deck.cards[idx].rank = r;
            state->deck.cards[idx].suit = s;
            idx++;
        }
    }
    state->players[0].card_count = 0;
    state->players[0].is_ai = false;
    state->players[1].card_count = 0;
    state->players[1].is_ai = true;
    state->table_pair_count = 0;
    state->result = RESULT_NONE;
    state->player_money -= 50; // Зняття за вхід до раунду
    
    for (int i = 0; i < MAX_TABLE; i++) {
        state->table_attack[i].rank = 0;
        state->table_defense[i].rank = 0;
    }
}

void game_shuffle_deck(GameState* state) {
    for (int i = DECK_SIZE - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        Card temp = state->deck.cards[i];
        state->deck.cards[i] = state->deck.cards[j];
        state->deck.cards[j] = temp;
    }
    state->trump_card = state->deck.cards[DECK_SIZE - 1];
    state->trump_suit = state->trump_card.suit;
    state->is_player_turn = true; 
}

void game_deal_cards(GameState* state) {
    for (int p = 0; p < 2; p++) {
        while (state->players[p].card_count < 6 && state->deck.top_index < DECK_SIZE) {
            state->players[p].hand[state->players[p].card_count] = state->deck.cards[state->deck.top_index];
            state->players[p].card_count++;
            state->deck.top_index++;
        }
    }
}

bool game_can_attack_with(GameState* state, Card card) {
    if (state->table_pair_count == 0) return true;
    if (state->table_pair_count >= MAX_TABLE) return false;
    for (int i = 0; i < state->table_pair_count; i++) {
        if (state->table_attack[i].rank == card.rank) return true;
        if (state->table_defense[i].rank == card.rank) return true;
    }
    return false;
}

bool game_can_defend_with(GameState* state, Card attack_card, Card defend_card) {
    if (defend_card.suit == attack_card.suit) return defend_card.rank > attack_card.rank;
    if (defend_card.suit == state->trump_suit) return true;
    return false;
}

// =========================================================
// ДИНАМІЧНИЙ ШІ: РІВЕНЬ СКЛАДНОСТІ ТА ВАЖКОСТІ ХОДІВ
// =========================================================

int game_ai_choose_attack(GameState* state) {
    int valid_indices[MAX_HAND];
    int valid_count = 0;
    
    for (int i = 0; i < state->players[1].card_count; i++) {
        if (game_can_attack_with(state, state->players[1].hand[i])) {
            valid_indices[valid_count++] = i;
        }
    }
    if (valid_count == 0) return -1;
    
    // На 1 рівні шанс зробити дурницю - 40%. З кожним рівнем він падає на 10%. На 5+ рівні бот ідеальний.
    int mistake_chance = 50 - (state->current_level * 10);
    if (mistake_chance < 0) mistake_chance = 0;
    
    if (rand() % 100 < mistake_chance) {
        return valid_indices[rand() % valid_count]; // Підкидає рандомну карту (може помилково злити козиря)
    }
    
    // Професійна логіка: вибір найслабшої карти, бережемо козирі
    int best_idx = valid_indices[0];
    for (int i = 1; i < valid_count; i++) {
        int idx = valid_indices[i];
        Card c = state->players[1].hand[idx];
        Card best_c = state->players[1].hand[best_idx];
        
        bool current_is_trump = (c.suit == state->trump_suit);
        bool best_is_trump = (best_c.suit == state->trump_suit);
        
        if (current_is_trump && !best_is_trump) continue;
        if (!current_is_trump && best_is_trump) best_idx = idx;
        else if (c.rank < best_c.rank) best_idx = idx;
    }
    return best_idx;
}

int game_ai_choose_defend(GameState* state, Card attack_card) {
    int valid_indices[MAX_HAND];
    int valid_count = 0;
    
    for (int i = 0; i < state->players[1].card_count; i++) {
        if (game_can_defend_with(state, attack_card, state->players[1].hand[i])) {
            valid_indices[valid_count++] = i;
        }
    }
    if (valid_count == 0) return -1;
    
    // Розрахунок шансу помилки для захисту ШІ
    int mistake_chance = 40 - (state->current_level * 8);
    if (mistake_chance < 0) mistake_chance = 0;
    
    if (rand() % 100 < mistake_chance) {
        return valid_indices[rand() % valid_count]; // Відбивається занадто великою картою або козирем завчасно
    }
    
    // Професійний захист: покрити мінімальною картою
    int best_idx = valid_indices[0];
    for (int i = 1; i < valid_count; i++) {
        int idx = valid_indices[i];
        Card c = state->players[1].hand[idx];
        Card best_c = state->players[1].hand[best_idx];
        
        bool current_is_trump = (c.suit == state->trump_suit);
        bool best_is_trump = (best_c.suit == state->trump_suit);
        
        if (current_is_trump && !best_is_trump) continue;
        if (!current_is_trump && best_is_trump) best_idx = idx;
        else if (c.rank < best_c.rank) best_idx = idx;
    }
    return best_idx;
}

void game_end_turn(GameState* state, bool took) {
    int defender_idx = state->is_player_turn ? 1 : 0;
    if (took) {
        for (int i = 0; i < state->table_pair_count; i++) {
            if (state->table_attack[i].rank >= 6) {
                state->players[defender_idx].hand[state->players[defender_idx].card_count] = state->table_attack[i];
                state->players[defender_idx].card_count++;
            }
            if (state->table_defense[i].rank >= 6) {
                state->players[defender_idx].hand[state->players[defender_idx].card_count] = state->table_defense[i];
                state->players[defender_idx].card_count++;
            }
        }
    } else {
        state->is_player_turn = !state->is_player_turn;
    }
    
    state->table_pair_count = 0;
    for (int i = 0; i < MAX_TABLE; i++) {
        state->table_attack[i].rank = 0;
        state->table_defense[i].rank = 0;
    }
    
    game_deal_cards(state);
    game_check_status(state);
}