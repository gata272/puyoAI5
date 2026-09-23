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

    // A quiet placement accumulates material relative to the last real clear.
    Board taller = board;
    for (int y = 0; y < 6; ++y) {
        taller.set(0, y, Cell::Red);
        taller.set(1, y, Cell::Blue);
    }
    history.observeMove(0, taller, 0, 0);
    assert(history.chainAge == 1);
    assert(history.quietTurns == 1);
    assert(history.occupiedGrowthSinceChain == occupiedCells(taller));
    assert(history.recentChainCount == 0);

    // Repeating a no-chain turn must not double-count growth from the same
    // baseline. The value is occupancy since the last actual clear, not a sum
    // of per-turn deltas.
    Board tallerAgain = taller;
    tallerAgain.set(2, 0, Cell::Green);
    history.observeMove(1, tallerAgain, 0, 0);
    assert(history.occupiedGrowthSinceChain == occupiedCells(tallerAgain));
    assert(history.quietTurns == 2);

    // A large actual chain enters persistent REBUILD state.
    Board cleared;
    cleared.set(0, 0, Cell::Red);
    history.observeMove(2, cleared, 5, 24);
    assert(history.lastActualChain == 5);
    assert(history.chainAge == 0);
    assert(history.quietTurns == 0);
    assert(history.lastBigChain == 5);
    assert(history.postBigChainAge == 0);
    assert(history.inRebuild());
    assert(history.mode == PolicyMode::Rebuild);
    assert(history.recentChainCount == 1);
    assert(history.recentClearPuyos == 24);

    // Small follow-up chains keep the rebuild window alive and reset quietness.
    history.observeMove(3, cleared, 2, 8);
    assert(history.inRebuild());
    assert(history.postBigChainAge == 1);
    assert(history.quietTurns == 0);
    assert(history.recentChainCount == 2);
    assert(history.recentClearPuyos == 32);

    // After the explicit rebuild window expires, control returns to BUILD.
    for (int turn = 4; turn <= 12; ++turn) {
        history.observeMove(turn, cleared, 0, 0);
    }
    assert(!history.inRebuild());
    assert(history.mode == PolicyMode::Build || history.mode == PolicyMode::Tension);

    // Synchronization is idempotent for repeated requests on the same post-move
    // board, but a skipped/unexpected turn discards stale history safely.
    const auto stateBefore = history.turn;
    history.synchronize(13, cleared);
    assert(history.turn == stateBefore);
    Board unrelated;
    history.synchronize(30, unrelated);
    assert(history.turn == 29);
    assert(history.mode == PolicyMode::Build);
    assert(history.quietTurns == 0);
    assert(history.recentChainCount == 0);

    std::cout << "game history tests passed\n";
    return 0;
}
