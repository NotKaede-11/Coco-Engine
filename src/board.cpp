#include "board.h"
#include "ravager_eval.h"
#include "movegen.h"
#include "nnue.h"
#include <iostream>
#include <sstream>
#include <cctype>
#include <cassert>
#include <charconv>
#include <bit>
#include <limits>

U64 Board::zobrist_pieces[2][6][64];
U64 Board::zobrist_castling[16];
U64 Board::zobrist_ep[8];
U64 Board::zobrist_side;
namespace {
U64 zobrist_chess960;
U64 zobrist_castle_rook[4][8];

int castle_index(Color color, bool kingside) {
    return 2 * color + !kingside;
}
}

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
    castle_rook[0] = SQ_H1;
    castle_rook[1] = SQ_A1;
    castle_rook[2] = SQ_H8;
    castle_rook[3] = SQ_A8;
    chess960 = false;
    en_passant_square = SQ_NONE;
    halfmove_clock = 0;
    fullmove_number = 1;
    hash_key = 0ULL;
    pawn_key = 0ULL;
    non_pawn_key[WHITE] = 0ULL;
    non_pawn_key[BLACK] = 0ULL;
    history_ply = 0;

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
    zobrist_chess960 = next_random();
    for (auto& wing : zobrist_castle_rook)
        for (U64& key : wing)
            key = next_random();

}

U64 Board::castling_key() const {
    U64 key = zobrist_castling[castling_rights];
    if (chess960) {
        key ^= zobrist_chess960;
        unsigned rights = castling_rights;
        while (rights) {
            const int wing = std::countr_zero(rights);
            key ^= zobrist_castle_rook[wing][castle_rook[wing] % 8];
            rights &= rights - 1;
        }
    }
    return key;
}

U64 Board::calculate_hash() const {
    U64 k = 0;
    for (int sq = 0; sq < 64; sq++) {
        Piece p = board_array[sq];
        if (p != NO_PIECE) {
            k ^= zobrist_pieces[p / 6][p % 6][sq];
        }
    }
    k ^= castling_key();
    if (en_passant_square != SQ_NONE) {
        k ^= zobrist_ep[en_passant_square % 8];
    }
    if (side_to_move == BLACK) {
        k ^= zobrist_side;
    }
    return k;
}

uint16_t Board::calculate_pawn_key() const {
    uint16_t key = 0;
    for (int color = WHITE; color <= BLACK; ++color) {
        U64 pawns = pieces[color][PAWN];
        while (pawns) {
            const int square = pop_lsb(pawns);
            key ^= static_cast<uint16_t>(zobrist_pieces[color][PAWN][square]);
        }
    }
    return key;
}

uint16_t Board::calculate_non_pawn_key(Color color) const {
    uint16_t key = 0;
    for (int piece_type = KNIGHT; piece_type <= KING; ++piece_type) {
        U64 occupied = pieces[color][piece_type];
        while (occupied) {
            const int square = pop_lsb(occupied);
            key ^= static_cast<uint16_t>(zobrist_pieces[color][piece_type][square]);
        }
    }
    return key;
}

