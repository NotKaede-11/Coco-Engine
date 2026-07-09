#include "board.h"
#include "movegen.h"
#include "nnue.h"
#include <iostream>
#include <sstream>
#include <cctype>

// Define static members of Board
U64 Board::zobrist_pieces[2][6][64];
U64 Board::zobrist_castling[16];
U64 Board::zobrist_ep[8];
U64 Board::zobrist_side;

// Castling rights update mask for fast updates in make_move
const int castling_rights_mask[64] = {
    13, 15, 15, 15, 12, 15, 15, 14, // Rank 1 (A1=13, E1=12, H1=14)
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    7,  15, 15, 15,  3, 15, 15, 11  // Rank 8 (A8=7, E8=3, H8=11)
};

Board::Board() {
    clear();
}

void Board::clear() {
    for (int c = 0; c < 2; c++) {
        for (int pt = 0; pt < 6; pt++) {
            pieces[c][pt] = 0ULL;
        }
    }
    for (int i = 0; i < 3; i++) {
        occupancies[i] = 0ULL;
    }
    for (int sq = 0; sq < 64; sq++) {
        board_array[sq] = NO_PIECE;
    }
    side_to_move = WHITE;
    castling_rights = NO_CASTLING;
    en_passant_square = SQ_NONE;
    halfmove_clock = 0;
    fullmove_number = 1;
    hash_key = 0ULL;
    history_ply = 0;

    // Initialize NNUE accumulator
    g_nnue.init_accumulator(*this, accumulator);
}

void Board::init_zobrist() {
    U64 state = 1804289383ULL;
    auto next_random = [&]() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 2685821657736338717ULL;
    };

    for (int c = 0; c < 2; c++) {
        for (int pt = 0; pt < 6; pt++) {
            for (int sq = 0; sq < 64; sq++) {
                zobrist_pieces[c][pt][sq] = next_random();
            }
        }
    }
    for (int i = 0; i < 16; i++) {
        zobrist_castling[i] = next_random();
    }
    for (int i = 0; i < 8; i++) {
        zobrist_ep[i] = next_random();
    }
    zobrist_side = next_random();
}

U64 Board::calculate_hash() const {
    U64 k = 0;
    for (int sq = 0; sq < 64; sq++) {
        Piece p = board_array[sq];
        if (p != NO_PIECE) {
            k ^= zobrist_pieces[p / 6][p % 6][sq];
        }
    }
    k ^= zobrist_castling[castling_rights];
    if (en_passant_square != SQ_NONE) {
        k ^= zobrist_ep[en_passant_square % 8];
    }
    if (side_to_move == BLACK) {
        k ^= zobrist_side;
    }
    return k;
}

void Board::update_occupancies() {
    occupancies[WHITE] = 0;
    for (int pt = 0; pt < 6; pt++) {
        occupancies[WHITE] |= pieces[WHITE][pt];
    }
    occupancies[BLACK] = 0;
    for (int pt = 0; pt < 6; pt++) {
        occupancies[BLACK] |= pieces[BLACK][pt];
    }
    occupancies[BOTH] = occupancies[WHITE] | occupancies[BLACK];
}

