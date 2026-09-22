#include "survival_horizon.h"

#include "../search/move_generator.h"
#include "../simulation/simulator.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace puyo {

SurvivalHorizon analyzeSurvivalHorizon(
    const Board& board,
    const PuyoPair* next,
    const PuyoPair* nextNext
) {
    SurvivalHorizon out;
    if (!next) return out;

    const auto moves = generateLegalMoves(board, *next);
    out.geometricMoves = static_cast<int>(moves.size());
    if (moves.empty()) {
        out.safeMoves = 0;
        out.bestNextGeometricMoves = nextNext ? 0 : -1;
        out.bestNextSafeMoves = nextNext ? 0 : -1;
        return out;
    }

    const auto h = board.heights();
    const int maxHeight = *std::max_element(h.begin(), h.end());
    out.safeMoves = 0;

    // Most of the time a cheap landing-height test is sufficient. Once the
    // board is near the danger line, however, resolve the candidate exactly.
    // This matters because a trigger can clear puyos and make an apparently
    // dangerous landing safe; the old probe counted such a move as unsafe.
    const bool exactSafety = h[2] >= 10 || (maxHeight >= 13 && h[2] >= 9) || out.geometricMoves <= 4;

    for (const Move& move : moves) {
        bool safe = false;
        if (exactSafety) {
            const SimulationResult sim = Simulator::drop(board, *next, move);
            safe = !sim.gameOver || sim.allClear;
        } else {
            const int y = Simulator::findDropY(board, *next, move.x, move.rotation);
            if (y < 0) continue;

            int landingHeight2 = h[2];
            switch (move.rotation & 3) {
                case 0: // main at x, sub above
                    if (move.x == 2) landingHeight2 = std::max(landingHeight2, y + 2);
                    break;
                case 1: // sub left
                    if (move.x == 2 || move.x - 1 == 2)
                        landingHeight2 = std::max(landingHeight2, y + 1);
                    break;
                case 2: // sub below
                    if (move.x == 2) landingHeight2 = std::max(landingHeight2, y + 1);
                    break;
                case 3: // sub right
                    if (move.x == 2 || move.x + 1 == 2)
                        landingHeight2 = std::max(landingHeight2, y + 1);
                    break;
            }
            safe = landingHeight2 < VISIBLE_HEIGHT;

        }

        if (!safe) continue;
        ++out.safeMoves;
    }

    // Exact visible-piece trigger/progress probe.  We evaluate every legal
    // first placement once, then follow only a bounded, diverse shortlist.
    // This preserves the important "setup without immediate fire" cases while
    // preventing the diagnostic from turning into a second full beam search.
    if (nextNext) {
        struct FirstStep {
            Move move;
            SimulationResult sim;
            int maxHeight = 0;
            int dangerHeight = 0;
        };
        std::vector<FirstStep> firstSteps;
        firstSteps.reserve(moves.size());

        for (const Move& move : moves) {
            const SimulationResult sim = Simulator::drop(board, *next, move);
            if (sim.gameOver && !sim.allClear) continue;
            const auto heights = sim.board.heights();
            out.trueImmediateChains = std::max(out.trueImmediateChains, sim.chains);
            out.bestImmediateChains = std::max(out.bestImmediateChains, sim.chains);
            if (sim.chains > 0) {
                ++out.trueTriggerMoves;
                ++out.productiveNextMoves;
            }
            out.trueTriggerPath = std::max(out.trueTriggerPath, sim.chains);
            out.bestTriggerPath = std::max(out.bestTriggerPath, sim.chains);
            firstSteps.push_back({
                move,
                sim,
                *std::max_element(heights.begin(), heights.end()),
                heights[2]
            });
        }

        std::stable_sort(firstSteps.begin(), firstSteps.end(),
            [](const FirstStep& a, const FirstStep& b) {
                if (a.sim.chains != b.sim.chains) return a.sim.chains > b.sim.chains;
                if (a.dangerHeight != b.dangerHeight) return a.dangerHeight < b.dangerHeight;
                if (a.maxHeight != b.maxHeight) return a.maxHeight < b.maxHeight;
                if (a.move.x != b.move.x) return a.move.x < b.move.x;
                return a.move.rotation < b.move.rotation;
            });

        // Keep every immediately productive move when possible, plus a bounded
        // set of non-firing setup moves. The latter is essential for detecting
        // A->B handoff construction instead of rewarding only cash-out moves.
        std::vector<const FirstStep*> shortlist;
        shortlist.reserve(std::min<std::size_t>(firstSteps.size(), 8));
        for (const auto& step : firstSteps) {
            if (step.sim.chains > 0) shortlist.push_back(&step);
        }
        const std::size_t maxProbe = 8;
        for (const auto& step : firstSteps) {
            if (shortlist.size() >= maxProbe) break;
            if (step.sim.chains == 0) shortlist.push_back(&step);
        }
        if (shortlist.size() > maxProbe) shortlist.resize(maxProbe);

        for (const FirstStep* step : shortlist) {
            const auto followMoves = generateLegalMoves(step->sim.board, *nextNext);
            int followSafe = 0;
            int bestFollow = 0;
            for (const Move& followMove : followMoves) {
                const SimulationResult follow =
                    Simulator::drop(step->sim.board, *nextNext, followMove);
                if (follow.gameOver && !follow.allClear) continue;
                ++followSafe;
                bestFollow = std::max(bestFollow, follow.chains);
            }

            out.trueFollowupSafeMoves =
                std::max(out.trueFollowupSafeMoves, followSafe);
            out.trueFollowupChains =
                std::max(out.trueFollowupChains, bestFollow);
            out.bestFollowupChains =
                std::max(out.bestFollowupChains, bestFollow);
            if (bestFollow > 0) ++out.productiveFollowupMoves;
            out.trueTriggerPath =
                std::max(out.trueTriggerPath, step->sim.chains + bestFollow);
            out.bestTriggerPath =
                std::max(out.bestTriggerPath, step->sim.chains + bestFollow);
        }
    } else {
        // Keep the one-pair diagnostic useful when the queue is exhausted.
        for (const Move& move : moves) {
            const SimulationResult sim = Simulator::drop(board, *next, move);
            if (sim.gameOver && !sim.allClear) continue;
            out.trueImmediateChains = std::max(out.trueImmediateChains, sim.chains);
            out.bestImmediateChains = std::max(out.bestImmediateChains, sim.chains);
            if (sim.chains > 0) {
                ++out.trueTriggerMoves;
                ++out.productiveNextMoves;
            }
            out.trueTriggerPath = std::max(out.trueTriggerPath, sim.chains);
            out.bestTriggerPath = std::max(out.bestTriggerPath, sim.chains);
        }
    }

    // Only inspect the second geometric horizon when the first horizon is
    // narrow. On exact-safety boards this uses the resolved post-chain board,
    // so a useful trigger-clearing move is not unfairly penalized.
    if (nextNext && out.safeMoves <= 4) {
        out.bestNextGeometricMoves = 0;
        out.bestNextSafeMoves = 0;
        // Re-simulate the first horizon exactly here. The cheap landing-height
        // probe above intentionally does not resolve chains on healthy boards;
        // reusing that approximate board would therefore under/over-count the
        // second-step escape route when the first placement triggers a clear.
        struct EscapeCandidate { Board board; int h2 = 0; int maxHeight = 0; };
        std::vector<EscapeCandidate> escapeCandidates;
        escapeCandidates.reserve(moves.size());
        for (const Move& move : moves) {
            const SimulationResult first = Simulator::drop(board, *next, move);
            if (first.gameOver && !first.allClear) continue;
            const auto heights = first.board.heights();
            escapeCandidates.push_back({
                first.board,
                heights[2],
                *std::max_element(heights.begin(), heights.end())
            });
        }
        std::stable_sort(escapeCandidates.begin(), escapeCandidates.end(),
            [](const EscapeCandidate& a, const EscapeCandidate& b) {
                if (a.h2 != b.h2) return a.h2 < b.h2;
                return a.maxHeight < b.maxHeight;
            });
        const std::size_t escapeProbe = std::min<std::size_t>(escapeCandidates.size(), 10);
        for (std::size_t i = 0; i < escapeProbe; ++i) {
            const auto nextMoves = generateLegalMoves(escapeCandidates[i].board, *nextNext);
            out.bestNextGeometricMoves = std::max(
                out.bestNextGeometricMoves,
                static_cast<int>(nextMoves.size())
            );

            int safeSecond = 0;
            for (const Move& followMove : nextMoves) {
                const SimulationResult second =
                    Simulator::drop(escapeCandidates[i].board, *nextNext, followMove);
                if (!second.gameOver || second.allClear) ++safeSecond;
            }
            out.bestNextSafeMoves = std::max(out.bestNextSafeMoves, safeSecond);
        }
    }

    return out;
}