bool Board::has_en_passant_capture() const {
    if (en_passant_square == SQ_NONE) {
        return false;
    }

    int expected_rank = side_to_move == WHITE ? 5 : 2;
    if (en_passant_square / 8 != expected_rank ||
        board_array[en_passant_square] != NO_PIECE) {
        return false;
    }

    int captured_sq = en_passant_square + (side_to_move == WHITE ? -8 : 8);
    int origin_sq = en_passant_square + (side_to_move == WHITE ? 8 : -8);
    Piece expected_pawn = side_to_move == WHITE ? B_PAWN : W_PAWN;
    if (captured_sq < 0 || captured_sq >= 64 || origin_sq < 0 || origin_sq >= 64 ||
        board_array[captured_sq] != expected_pawn || board_array[origin_sq] != NO_PIECE) {
        return false;
    }

    U64 capturers = get_pawn_attacks(side_to_move ^ 1, en_passant_square) &
                    pieces[side_to_move][PAWN];
    return capturers != 0;
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

bool Board::parse_fen(const std::string& fen, bool variant) {
    std::istringstream input(fen);
    std::string placement, active, castling, ep, halfmove, fullmove, extra;
    if (!(input >> placement >> active >> castling >> ep >> halfmove >> fullmove)
        || (input >> extra)) {
        return false;
    }

    int rank = 7;
    int file = 0;
    int white_kings = 0;
    int black_kings = 0;
    int home_king[2] = {SQ_NONE, SQ_NONE};
    U64 home_rooks[2] = {};
    for (char c : placement) {
        if (c == '/') {
            if (file != 8 || rank == 0) return false;
            --rank;
            file = 0;
        } else if (c >= '1' && c <= '8') {
            file += c - '0';
            if (file > 8) return false;
        } else {
            const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower != 'p' && lower != 'n' && lower != 'b'
                && lower != 'r' && lower != 'q' && lower != 'k') return false;
            if (++file > 8) return false;
            if (c == 'K') ++white_kings;
            if (c == 'k') ++black_kings;
            const Color color = std::isupper(static_cast<unsigned char>(c)) ? WHITE : BLACK;
            if (rank == 7 * color) {
                const int square = rank * 8 + file - 1;
                if (lower == 'k') home_king[color] = square;
                if (lower == 'r') home_rooks[color] |= 1ULL << square;
            }
        }
    }
    if (rank != 0 || file != 8 || white_kings != 1 || black_kings != 1) return false;
    if (active != "w" && active != "b") return false;

    int rights = NO_CASTLING;
    int rook_origins[4] = {SQ_H1, SQ_A1, SQ_H8, SQ_A8};
    if (castling != "-") {
        for (char c : castling) {
            const Color color = std::isupper(static_cast<unsigned char>(c)) ? WHITE : BLACK;
            const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool kingside = lower == 'k';
            int rook = SQ_NONE;
            if (!variant) {
                if (lower != 'k' && lower != 'q') return false;
            } else {
                const int king = home_king[color];
                if (king == SQ_NONE) return false;
                if (lower == 'k' || lower == 'q') {
                    U64 rooks = home_rooks[color];
                    // X-FEN K/Q identify the outermost rook on that side.
                    while (rooks) {
                        const int square = pop_lsb(rooks);
                        if (kingside && square > king) rook = square;
                        if (!kingside && square < king && rook == SQ_NONE) rook = square;
                    }
                } else if (lower >= 'a' && lower <= 'h') {
                    rook = 56 * color + lower - 'a';
                    kingside = rook > king;
                }
                if (rook == SQ_NONE || !(home_rooks[color] & (1ULL << rook))) return false;
            }
            const int wing = castle_index(color, kingside);
            if (rights & (1 << wing)) return false;
            rights |= 1 << wing;
            if (variant) rook_origins[wing] = rook;
        }
    }
    if (ep != "-" && (ep.size() != 2 || ep[0] < 'a' || ep[0] > 'h'
                       || (ep[1] != '3' && ep[1] != '6'))) return false;

    auto parse_clock = [](const std::string& text, int minimum, int& value) {
        if (text.empty()) return false;
        int parsed = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()
            || parsed < minimum) return false;
        value = parsed;
        return true;
    };
    int parsed_halfmove = 0;
    int parsed_fullmove = 1;
    if (!parse_clock(halfmove, 0, parsed_halfmove)
        || !parse_clock(fullmove, 1, parsed_fullmove)) return false;

    // Validation is complete. From here onward every square index is bounded,
    // so malformed protocol input can never partially replace the live board.
    clear();
    rank = 7;
    file = 0;
    for (char c : placement) {
        if (c == '/') {
            --rank;
            file = 0;
        } else if (c >= '1' && c <= '8') {
            file += c - '0';
        } else {
            const Color color = std::isupper(static_cast<unsigned char>(c)) ? WHITE : BLACK;
            const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            PieceType pt = lower == 'p' ? PAWN : lower == 'n' ? KNIGHT
                         : lower == 'b' ? BISHOP : lower == 'r' ? ROOK
                         : lower == 'q' ? QUEEN : KING;
            const int sq = rank * 8 + file++;
            set_bit(pieces[color][pt], sq);
            board_array[sq] = static_cast<Piece>(color * 6 + pt);
        }
    }

    side_to_move = active == "w" ? WHITE : BLACK;
    chess960 = variant;
    castling_rights = rights;
    for (int wing = 0; wing < 4; ++wing) castle_rook[wing] = rook_origins[wing];
    en_passant_square = ep == "-" ? SQ_NONE
        : static_cast<Square>((ep[1] - '1') * 8 + ep[0] - 'a');
    halfmove_clock = parsed_halfmove;
    fullmove_number = parsed_fullmove;

    update_occupancies();
    if (!has_en_passant_capture()) {
        en_passant_square = SQ_NONE;
    }
    hash_key = calculate_hash();
    pawn_key = calculate_pawn_key();
    non_pawn_key[WHITE] = calculate_non_pawn_key(WHITE);
    non_pawn_key[BLACK] = calculate_non_pawn_key(BLACK);
    history_ply = 0;

    g_nnue.init_accumulator(*this, accumulator);
    return true;
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
    for (int wing = 0; wing < 4; ++wing) {
        if (castling_rights & (1 << wing)) {
            castling += chess960 ? (wing < 2 ? 'A' : 'a') + castle_rook[wing] % 8
                                 : "KQkq"[wing];
        }
    }
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
    return is_square_attacked(square, attacker_color, occupancies[BOTH]);
}

