#include "../src/board.h"
#include "../src/movegen.h"
#include "../src/nnue.h"

#include <array>
#include <iostream>
#include <memory>
#include <string>

namespace {

bool fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

bool accumulators_equal(const Accumulator& lhs, const Accumulator& rhs) {
    for (int side = 0; side < 2; ++side)
        for (int lane = 0; lane < L1_SIZE; ++lane)
            if (lhs.v[side][lane] != rhs.v[side][lane])
                return false;
    return true;
}

bool occupancies_match(const Board& board) {
    U64 expected[2] = {0, 0};
    for (int square = 0; square < 64; ++square) {
        Piece piece = board.get_piece_at(square);
        if (piece != NO_PIECE)
            expected[piece / 6] |= 1ULL << square;
    }
    return board.get_occupancy(WHITE) == expected[WHITE] &&
           board.get_occupancy(BLACK) == expected[BLACK] &&
           board.get_occupancy(BOTH) == (expected[WHITE] | expected[BLACK]);
}

Move find_move(const Board& board, int from, int to, int flag) {
    MoveList moves;
    generate_pseudo_legal_moves(board, moves);
    for (int i = 0; i < moves.count; ++i)
        if (moves.moves[i].from() == from && moves.moves[i].to() == to &&
            moves.moves[i].flag() == flag)
            return moves.moves[i];
    return Move();
}

bool verify_fast_legality(Board& board, const std::string& label) {
    if (board.get_hash_key() != board.recompute_hash() || !occupancies_match(board))
        return fail(label + ": FEN hash mismatch");

    const std::string root_fen = board.get_fen();
    const U64 root_hash = board.get_hash_key();
    const Accumulator root_acc = board.get_accumulator();
    const LegalityMasks masks = board.get_legality_masks();
    MoveList moves;
    generate_pseudo_legal_moves(board, moves);

    for (int i = 0; i < moves.count; ++i) {
        const Move move = moves.moves[i];
        const bool fast = board.is_move_legal(move, masks);
        const bool slow = board.make_move(move);
        if (fast != slow)
            return fail(label + ": fast/slow legality mismatch for " +
                        square_to_str(move.from()) + square_to_str(move.to()));

        if (slow) {
            if (board.get_hash_key() != board.recompute_hash() || !occupancies_match(board))
                return fail(label + ": incremental hash mismatch after move");
            Accumulator rebuilt;
            g_nnue.init_accumulator(board, rebuilt);
            if (!accumulators_equal(board.get_accumulator(), rebuilt))
                return fail(label + ": incremental accumulator mismatch after move");
            board.unmake_move(move);
        }

        if (board.get_fen() != root_fen || board.get_hash_key() != root_hash ||
            !occupancies_match(board) ||
            !accumulators_equal(board.get_accumulator(), root_acc))
            return fail(label + ": state mismatch after make/unmake");
    }

    board.make_null_move();
    if (board.get_hash_key() != board.recompute_hash() || !occupancies_match(board))
        return fail(label + ": null-move hash mismatch");
    board.unmake_null_move();
    if (board.get_fen() != root_fen || board.get_hash_key() != root_hash ||
        !accumulators_equal(board.get_accumulator(), root_acc))
        return fail(label + ": state mismatch after null move");
    return true;
}

bool verify_history_bounds(Board& board) {
    const std::string root_fen = board.get_fen();
    const U64 root_hash = board.get_hash_key();
    const Accumulator root_acc = board.get_accumulator();

    for (int ply = 0; ply < Board::HISTORY_CAPACITY; ++ply)
        if (!board.make_null_move())
            return fail("history rejected an in-capacity null move");
    if (board.make_null_move())
        return fail("history accepted an out-of-capacity null move");
    for (int ply = 0; ply < Board::HISTORY_CAPACITY; ++ply)
        board.unmake_null_move();

    if (board.get_fen() != root_fen || board.get_hash_key() != root_hash ||
        !accumulators_equal(board.get_accumulator(), root_acc))
        return fail("state mismatch after history-capacity test");
    return true;
}

bool verify_ep_canonicalization(Board& board) {
    board.parse_fen("4k3/8/8/8/4P3/8/8/4K3 b - - 0 1");
    U64 without_ep = board.get_hash_key();
    board.parse_fen("4k3/8/8/8/4P3/8/8/4K3 b - e3 0 1");
    if (board.get_en_passant_square() != SQ_NONE || board.get_hash_key() != without_ep)
        return fail("uncapturable EP square was not canonicalized");

    board.parse_fen("4k3/8/8/8/3pP3/8/8/4K3 b - - 0 1");
    without_ep = board.get_hash_key();
    board.parse_fen("4k3/8/8/8/3pP3/8/8/4K3 b - e3 0 1");
    if (board.get_en_passant_square() != SQ_E3 || board.get_hash_key() == without_ep)
        return fail("capturable EP square was not retained and hashed");

    board.parse_fen("4k3/8/8/8/3p4/8/4P3/4K3 w - - 0 1");
    Move capturable_push = find_move(board, SQ_E2, SQ_E4, FLAG_DOUBLE_PAWN);
    if (capturable_push.is_none() || !board.make_move(capturable_push))
        return fail("could not make capturable double pawn push");
    if (board.get_en_passant_square() != SQ_E3 || board.get_hash_key() != board.recompute_hash())
        return fail("double push did not retain capturable EP square");
    board.unmake_move(capturable_push);

    board.parse_fen("4k3/8/8/8/8/8/4P3/4K3 w - - 0 1");
    Move uncapturable_push = find_move(board, SQ_E2, SQ_E4, FLAG_DOUBLE_PAWN);
    if (uncapturable_push.is_none() || !board.make_move(uncapturable_push))
        return fail("could not make uncapturable double pawn push");
    if (board.get_en_passant_square() != SQ_NONE || board.get_hash_key() != board.recompute_hash())
        return fail("double push retained uncapturable EP square");
    return true;
}

bool verify_batched_accumulator_updates(Board& board) {
    board.parse_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    const std::string root_fen = board.get_fen();
    const Accumulator root_acc = board.get_accumulator();
    constexpr std::array<std::pair<int, int>, 4> coordinates = {{
        {SQ_E2, SQ_E4}, {SQ_E7, SQ_E5}, {SQ_G1, SQ_F3}, {SQ_B8, SQ_C6}
    }};
    std::array<Move, coordinates.size()> moves;

    for (std::size_t index = 0; index < coordinates.size(); ++index) {
        MoveList list;
        generate_pseudo_legal_moves(board, list);
        Move selected;
        for (int move_index = 0; move_index < list.count; ++move_index)
            if (list.moves[move_index].from() == coordinates[index].first &&
                list.moves[move_index].to() == coordinates[index].second) {
                selected = list.moves[move_index];
                break;
            }
        if (selected.is_none() || !board.make_move(selected))
            return fail("could not build batched accumulator sequence");
        moves[index] = selected;
    }

    Accumulator rebuilt;
    g_nnue.init_accumulator(board, rebuilt);
    if (!accumulators_equal(board.get_accumulator(), rebuilt))
        return fail("batched accumulator catch-up mismatch");

    for (std::size_t index = moves.size(); index-- > 0;)
        board.unmake_move(moves[index]);
    if (board.get_fen() != root_fen ||
        !accumulators_equal(board.get_accumulator(), root_acc))
        return fail("batched accumulator rewind mismatch");
    return true;
}

}  // namespace

