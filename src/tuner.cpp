/* Coco production-trace HCE tuner. T6 search parameters are intentionally excluded. */
#include "board.h"
#include "evaluate.h"
#include "movegen.h"
#include "nnue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr double TEXEL_K = 1.603;
constexpr double WEIGHT_SCALE = 1024.0;
using FeatureVector = std::array<double, Evaluation::HCE_FEATURE_COUNT>;

struct Sample {
    FeatureVector mg{};
    FeatureVector eg{};
    int phase = 0;
    int scale = 128;
    bool exact_draw = false;
    bool white_to_move = true;
    double result = 0.5;
};

struct Parameter {
    double mg = WEIGHT_SCALE;
    double eg = WEIGHT_SCALE;
    double accumulator_mg = 1e-8;
    double accumulator_eg = 1e-8;
};

struct TunerState {
    std::array<Parameter, Evaluation::HCE_FEATURE_COUNT> parameter{};
    int completed_epochs = 0;
    uint64_t seed = 0xC0C05EEDULL;
};

struct Options {
    std::filesystem::path dataset;
    std::filesystem::path checkpoint;
    std::filesystem::path resume;
    std::filesystem::path export_path;
    std::filesystem::path import_path;
    int epochs = 200;
    int threads = 1;
    int batch_size = 4096;
    double learning_rate = 0.5;
    double validation_fraction = 0.1;
    uint64_t seed = 0xC0C05EEDULL;
    bool self_test = false;
};

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool validate_fen_syntax(const std::string& fen, std::string& reason) {
    std::istringstream input(fen);
    std::string placement, side, castling, ep, halfmove, fullmove, extra;
    if (!(input >> placement >> side >> castling >> ep >> halfmove >> fullmove) || (input >> extra)) {
        reason = "FEN must contain exactly six fields";
        return false;
    }
    int ranks = 1, files = 0, white_kings = 0, black_kings = 0;
    for (char ch : placement) {
        if (ch == '/') {
            if (files != 8) { reason = "FEN rank width is not eight"; return false; }
            ++ranks; files = 0; continue;
        }
        if (ch >= '1' && ch <= '8') files += ch - '0';
        else if (std::string("PNBRQKpnbrqk").find(ch) != std::string::npos) {
            ++files;
            white_kings += ch == 'K';
            black_kings += ch == 'k';
        } else { reason = "FEN contains an invalid piece character"; return false; }
        if (files > 8) { reason = "FEN rank is wider than eight"; return false; }
    }
    if (ranks != 8 || files != 8) { reason = "FEN must contain eight complete ranks"; return false; }
    if (white_kings != 1 || black_kings != 1) { reason = "FEN must contain one king per side"; return false; }
    if (side != "w" && side != "b") { reason = "invalid side-to-move field"; return false; }
    if (castling != "-") for (char ch : castling)
        if (std::string("KQkq").find(ch) == std::string::npos) { reason = "invalid castling field"; return false; }
    if (ep != "-" && (ep.size() != 2 || ep[0] < 'a' || ep[0] > 'h'
        || (ep[1] != '3' && ep[1] != '6'))) { reason = "invalid en-passant field"; return false; }
    try {
        size_t used = 0;
        const long long half = std::stoll(halfmove, &used);
        if (used != halfmove.size() || half < 0 || half > 1000000) throw std::out_of_range("halfmove");
        const long long full = std::stoll(fullmove, &used);
        if (used != fullmove.size() || full < 1 || full > 1000000) throw std::out_of_range("fullmove");
    } catch (...) { reason = "invalid move counters"; return false; }
    return true;
}