bool Board::is_square_attacked(int square, int attacker_color, U64 occupancy) const {
    // 1. Attacked by pawns
    int defender_color = attacker_color ^ 1;
    U64 pawn_attackers = get_pawn_attacks(defender_color, square) & pieces[attacker_color][PAWN];
    if (pawn_attackers) return true;

    // 2. Attacked by knights
    U64 knight_attackers = get_knight_attacks(square) & pieces[attacker_color][KNIGHT];
    if (knight_attackers) return true;

    // 3. Attacked by bishops/queens (diagonal sliding)
    U64 bishop_queen_attackers = get_bishop_attacks(square, occupancy) & (pieces[attacker_color][BISHOP] | pieces[attacker_color][QUEEN]);
    if (bishop_queen_attackers) return true;

    // 4. Attacked by rooks/queens (orthogonal sliding)
    U64 rook_queen_attackers = get_rook_attacks(square, occupancy) & (pieces[attacker_color][ROOK] | pieces[attacker_color][QUEEN]);
    if (rook_queen_attackers) return true;

    // 5. Attacked by king
    U64 king_attackers = get_king_attacks(square) & pieces[attacker_color][KING];
    if (king_attackers) return true;

    return false;
}

Move Board::castle_move(bool kingside) const {
    const Color us = side_to_move;
    const int wing = castle_index(us, kingside);
    if (!(castling_rights & (1 << wing))) return Move();
    const int king = get_lsb(pieces[us][KING]);
    const int rook = castle_rook[wing];
    if (king / 8 != 7 * us || (!chess960 && king % 8 != 4)
        || !(pieces[us][ROOK] & (1ULL << rook))) return Move();

    const int king_to = 56 * us + (kingside ? 6 : 2);
    const int rook_to = 56 * us + (kingside ? 5 : 3);
    const U64 kingless = occupancies[BOTH] & ~(1ULL << king);
    const U64 vacant = kingless & ~(1ULL << rook);
    const U64 travel = between_bb[king][king_to] | between_bb[rook][rook_to]
                     | (1ULL << king_to) | (1ULL << rook_to);
    if (vacant & travel) return Move();

    U64 crossings = between_bb[king][king_to] | (1ULL << king);
    while (crossings)
        if (is_square_attacked(pop_lsb(crossings), us ^ 1, kingless)) return Move();
    if (is_square_attacked(king_to, us ^ 1, vacant | (1ULL << rook_to))) return Move();

    return Move(king, chess960 ? rook : king_to, kingside ? FLAG_KING_CASTLE : FLAG_QUEEN_CASTLE);
}

