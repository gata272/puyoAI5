#include "ai.h"
#include "simulation/simulator.h"
#include "search/move_generator.h"
#include "evaluation/debug_log.h"
#include "evaluation/game_history.h"
#include <sstream>

#include <algorithm>
#include <limits>
#include <numeric>

namespace puyo {

AI::AI()
    : weights_(amaBuildWeights()) {
}

void AI::reset() {
    gtr_.reset();
    patternName_.clear();
    history_ = GameHistory{};
}

Move AI::chooseMove(
    int turn,
    const Board& board,
    const std::vector<PuyoPair>& pieces
) {
    return chooseMove(turn, board, pieces, 3, 12);
}

Move AI::chooseMove(
    int turn,
    const Board& board,
    const std::vector<PuyoPair>& pieces,
    int depth,
    int beamWidth
) {
    if (pieces.empty()) return {-1, 0, false};

    history_.synchronize(turn, board);

    // Preserve the current AI's first three GTR moves. Once the GTR plan is
    // unavailable or exhausted, switch to the general search/evaluation engine.
    if (turn >= 0 && turn < 3 && pieces.size() >= 3) {
        Move gtrMove = gtr_.chooseMove(
            turn,
            pieces[0],
            pieces[1],
            pieces[2]
        );

        patternName_ = gtr_.patternName();

        if (gtrMove.valid) {
            // Keep GTR when it is safe. If the planned GTR placement would
            // itself cause game over while another safe placement exists,
            // fall through to the general search instead. If no safe move
            // exists, the general search has an explicit death-placement
            // fallback and will return the least-bad game-over move.
            const auto legal = generateLegalMoves(board, pieces[0]);
            bool safeExists = false;
            for (const auto& move : legal) {
                const auto sim = Simulator::drop(board, pieces[0], move);
                if (!sim.gameOver || sim.allClear) {
                    safeExists = true;
                    break;
                }
            }
            const auto gtrSim = Simulator::drop(board, pieces[0], gtrMove);
            if (debugLoggingEnabled()) {
                std::ostringstream oss;
                oss << "[AI-DEBUG] GTR turn=" << turn
                    << " pattern=" << patternName_
                    << " root=(" << gtrMove.x << "," << gtrMove.rotation << ")"
                    << " safeExists=" << (safeExists ? 1 : 0)
                    << " gtrGameOver=" << (gtrSim.gameOver ? 1 : 0)
                    << " gtrAllClear=" << (gtrSim.allClear ? 1 : 0);
                debugLog(oss.str());
            }
            if (!safeExists || !gtrSim.gameOver || gtrSim.allClear) {
                observeChosenMove(turn, board, pieces, gtrMove);
                return gtrMove;
            }
            if (debugLoggingEnabled()) {
                debugLog("[AI-DEBUG] GTR rejected because it would die while another safe move exists");
            }
        }
    }

    patternName_.clear();

    Move selected = search_.chooseMove(
        board,
        pieces,
        weights_,
        std::max(1, depth),
        std::max(1, beamWidth),
        history_
    );

    // Final hard safety guard. The search already reserves safe candidates,
    // but this check is intentionally outside the scorer so a future scoring
    // change can never reintroduce the old failure mode: selecting a
    // game-over placement while at least one safe placement exists. It only
    // runs on an invalid/death selection, so it cannot perturb normal
    // long-chain ranking.
    bool needsFallback = !selected.valid;
    SimulationResult selectedSim{};
    if (selected.valid) {
        selectedSim = Simulator::drop(board, pieces.front(), selected);
        needsFallback = selectedSim.gameOver && !selectedSim.allClear;
    }

    if (needsFallback) {
        const auto legal = generateLegalMoves(board, pieces.front());
        Move bestSafe{-1, 0, false};
        long long bestSafeValue = std::numeric_limits<long long>::min();
        bool anySafe = false;

        for (const auto& move : legal) {
            const auto sim = Simulator::drop(board, pieces.front(), move);
            if (sim.gameOver && !sim.allClear) continue;
            anySafe = true;

            const auto heights = sim.board.heights();
            const int maxHeight = *std::max_element(heights.begin(), heights.end());
            const int occupied = std::accumulate(heights.begin(), heights.end(), 0);
            const long long value =
                static_cast<long long>(sim.chains) * 1000000LL +
                static_cast<long long>(std::max(0, sim.erased)) * 10000LL -
                static_cast<long long>(maxHeight) * 1000LL -
                static_cast<long long>(occupied) * 10LL;

            if (bestSafe.x < 0 || value > bestSafeValue) {
                bestSafe = move;
                bestSafeValue = value;
            }
        }

        if (anySafe) {
            if (debugLoggingEnabled()) {
                std::ostringstream oss;
                oss << "[AI-DEBUG] FINAL-SAFETY-FALLBACK selected=("
                    << (selected.valid ? selected.x : -1) << ","
                    << (selected.valid ? selected.rotation : -1) << ")"
                    << " fallback=(" << bestSafe.x << "," << bestSafe.rotation << ")";
                debugLog(oss.str());
            }
            selected = bestSafe;
        }
    }

    observeChosenMove(turn, board, pieces, selected);
    return selected;
}

void AI::resetWeights() {
    weights_ = amaBuildWeights();
}

bool AI::setWeight(int index, double value) {
    return puyo::setWeight(weights_, index, value);
}

double AI::getWeight(int index) const {
    return puyo::getWeight(weights_, index);
}

int AI::weightCount() const {
    return puyo::weightCount();
}

const char* AI::weightName(int index) const {
    return puyo::weightName(index);
}

const char* AI::patternName() const {
    return patternName_.c_str();
}

void AI::observeChosenMove(
    int turn,
    const Board& board,
    const std::vector<PuyoPair>& pieces,
    const Move& move
) {
    if (!move.valid || pieces.empty()) return;
    const SimulationResult sim = Simulator::drop(board, pieces.front(), move);
    history_.observeMove(turn, sim.board, sim.chains, sim.erased);
}

} // namespace puyo