Sample sample_from_board(const Board& board, double result) {
    Evaluation::HceTrace trace;
    Evaluation::evaluate_hce(board, &trace);
    Sample sample;
    sample.mg = {double(trace.material_mg), double(trace.psqt_mg), double(trace.pawns_mg),
                 double(trace.imbalance_mg), double(trace.passers_mg), double(trace.threats_mg),
                 double(trace.mobility_mg), double(trace.king_safety_mg), double(trace.endgame_mg),
                 double(trace.tempo_mg)};
    sample.eg = {double(trace.material_eg), double(trace.psqt_eg), double(trace.pawns_eg),
                 double(trace.imbalance_eg), double(trace.passers_eg), double(trace.threats_eg),
                 double(trace.mobility_eg), double(trace.king_safety_eg), double(trace.endgame_eg),
                 double(trace.tempo_eg)};
    sample.phase = trace.phase;
    sample.scale = trace.scale;
    sample.exact_draw = trace.exact_draw;
    sample.white_to_move = board.get_side_to_move() == WHITE;
    // Dataset labels are White-relative; production evaluations are relative
    // to the side to move.
    sample.result = sample.white_to_move ? result : 1.0 - result;
    return sample;
}

bool load_dataset(const std::filesystem::path& path, std::vector<Sample>& samples) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open dataset: " + path.string());
    std::string line;
    size_t line_number = 0, rejected = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto separator = line.rfind('|');
        if (separator == std::string::npos) {
            std::cerr << "reject line " << line_number << ": missing result separator\n";
            ++rejected; continue;
        }
        const std::string fen = trim(line.substr(0, separator));
        const std::string result_text = trim(line.substr(separator + 1));
        std::string reason;
        if (!validate_fen_syntax(fen, reason)) {
            std::cerr << "reject line " << line_number << ": " << reason << '\n';
            ++rejected; continue;
        }
        double result = 0.0;
        try {
            size_t used = 0;
            result = std::stod(result_text, &used);
            if (used != result_text.size() || !std::isfinite(result) || result < 0.0 || result > 1.0)
                throw std::out_of_range("result");
        } catch (...) {
            std::cerr << "reject line " << line_number << ": result must be within [0,1]\n";
            ++rejected; continue;
        }
        Board board;
        board.parse_fen(fen);
        const U64 back_ranks = 0xFF000000000000FFULL;
        if ((board.get_pieces(WHITE, PAWN) | board.get_pieces(BLACK, PAWN)) & back_ranks) {
            std::cerr << "reject line " << line_number << ": pawn on promotion rank\n";
            ++rejected; continue;
        }
        samples.push_back(sample_from_board(board, result));
    }
    std::cerr << "loaded=" << samples.size() << " rejected=" << rejected << '\n';
    return !samples.empty();
}

double sigmoid(double evaluation) {
    const double x = std::clamp(TEXEL_K * evaluation / 400.0, -40.0, 40.0);
    return 1.0 / (1.0 + std::exp(-x));
}

double score(const Sample& sample, const TunerState& state) {
    if (sample.exact_draw) return 0.0;
    double mg = 0.0, eg = 0.0;
    for (int feature = 0; feature < Evaluation::HCE_FEATURE_COUNT; ++feature) {
        mg += sample.mg[feature] * state.parameter[feature].mg / WEIGHT_SCALE;
        eg += sample.eg[feature] * state.parameter[feature].eg / WEIGHT_SCALE;
    }
    double value = (mg * sample.phase + eg * (24 - sample.phase)) / 24.0;
    value *= sample.scale / 128.0;
    return sample.white_to_move ? value : -value;
}

double loss(const std::vector<Sample>& samples, const std::vector<size_t>& indices,
            const TunerState& state) {
    if (indices.empty()) return std::numeric_limits<double>::quiet_NaN();
    double total = 0.0;
    for (size_t index : indices) {
        const double error = sigmoid(score(samples[index], state)) - samples[index].result;
        total += error * error;
    }
    return total / indices.size();
}

struct Gradient {
    FeatureVector mg{};
    FeatureVector eg{};
};

void accumulate_gradient(const Sample& sample, const TunerState& state, Gradient& gradient) {
    if (sample.exact_draw) return;
    const double prediction = sigmoid(score(sample, state));
    const double dloss_deval = 2.0 * (prediction - sample.result) * prediction
        * (1.0 - prediction) * TEXEL_K / 400.0;
    const double perspective = sample.white_to_move ? 1.0 : -1.0;
    const double scale = sample.scale / 128.0;
    for (int feature = 0; feature < Evaluation::HCE_FEATURE_COUNT; ++feature) {
        gradient.mg[feature] += dloss_deval * perspective * scale
            * sample.phase / 24.0 * sample.mg[feature] / WEIGHT_SCALE;
        gradient.eg[feature] += dloss_deval * perspective * scale
            * (24 - sample.phase) / 24.0 * sample.eg[feature] / WEIGHT_SCALE;
    }
}