void Board::parse_fen(const std::string& fen) {
    clear();
    int i = 0;
    int n = fen.length();

    // 1. Piece placement
    int rank = 7;
    int file = 0;
    while (i < n && fen[i] != ' ') {
        char c = fen[i];
        if (c == '/') {
            rank--;
            file = 0;
        } else if (c >= '1' && c <= '8') {
            file += (c - '0');
        } else {
            Color color = isupper(c) ? WHITE : BLACK;
            char lower = tolower(c);
            PieceType pt;
            if (lower == 'p') pt = PAWN;
            else if (lower == 'n') pt = KNIGHT;
            else if (lower == 'b') pt = BISHOP;
            else if (lower == 'r') pt = ROOK;
            else if (lower == 'q') pt = QUEEN;
            else if (lower == 'k') pt = KING;
            else pt = NO_PIECE_TYPE;

            if (pt != NO_PIECE_TYPE) {
                int sq = rank * 8 + file;
                set_bit(pieces[color][pt], sq);
                board_array[sq] = (Piece)(color * 6 + pt);
                file++;
            }
        }
        i++;
    }

    // Skip spaces
    while (i < n && fen[i] == ' ') i++;

    // 2. Active color
    if (i < n) {
        side_to_move = (fen[i] == 'w') ? WHITE : BLACK;
        i++;
    }

    // Skip spaces
    while (i < n && fen[i] == ' ') i++;

    // 3. Castling rights
    castling_rights = NO_CASTLING;
    if (i < n && fen[i] == '-') {
        i++;
    } else {
        while (i < n && fen[i] != ' ') {
            char c = fen[i];
            if (c == 'K') castling_rights |= WHITE_OO;
            else if (c == 'Q') castling_rights |= WHITE_OOO;
            else if (c == 'k') castling_rights |= BLACK_OO;
            else if (c == 'q') castling_rights |= BLACK_OOO;
            i++;
        }
    }

    // Skip spaces
    while (i < n && fen[i] == ' ') i++;

    // 4. En passant target square
    en_passant_square = SQ_NONE;
    if (i < n && fen[i] == '-') {
        i++;
    } else if (i + 1 < n && fen[i] >= 'a' && fen[i] <= 'h' && fen[i+1] >= '1' && fen[i+1] <= '8') {
        int f = fen[i] - 'a';
        int r = fen[i+1] - '1';
        en_passant_square = (Square)(r * 8 + f);
        i += 2;
    }

    // Skip spaces
    while (i < n && fen[i] == ' ') i++;

    // 5. Halfmove clock
    halfmove_clock = 0;
    if (i < n && isdigit(fen[i])) {
        std::string s = "";
        while (i < n && isdigit(fen[i])) {
            s += fen[i];
            i++;
        }
        halfmove_clock = std::stoi(s);
    }

    // Skip spaces
    while (i < n && fen[i] == ' ') i++;

    // 6. Fullmove number
    fullmove_number = 1;
    if (i < n && isdigit(fen[i])) {
        std::string s = "";
        while (i < n && isdigit(fen[i])) {
            s += fen[i];
            i++;
        }
        fullmove_number = std::stoi(s);
    }

    update_occupancies();
    hash_key = calculate_hash();
    history_ply = 0;

    // Initialize NNUE accumulator
    g_nnue.init_accumulator(*this, accumulator);
}

std::string Board::get_fen() const {
    std::string fen = "";
    for (int r = 7; r >= 0; r--) {
        int empty_count = 0;
        for (int f = 0; f < 8; f++) {
            int sq = r * 8 + f;
            Piece p = board_array[sq];
            if (p == NO_PIECE) {
                empty_count++;
            } else {
                if (empty_count > 0) {
                    fen += std::to_string(empty_count);
                    empty_count = 0;
                }
                char c;
                PieceType pt = (PieceType)(p % 6);
                Color color = (Color)(p / 6);
                if (pt == PAWN) c = 'p';
                else if (pt == KNIGHT) c = 'n';
                else if (pt == BISHOP) c = 'b';
                else if (pt == ROOK) c = 'r';
                else if (pt == QUEEN) c = 'q';
                else c = 'k';

                if (color == WHITE) c = toupper(c);
                fen += c;
            }
        }
        if (empty_count > 0) {
            fen += std::to_string(empty_count);
        }
        if (r > 0) fen += '/';
    }

    fen += " ";
    fen += (side_to_move == WHITE) ? "w" : "b";

    fen += " ";
    std::string castling = "";
    if (castling_rights & WHITE_OO) castling += "K";
    if (castling_rights & WHITE_OOO) castling += "Q";
    if (castling_rights & BLACK_OO) castling += "k";
    if (castling_rights & BLACK_OOO) castling += "q";
    if (castling.empty()) castling = "-";
    fen += castling;

    fen += " ";
    fen += square_to_str(en_passant_square);

    fen += " ";
    fen += std::to_string(halfmove_clock);

    fen += " ";
    fen += std::to_string(fullmove_number);

    return fen;
}

