#include "evaluate.h"
#include "types.h"
#include "nnue.h"
#include "movegen.h"
#include "hce_tables.h"
#include <algorithm>
#include <cmath>

namespace Evaluation {

    const HceWeights& default_hce_weights() {
        static const HceWeights weights = [] {
            HceWeights result{};
            for (Score& weight : result.feature) weight = Score(1024, 1024);
            return result;
        }();
        return weights;
    }

    // Piece-Square Tables (PSQT) for positional scoring
    // Scores are from White's perspective. Black squares are flipped symmetrically.
    // Higher scores are better for the side playing.

    const int PSQT_PAWN[64] = {
         0,  0,  0,  0,  0,  0,  0,  0,
         5, 10, 15, 20, 20, 15, 10,  5,
         5,  5, 10, 25, 25, 10,  5,  5,
         0,  0, 15, 30, 30, 15,  0,  0,
         5,  5, 15, 35, 35, 15,  5,  5,
        10, 10, 20, 35, 35, 20, 10, 10,
        50, 50, 50, 50, 50, 50, 50, 50,
         0,  0,  0,  0,  0,  0,  0,  0
    };

    const int PSQT_KNIGHT[64] = {
        -50, -40, -30, -30, -30, -30, -40, -50,
        -40, -20,   0,   5,   5,   0, -20, -40,
        -30,   5,  10,  15,  15,  10,   5, -30,
        -30,   0,  15,  20,  20,  15,   0, -30,
        -30,   5,  15,  20,  20,  15,   5, -30,
        -30,   0,  10,  15,  15,  10,   0, -30,
        -40, -20,   0,   0,   0,   0, -20, -40,
        -50, -40, -30, -30, -30, -30, -40, -50
    };

    const int PSQT_BISHOP[64] = {
        -20, -10, -10, -10, -10, -10, -10, -20,
        -10,   5,   0,   0,   0,   0,   5, -10,
        -10,  10,  10,  10,  10,  10,  10, -10,
        -10,   0,  10,  10,  10,  10,   0, -10,
        -10,   5,   5,  10,  10,   5,   5, -10,
        -10,   0,   5,  10,  10,   5,   0, -10,
        -10,   0,   0,   0,   0,   0,   0, -10,
        -20, -10, -10, -10, -10, -10, -10, -20
    };

    const int PSQT_ROOK[64] = {
          0,  0,  5, 10, 10,  5,  0,  0,
          5, 10, 10, 10, 10, 10, 10,  5,
         -5,  0,  0,  0,  0,  0,  0, -5,
         -5,  0,  0,  0,  0,  0,  0, -5,
         -5,  0,  0,  0,  0,  0,  0, -5,
         -5,  0,  0,  0,  0,  0,  0, -5,
          5, 10, 10, 10, 10, 10, 10,  5,
          0,  0,  0,  0,  0,  0,  0,  0
    };

    const int PSQT_QUEEN[64] = {
        -20, -10, -10,  -5,  -5, -10, -10, -20,
        -10,   0,   5,   0,   0,   5,   0, -10,
        -10,   5,   5,   5,   5,   5,   5, -10,
         -5,   0,   5,   5,   5,   5,   0,  -5,
          0,   0,   5,   5,   5,   5,   0,   0,
        -10,   0,   5,   5,   5,   5,   0, -10,
        -10,   0,   0,   0,   0,   0,   0, -10,
        -20, -10, -10,  -5,  -5, -10, -10, -20
    };

    const int PSQT_KING[64] = {
         20,  30,  10,   0,   0,  10,  30,  20,
         20,  20,   0,   0,   0,   0,  20,  20,
        -10, -20, -20, -20, -20, -20, -20, -10,
        -20, -30, -30, -40, -40, -30, -30, -20,
        -30, -40, -40, -50, -50, -40, -40, -30,
        -30, -40, -40, -50, -50, -40, -40, -30,
        -30, -40, -40, -50, -50, -40, -40, -30,
        -40, -50, -50, -50, -50, -50, -50, -40
    };

