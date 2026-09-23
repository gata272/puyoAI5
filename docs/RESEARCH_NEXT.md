# AI upgrade roadmap

## Completed in this revision

1. Human-form matcher: public ama GTR / SGTR / FRON patterns.
2. Scalar-equivalent `link_2` / `link_3` masks.
3. Ama-style quiescence with a three-puyo tactical drop depth.
4. Beam benchmark harness and explicit search configuration.
5. SPSA automatic weight-tuning harness with common random numbers.

## Still recommended

- Validate every C++ simulator transition against `puyoSim.js`, including
  wall-kick, top-row behavior, garbage removal and chain scoring.
- Add a transposition table if browser profiling shows it is worthwhile.
- Tune the production beam width on the actual deployment device rather than
  assuming that a wider beam is always stronger.
- Build a curated tactical benchmark (GTR completion, trigger selection,
  extension, nuisance handling and death avoidance) before accepting tuned
  weights.
- Compare tuned profiles against the public ama `build` profile using the same
  queue corpus.


## Maximum-chain focused revision

- Main search is constrained to three pairs (current + two lookahead), matching the intended human-information limit.
- Root selection is maximum-chain-first, then normal evaluation as the tie-breaker.
- Immediate chains receive a nonlinear `chains^4` reward.
- A `chainPotential` feature rewards extendable 2/3-puyo groups.
- If safe placements exist, game-over placements are excluded. If none exist,
  the least-bad game-over placement is returned instead of reporting no move.

## Trigger relay construction (PuyoAI8.1)

The current chain-building experiment targets a concrete delayed-trigger relay:

1. Find an existing exact-3 group of color A.
2. Construct a vertical B->A pair above it, giving `A(3) / B / A`.
3. Do not fire A while constructing the relay; the separating B is intentional.
4. Build B into a trigger on a later turn by dropping additional B puyos next to the relay B.
5. When B fires, the relay B disappears and the upper A falls onto the existing A(3), making A(4), producing the next chain.

The evaluator rewards such latent relays, especially when the relay B already has a 1- or 2-puyo support group and therefore needs only a small number of future B drops to fire. A 3-puyo support is not rewarded because adding the relay B would immediately fire B and destroy the intended delayed construction.

## PuyoAI10: persistent trigger-transfer construction

PuyoAI10 changes the research objective from direct long-horizon maximum-chain search to a human-information-constrained trigger-transfer policy.

- The AI receives and uses only the current pair plus two lookahead pairs (3 pairs total).
- Exact-3 groups are treated as candidate marked triggers.
- A trigger dependency `B -> A` exists when removing an exact-3 B group and applying gravity makes an A group reach four or more.
- Dependencies are recognized in both vertical and horizontal arrangements, so motifs such as `A / BAAA` and `A / B / AAA` are both represented by the same dependency test.
- A route such as `D -> C -> B -> A` receives a strong structural reward.
- The strongest exact-3 anchor is protected from accidental destruction unless the move actually resolves a chain.
- The visible three-pair queue is used only as compatibility information: if a useful predecessor color is not present, the anchor remains valuable and the AI may wait instead of forcing a destructive construction.
- `Simulator::resolveBoard()` exposes the exact production resolution rules to the trigger planner, avoiding a second, inconsistent chain implementation.

The intention is to repeatedly move the marked trigger upward or sideways rather than spending the trigger immediately. This can build a long latent dependency chain while respecting the three-pair information limit.


## PuyoAI11: Long Chain Potential

PuyoAI11 keeps the human-information constraint of three visible pairs, but changes the search objective from immediate maximum-chain preference toward latent large-chain construction. The new evaluator scores extendable 2/3-groups, reachable extension cells, construction shape, queue compatibility and height pressure. Existing ama form matching and trigger-transfer evaluation remain active.

The beam also reserves a potential elite so a quiet but promising construction is not removed solely because its immediate evaluator score is lower. Final selection uses accumulated search score plus a moderate actual-chain bonus and latent-potential bonus instead of lexicographic maximum-chain-first selection.

This revision is deliberately conservative about hidden information: no future queue beyond the visible three pairs is read. The benchmark must be repeated on larger fixed seed corpora before claiming a statistically significant improvement.


## Survival-aware chain revision

The latest survival experiment adds an exact two-step escape-route signal,
earlier danger-zone probing, a bounded survival reserve in beam pruning, and
full root-child survival probing. The survival cache key includes both visible
future pairs so the two-step measurement cannot be reused across incompatible
queue contexts.

The current benchmark diagnostics distinguish `no_safe_move` from
`selected_death_with_safe_move`; future tuning should prioritize reducing the
former without lowering the maximum-chain distribution.


## PuyoAI22: Recovery and post-chain reactivation

The next refinement separates theoretical long-chain potential from realized recovery.
Visible next/next-next probes now record actual net clearing, post-placement safe mobility,
and follow-up clearing. These signals are used mainly in the danger zone so healthy GTR
construction does not become a cash-out policy.

After a recent chain of four or more, a short rebuild window rewards visible follow-up
trigger potential and real continuation rather than passive survival. A separate short
stagnation detector combines quiet turns, material accumulation, and weak visible trigger
progress; it is danger-gated and therefore does not penalize ordinary quiet construction.

Path-level minimum-safe-move memory remains diagnostic and is not folded into utility.
The benchmark target is to reduce low-chain/death trajectories and post-large-chain deaths
without reducing the existing 8+/10+/12+ distribution on both fixed seeds.

## PuyoAI24 lifecycle policy: BUILD -> TENSION -> RECOVER -> REBUILD

PuyoAI24 introduces persistent game-level history. Search-tree hypothetical chains
no longer determine rebuild/stagnation state. The actual game records recent chain
activity, quiet turns, occupancy growth since the last real clear, and the age of
the most recent 4+ chain.

Policy states:
- BUILD: normal construction; the existing main-chain / trigger machinery remains primary.
- TENSION: the board is entering a danger zone or recent progress has weakened; retain escape candidates without globally maximizing safeMoves.
- RECOVER: activate when accumulation and low mobility combine, or when the theoretical main-chain path diverges strongly from the visible realizable path. Recovery uses post-placement safety, net clearing, truePath, and follow-up clearing.
- REBUILD: after an actual 4+ chain, prioritize visible follow-up trigger formation for a bounded window before returning to ordinary construction.

The implementation also keeps a bounded Recovery reserve in beam pruning. This is
separate from the existing survival reserve so that a candidate with a modest
current score but a strong visible recovery route can survive pruning without
turning normal construction into a mobility-maximizing policy.
