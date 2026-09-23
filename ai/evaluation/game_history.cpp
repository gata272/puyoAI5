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
    occupiedAtLastChain = occupiedCells(board);
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

    if (!initialized) reset(board);

    turn = turnNumber;
    lastActualChain = chains;
    chainAge = chains > 0 ? 0 : std::min(chainAge + 1, 99);
    quietTurns = chains > 0 ? 0 : std::min(quietTurns + 1, 99);

    if (chains > 0) {
        occupiedAtLastChain = occupied;
        occupiedGrowthSinceChain = 0;
    } else {
        occupiedGrowthSinceChain = std::min(
            std::max(0, occupied - occupiedAtLastChain),
            96
        );
    }

    if (chains >= 4) {
        lastBigChain = chains;
        postBigChainAge = 0;
        mode = PolicyMode::Rebuild;
    } else if (postBigChainAge < 99) {
        postBigChainAge = std::min(postBigChainAge + 1, 99);
        // Give the freshly opened board several turns to rebuild a real next
        // trigger. Once that window expires, return to ordinary construction.
        if (postBigChainAge > 8 && quietTurns >= 3) {
            mode = PolicyMode::Tension;
        } else if (postBigChainAge > 8) {
            mode = PolicyMode::Build;
        }
    }

    if (windowSize < kWindow) {
        chainWindow[windowSize] = chains > 0 ? 1 : 0;
        erasedWindow[windowSize] = std::max(0, erased);
        ++windowSize;
        windowIndex = windowSize % kWindow;
    } else {
        recentChainCount -= chainWindow[windowIndex];
        recentClearPuyos -= erasedWindow[windowIndex];
        chainWindow[windowIndex] = chains > 0 ? 1 : 0;
        erasedWindow[windowIndex] = std::max(0, erased);
        windowIndex = (windowIndex + 1) % kWindow;
    }
    recentChainCount += chains > 0 ? 1 : 0;
    recentClearPuyos += std::max(0, erased);

    // A chain event during REBUILD is meaningful progress, but do not leave
    // REBUILD solely because one small chain happened: the policy should get a
    // few more turns to establish the next actual trigger.
    if (mode == PolicyMode::Rebuild && postBigChainAge > 8) {
        mode = PolicyMode::Build;
    }

    initialized = true;
    lastBoardHash = boardStateHash(board);

}

bool GameHistory::inRebuild() const {
    return mode == PolicyMode::Rebuild && postBigChainAge <= 8;
}

} // namespace puyo
