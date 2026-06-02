# Making DiskANN Competitive With Starling

## Goal

The research goal is to improve the original DiskANN beam-search path until it
can either:

- beat Starling's full pipeline on the same dataset and fairness setting, or
- clearly outperform Starling page-search-only when Starling is not allowed to
  use workload-aware frequency tuning or memory navigation graphs.

This document tracks design directions, fairness constraints, and experiment
plans for that goal.

## Baselines To Separate

Starling has multiple operating points. They should not be collapsed into one
baseline.

| Baseline | Configuration | Fairness note |
| --- | --- | --- |
| DiskANN beam search | `USE_PAGE_SEARCH=0`, no new early stop | Original baseline. |
| Starling page-only | `USE_PAGE_SEARCH=1`, `GP_USE_FREQ=0`, `MEM_L=0` | Fairer general-purpose Starling comparison. |
| Starling page + memory graph | `USE_PAGE_SEARCH=1`, `MEM_L>0`, `MEM_USE_FREQ=0` | Uses extra memory index but no test-query frequency. |
| Starling workload-aware | `GP_USE_FREQ=1`, `MEM_USE_FREQ=1` | Only fair if frequency comes from a separate training/historical workload. If frequency is generated from evaluation queries, treat it as an oracle upper bound. |

The main paper-quality comparison should prioritize:

```text
DiskANN improved vs Starling page-only
DiskANN improved vs Starling general-purpose
```

The workload-aware Starling line should be reported separately.

## Current Sift1M Reference Points

Observed in this checkout:

| Method | Representative setting | QPS | Recall@10 |
| --- | --- | ---: | ---: |
| DiskANN beam | `T=16`, `L=50`, `BW=4` | 5517.17 | 99.27 |
| Starling page-only | `T=16`, `L=50`, `BW=4`, `PS_USE_RATIO=1.0` | 8370.34 | 99.66 |
| Starling full/workload-aware | `T=32`, `L=50`, `BW=4`, `MEM_L=10`, `PS_USE_RATIO=0.5`, `GP_USE_FREQ=1` | 15298.71 | 99.62 |

The page-only target is reachable if DiskANN reduces wasted expansions and tail
latency. The workload-aware target is much harder because it uses layout and
memory graph help.

## Design Direction 1: PFM Early Termination

PFM, or Proxy Frontier Monitor, uses a ratio:

```text
pq_ratio = best_unexpanded_pq / kth_result_pq
```

Interpretation:

- `best_unexpanded_pq`: best PQ distance among unexpanded frontier candidates.
- `kth_result_pq`: current K-th best result distance in the retained result set.
- if `pq_ratio` is high, the remaining frontier is unlikely to improve top-K.

Stopping rule:

```text
stop if pq_ratio > effective_theta
```

Why this is promising for DiskANN:

- It targets the original beam-search waste directly.
- It uses values already in DRAM.
- It does not need extra SSD reads.
- It is per-query adaptive: easy queries stop early, hard queries continue.

Expected impact:

- lower expanded nodes/hops
- lower mean IOs
- lower mean and tail latency
- better QPS at the same recall

Implementation notes:

- Add counters for `expanded_nodes`, `expanded_hops`, and `early_stop_reason`.
- Log `pq_ratio` at stop time for debugging.
- Keep the original fixed-L search available for exact A/B comparisons.

## Design Direction 2: DRA Adaptive Threshold

DRA, or Divergence-Rate Adaptor, adjusts the PFM threshold based on how fast the
frontier ratio is moving:

```text
delta_ratio_t    = pq_ratio_t - pq_ratio_{t-1}
ema_delta_ratio  = alpha * delta_ratio_t + (1 - alpha) * ema_delta_ratio
effective_theta  = max(1.0, theta - k * ema_delta_ratio)
```

Intuition:

- rapidly increasing ratio means the frontier is diverging, so stop earlier
- flat ratio behaves like fixed-threshold PFM
- decreasing ratio means the current hop improved the result set, so be more
  conservative

Suggested sweep:

```text
theta = 1.05, 1.10, 1.15, 1.20
k     = 0.2, 0.4, 0.6
alpha = 0.2, 0.3, 0.5
```

Expected role:

- DRA is an incremental improvement after PFM works.
- It should improve QPS/latency without much recall loss.
- It may be most useful in high-recall regions where fixed PFM is conservative.

## Design Direction 3: ECG Safety Gate

ECG, or Exact Convergence Gate, has two parts.

Part A is mandatory correctness protection:

```text
do not stop if hop_round < MIN_EXPLORE_HOPS
do not stop if retset_size < K
```

Rationale:

