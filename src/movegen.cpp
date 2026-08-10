#include "movegen.h"
#include <cassert>
#include <iterator>
#include <array>
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
U64* bishop_pext_attacks[64];
U64* rook_pext_attacks[64];
bool use_pext_attacks = false;

U64 between_bb[64][64];
U64 line_bb[64][64];

// Flat tables for sliding attacks
U64 bishop_attacks_table[5248];
U64 rook_attacks_table[102400];
U64 bishop_pext_attacks_table[5248];
U64 rook_pext_attacks_table[102400];

bool pext_available() {
#if defined(__BMI2__)
    return __builtin_cpu_supports("bmi2");
#else
    return false;
#endif
}

void set_pext_enabled(bool enabled) {
    use_pext_attacks = enabled && pext_available();
}

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

        assert(bishop_offset + (1ULL << b_bits) <= std::size(bishop_attacks_table));
        assert(rook_offset + (1ULL << r_bits) <= std::size(rook_attacks_table));
        bishop_attacks[sq] = &bishop_attacks_table[bishop_offset];
        rook_attacks[sq] = &rook_attacks_table[rook_offset];
        bishop_pext_attacks[sq] = &bishop_pext_attacks_table[bishop_offset];
        rook_pext_attacks[sq] = &rook_pext_attacks_table[rook_offset];

        // Fill attack tables
        int b_indices = 1 << b_bits;
        for (int i = 0; i < b_indices; i++) {
            U64 block = set_occupancy_helper(i, bishop_masks[sq]);
            int idx = (block * bishop_magics[sq]) >> bishop_shifts[sq];
            assert(idx >= 0 && idx < b_indices);
            bishop_attacks[sq][idx] = bishop_attacks_on_the_fly(sq, block);
            bishop_pext_attacks[sq][i] = bishop_attacks_on_the_fly(sq, block);
        }

        int r_indices = 1 << r_bits;
        for (int i = 0; i < r_indices; i++) {
            U64 block = set_occupancy_helper(i, rook_masks[sq]);
            int idx = (block * rook_magics[sq]) >> rook_shifts[sq];
            assert(idx >= 0 && idx < r_indices);
            rook_attacks[sq][idx] = rook_attacks_on_the_fly(sq, block);
            rook_pext_attacks[sq][i] = rook_attacks_on_the_fly(sq, block);
        }

        bishop_offset += b_indices;
        rook_offset += r_indices;
    }
    assert(bishop_offset == std::size(bishop_attacks_table));
    assert(rook_offset == std::size(rook_attacks_table));
    set_pext_enabled(true);

    // 3. Initialize between_bb and line_bb
    for (int sq1 = 0; sq1 < 64; sq1++) {
        for (int sq2 = 0; sq2 < 64; sq2++) {
            between_bb[sq1][sq2] = 0ULL;
            line_bb[sq1][sq2] = 0ULL;
            if (sq1 == sq2) continue;
            
            int r1 = sq1 / 8, f1 = sq1 % 8;
            int r2 = sq2 / 8, f2 = sq2 % 8;
            
            int dr = r2 - r1;
            int df = f2 - f1;
            
            if (dr == 0) { // Same rank
                line_bb[sq1][sq2] = 0xFFULL << (r1 * 8);
                int step = (df > 0) ? 1 : -1;
                for (int f = f1 + step; f != f2; f += step) {
                    between_bb[sq1][sq2] |= (1ULL << (r1 * 8 + f));
                }
            } else if (df == 0) { // Same file
                line_bb[sq1][sq2] = 0x0101010101010101ULL << f1;
                int step = (dr > 0) ? 1 : -1;
                for (int r = r1 + step; r != r2; r += step) {
                    between_bb[sq1][sq2] |= (1ULL << (r * 8 + f1));
                }
            } else if (abs(dr) == abs(df)) { // Same diagonal
                int step_r = (dr > 0) ? 1 : -1;
                int step_f = (df > 0) ? 1 : -1;
                
                // Precompute full line
                for (int r = r1, f = f1; r >= 0 && r < 8 && f >= 0 && f < 8; r += step_r, f += step_f) {
                    line_bb[sq1][sq2] |= (1ULL << (r * 8 + f));
                }
                for (int r = r1 - step_r, f = f1 - step_f; r >= 0 && r < 8 && f >= 0 && f < 8; r -= step_r, f -= step_f) {
                    line_bb[sq1][sq2] |= (1ULL << (r * 8 + f));
                }
                
                for (int r = r1 + step_r, f = f1 + step_f; r != r2; r += step_r, f += step_f) {
                    between_bb[sq1][sq2] |= (1ULL << (r * 8 + f));
                }
            }
        }
    }
}

