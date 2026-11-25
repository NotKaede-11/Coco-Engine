#pragma once

#include <filesystem>

class Board;

namespace RavagerEval {
bool load(const std::filesystem::path& path);
bool load_embedded();
int raw(const Board& board);
int display(const Board& board);
void before_move(const Board& board, int ply);

class SearchScope {
public:
    explicit SearchScope(const Board& board);
    ~SearchScope();
    SearchScope(const SearchScope&) = delete;
    SearchScope& operator=(const SearchScope&) = delete;
};
}
