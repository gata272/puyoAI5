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
namespace {

// ---------------------------------------------------------------------------
// Fire-decision layer (v26)
//
// Diagnosis (fixed-seed benchmark, seed 20260908, 10 games x 100 turns,
// 30% game-over): every death was classified `no_safe_move`, never
// `selected_death_with_safe_move`.  Replaying the decision log with an
// independent simulator (validated 921/921 against the C++ simulator)
// showed the real failure mode: the beam repeatedly *refused to fire*
// chains that were already available -- including 7-9 chains erasing
// 30-46 puyo -- while the board filled up.  By the last ~5 turns before
// death no firing move remained and the AI suffocated at occupied=76-78,
// danger-column height 11-13.
//
// Why the beam does this: a node's score is the *cumulative sum* of the
// static evaluator over depths, and the static evaluator pays up to
// ~300k points per node for latent structure (long-chain potential x1200,
// virtual-chain potential, chain units).  Actually firing a chain
// destroys exactly those features, so the one-off chain reward
// (<= ~360k incl. the final maxChain bonus) is routinely outbid by three
// more depths of "potential".  Potential is never cashed in.
//
// Human players and ama-style AIs separate the two questions: "build"
// (handled by the beam) and "when to fire" (a decision rule).  This layer
// adds that missing rule *outside* the scorer, so no weight tuning can
// silently reintroduce the hoarding failure.  It only overrides the beam
// when a chain is (a) so large that extending it is not worth the risk,
// or (b) needed to survive.  In all other states the beam is untouched.
// ---------------------------------------------------------------------------

int countSafeMoves(const Board& board, const PuyoPair& pair) {
    int safe = 0;
    for (const auto& move : generateLegalMoves(board, pair)) {
        const auto sim = Simulator::drop(board, pair, move);
        if (!sim.gameOver || sim.allClear) ++safe;
    }
    return safe;
}

// Variant W (pro-inspired, round 5): trigger continuity.
//
// Professional procedure (from the pro-technique dataset) is built
// around never losing the ability to fire: "place everything except
// the puyo that buries the trigger first, and bury the trigger last";
// "do not bury the old trigger until the new one is complete".  The
// beam has no such notion: it sometimes plays a quiet building move
// that buries the only exposed trigger group, after which the chain
// can no longer be fired at all -- the burial half of the hoarding
// death pattern.
//
// Board-level fireability: the largest chain obtainable by adding ONE
// puyo of any color on top of any column.  >= 4 means a real chain is
// one puyo away from firing (an exposed trigger).
int maxTriggerChain(const Board& board) {
    int best = 0;
    for (int x = 0; x < BOARD_WIDTH; ++x) {
        const int h = board.height(x);
        if (h >= BOARD_HEIGHT) continue;
        for (int c = 1; c <= 4; ++c) {
            Board b = board;
            b.set(x, h, static_cast<Cell>(c));
            int score = 0, erased = 0;
            const int chains = Simulator::resolveBoard(b, score, erased);
            if (chains > best) best = chains;
        }
    }
    return best;
}

struct FireCandidate {
    Move move{-1, 0, false};
    int chains = 0;
    int erased = 0;
    int postOccupied = 0;
    int postDangerHeight = 0;
    int postSafeMoves = -1;
};

// Returns a rule number (1-9) when the AI must fire now, filling `out`.
// Returns 0 when the beam search should decide (normal building state).
// `dangerChainSinceTurn` is the caller's persistent grace-period state.
int decideFireMove(const Board& board, const std::vector<PuyoPair>& pieces, Move& out,
                   int turn, int& dangerChainSinceTurn) {
    if (pieces.empty()) return 0;
    const auto heights = board.heights();
    const int occupied = std::accumulate(heights.begin(), heights.end(), 0);
    const int dangerHeight = heights[2];  // game-over column (x=2)
    const int maxHeight = *std::max_element(heights.begin(), heights.end());
    const int currentSafe = countSafeMoves(board, pieces.front());

    FireCandidate bestChain;  // maximise chains, then erased, then mobility
    FireCandidate bestClear;  // maximise erased, then chains, then mobility
    FireCandidate bestDig;    // maximise erased, then lowest post danger height
    bool haveChain = false;

    for (const auto& move : generateLegalMoves(board, pieces.front())) {
        const auto sim = Simulator::drop(board, pieces.front(), move);
        if (sim.gameOver && !sim.allClear) continue;  // never fire into death
        if (sim.chains <= 0) continue;
        FireCandidate c;
        c.move = move;
        c.chains = sim.chains;
        c.erased = sim.erased;
        const auto postHeights = sim.board.heights();
        c.postOccupied = std::accumulate(postHeights.begin(), postHeights.end(), 0);
        c.postDangerHeight = postHeights[2];
        c.postSafeMoves = pieces.size() >= 2
            ? countSafeMoves(sim.board, pieces[1]) : 22;
        if (!haveChain ||
            c.chains > bestChain.chains ||
            (c.chains == bestChain.chains && c.erased > bestChain.erased) ||
            (c.chains == bestChain.chains && c.erased == bestChain.erased &&
             c.postSafeMoves > bestChain.postSafeMoves)) {
            bestChain = c;
        }
        if (!haveChain ||
            c.erased > bestClear.erased ||
            (c.erased == bestClear.erased && c.chains > bestClear.chains) ||
            (c.erased == bestClear.erased && c.chains == bestClear.chains &&
             c.postSafeMoves > bestClear.postSafeMoves)) {
            bestClear = c;
        }
        if (!haveChain ||
            c.erased > bestDig.erased ||
            (c.erased == bestDig.erased &&
             c.postDangerHeight < bestDig.postDangerHeight) ||
            (c.erased == bestDig.erased &&
             c.postDangerHeight == bestDig.postDangerHeight &&
             c.postSafeMoves > bestDig.postSafeMoves)) {
            bestDig = c;
        }
        haveChain = true;
    }
    if (!haveChain) {
        dangerChainSinceTurn = -1;
        return 0;
    }

    const int ch = bestChain.chains;
    int rule = 0;
    const FireCandidate* pick = &bestChain;

    // Variant S (simulation-focused backstop): the fire layer stays
    // silent while the beam builds, exactly like the original AI --
    // that undisturbed building is what produced the original's 10-13
    // chain peaks.  It intervenes only when suffocation is actually
    // progressing.  Death in this simulator is column x=2 reaching the
    // top or running out of safe placements, and replay analysis of the
    // original's five death games showed every one of them passed
    // through a state with a >=5 chain available while occupancy and
    // the danger column were both already high -- while peak-chain
    // games showed the same danger-column height at much lower
    // occupancy.  So the trigger couples the danger column with
    // occupancy instead of using either alone, and ignores raw max
    // height (tall edge columns do not kill).
    //
    // Earlier variants tried (a) firing 8+ chains promptly unless a +1
    // extension was visible [best score/frequency, peaks capped at
    // 9-10], (b) deferring by stagnation time, and (c) deferring until
    // a max-height-based danger threshold: (b) and (c) produced
    // identical, worse results -- during deferral the beam does not
    // extend the chain, it fills the board, and moderate-pressure
    // mid-chain fires (old Rules 3/4) still reset the main structure.
    const bool hardDanger = occupied >= 58 || currentSafe <= 6 ||
                            (dangerHeight >= 9 && occupied >= 50);

    // Grace period: firing the moment suffocation starts progressing
    // (variant S) still capped peaks at 9, because peak games also
    // pass through (danger >= 9, occupied >= 50) -- and in the last
    // few turns before the beam's own natural fire the chain often
    // grows by +1 several times.  Death games, by contrast, lingered
    // in that state for 8+ turns (observed margins: 8-45 turns
    // between onset and death).  So track how long a >=4 chain has
    // been available under hard danger and force the fire only after
    // a 4-turn grace period; extreme states skip the grace.
    // Variant U: strip the backstop to the minimum.  Variants S and T
    // still capped peaks at 9 even though their big-chain rules rarely
    // fired -- the remaining suspects were the small-clear rescues
    // (old Rules 5/6): a 1-4 chain fired mid-build erases groups that
    // are links of the main chain, silently resetting the structure
    // that would have become 10+.  So U keeps ONLY:
    //   Rule 1 (14+ chains), Rule 9 (>=4 chains under progressing
    //   suffocation, after the grace period), Rule 7 (desperate).
    // Everything else is the original beam, untouched.
    if (ch >= 4 && hardDanger) {
        if (dangerChainSinceTurn < 0 || turn < dangerChainSinceTurn) {
            dangerChainSinceTurn = turn;
        }
    } else {
        dangerChainSinceTurn = -1;
    }
    // Variant U4: variant U (flicker-reset grace clock, which best
    // preserved peak chains) plus the two targeted rescues that U's
    // death replays called for, neither of which touches the build
    // phase: (a) occupancy >= 60 skips the grace -- U's s777 game 1
    // died holding a 7-chain at occupancy 62-64 because growth and
    // flicker kept the clock from ever expiring; (b) Rule 6' late-fill
    // rescue for boards refilling with no real chain available.
    const bool extreme = occupied >= 60 || currentSafe <= 2;

    if (ch >= 14) {
        // Rule 1: 14+ chains -- beyond any realistic extension.
        rule = 1;
    } else if (ch >= 4 && hardDanger &&
               (extreme || turn - dangerChainSinceTurn >= 4)) {
        // Rule 9: a real chain + suffocation that the beam failed to
        // resolve within the grace period -- fire now.
        rule = 9;
    }
    // Rule 6': late-fill rescue (only when no real chain exists).
    else if (ch <= 3 && (occupied >= 60 || currentSafe <= 4) &&
             bestClear.erased >= 6 && bestClear.postSafeMoves >= 4) {
        rule = 6;
        pick = &bestClear;
    }
    // Rule 8 (variant X, round 6): plateau cleanup.  Long-game replays
    // (user-supplied 500-turn log, death at turn 302) show a failure
    // mode the older rules never engage: after a big fire leaves
    // ~20-30 unstructured puyo behind, the rebuild fills the board
    // into a flat-topped mesa (all columns 9-13 high, occupancy
    // climbing past 60) with no chain of 4+ ever becoming available,
    // so Rules 9/6' stay silent while every placement just raises the
    // mesa until no safe move remains.  Strong players avoid this by
    // keeping the post-fire board clean and by actively digging a
    // stalled board down instead of piling onto it.  So: when the
    // board is a high flat plateau (or the danger column already
    // touches 11) AND no real chain is available now or one puyo away
    // (nothing is being built that a small fire could destroy), take
    // the max-erasure placement as a cleanup fire -- provided it
    // actually shrinks the board and leaves plenty of safe moves.
    // The rule re-evaluates every turn, so a stalled board is dug
    // down over consecutive turns until it drops out of the plateau
    // state or a real chain appears.
    else if (ch <= 3 && occupied >= 48 && bestDig.erased >= 6 &&
             bestDig.postOccupied <= occupied - 4 &&
             bestDig.postSafeMoves >= 6) {
        const int minHeight = *std::min_element(heights.begin(), heights.end());
        const bool plateau = (maxHeight >= 9 && maxHeight - minHeight <= 4) ||
                             dangerHeight >= 11;
        if (plateau && maxTriggerChain(board) <= 3) {
            rule = 8;
            pick = &bestDig;
        }
    }
    // Rule 7: desperate -- almost no safe placement remains.
    else if (currentSafe <= 3 && bestClear.erased >= 4) {
        rule = 7;
        pick = &bestClear;
    }

    if (rule == 0) return 0;
    out = pick->move;
    if (debugLoggingEnabled()) {
        std::ostringstream oss;
        oss << "[AI-DEBUG] FIRE-RULE rule=" << rule
            << " move=(" << out.x << "," << out.rotation << ")"
            << " chains=" << pick->chains
            << " erased=" << pick->erased
            << " occupied=" << occupied << "->" << pick->postOccupied
            << " dangerHeight=" << dangerHeight
            << " maxHeight=" << maxHeight
            << " currentSafe=" << currentSafe
            << " postSafe=" << pick->postSafeMoves;
        debugLog(oss.str());
    }
    return rule;
}

} // namespace

