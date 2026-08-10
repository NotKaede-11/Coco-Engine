#include "datagen.h"
#include "board.h"
#include "types.h"
#include "movegen.h"
#include "search.h"
#include "tt.h"
#include "nnue.h"
#include "build_info.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <random>
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <memory>
#include <cmath>
#include <sstream>
#include <limits>
#include <filesystem>

// Enforce 32-byte memory packing layout required by bullet trainers
#pragma pack(push, 1)
struct BulletChessBoard {
    uint64_t occ;         // Piece occupancy bitboard map (8 bytes)
    uint8_t pcs[16];      // Squeezed piece feature indexing block (16 bytes)
    int16_t score;        // Engine evaluation score from perspective of side to move (2 bytes)
    uint8_t result;       // Final game outcome: 2 = Win (stm relative), 1 = Draw, 0 = Loss (stm relative) (1 byte)
    uint8_t ksq;          // Active side king square index (1 byte)
    uint8_t opp_ksq;      // Opponent king square index (1 byte)
    uint8_t extra[3];     // Zero-padding bytes to hit alignment target exactly (3 bytes)
};
#pragma pack(pop)

static_assert(sizeof(BulletChessBoard) == 32,
              "Bullet datagen records must remain exactly 32 bytes");

// Tracking buffers for in-flight positions prior to game termination
struct HarvestedPosition {
    BulletChessBoard packed;
    Color stm;
};

// Thread synchronization and global tracking variables
std::mutex file_mutex;
std::atomic<long long> global_positions_saved(0);
std::atomic<long long> global_total_games(0);
std::atomic<long long> global_total_game_plies(0);
std::atomic<long long> global_win_adjudications(0);
std::atomic<long long> global_draw_adjudications(0);
std::atomic<bool> datagen_failed(false);

std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (unsigned char c : value) {
        if (c == '\\') escaped += "\\\\";
        else if (c == '"') escaped += "\\\"";
        else if (c == '\n') escaped += "\\n";
        else if (c == '\r') escaped += "\\r";
        else if (c == '\t') escaped += "\\t";
        else if (c >= 0x20) escaped += static_cast<char>(c);
    }
    return escaped;
}