bool Board::is_square_attacked(int square, int attacker_color) const {
    // 1. Attacked by pawns
    int defender_color = attacker_color ^ 1;
    U64 pawn_attackers = get_pawn_attacks(defender_color, square) & pieces[attacker_color][PAWN];
    if (pawn_attackers) return true;

    // 2. Attacked by knights
    U64 knight_attackers = get_knight_attacks(square) & pieces[attacker_color][KNIGHT];
    if (knight_attackers) return true;

    // 3. Attacked by bishops/queens (diagonal sliding)
    U64 bishop_queen_attackers = get_bishop_attacks(square, occupancies[BOTH]) & (pieces[attacker_color][BISHOP] | pieces[attacker_color][QUEEN]);
    if (bishop_queen_attackers) return true;

    // 4. Attacked by rooks/queens (orthogonal sliding)
    U64 rook_queen_attackers = get_rook_attacks(square, occupancies[BOTH]) & (pieces[attacker_color][ROOK] | pieces[attacker_color][QUEEN]);
    if (rook_queen_attackers) return true;

    // 5. Attacked by king
    U64 king_attackers = get_king_attacks(square) & pieces[attacker_color][KING];
    if (king_attackers) return true;

    return false;
}

bool Board::make_move(Move move, bool checked) {
    int from = move.from();
    int to = move.to();
    int flag = move.flag();

    Color us = side_to_move;
    Color them = (Color)(us ^ 1);
    Piece moving_piece = board_array[from];
    PieceType pt = (PieceType)(moving_piece % 6);

    side_to_move = them;

    if (us == BLACK) {
        fullmove_number++;
    }

    // Save state before making the move
    history[history_ply] = {
        castling_rights,
        en_passant_square,
        halfmove_clock,
        NO_PIECE,
        hash_key,
        accumulator
    };
    history_ply++;

    // XOR out old side, castling, and EP
    hash_key ^= zobrist_side;
    hash_key ^= zobrist_castling[castling_rights];
    if (en_passant_square != SQ_NONE) {
        hash_key ^= zobrist_ep[en_passant_square % 8];
    }

    // Default next EP square is none
    int new_en_passant_square = SQ_NONE;

    // Handle captures
    if (move.is_capture()) {
        if (flag == FLAG_EP) {
            int ep_pawn_sq = (us == WHITE) ? (to - 8) : (to + 8);
            Piece captured = board_array[ep_pawn_sq];
            history[history_ply - 1].captured_piece = captured;
            
            // NNUE INCREMENTAL ACCUMULATOR UPDATE HOOK
            nnue_deactivate_piece(them, PAWN, ep_pawn_sq);

            clear_bit(pieces[them][PAWN], ep_pawn_sq);
            board_array[ep_pawn_sq] = NO_PIECE;
            hash_key ^= zobrist_pieces[them][PAWN][ep_pawn_sq];
        } else {
            Piece captured = board_array[to];
            history[history_ply - 1].captured_piece = captured;
            PieceType cap_type = (PieceType)(captured % 6);

            // NNUE INCREMENTAL ACCUMULATOR UPDATE HOOK
            nnue_deactivate_piece(them, cap_type, to);

            clear_bit(pieces[them][cap_type], to);
            hash_key ^= zobrist_pieces[them][cap_type][to];
        }
        halfmove_clock = 0; // Reset on capture
    } else {
        halfmove_clock++;
    }

    // NNUE INCREMENTAL ACCUMULATOR UPDATE HOOK
    nnue_deactivate_piece(us, pt, from);
    nnue_activate_piece(us, pt, to);

    clear_bit(pieces[us][pt], from);
    set_bit(pieces[us][pt], to);
    board_array[from] = NO_PIECE;
    board_array[to] = moving_piece;

    hash_key ^= zobrist_pieces[us][pt][from];
    hash_key ^= zobrist_pieces[us][pt][to];

    // Handle promotions
    if (move.is_promotion()) {
        PieceType promo_pt = (PieceType)move.promotion_piece_type();
        
        // NNUE INCREMENTAL ACCUMULATOR UPDATE HOOK
        nnue_deactivate_piece(us, PAWN, to);
        nnue_activate_piece(us, promo_pt, to);

        clear_bit(pieces[us][PAWN], to);
        set_bit(pieces[us][promo_pt], to);
        board_array[to] = (Piece)(us * 6 + promo_pt);

        hash_key ^= zobrist_pieces[us][PAWN][to];
        hash_key ^= zobrist_pieces[us][promo_pt][to];
    }

    // Handle double pawn pushes (setup en passant)
    if (flag == FLAG_DOUBLE_PAWN) {
        new_en_passant_square = (us == WHITE) ? (from + 8) : (from - 8);
    }

    // Handle castling
    if (flag == FLAG_KING_CASTLE) {
        int r_from = (us == WHITE) ? SQ_H1 : SQ_H8;
        int r_to = (us == WHITE) ? SQ_F1 : SQ_F8;
        Piece rook = (us == WHITE) ? W_ROOK : B_ROOK;

        // NNUE INCREMENTAL ACCUMULATOR UPDATE HOOK
        nnue_deactivate_piece(us, ROOK, r_from);
        nnue_activate_piece(us, ROOK, r_to);

        clear_bit(pieces[us][ROOK], r_from);
        set_bit(pieces[us][ROOK], r_to);
        board_array[r_from] = NO_PIECE;
        board_array[r_to] = rook;

        hash_key ^= zobrist_pieces[us][ROOK][r_from];
        hash_key ^= zobrist_pieces[us][ROOK][r_to];
    } else if (flag == FLAG_QUEEN_CASTLE) {
        int r_from = (us == WHITE) ? SQ_A1 : SQ_A8;
        int r_to = (us == WHITE) ? SQ_D1 : SQ_D8;
        Piece rook = (us == WHITE) ? W_ROOK : B_ROOK;

        // NNUE INCREMENTAL ACCUMULATOR UPDATE HOOK
        nnue_deactivate_piece(us, ROOK, r_from);
        nnue_activate_piece(us, ROOK, r_to);

        clear_bit(pieces[us][ROOK], r_from);
        set_bit(pieces[us][ROOK], r_to);
        board_array[r_from] = NO_PIECE;
        board_array[r_to] = rook;

        hash_key ^= zobrist_pieces[us][ROOK][r_from];
        hash_key ^= zobrist_pieces[us][ROOK][r_to];
    }

    // Update occupancies
    update_occupancies();

    // Verify move legality (cannot expose king to check)
    if (!checked) {
        int king_sq = get_lsb(pieces[us][KING]);
        if (is_square_attacked(king_sq, them)) {
            unmake_move(move);
            return false;
        }
    }

    // Finalize state updates
    int new_castling_rights = castling_rights & castling_rights_mask[from] & castling_rights_mask[to];
    hash_key ^= zobrist_castling[new_castling_rights];
    castling_rights = new_castling_rights;

    en_passant_square = new_en_passant_square;
    if (en_passant_square != SQ_NONE) {
        hash_key ^= zobrist_ep[en_passant_square % 8];
    }

    return true;
}

