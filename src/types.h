#ifndef TYPES_H
#define TYPES_H

#include <cstdint>
#include <string>

// Compile-time option to change hidden layer size (e.g., 256, 512, 1024)
constexpr int L1_SIZE = 256;

// Maximum number of search threads for Lazy SMP
constexpr int MAX_THREADS = 256;

// 64-bit unsigned integer for bitboards
using U64 = uint64_t;

// Colors
enum Color {
    WHITE = 0,
    BLACK = 1,
    BOTH = 2,
    NO_COLOR = 3
};

// Piece Types
enum PieceType {
    PAWN = 0,
    KNIGHT = 1,
    BISHOP = 2,
    ROOK = 3,
    QUEEN = 4,
    KING = 5,
    NO_PIECE_TYPE = 6
};

// Pieces
enum Piece {
    W_PAWN = 0, W_KNIGHT = 1, W_BISHOP = 2, W_ROOK = 3, W_QUEEN = 4, W_KING = 5,
    B_PAWN = 6, B_KNIGHT = 7, B_BISHOP = 8, B_ROOK = 9, B_QUEEN = 10, B_KING = 11,
    NO_PIECE = 12
};

// Squares
enum Square {
    SQ_A1 = 0, SQ_B1 = 1, SQ_C1 = 2, SQ_D1 = 3, SQ_E1 = 4, SQ_F1 = 5, SQ_G1 = 6, SQ_H1 = 7,
    SQ_A2 = 8, SQ_B2 = 9, SQ_C2 = 10, SQ_D2 = 11, SQ_E2 = 12, SQ_F2 = 13, SQ_G2 = 14, SQ_H2 = 15,
    SQ_A3 = 16, SQ_B3 = 17, SQ_C3 = 18, SQ_D3 = 19, SQ_E3 = 20, SQ_F3 = 21, SQ_G3 = 22, SQ_H3 = 23,
    SQ_A4 = 24, SQ_B4 = 25, SQ_C4 = 26, SQ_D4 = 27, SQ_E4 = 28, SQ_F4 = 29, SQ_G4 = 30, SQ_H4 = 31,
    SQ_A5 = 32, SQ_B5 = 33, SQ_C5 = 34, SQ_D5 = 35, SQ_E5 = 36, SQ_F5 = 37, SQ_G5 = 38, SQ_H5 = 39,
    SQ_A6 = 40, SQ_B6 = 41, SQ_C6 = 42, SQ_D6 = 43, SQ_E6 = 44, SQ_F6 = 45, SQ_G6 = 46, SQ_H6 = 47,
    SQ_A7 = 48, SQ_B7 = 49, SQ_C7 = 50, SQ_D7 = 51, SQ_E7 = 52, SQ_F7 = 53, SQ_G7 = 54, SQ_H7 = 55,
    SQ_A8 = 56, SQ_B8 = 57, SQ_C8 = 58, SQ_D8 = 59, SQ_E8 = 60, SQ_F8 = 61, SQ_G8 = 62, SQ_H8 = 63,
    SQ_NONE = 64
};

// Castling rights (4-bit representation)
enum CastlingRights {
    NO_CASTLING = 0,
    WHITE_OO = 1,
    WHITE_OOO = 2,
    BLACK_OO = 4,
    BLACK_OOO = 8,
    ANY_WHITE_CASTLING = WHITE_OO | WHITE_OOO,
    ANY_BLACK_CASTLING = BLACK_OO | BLACK_OOO,
    ALL_CASTLING = WHITE_OO | WHITE_OOO | BLACK_OO | BLACK_OOO
};

// Move flags
enum MoveFlag {
    FLAG_QUIET = 0,
    FLAG_DOUBLE_PAWN = 1,
    FLAG_KING_CASTLE = 2,
    FLAG_QUEEN_CASTLE = 3,
    FLAG_CAPTURE = 4,
    FLAG_EP = 5,
    FLAG_PROMO_KNIGHT = 8,
    FLAG_PROMO_BISHOP = 9,
    FLAG_PROMO_ROOK = 10,
    FLAG_PROMO_QUEEN = 11,
    FLAG_PROMO_KNIGHT_CAP = 12,
    FLAG_PROMO_BISHOP_CAP = 13,
    FLAG_PROMO_ROOK_CAP = 14,
    FLAG_PROMO_QUEEN_CAP = 15
};

// Bitwise operations helpers
inline void set_bit(U64& bb, int square) { bb |= (1ULL << square); }
inline void clear_bit(U64& bb, int square) { bb &= ~(1ULL << square); }
inline int count_bits(U64 bb) { return __builtin_popcountll(bb); }
inline int get_lsb(U64 bb) { return __builtin_ctzll(bb); }

// Pop LSB and return its square index
inline int pop_lsb(U64& bb) {
    int index = get_lsb(bb);
    bb &= bb - 1;
    return index;
}

// 16-bit Move structure
// Bits 0-5: Source square (0 to 63)
// Bits 6-11: Destination square (0 to 63)
// Bits 12-15: Move flags
struct Move {
    uint16_t value;

    Move() : value(0) {}
    explicit Move(uint16_t val) : value(val) {}
    Move(int from, int to, int flags = 0) {
        value = (from & 0x3F) | ((to & 0x3F) << 6) | ((flags & 0x0F) << 12);
    }

    int from() const { return value & 0x3F; }
    int to() const { return (value >> 6) & 0x3F; }
    int flag() const { return (value >> 12) & 0x0F; }

    bool is_capture() const { return (flag() & FLAG_CAPTURE) != 0; }
    bool is_promotion() const { return (flag() & 8) != 0; }
    bool is_en_passant() const { return flag() == FLAG_EP; }
    bool is_castling() const { return flag() == FLAG_KING_CASTLE || flag() == FLAG_QUEEN_CASTLE; }

    int promotion_piece_type() const {
        if (!is_promotion()) return NO_PIECE_TYPE;
        // flag & 3 maps: 0->KNIGHT, 1->BISHOP, 2->ROOK, 3->QUEEN
        return (flag() & 3) + 1; // KNIGHT is 1, BISHOP is 2, ROOK is 3, QUEEN is 4
    }

    bool is_none() const { return value == 0; }

    bool operator==(const Move& other) const { return value == other.value; }
    bool operator!=(const Move& other) const { return value != other.value; }
};

// Conversions for square to coordinates
inline std::string square_to_str(int square) {
    if (square < 0 || square >= 64) return "-";
    char file = 'a' + (square % 8);
    char rank = '1' + (square / 8);
    return std::string{file, rank};
}

#endif // TYPES_H
