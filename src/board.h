#ifndef BOARD_H
#define BOARD_H

#include "types.h"
#include <string>
#include <vector>

struct Accumulator {
    int16_t v[2][256];
};

// Structure to store information needed to restore board state in unmake_move
struct StateInfo {
    int castling_rights;
    int en_passant_square;
    int halfmove_clock;
    Piece captured_piece;
    U64 hash_key;
    Accumulator accumulator;
};

struct LegalityMasks {
    U64 checkers;
    U64 pinned;
    U64 pin_rays[64];
};

class Board {
public:
    Board();

    // Reset board to empty
    void clear();

    // Parse FEN string and setup board
    void parse_fen(const std::string& fen);

    // Convert current board state to FEN string
    std::string get_fen() const;

    // Make a move. Returns false if the move was illegal (exposed king to check),
    // in which case the board is reverted to the original state.
    bool make_move(Move move, bool checked = false);

    // Unmake a move, reverting the board state
    void unmake_move(Move move);

    // Null moves
    void make_null_move();
    void unmake_null_move();


    // Check if a square is attacked by any piece of the specified attacker color
    bool is_square_attacked(int square, int attacker_color) const;

    // Legality checks
    LegalityMasks get_legality_masks() const;
    bool is_move_legal(Move move, const LegalityMasks& masks) const;

    // Static Exchange Evaluation (SEE)
    int see(Move move) const;
    U64 get_all_attackers(int square, U64 occ) const;

    // Getters
    U64 get_pieces(int color, int piece_type) const { return pieces[color][piece_type]; }
    U64 get_occupancy(int color) const { return occupancies[color]; }
    Color get_side_to_move() const { return side_to_move; }
    int get_castling_rights() const { return castling_rights; }
    int get_en_passant_square() const { return en_passant_square; }
    int get_halfmove_clock() const { return halfmove_clock; }
    U64 get_hash_key() const { return hash_key; }
    Piece get_piece_at(int square) const { return board_array[square]; }
    const Accumulator& get_accumulator() const { return accumulator; }
    bool is_repetition() const;

    // NNUE incremental update helpers
    void nnue_activate_piece(int color, int pt, int sq);
    void nnue_deactivate_piece(int color, int pt, int sq);

    // Print board to stdout (for debugging)
    void print() const;

    // Public Zobrist hash keys for incremental updates
    static U64 zobrist_pieces[2][6][64];
    static U64 zobrist_castling[16];
    static U64 zobrist_ep[8]; // Indexed by file (0-7)
    static U64 zobrist_side;

    // Initialize Zobrist keys (called once at startup)
    static void init_zobrist();

private:
    // Core bitboards
    U64 pieces[2][6];         // [color][piece_type]
    U64 occupancies[3];       // [WHITE], [BLACK], [BOTH]

    // Piece list array for O(1) piece lookup on a square
    Piece board_array[64];

    // State variables
    Color side_to_move;
    int castling_rights;
    int en_passant_square;
    int halfmove_clock;
    int fullmove_number;

    // Current Zobrist hash key
    U64 hash_key;

    // Current accumulator state
    Accumulator accumulator;

    // History stack for unmake_move
    StateInfo history[1024];
    int history_ply;

    // Helper to calculate full Zobrist hash from scratch
    U64 calculate_hash() const;

    // Helper to update occupancies based on piece bitboards
    void update_occupancies();
};

#endif // BOARD_H