void Board::unmake_move(Move move) {
    // Pop state info
    history_ply--;
    StateInfo state = history[history_ply];

    int from = move.from();
    int to = move.to();
    int flag = move.flag();

    Color us = side_to_move; // us is the color to move NOW (which is them of the move)
    Color player = (Color)(us ^ 1);
    Piece moving_piece = board_array[to];

    // Restore side to move
    side_to_move = player;

    // Handle promotions: revert piece back to pawn before moving back
    if (move.is_promotion()) {
        PieceType promo_pt = (PieceType)move.promotion_piece_type();
        
        // NNUE INCREMENTAL ACCUMULATOR UPDATE HOOK
        clear_bit(pieces[player][promo_pt], to);
        set_bit(pieces[player][PAWN], to);
        board_array[to] = (Piece)(player * 6 + PAWN);
        moving_piece = board_array[to];
    }

    PieceType pt = (PieceType)(moving_piece % 6);

    // NNUE INCREMENTAL ACCUMULATOR UPDATE HOOK
    // Move the piece from to -> from
    clear_bit(pieces[player][pt], to);
    set_bit(pieces[player][pt], from);
    board_array[from] = moving_piece;
    board_array[to] = NO_PIECE;

    // Handle captured pieces
    if (move.is_capture()) {
        Piece captured = state.captured_piece;
        PieceType cap_type = (PieceType)(captured % 6);
        
        if (flag == FLAG_EP) {
            int ep_pawn_sq = (player == WHITE) ? (to - 8) : (to + 8);
            
            // NNUE INCREMENTAL ACCUMULATOR UPDATE HOOK
            set_bit(pieces[us][PAWN], ep_pawn_sq);
            board_array[ep_pawn_sq] = captured;
        } else {
            // NNUE INCREMENTAL ACCUMULATOR UPDATE HOOK
            set_bit(pieces[us][cap_type], to);
            board_array[to] = captured;
        }
    }

    // Handle castling rook moves back
    if (flag == FLAG_KING_CASTLE) {
        int r_from = (player == WHITE) ? SQ_H1 : SQ_H8;
        int r_to = (player == WHITE) ? SQ_F1 : SQ_F8;
        Piece rook = (player == WHITE) ? W_ROOK : B_ROOK;

        // NNUE INCREMENTAL ACCUMULATOR UPDATE HOOK
        clear_bit(pieces[player][ROOK], r_to);
        set_bit(pieces[player][ROOK], r_from);
        board_array[r_to] = NO_PIECE;
        board_array[r_from] = rook;
    } else if (flag == FLAG_QUEEN_CASTLE) {
        int r_from = (player == WHITE) ? SQ_A1 : SQ_A8;
        int r_to = (player == WHITE) ? SQ_D1 : SQ_D8;
        Piece rook = (player == WHITE) ? W_ROOK : B_ROOK;

        // NNUE INCREMENTAL ACCUMULATOR UPDATE HOOK
        clear_bit(pieces[player][ROOK], r_to);
        set_bit(pieces[player][ROOK], r_from);
        board_array[r_to] = NO_PIECE;
        board_array[r_from] = rook;
    }

    // Restore state variables
    castling_rights = state.castling_rights;
    en_passant_square = state.en_passant_square;
    halfmove_clock = state.halfmove_clock;
    hash_key = state.hash_key;
    accumulator = state.accumulator; // Restore accumulator from history stack

    if (player == BLACK) {
        fullmove_number--;
    }

    update_occupancies();
}

