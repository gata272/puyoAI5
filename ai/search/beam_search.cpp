#include "beam_search.h"

#include "move_generator.h"
#include "../evaluation/evaluation.h"
#include "../evaluation/trigger_route.h"
#include "../evaluation/long_chain_potential.h"
#include "../evaluation/main_chain.h"
#include "../evaluation/debug_log.h"
#include "../evaluation/virtual_chain_potential.h"
#include "../evaluation/survival_horizon.h"
#include "../evaluation/route_recovery.h"
#include "../simulation/simulator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <array>
#include <numeric>

namespace puyo {
namespace {

// This is a true beam search: every depth expands the current global beam and
// then prunes back to `beamWidth`. The beam deliberately keeps root-action
// identity alive all the way through the search so that one attractive first
// move cannot erase every alternative before terminal scoring.
struct Node {
    Board board;
    Move root;
    double score = 0.0;
    int maxChain = 0;
    int lastChain = 0;
    int triggerRoute = 0;
    double longPotential = 0.0;
    double structure = 0.0;
    MainChainPlan mainChain;
    double mainChainScore = 0.0;
    double construction = 0.0;
    double prematureRisk = 0.0;
    double virtualPotential = 0.0;
    VirtualChainFeatures virtualFeatures;
    bool hasVirtual = false;
    TriggerViability triggerViability;
    double triggerViabilityScore = 0.0;
    bool hasTriggerViability = false;
    int futureSafeMoves = -1;
    int previousFutureSafeMoves = -1;
    int previousFutureGeometricMoves = -1;
    int rootFutureSafeMoves = -1;
    int rootBestNextSafeMoves = -1;
    double rootSurvivalScore = 0.0;
    bool hasRootSurvival = false;
    int futureGeometricMoves = -1;
    int bestNextGeometricMoves = -1;
    int bestNextSafeMoves = -1;
    double survivalScore = 0.0;
    bool hasSurvival = false;

    // Root-action diagnostics are only used for diagnostics and the genuinely
    // critical escape rule. The explicit probe flags are important: zero is a
    // valid measured result, but it must not also mean "not probed".
    int trueTriggerPath = -1;
    int trueImmediateChains = 0;
    int trueFollowupChains = 0;
    bool hasTrueProbe = false;

    int rootTrueTriggerPath = 0;
    int rootTrueImmediateChains = 0;
    int rootTrueFollowupChains = 0;
    int rootTriggerRoute = 0;
    int rootMaxHeight = 0;
    int rootDangerHeight = 0;
    bool hasRootTrueProbe = false;
    bool hasRootRouteProbe = false;
    bool historicalStale = false;

    int previousStaleRouteAge = 0;
    int previousBestNextSafeMoves = -1;
    int currentTriggerRoute = 0;
    bool hasCurrentRouteProbe = false;
    int staleRouteAge = 0;
    RouteRecoveryState routeRecovery;
    RouteRecoveryState rootRouteRecovery;
    bool hasRootRouteRecovery = false;
    bool rebuildPolicy = false;
    int productiveNextMoves = 0;
    int productiveFollowupMoves = 0;
    int bestImmediateNetClear = 0;
    int bestImmediatePostSafeMoves = -1;
    int bestFollowupNetClear = 0;
    int bestRebuildChain = 0;
    int bestRebuildNetClear = 0;
    int bestRebuildNextSafeMoves = -1;
    double recoveryScore = 0.0;
    bool hasRecoveryProbe = false;