    int evaluate_hce(const Board& board, HceTrace* trace) {
        return evaluate_hce(board, trace, default_hce_weights());
    }

    int evaluate_hce(const Board& board, HceTrace* trace, const HceWeights& weights) {
        constexpr int MG_VALUE[6] = {100, 320, 330, 500, 900, 0};
        constexpr int EG_VALUE[6] = {120, 310, 330, 510, 900, 0};
        constexpr int PHASE_WEIGHT[6] = {0, 1, 1, 2, 4, 0};
        constexpr int PASSER_MG[8] = {0, 0, 8, 18, 32, 55, 90, 0};
        constexpr int PASSER_EG[8] = {0, 5, 15, 32, 58, 100, 170, 0};
        constexpr int KNIGHT_MOBILITY_MG[9] = {-24, -16, -8, 0, 7, 13, 18, 22, 25};
        constexpr int KNIGHT_MOBILITY_EG[9] = {-20, -12, -5, 2, 8, 13, 17, 20, 22};
        constexpr int BISHOP_MOBILITY_MG[14] = {-20, -14, -8, -2, 4, 9, 14, 18, 22, 25, 28, 30, 32, 34};
        constexpr int BISHOP_MOBILITY_EG[14] = {-16, -10, -4, 2, 7, 12, 17, 21, 25, 28, 31, 34, 36, 38};
        constexpr int ROOK_MOBILITY_MG[15] = {-16, -12, -8, -4, 0, 4, 8, 11, 14, 17, 20, 23, 25, 27, 29};
        constexpr int ROOK_MOBILITY_EG[15] = {-12, -8, -4, 0, 5, 10, 15, 20, 25, 30, 34, 38, 42, 45, 48};
        constexpr int QUEEN_MOBILITY_MG[28] = {
            -12, -10, -8, -6, -4, -2, 0, 2, 4, 6, 8, 10, 12, 14,
            16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42
        };
        constexpr int QUEEN_MOBILITY_EG[28] = {
            -8, -6, -4, -2, 0, 2, 4, 6, 8, 10, 12, 14, 16, 18,
            20, 22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46
        };
        constexpr int MAX_PHASE = 24;
        constexpr U64 FILE_A = 0x0101010101010101ULL;
        Score material, positional, pawns, imbalance, passers, threats;
        Score mobility, king_safety, endgame, tempo;
        int phase = 0;

        auto attacks_by = [&](Color color) {
            U64 attacks = color == WHITE
                ? pawn_attacks_white(board.get_pieces(color, PAWN))
                : pawn_attacks_black(board.get_pieces(color, PAWN));
            U64 pieces = board.get_pieces(color, KNIGHT);
            while (pieces) attacks |= get_knight_attacks(pop_lsb(pieces));
            pieces = board.get_pieces(color, BISHOP);
            while (pieces) attacks |= get_bishop_attacks(pop_lsb(pieces), board.get_occupancy(BOTH));
            pieces = board.get_pieces(color, ROOK);
            while (pieces) attacks |= get_rook_attacks(pop_lsb(pieces), board.get_occupancy(BOTH));
            pieces = board.get_pieces(color, QUEEN);
            while (pieces) attacks |= get_queen_attacks(pop_lsb(pieces), board.get_occupancy(BOTH));
            pieces = board.get_pieces(color, KING);
            if (pieces) attacks |= get_king_attacks(get_lsb(pieces));
            return attacks;
        };

        const U64 all_attacks[2] = {attacks_by(WHITE), attacks_by(BLACK)};
        const U64 occupied = board.get_occupancy(BOTH);
        auto attackers_to = [&](Color color, int square, U64 occupancy) {
            return board.get_all_attackers(square, occupancy) & board.get_occupancy(color);
        };
        for (int color = WHITE; color <= BLACK; ++color) {
            const Color us = static_cast<Color>(color);
            const Color them = static_cast<Color>(color ^ 1);
            const int sign = color == WHITE ? 1 : -1;
            const U64 own_occ = board.get_occupancy(us);
            const U64 own_pawns = board.get_pieces(us, PAWN);
            const U64 enemy_pawns = board.get_pieces(them, PAWN);
            const U64 enemy_pawn_attacks = them == WHITE
                ? pawn_attacks_white(enemy_pawns) : pawn_attacks_black(enemy_pawns);

            for (int piece = PAWN; piece <= KING; ++piece) {
                U64 pieces = board.get_pieces(us, static_cast<PieceType>(piece));
                while (pieces) {
                    const int square = pop_lsb(pieces);
                    const int relative_square = color == WHITE ? square : square ^ 56;
                    material.mg += sign * MG_VALUE[piece];
                    material.eg += sign * EG_VALUE[piece];
                    positional.mg += sign * HCE_PSQT_MG[piece][relative_square];
                    positional.eg += sign * HCE_PSQT_EG[piece][relative_square];
                    phase += PHASE_WEIGHT[piece];
                }
            }

            // T2: pawn structure and basic material imbalances.
            for (int file = 0; file < 8; ++file) {
                const int count = count_bits(own_pawns & (FILE_A << file));
                if (count > 1) {
                    pawns.mg -= sign * 12 * (count - 1);
                    pawns.eg -= sign * 18 * (count - 1);
                }
            }
            U64 pawn_scan = own_pawns;
            bool has_queen_flank_pawn = false;
            bool has_king_flank_pawn = false;
            while (pawn_scan) {
                const int square = pop_lsb(pawn_scan);
                const int file = square & 7;
                const int rank = square >> 3;
                const int relative_rank = color == WHITE ? rank : 7 - rank;
                has_queen_flank_pawn |= file <= 3;
                has_king_flank_pawn |= file >= 4;
                U64 adjacent_files = 0;
                if (file > 0) adjacent_files |= FILE_A << (file - 1);
                if (file < 7) adjacent_files |= FILE_A << (file + 1);
                if (!(own_pawns & adjacent_files)) {
                    pawns.mg -= sign * 14;
                    pawns.eg -= sign * 10;
                }
                if (file < 7 && (own_pawns & (1ULL << (square + 1)))) {
                    pawns.mg += sign * (2 + 2 * relative_rank);
                    pawns.eg += sign * (3 + 3 * relative_rank);
                }
                const bool pawn_defended = (pawn_attacks[them][square] & own_pawns) != 0;
                if ((enemy_pawn_attacks & (1ULL << square)) && !pawn_defended) {
                    pawns.mg -= sign * 10;
                    pawns.eg -= sign * 6;
                }
                const int push_square = square + (color == WHITE ? 8 : -8);
                if (push_square >= 0 && push_square < 64
                    && (enemy_pawn_attacks & (1ULL << push_square)) && !pawn_defended) {
                    pawns.mg -= sign * 8;
                    pawns.eg -= sign * 12;
                }

                bool passed = true;
                U64 enemy_scan = enemy_pawns;
                while (enemy_scan) {
                    const int enemy_square = pop_lsb(enemy_scan);
                    const int enemy_file = enemy_square & 7;
                    const int enemy_rank = enemy_square >> 3;
                    if (std::abs(enemy_file - file) <= 1
                        && (color == WHITE ? enemy_rank > rank : enemy_rank < rank)) {
                        passed = false;
                        break;
                    }
                }
                if (passed) {
                    passers.mg += sign * PASSER_MG[relative_rank];
                    passers.eg += sign * PASSER_EG[relative_rank];
                    if (pawn_attacks[them][square] & own_pawns) {
                        passers.mg += sign * 10;
                        passers.eg += sign * 18;
                    }
                    if (push_square >= 0 && push_square < 64) {
                        const U64 push_bit = 1ULL << push_square;
                        if (board.get_occupancy(BOTH) & push_bit) {
                            passers.mg -= sign * 10;
                            passers.eg -= sign * 22;
                        } else if (!(all_attacks[color ^ 1] & push_bit)) {
                            passers.mg += sign * 8;
                            passers.eg += sign * 20;
                        }
                    }
                    const U64 file_mask = FILE_A << file;
                    const U64 behind_mask = color == WHITE
                        ? file_mask & (square == 0 ? 0ULL : (1ULL << square) - 1)
                        : file_mask & (square == 63 ? 0ULL : ~0ULL << (square + 1));
                    if (board.get_pieces(us, ROOK) & behind_mask) {
                        passers.mg += sign * 12;
                        passers.eg += sign * 28;
                    }
                }
            }
            if (has_queen_flank_pawn && has_king_flank_pawn) {
                pawns.mg += sign * 6;
                pawns.eg += sign * 10;
            }

            if (count_bits(board.get_pieces(us, BISHOP)) >= 2) {
                imbalance.mg += sign * 30;
                imbalance.eg += sign * 40;
            }
            if (count_bits(board.get_pieces(us, KNIGHT)) >= 2)
                imbalance.eg -= sign * 8;
            if (count_bits(board.get_pieces(us, ROOK)) >= 2)
                imbalance.mg -= sign * 10;
            const int pawn_count = count_bits(own_pawns);
            imbalance.mg += sign * count_bits(board.get_pieces(us, KNIGHT)) * (pawn_count - 4);
            imbalance.eg += sign * count_bits(board.get_pieces(us, ROOK)) * (4 - pawn_count);

            // T3: levers and threats.
            const U64 pawn_attack_map = color == WHITE
                ? pawn_attacks_white(own_pawns) : pawn_attacks_black(own_pawns);
            const int levers = count_bits(pawn_attack_map & enemy_pawns);
            pawns.mg += sign * 5 * levers;
            pawns.eg += sign * 8 * levers;
            const U64 enemy_nonpawns = board.get_occupancy(them) & ~enemy_pawns
                                     & ~board.get_pieces(them, KING);
            const int pawn_threats = count_bits(pawn_attack_map & enemy_nonpawns);
            threats.mg += sign * 18 * pawn_threats;
            threats.eg += sign * 12 * pawn_threats;
            const int hanging = count_bits(all_attacks[color] & enemy_nonpawns & ~all_attacks[color ^ 1]);
            threats.mg += sign * 12 * hanging;
            threats.eg += sign * 8 * hanging;
            U64 overloaded_scan = enemy_nonpawns;
            while (overloaded_scan) {
                const int square = pop_lsb(overloaded_scan);
                const int attackers = count_bits(attackers_to(us, square, occupied));
                const int defenders = count_bits(attackers_to(them, square, occupied));
                if (attackers >= 2 && defenders <= 1) {
                    threats.mg += sign * 10;
                    threats.eg += sign * 6;
                }
            }
            U64 push_threat_pawns = own_pawns;
            while (push_threat_pawns) {
                const int square = pop_lsb(push_threat_pawns);
                const int push_square = square + (color == WHITE ? 8 : -8);
                if (push_square >= 0 && push_square < 64
                    && !(board.get_occupancy(BOTH) & (1ULL << push_square))
                    && (pawn_attacks[us][push_square] & enemy_nonpawns)) {
                    threats.mg += sign * 12;
                    threats.eg += sign * 8;
                }
            }

            const U64 enemy_rooks = board.get_pieces(them, ROOK);
            U64 minor_scan = board.get_pieces(us, KNIGHT);
            while (minor_scan) {
                if (get_knight_attacks(pop_lsb(minor_scan)) & enemy_rooks) {
                    threats.mg += sign * 24;
                    threats.eg += sign * 16;
                }
            }
            minor_scan = board.get_pieces(us, BISHOP);
            while (minor_scan) {
                if (get_bishop_attacks(pop_lsb(minor_scan), occupied) & enemy_rooks) {
                    threats.mg += sign * 20;
                    threats.eg += sign * 14;
                }
            }

            const U64 enemy_king_bb = board.get_pieces(them, KING);
            if (enemy_king_bb) {
                const int enemy_king_square = get_lsb(enemy_king_bb);
                U64 possible_blockers = board.get_occupancy(them) & ~enemy_king_bb;
                while (possible_blockers) {
                    const int blocker = pop_lsb(possible_blockers);
                    const U64 without_blocker = occupied & ~(1ULL << blocker);
                    const U64 diagonal_sliders = board.get_pieces(us, BISHOP) | board.get_pieces(us, QUEEN);
                    const U64 straight_sliders = board.get_pieces(us, ROOK) | board.get_pieces(us, QUEEN);
                    const U64 diagonal_pinners = (get_bishop_attacks(enemy_king_square, without_blocker)
                        & ~get_bishop_attacks(enemy_king_square, occupied)) & diagonal_sliders;
                    const U64 straight_pinners = (get_rook_attacks(enemy_king_square, without_blocker)
                        & ~get_rook_attacks(enemy_king_square, occupied)) & straight_sliders;
                    if (diagonal_pinners | straight_pinners) {
                        threats.mg += sign * 14;
                        threats.eg += sign * 8;
                    }
                }
                const U64 checking_attackers = attackers_to(us, enemy_king_square, occupied)
                    & ~all_attacks[color ^ 1];
                const int safe_checks = count_bits(checking_attackers);
                threats.mg += sign * 18 * safe_checks;
                threats.eg += sign * 10 * safe_checks;

                U64 own_blockers = own_occ & ~board.get_pieces(us, KING)
                                 & ~board.get_pieces(us, BISHOP)
                                 & ~board.get_pieces(us, ROOK)
                                 & ~board.get_pieces(us, QUEEN);
                while (own_blockers) {
                    const int blocker = pop_lsb(own_blockers);
                    const U64 without_blocker = occupied & ~(1ULL << blocker);
                    const U64 discovered_diagonal = (get_bishop_attacks(enemy_king_square, without_blocker)
                        & ~get_bishop_attacks(enemy_king_square, occupied))
                        & (board.get_pieces(us, BISHOP) | board.get_pieces(us, QUEEN));
                    const U64 discovered_straight = (get_rook_attacks(enemy_king_square, without_blocker)
                        & ~get_rook_attacks(enemy_king_square, occupied))
                        & (board.get_pieces(us, ROOK) | board.get_pieces(us, QUEEN));
                    if (discovered_diagonal | discovered_straight) {
                        threats.mg += sign * 12;
                        threats.eg += sign * 6;
                    }
                }
            }

            // T4: mobility and activity.
            U64 pieces = board.get_pieces(us, KNIGHT);
            while (pieces) {
                const int square = pop_lsb(pieces);
                const U64 attacks = get_knight_attacks(square) & ~own_occ;
                const int moves = std::min(8, count_bits(attacks & ~enemy_pawn_attacks));
                mobility.mg += sign * KNIGHT_MOBILITY_MG[moves];
                mobility.eg += sign * KNIGHT_MOBILITY_EG[moves];
                if ((pawn_attacks[them][square] & own_pawns)
                    && !(enemy_pawn_attacks & (1ULL << square))) {
                    mobility.mg += sign * 18;
                    mobility.eg += sign * 12;
                }
            }
            pieces = board.get_pieces(us, BISHOP);
            while (pieces) {
                const int square = pop_lsb(pieces);
                const U64 attacks = get_bishop_attacks(square, board.get_occupancy(BOTH)) & ~own_occ;
                const int moves = std::min(13, count_bits(attacks & ~enemy_pawn_attacks));
                mobility.mg += sign * BISHOP_MOBILITY_MG[moves];
                mobility.eg += sign * BISHOP_MOBILITY_EG[moves];
                if ((pawn_attacks[them][square] & own_pawns)
                    && !(enemy_pawn_attacks & (1ULL << square))) {
                    mobility.mg += sign * 12;
                    mobility.eg += sign * 10;
                }
                const bool fianchetto = color == WHITE
                    ? (square == SQ_B2 || square == SQ_G2)
                    : (square == SQ_B7 || square == SQ_G7);
                if (fianchetto) {
                    mobility.mg += sign * 10;
                    mobility.eg += sign * 6;
                }
                constexpr U64 LONG_DIAGONALS = 0x8040201008040201ULL | 0x0102040810204080ULL;
                if ((1ULL << square) & LONG_DIAGONALS) {
                    mobility.mg += sign * 5;
                    mobility.eg += sign * 8;
                }
            }
            pieces = board.get_pieces(us, ROOK);
            while (pieces) {
                const int square = pop_lsb(pieces);
                const int file = square & 7;
                const int relative_rank = color == WHITE ? square >> 3 : 7 - (square >> 3);
                const U64 attacks = get_rook_attacks(square, board.get_occupancy(BOTH)) & ~own_occ;
                const int moves = std::min(14, count_bits(attacks & ~enemy_pawn_attacks));
                mobility.mg += sign * ROOK_MOBILITY_MG[moves];
                mobility.eg += sign * ROOK_MOBILITY_EG[moves];
                const U64 file_mask = FILE_A << file;
                if (!(own_pawns & file_mask)) {
                    mobility.mg += sign * (enemy_pawns & file_mask ? 10 : 18);
                    mobility.eg += sign * (enemy_pawns & file_mask ? 5 : 10);
                }
                if (relative_rank == 6) {
                    mobility.mg += sign * 15;
                    mobility.eg += sign * 25;
                }
            }
            pieces = board.get_pieces(us, QUEEN);
            while (pieces) {
                const int square = pop_lsb(pieces);
                const U64 attacks = get_queen_attacks(square, board.get_occupancy(BOTH)) & ~own_occ;
                const int moves = std::min(27, count_bits(attacks & ~enemy_pawn_attacks));
                mobility.mg += sign * QUEEN_MOBILITY_MG[moves];
                mobility.eg += sign * QUEEN_MOBILITY_EG[moves];
            }

            U64 rook_batteries = board.get_pieces(us, ROOK);
            while (rook_batteries) {
                const int square = pop_lsb(rook_batteries);
                if (get_rook_attacks(square, occupied) & board.get_pieces(us, QUEEN)) {
                    mobility.mg += sign * 8;
                    mobility.eg += sign * 5;
                }
                const U64 attacked_pawns = get_rook_attacks(square, occupied) & enemy_pawns;
                if (attacked_pawns) {
                    mobility.mg += sign * 3 * count_bits(attacked_pawns);
                    mobility.eg += sign * 5 * count_bits(attacked_pawns);
                }
            }

            const U64 center = 0x00003C3C3C3C0000ULL;
            const int space = count_bits((pawn_attack_map | all_attacks[color]) & center & ~own_occ);
            mobility.mg += sign * 2 * space;
            mobility.eg += sign * space;

            // T5: king shelter, king-ring pressure, and endgame king activity.
            const U64 king = board.get_pieces(us, KING);
            if (king) {
                const int square = get_lsb(king);
                const int file = square & 7;
                const int rank = square >> 3;
                const int shield_rank = rank + (color == WHITE ? 1 : -1);
                if (shield_rank >= 0 && shield_rank < 8) {
                    for (int df = -1; df <= 1; ++df) {
                        const int shield_file = file + df;
                        if (shield_file >= 0 && shield_file < 8
                            && (own_pawns & (1ULL << (shield_rank * 8 + shield_file))))
                            king_safety.mg += sign * 10;
                    }
                }
                const int ring_pressure = count_bits(get_king_attacks(square) & all_attacks[color ^ 1]);
                const U64 king_zone = get_king_attacks(square) | king;
                int attack_units = ring_pressure * 2;
                U64 attacker_scan = board.get_pieces(them, KNIGHT);
                while (attacker_scan)
                    if (get_knight_attacks(pop_lsb(attacker_scan)) & king_zone) attack_units += 2;
                attacker_scan = board.get_pieces(them, BISHOP);
                while (attacker_scan)
                    if (get_bishop_attacks(pop_lsb(attacker_scan), occupied) & king_zone) attack_units += 2;
                attacker_scan = board.get_pieces(them, ROOK);
                while (attacker_scan)
                    if (get_rook_attacks(pop_lsb(attacker_scan), occupied) & king_zone) attack_units += 3;
                attacker_scan = board.get_pieces(them, QUEEN);
                while (attacker_scan)
                    if (get_queen_attacks(pop_lsb(attacker_scan), occupied) & king_zone) attack_units += 5;
                const int defenders = count_bits(all_attacks[color] & king_zone);
                const int danger = std::max(0, attack_units * attack_units - 3 * defenders);
                king_safety.mg -= sign * danger;
                king_safety.eg -= sign * (ring_pressure * 3);

                const int virtual_mobility = count_bits(
                    get_queen_attacks(square, occupied & ~king) & ~own_occ);
                king_safety.mg -= sign * std::max(0, virtual_mobility - 3) * 2;

                for (int shield_file = std::max(0, file - 1);
                     shield_file <= std::min(7, file + 1); ++shield_file) {
                    const U64 file_mask = FILE_A << shield_file;
                    if (!(own_pawns & file_mask)) {
                        king_safety.mg -= sign * 12;
                        king_safety.eg -= sign * 3;
                        if (!(enemy_pawns & file_mask)) king_safety.mg -= sign * 8;
                    }
                }
                const int center_distance = std::abs(file * 2 - 7) + std::abs(rank * 2 - 7);
                endgame.eg -= sign * 2 * center_distance;
            }

            const int own_bishops = count_bits(board.get_pieces(us, BISHOP));
            const int own_knights = count_bits(board.get_pieces(us, KNIGHT));
            const int enemy_nonking_count = count_bits(board.get_occupancy(them) & ~board.get_pieces(them, KING));
            const U64 all_rooks_queens = board.get_pieces(WHITE, ROOK) | board.get_pieces(BLACK, ROOK)
                                       | board.get_pieces(WHITE, QUEEN) | board.get_pieces(BLACK, QUEEN);
            if (own_bishops == 1 && own_knights == 1 && enemy_nonking_count == 0
                && !own_pawns && !all_rooks_queens && enemy_king_bb && king) {
                const int bishop_square = get_lsb(board.get_pieces(us, BISHOP));
                const int bishop_color = ((bishop_square & 7) + (bishop_square >> 3)) & 1;
                const int enemy_king_square = get_lsb(enemy_king_bb);
                const int corners[2][2] = {{SQ_A1, SQ_H8}, {SQ_H1, SQ_A8}};
                auto distance = [](int lhs, int rhs) {
                    return std::max(std::abs((lhs & 7) - (rhs & 7)),
                                    std::abs((lhs >> 3) - (rhs >> 3)));
                };
                const int corner_distance = std::min(distance(enemy_king_square, corners[bishop_color][0]),
                                                     distance(enemy_king_square, corners[bishop_color][1]));
                const int king_distance = distance(get_lsb(king), enemy_king_square);
                endgame.eg += sign * (120 - 16 * corner_distance - 4 * king_distance);
            }
        }

        phase = std::min(phase, MAX_PHASE);
        const int tempo_sign = board.get_side_to_move() == WHITE ? 1 : -1;
        tempo.mg += tempo_sign * 12;
        tempo.eg += tempo_sign * 6;
        const Score categories[HCE_FEATURE_COUNT] = {
            material, positional, pawns, imbalance, passers,
            threats, mobility, king_safety, endgame, tempo
        };
        int64_t weighted_mg = 0;
        int64_t weighted_eg = 0;
        for (int feature = 0; feature < HCE_FEATURE_COUNT; ++feature) {
            weighted_mg += static_cast<int64_t>(categories[feature].mg) * weights.feature[feature].mg;
            weighted_eg += static_cast<int64_t>(categories[feature].eg) * weights.feature[feature].eg;
        }
        const int mg = static_cast<int>(weighted_mg / 1024);
        const int eg = static_cast<int>(weighted_eg / 1024);
        int scale = 128;
        bool exact_draw = false;

        const U64 all_pawns = board.get_pieces(WHITE, PAWN) | board.get_pieces(BLACK, PAWN);
        const U64 heavy = board.get_pieces(WHITE, ROOK) | board.get_pieces(BLACK, ROOK)
                        | board.get_pieces(WHITE, QUEEN) | board.get_pieces(BLACK, QUEEN);
        const int minors = count_bits(board.get_pieces(WHITE, KNIGHT) | board.get_pieces(BLACK, KNIGHT)
                                    | board.get_pieces(WHITE, BISHOP) | board.get_pieces(BLACK, BISHOP));
        if (!all_pawns && !heavy && minors <= 1) {
            exact_draw = true;
        } else if (!heavy
                   && !board.get_pieces(WHITE, KNIGHT) && !board.get_pieces(BLACK, KNIGHT)
                   && count_bits(board.get_pieces(WHITE, BISHOP)) == 1
                   && count_bits(board.get_pieces(BLACK, BISHOP)) == 1) {
            const int wb = get_lsb(board.get_pieces(WHITE, BISHOP));
            const int bb = get_lsb(board.get_pieces(BLACK, BISHOP));
            const int wb_color = ((wb & 7) + (wb >> 3)) & 1;
            const int bb_color = ((bb & 7) + (bb >> 3)) & 1;
            if (wb_color != bb_color) {
                const int pawn_count = count_bits(all_pawns);
                scale = pawn_count <= 4 ? 72 : 96;
            }
        }

        if (!all_pawns && !exact_draw) {
            const int material_edge = std::abs(material.eg);
            if (material_edge <= 500) scale = std::min(scale, 96);
        }

        const U64 all_rooks = board.get_pieces(WHITE, ROOK) | board.get_pieces(BLACK, ROOK);
        const U64 all_queens = board.get_pieces(WHITE, QUEEN) | board.get_pieces(BLACK, QUEEN);
        if (count_bits(all_pawns) == 1 && count_bits(all_rooks) == 2 && !all_queens && minors == 0
            && count_bits(board.get_pieces(WHITE, ROOK)) == 1
            && count_bits(board.get_pieces(BLACK, ROOK)) == 1) {
            const Color pawn_side = board.get_pieces(WHITE, PAWN) ? WHITE : BLACK;
            const Color defender = static_cast<Color>(pawn_side ^ 1);
            const int pawn_square = get_lsb(all_pawns);
            const U64 defender_king = board.get_pieces(defender, KING);
            if (defender_king) {
                const int king_square = get_lsb(defender_king);
                const bool ahead = pawn_side == WHITE
                    ? (king_square >> 3) > (pawn_square >> 3)
                    : (king_square >> 3) < (pawn_square >> 3);
                if ((king_square & 7) == (pawn_square & 7) && ahead)
                    scale = std::min(scale, 80);
            }
        }

        const int rule50_remaining = std::clamp(100 - board.get_halfmove_clock(), 0, 100);
        scale = std::min(scale, rule50_remaining * 128 / 100);

        int white_score = exact_draw ? 0 : ((mg * phase + eg * (MAX_PHASE - phase)) / MAX_PHASE) * scale / 128;
        const int stm_score = board.get_side_to_move() == WHITE ? white_score : -white_score;

        if (trace) {
            trace->material_mg = material.mg; trace->material_eg = material.eg;
            trace->psqt_mg = positional.mg; trace->psqt_eg = positional.eg;
            trace->pawns_mg = pawns.mg; trace->pawns_eg = pawns.eg;
            trace->imbalance_mg = imbalance.mg; trace->imbalance_eg = imbalance.eg;
            trace->passers_mg = passers.mg; trace->passers_eg = passers.eg;
            trace->threats_mg = threats.mg; trace->threats_eg = threats.eg;
            trace->mobility_mg = mobility.mg; trace->mobility_eg = mobility.eg;
            trace->king_safety_mg = king_safety.mg; trace->king_safety_eg = king_safety.eg;
            trace->endgame_mg = endgame.mg; trace->endgame_eg = endgame.eg;
            trace->tempo_mg = tempo.mg; trace->tempo_eg = tempo.eg;
            trace->phase = phase; trace->scale = scale; trace->exact_draw = exact_draw;
            trace->white_score = white_score;
            trace->side_to_move_score = stm_score;
        }
        return stm_score;
    }

    // Master evaluation function
    int evaluate(const Board& board) {
        // Return the NNUE evaluation directly
        return g_nnue.evaluate_nnue(board);
    }
}