void Board::print() const {
    std::cout << "\n  +---+---+---+---+---+---+---+---+\n";
    for (int r = 7; r >= 0; r--) {
        std::cout << (r + 1) << " |";
        for (int f = 0; f < 8; f++) {
            int sq = r * 8 + f;
            Piece p = board_array[sq];
            char c = ' ';
            if (p != NO_PIECE) {
                PieceType pt = (PieceType)(p % 6);
                Color col = (Color)(p / 6);
                if (pt == PAWN) c = 'P';
                else if (pt == KNIGHT) c = 'N';
                else if (pt == BISHOP) c = 'B';
                else if (pt == ROOK) c = 'R';
                else if (pt == QUEEN) c = 'Q';
                else if (pt == KING) c = 'K';
                if (col == BLACK) c = tolower(c);
            }
            std::cout << " " << c << " |";
        }
        std::cout << "\n  +---+---+---+---+---+---+---+---+\n";
    }
    std::cout << "    a   b   c   d   e   f   g   h\n\n";
    std::cout << "Fen: " << get_fen() << "\n";
    std::cout << "Side to move: " << (side_to_move == WHITE ? "White" : "Black") << "\n";
    std::cout << "Castling Rights: "
              << ((castling_rights & WHITE_OO) ? "K" : "")
              << ((castling_rights & WHITE_OOO) ? "Q" : "")
              << ((castling_rights & BLACK_OO) ? "k" : "")
              << ((castling_rights & BLACK_OOO) ? "q" : "")
              << "\n";
    std::cout << "En Passant Square: " << square_to_str(en_passant_square) << "\n";
    std::cout << "Halfmove Clock: " << halfmove_clock << "\n";
    std::cout << "Fullmove Number: " << fullmove_number << "\n";
    std::cout << "Zobrist Hash: 0x" << std::hex << hash_key << std::dec << "\n\n";
}

void Board::nnue_activate_piece(int color, int pt, int sq) {
    int idx_w = get_feature_index_white(color, pt, sq);
    int idx_b = get_feature_index_black(color, pt, sq);
    
    g_nnue.accumulator_activate(accumulator, WHITE, idx_w);
    g_nnue.accumulator_activate(accumulator, BLACK, idx_b);
}

void Board::nnue_deactivate_piece(int color, int pt, int sq) {
    int idx_w = get_feature_index_white(color, pt, sq);
    int idx_b = get_feature_index_black(color, pt, sq);
    
    g_nnue.accumulator_deactivate(accumulator, WHITE, idx_w);
    g_nnue.accumulator_deactivate(accumulator, BLACK, idx_b);
}