- before the result set has K entries, `retset[K-1]` is invalid
- early hops have unstable PQ frontier estimates
- without a warm-up gate, PFM can false-trigger on sparse initial state

Part B uses exact distances already computed during expansion:

```text
stop if hop_best_exact > kth_exact * exact_alpha
```

Suggested sweep:

```text
MIN_EXPLORE_HOPS = 2, 3, 4
exact_alpha      = disabled, 1.20, 1.15, 1.12, 1.10
```

Expected role:

- Part A should be enabled for all early-stop experiments.
- Part B should be enabled only if exact distances are already available in the
  code path, avoiding extra SSD reads or expensive reranking.

## Design Direction 4: Better Beam Scheduling

PFM/DRA decide when to stop, but DiskANN can also improve which candidates to
expand.

Candidate ideas:

- dynamic beamwidth: start wider, narrow when PFM ratio rises
- IO-budget-aware expansion: stop issuing reads when expected improvement is low
- frontier batching: group nearby node IDs or sectors to reduce random IO
- stale-candidate pruning: remove candidates whose PQ distance is far beyond
  current K-th result

These ideas are closer to Starling's page-locality advantage, but they should
stay within the DiskANN beam-search layout so the method remains a DiskANN
enhancement rather than a Starling clone.

## Design Direction 5: Fair Memory And Cache Use

DiskANN beam search already supports:

```text
CACHE
MEM_L
mem_index_path
```

For fair comparison:

- random memory graph is acceptable if Starling also uses only random memory
  graph
- frequency memory graph is workload-aware and should not be mixed into the main
  general-purpose comparison
- cache nodes are allowed if cache budget is explicitly reported

Potential DiskANN configuration families:

```text
DiskANN+PFM
DiskANN+PFM+DRA
DiskANN+PFM+DRA+ECG
DiskANN+PFM+DRA+ECG+CACHE
DiskANN+PFM+DRA+ECG+random MEM_L
```

## Required Metrics

Do not judge only by QPS. Track:

```text
Recall@K
QPS
mean latency
p50 latency
p99 latency
p99.9 latency
mean IOs
expanded nodes
expanded hops
early stop rate
early stop reason distribution
peak memory
```

The current KNN log already reports:

```text
QPS
Mean Latency
P50 Latency
P99 Latency
99.9 Latency
Mean IOs
Recall@K
```

Add expanded-node and early-stop counters before evaluating PFM/DRA/ECG.

## Experiment Plan

Start with `sift1m` because it is fast enough for repeated sweeps.

1. Reproduce baseline DiskANN beam Pareto:

```text
USE_PAGE_SEARCH=0
BM_LIST=(2 4 8)
T_LIST=(8 16 32)
LS="30 50 80 100 150 200"
```

2. Add PFM with fixed theta:

```text
theta = 1.05, 1.10, 1.15, 1.20
MIN_EXPLORE_HOPS = 2
```

3. Add DRA:

```text
k = 0.2, 0.4, 0.6
alpha = 0.3
```

4. Add ECG Part B:

```text
exact_alpha = disabled, 1.20, 1.15, 1.12
```

5. Compare against Starling page-only:

```text
USE_PAGE_SEARCH=1
GP_USE_FREQ=0
MEM_L=0
PS_USE_RATIO=1.0, 0.75, 0.5, 0.25
```

6. Only after the general-purpose comparison is strong, compare against
   workload-aware Starling as a separate line.

## Success Criteria

Primary target:

```text
DiskANN improved beats Starling page-only on Recall-QPS Pareto,
or is clearly better on Recall-p99 latency Pareto at comparable recall.
```

Secondary target:

```text
DiskANN improved approaches Starling workload-aware performance without using
evaluation-query frequency.
```

A result is weak if it only wins at low recall but loses at:

```text
Recall@10 >= 99.5
Recall@10 >= 99.8
Recall@10 >= 99.9
```

## Risks

- PQ ratio can be overconfident for hard queries, causing recall loss.
- DRA can become too aggressive if ratio jumps are noisy.
- Exact-gate logic may not be free if exact distances are not already computed.
- Over-tuning on `sift1m` may not transfer to `deep1m`, `gist1m`, or MIPS
  datasets.
- Comparing against Starling workload-aware with test-query frequency is not a
  fair main result.

## Recommended Next Implementation

Implement in this order:

1. Add instrumentation only: expanded hops, expanded nodes, frontier sizes, and
   final `pq_ratio`.
2. Add ECG Part A safety guard.
3. Add PFM fixed-threshold stop.
4. Add DRA.
5. Add ECG Part B exact-distance gate if it is genuinely zero extra IO.

Each step should produce a separate report line and CSV, not overwrite the
baseline.
