#include "bitboard.h"
#include "inference.h"

static _Thread_local Board position;

static void set_board(const uint64_t *pieces, const uint8_t *squares, int side, int ply) {
    memcpy(position.pieces, pieces, sizeof position.pieces);
    memcpy(position.piece_on, squares, sizeof position.piece_on);
    position.occupancy[0] = position.occupancy[1] = 0;
    for (int color = 0; color < 2; ++color)
        for (int type = 0; type < 6; ++type)
            position.occupancy[color] |= position.pieces[color][type];
    position.occ_all = position.occupancy[0] | position.occupancy[1];
    position.side = side;
    position.ply = ply;
}

int rv_install(const unsigned char *data, size_t size) {
    init_bitboards();
    return net_load(data, size);
}

void rv_prepare(const uint64_t *pieces, const uint8_t *squares, int side, int ply) {
    set_board(pieces, squares, side, ply);
    net_prepare(&position);
}

void rv_push(int ply) {
    position.ply = ply;
    net_push(&position);
}

int rv_eval(const uint64_t *pieces, const uint8_t *squares, int side, int ply, int fresh) {
    set_board(pieces, squares, side, ply);
    return net_eval(&position, fresh);
}