void Board::make_null_move() {
    // Save current state on history stack
    history[history_ply] = {
        castling_rights,
        en_passant_square,
        halfmove_clock,
        NO_PIECE,
        hash_key,
        accumulator
    };
    history_ply++;

    // XOR out old side and en passant target
    hash_key ^= zobrist_side;
    if (en_passant_square != SQ_NONE) {
        hash_key ^= zobrist_ep[en_passant_square % 8];
    }

    // Toggle side to move
    side_to_move = (Color)(side_to_move ^ 1);

    // Clear en passant square
    en_passant_square = SQ_NONE;
}

void Board::unmake_null_move() {
    history_ply--;
    StateInfo state = history[history_ply];

    castling_rights = state.castling_rights;
    en_passant_square = state.en_passant_square;
    halfmove_clock = state.halfmove_clock;
    hash_key = state.hash_key;
    accumulator = state.accumulator; // NNUE accumulator states are untouched

    side_to_move = (Color)(side_to_move ^ 1);
}

bool Board::is_repetition() const {
    int limit = std::min(halfmove_clock, history_ply);
    for (int i = 2; i <= limit; i += 2) {
        int idx = history_ply - i;
        if (idx < 0) break; // Defensive boundary shield against stack underflow
        if (history[idx].hash_key == hash_key) {
            return true;
        }
    }
    return false;
}

U64 Board::get_all_attackers(int square, U64 occ) const {
    U64 attackers = 0;
    
    // Pawns
    attackers |= get_pawn_attacks(BLACK, square) & pieces[WHITE][PAWN];
    attackers |= get_pawn_attacks(WHITE, square) & pieces[BLACK][PAWN];
    
    // Knights
    attackers |= get_knight_attacks(square) & (pieces[WHITE][KNIGHT] | pieces[BLACK][KNIGHT]);
    
    // Bishops / Queens
    attackers |= get_bishop_attacks(square, occ) & (pieces[WHITE][BISHOP] | pieces[BLACK][BISHOP] | pieces[WHITE][QUEEN] | pieces[BLACK][QUEEN]);
    
    // Rooks / Queens
    attackers |= get_rook_attacks(square, occ) & (pieces[WHITE][ROOK] | pieces[BLACK][ROOK] | pieces[WHITE][QUEEN] | pieces[BLACK][QUEEN]);
    
    // Kings
    attackers |= get_king_attacks(square) & (pieces[WHITE][KING] | pieces[BLACK][KING]);
    
    return attackers & occ;
}

int Board::see(Move move) const {
    int from = move.from();
    int to = move.to();
    int flag = move.flag();
    
    int us = side_to_move;
    int them = us ^ 1;
    
    Piece moving_piece = board_array[from];
    PieceType pt = (PieceType)(moving_piece % 6);
    
    Piece captured = (flag == FLAG_EP) ? (us == WHITE ? B_PAWN : W_PAWN) : board_array[to];
    int cap_type = (captured != NO_PIECE) ? (captured % 6) : PAWN;
    
    const int piece_values[6] = { 100, 320, 330, 500, 900, 20000 };
    
    int gain[32];
    gain[0] = piece_values[cap_type];
    if (move.is_promotion()) {
        int promo_pt = move.promotion_piece_type();
        gain[0] += piece_values[promo_pt] - piece_values[PAWN];
    }
    
    U64 occ = occupancies[BOTH];
    clear_bit(occ, from);
    
    int depth = 1;
    int active_side = them;
    int current_piece_value = piece_values[pt];
    
    U64 attackers = get_all_attackers(to, occ);
    
    while (true) {
        int cheapest_sq = -1;
        int cheapest_val = 999999;
        
        U64 side_attackers = attackers & occupancies[active_side];
        if (!side_attackers) break;
        
        U64 temp = side_attackers;
        while (temp) {
            int sq = pop_lsb(temp);
            Piece p = board_array[sq];
            int type = p % 6;
            if (piece_values[type] < cheapest_val) {
                cheapest_val = piece_values[type];
                cheapest_sq = sq;
            }
        }
        
        if (cheapest_sq == -1) break;
        
        clear_bit(occ, cheapest_sq);
        gain[depth] = current_piece_value;
        current_piece_value = cheapest_val;
        
        if (board_array[cheapest_sq] % 6 == KING) {
            depth++;
            break;
        }
        
        attackers = get_all_attackers(to, occ);
        
        active_side ^= 1;
        depth++;
    }
    
    while (--depth > 0) {
        gain[depth - 1] = gain[depth - 1] - gain[depth];
        if (depth - 1 > 0) {
            gain[depth - 1] = std::max(0, gain[depth - 1]);
        }
    }
    
    return gain[0];
}

