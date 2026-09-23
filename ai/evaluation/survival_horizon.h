#pragma once

#include "../simulation/board.h"
#include "../search/move.h"

namespace puyo {

struct SurvivalHorizon {
    // Number of placements of `next` that do not immediately enter game over.
    // -1 means that no next pair was supplied.
    int geometricMoves = -1;
    int safeMoves = -1;

    // Among safe placements of `next`, the largest number of geometric
    // placements available for `nextNext`.
    int bestNextGeometricMoves = -1;

    // A stronger two-step signal than geometric mobility alone.  We count the
    // largest number of actually safe placements for nextNext from any safe
    // next placement.  This is only evaluated when the first horizon is
    // narrow, so it remains a cheap escape-route probe rather than a second
    // full search.
    int bestNextSafeMoves = -1;

    // Exact probes using the actually visible next pair(s), as opposed to the
    // arbitrary colour pairs used by virtual-chain potential.
    int trueImmediateChains = 0;
    int trueFollowupChains = 0;
    int trueTriggerPath = 0;
    int trueTriggerMoves = 0;
    int trueFollowupSafeMoves = 0;

    // Chain-progress diagnostics using only the visible next two pairs.
    // These are intentionally not hidden-future search: they answer whether
    // the current construction has a real way to start/continue a chain with
    // the pieces the AI can actually see.
    int productiveNextMoves = 0;
    int productiveFollowupMoves = 0;
    int bestImmediateChains = 0;
    int bestImmediateErased = 0;
    int bestImmediatePostOccupied = BOARD_WIDTH * VISIBLE_HEIGHT;
    int bestFollowupChains = 0;
    int bestTriggerPath = 0;

    // Recovery / reactivation probes. These are computed from the visible
    // next two pairs only and describe whether a candidate can actually clear
    // space and continue building after that clear.
    int bestImmediateNetClear = 0;
    int bestImmediatePostSafeMoves = -1;
    int bestImmediatePostMaxHeight = VISIBLE_HEIGHT;
    int bestFollowupNetClear = 0;
    int bestRebuildChain = 0;
    int bestRebuildNetClear = 0;
    int bestRebuildNextSafeMoves = -1;
    int rebuildCandidates = 0;
};

SurvivalHorizon analyzeSurvivalHorizon(
    const Board& board,
    const PuyoPair* next,
    const PuyoPair* nextNext = nullptr
);

// Returns a bounded utility in score units. The curve is intentionally steep
// around 0-2 safe moves: a rapid loss of future mobility is a warning signal,
// not a reason to abandon a strong long-chain construction at comfortable
// mobility levels.
double survivalHorizonScore(
    const SurvivalHorizon& horizon,
    int previousSafeMoves = -1,
    int previousGeometricMoves = -1
);

} // namespace puyo
