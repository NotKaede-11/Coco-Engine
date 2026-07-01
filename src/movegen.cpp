#include "movegen.h"
#include <vector>
#include <algorithm>
#include <iostream>

// Define global attack tables
U64 pawn_attacks[2][64];
U64 knight_attacks[64];
U64 king_attacks[64];

U64 bishop_masks[64];
U64 rook_masks[64];
int bishop_shifts[64];
int rook_shifts[64];
U64 bishop_magics[64];
U64 rook_magics[64];

U64* bishop_attacks[64];
U64* rook_attacks[64];

// Flat tables for sliding attacks
U64 bishop_attacks_table[5248];
U64 rook_attacks_table[102400];

// Helper functions for magic bitboards generation
U64 bishop_attacks_on_the_fly(int sq, U64 block) {
    U64 attacks = 0;
    int r, f;
    int target_r = sq / 8;
    int target_f = sq % 8;

    for (r = target_r + 1, f = target_f + 1; r <= 7 && f <= 7; r++, f++) {
        int s = r * 8 + f;
        attacks |= (1ULL << s);
        if (block & (1ULL << s)) break;
    }
    for (r = target_r + 1, f = target_f - 1; r <= 7 && f >= 0; r++, f--) {
        int s = r * 8 + f;
        attacks |= (1ULL << s);
        if (block & (1ULL << s)) break;
    }
    for (r = target_r - 1, f = target_f + 1; r >= 0 && f <= 7; r--, f++) {
        int s = r * 8 + f;
        attacks |= (1ULL << s);
        if (block & (1ULL << s)) break;
    }
    for (r = target_r - 1, f = target_f - 1; r >= 0 && f >= 0; r--, f--) {
        int s = r * 8 + f;
        attacks |= (1ULL << s);
        if (block & (1ULL << s)) break;
    }
    return attacks;
}

U64 rook_attacks_on_the_fly(int sq, U64 block) {
    U64 attacks = 0;
    int r, f;
    int target_r = sq / 8;
    int target_f = sq % 8;

    for (r = target_r + 1; r <= 7; r++) {
        int s = r * 8 + target_f;
        attacks |= (1ULL << s);
        if (block & (1ULL << s)) break;
    }
    for (r = target_r - 1; r >= 0; r--) {
        int s = r * 8 + target_f;
        attacks |= (1ULL << s);
        if (block & (1ULL << s)) break;
    }
    for (f = target_f + 1; f <= 7; f++) {
        int s = target_r * 8 + f;
        attacks |= (1ULL << s);
        if (block & (1ULL << s)) break;
    }
    for (f = target_f - 1; f >= 0; f--) {
        int s = target_r * 8 + f;
        attacks |= (1ULL << s);
        if (block & (1ULL << s)) break;
    }
    return attacks;
}

U64 get_bishop_mask(int sq) {
    U64 mask = 0;
    int r = sq / 8;
    int f = sq % 8;
    for (int tr = r + 1, tf = f + 1; tr <= 6 && tf <= 6; tr++, tf++) mask |= (1ULL << (tr * 8 + tf));
    for (int tr = r + 1, tf = f - 1; tr <= 6 && tf >= 1; tr++, tf--) mask |= (1ULL << (tr * 8 + tf));
    for (int tr = r - 1, tf = f + 1; tr >= 1 && tf <= 6; tr--, tf++) mask |= (1ULL << (tr * 8 + tf));
    for (int tr = r - 1, tf = f - 1; tr >= 1 && tf >= 1; tr--, tf--) mask |= (1ULL << (tr * 8 + tf));
    return mask;
}

U64 get_rook_mask(int sq) {
    U64 mask = 0;
    int r = sq / 8;
    int f = sq % 8;
    for (int tr = r + 1; tr <= 6; tr++) mask |= (1ULL << (tr * 8 + f));
    for (int tr = r - 1; tr >= 1; tr--) mask |= (1ULL << (tr * 8 + f));
    for (int tf = f + 1; tf <= 6; tf++) mask |= (1ULL << (r * 8 + tf));
    for (int tf = f - 1; tf >= 1; tf--) mask |= (1ULL << (r * 8 + tf));
    return mask;
}

U64 set_occupancy_helper(int index, U64 mask) {
    U64 occupancy = 0ULL;
    int bit_count = count_bits(mask);
    for (int i = 0; i < bit_count; i++) {
        int sq = get_lsb(mask);
        clear_bit(mask, sq);
        if (index & (1 << i)) {
            occupancy |= (1ULL << sq);
        }
    }
    return occupancy;
}