void Board::relocate_castling(Color color, int king_from, int rook_from, int king_to, int rook_to) {
    // Clear both origins before writing either destination. In Chess960 a
    // piece can stay in place, land on the other piece's square, or swap.
    board_array[king_from] = board_array[rook_from] = NO_PIECE;
    board_array[king_to] = static_cast<Piece>(color * 6 + KING);
    board_array[rook_to] = static_cast<Piece>(color * 6 + ROOK);
    pieces[color][KING] = 1ULL << king_to;
    clear_bit(pieces[color][ROOK], rook_from);
    set_bit(pieces[color][ROOK], rook_to);
    const U64 origins = (1ULL << king_from) | (1ULL << rook_from);
    const U64 destinations = (1ULL << king_to) | (1ULL << rook_to);
    occupancies[color] = (occupancies[color] & ~origins) | destinations;
    occupancies[BOTH] = (occupancies[BOTH] & ~origins) | destinations;
    if (king_from != king_to) {
        nnue_move_piece(color, KING, king_from, king_to);
        xor_piece_keys(color, KING, king_from);
        xor_piece_keys(color, KING, king_to);
    }
    if (rook_from != rook_to) {
        nnue_move_piece(color, ROOK, rook_from, rook_to);
        xor_piece_keys(color, ROOK, rook_from);
        xor_piece_keys(color, ROOK, rook_to);
    }
}