    bool gameOver = false;
    std::uint64_t boardHash = 0;
    Features features;
    bool hasFeatures = false;
};

constexpr double kDeathPenalty = 250000.0;

// Immediate chain reward is deliberately nonlinear. It makes an actual long
// chain dominate small scoring differences, while the static evaluator remains
// responsible for constructing the chain before it fires.
double chainReward(int chains) {
    static constexpr double rewards[] = {
        0.0,      // 0
        -18000.0, // 1
        -36000.0, // 2
        -54000.0, // 3
        5000.0,   // 4
        18000.0,  // 5
        45000.0,  // 6
        85000.0,  // 7
        145000.0, // 8
        235000.0, // 9
        360000.0, // 10
        525000.0, // 11
        740000.0, // 12
        1000000.0,// 13
        1300000.0,// 14
        1650000.0 // 15
    };
    if (chains <= 0) return 0.0;
    if (chains < static_cast<int>(std::size(rewards))) return rewards[chains];
    const double c = static_cast<double>(chains);
    return rewards[15] + (c - 15.0) * 400000.0 +
           std::max(0.0, c - 15.0) * std::max(0.0, c - 15.0) * 15000.0;
}

// Contextual anti-cashout rule inspired by the stronger v10-v13 style search
// profile. A 7-9 chain is not inherently bad: when the pre-fire board is tall
// or dangerous it can be exactly the recovery fire we need. Penalize it only
// when the board before the fire is still comfortable AND visibly contains
// enough chain-building material to justify waiting.
double contextualCashoutPenalty(const Node& parent, int chains) {
    if (chains < 7 || chains > 9 || !parent.hasFeatures) return 0.0;

    const auto heights = parent.board.heights();
    const int maxHeight = *std::max_element(heights.begin(), heights.end());
    // The game-over column is x=2 in this simulator. Stay conservative here:
    // once that column or any column is already entering the danger zone, do
    // not discourage the fire that may be needed to survive.
    if (heights[2] >= 9 || maxHeight > 10) return 0.0;

    const Features& f = parent.features;
    const double latent =
        f.chainUnit4 * 6.0 +
        f.chainUnit5 * 3.0 +
        f.handoffPotential * 1.25 +
        f.futureChainSpace * 0.55 +
        f.tailSpace * 0.35 +
        f.buildSpace * 0.18;

    const bool strongPreparation =
        latent >= 28.0 ||
        f.chainUnit4 >= 2.0 ||
        (f.chainUnit4 >= 1.0 &&
         f.chainUnit5 >= 1.0 &&
         f.futureChainSpace >= 8.0);
    if (!strongPreparation) return 0.0;

    const double strength = std::clamp((latent - 28.0) / 26.0, 0.0, 1.0);
    static constexpr double base[] = {
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        28000.0, // 7
        50000.0, // 8
        78000.0  // 9
    };
    return base[chains] * (0.55 + 0.45 * strength);
}

std::uint64_t fastBoardHash(const Board& b) {
    std::uint64_t h = 1469598103934665603ULL;
    for (int y = 0; y < BOARD_HEIGHT; ++y) {
        for (int x = 0; x < BOARD_WIDTH; ++x) {
            h ^= static_cast<std::uint64_t>(static_cast<int>(b.get(x, y)) + 1);
            h *= 1099511628211ULL;
        }
    }
    return h;
}

std::vector<Node> expandNode(
    const Node& parent,
    const PuyoPair& pair,
    const std::vector<PuyoPair>& remainingPieces,
    const Weights& weights,
    int nextDepth,
    int maxDepth
) {
    const auto moves = generateLegalMoves(parent.board, pair);
    std::vector<Node> safe;
    std::vector<Node> death;
    safe.reserve(moves.size());
    death.reserve(moves.size());

    for (const Move& move : moves) {
        const SimulationResult sim = Simulator::drop(
            parent.board, pair, move);

        const bool deathMove = sim.gameOver && !sim.allClear;
        EvaluationContext ctx;
        // The trigger planner is deliberately limited to the same three
        // visible pairs a human-style policy is allowed to use.
        const std::size_t lookStart = 1;
        const std::size_t lookEnd = std::min(
            remainingPieces.size(), lookStart + static_cast<std::size_t>(3));
        if (lookStart < lookEnd) {
            ctx.lookahead.assign(
                remainingPieces.begin() + static_cast<std::ptrdiff_t>(lookStart),
                remainingPieces.begin() + static_cast<std::ptrdiff_t>(lookEnd));
        }
        ctx.quiescenceDepth = (nextDepth >= maxDepth) ? 3 : 0;

        const Features childFeatures = extractStaticFeatures(sim.board);
        const double cashoutPenalty = contextualCashoutPenalty(parent, sim.chains);
        double local = evaluate(sim.board, weights, ctx, &childFeatures)
                     + actionPenalty(parent.board, sim, move, weights,
                                     parent.hasFeatures ? &parent.features : nullptr,
                                     &childFeatures)
                     + chainReward(sim.chains)
                     - cashoutPenalty;

        if (deathMove) local -= kDeathPenalty;

        Node candidate;
        candidate.features = childFeatures;
        candidate.hasFeatures = true;
        candidate.board = sim.board;
        candidate.boardHash = fastBoardHash(candidate.board);
        candidate.root = parent.root.valid ? parent.root : move;
        candidate.maxChain = std::max(parent.maxChain, sim.chains);
        candidate.lastChain = sim.chains;
        candidate.triggerRoute = parent.triggerRoute;
        candidate.longPotential = 0.0;
        candidate.mainChainScore = 0.0;
        candidate.construction = 0.0;
        candidate.prematureRisk = 0.0;
        candidate.score = parent.score + local;
        candidate.futureSafeMoves = -1;
        candidate.previousFutureSafeMoves = parent.hasSurvival ? parent.futureSafeMoves : -1;
        candidate.previousFutureGeometricMoves = parent.hasSurvival ? parent.futureGeometricMoves : -1;
        candidate.rootFutureSafeMoves = parent.rootFutureSafeMoves;
        candidate.rootSurvivalScore = parent.rootSurvivalScore;
        candidate.hasRootSurvival = parent.hasRootSurvival;
        candidate.futureGeometricMoves = -1;
        candidate.bestNextGeometricMoves = -1;
        candidate.bestNextSafeMoves = -1;
        candidate.survivalScore = 0.0;
        candidate.hasSurvival = false;
        candidate.rootTrueTriggerPath = parent.rootTrueTriggerPath;
        candidate.rootTrueImmediateChains = parent.rootTrueImmediateChains;
        candidate.rootTrueFollowupChains = parent.rootTrueFollowupChains;
        candidate.rootTriggerRoute = parent.rootTriggerRoute;
        candidate.rootMaxHeight = parent.rootMaxHeight;
        candidate.rootDangerHeight = parent.rootDangerHeight;
        candidate.hasRootTrueProbe = parent.hasRootTrueProbe;
        candidate.hasRootRouteProbe = parent.hasRootRouteProbe;
        candidate.historicalStale = parent.historicalStale;
        candidate.previousStaleRouteAge = parent.staleRouteAge;
        candidate.previousBestNextSafeMoves = parent.hasSurvival ? parent.bestNextSafeMoves : -1;
        candidate.currentTriggerRoute = parent.currentTriggerRoute;
        candidate.hasCurrentRouteProbe = parent.hasCurrentRouteProbe;
        candidate.staleRouteAge = parent.staleRouteAge;
        candidate.routeRecovery = parent.routeRecovery;
        candidate.rootRouteRecovery = parent.rootRouteRecovery;
        candidate.hasRootRouteRecovery = parent.hasRootRouteRecovery;
        candidate.rebuildPolicy = parent.rebuildPolicy;
        candidate.productiveNextMoves = 0;
        candidate.productiveFollowupMoves = 0;
        candidate.bestImmediateNetClear = 0;
        candidate.bestImmediatePostSafeMoves = -1;
        candidate.bestFollowupNetClear = 0;
        candidate.bestRebuildChain = 0;
        candidate.bestRebuildNetClear = 0;
        candidate.bestRebuildNextSafeMoves = -1;
        candidate.recoveryScore = 0.0;
        candidate.hasRecoveryProbe = false;
        candidate.gameOver = deathMove;

        if (deathMove) death.push_back(std::move(candidate));
        else safe.push_back(std::move(candidate));
    }

    // Critical fallback rule: death placements are ignored whenever at least
    // one safe placement exists. If none exists, return the death candidates
    // so the AI can still place the current pair and let the game end naturally
    // instead of producing an invalid/no-op move.
    if (!safe.empty()) return safe;
    return death;
}

bool recoveryReserveCandidate(const Node& n) {
    if (!n.hasRecoveryProbe) return false;
    if (n.routeRecovery.status == RouteRecoveryStatus::Weak &&
        (n.routeRecovery.deltaSafeMoves > 0 ||
         n.routeRecovery.deltaBestNextSafeMoves > 0 ||
         n.productiveFollowupMoves > 0)) {
        return true;
    }
    if ((n.routeRecovery.status == RouteRecoveryStatus::Stale ||
         n.routeRecovery.status == RouteRecoveryStatus::Abandon) &&
        (n.routeRecovery.deltaSafeMoves >= 1 ||
         n.routeRecovery.deltaBestNextSafeMoves >= 1 ||
         n.productiveFollowupMoves > 0 ||
         n.bestRebuildChain > 0)) {
        return true;
    }
    return false;
}

double survivalCorrection(const Node& n) {
    if (!n.hasSurvival) return 0.0;
    const double asset = std::clamp(
        n.features.chainUnit4 + 0.5 * n.features.chainUnit5 +
        0.35 * n.features.handoffPotential,
        0.0, 6.0
    );
    const double protection = 1.0 - 0.08 * asset;
    const bool imminent = n.futureSafeMoves >= 0 && n.futureSafeMoves <= 1;
    const bool narrow = n.futureSafeMoves >= 0 && n.futureSafeMoves <= 3;
    const bool collapsing =
        n.futureSafeMoves >= 0 && n.futureSafeMoves <= 3 &&
        n.previousFutureSafeMoves >= 0 &&
        n.previousFutureSafeMoves - n.futureSafeMoves >= 2;
    const bool weakEscape =
        n.futureSafeMoves >= 0 && n.futureSafeMoves <= 3 &&
        n.bestNextSafeMoves >= 0 && n.bestNextSafeMoves <= 1;
    if (!imminent && !narrow && !collapsing && !weakEscape) return 0.0;
    return n.survivalScore * std::clamp(protection, 0.52, 1.0);
}

double rootSafetyCorrection(const Node& n) {
    if (!n.hasRootSurvival || n.rootFutureSafeMoves < 0 || n.rootFutureSafeMoves > 5)
        return 0.0;

    static constexpr double safePenalty[] = {
        -30000.0, // 0
        -12000.0, // 1
        -3500.0,  // 2
        -1000.0,  // 3
        0.0,      // 4
        0.0,      // 5+
        0.0
    };
    double score = safePenalty[std::clamp(n.rootFutureSafeMoves, 0, 6)];

    const int rootNextSafe = n.rootBestNextSafeMoves >= 0
        ? n.rootBestNextSafeMoves : n.bestNextSafeMoves;
    if (rootNextSafe >= 0 && n.rootFutureSafeMoves <= 4) {
        if (rootNextSafe <= 0) score -= 5000.0;
        else if (rootNextSafe == 1) score -= 2500.0;
        else if (rootNextSafe == 2) score -= 1000.0;
    }
    score += std::clamp(0.35 * n.rootSurvivalScore, -20000.0, 0.0);

    if (n.hasSurvival && n.futureSafeMoves >= 8) score *= 0.10;
    else if (n.hasSurvival && n.futureSafeMoves >= 6) score *= 0.25;
    return std::clamp(score, -50000.0, 0.0);
}

double beamUtility(const Node& n) {
    return n.score + survivalCorrection(n) + rootSafetyCorrection(n);
}

bool betterForBeam(const Node& a, const Node& b) {
    const double ua = beamUtility(a);
    const double ub = beamUtility(b);
    if (ua != ub) return ua > ub;
    if (a.maxChain != b.maxChain) return a.maxChain > b.maxChain;
    return a.lastChain > b.lastChain;
}

bool sameRootAction(const Move& a, const Move& b) {
    return a.valid == b.valid && a.x == b.x && a.rotation == b.rotation;
}

// Preserve root identities before filling the beam by utility. This is the
// key structural change: diversity is enforced while the frontier is alive,
// rather than trying to recover discarded roots at the very end.
void pruneBeam(std::vector<Node>& candidates, int beamWidth) {
    if (static_cast<int>(candidates.size()) <= beamWidth) return;

    std::sort(candidates.begin(), candidates.end(), betterForBeam);
    std::vector<Node> selected;
    selected.reserve(static_cast<std::size_t>(beamWidth));

    // Keep a moderate root-action reserve. We intentionally do not preserve
    // every legal root: with a 16-node active beam that would consume almost
    // the whole frontier and recreate the over-diversification problem seen
    // in the previous version. The remainder stays globally ranked by utility.
    const int rootReserve = std::min(8, std::max(1, beamWidth / 2));
    bool seenRoot[BOARD_WIDTH][4]{};
    for (const auto& node : candidates) {
        if (static_cast<int>(selected.size()) >= rootReserve) break;
        if (!node.root.valid || node.root.x < 0 || node.root.x >= BOARD_WIDTH ||
            node.root.rotation < 0 || node.root.rotation >= 4) {
            continue;
        }
        if (seenRoot[node.root.x][node.root.rotation]) continue;
        seenRoot[node.root.x][node.root.rotation] = true;
        selected.push_back(node);
    }

    // In the genuine danger zone, add a tiny survival reserve after root
    // diversity has already been secured. Survival therefore cannot erase the
    // long-chain root set, and safety is not allowed to become a broad scalar
    // reward across the ordinary search.
    bool dangerPresent = false;
    for (const auto& node : candidates) {
        const auto h = node.board.heights();
        if (h[2] >= 8 || *std::max_element(h.begin(), h.end()) >= 10) {
            dangerPresent = true;
            break;
        }
    }

    bool recoveryPresent = false;
    for (const auto& node : candidates) { if (recoveryReserveCandidate(node)) { recoveryPresent = true; break; } }

    if (dangerPresent || recoveryPresent) {
        std::vector<const Node*> safety;
        safety.reserve(candidates.size());
        for (const auto& node : candidates) {
            if (node.hasSurvival) safety.push_back(&node);
        }
        std::sort(safety.begin(), safety.end(), [](const Node* a, const Node* b) {
            if (recoveryReserveCandidate(*a) != recoveryReserveCandidate(*b)) return recoveryReserveCandidate(*a) > recoveryReserveCandidate(*b);
            if (a->recoveryScore != b->recoveryScore) return a->recoveryScore > b->recoveryScore;
            const int as = a->futureSafeMoves;
            const int bs = b->futureSafeMoves;
            if (as != bs) return as > bs;
            if (a->bestNextSafeMoves != b->bestNextSafeMoves) return a->bestNextSafeMoves > b->bestNextSafeMoves;
            if (a->maxChain != b->maxChain) return a->maxChain > b->maxChain;
            return a->score > b->score;
        });

        const int reserve = std::min(recoveryPresent ? 2 : 1, std::max(0, beamWidth - rootReserve));
        for (const Node* node : safety) {
            if (static_cast<int>(selected.size()) >= rootReserve + reserve) break;
            bool duplicateBoard = false;
            for (const auto& existing : selected) {
                if (existing.boardHash == node->boardHash &&
                    sameRootAction(existing.root, node->root)) {
                    duplicateBoard = true;
                    break;
                }
            }
            if (!duplicateBoard) selected.push_back(*node);
        }
    }

    // Fill by the ordinary beam utility. All root diversity decisions above
    // are pure reserves: they do not permanently boost weak nodes.
    for (const auto& node : candidates) {
        if (static_cast<int>(selected.size()) >= beamWidth) break;
        bool duplicate = false;
        for (const auto& existing : selected) {
            if (existing.boardHash == node.boardHash &&
                sameRootAction(existing.root, node.root)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) selected.push_back(node);
    }

    candidates.swap(selected);
}

struct SurvivalCacheKey {
    std::uint64_t board = 0;
    std::uint16_t pair = 0;
    std::uint16_t nextNextPair = 0;
    bool operator==(const SurvivalCacheKey& other) const {
        return board == other.board &&
               pair == other.pair &&
               nextNextPair == other.nextNextPair;
    }
};

struct SurvivalCacheKeyHash {
    std::size_t operator()(const SurvivalCacheKey& key) const {
        std::uint64_t x =
            key.board ^
            (static_cast<std::uint64_t>(key.pair) * 0x9e3779b97f4a7c15ULL) ^
            (static_cast<std::uint64_t>(key.nextNextPair) * 0xbf58476d1ce4e5b9ULL);
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        return static_cast<std::size_t>(x ^ (x >> 31));
    }
};

void applySurvivalProbe(
    std::vector<Node>& candidates,
    const std::vector<PuyoPair>& pieces,
    int depth,
    int probeLimit,
    std::unordered_map<SurvivalCacheKey, SurvivalHorizon, SurvivalCacheKeyHash>& cache
) {
    if (candidates.empty() || probeLimit <= 0 || depth >= static_cast<int>(pieces.size())) return;
    const PuyoPair* next = &pieces[static_cast<std::size_t>(depth)];
    const PuyoPair* nextNext = (depth + 1 < static_cast<int>(pieces.size()))
        ? &pieces[static_cast<std::size_t>(depth + 1)] : nullptr;

    const int n = std::min(probeLimit, static_cast<int>(candidates.size()));
    for (int i = 0; i < n; ++i) {
        Node& node = candidates[static_cast<std::size_t>(i)];
        const auto heights = node.board.heights();
        const int maxHeight = *std::max_element(heights.begin(), heights.end());
        if (heights[2] < 8 && maxHeight < 10) continue;
        const int previousSafeMoves = node.previousFutureSafeMoves;
        const int previousGeometricMoves = node.previousFutureGeometricMoves;
        const SurvivalCacheKey key{
            node.boardHash,
            static_cast<std::uint16_t>(
                (static_cast<int>(next->main) << 8) |
                static_cast<int>(next->sub)),
            nextNext
                ? static_cast<std::uint16_t>(
                    (static_cast<int>(nextNext->main) << 8) |
                    static_cast<int>(nextNext->sub))
                : static_cast<std::uint16_t>(0)
        };
        auto it = cache.find(key);
        if (it == cache.end()) {
            it = cache.emplace(key, analyzeSurvivalHorizon(node.board, next, nextNext)).first;
        }
        const SurvivalHorizon& h = it->second;
        node.futureSafeMoves = h.safeMoves;
        node.futureGeometricMoves = h.geometricMoves;
        node.bestNextGeometricMoves = h.bestNextGeometricMoves;
        node.bestNextSafeMoves = h.bestNextSafeMoves;
        node.survivalScore = survivalHorizonScore(h, previousSafeMoves, previousGeometricMoves);
        node.trueTriggerPath = h.trueTriggerPath;
        node.trueImmediateChains = h.trueImmediateChains;
        node.trueFollowupChains = h.trueFollowupChains;
        node.hasTrueProbe = true;
        node.productiveNextMoves = h.productiveNextMoves;
        node.productiveFollowupMoves = h.productiveFollowupMoves;
        node.bestImmediateNetClear = h.bestImmediateNetClear;
        node.bestImmediatePostSafeMoves = h.bestImmediatePostSafeMoves;
        node.bestFollowupNetClear = h.bestFollowupNetClear;
        node.bestRebuildChain = h.bestRebuildChain;
        node.bestRebuildNetClear = h.bestRebuildNetClear;
        node.bestRebuildNextSafeMoves = h.bestRebuildNextSafeMoves;
        if (h.safeMoves >= 0 && h.safeMoves <= 5) {
            node.currentTriggerRoute = triggerRouteLength(node.board);
            node.hasCurrentRouteProbe = true;
        }
        if (node.hasCurrentRouteProbe || h.trueTriggerPath > 0) {
            node.routeRecovery = analyzeRouteRecovery(node.currentTriggerRoute, h.trueTriggerPath, h.safeMoves, previousSafeMoves, h.bestNextSafeMoves, node.previousBestNextSafeMoves, node.previousStaleRouteAge, h.productiveNextMoves, h.productiveFollowupMoves, h.bestImmediateNetClear, h.bestRebuildChain, h.bestRebuildNetClear, h.bestRebuildNextSafeMoves, maxHeight, node.rebuildPolicy);
            node.staleRouteAge = node.routeRecovery.staleAge;
            node.recoveryScore = node.routeRecovery.score;
            node.hasRecoveryProbe = true;
        }

        if (depth == 1) {
            node.rootMaxHeight = maxHeight;
            node.rootDangerHeight = heights[2];
            const bool emergencyProbe =
                h.safeMoves >= 0 && h.safeMoves <= 5 &&
                (maxHeight >= 11 || heights[2] >= 9 ||
                 (h.bestNextSafeMoves >= 0 && h.bestNextSafeMoves <= 2));
            if (emergencyProbe) {
                node.rootTrueTriggerPath = h.trueTriggerPath;
                node.rootTrueImmediateChains = h.trueImmediateChains;
                node.rootTrueFollowupChains = h.trueFollowupChains;
                node.hasRootTrueProbe = true;
                node.rootTriggerRoute = triggerRouteLength(node.board);
                node.hasRootRouteProbe = true;
            }
        }

        if (depth == 1) {
            node.rootFutureSafeMoves = h.safeMoves;
            node.rootBestNextSafeMoves = h.bestNextSafeMoves;
            node.rootSurvivalScore = node.survivalScore;
            node.hasRootSurvival = true;
            if (node.hasRecoveryProbe) { node.rootRouteRecovery = node.routeRecovery; node.hasRootRouteRecovery = true; }
        }
        node.hasSurvival = true;
    }
}

void applyVirtualRerank(std::vector<Node>& beam, int topM) {
    if (beam.empty() || topM <= 0) return;
    std::sort(beam.begin(), beam.end(), betterForBeam);
    const int n = std::min(topM, static_cast<int>(beam.size()));
    for (int i = 0; i < n; ++i) {
        Node& node = beam[static_cast<std::size_t>(i)];
        if (!node.hasVirtual) {
            node.virtualFeatures = analyzeVirtualChainPotential(node.board);
            node.virtualPotential = virtualChainPotentialScore(
                node.virtualFeatures, node.board);
            node.hasVirtual = true;
        }
    }
}

std::string debugBoard(const Board& board) {
    std::string out;
    out.reserve(BOARD_WIDTH * (BOARD_HEIGHT + 1));
    for (int y = BOARD_HEIGHT - 1; y >= 0; --y) {
        for (int x = 0; x < BOARD_WIDTH; ++x) {
            const int v = static_cast<int>(board.get(x, y));
            out += (v >= 1 && v <= 4) ? char('0' + v) : (v == 5 ? '#' : '.');
        }
        out += '\n';
    }
    return out;
}

double finalUtility(const Node& n);

void debugBeamSummary(const std::vector<Node>& beam, int depth, int beamWidth) {
    if (!debugLoggingEnabled()) return;
    std::vector<const Node*> ranked;
    ranked.reserve(beam.size());
    for (const auto& n : beam) ranked.push_back(&n);
    std::sort(ranked.begin(), ranked.end(), [](const Node* a, const Node* b) {
        const double ua = finalUtility(*a);
        const double ub = finalUtility(*b);
        if (ua != ub) return ua > ub;
        return a->maxChain > b->maxChain;
    });

    std::ostringstream oss;
    oss << "\n[AI-DEBUG] beam depth=" << depth
        << " size=" << beam.size()
        << " beamWidth=" << beamWidth << '\n';
    const std::size_t n = std::min<std::size_t>(ranked.size(), 8);
    for (std::size_t i = 0; i < n; ++i) {
        const Node& x = *ranked[i];
        oss << "  #" << (i + 1)
            << " root=(" << x.root.x << "," << x.root.rotation << ")"
            << " utility=" << finalUtility(x)
            << " score=" << x.score
            << " maxChain=" << x.maxChain
            << " lastChain=" << x.lastChain
            << " route=" << x.triggerRoute
            << " longPotential=" << x.longPotential
            << " virtual=" << x.virtualPotential
            << " vBest=" << x.virtualFeatures.bestChain
            << " vTop3=" << x.virtualFeatures.top3ChainSum
            << " viability=" << x.triggerViabilityScore
            << " vPath=" << x.triggerViability.bestPath
            << " vTrig=" << x.triggerViability.viableTriggers
            << " safeNext=" << x.futureSafeMoves
            << " prevSafe=" << x.previousFutureSafeMoves
            << " geomNext=" << x.futureGeometricMoves
            << " next2Geom=" << x.bestNextGeometricMoves
            << " next2Safe=" << x.bestNextSafeMoves
            << " survival=" << x.survivalScore
            << " truePath=" << x.trueTriggerPath
            << " trueNow=" << x.trueImmediateChains
            << " trueFollow=" << x.trueFollowupChains
            << " trueProbe=" << (x.hasTrueProbe ? 1 : 0)
            << " rootTruePath=" << x.rootTrueTriggerPath
            << " rootTrueNow=" << x.rootTrueImmediateChains
            << " rootTrueFollow=" << x.rootTrueFollowupChains
            << " rootTrueProbe=" << (x.hasRootTrueProbe ? 1 : 0)
            << " rootRoute=" << x.rootTriggerRoute
            << " rootRouteProbe=" << (x.hasRootRouteProbe ? 1 : 0)
            << " recovery=" << routeRecoveryStatusName(x.routeRecovery.status)
            << " staleAge=" << x.staleRouteAge
            << " recoveryScore=" << x.recoveryScore
            << " rootSafe=" << x.rootFutureSafeMoves
            << " structure=" << x.structure
            << " mainChain=" << x.mainChain.length()
            << " mainContinuity=" << x.mainChainScore
            << " gameOver=" << (x.gameOver ? 1 : 0) << '\n';
    }
    debugLog(oss.str());
}

double finalUtility(const Node& n) {
    const double survival = survivalCorrection(n) + rootSafetyCorrection(n);

    double constructionGate = 1.0;
    if (n.hasVirtual) {
        if (n.virtualPotential < -20000.0) constructionGate = 0.22;
        else if (n.virtualPotential < 0.0) constructionGate = 0.38;
        else if (n.virtualPotential < 20000.0) constructionGate = 0.62;
        else if (n.virtualPotential < 60000.0) constructionGate = 0.84;
    }

    double routeGate = 1.0;
    const int visibleTruePath = n.hasTrueProbe
        ? n.trueTriggerPath
        : (n.hasRootTrueProbe ? n.rootTrueTriggerPath : -1);
    const auto h = n.board.heights();
    const int maxHeight = *std::max_element(h.begin(), h.end());
    const bool currentDanger = n.hasSurvival
        ? ((n.futureSafeMoves >= 0 && n.futureSafeMoves <= 5) ||
           (n.bestNextSafeMoves >= 0 && n.bestNextSafeMoves <= 2))
        : (h[2] >= 9 || maxHeight >= 11);
    if (currentDanger && visibleTruePath >= 0) {
        if (visibleTruePath <= 0) routeGate = 0.22;
        else if (visibleTruePath == 1) routeGate = 0.55;
        else if (visibleTruePath == 2) routeGate = 0.78;
    }
    const bool currentRecoveryDanger =
        n.hasSurvival && n.futureSafeMoves >= 0 && n.futureSafeMoves <= 5;
    if (currentRecoveryDanger && n.routeRecovery.status == RouteRecoveryStatus::Stale) {
        routeGate = std::min(routeGate, 0.18);
    } else if (currentRecoveryDanger && n.routeRecovery.status == RouteRecoveryStatus::Abandon) {
        routeGate = std::min(routeGate, 0.10);
    }

    const double viability = n.hasTriggerViability
        ? n.triggerViabilityScore * routeGate
        : 0.0;

    const double constructionRaw =
        static_cast<double>(n.mainChain.length()) * 16000.0 +
        n.mainChainScore * 0.50 +
        n.construction * 0.06 -
        n.prematureRisk * 0.06;
    const double gatedConstruction = constructionRaw >= 0.0
        ? constructionRaw * constructionGate * routeGate
        : constructionRaw * constructionGate;

    double gatedVirtual = n.virtualPotential;
    if (currentDanger && n.hasTrueProbe && gatedVirtual > 0.0) {
        gatedVirtual *= routeGate;
    }

    return n.score + static_cast<double>(n.maxChain) * 25000.0
         + gatedVirtual
         + gatedConstruction
         + viability * (constructionGate < 0.65 ? 0.72 : 0.22)
         + survival;
}

bool betterFinal(const Node& a, const Node& b) {
    const double ua = finalUtility(a);
    const double ub = finalUtility(b);
    if (ua != ub) return ua > ub;
    if (a.structure != b.structure) return a.structure > b.structure;
    return a.maxChain > b.maxChain;
}

struct RootBoardKey {
    std::uint64_t board = 0;
    std::uint8_t x = 0;
    std::uint8_t rotation = 0;
    bool operator==(const RootBoardKey& other) const {
        return board == other.board && x == other.x && rotation == other.rotation;
    }
};

struct RootBoardKeyHash {
    std::size_t operator()(const RootBoardKey& key) const {
        std::uint64_t x = key.board;
        x ^= static_cast<std::uint64_t>(key.x + 1) * 0x9e3779b97f4a7c15ULL;
        x ^= static_cast<std::uint64_t>(key.rotation + 1) * 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        return static_cast<std::size_t>(x ^ (x >> 31));
    }
};

Move chooseRoot(
    const Board& board,
    const std::vector<PuyoPair>& pieces,
    const Weights& weights,
    int maxDepth,
    int beamWidth,
    const GameHistory& history
) {
    if (pieces.empty()) return {-1, 0, false};

    const int horizon = std::min(
        maxDepth,
        static_cast<int>(pieces.size())
    );

    // Keep a bounded active frontier so the browser remains practical, but
    // give the search more room than the previous 12-node cap. The extra
    // capacity is used for strong continuations, not as a survival score.
    const int activeBeamWidth = std::min(beamWidth, 16);
    if (horizon <= 0) return {-1, 0, false};

    Node root;
    root.board = board;
    root.boardHash = fastBoardHash(root.board);
    root.historicalStale =
        history.quietTurns >= 2 &&
        history.lastActualChain <= 1 &&
        history.clearDebtScore() >= 24;
    root.rebuildPolicy = history.inRebuild();
    root.features = extractStaticFeatures(board);
    root.hasFeatures = true;
    root.mainChain = analyzeMainChain(board);

    std::vector<Node> beam = {root};
    std::unordered_map<SurvivalCacheKey, SurvivalHorizon, SurvivalCacheKeyHash> survivalCache;
    survivalCache.reserve(static_cast<std::size_t>(activeBeamWidth * 8));

    for (int depth = 0; depth < horizon; ++depth) {
        std::vector<Node> next;
        next.reserve(static_cast<std::size_t>(activeBeamWidth) * 24U);

        for (const Node& node : beam) {
            std::vector<PuyoPair> remainingPieces;
            const std::size_t start = static_cast<std::size_t>(depth);
            const std::size_t end = std::min(pieces.size(), start + 3);
            remainingPieces.assign(pieces.begin() + static_cast<std::ptrdiff_t>(start),
                                   pieces.begin() + static_cast<std::ptrdiff_t>(end));
            auto children = expandNode(
                node, pieces[depth], remainingPieces, weights, depth + 1, horizon);
            for (auto& child : children) next.push_back(std::move(child));
        }

        if (next.empty()) return {-1, 0, false};

        if (depth == 0) {
            applyVirtualRerank(next, std::min(14, activeBeamWidth));
            applySurvivalProbe(next, pieces, depth + 1,
                               static_cast<int>(next.size()), survivalCache);
            std::sort(next.begin(), next.end(), [](const Node& a, const Node& b) {
                return finalUtility(a) > finalUtility(b);
            });
        }

        // Root-aware transposition reduction. At this point board hash alone is
        // no longer a sufficient search-state identity: two different root
        // actions can reach the same board and still represent different first
        // moves. Keep the strongest representative *per root action*.
        std::unordered_map<RootBoardKey, std::size_t, RootBoardKeyHash> transpositions;
        transpositions.reserve(next.size());
        std::vector<Node> uniqueNext;
        uniqueNext.reserve(next.size());
        for (auto& candidate : next) {
            const RootBoardKey key{
                candidate.boardHash,
                static_cast<std::uint8_t>(std::clamp(candidate.root.x, 0, 255)),
                static_cast<std::uint8_t>(std::clamp(candidate.root.rotation, 0, 255))
            };
            const auto it = transpositions.find(key);
            if (it == transpositions.end()) {
                transpositions.emplace(key, uniqueNext.size());
                uniqueNext.push_back(std::move(candidate));
            } else {
                Node& existing = uniqueNext[it->second];
                if (betterForBeam(candidate, existing)) {
                    existing = std::move(candidate);
                }
            }
        }
        next.swap(uniqueNext);

        if (depth > 0) {
            bool dangerPresent = false;
            for (const auto& candidate : next) {
                const auto h = candidate.board.heights();
                const int maxH = *std::max_element(h.begin(), h.end());
                if (h[2] >= 8 || maxH >= 10) {
                    dangerPresent = true;
                    break;
                }
            }
            const int probeLimit = dangerPresent
                ? static_cast<int>(next.size())
                : std::min(12, activeBeamWidth);
            applySurvivalProbe(next, pieces, depth + 1, probeLimit, survivalCache);
        }

        pruneBeam(next, activeBeamWidth);

        beam.swap(next);

        if (depth + 1 >= 2) {
            applyVirtualRerank(beam, std::min(10, activeBeamWidth));
            std::sort(beam.begin(), beam.end(), [](const Node& a, const Node& b) {
                return finalUtility(a) > finalUtility(b);
            });
            if (static_cast<int>(beam.size()) > activeBeamWidth) {
                beam.resize(static_cast<std::size_t>(activeBeamWidth));
            }
        }

        debugBeamSummary(beam, depth + 1, activeBeamWidth);

        bool allDead = true;
        for (const auto& node : beam) {
            if (!node.gameOver) {
                allDead = false;
                break;
            }
        }
        if (allDead) break;
    }

    applyVirtualRerank(beam, std::min(16, static_cast<int>(beam.size())));
    if (horizon < static_cast<int>(pieces.size())) {
        applySurvivalProbe(beam, pieces, horizon, static_cast<int>(beam.size()), survivalCache);
    }
    std::sort(beam.begin(), beam.end(), [](const Node& a, const Node& b) {
        return finalUtility(a) > finalUtility(b);
    });

    // Evaluate one structurally best candidate per root first. This keeps the
    // final expensive stage root-diverse too, instead of preserving diversity
    // in the early beam and then collapsing it again at terminal scoring.
    std::vector<int> structuralIndices;
    structuralIndices.reserve(10);
    bool seenRoot[BOARD_WIDTH][4]{};
    const int structuralLimit = std::min(10, static_cast<int>(beam.size()));
    for (int i = 0; i < static_cast<int>(beam.size()) &&
                    static_cast<int>(structuralIndices.size()) < structuralLimit; ++i) {
        const Node& node = beam[static_cast<std::size_t>(i)];
        if (!node.root.valid || node.root.x < 0 || node.root.x >= BOARD_WIDTH ||
            node.root.rotation < 0 || node.root.rotation >= 4) continue;
        if (seenRoot[node.root.x][node.root.rotation]) continue;
        seenRoot[node.root.x][node.root.rotation] = true;
        structuralIndices.push_back(i);
    }

    for (const int index : structuralIndices) {
        Node& node = beam[static_cast<std::size_t>(index)];
        node.triggerRoute = triggerRouteLength(node.board);
        node.longPotential = longChainPotential(node.board, {});
        node.mainChain = analyzeMainChain(node.board);
        node.construction = mainChainConstructionScore(node.board, node.mainChain);
        node.prematureRisk = prematureMainChainTriggerRisk(node.board, node.mainChain);
        node.structure += postTriggerTailScore(node.board) * 0.05;
        node.structure -= prematureTriggerRisk(node.board) * 0.03;
        node.mainChainScore = mainChainConstructionScore(node.board, node.mainChain) * 0.05;
    }

    for (const int index : structuralIndices) {
        Node& node = beam[static_cast<std::size_t>(index)];
        node.triggerViability = analyzeTriggerViability(node.board, node.triggerRoute);
        node.triggerViabilityScore = triggerViabilityScore(node.triggerViability);
        node.hasTriggerViability = true;
    }

    const auto best = std::max_element(
        beam.begin(), beam.end(),
        [](const Node& a, const Node& b) {
            return betterFinal(b, a);
        }
    );

    if (best == beam.end() || !best->root.valid) {
        if (debugLoggingEnabled()) debugLog("[AI-DEBUG] no valid root move");
        return {-1, 0, false};
    }

    // Safety intervention is intentionally tiny. Root diversity is now the
    // normal safeguard. Only a genuinely critical 0-1 safe-move state may
    // trigger a root escape, and the escape must preserve meaningful chain
    // value. This removes the broad stale-route Rescue that previously caused
    // several 12->9, 12->8 and 13->11 regressions.
    const Node* selected = &(*best);
    bool criticalEscapeApplied = false;

    if (best->hasRootSurvival &&
        best->rootFutureSafeMoves >= 0 && best->rootFutureSafeMoves <= 2) {
        const Node* escape = nullptr;
        for (const auto& node : beam) {
            if (!node.root.valid || !node.hasRootSurvival) continue;
            if (node.rootFutureSafeMoves < 3) continue;
            if (node.maxChain + 2 < best->maxChain) continue;

            const double chainAsset =
                node.features.chainUnit4 +
                0.5 * node.features.chainUnit5 +
                0.35 * node.features.handoffPotential;
            const bool productive =
                (node.hasVirtual && node.virtualPotential >= 10000.0) ||
                node.triggerRoute >= 5 ||
                chainAsset >= 1.25 ||
                node.lastChain > 0;
            if (!productive) continue;
            if (node.score + 120000.0 < best->score && node.maxChain <= best->maxChain)
                continue;

            if (!escape ||
                node.maxChain > escape->maxChain ||
                (node.maxChain == escape->maxChain &&
                 node.rootFutureSafeMoves > escape->rootFutureSafeMoves) ||
                (node.maxChain == escape->maxChain &&
                 node.rootFutureSafeMoves == escape->rootFutureSafeMoves &&
                 finalUtility(node) > finalUtility(*escape))) {
                escape = &node;
            }
        }
        if (escape) {
            selected = escape;
            criticalEscapeApplied = true;
        }
    }

    bool recoveryEscapeApplied = false;
    if (!criticalEscapeApplied && best->hasRootRouteRecovery &&
        (best->rootRouteRecovery.status == RouteRecoveryStatus::Stale || best->rootRouteRecovery.status == RouteRecoveryStatus::Abandon || best->rootRouteRecovery.status == RouteRecoveryStatus::Weak)) {
        const Node* escape = nullptr;
        for (const auto& node : beam) {
            if (!node.root.valid || !node.hasRootSurvival || !node.hasRootRouteRecovery) continue;
            const bool reconnected = node.rootRouteRecovery.status == RouteRecoveryStatus::Connected || node.rootTrueTriggerPath >= 2;
            const bool safer = node.rootFutureSafeMoves >= best->rootFutureSafeMoves + 2;
            const bool productive = node.bestRebuildChain > 0 || node.productiveFollowupMoves > 0 || node.rootTrueImmediateChains > 0 || node.rootTrueFollowupChains > 0;
            if (!reconnected && !safer) continue;
            if (!productive && !safer) continue;
            if (node.maxChain + 4 < best->maxChain && !safer) continue;
            if (!escape || node.rootFutureSafeMoves > escape->rootFutureSafeMoves ||
                (node.rootFutureSafeMoves == escape->rootFutureSafeMoves && node.rootRouteRecovery.score > escape->rootRouteRecovery.score) ||
                (node.rootFutureSafeMoves == escape->rootFutureSafeMoves && node.rootRouteRecovery.score == escape->rootRouteRecovery.score && node.maxChain > escape->maxChain)) escape = &node;
        }
        if (escape) { selected = escape; recoveryEscapeApplied = true; }
    }

    if (debugLoggingEnabled()) {
        std::ostringstream oss;
        oss << "[AI-DEBUG] SELECT root=(" << selected->root.x << "," << selected->root.rotation
            << ") utility=" << finalUtility(*selected)
            << " score=" << selected->score
            << " maxChain=" << selected->maxChain
            << " lastChain=" << selected->lastChain
            << " route=" << selected->triggerRoute
            << " longPotential=" << selected->longPotential
            << " structure=" << selected->structure
            << " mainChain=" << selected->mainChain.length()
            << " construction=" << selected->construction
            << " prematureRisk=" << selected->prematureRisk
            << " viability=" << selected->triggerViabilityScore
            << " vPath=" << selected->triggerViability.bestPath
            << " rootSafe=" << selected->rootFutureSafeMoves
            << " rootTruePath=" << selected->rootTrueTriggerPath
            << " rootTrueProbe=" << (selected->hasRootTrueProbe ? 1 : 0)
            << " rootRoute=" << selected->rootTriggerRoute
            << " rootRouteProbe=" << (selected->hasRootRouteProbe ? 1 : 0)
            << " routeRecovery=" << routeRecoveryStatusName(selected->routeRecovery.status)
            << " staleAge=" << selected->staleRouteAge
            << " recoveryScore=" << selected->recoveryScore
            << " rootRecovery=" << routeRecoveryStatusName(selected->rootRouteRecovery.status)
            << " historicalStale=" << (selected->historicalStale ? 1 : 0)
            << " criticalEscape=" << (criticalEscapeApplied ? 1 : 0)
            << " recoveryEscape=" << (recoveryEscapeApplied ? 1 : 0)
            << " mainRoute=";
        for (std::size_t i = 0; i < selected->mainChain.colors.size(); ++i) {
            if (i) oss << "->";
            oss << selected->mainChain.colors[i];
        }
        oss << " mainContinuity=" << selected->mainChainScore
            << " gameOver=" << (selected->gameOver ? 1 : 0) << "\n"
            << "[AI-DEBUG] selected board (top->bottom):\n"
            << debugBoard(selected->board);
        debugLog(oss.str());
    }
    return selected->root;
}

} // namespace

Move BeamSearch::chooseMove(
    const Board& board,
    const std::vector<PuyoPair>& pieces,
    const Weights& weights,
    int depth,
    int beamWidth,
    const GameHistory& history
) const {
    return chooseRoot(board, pieces, weights,
                      std::clamp(depth, 1, 50),
                      std::clamp(beamWidth, 1, 500),
                      history);
}

} // namespace puyo
