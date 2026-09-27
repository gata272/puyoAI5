# Survival-aware route preservation (vNext)

## Purpose

This change targets a specific failure mode found in the benchmark decision logs:
A node can retain a very strong theoretical long-chain route while the currently
visible next pieces no longer have a safe way to start or continue that route.
When this happens, static route/viability terms can keep the branch in the beam
until the board collapses.

The policy therefore separates two signals:

- **Root safety memory**: the measured safe-move horizon immediately after the
  first move is carried through the search. It is a correction, not a hard veto,
  and fades after the branch rebuilds a wide current safety horizon.
- **Current visible-route viability**: when the current safety horizon is narrow
  (5 or fewer safe moves, or the second horizon is 2 or fewer safe moves),
  positive construction/virtual-chain terms are attenuated unless the visible
  pieces actually expose a trigger path.

Healthy boards keep the existing long-chain construction incentives unchanged.
Raw board height alone is deliberately not used to classify a branch as being
in danger when measured mobility remains wide.

## Beam behavior

The existing root-diversity beam policy is preserved. The new safety correction
is applied to utility, while the existing survival reserve remains unchanged.
This avoids replacing long-chain diversity with a broad mobility preference.

The final critical escape rule is widened from a root horizon of 0-1 safe moves
to 0-2 safe moves, but only accepts an escape candidate that retains meaningful
chain productivity and does not discard substantially more chain value.

## Local deterministic check

Using the repository's deterministic queue generator, depth 3, beam 16, and
100 turns for each of the five benchmark seeds `1, 2, 3, 1015, 20260908`,
the candidate was compared with the unmodified repository source.

| Metric | Baseline | vNext candidate |
|---|---:|---:|
| Mean maximum chain (5 games) | 8.4 | 10.0 |
| Games over | 1 / 5 | 0 / 5 |
| Games reaching 5+ | 4 / 5 | 5 / 5 |
| Games reaching 8+ | 4 / 5 | 5 / 5 |
| Games reaching 10+ | 3 / 5 | 3 / 5 |
| Games reaching 12+ | 1 / 5 | 1 / 5 |

These are deterministic single-game-per-seed regression measurements, not a
claim about large-sample statistical performance. A larger benchmark should
be run before treating the change as a final tuning optimum.