int main() {
    Board::init_zobrist();
    init_all_attack_tables();
    if (!g_nnue.load_network("coco.nnue"))
        return fail("could not load coco.nnue") ? 0 : 1;

    auto board = std::make_unique<Board>();

    board->parse_fen("8/8/8/3pP3/4K3/8/8/4k3 w - d6 0 1");
    Move ep_evasion = find_move(*board, SQ_E5, SQ_D6, FLAG_EP);
    if (ep_evasion.is_none()) return fail("missing legal EP evasion") ? 0 : 1;
    if (!board->is_move_legal(ep_evasion, board->get_legality_masks()))
        return fail("fast legality rejected EP capture of checking pawn") ? 0 : 1;

    board->parse_fen("8/8/8/r4pPK/8/8/8/4k3 w - f6 0 1");
    Move exposed_ep = find_move(*board, SQ_G5, SQ_F6, FLAG_EP);
    if (exposed_ep.is_none()) return fail("missing discovered-check EP case") ? 0 : 1;
    if (board->is_move_legal(exposed_ep, board->get_legality_masks()))
        return fail("fast legality accepted EP exposing rook check") ? 0 : 1;

    constexpr std::array<const char*, 7> positions = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/8/8/3pP3/4K3/8/8/4k3 w - d6 0 1",
        "8/8/8/r4pPK/8/8/8/4k3 w - f6 0 1",
        "4k3/P6p/8/8/8/8/p6P/4K3 w - - 0 1",
        "4r1k1/8/8/8/8/2b5/3P4/4K3 w - - 0 1",
        "4k2r/6P1/8/8/8/8/8/4K3 w - - 0 1",
    };

    for (std::size_t i = 0; i < positions.size(); ++i) {
        board->parse_fen(positions[i]);
        if (!verify_fast_legality(*board, "position " + std::to_string(i + 1)))
            return 1;
    }

    board->parse_fen(positions[0]);
    const std::string preserved_fen = board->get_fen();
    constexpr std::array<const char*, 8> malformed_fens = {
        "8/8/8/8/8/8/8/9 w - - 0 1",
        "8/8/8/8/8/8/8 w - - 0 1",
        "8/8/8/8/8/8/8/8 x - - 0 1",
        "8/8/8/8/8/8/8/K6k w KK - 0 1",
        "8/8/8/8/8/8/8/K6k w - a4 0 1",
        "8/8/8/8/8/8/8/K6k w - - -1 1",
        "8/8/8/8/8/8/8/K6k w - - 999999999999999999999 1",
        "8/8/8/8/8/8/8/K7 w - - 0 1",
    };
    for (const char* fen : malformed_fens) {
        if (board->parse_fen(fen))
            return fail(std::string("accepted malformed FEN: ") + fen) ? 0 : 1;
        if (board->get_fen() != preserved_fen)
            return fail("malformed FEN mutated the live board") ? 0 : 1;
    }
    if (!verify_history_bounds(*board))
        return 1;
    if (!verify_ep_canonicalization(*board))
        return 1;
    if (!verify_batched_accumulator_updates(*board))
        return 1;

    std::cout << "PASS: EP legality, make/unmake, null move, hash, and all "
              << L1_SIZE << " accumulator lanes; sizeof(Board)="
              << sizeof(Board) << " bytes\n";
    return 0;
}