Gradient batch_gradient(const std::vector<Sample>& samples, const std::vector<size_t>& order,
                        size_t begin, size_t end, const TunerState& state, int thread_count) {
    const int workers = std::max(1, std::min<int>(thread_count, static_cast<int>(end - begin)));
    std::vector<Gradient> local(workers);
    std::vector<std::thread> threads;
    for (int worker = 0; worker < workers; ++worker) {
        const size_t first = begin + (end - begin) * worker / workers;
        const size_t last = begin + (end - begin) * (worker + 1) / workers;
        threads.emplace_back([&, worker, first, last] {
            for (size_t offset = first; offset < last; ++offset)
                accumulate_gradient(samples[order[offset]], state, local[worker]);
        });
    }
    for (auto& thread : threads) thread.join();
    Gradient result;
    for (const auto& part : local)
        for (int feature = 0; feature < Evaluation::HCE_FEATURE_COUNT; ++feature) {
            result.mg[feature] += part.mg[feature];
            result.eg[feature] += part.eg[feature];
        }
    const double inverse = 1.0 / double(end - begin);
    for (int feature = 0; feature < Evaluation::HCE_FEATURE_COUNT; ++feature) {
        result.mg[feature] *= inverse;
        result.eg[feature] *= inverse;
    }
    return result;
}

Evaluation::HceWeights integer_weights(const TunerState& state) {
    Evaluation::HceWeights weights{};
    for (int feature = 0; feature < Evaluation::HCE_FEATURE_COUNT; ++feature) {
        const int mg = std::clamp<int>(int(std::llround(state.parameter[feature].mg)), -32768, 32767);
        const int eg = std::clamp<int>(int(std::llround(state.parameter[feature].eg)), -32768, 32767);
        weights.feature[feature] = Evaluation::Score(mg, eg);
    }
    return weights;
}

int integer_score(const Sample& sample, const Evaluation::HceWeights& weights) {
    if (sample.exact_draw) return 0;
    int64_t weighted_mg = 0, weighted_eg = 0;
    for (int feature = 0; feature < Evaluation::HCE_FEATURE_COUNT; ++feature) {
        weighted_mg += static_cast<int64_t>(sample.mg[feature]) * weights.feature[feature].mg;
        weighted_eg += static_cast<int64_t>(sample.eg[feature]) * weights.feature[feature].eg;
    }
    const int mg = static_cast<int>(weighted_mg / 1024);
    const int eg = static_cast<int>(weighted_eg / 1024);
    const int white = ((mg * sample.phase + eg * (24 - sample.phase)) / 24)
        * sample.scale / 128;
    return sample.white_to_move ? white : -white;
}

void export_weights(std::ostream& output, const TunerState& state) {
    output << "COCO_HCE_WEIGHTS 1\n";
    output << std::setprecision(17);
    for (int feature = 0; feature < Evaluation::HCE_FEATURE_COUNT; ++feature)
        output << Evaluation::HCE_FEATURE_NAMES[feature] << ' '
               << state.parameter[feature].mg << ' ' << state.parameter[feature].eg << '\n';
}

void import_weights(std::istream& input, TunerState& state) {
    std::string magic;
    int version = 0;
    if (!(input >> magic >> version) || magic != "COCO_HCE_WEIGHTS" || version != 1)
        throw std::runtime_error("invalid HCE weight file header");
    std::array<bool, Evaluation::HCE_FEATURE_COUNT> seen{};
    std::string name;
    double mg = 0.0, eg = 0.0;
    while (input >> name >> mg >> eg) {
        int index = -1;
        for (int feature = 0; feature < Evaluation::HCE_FEATURE_COUNT; ++feature)
            if (name == Evaluation::HCE_FEATURE_NAMES[feature]) index = feature;
        if (index < 0 || seen[index] || !std::isfinite(mg) || !std::isfinite(eg))
            throw std::runtime_error("invalid or duplicate HCE weight: " + name);
        state.parameter[index].mg = mg;
        state.parameter[index].eg = eg;
        seen[index] = true;
    }
    if (std::find(seen.begin(), seen.end(), false) != seen.end())
        throw std::runtime_error("HCE weight file is incomplete");
}