LegalityMasks Board::get_legality_masks() const {
    LegalityMasks masks;
    masks.checkers = 0;
    masks.pinned = 0;
    std::fill(std::begin(masks.pin_rays), std::end(masks.pin_rays), 0ULL);

    int us = side_to_move;
    int them = us ^ 1;
    int king_sq = get_lsb(pieces[us][KING]);

    // 1. Get checkers
    masks.checkers = (get_knight_attacks(king_sq) & pieces[them][KNIGHT]) |
                     (get_pawn_attacks(us, king_sq) & pieces[them][PAWN]) |
                     (get_bishop_attacks(king_sq, occupancies[BOTH]) & (pieces[them][BISHOP] | pieces[them][QUEEN])) |
                     (get_rook_attacks(king_sq, occupancies[BOTH]) & (pieces[them][ROOK] | pieces[them][QUEEN]));

    // 2. Get pinned pieces and their pin rays
    U64 potential_pinners = (get_bishop_attacks(king_sq, 0) & (pieces[them][BISHOP] | pieces[them][QUEEN])) |
                            (get_rook_attacks(king_sq, 0) & (pieces[them][ROOK] | pieces[them][QUEEN]));

    while (potential_pinners) {
        int slider_sq = pop_lsb(potential_pinners);
        U64 blockers = between_bb[king_sq][slider_sq] & occupancies[BOTH];
        if (count_bits(blockers) == 1) {
            int pinned_sq = get_lsb(blockers);
            if (occupancies[us] & (1ULL << pinned_sq)) {
                masks.pinned |= (1ULL << pinned_sq);
                masks.pin_rays[pinned_sq] = line_bb[king_sq][slider_sq];
            }
        }
    }

    return masks;
}

bool Board::is_move_legal(Move move, const LegalityMasks& masks) const {
    int from = move.from();
    int to = move.to();
    int flag = move.flag();

    int us = side_to_move;
    int them = us ^ 1;
    Piece moving_piece = board_array[from];
    PieceType pt = (PieceType)(moving_piece % 6);
    int king_sq = get_lsb(pieces[us][KING]);

    if (pt == KING) {
        U64 temp_occ = occupancies[BOTH] ^ (1ULL << from);
        
        Piece captured = board_array[to];
        int cap_type = (captured != NO_PIECE) ? (captured % 6) : -1;
        if (cap_type != -1) {
            temp_occ ^= (1ULL << to);
        }
        
        U64 pawn_attackers = get_pawn_attacks(us, to) & pieces[them][PAWN];
        if (pawn_attackers) return false;
        
        U64 knight_attackers = get_knight_attacks(to) & pieces[them][KNIGHT];
        if (knight_attackers) return false;
        
        U64 bishop_attackers = get_bishop_attacks(to, temp_occ) & (pieces[them][BISHOP] | pieces[them][QUEEN]);
        if (bishop_attackers) return false;
        
        U64 rook_attackers = get_rook_attacks(to, temp_occ) & (pieces[them][ROOK] | pieces[them][QUEEN]);
        if (rook_attackers) return false;
        
        U64 king_attackers = get_king_attacks(to) & pieces[them][KING];
        if (king_attackers) return false;
        
        return true;
    }

    if (masks.checkers) {
        if (count_bits(masks.checkers) > 1) {
            return false;
        }
        int checker_sq = get_lsb(masks.checkers);
        U64 check_mask = (1ULL << checker_sq) | between_bb[king_sq][checker_sq];
        if (!(check_mask & (1ULL << to))) {
            return false;
        }
    }

    if (masks.pinned & (1ULL << from)) {
        if (!(masks.pin_rays[from] & (1ULL << to))) {
            return false;
        }
    }

    if (flag == FLAG_EP) {
        int ep_pawn_sq = (us == WHITE) ? (to - 8) : (to + 8);
        U64 temp_occ = occupancies[BOTH] ^ (1ULL << from) ^ (1ULL << ep_pawn_sq) ^ (1ULL << to);
        U64 horizontal_attacks = get_rook_attacks(king_sq, temp_occ);
        if (horizontal_attacks & (pieces[them][ROOK] | pieces[them][QUEEN])) {
            return false;
        }
    }

    return true;
}