namespace {
void add_pawn_promotions(MoveList& list, int from, int to, bool capture) {
    const int base = capture ? FLAG_PROMO_KNIGHT_CAP : FLAG_PROMO_KNIGHT;
    list.add(Move(from, to, base));
    list.add(Move(from, to, base + 1));
    list.add(Move(from, to, base + 2));
    list.add(Move(from, to, base + 3));
}

void generate_pawn_moves_oracle(const Board& board, MoveList& list) {
    const Color us = board.get_side_to_move();
    const Color them = Color(us ^ 1);
    const U64 enemy = board.get_occupancy(them);
    const U64 empty = ~board.get_occupancy(BOTH);
    U64 pawns = board.get_pieces(us, PAWN);

    while (pawns) {
        const int from = pop_lsb(pawns);
        const int rank = from / 8;
        const int step = us == WHITE ? 8 : -8;
        const int to = from + step;

        if (to >= 0 && to < 64 && (empty & (1ULL << to))) {
            if (to >= 56 || to <= 7)
                add_pawn_promotions(list, from, to, false);
            else
                list.add(Move(from, to, FLAG_QUIET));

            const int to2 = from + 2 * step;
            if (((us == WHITE && rank == 1) || (us == BLACK && rank == 6))
                && (empty & (1ULL << to2)))
                list.add(Move(from, to2, FLAG_DOUBLE_PAWN));
        }

        U64 attacks = pawn_attacks[us][from] & enemy;
        while (attacks) {
            const int target = pop_lsb(attacks);
            if (target >= 56 || target <= 7)
                add_pawn_promotions(list, from, target, true);
            else
                list.add(Move(from, target, FLAG_CAPTURE));
        }

        const int ep = board.get_en_passant_square();
        if (ep != SQ_NONE && (pawn_attacks[us][from] & (1ULL << ep)))
            list.add(Move(from, ep, FLAG_EP));
    }
}

void generate_pawn_moves_setwise(const Board& board, MoveList& list) {
    constexpr U64 RANK_3 = 0x0000000000FF0000ULL;
    constexpr U64 RANK_6 = 0x0000FF0000000000ULL;

    const Color us = board.get_side_to_move();
    const Color them = Color(us ^ 1);
    const U64 pawns = board.get_pieces(us, PAWN);
    const U64 enemy = board.get_occupancy(them);
    const U64 empty = ~board.get_occupancy(BOTH);

    const int ep = board.get_en_passant_square();
    const U64 single = us == WHITE ? (pawns << 8) & empty : (pawns >> 8) & empty;
    const U64 doubles = us == WHITE ? ((single & RANK_3) << 8) & empty
                                    : ((single & RANK_6) >> 8) & empty;
    const U64 capture_targets = (us == WHITE ? pawn_attacks_white(pawns)
                                             : pawn_attacks_black(pawns)) & enemy;

    // Emit in the original source-square order. This retains tied move-order
    // behavior while the availability masks themselves are computed setwise.
    U64 sources = pawns;
    while (sources) {
        const int from = pop_lsb(sources);
        const int step = us == WHITE ? 8 : -8;
        const int to = from + step;
        if (single & (1ULL << to)) {
            if (to >= 56 || to <= 7)
                add_pawn_promotions(list, from, to, false);
            else
                list.add(Move(from, to, FLAG_QUIET));

            const int to2 = from + 2 * step;
            if (to2 >= 0 && to2 < 64 && (doubles & (1ULL << to2)))
                list.add(Move(from, to2, FLAG_DOUBLE_PAWN));
        }

        U64 attacks = pawn_attacks[us][from] & capture_targets;
        while (attacks) {
            const int target = pop_lsb(attacks);
            if (target >= 56 || target <= 7)
                add_pawn_promotions(list, from, target, true);
            else
                list.add(Move(from, target, FLAG_CAPTURE));
        }

        if (ep != SQ_NONE && (pawn_attacks[us][from] & (1ULL << ep)))
            list.add(Move(from, ep, FLAG_EP));
    }
}

#ifndef NDEBUG
bool same_move_set(const MoveList& lhs, const MoveList& rhs) {
    if (lhs.count != rhs.count) return false;
    std::array<bool, 65536> seen{};
    for (int i = 0; i < lhs.count; ++i) {
        if (seen[lhs.moves[i].value]) return false;
        seen[lhs.moves[i].value] = true;
    }
    for (int i = 0; i < rhs.count; ++i) {
        if (!seen[rhs.moves[i].value]) return false;
        seen[rhs.moves[i].value] = false;
    }
    return true;
}
#endif
}

