#include "ravager_eval.h"
#include "board.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <vector>

extern "C" {
int rv_install(const unsigned char*, size_t);
const unsigned char* coco_network_data();
unsigned int coco_network_size();
void rv_prepare(const uint64_t*, const uint8_t*, int, int);
void rv_push(int ply);
int rv_eval(const uint64_t*, const uint8_t*, int, int, int);
}

namespace {
thread_local const Board* search_board = nullptr;

struct Features {
    uint64_t pieces[12];
    uint8_t squares[64];
    explicit Features(const Board& board) {
        for (int side = 0; side < 2; ++side)
            for (int type = 0; type < 6; ++type)
                pieces[side * 6 + type] = board.get_pieces(side, type);
        for (int square = 0; square < 64; ++square)
            squares[square] = board.get_piece_at(square);
    }
};

int evaluate(const Board& board, bool fresh) {
    const Features f(board);
    return rv_eval(f.pieces, f.squares, board.get_side_to_move(), board.get_history_ply(), fresh);
}
}

bool RavagerEval::load(const std::filesystem::path& path) {
    constexpr size_t bytes = 89315360;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream || stream.tellg() != bytes)
        return false;
    std::vector<unsigned char> weights(bytes);
    stream.seekg(0);
    return stream.read(reinterpret_cast<char*>(weights.data()), weights.size())
        && rv_install(weights.data(), weights.size());
}

bool RavagerEval::load_embedded() {
    return coco_network_size() == 89315360u
        && rv_install(coco_network_data(), coco_network_size());
}

int RavagerEval::raw(const Board& board) {
    const bool fresh = &board != search_board;
    const int score = evaluate(board, fresh);
#ifdef RAVAGER_VERIFY_FRAMES
    if (!fresh && score != evaluate(board, true))
        std::abort();
#endif
    return std::clamp(score, -VALUE_TB + 1, VALUE_TB - 1);
}

int RavagerEval::display(const Board& board) {
    int phase = 0;
    constexpr int piece_phase[]{0, 1, 1, 2, 4, 0};
    for (int type = 0; type < 6; ++type)
        phase += piece_phase[type] * count_bits(board.get_pieces(WHITE, type) | board.get_pieces(BLACK, type));
    return std::clamp(evaluate(board, true) * (128 + std::min(phase, 24)) / 152, -30076, 30076);
}

void RavagerEval::before_move(const Board& board, int ply) {
    if (&board == search_board)
        rv_push(ply);
}

RavagerEval::SearchScope::SearchScope(const Board& board) {
    const Features f(board);
    rv_prepare(f.pieces, f.squares, board.get_side_to_move(), board.get_history_ply());
    search_board = &board;
}

RavagerEval::SearchScope::~SearchScope() {
    search_board = nullptr;
}
