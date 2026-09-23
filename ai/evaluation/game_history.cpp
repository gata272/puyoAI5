#include "game_history.h"

#include <algorithm>
#include <numeric>

namespace puyo {

const char* policyModeName(PolicyMode mode) {
    switch (mode) {
        case PolicyMode::Build: return "BUILD";
        case PolicyMode::Tension: return "TENSION";
        case PolicyMode::Recover: return "RECOVER";
        case PolicyMode::Rebuild: return "REBUILD";
    }
    return "BUILD";
}

int occupiedCells(const Board& board) {
    const auto heights = board.heights();
    return std::accumulate(heights.begin(), heights.end(), 0);
}

std::uint64_t boardStateHash(const Board& board) {
    std::uint64_t h = 1469598103934665603ULL;
    for (int y = 0; y < BOARD_HEIGHT; ++y) {
        for (int x = 0; x < BOARD_WIDTH; ++x) {
            h ^= static_cast<std::uint64_t>(static_cast<int>(board.get(x, y)) + 1);
            h *= 1099511628211ULL;
        }
    }
    return h;
}

void GameHistory::reset(const Board& board) {
    *this = GameHistory{};
    initialized = true;
    lastBoardHash = boardStateHash(board);
    const int occupied = occupiedCells(board);
    occupiedAtLastChain = occupied;
    occupiedAtLastMeaningfulClear = occupied;
}

void GameHistory::synchronize(int turnNumber, const Board& board) {
    const std::uint64_t hash = boardStateHash(board);
    if (!initialized) {
        reset(board);
        turn = turnNumber - 1;
        return;
    }

    // The next decision normally sees turn+1 and exactly the previous post-move
    // board. Repeated requests for the same turn are intentionally idempotent.
    if (turnNumber == turn && hash == lastBoardHash) return;

    if (turnNumber != turn + 1 || hash != lastBoardHash) {
        reset(board);
        turn = turnNumber - 1;
    }
}

void GameHistory::observeMove(
    int turnNumber,
    const Board& board,
    int chains,
    int erased
) {
    const int occupied = occupiedCells(board);
    const int normalizedErased = std::max(0, erased);
    const int normalizedChains = std::max(0, chains);

    if (!initialized) reset(board);

    turn = turnNumber;
    lastActualChain = normalizedChains;
    chainAge = normalizedChains > 0 ? 0 : std::min(chainAge + 1, 99);
    quietTurns = normalizedChains > 0 ? 0 : std::min(quietTurns + 1, 99);

    if (normalizedChains > 0) {
        occupiedAtLastChain = occupied;
        occupiedGrowthSinceChain = 0;
    } else {
        occupiedGrowthSinceChain = std::min(
            std::max(0, occupied - occupiedAtLastChain),
            96
        );
    }

    // A meaningful clear must remove enough material to materially change the
    // board, or involve a multi-chain event, or leave the board empty. This is
    // deliberately stricter than `chains > 0`: a four-puyo 1-chain must not
    // erase the evidence that the board has been accumulating material.
    const bool meaningfulClear =
        normalizedErased >= kMeaningfulClearPuyos ||
        normalizedChains >= 2 ||
        occupied == 0;

    meaningfulClearAge = meaningfulClear
        ? 0
        : std::min(meaningfulClearAge + 1, 99);
    clearedSinceMeaningfulClear = std::min(
        clearedSinceMeaningfulClear + normalizedErased,
        256
    );

    if (meaningfulClear) {
        occupiedAtLastMeaningfulClear = occupied;
        occupiedGrowthSinceMeaningfulClear = 0;
        clearedSinceMeaningfulClear = 0;
    } else {
        occupiedGrowthSinceMeaningfulClear = std::min(
            std::max(0, occupied - occupiedAtLastMeaningfulClear),
            96
        );
    }

    if (normalizedChains >= 4) {
        lastBigChain = normalizedChains;
        postBigChainAge = 0;
        mode = PolicyMode::Rebuild;
    } else if (postBigChainAge < 99) {
        postBigChainAge = std::min(postBigChainAge + 1, 99);
        // REBUILD is no longer a hard 8-turn phase. It may persist until a
        // visible trigger route becomes ready, with this age acting only as a
        // safety cap against getting stuck forever.
        if (postBigChainAge > kRebuildMaxAge) {
            mode = (occupied >= 60 || meaningfulClearAge >= 8)
                ? PolicyMode::Tension
                : PolicyMode::Build;
        } else {
            mode = PolicyMode::Rebuild;
        }
    }

    if (windowSize < kWindow) {
        chainWindow[windowSize] = normalizedChains > 0 ? 1 : 0;
        erasedWindow[windowSize] = normalizedErased;
        meaningfulWindow[windowSize] = meaningfulClear ? 1 : 0;
        ++windowSize;
        windowIndex = windowSize % kWindow;
    } else {
        recentChainCount -= chainWindow[windowIndex];
        recentClearPuyos -= erasedWindow[windowIndex];
        recentMeaningfulClears -= meaningfulWindow[windowIndex];

        chainWindow[windowIndex] = normalizedChains > 0 ? 1 : 0;
        erasedWindow[windowIndex] = normalizedErased;
        meaningfulWindow[windowIndex] = meaningfulClear ? 1 : 0;
        windowIndex = (windowIndex + 1) % kWindow;
    }
    recentChainCount += normalizedChains > 0 ? 1 : 0;
    recentClearPuyos += normalizedErased;
    recentMeaningfulClears += meaningfulClear ? 1 : 0;

    // A small chain during REBUILD is useful evidence, but it is not by itself
    // enough to end REBUILD. The mode changes only through actual trigger
    // readiness in policy evaluation or the age safety cap above.
    initialized = true;
    lastBoardHash = boardStateHash(board);
}

bool GameHistory::inRebuild() const {
    return mode == PolicyMode::Rebuild &&
           postBigChainAge <= kRebuildMaxAge;
}

int GameHistory::clearDebtScore() const {
    // Age and accumulated material are the two primary forms of debt. Recent
    // clearing offsets that pressure, but is intentionally capped so many
    // small clears cannot completely hide a chronically tall board.
    const int meaningfulAge = meaningfulClearAge >= 99 ? 0 : std::max(0, meaningfulClearAge);
    const int agePart = std::min(50, meaningfulAge * 2);
    const int growthPart = std::min(96, std::max(0, occupiedGrowthSinceMeaningfulClear) * 4);
    const int recentClearRelief = std::min(48, std::max(0, recentClearPuyos));
    return std::clamp(agePart + growthPart - recentClearRelief, 0, 120);
}

} // namespace puyo