std::string fnv1a64(const void* bytes, size_t size) {
    const auto* data = static_cast<const uint8_t*>(bytes);
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string file_fingerprint(const std::string& path) {
    if (path.empty()) return "none";
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return "unavailable";
    uint64_t hash = 14695981039346656037ULL;
    char buffer[16384];
    while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
        const auto count = static_cast<size_t>(input.gcount());
        for (size_t i = 0; i < count; ++i) {
            hash ^= static_cast<uint8_t>(buffer[i]);
            hash *= 1099511628211ULL;
        }
    }
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

std::string datagen_configuration(const DatagenOptions& options, int threads,
                                  const std::string& engine_hash,
                                  const std::string& book_hash) {
    std::ostringstream config;
    config << "bullet-v1|record=32|seed=" << options.seed << "|threads=" << threads
           << "|buffer=" << options.buffer_positions << "|tt=" << options.tt_mb_per_worker
           << "|maxply=" << options.max_game_ply << "|engine=" << engine_hash
           << "|network=" << g_nnue.network_fingerprint() << "|book=" << book_hash
           << "|opening_pool=3|opening_depth=3|search_depth=4"
           << "|win=1000x4@ply10|draw=10x10@ply40";
    const std::string value = config.str();
    return fnv1a64(value.data(), value.size());
}

bool write_datagen_manifest(const std::string& path, const std::string& output_path,
                            const DatagenOptions& options, int threads,
                            long long target, long long records,
                            const std::string& engine_hash,
                            const std::string& book_hash,
                            const std::string& configuration,
                            const char* status) {
    const std::filesystem::path destination(path);
    std::error_code directory_error;
    if (destination.has_parent_path())
        std::filesystem::create_directories(destination.parent_path(), directory_error);
    if (directory_error) return false;
    const std::filesystem::path temporary = destination.string() + ".tmp";
    std::ofstream manifest(temporary, std::ios::trunc);
    if (!manifest.is_open()) return false;
    manifest << "{\n"
             << "  \"schema\": \"coco-datagen-manifest-v1\",\n"
             << "  \"status\": \"" << status << "\",\n"
             << "  \"configuration_fingerprint\": \"" << configuration << "\",\n"
             << "  \"output\": \"" << json_escape(output_path) << "\",\n"
             << "  \"record_schema\": \"bullet-chessboard-v1\",\n"
             << "  \"record_bytes\": 32,\n"
             << "  \"target_records\": " << target << ",\n"
             << "  \"exact_records\": " << records << ",\n"
             << "  \"seed\": " << options.seed << ",\n"
             << "  \"threads\": " << threads << ",\n"
             << "  \"buffer_positions\": " << options.buffer_positions << ",\n"
             << "  \"tt_mb_per_worker\": " << options.tt_mb_per_worker << ",\n"
             << "  \"max_game_ply\": " << options.max_game_ply << ",\n"
             << "  \"engine\": \"" << json_escape(options.engine_path) << "\",\n"
             << "  \"engine_fnv1a64\": \"" << engine_hash << "\",\n"
             << "  \"build_arch\": \"" << COCO_BUILD_ARCH << "\",\n"
             << "  \"embedded_nnue_sha256\": \"" << COCO_EMBEDDED_NNUE_SHA256 << "\",\n"
             << "  \"active_nnue\": \"" << g_nnue.network_fingerprint() << "\",\n"
             << "  \"opening_book\": "
             << (options.opening_book_path.empty() ? "null" : "\"" + json_escape(options.opening_book_path) + "\"") << ",\n"
             << "  \"opening_book_fnv1a64\": \"" << book_hash << "\",\n"
             << "  \"search\": {\"opening_pool\": 3, \"opening_depth\": 3, \"main_depth\": 4},\n"
             << "  \"adjudication\": {\"win_cp\": 1000, \"win_count\": 4, \"win_min_ply\": 10, "
                "\"draw_cp\": 10, \"draw_count\": 10, \"draw_min_ply\": 40}\n"
             << "}\n";
    manifest.close();
    if (!manifest.good()) return false;
    std::error_code error;
    std::filesystem::remove(destination, error);
    error.clear();
    std::filesystem::rename(temporary, destination, error);
    return !error;
}

bool manifest_matches_configuration(const std::string& path,
                                    const std::string& configuration) {
    std::ifstream input(path);
    if (!input.is_open()) return false;
    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    const std::string expected = "\"configuration_fingerprint\": \""
        + configuration + "\"";
    return contents.find(expected) != std::string::npos;
}

// Portable byte swapper for 64-bit integers
inline uint64_t swap_bytes(uint64_t v) {
    return ((v & 0x00000000000000FFULL) << 56) |
           ((v & 0x000000000000FF00ULL) << 40) |
           ((v & 0x0000000000FF0000ULL) << 24) |
           ((v & 0x00000000FF000000ULL) << 8)  |
           ((v & 0x000000FF00000000ULL) >> 8)  |
           ((v & 0x0000FF0000000000ULL) >> 24) |
           ((v & 0x00FF000000000000ULL) >> 40) |
           ((v & 0xFF00000000000000ULL) >> 56);
}

// Check if side to move is in check
bool in_check(const Board& board) {
    Color us = board.get_side_to_move();
    int king_sq = 0;
    uint64_t king_bb = board.get_pieces(us, KING);
    if (king_bb > 0) {
        while ((king_bb & 1) == 0) {
            king_bb >>= 1;
            king_sq++;
        }
    }
    return board.is_square_attacked(king_sq, us ^ 1);
}

// Check if there are any legal capture moves available in the position
bool has_legal_captures(const Board& board, Board& temp_board) {
    MoveList list;
    generate_pseudo_legal_moves(const_cast<Board&>(board), list);
    for (int i = 0; i < list.count; ++i) {
        Move m = list.moves[i];
        if (m.is_capture()) {
            temp_board = board;
            if (temp_board.make_move(m)) {
                return true;
            }
        }
    }
    return false;
}

// Convert internal engine Board to 32-byte BulletChessBoard
void pack_board_state(const Board& board, int16_t score, float game_result_white, BulletChessBoard& cb) {
    uint64_t bbs[8];
    bbs[0] = board.get_occupancy(WHITE);
    bbs[1] = board.get_occupancy(BLACK);
    for (int pt = 0; pt < 6; ++pt) {
        bbs[2 + pt] = board.get_pieces(WHITE, pt) | board.get_pieces(BLACK, pt);
    }

    int stm = (board.get_side_to_move() == WHITE) ? 0 : 1;
    float result = game_result_white;

    if (stm == 1) { // Black is side to move, flip vertically
        for (int i = 0; i < 8; ++i) {
            bbs[i] = swap_bytes(bbs[i]);
        }
        // Swap White and Black occupancies
        uint64_t temp = bbs[0];
        bbs[0] = bbs[1];
        bbs[1] = temp;

        result = 1.0f - result;
    }

    cb.occ = bbs[0] | bbs[1];
    std::memset(cb.pcs, 0, sizeof(cb.pcs));

    int idx = 0;
    uint64_t occ2 = cb.occ;
    while (occ2 > 0) {
        int sq = 0;
        uint64_t temp_occ = occ2;
        if (temp_occ > 0) {
            while ((temp_occ & 1) == 0) {
                temp_occ >>= 1;
                sq++;
            }
        }
        
        uint64_t bit = 1ULL << sq;
        occ2 &= occ2 - 1; // Clear LSB

        uint8_t color_bit = ((bit & bbs[1]) > 0) ? 8 : 0;
        uint8_t piece_type = 0;
        for (int pt = 0; pt < 6; ++pt) {
            if ((bit & bbs[2 + pt]) > 0) {
                piece_type = pt;
                break;
            }
        }

        uint8_t pc = color_bit | piece_type;
        cb.pcs[idx / 2] |= (pc << (4 * (idx & 1)));
        idx++;
    }

    cb.score = score;
    cb.result = (uint8_t)(2.0f * result + 0.5f);
    
    // Find king squares
    uint64_t stm_king = bbs[0] & bbs[7];
    int stm_ksq = 0;
    if (stm_king > 0) {
        while ((stm_king & 1) == 0) {
            stm_king >>= 1;
            stm_ksq++;
        }
    }
    cb.ksq = (uint8_t)stm_ksq;

    uint64_t opp_king = bbs[1] & bbs[7];
    int opp_ksq_val = 0;
    if (opp_king > 0) {
        while ((opp_king & 1) == 0) {
            opp_king >>= 1;
            opp_ksq_val++;
        }
    }
    cb.opp_ksq = (uint8_t)(opp_ksq_val ^ 56);

    cb.extra[0] = 0;
    cb.extra[1] = 0;
    cb.extra[2] = 0;
}

double datagen_soft_wdl_target(int16_t score, uint8_t result,
                               double lambda, double scale) {
    lambda = std::clamp(lambda, 0.0, 1.0);
    scale = std::max(scale, 1.0);
    const double x = std::clamp(static_cast<double>(score) / scale, -40.0, 40.0);
    const double eval_wdl = 1.0 / (1.0 + std::exp(-x));
    const double hard_wdl = std::clamp(static_cast<double>(result) / 2.0, 0.0, 1.0);
    return lambda * eval_wdl + (1.0 - lambda) * hard_wdl;
}

bool validate_datagen_file(const std::string& path, std::string* error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input.is_open()) {
        if (error) *error = "could not open file";
        return false;
    }
    const std::streamoff size = input.tellg();
    if (size < 0 || size % static_cast<std::streamoff>(sizeof(BulletChessBoard)) != 0) {
        if (error) *error = "file size is not a multiple of 32 bytes";
        return false;
    }
    input.seekg(0);
    BulletChessBoard record{};
    while (input.read(reinterpret_cast<char*>(&record), sizeof(record))) {
        if (record.result > 2 || record.ksq >= 64 || record.opp_ksq >= 64) {
            if (error) *error = "record contains an invalid result or king square";
            return false;
        }
        if (record.extra[0] || record.extra[1] || record.extra[2]) {
            if (error) *error = "record padding is non-zero";
            return false;
        }
        for (uint8_t packed : record.pcs) {
            const uint8_t lo = packed & 0x0F;
            const uint8_t hi = packed >> 4;
            const auto valid_piece = [](uint8_t piece) {
                return piece <= 5 || (piece >= 8 && piece <= 13);
            };
            if (!valid_piece(lo) || !valid_piece(hi)) {
                if (error) *error = "record contains an invalid packed piece";
                return false;
            }
        }
    }
    return input.eof();
}