void save_checkpoint(const std::filesystem::path& path, const TunerState& state) {
    const auto temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write checkpoint: " + temporary);
    output << "COCO_TUNER_CHECKPOINT 1\n" << state.completed_epochs << ' ' << state.seed << '\n';
    output << std::setprecision(17);
    for (int feature = 0; feature < Evaluation::HCE_FEATURE_COUNT; ++feature) {
        const auto& p = state.parameter[feature];
        output << Evaluation::HCE_FEATURE_NAMES[feature] << ' ' << p.mg << ' ' << p.eg << ' '
               << p.accumulator_mg << ' ' << p.accumulator_eg << '\n';
    }
    output.close();
    if (!output) throw std::runtime_error("checkpoint write failed: " + temporary);
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::rename(temporary, path);
}

void load_checkpoint(const std::filesystem::path& path, TunerState& state) {
    std::ifstream input(path);
    std::string magic;
    int version = 0;
    if (!(input >> magic >> version) || magic != "COCO_TUNER_CHECKPOINT" || version != 1)
        throw std::runtime_error("invalid tuner checkpoint header");
    if (!(input >> state.completed_epochs >> state.seed) || state.completed_epochs < 0)
        throw std::runtime_error("invalid tuner checkpoint metadata");
    std::array<bool, Evaluation::HCE_FEATURE_COUNT> seen{};
    std::string name;
    double mg, eg, amg, aeg;
    while (input >> name >> mg >> eg >> amg >> aeg) {
        int index = -1;
        for (int feature = 0; feature < Evaluation::HCE_FEATURE_COUNT; ++feature)
            if (name == Evaluation::HCE_FEATURE_NAMES[feature]) index = feature;
        if (index < 0 || seen[index] || !std::isfinite(mg) || !std::isfinite(eg)
            || !std::isfinite(amg) || !std::isfinite(aeg) || amg <= 0.0 || aeg <= 0.0)
            throw std::runtime_error("invalid tuner checkpoint parameter");
        state.parameter[index] = {mg, eg, amg, aeg};
        seen[index] = true;
    }
    if (std::find(seen.begin(), seen.end(), false) != seen.end())
        throw std::runtime_error("tuner checkpoint is incomplete");
}

void update(TunerState& state, const Gradient& gradient, double learning_rate) {
    for (int feature = 0; feature < Evaluation::HCE_FEATURE_COUNT; ++feature) {
        auto& parameter = state.parameter[feature];
        parameter.accumulator_mg += gradient.mg[feature] * gradient.mg[feature];
        parameter.accumulator_eg += gradient.eg[feature] * gradient.eg[feature];
        parameter.mg -= learning_rate * gradient.mg[feature] / std::sqrt(parameter.accumulator_mg);
        parameter.eg -= learning_rate * gradient.eg[feature] / std::sqrt(parameter.accumulator_eg);
    }
}

bool finite_difference_test(const Sample& sample) {
    TunerState state;
    Gradient analytic;
    accumulate_gradient(sample, state, analytic);
    constexpr double epsilon = 1e-3;
    for (int feature = 0; feature < Evaluation::HCE_FEATURE_COUNT; ++feature) {
        for (int component = 0; component < 2; ++component) {
            double& value = component == 0 ? state.parameter[feature].mg : state.parameter[feature].eg;
            value += epsilon;
            const double plus_error = sigmoid(score(sample, state)) - sample.result;
            const double plus = plus_error * plus_error;
            value -= 2.0 * epsilon;
            const double minus_error = sigmoid(score(sample, state)) - sample.result;
            const double minus = minus_error * minus_error;
            value += epsilon;
            const double numerical = (plus - minus) / (2.0 * epsilon);
            const double expected = component == 0 ? analytic.mg[feature] : analytic.eg[feature];
            if (std::abs(numerical - expected) > 2e-8 + 2e-5 * std::abs(numerical)) {
                std::cerr << "gradient mismatch " << Evaluation::HCE_FEATURE_NAMES[feature]
                          << (component == 0 ? ".mg" : ".eg") << " numerical=" << numerical
                          << " analytic=" << expected << '\n';
                return false;
            }
        }
    }
    return true;
}

