#include "ai/evaluation/game_history.h"
#include <cassert>
#include <iostream>

using namespace puyo;

int main() {
    Board board;
    GameHistory history;

    history.synchronize(0, board);
    assert(history.initialized);
    assert(history.turn == -1);
    assert(history.mode == PolicyMode::Build);
    assert(history.meaningfulClearAge == 100 || history.meaningfulClearAge == 99);
    assert(history.clearDebtScore() == 0);

    // A quiet placement accumulates material relative to the last meaningful
    // clear baseline.
    Board taller = board;
    for (int y = 0; y < 6; ++y) {
        taller.set(0, y, Cell::Red);
        taller.set(1, y, Cell::Blue);
    }
    history.observeMove(0, taller, 0, 0);
    assert(history.chainAge == 1);
    assert(history.quietTurns == 1);
    assert(history.occupiedGrowthSinceChain == occupiedCells(taller));
    assert(history.occupiedGrowthSinceMeaningfulClear == occupiedCells(taller));
    assert(history.recentChainCount == 0);

    // Repeating a no-chain turn must not double-count growth from the same
    // baseline.
    Board tallerAgain = taller;
    tallerAgain.set(2, 0, Cell::Green);
    history.observeMove(1, tallerAgain, 0, 0);
    assert(history.occupiedGrowthSinceChain == occupiedCells(tallerAgain));
    assert(history.occupiedGrowthSinceMeaningfulClear == occupiedCells(tallerAgain));
    assert(history.quietTurns == 2);

    // A 1-chain that removes only four puyos is NOT a meaningful clear. This
    // prevents tiny cash-out chains from erasing the accumulation evidence.
    Board stillTall = tallerAgain;
    history.observeMove(2, stillTall, 1, 4);
    assert(history.chainAge == 0);
    assert(history.quietTurns == 0);
    assert(history.meaningfulClearAge >= 3);
    assert(history.occupiedGrowthSinceMeaningfulClear == occupiedCells(stillTall));

    // A larger clear resets the meaningful-clear baseline.
    Board cleared = board;
    cleared.set(0, 0, Cell::Red);
    history.observeMove(3, cleared, 2, 8);
    assert(history.lastActualChain == 2);
    assert(history.chainAge == 0);
    assert(history.quietTurns == 0);
    assert(history.meaningfulClearAge == 0);
    assert(history.occupiedGrowthSinceMeaningfulClear == 0);
    assert(history.recentMeaningfulClears >= 1);

    // A large actual chain enters persistent REBUILD state.
    history.observeMove(4, cleared, 5, 24);
    assert(history.lastBigChain == 5);
    assert(history.postBigChainAge == 0);
    assert(history.inRebuild());
    assert(history.mode == PolicyMode::Rebuild);

    // Small follow-up chains do not reset the REBUILD age; they also do not
    // falsely imply a new big-chain event.
    history.observeMove(5, cleared, 1, 4);
    assert(history.inRebuild());
    assert(history.postBigChainAge == 1);
    assert(history.lastBigChain == 5);

    // REBUILD now has a safety cap, not the old hard 8-turn expiry.
    for (int turn = 6; turn <= 19; ++turn) {
        history.observeMove(turn, cleared, 0, 0);
    }
    assert(history.postBigChainAge == GameHistory::kRebuildMaxAge + 1);
    assert(!history.inRebuild());
    assert(history.mode == PolicyMode::Build || history.mode == PolicyMode::Tension);

    // Synchronization is idempotent for repeated requests on the same post-move
    // board, but a skipped/unexpected turn discards stale history safely.
    const auto stateBefore = history.turn;
    history.synchronize(20, cleared);
    assert(history.turn == stateBefore);
    Board unrelated;
    history.synchronize(30, unrelated);
    assert(history.turn == 29);
    assert(history.mode == PolicyMode::Build);
    assert(history.quietTurns == 0);
    assert(history.recentChainCount == 0);
    assert(history.meaningfulClearAge == 99);

    std::cout << "game history tests passed\n";
    return 0;
}