double survivalHorizonScore(
    const SurvivalHorizon& horizon,
    int previousSafeMoves,
    int previousGeometricMoves
) {
    if (horizon.safeMoves < 0) return 0.0;

    // Survival is a correction signal, not a general preference for empty
    // space. Comfortable mobility receives no reward. The negative region is
    // concentrated near collapse so the chain evaluator remains dominant.
    static constexpr double safePenalty[] = {
        -120000.0, // 0
        -50000.0,  // 1
        -18000.0,  // 2
        -4500.0,   // 3
        0.0,       // 4
        0.0,       // 5
        0.0,       // 6
        0.0        // 7+
    };
    const int safeIndex = std::min(horizon.safeMoves, 7);
    double score = safePenalty[safeIndex];


    if (horizon.bestNextGeometricMoves >= 0 && horizon.safeMoves <= 4) {
        score += std::clamp(
            static_cast<double>(horizon.bestNextGeometricMoves - 8) * 300.0,
            -3000.0,
            3000.0
        );
    }

    // Prefer an actual escape route, not merely a board on which the next
    // piece happens to fit geometrically.  This term is only meaningful in
    // the narrow first-horizon region, so it cannot turn healthy construction
    // into a generic "maximize mobility" policy.
    if (horizon.bestNextSafeMoves >= 0 && horizon.safeMoves <= 4) {
        score += std::clamp(
            static_cast<double>(horizon.bestNextSafeMoves - 3) * 1200.0,
            -6000.0,
            4500.0
        );
    }

    if (previousSafeMoves >= 0) {
        const int collapse = previousSafeMoves - horizon.safeMoves;
        if (collapse >= 4) score -= 9000.0;
        else if (collapse == 3) score -= 5500.0;
        else if (collapse == 2) score -= 1800.0;
        else if (collapse == 1) score -= 250.0;
    }

    if (previousGeometricMoves >= 0) {
        const int geometricCollapse = previousGeometricMoves - horizon.geometricMoves;
        if (geometricCollapse >= 5) score -= 5000.0;
        else if (geometricCollapse >= 3) score -= 2500.0;
        else if (geometricCollapse == 2) score -= 900.0;
        else if (geometricCollapse == 1) score -= 150.0;
    }

    return std::clamp(score, -150000.0, 0.0);
}

} // namespace puyo