bool self_test() {
    const char* fens[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "4k3/8/8/8/3Q4/8/8/4K3 w - - 0 1",
        "4k3/8/8/8/3q4/8/8/4K3 b - - 0 1",
        "r3k2r/pp1n1p1p/2pbpn2/q5p1/2BPP3/2N2N2/PPQ2PPP/R4RK1 w kq - 3 11"
    };
    const double results[] = {0.5, 1.0, 0.0, 0.7};
    std::vector<Sample> samples;
    std::vector<Board> boards;
    for (int index = 0; index < 4; ++index) {
        Board board;
        board.parse_fen(fens[index]);
        samples.push_back(sample_from_board(board, results[index]));
        boards.push_back(board);
    }
    if (!finite_difference_test(samples.back())) return false;

    TunerState state;
    state.parameter[Evaluation::HCE_MOBILITY].mg = 997.0;
    state.parameter[Evaluation::HCE_KING_SAFETY].eg = 1061.0;
    std::stringstream serialized;
    export_weights(serialized, state);
    TunerState imported;
    import_weights(serialized, imported);
    for (int feature = 0; feature < Evaluation::HCE_FEATURE_COUNT; ++feature)
        if (state.parameter[feature].mg != imported.parameter[feature].mg
            || state.parameter[feature].eg != imported.parameter[feature].eg) return false;

    const Evaluation::HceWeights weights = integer_weights(imported);
    for (size_t index = 0; index < boards.size(); ++index) {
        Evaluation::HceTrace trace;
        const int production = Evaluation::evaluate_hce(boards[index], &trace, weights);
        Sample exact = sample_from_board(boards[index], results[index]);
        const int reconstructed = integer_score(exact, weights);
        if (production != reconstructed) {
            std::cerr << "production/tuner trace mismatch: " << production << " versus " << reconstructed << '\n';
            return false;
        }
    }

    std::vector<size_t> indices(samples.size());
    std::iota(indices.begin(), indices.end(), 0);
    TunerState learning;
    const double before = loss(samples, indices, learning);
    for (int step = 0; step < 20; ++step)
        update(learning, batch_gradient(samples, indices, 0, indices.size(), learning, 2), 4.0);
    const double after = loss(samples, indices, learning);
    if (!(after < before)) {
        std::cerr << "smoke training failed to improve held-out fixture loss\n";
        return false;
    }

    const auto checkpoint = std::filesystem::temp_directory_path() / "coco_tuner_selftest.chk";
    learning.completed_epochs = 7;
    save_checkpoint(checkpoint, learning);
    TunerState resumed;
    load_checkpoint(checkpoint, resumed);
    std::error_code error;
    std::filesystem::remove(checkpoint, error);
    if (resumed.completed_epochs != learning.completed_epochs
        || resumed.parameter[0].accumulator_mg != learning.parameter[0].accumulator_mg) return false;
    std::cout << "PASS: production trace parity, gradients, split-loss update, weight round-trip, checkpoint/resume\n";
    return true;
}

Options parse_options(int argc, char** argv) {
    Options options;
    auto require_value = [&](int& index, const char* name) -> std::string {
        if (++index >= argc) throw std::runtime_error(std::string("missing value for ") + name);
        return argv[index];
    };
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--self-test") options.self_test = true;
        else if (argument == "--dataset") options.dataset = require_value(index, "--dataset");
        else if (argument == "--epochs") options.epochs = std::stoi(require_value(index, "--epochs"));
        else if (argument == "--threads") options.threads = std::stoi(require_value(index, "--threads"));
        else if (argument == "--batch-size") options.batch_size = std::stoi(require_value(index, "--batch-size"));
        else if (argument == "--lr") options.learning_rate = std::stod(require_value(index, "--lr"));
        else if (argument == "--validation") options.validation_fraction = std::stod(require_value(index, "--validation"));
        else if (argument == "--seed") options.seed = std::stoull(require_value(index, "--seed"));
        else if (argument == "--checkpoint") options.checkpoint = require_value(index, "--checkpoint");
        else if (argument == "--resume") options.resume = require_value(index, "--resume");
        else if (argument == "--export") options.export_path = require_value(index, "--export");
        else if (argument == "--import") options.import_path = require_value(index, "--import");
        else throw std::runtime_error("unknown argument: " + argument);
    }
    if (!options.self_test && options.dataset.empty()) throw std::runtime_error("--dataset is required");
    if (options.epochs < 0 || options.threads < 1 || options.threads > MAX_THREADS
        || options.batch_size < 1 || !std::isfinite(options.learning_rate) || options.learning_rate <= 0.0
        || !std::isfinite(options.validation_fraction) || options.validation_fraction < 0.0
        || options.validation_fraction >= 0.5)
        throw std::runtime_error("invalid numeric option");
    if (!options.resume.empty() && !options.import_path.empty())
        throw std::runtime_error("--resume and --import are mutually exclusive");
    return options;
}

