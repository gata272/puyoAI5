
#include "../ai/simulation/simulator.h"
#include "../ai/ai.h"
#include "../ai/evaluation/trigger_route.h"
#include "../ai/evaluation/debug_log.h"
#include "../ai/evaluation/game_history.h"
#include <cassert>
#include <iostream>

int main() {
    puyo::Board board;
    puyo::AI ai;

    std::vector<puyo::PuyoPair> pieces = {
        {1, 1}, {1, 2}, {1, 2}
    };

    auto move = ai.chooseMove(0, board, pieces);
    assert(move.valid);
    assert(move.x >= 0 && move.x < puyo::BOARD_WIDTH);
    assert(move.rotation >= 0 && move.rotation < 4);

    auto sim = puyo::Simulator::drop(board, pieces[0], move);
    assert(!sim.gameOver);

    auto next = ai.chooseMove(3, sim.board, pieces);
    assert(next.valid);

    // Regression test: when every legal placement is a game-over placement,
    // the AI must still return a valid move rather than refusing to move.
    puyo::Board doomed;
    for (int x = 0; x < puyo::BOARD_WIDTH; ++x) {
        for (int y = 0; y < puyo::VISIBLE_HEIGHT; ++y) {
            doomed.set(x, y, static_cast<puyo::Cell>(1 + ((x + y) % 4)));
        }
    }
    auto fallback = ai.chooseMove(3, doomed, pieces, 4, 8);
    assert(fallback.valid);
    auto fallbackSim = puyo::Simulator::drop(doomed, pieces[0], fallback);
    assert(fallbackSim.gameOver);
    assert(!fallbackSim.allClear);


    // Trigger-transfer regression: C -> B -> A is recognized as a three-level
    // dependency.  Clearing C makes B a four-group; clearing B then makes A
    // a four-group.
    puyo::Board relay;
    relay.set(1,0,puyo::Cell::Red); relay.set(2,0,puyo::Cell::Red); relay.set(3,0,puyo::Cell::Red);
    relay.set(1,1,puyo::Cell::Blue); relay.set(2,1,puyo::Cell::Blue); relay.set(1,2,puyo::Cell::Blue);
    relay.set(2,2,puyo::Cell::Red);
    relay.set(0,3,puyo::Cell::Green); relay.set(1,3,puyo::Cell::Green); relay.set(0,4,puyo::Cell::Green);
    relay.set(1,4,puyo::Cell::Blue);
    assert(puyo::triggerRouteLength(relay) >= 3);

    // Horizontal transfer regression: clearing a vertical B trigger lets an
    // upper A fall beside an existing horizontal AAA, proving the dependency
    // detector is not restricted to the vertical motif.
    puyo::Board horizontal;
    horizontal.set(1,0,puyo::Cell::Red);
    horizontal.set(2,0,puyo::Cell::Red);
    horizontal.set(3,0,puyo::Cell::Red);
    horizontal.set(0,0,puyo::Cell::Blue);
    horizontal.set(0,1,puyo::Cell::Blue);
    horizontal.set(0,2,puyo::Cell::Blue);
    horizontal.set(0,3,puyo::Cell::Red);
    assert(puyo::triggerRouteLength(horizontal) >= 2);

    // Debug-mode determinism regression: enabling debug tracing may add work,
    // but it must not alter the selected moves or the resulting board state.
    {
        std::vector<puyo::PuyoPair> queue = {
            {1, 2}, {3, 4}, {2, 1}, {4, 3}, {1, 1},
            {2, 4}, {3, 2}, {4, 4}, {1, 3}, {2, 2}
        };
        puyo::Board normalBoard;
        puyo::Board debugBoard = normalBoard;
        puyo::AI normalAI;
        puyo::AI debugAI;

        puyo::setDebugConsoleLogging(false);
        puyo::setDebugLogging(false);
        for (int turn = 0; turn < 8; ++turn) {
            std::vector<puyo::PuyoPair> visible(queue.begin() + turn,
                                                 queue.begin() + turn + 3);
            const auto normalMove = normalAI.chooseMove(
                turn, normalBoard, visible, 3, 8);
            puyo::setDebugLogging(true);
            const auto debugMove = debugAI.chooseMove(
                turn, debugBoard, visible, 3, 8);
            puyo::setDebugLogging(false);

            assert(normalMove.valid == debugMove.valid);
            assert(normalMove.x == debugMove.x);
            assert(normalMove.rotation == debugMove.rotation);

            const auto normalSim = puyo::Simulator::drop(
                normalBoard, visible.front(), normalMove);
            const auto debugSim = puyo::Simulator::drop(
                debugBoard, visible.front(), debugMove);
            assert(puyo::boardStateHash(normalSim.board) ==
                   puyo::boardStateHash(debugSim.board));
            normalBoard = normalSim.board;
            debugBoard = debugSim.board;
        }
        puyo::setDebugLogging(false);
        puyo::setDebugConsoleLogging(true);
    }

    std::cout << "native AI smoke test passed: "
              << next.x << "," << next.rotation << "\n";
    return 0;
}