// Generate magic numbers dynamically
U64 find_magic(int sq, int relevant_bits, bool bishop) {
    U64 mask = bishop ? get_bishop_mask(sq) : get_rook_mask(sq);
    int num_indices = 1 << relevant_bits;

    std::vector<U64> blockers(num_indices);
    std::vector<U64> attacks(num_indices);

    for (int i = 0; i < num_indices; i++) {
        blockers[i] = set_occupancy_helper(i, mask);
        attacks[i] = bishop ? bishop_attacks_on_the_fly(sq, blockers[i]) : rook_attacks_on_the_fly(sq, blockers[i]);
    }

    U64 state = 1804289383ULL + sq * 987654321ULL;
    auto next_random = [&]() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 2685821657736338717ULL;
    };

    std::vector<U64> used_attacks(num_indices, 0ULL);
    int shift = 64 - relevant_bits;

    while (true) {
        U64 candidate = next_random() & next_random() & next_random();
        if (count_bits((candidate * mask) & 0xFF00000000000000ULL) < 6) continue;

        std::fill(used_attacks.begin(), used_attacks.end(), 0ULL);
        bool fail = false;
        for (int i = 0; i < num_indices; i++) {
            int idx = (blockers[i] * candidate) >> shift;
            if (used_attacks[idx] == 0) {
                used_attacks[idx] = attacks[i];
            } else if (used_attacks[idx] != attacks[i]) {
                fail = fail = true;
                break;
            }
        }
        if (!fail) {
            return candidate;
        }
    }
}

void init_all_attack_tables() {
    // 1. Initialize leaps (pawns, knights, kings)
    for (int sq = 0; sq < 64; sq++) {
        int r = sq / 8;
        int f = sq % 8;

        // Pawns
        pawn_attacks[WHITE][sq] = 0;
        pawn_attacks[BLACK][sq] = 0;
        if (r < 7) {
            if (f > 0) set_bit(pawn_attacks[WHITE][sq], (r + 1) * 8 + (f - 1));
            if (f < 7) set_bit(pawn_attacks[WHITE][sq], (r + 1) * 8 + (f + 1));
        }
        if (r > 0) {
            if (f > 0) set_bit(pawn_attacks[BLACK][sq], (r - 1) * 8 + (f - 1));
            if (f < 7) set_bit(pawn_attacks[BLACK][sq], (r - 1) * 8 + (f + 1));
        }

        // Knights
        knight_attacks[sq] = 0;
        int knight_offsets[8][2] = {
            {2, 1}, {2, -1}, {1, 2}, {1, -2},
            {-2, 1}, {-2, -1}, {-1, 2}, {-1, -2}
        };
        for (int i = 0; i < 8; i++) {
            int tr = r + knight_offsets[i][0];
            int tf = f + knight_offsets[i][1];
            if (tr >= 0 && tr <= 7 && tf >= 0 && tf <= 7) {
                set_bit(knight_attacks[sq], tr * 8 + tf);
            }
        }

        // Kings
        king_attacks[sq] = 0;
        int king_offsets[8][2] = {
            {1, 0}, {-1, 0}, {0, 1}, {0, -1},
            {1, 1}, {1, -1}, {-1, 1}, {-1, -1}
        };
        for (int i = 0; i < 8; i++) {
            int tr = r + king_offsets[i][0];
            int tf = f + king_offsets[i][1];
            if (tr >= 0 && tr <= 7 && tf >= 0 && tf <= 7) {
                set_bit(king_attacks[sq], tr * 8 + tf);
            }
        }
    }

    // 2. Initialize sliding attack magics and table pointers
    U64 bishop_offset = 0;
    U64 rook_offset = 0;

    for (int sq = 0; sq < 64; sq++) {
        bishop_masks[sq] = get_bishop_mask(sq);
        rook_masks[sq] = get_rook_mask(sq);

        int b_bits = count_bits(bishop_masks[sq]);
        int r_bits = count_bits(rook_masks[sq]);

        bishop_shifts[sq] = 64 - b_bits;
        rook_shifts[sq] = 64 - r_bits;

        bishop_magics[sq] = find_magic(sq, b_bits, true);
        rook_magics[sq] = find_magic(sq, r_bits, false);

        bishop_attacks[sq] = &bishop_attacks_table[bishop_offset];
        rook_attacks[sq] = &rook_attacks_table[rook_offset];

        // Fill attack tables
        int b_indices = 1 << b_bits;
        for (int i = 0; i < b_indices; i++) {
            U64 block = set_occupancy_helper(i, bishop_masks[sq]);
            int idx = (block * bishop_magics[sq]) >> bishop_shifts[sq];
            bishop_attacks[sq][idx] = bishop_attacks_on_the_fly(sq, block);
        }

        int r_indices = 1 << r_bits;
        for (int i = 0; i < r_indices; i++) {
            U64 block = set_occupancy_helper(i, rook_masks[sq]);
            int idx = (block * rook_magics[sq]) >> rook_shifts[sq];
            rook_attacks[sq][idx] = rook_attacks_on_the_fly(sq, block);
        }

        bishop_offset += b_indices;
        rook_offset += r_indices;
    }
}

