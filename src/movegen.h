#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "types.h"
#include "board.h"

// Array-backed, zero-heap-allocation move list
struct MoveList {
    Move moves[256];
    int count;

    MoveList() : count(0) {}

    void add(Move move) {
        if (count < 256) {
            moves[count++] = move;
        }
    }
};

// Attack tables declarations (defined in movegen.cpp)
extern U64 pawn_attacks[2][64];
extern U64 knight_attacks[64];
extern U64 king_attacks[64];

extern U64 bishop_masks[64];
extern U64 rook_masks[64];
extern int bishop_shifts[64];
extern int rook_shifts[64];
extern U64 bishop_magics[64];
extern U64 rook_magics[64];

extern U64* bishop_attacks[64];
extern U64* rook_attacks[64];

// Initialization functions
void init_all_attack_tables();

// Inline attack getters
inline U64 get_pawn_attacks(int color, int square) {
    return pawn_attacks[color][square];
}

inline U64 get_knight_attacks(int square) {
    return knight_attacks[square];
}

inline U64 get_king_attacks(int square) {
    return king_attacks[square];
}

inline U64 get_bishop_attacks(int square, U64 occupancy) {
    U64 occ = occupancy & bishop_masks[square];
    int index = (occ * bishop_magics[square]) >> bishop_shifts[square];
    return bishop_attacks[square][index];
}

inline U64 get_rook_attacks(int square, U64 occupancy) {
    U64 occ = occupancy & rook_masks[square];
    int index = (occ * rook_magics[square]) >> rook_shifts[square];
    return rook_attacks[square][index];
}

inline U64 get_queen_attacks(int square, U64 occupancy) {
    return get_bishop_attacks(square, occupancy) | get_rook_attacks(square, occupancy);
}

// Pseudo-legal move generator
void generate_pseudo_legal_moves(const Board& board, MoveList& move_list);

#endif // MOVEGEN_H