enum class GenerationMode : uint8_t { ALL, CAPTURES, QUIETS, NOISY };

inline bool includes_move(GenerationMode mode, Move move) {
    switch (mode) {
        case GenerationMode::CAPTURES: return move.is_capture();
        case GenerationMode::QUIETS:   return !move.is_capture();
        case GenerationMode::NOISY:    return move.is_capture() || move.is_promotion();
        case GenerationMode::ALL:      return true;
    }
    return false;
}

inline void emit_move(MoveList& list, GenerationMode mode, Move move) {
    if (includes_move(mode, move))
        list.add(move);
}

void generate_moves(const Board& board, MoveList& move_list, GenerationMode mode) {
    Color us = board.get_side_to_move();
    Color them = (Color)(us ^ 1);

    U64 own_occ = board.get_occupancy(us);
    U64 enemy_occ = board.get_occupancy(them);
    U64 empty = ~board.get_occupancy(BOTH);

    // 1. The setwise candidate is retained only as a debug differential
    // oracle: it regressed release NPS, so production keeps the proven
    // per-pawn emission path.
#ifndef NDEBUG
    MoveList pawn_setwise;
    generate_pawn_moves_setwise(board, pawn_setwise);
    MoveList pawn_oracle;
    generate_pawn_moves_oracle(board, pawn_oracle);
    assert(same_move_set(pawn_setwise, pawn_oracle));
    for (int i = 0; i < pawn_oracle.count; ++i)
        emit_move(move_list, mode, pawn_oracle.moves[i]);
#else
    if (mode == GenerationMode::ALL) {
        generate_pawn_moves_oracle(board, move_list);
    } else {
        MoveList pawn_moves;
        generate_pawn_moves_oracle(board, pawn_moves);
        for (int i = 0; i < pawn_moves.count; ++i)
            emit_move(move_list, mode, pawn_moves.moves[i]);
    }
#endif

    // 2. Knight Moves
    U64 knights = board.get_pieces(us, KNIGHT);
    while (knights) {
        int from = pop_lsb(knights);
        U64 attacks = knight_attacks[from] & ~own_occ;
        U64 captures = attacks & enemy_occ;
        U64 quiets = attacks & empty;
        while (captures) {
            int to = pop_lsb(captures);
            emit_move(move_list, mode, Move(from, to, FLAG_CAPTURE));
        }
        while (quiets) {
            int to = pop_lsb(quiets);
            emit_move(move_list, mode, Move(from, to, FLAG_QUIET));
        }
    }

    // 3. Bishop Moves
    U64 bishops = board.get_pieces(us, BISHOP);
    while (bishops) {
        int from = pop_lsb(bishops);
        U64 attacks = get_bishop_attacks(from, board.get_occupancy(BOTH)) & ~own_occ;
        U64 captures = attacks & enemy_occ;
        U64 quiets = attacks & empty;
        while (captures) {
            int to = pop_lsb(captures);
            emit_move(move_list, mode, Move(from, to, FLAG_CAPTURE));
        }
        while (quiets) {
            int to = pop_lsb(quiets);
            emit_move(move_list, mode, Move(from, to, FLAG_QUIET));
        }
    }

    // 4. Rook Moves
    U64 rooks = board.get_pieces(us, ROOK);
    while (rooks) {
        int from = pop_lsb(rooks);
        U64 attacks = get_rook_attacks(from, board.get_occupancy(BOTH)) & ~own_occ;
        U64 captures = attacks & enemy_occ;
        U64 quiets = attacks & empty;
        while (captures) {
            int to = pop_lsb(captures);
            emit_move(move_list, mode, Move(from, to, FLAG_CAPTURE));
        }
        while (quiets) {
            int to = pop_lsb(quiets);
            emit_move(move_list, mode, Move(from, to, FLAG_QUIET));
        }
    }

    // 5. Queen Moves
    U64 queens = board.get_pieces(us, QUEEN);
    while (queens) {
        int from = pop_lsb(queens);
        U64 attacks = get_queen_attacks(from, board.get_occupancy(BOTH)) & ~own_occ;
        U64 captures = attacks & enemy_occ;
        U64 quiets = attacks & empty;
        while (captures) {
            int to = pop_lsb(captures);
            emit_move(move_list, mode, Move(from, to, FLAG_CAPTURE));
        }
        while (quiets) {
            int to = pop_lsb(quiets);
            emit_move(move_list, mode, Move(from, to, FLAG_QUIET));
        }
    }

    // 6. King Moves & Castling
    U64 king = board.get_pieces(us, KING);
    if (king) {
        int from = get_lsb(king);
        U64 attacks = king_attacks[from] & ~own_occ;
        U64 captures = attacks & enemy_occ;
        U64 quiets = attacks & empty;
        while (captures) {
            int to = pop_lsb(captures);
            emit_move(move_list, mode, Move(from, to, FLAG_CAPTURE));
        }
        while (quiets) {
            int to = pop_lsb(quiets);
            emit_move(move_list, mode, Move(from, to, FLAG_QUIET));
        }

        // Castling rights checking
        int rights = board.get_castling_rights();
        if (us == WHITE) {
            // White King Castle
            if (rights & WHITE_OO) {
                if (!(board.get_occupancy(BOTH) & ((1ULL << SQ_F1) | (1ULL << SQ_G1)))) {
                    if (!board.is_square_attacked(SQ_E1, BLACK) && !board.is_square_attacked(SQ_F1, BLACK)) {
                        emit_move(move_list, mode, Move(SQ_E1, SQ_G1, FLAG_KING_CASTLE));
                    }
                }
            }
            // White Queen Castle
            if (rights & WHITE_OOO) {
                if (!(board.get_occupancy(BOTH) & ((1ULL << SQ_D1) | (1ULL << SQ_C1) | (1ULL << SQ_B1)))) {
                    if (!board.is_square_attacked(SQ_E1, BLACK) && !board.is_square_attacked(SQ_D1, BLACK)) {
                        emit_move(move_list, mode, Move(SQ_E1, SQ_C1, FLAG_QUEEN_CASTLE));
                    }
                }
            }
        } else { // us == BLACK
            // Black King Castle
            if (rights & BLACK_OO) {
                if (!(board.get_occupancy(BOTH) & ((1ULL << SQ_F8) | (1ULL << SQ_G8)))) {
                    if (!board.is_square_attacked(SQ_E8, WHITE) && !board.is_square_attacked(SQ_F8, WHITE)) {
                        emit_move(move_list, mode, Move(SQ_E8, SQ_G8, FLAG_KING_CASTLE));
                    }
                }
            }
            // Black Queen Castle
            if (rights & BLACK_OOO) {
                if (!(board.get_occupancy(BOTH) & ((1ULL << SQ_D8) | (1ULL << SQ_C8) | (1ULL << SQ_B8)))) {
                    if (!board.is_square_attacked(SQ_E8, WHITE) && !board.is_square_attacked(SQ_D8, WHITE)) {
                        emit_move(move_list, mode, Move(SQ_E8, SQ_C8, FLAG_QUEEN_CASTLE));
                    }
                }
            }
        }
    }
}

void generate_pseudo_legal_moves(const Board& board, MoveList& move_list) {
    generate_moves(board, move_list, GenerationMode::ALL);
}

void generate_capture_moves(const Board& board, MoveList& move_list) {
    generate_moves(board, move_list, GenerationMode::CAPTURES);
}

void generate_quiet_moves(const Board& board, MoveList& move_list) {
    generate_moves(board, move_list, GenerationMode::QUIETS);
}

void generate_noisy_moves(const Board& board, MoveList& move_list) {
    generate_moves(board, move_list, GenerationMode::NOISY);
}

void generate_evasion_moves(const Board& board, MoveList& move_list) {
    // Legality filtering in make_move()/is_move_legal() removes candidates
    // that do not evade check.  Keeping the oracle result here provides a
    // correctness boundary before introducing the dedicated fast backend.
    generate_moves(board, move_list, GenerationMode::ALL);
}