AI::AI()
    : weights_(amaBuildWeights()) {
}

void AI::reset() {
    gtr_.reset();
    patternName_.clear();
    history_ = GameHistory{};
    dangerChainSinceTurn_ = -1;
}

Move AI::chooseMove(
    int turn,
    const Board& board,
    const std::vector<PuyoPair>& pieces
) {
    // Beam width 24 (was 12).  Round-3 finding, from studying human
    // pro play: pros keep several different build lines alive at once
    // (multiple templates / tail patterns / second-fold options) and
    // prune late.  With width 12 the beam's static-evaluation pruning
    // discarded exactly those slow-maturing lines early, which capped
    // peak chains.  Width 24 measurably recovers them (10+ chain games
    // 6/30 -> 9/30, max chain 11 -> 12, deaths still 0/30); width 36
    // produced identical games, so 24 is the saturation point.
    return chooseMove(turn, board, pieces, 3, 24);
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

    // Fire-decision layer: when a chain must be fired now (finished big
    // chain, or survival), do not ask the beam -- its cumulative potential
    // scoring is structurally biased toward never firing (see above).
    {
        Move fireMove{-1, 0, false};
        if (decideFireMove(board, pieces, fireMove, turn,
                           dangerChainSinceTurn_) > 0 && fireMove.valid) {
            observeChosenMove(turn, board, pieces, fireMove);
            return fireMove;
        }
    }

    Move selected = search_.chooseMove(
        board,
        pieces,
        weights_,
        std::max(1, depth),
        std::max(1, beamWidth),
        history_
    );

    // Trigger-continuity guard (variant W): if the board currently has
    // a real trigger (a >=4 chain one puyo away) and the beam's quiet
    // move would bury it without a replacement trigger being complete,
    // switch to the best alternative placement that keeps a trigger
    // alive.  Firing moves and trigger-preserving moves are untouched;
    // if no placement preserves a trigger, the beam's move stands.
    if (selected.valid && maxTriggerChain(board) >= 4) {
        const auto selSim = Simulator::drop(board, pieces.front(), selected);
        if (!selSim.gameOver && selSim.chains < 4 &&
            maxTriggerChain(selSim.board) < 4) {
            Move bestAlt{-1, 0, false};
            int bestAltTrigger = -1;
            int bestAltChains = -1;
            int bestAltOccupied = std::numeric_limits<int>::max();
            for (const auto& move : generateLegalMoves(board, pieces.front())) {
                const auto sim = Simulator::drop(board, pieces.front(), move);
                if (sim.gameOver && !sim.allClear) continue;
                const int trig = (sim.chains >= 4)
                    ? sim.chains
                    : maxTriggerChain(sim.board);
                if (trig < 4) continue;
                const auto hs = sim.board.heights();
                const int occ = std::accumulate(hs.begin(), hs.end(), 0);
                if (trig > bestAltTrigger ||
                    (trig == bestAltTrigger && sim.chains > bestAltChains) ||
                    (trig == bestAltTrigger && sim.chains == bestAltChains &&
                     occ < bestAltOccupied)) {
                    bestAlt = move;
                    bestAltTrigger = trig;
                    bestAltChains = sim.chains;
                    bestAltOccupied = occ;
                }
            }
            if (bestAlt.valid) selected = bestAlt;
        }
    }

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