std::vector<std::string> load_opening_book(const std::string& path) {
    std::vector<std::string> openings;
    if (path.empty()) return openings;
    std::ifstream input(path);
    if (!input.is_open()) return openings;

    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream stream(line);
        std::string fields[6];
        bool complete = true;
        for (std::string& field : fields)
            complete = complete && static_cast<bool>(stream >> field);
        if (!complete) continue;
        std::string fen = fields[0];
        for (int i = 1; i < 6; ++i) fen += " " + fields[i];

        Board board;
        if (!board.parse_fen(fen)) continue;
        if (count_bits(board.get_pieces(WHITE, KING)) != 1
            || count_bits(board.get_pieces(BLACK, KING)) != 1)
            continue;
        MoveList moves;
        generate_pseudo_legal_moves(board, moves);
        const LegalityMasks masks = board.get_legality_masks();
        bool has_legal_move = false;
        for (int i = 0; i < moves.count; ++i) {
            if (board.is_move_legal(moves.moves[i], masks)) {
                has_legal_move = true;
                break;
            }
        }
        if (has_legal_move) openings.push_back(std::move(fen));
    }
    return openings;
}

int fen_game_ply(const std::string& fen) {
    std::istringstream stream(fen);
    std::string board, side, castling, ep;
    int halfmove = 0;
    int fullmove = 1;
    if (!(stream >> board >> side >> castling >> ep >> halfmove >> fullmove))
        return 0;
    return std::max(0, 2 * (fullmove - 1) + (side == "b" ? 1 : 0));
}