bool Board::make_move(Move move, bool checked) {
    if (history_ply < 0 || history_ply >= HISTORY_CAPACITY) {
        return false;
    }
    RavagerEval::before_move(*this, history_ply);
    assert(history_ply >= 0 && history_ply < HISTORY_CAPACITY);

    int from = move.from();
    int to = move.to();
    int flag = move.flag();

    Color us = side_to_move;
    Color them = (Color)(us ^ 1);
    Piece moving_piece = board_array[from];
    PieceType pt = (PieceType)(moving_piece % 6);
    if (chess960 && move.is_castling() && !checked && move != castle_move(flag == FLAG_KING_CASTLE))
        return false;

    side_to_move = them;

    if (us == BLACK) {
        fullmove_number++;
    }

    history[history_ply] = {
        castling_rights,
        en_passant_square,
        halfmove_clock,
        NO_PIECE,
        hash_key,
        pawn_key,
        {non_pawn_key[WHITE], non_pawn_key[BLACK]}
    };
    history_ply++;

    // XOR out old side, castling, and EP
    hash_key ^= zobrist_side;
    hash_key ^= castling_key();
    if (en_passant_square != SQ_NONE) {
        hash_key ^= zobrist_ep[en_passant_square % 8];
    }

    int new_en_passant_square = SQ_NONE;

    if (move.is_castling()) {
        const bool kingside = flag == FLAG_KING_CASTLE;
        relocate_castling(us, from, castle_rook[castle_index(us, kingside)],
                          56 * us + (kingside ? 6 : 2), 56 * us + (kingside ? 5 : 3));
        ++halfmove_clock;
    } else {
        if (move.is_capture()) {
            if (flag == FLAG_EP) {
                int ep_pawn_sq = (us == WHITE) ? (to - 8) : (to + 8);
                Piece captured = board_array[ep_pawn_sq];
                history[history_ply - 1].captured_piece = captured;

                nnue_deactivate_piece(them, PAWN, ep_pawn_sq);

                clear_bit(pieces[them][PAWN], ep_pawn_sq);
                board_array[ep_pawn_sq] = NO_PIECE;
                clear_bit(occupancies[them], ep_pawn_sq);
                clear_bit(occupancies[BOTH], ep_pawn_sq);
                xor_piece_keys(them, PAWN, ep_pawn_sq);
            } else {
                Piece captured = board_array[to];
                history[history_ply - 1].captured_piece = captured;
                PieceType cap_type = (PieceType)(captured % 6);

                nnue_deactivate_piece(them, cap_type, to);

                clear_bit(pieces[them][cap_type], to);
                clear_bit(occupancies[them], to);
                clear_bit(occupancies[BOTH], to);
                xor_piece_keys(them, cap_type, to);
            }
            halfmove_clock = 0; // Reset on capture
        } else if (pt == PAWN) {
            halfmove_clock = 0; // Pawn pushes and non-capturing promotions reset rule 50 too.
        } else {
            halfmove_clock++;
        }

        nnue_move_piece(us, pt, from, to);

        clear_bit(pieces[us][pt], from);
        set_bit(pieces[us][pt], to);
        clear_bit(occupancies[us], from);
        set_bit(occupancies[us], to);
        clear_bit(occupancies[BOTH], from);
        set_bit(occupancies[BOTH], to);
        board_array[from] = NO_PIECE;
        board_array[to] = moving_piece;

        xor_piece_keys(us, pt, from);
        xor_piece_keys(us, pt, to);

        if (move.is_promotion()) {
            PieceType promo_pt = (PieceType)move.promotion_piece_type();

            nnue_deactivate_piece(us, PAWN, to);
            nnue_activate_piece(us, promo_pt, to);

            clear_bit(pieces[us][PAWN], to);
            set_bit(pieces[us][promo_pt], to);
            board_array[to] = (Piece)(us * 6 + promo_pt);

            xor_piece_keys(us, PAWN, to);
            xor_piece_keys(us, promo_pt, to);
        }

        // Handle double pawn pushes (setup en passant)
        if (flag == FLAG_DOUBLE_PAWN) {
            new_en_passant_square = (us == WHITE) ? (from + 8) : (from - 8);
        }
    }

    // Verify move legality (cannot expose king to check)
    if (!checked) {
        int king_sq = get_lsb(pieces[us][KING]);
        if (is_square_attacked(king_sq, them)) {
            unmake_move(move);
            return false;
        }
    }

    if (!chess960) {
        castling_rights &= castling_rights_mask[from] & castling_rights_mask[to];
    } else if (castling_rights) {
        if (pt == KING) castling_rights &= ~(3 << (2 * us));
        for (int wing = 0; wing < 4; ++wing)
            if (from == castle_rook[wing] || to == castle_rook[wing])
                castling_rights &= ~(1 << wing);
    }
    hash_key ^= castling_key();

    en_passant_square = new_en_passant_square;
    if (!has_en_passant_capture()) {
        en_passant_square = SQ_NONE;
    }
    if (en_passant_square != SQ_NONE) {
        hash_key ^= zobrist_ep[en_passant_square % 8];
    }

    return true;
}

