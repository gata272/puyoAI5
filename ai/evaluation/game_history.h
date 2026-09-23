#pragma once

#include "../simulation/board.h"

#include <array>
#include <cstdint>

namespace puyo {

enum class PolicyMode {
    Build,
    Tension,
    Recover,
    Rebuild,
};

const char* policyModeName(PolicyMode mode);

struct GameHistory {
    static constexpr int kWindow = 12;
    static constexpr int kMeaningfulClearPuyos = 8;
    static constexpr int kRebuildMaxAge = 14;

    int turn = -1;
    int lastActualChain = 0;
    int chainAge = 0;

    int lastBigChain = 0;
    int postBigChainAge = 99;

    // `quietTurns` tracks any no-chain turns. These fields track a stricter
    // notion of recovery progress: a small 1-chain of four puyos is not enough
    // to declare the board meaningfully cleared.
    int meaningfulClearAge = 99;
    int clearedSinceMeaningfulClear = 0;
    int occupiedAtLastMeaningfulClear = 0;
    int occupiedGrowthSinceMeaningfulClear = 0;

    int quietTurns = 0;
    int occupiedAtLastChain = 0;
    int occupiedGrowthSinceChain = 0;

    int recentChainCount = 0;
    int recentClearPuyos = 0;
    int recentMeaningfulClears = 0;

    PolicyMode mode = PolicyMode::Build;

    // The board hash is used only to detect whether the caller has advanced to
    // the board produced by the last committed move. This prevents a repeated
    // chooseMove() call for the same turn from advancing history twice.
    std::uint64_t lastBoardHash = 0;
    bool initialized = false;

    std::array<int, kWindow> chainWindow{};
    std::array<int, kWindow> erasedWindow{};
    std::array<int, kWindow> meaningfulWindow{};
    int windowSize = 0;
    int windowIndex = 0;

    void reset(const Board& board);

    // Synchronize before a decision. A normal turn advances by exactly one and
    // presents the board produced by the previous move. If a caller restarts,
    // skips a turn, or supplies an unexpected board, discard stale policy state
    // instead of carrying it into a different game state.
    void synchronize(int turnNumber, const Board& board);

    // Commit the result of the actual move selected for turnNumber.
    void observeMove(int turnNumber, const Board& board, int chains, int erased);

    bool inRebuild() const;
    bool hasRecentMeaningfulClear() const { return meaningfulClearAge < 99; }

    // A bounded pressure score used by policy code. It intentionally rewards
    // actual recent clearing rather than treating every chain event as equal.
    int clearDebtScore() const;
};

int occupiedCells(const Board& board);
std::uint64_t boardStateHash(const Board& board);

} // namespace puyo