void generate_pseudo_legal_moves(const Board& board, MoveList& move_list) {
    Color us = board.get_side_to_move();
    Color them = (Color)(us ^ 1);

    U64 own_occ = board.get_occupancy(us);
    U64 enemy_occ = board.get_occupancy(them);
    U64 empty = ~board.get_occupancy(BOTH);

    // 1. Pawn Moves
    U64 pawns = board.get_pieces(us, PAWN);
    while (pawns) {
        int from = pop_lsb(pawns);
        int r = from / 8;
        int f = from % 8;

        if (us == WHITE) {
            // Single push
            int to = from + 8;
            if (to < 64 && (empty & (1ULL << to))) {
                if (to >= 56) { // Promotion
                    move_list.add(Move(from, to, FLAG_PROMO_KNIGHT));
                    move_list.add(Move(from, to, FLAG_PROMO_BISHOP));
                    move_list.add(Move(from, to, FLAG_PROMO_ROOK));
                    move_list.add(Move(from, to, FLAG_PROMO_QUEEN));
                } else {
                    move_list.add(Move(from, to, FLAG_QUIET));
                }

                // Double push
                int to2 = from + 16;
                if (r == 1 && (empty & (1ULL << to2))) {
                    move_list.add(Move(from, to2, FLAG_DOUBLE_PAWN));
                }
            }

            // Normal attacks
            U64 attacks = pawn_attacks[WHITE][from] & enemy_occ;
            while (attacks) {
                int target = pop_lsb(attacks);
                if (target >= 56) { // Promotion capture
                    move_list.add(Move(from, target, FLAG_PROMO_KNIGHT_CAP));
                    move_list.add(Move(from, target, FLAG_PROMO_BISHOP_CAP));
                    move_list.add(Move(from, target, FLAG_PROMO_ROOK_CAP));
                    move_list.add(Move(from, target, FLAG_PROMO_QUEEN_CAP));
                } else {
                    move_list.add(Move(from, target, FLAG_CAPTURE));
                }
            }

            // En passant
            int ep = board.get_en_passant_square();
            if (ep != SQ_NONE) {
                if (pawn_attacks[WHITE][from] & (1ULL << ep)) {
                    move_list.add(Move(from, ep, FLAG_EP));
                }
            }
        } else { // us == BLACK
            // Single push
            int to = from - 8;
            if (to >= 0 && (empty & (1ULL << to))) {
                if (to <= 7) { // Promotion
                    move_list.add(Move(from, to, FLAG_PROMO_KNIGHT));
                    move_list.add(Move(from, to, FLAG_PROMO_BISHOP));
                    move_list.add(Move(from, to, FLAG_PROMO_ROOK));
                    move_list.add(Move(from, to, FLAG_PROMO_QUEEN));
                } else {
                    move_list.add(Move(from, to, FLAG_QUIET));
                }

                // Double push
                int to2 = from - 16;
                if (r == 6 && (empty & (1ULL << to2))) {
                    move_list.add(Move(from, to2, FLAG_DOUBLE_PAWN));
                }
            }

            // Normal attacks
            U64 attacks = pawn_attacks[BLACK][from] & enemy_occ;
            while (attacks) {
                int target = pop_lsb(attacks);
                if (target <= 7) { // Promotion capture
                    move_list.add(Move(from, target, FLAG_PROMO_KNIGHT_CAP));
                    move_list.add(Move(from, target, FLAG_PROMO_BISHOP_CAP));
                    move_list.add(Move(from, target, FLAG_PROMO_ROOK_CAP));
                    move_list.add(Move(from, target, FLAG_PROMO_QUEEN_CAP));
                } else {
                    move_list.add(Move(from, target, FLAG_CAPTURE));
                }
            }

            // En passant
            int ep = board.get_en_passant_square();
            if (ep != SQ_NONE) {
                if (pawn_attacks[BLACK][from] & (1ULL << ep)) {
                    move_list.add(Move(from, ep, FLAG_EP));
                }
            }
        }
    }

    // 2. Knight Moves
    U64 knights = board.get_pieces(us, KNIGHT);
    while (knights) {
        int from = pop_lsb(knights);
        U64 attacks = knight_attacks[from] & ~own_occ;
        while (attacks) {
            int to = pop_lsb(attacks);
            if (enemy_occ & (1ULL << to)) {
                move_list.add(Move(from, to, FLAG_CAPTURE));
            } else {
                move_list.add(Move(from, to, FLAG_QUIET));
            }
        }
    }

    // 3. Bishop Moves
    U64 bishops = board.get_pieces(us, BISHOP);
    while (bishops) {
        int from = pop_lsb(bishops);
        U64 attacks = get_bishop_attacks(from, board.get_occupancy(BOTH)) & ~own_occ;
        while (attacks) {
            int to = pop_lsb(attacks);
            if (enemy_occ & (1ULL << to)) {
                move_list.add(Move(from, to, FLAG_CAPTURE));
            } else {
                move_list.add(Move(from, to, FLAG_QUIET));
            }
        }
    }

    // 4. Rook Moves
    U64 rooks = board.get_pieces(us, ROOK);
    while (rooks) {
        int from = pop_lsb(rooks);
        U64 attacks = get_rook_attacks(from, board.get_occupancy(BOTH)) & ~own_occ;
        while (attacks) {
            int to = pop_lsb(attacks);
            if (enemy_occ & (1ULL << to)) {
                move_list.add(Move(from, to, FLAG_CAPTURE));
            } else {
                move_list.add(Move(from, to, FLAG_QUIET));
            }
        }
    }

    // 5. Queen Moves
    U64 queens = board.get_pieces(us, QUEEN);
    while (queens) {
        int from = pop_lsb(queens);
        U64 attacks = get_queen_attacks(from, board.get_occupancy(BOTH)) & ~own_occ;
        while (attacks) {
            int to = pop_lsb(attacks);
            if (enemy_occ & (1ULL << to)) {
                move_list.add(Move(from, to, FLAG_CAPTURE));
            } else {
                move_list.add(Move(from, to, FLAG_QUIET));
            }
        }
    }

    // 6. King Moves & Castling
    U64 king = board.get_pieces(us, KING);
    if (king) {
        int from = get_lsb(king);
        U64 attacks = king_attacks[from] & ~own_occ;
        while (attacks) {
            int to = pop_lsb(attacks);
            if (enemy_occ & (1ULL << to)) {
                move_list.add(Move(from, to, FLAG_CAPTURE));
            } else {
                move_list.add(Move(from, to, FLAG_QUIET));
            }
        }

        // Castling rights checking
        int rights = board.get_castling_rights();
        if (us == WHITE) {
            // White King Castle
            if (rights & WHITE_OO) {
                if (!(board.get_occupancy(BOTH) & ((1ULL << SQ_F1) | (1ULL << SQ_G1)))) {
                    if (!board.is_square_attacked(SQ_E1, BLACK) && !board.is_square_attacked(SQ_F1, BLACK)) {
                        move_list.add(Move(SQ_E1, SQ_G1, FLAG_KING_CASTLE));
                    }
                }
            }
            // White Queen Castle
            if (rights & WHITE_OOO) {
                if (!(board.get_occupancy(BOTH) & ((1ULL << SQ_D1) | (1ULL << SQ_C1) | (1ULL << SQ_B1)))) {
                    if (!board.is_square_attacked(SQ_E1, BLACK) && !board.is_square_attacked(SQ_D1, BLACK)) {
                        move_list.add(Move(SQ_E1, SQ_C1, FLAG_QUEEN_CASTLE));
                    }
                }
            }
        } else { // us == BLACK
            // Black King Castle
            if (rights & BLACK_OO) {
                if (!(board.get_occupancy(BOTH) & ((1ULL << SQ_F8) | (1ULL << SQ_G8)))) {
                    if (!board.is_square_attacked(SQ_E8, WHITE) && !board.is_square_attacked(SQ_F8, WHITE)) {
                        move_list.add(Move(SQ_E8, SQ_G8, FLAG_KING_CASTLE));
                    }
                }
            }
            // Black Queen Castle
            if (rights & BLACK_OOO) {
                if (!(board.get_occupancy(BOTH) & ((1ULL << SQ_D8) | (1ULL << SQ_C8) | (1ULL << SQ_B8)))) {
                    if (!board.is_square_attacked(SQ_E8, WHITE) && !board.is_square_attacked(SQ_D8, WHITE)) {
                        move_list.add(Move(SQ_E8, SQ_C8, FLAG_QUEEN_CASTLE));
                    }
                }
            }
        }
    }
}