void Board::unmake_move(Move move) {
    assert(history_ply > 0);
    if (history_ply <= 0) {
        return;
    }

    history_ply--;
    StateInfo state = history[history_ply];

    int from = move.from();
    int to = move.to();
    int flag = move.flag();

    Color us = side_to_move; // us is the color to move NOW (which is them of the move)
    Color player = (Color)(us ^ 1);
    side_to_move = player;
    if (move.is_castling()) {
        const bool kingside = flag == FLAG_KING_CASTLE;
        relocate_castling(player, 56 * player + (kingside ? 6 : 2), 56 * player + (kingside ? 5 : 3),
                          from, castle_rook[castle_index(player, kingside)]);
    } else {
        Piece moving_piece = board_array[to];

        // Reverse the exact NNUE feature changes made by make_move.  This avoids
        // storing a 2 KiB accumulator snapshot in every history entry.
        if (move.is_promotion()) {
            PieceType promo_pt = (PieceType)move.promotion_piece_type();
            nnue_deactivate_piece(player, promo_pt, to);
            nnue_activate_piece(player, PAWN, from);
        } else {
            PieceType moved_pt = (PieceType)(moving_piece % 6);
            nnue_move_piece(player, moved_pt, to, from);
        }

        if (move.is_capture()) {
            int captured_sq = (flag == FLAG_EP)
                ? ((player == WHITE) ? (to - 8) : (to + 8))
                : to;
            Piece captured = state.captured_piece;
            nnue_activate_piece(us, captured % 6, captured_sq);
        }

        // Handle promotions: revert piece back to pawn before moving back
        if (move.is_promotion()) {
            PieceType promo_pt = (PieceType)move.promotion_piece_type();

            clear_bit(pieces[player][promo_pt], to);
            set_bit(pieces[player][PAWN], to);
            board_array[to] = (Piece)(player * 6 + PAWN);
            moving_piece = board_array[to];
        }

        PieceType pt = (PieceType)(moving_piece % 6);

        clear_bit(pieces[player][pt], to);
        set_bit(pieces[player][pt], from);
        clear_bit(occupancies[player], to);
        set_bit(occupancies[player], from);
        clear_bit(occupancies[BOTH], to);
        set_bit(occupancies[BOTH], from);
        board_array[from] = moving_piece;
        board_array[to] = NO_PIECE;

        if (move.is_capture()) {
            Piece captured = state.captured_piece;
            PieceType cap_type = (PieceType)(captured % 6);

            if (flag == FLAG_EP) {
                int ep_pawn_sq = (player == WHITE) ? (to - 8) : (to + 8);

                set_bit(pieces[us][PAWN], ep_pawn_sq);
                set_bit(occupancies[us], ep_pawn_sq);
                set_bit(occupancies[BOTH], ep_pawn_sq);
                board_array[ep_pawn_sq] = captured;
            } else {
                set_bit(pieces[us][cap_type], to);
                set_bit(occupancies[us], to);
                set_bit(occupancies[BOTH], to);
                board_array[to] = captured;
            }
        }
    }

    castling_rights = state.castling_rights;
    en_passant_square = state.en_passant_square;
    halfmove_clock = state.halfmove_clock;
    hash_key = state.hash_key;
    pawn_key = state.pawn_key;
    non_pawn_key[WHITE] = state.non_pawn_key[WHITE];
    non_pawn_key[BLACK] = state.non_pawn_key[BLACK];

    if (player == BLACK) {
        fullmove_number--;
    }

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

void Board::nnue_activate_piece(int color, int pt, int sq) {}

void Board::nnue_deactivate_piece(int color, int pt, int sq) {}

void Board::nnue_move_piece(int color, int pt, int from, int to) {}

bool Board::make_null_move() {
    if (history_ply < 0 || history_ply >= HISTORY_CAPACITY) {
        return false;
    }
    RavagerEval::before_move(*this, history_ply);
    assert(history_ply >= 0 && history_ply < HISTORY_CAPACITY);

    // Save current state on history stack
    history[history_ply] = {
        castling_rights,
        en_passant_square,
        halfmove_clock,
        NO_PIECE,
        hash_key,
        pawn_key,
        {non_pawn_key[WHITE], non_pawn_key[BLACK]}
    };
    history_ply++;

    // XOR out old side and en passant target
    hash_key ^= zobrist_side;
    if (en_passant_square != SQ_NONE) {
        hash_key ^= zobrist_ep[en_passant_square % 8];
    }

    side_to_move = (Color)(side_to_move ^ 1);

    en_passant_square = SQ_NONE;
    return true;
}

void Board::unmake_null_move() {
    assert(history_ply > 0);
    if (history_ply <= 0) {
        return;
    }

    history_ply--;
    StateInfo state = history[history_ply];

    castling_rights = state.castling_rights;
    en_passant_square = state.en_passant_square;
    halfmove_clock = state.halfmove_clock;
    hash_key = state.hash_key;
    pawn_key = state.pawn_key;
    non_pawn_key[WHITE] = state.non_pawn_key[WHITE];
    non_pawn_key[BLACK] = state.non_pawn_key[BLACK];

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
    const int piece_values[6] = { 100, 320, 330, 500, 900, 20000 };
    
    int cap_value = (captured != NO_PIECE) ? piece_values[captured % 6] : 0;
    
    int gain[32];
    gain[0] = cap_value;
    if (move.is_promotion()) {
        int promo_pt = move.promotion_piece_type();
        gain[0] += piece_values[promo_pt] - piece_values[PAWN];
    }
    
    U64 occ = occupancies[BOTH];
    clear_bit(occ, from);
    U64 remaining[2] = { occupancies[WHITE], occupancies[BLACK] };
    clear_bit(remaining[us], from);

    if (captured != NO_PIECE) {
        const int captured_square = move.is_en_passant()
            ? to + (us == WHITE ? -8 : 8)
            : to;
        clear_bit(occ, captured_square);
        clear_bit(remaining[them], captured_square);
    }
    
    int depth = 1;
    int active_side = them;
    int current_piece_value = move.is_promotion()
        ? piece_values[move.promotion_piece_type()]
        : piece_values[pt];
    
    U64 attackers = get_all_attackers(to, occ);
    
    while (true) {
        int cheapest_sq = -1;
        int cheapest_val = 999999;
        
        U64 side_attackers = attackers & remaining[active_side];
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
        clear_bit(remaining[active_side], cheapest_sq);
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

    // 3. Compute threats (all squares attacked by the opponent)
    U64 threats = 0;

    // Kingless occupancy: remove our king so slider rays "see through" it
    U64 kingless_occ = occupancies[BOTH] ^ (1ULL << king_sq);

    // Pawns (setwise, no loop)
    U64 their_pawns = pieces[them][PAWN];
    threats |= (them == WHITE) ? pawn_attacks_white(their_pawns) 
                               : pawn_attacks_black(their_pawns);

    // Knights (loop)
    U64 their_knights = pieces[them][KNIGHT];
    while (their_knights) {
        threats |= get_knight_attacks(pop_lsb(their_knights));
    }

    // Bishops + Queen diagonals (loop, using kingless_occ)
    U64 their_bishops = pieces[them][BISHOP] | pieces[them][QUEEN];
    while (their_bishops) {
        threats |= get_bishop_attacks(pop_lsb(their_bishops), kingless_occ);
    }

    // Rooks + Queen orthogonals (loop, using kingless_occ)
    U64 their_rooks = pieces[them][ROOK] | pieces[them][QUEEN];
    while (their_rooks) {
        threats |= get_rook_attacks(pop_lsb(their_rooks), kingless_occ);
    }

    // Enemy king
    threats |= get_king_attacks(get_lsb(pieces[them][KING]));

    masks.threats = threats;

    return masks;
}

bool Board::is_move_legal(Move move, const LegalityMasks& masks) const {
    if (chess960 && move.is_castling()) return move == castle_move(move.flag() == FLAG_KING_CASTLE);
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
        int ep_pawn_sq = (us == WHITE) ? (to - 8) : (to + 8);
        bool captures_checker_ep = flag == FLAG_EP && checker_sq == ep_pawn_sq;
        if (!captures_checker_ep && !(check_mask & (1ULL << to))) {
            return false;
        }
    }

    if (masks.pinned & (1ULL << from)) {
        if (!(masks.pin_rays[from] & (1ULL << to))) {
            return false;
        }
    }

    // En passant removes a pawn from a square other than the destination, so
    // validate both slider axes using the resulting occupancy.
    if (flag == FLAG_EP) {
        int ep_pawn_sq = (us == WHITE) ? (to - 8) : (to + 8);
        U64 temp_occ = (occupancies[BOTH] ^ (1ULL << from) ^
                        (1ULL << ep_pawn_sq)) | (1ULL << to);
        if (get_rook_attacks(king_sq, temp_occ) &
            (pieces[them][ROOK] | pieces[them][QUEEN])) return false;
        if (get_bishop_attacks(king_sq, temp_occ) &
            (pieces[them][BISHOP] | pieces[them][QUEEN])) return false;
    }

    return true;
}


