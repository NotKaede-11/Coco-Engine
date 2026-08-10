#ifndef EVALUATE_H
#define EVALUATE_H

#include "board.h"
#include <array>
#include <cstdint>

namespace Evaluation {
    struct Score {
        int16_t mg = 0;
        int16_t eg = 0;

        constexpr Score() = default;
        constexpr Score(int mg_value, int eg_value)
            : mg(static_cast<int16_t>(mg_value)), eg(static_cast<int16_t>(eg_value)) {}
        constexpr Score operator+(Score rhs) const { return {mg + rhs.mg, eg + rhs.eg}; }
        constexpr Score operator-(Score rhs) const { return {mg - rhs.mg, eg - rhs.eg}; }
        constexpr Score operator*(int factor) const { return {mg * factor, eg * factor}; }
        constexpr Score& operator+=(Score rhs) {
            mg = static_cast<int16_t>(mg + rhs.mg);
            eg = static_cast<int16_t>(eg + rhs.eg);
            return *this;
        }
        constexpr Score& operator-=(Score rhs) {
            mg = static_cast<int16_t>(mg - rhs.mg);
            eg = static_cast<int16_t>(eg - rhs.eg);
            return *this;
        }
    };
    static_assert(sizeof(Score) == 4, "HCE Score must remain a packed MG/EG pair");

    enum HceFeature : int {
        HCE_MATERIAL,
        HCE_PSQT,
        HCE_PAWNS,
        HCE_IMBALANCE,
        HCE_PASSERS,
        HCE_THREATS,
        HCE_MOBILITY,
        HCE_KING_SAFETY,
        HCE_ENDGAME,
        HCE_TEMPO,
        HCE_FEATURE_COUNT
    };

    inline constexpr const char* HCE_FEATURE_NAMES[HCE_FEATURE_COUNT] = {
        "material", "psqt", "pawns", "imbalance", "passers",
        "threats", "mobility", "king_safety", "endgame", "tempo"
    };

    struct HceWeights {
        // Fixed-point category multipliers: 1024 means 1.0.
        std::array<Score, HCE_FEATURE_COUNT> feature;
    };

    const HceWeights& default_hce_weights();

    struct HceTrace {
        int material_mg = 0;
        int material_eg = 0;
        int psqt_mg = 0;
        int psqt_eg = 0;
        int pawns_mg = 0;
        int pawns_eg = 0;
        int imbalance_mg = 0;
        int imbalance_eg = 0;
        int passers_mg = 0;
        int passers_eg = 0;
        int threats_mg = 0;
        int threats_eg = 0;
        int mobility_mg = 0;
        int mobility_eg = 0;
        int king_safety_mg = 0;
        int king_safety_eg = 0;
        int endgame_mg = 0;
        int endgame_eg = 0;
        int tempo_mg = 0;
        int tempo_eg = 0;
        int phase = 0;
        int scale = 128;
        bool exact_draw = false;
        int white_score = 0;
        int side_to_move_score = 0;
    };

    // Piece value constants
    const int VAL_PAWN = 100;
    const int VAL_KNIGHT = 320;
    const int VAL_BISHOP = 330;
    const int VAL_ROOK = 500;
    const int VAL_QUEEN = 900;
    const int VAL_KING = 20000;

    // Master evaluation function returning score relative to the side to move
    int evaluate(const Board& board);

    // Disabled-by-default HCE foundation used by the tuner/parity tests. It
    // must not influence NNUE search until calibrated and separately gated.
    int evaluate_hce(const Board& board, HceTrace* trace = nullptr);
    int evaluate_hce(const Board& board, HceTrace* trace, const HceWeights& weights);
}

#endif // EVALUATE_H