void print_usage() {
    std::cerr << "usage: tuner --dataset FILE [--epochs N] [--threads N] [--batch-size N] [--lr X]\n"
                 "             [--validation X] [--seed N] [--checkpoint FILE] [--resume FILE]\n"
                 "             [--import FILE] [--export FILE]\n"
                 "       tuner --self-test\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        Board::init_zobrist();
        init_all_attack_tables();
        if (!g_nnue.load_network("coco.nnue")) throw std::runtime_error("failed to load coco.nnue");
        if (options.self_test) return self_test() ? 0 : 1;

        TunerState state;
        state.seed = options.seed;
        if (!options.resume.empty()) load_checkpoint(options.resume, state);
        if (!options.import_path.empty()) {
            std::ifstream input(options.import_path);
            if (!input) throw std::runtime_error("cannot open weight import");
            import_weights(input, state);
        }

        std::vector<Sample> samples;
        if (!load_dataset(options.dataset, samples)) throw std::runtime_error("dataset has no valid positions");
        std::vector<size_t> order(samples.size());
        std::iota(order.begin(), order.end(), 0);
        // A resumed run must reconstruct the exact original split even when
        // the caller does not repeat --seed on the command line.
        std::mt19937_64 split_generator(state.seed);
        std::shuffle(order.begin(), order.end(), split_generator);
        size_t validation_count = static_cast<size_t>(std::llround(samples.size() * options.validation_fraction));
        if (options.validation_fraction > 0.0 && samples.size() > 1) validation_count = std::max<size_t>(1, validation_count);
        validation_count = std::min(validation_count, samples.size() - 1);
        std::vector<size_t> validation(order.begin(), order.begin() + validation_count);
        std::vector<size_t> training(order.begin() + validation_count, order.end());
        std::cerr << "train=" << training.size() << " validation=" << validation.size()
                  << " start_epoch=" << state.completed_epochs << '\n';

        for (int run_epoch = 0; run_epoch < options.epochs; ++run_epoch) {
            const int epoch = state.completed_epochs;
            std::mt19937_64 generator(state.seed + static_cast<uint64_t>(epoch));
            std::vector<size_t> epoch_order = training;
            std::shuffle(epoch_order.begin(), epoch_order.end(), generator);
            for (size_t begin = 0; begin < epoch_order.size(); begin += options.batch_size) {
                const size_t end = std::min(epoch_order.size(), begin + static_cast<size_t>(options.batch_size));
                update(state, batch_gradient(samples, epoch_order, begin, end, state, options.threads),
                       options.learning_rate);
            }
            ++state.completed_epochs;
            const double train_loss = loss(samples, training, state);
            const double validation_loss = loss(samples, validation, state);
            std::cerr << "epoch=" << state.completed_epochs << " train_loss=" << std::setprecision(10)
                      << train_loss << " validation_loss=" << validation_loss << '\n';
            if (!options.checkpoint.empty()) save_checkpoint(options.checkpoint, state);
        }

        if (!options.export_path.empty()) {
            std::ofstream output(options.export_path, std::ios::trunc);
            if (!output) throw std::runtime_error("cannot write weight export");
            export_weights(output, state);
        } else export_weights(std::cout, state);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        print_usage();
        return 2;
    }
}