bool flush_thread_buffer(std::vector<BulletChessBoard>& buffer,
                         long long target, const std::string& output_path) {
    if (buffer.empty()) return true;
    std::lock_guard<std::mutex> lock(file_mutex);
    const long long saved = global_positions_saved.load(std::memory_order_relaxed);
    const long long remaining = std::max(0LL, target - saved);
    const size_t write_count = std::min(buffer.size(), static_cast<size_t>(remaining));
    if (write_count == 0) {
        buffer.clear();
        return true;
    }

    std::ofstream output(output_path, std::ios::binary | std::ios::app);
    if (!output.is_open()) {
        datagen_failed.store(true, std::memory_order_relaxed);
        return false;
    }
    output.write(reinterpret_cast<const char*>(buffer.data()),
                 static_cast<std::streamsize>(write_count * sizeof(BulletChessBoard)));
    output.flush();
    if (!output.good()) {
        datagen_failed.store(true, std::memory_order_relaxed);
        return false;
    }
    global_positions_saved.fetch_add(static_cast<long long>(write_count),
                                     std::memory_order_relaxed);
    buffer.clear();
    return true;
}

void datagen_worker(long long target, const std::string& output_path, int thread_id,
                    const DatagenOptions& options,
                    const std::vector<std::string>& openings,
                    size_t flush_threshold) {
    const uint64_t stream_seed = options.seed
        + 0x9E3779B97F4A7C15ULL * static_cast<uint64_t>(thread_id + 1);
    std::mt19937_64 generator(stream_seed);

    TranspositionTable private_tt;
    private_tt.resize(options.tt_mb_per_worker);
    Search::set_thread_tt(&private_tt);

    auto board_ptr = std::make_unique<Board>();
    Board& board = *board_ptr;
    auto temp_board_ptr = std::make_unique<Board>();
    Board& temp_board = *temp_board_ptr;

    std::vector<BulletChessBoard> thread_buffer;
    thread_buffer.reserve(flush_threshold + 256);

    while (!datagen_failed.load(std::memory_order_relaxed)
           && global_positions_saved.load(std::memory_order_relaxed) < target) {
        int game_ply = 0;
        if (openings.empty()) {
            board.parse_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        } else {
            std::uniform_int_distribution<size_t> opening_dist(0, openings.size() - 1);
            const std::string& opening = openings[opening_dist(generator)];
            board.parse_fen(opening);
            game_ply = fen_game_ply(opening);
        }

        Search::reset_worker_search_state(board.get_side_to_move(), thread_id);
        private_tt.new_search();

        std::vector<HarvestedPosition> game_buffer;
        float game_result = 0.5f;
        int win_count = 0;
        int draw_count = 0;
        int predicted_winner = -1;
        bool abandon_game = false;

        while (game_ply < options.max_game_ply) {
            if (global_positions_saved.load(std::memory_order_relaxed) >= target
                || datagen_failed.load(std::memory_order_relaxed)) {
                abandon_game = true;
                break;
            }

            std::vector<Move> legal_moves;
            MoveList list;
            generate_pseudo_legal_moves(board, list);
            for (int i = 0; i < list.count; ++i) {
                const Move move = list.moves[i];
                if (board.make_move(move)) {
                    legal_moves.push_back(move);
                    board.unmake_move(move);
                }
            }

            if (legal_moves.empty()) {
                game_result = in_check(board)
                    ? (board.get_side_to_move() == WHITE ? 0.0f : 1.0f)
                    : 0.5f;
                break;
            }
            if (board.is_repetition() || board.get_halfmove_clock() >= 100
                || count_bits(board.get_occupancy(BOTH)) == 2) {
                game_result = 0.5f;
                break;
            }

            Move chosen_move;
            int eval_score = 0;
            if (game_ply < 8) {
                std::vector<std::pair<Move, int>> scored_moves;
                scored_moves.reserve(legal_moves.size());
                for (Move move : legal_moves) {
                    temp_board = board;
                    if (!temp_board.make_move(move)) continue;
                    const int move_score = -alpha_beta(temp_board, -INFINITY_SCORE,
                        INFINITY_SCORE, 3, 1, Search::NodeType::NON_PV, false);
                    scored_moves.push_back({move, move_score});
                }
                if (scored_moves.empty()) {
                    datagen_failed.store(true, std::memory_order_relaxed);
                    abandon_game = true;
                    break;
                }
                std::stable_sort(scored_moves.begin(), scored_moves.end(),
                    [](const auto& lhs, const auto& rhs) {
                        return lhs.second > rhs.second;
                    });
                const size_t pool = std::min<size_t>(3, scored_moves.size());
                std::uniform_int_distribution<size_t> choice(0, pool - 1);
                const auto& selected = scored_moves[choice(generator)];
                chosen_move = selected.first;
                eval_score = selected.second;
            } else {
                eval_score = alpha_beta(board, -INFINITY_SCORE, INFINITY_SCORE,
                                        4, 0, Search::NodeType::PV, false);
                int tt_score = 0;
                private_tt.probe(board.get_hash_key(), tt_score, chosen_move, 4,
                                 -INFINITY_SCORE, INFINITY_SCORE, 0);
            }

            if (std::find(legal_moves.begin(), legal_moves.end(), chosen_move)
                == legal_moves.end())
                chosen_move = legal_moves.front();

            if (game_ply >= 8 && !in_check(board)
                && !has_legal_captures(board, temp_board)) {
                BulletChessBoard packed{};
                pack_board_state(board, static_cast<int16_t>(std::clamp(
                    eval_score, static_cast<int>(std::numeric_limits<int16_t>::min()),
                    static_cast<int>(std::numeric_limits<int16_t>::max()))),
                    0.5f, packed);
                game_buffer.push_back({packed, board.get_side_to_move()});
            }

            if (game_ply >= 10 && std::abs(eval_score) > 1000) {
                const int winner = eval_score > 0
                    ? (board.get_side_to_move() == WHITE ? 1 : 0)
                    : (board.get_side_to_move() == WHITE ? 0 : 1);
                if (winner == predicted_winner) ++win_count;
                else {
                    predicted_winner = winner;
                    win_count = 1;
                }
                if (win_count >= 4) {
                    game_result = winner ? 1.0f : 0.0f;
                    global_win_adjudications.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            } else {
                win_count = 0;
                predicted_winner = -1;
            }

            if (game_ply >= 40 && std::abs(eval_score) < 10) {
                if (++draw_count >= 10) {
                    game_result = 0.5f;
                    global_draw_adjudications.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            } else {
                draw_count = 0;
            }

            if (!board.make_move(chosen_move)) {
                datagen_failed.store(true, std::memory_order_relaxed);
                abandon_game = true;
                break;
            }
            ++game_ply;
        }

        global_total_games.fetch_add(1, std::memory_order_relaxed);
        global_total_game_plies.fetch_add(game_ply, std::memory_order_relaxed);
        if (!abandon_game) {
            for (HarvestedPosition& harvested : game_buffer) {
                float result = game_result;
                if (harvested.stm == BLACK) result = 1.0f - result;
                harvested.packed.result = static_cast<uint8_t>(2.0f * result + 0.5f);
                thread_buffer.push_back(harvested.packed);
            }
        }

        if (thread_buffer.size() >= flush_threshold
            && !flush_thread_buffer(thread_buffer, target, output_path))
            break;
    }

    flush_thread_buffer(thread_buffer, target, output_path);
    Search::set_thread_tt(nullptr);
}

bool run_datagen(long long target_positions, int num_threads,
                 const std::string& output_path, const DatagenOptions& options) {
    if (target_positions <= 0 || num_threads <= 0 || num_threads > MAX_THREADS
        || output_path.empty() || options.buffer_positions == 0
        || options.tt_mb_per_worker == 0 || options.max_game_ply < 20) {
        std::cerr << "[Datagen] Invalid target, thread count, path, buffer, TT, or ply limit.\n";
        return false;
    }

    const auto start_time = std::chrono::high_resolution_clock::now();
    long long existing_positions = 0;
    std::ifstream existing(output_path, std::ios::binary | std::ios::ate);
    if (existing.is_open()) {
        const std::streamoff size = existing.tellg();
        if (size < 0 || size % static_cast<std::streamoff>(sizeof(BulletChessBoard)) != 0) {
            std::cerr << "[Datagen] Refusing misaligned output file (record size is 32 bytes).\n";
            return false;
        }
        existing_positions = size / sizeof(BulletChessBoard);
    } else {
        std::ofstream create(output_path, std::ios::binary | std::ios::app);
        if (!create.is_open()) {
            std::cerr << "[Datagen] Could not create output file.\n";
            return false;
        }
    }

    const std::vector<std::string> openings = load_opening_book(options.opening_book_path);
    if (!options.opening_book_path.empty() && openings.empty()) {
        std::cerr << "[Datagen] Opening book contained no valid FEN records.\n";
        return false;
    }

    const std::string manifest_path = options.manifest_path.empty()
        ? output_path + ".manifest.json"
        : options.manifest_path;
    const std::string engine_hash = file_fingerprint(options.engine_path);
    const std::string book_hash = file_fingerprint(options.opening_book_path);
    const std::string configuration = datagen_configuration(
        options, num_threads, engine_hash, book_hash);

    if (existing_positions > 0
        && !manifest_matches_configuration(manifest_path, configuration)) {
        std::cerr << "[Datagen] Refusing to append: the existing data has no compatible "
                     "provenance manifest. Start a new output or restore its matching manifest.\n";
        return false;
    }

    global_positions_saved.store(existing_positions, std::memory_order_relaxed);
    global_total_games.store(0, std::memory_order_relaxed);
    global_total_game_plies.store(0, std::memory_order_relaxed);
    global_win_adjudications.store(0, std::memory_order_relaxed);
    global_draw_adjudications.store(0, std::memory_order_relaxed);
    datagen_failed.store(false, std::memory_order_relaxed);

    std::cout << "[Datagen] Target: " << target_positions
              << " | Threads: " << num_threads
              << " | Seed: " << options.seed
              << " | Buffer: " << options.buffer_positions
              << " | Private TT: " << options.tt_mb_per_worker << " MB"
              << " | Book positions: " << openings.size() << '\n';

    if (!write_datagen_manifest(manifest_path, output_path, options, num_threads,
                                target_positions, existing_positions, engine_hash,
                                book_hash, configuration, "running")) {
        std::cerr << "[Datagen] Could not write provenance manifest: "
                  << manifest_path << '\n';
        return false;
    }

    if (existing_positions >= target_positions) {
        std::cout << "[Datagen] Target already achieved at " << existing_positions << " positions.\n";
        write_datagen_manifest(manifest_path, output_path, options, num_threads,
                               target_positions, existing_positions, engine_hash,
                               book_hash, configuration, "complete");
        return true;
    }

    Search::target_time = 0;
    Search::soft_limit = 0;
    Search::hard_limit = 0;
    Search::node_limit = 0;
    Search::time_check_mask = 1023;
    Search::b_abort.store(false, std::memory_order_relaxed);

    const long long remaining = target_positions - existing_positions;
    const size_t flush_threshold = std::max<size_t>(1, std::min<size_t>(
        options.buffer_positions,
        static_cast<size_t>((remaining + num_threads - 1) / num_threads)));

    std::vector<std::thread> workers;
    workers.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        workers.emplace_back(datagen_worker, target_positions,
                             std::cref(output_path), i, std::cref(options),
                             std::cref(openings), flush_threshold);
    }

    long long last_reported = existing_positions;
    while (!datagen_failed.load(std::memory_order_relaxed)
           && global_positions_saved.load(std::memory_order_relaxed) < target_positions) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const long long current = global_positions_saved.load(std::memory_order_relaxed);
        if (current >= last_reported + 100000 || current == target_positions) {
            const std::chrono::duration<double> elapsed
                = std::chrono::high_resolution_clock::now() - start_time;
            const double speed = (current - existing_positions) / elapsed.count();
            const double progress = 100.0 * current / target_positions;
            std::cout << "[Datagen Progress] " << current << " / " << target_positions
                      << " | " << static_cast<long long>(speed) << " pos/sec"
                      << " | " << std::fixed << std::setprecision(2) << progress << "%\n";
            last_reported = current;
        }
    }

    for (std::thread& worker : workers)
        if (worker.joinable()) worker.join();

    if (datagen_failed.load(std::memory_order_relaxed)) {
        std::cerr << "[Datagen] Worker failure; output stopped at "
                  << global_positions_saved.load(std::memory_order_relaxed) << " records.\n";
        write_datagen_manifest(manifest_path, output_path, options, num_threads,
                               target_positions, global_positions_saved.load(), engine_hash,
                               book_hash, configuration, "failed");
        return false;
    }

    std::string validation_error;
    if (!validate_datagen_file(output_path, &validation_error)) {
        std::cerr << "[Datagen] Record validation failed: " << validation_error << '\n';
        write_datagen_manifest(manifest_path, output_path, options, num_threads,
                               target_positions, global_positions_saved.load(), engine_hash,
                               book_hash, configuration, "failed");
        return false;
    }

    const std::chrono::duration<double> elapsed
        = std::chrono::high_resolution_clock::now() - start_time;
    const long long games = global_total_games.load(std::memory_order_relaxed);
    const double average_ply = games > 0
        ? static_cast<double>(global_total_game_plies.load(std::memory_order_relaxed)) / games
        : 0.0;
    const double speed = (global_positions_saved.load(std::memory_order_relaxed)
                         - existing_positions) / std::max(0.001, elapsed.count());

    std::cout << "[Datagen Complete] " << global_positions_saved.load(std::memory_order_relaxed)
              << " records | " << static_cast<long long>(speed) << " pos/sec"
              << " | Games: " << games << " | Avg ply: " << std::fixed
              << std::setprecision(2) << average_ply
              << " | W/L adjudications: " << global_win_adjudications.load()
              << " | Draw adjudications: " << global_draw_adjudications.load() << '\n';
    if (!write_datagen_manifest(manifest_path, output_path, options, num_threads,
                                target_positions, global_positions_saved.load(), engine_hash,
                                book_hash, configuration, "complete")) {
        std::cerr << "[Datagen] Data completed but final provenance manifest could not be written.\n";
        return false;
    }
    return true;
}
