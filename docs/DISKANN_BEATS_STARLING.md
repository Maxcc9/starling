# PaceANN on Starling: Current State, Results, and Implementation Guide

> **Last updated: 2026-06-03**
> This document is written for an agent that will continue this research.
> Read the entire document before writing any code. The most important section
> is "What to implement next" at the bottom.

---

## 1. What Is PaceANN

PaceANN is an adaptive early-stopping criterion for disk-based graph ANN search.
It detects when the search frontier has converged and stops before the full L-hop
budget is consumed.

### Core criterion: PFM (Proxy Frontier Monitor)

```
pq_ratio = best_unexpanded_pq / kth_result_pq
stop if pq_ratio > effective_theta
```

- `best_unexpanded_pq`: PQ distance of the best not-yet-expanded candidate in the
  retset (i.e., the next node that would be expanded).
- `kth_result_pq`: PQ distance of the current K-th result.
- When the ratio is high, the remaining frontier is far from the current top-K
  and further expansion is unlikely to improve recall.

### Adaptive threshold: DRA (Divergence Rate Adaptor)

```
delta_ratio_t   = pq_ratio_t - pq_ratio_{t-1}
ema_delta       = alpha * delta_ratio + (1 - alpha) * ema_delta
effective_theta = max(1.0, theta - k * ema_delta)
```

If the ratio is rising fast (frontier diverging quickly), `effective_theta` is
reduced → stop sooner. If the ratio is falling (search is still finding better
results), `effective_theta` is raised → keep going.

Default constants used in DiskANN experiments: `alpha=0.3`, `k=0.4`.

---

## 2. Implementation Status

### In DiskANN (`/home/gt/research/DiskANN`)

**Fully implemented and validated.** Parameters:

```
--frontier_stop_theta FLOAT   PFM theta (0 = disabled)
--frontier_divergence_k FLOAT DRA k coefficient (default 0.4)
```

Key implementation file:
[src/pq_flash_index.cpp](../../../DiskANN/src/pq_flash_index.cpp)

Search entry point: `cached_beam_search()` — PFM+DRA logic is near the bottom of
the main beam-search loop.

### In Starling (`/home/gt/research/starling`)

**Also implemented.** Parameters:

```
--pfm_theta FLOAT     PFM theta (0 = disabled)
--divergence_k FLOAT  DRA k coefficient
```

Key implementation file:
[src/pq_flash_index.cpp](../src/pq_flash_index.cpp) — PFM+DRA block starts at
approximately line 1239. Constants:

```cpp
static constexpr unsigned PFM_MIN_EXPLORE_HOPS = 2;
static constexpr float    PFM_DRA_ALPHA         = 0.3f;
```

PFM applies to **both** beam search (`use_page_search=0`) and page search
(`use_page_search=1`).

---

## Candidate Top-Tier Contributions

The strongest paper direction is not "pure DiskANN beam search beats Starling
page search." Starling's page layout is a structural IO-locality advantage. The
more defensible thesis is:

> Static page-local graph layout is not enough; disk-resident ANN search also
> needs runtime convergence and tail-aware adaptivity.

This frames Starling's page graph as the layout substrate and positions this
work as the adaptive runtime layer on top of it.

### C1. Runtime Page Convergence Control

Status: implemented partially and measured on sift1m.

Core idea: add query-adaptive early stopping directly inside Starling page
search. Page search reads multiple colocated nodes per IO; our policy decides
when the current page-level frontier has already converged enough for the target
recall.

Implemented mechanisms:

- PFM/DRA in page search (`--pfm_theta`, `--divergence_k`).
- Runtime Page ECG rule (`--page_ecg_alpha`, `--page_ecg_pq_guard`,
  `--page_ecg_min_hops`).
- Telemetry for PFM and exact convergence features.

Why this can be a contribution:

- Original Starling page search uses static `L`.
- Our page-search policy is runtime adaptive and target-dependent.
- On sift1m, Page ECG beats original page-only QPS at measured target regions,
  with the strongest wins around 95%, 98%, and 99.8% recall.

What is still needed:

- Clean ablation: Page-only vs PFM vs ECG vs PFM+ECG.
- DRA ablation: `divergence_k=0` vs nonzero.
- `page_ecg_min_hops` sweep.
- Cross-dataset validation on deep1m, gist1m, text2image1m, and sift100m when
  the page index is available.

### C2. Target-Aware Page Search Policy

Status: design ready; implementation should be script-first, then optional C++
policy loader.

Core idea: choose the search policy from a target recall objective instead of
forcing a single static `L/theta/alpha` setting.

Proposed policy table:

```text
target_recall,L,pfm_theta,divergence_k,page_ecg_alpha,page_ecg_pq_guard,min_hops
95.0,...
97.0,...
98.0,...
99.0,...
99.5,...
99.8,...
```

Implementation path:

1. Sweep `L`, `pfm_theta`, `divergence_k`, `page_ecg_alpha`,
   `page_ecg_pq_guard`, and `page_ecg_min_hops`.
2. For each target recall, pick the highest-QPS point that meets the target.
3. Generate Pareto plots for QPS, mean latency, p50, and p99.
4. Optionally add CLI support:

```bash
--target_recall 99.0
--policy_file reports/.../page_policy.csv
```

Why this can be a contribution:

- Many real retrieval systems operate in the 95-99% range, not only at 99.8%.
- Static-L systems waste IO on easy queries.
- A target-aware policy gives a clean recall/QPS Pareto story.

What is still needed:

- Separate policy fitting and test query sets.
- Guard bands so selected policies do not miss target recall on held-out
  queries.
- Dataset-specific and cross-dataset transfer analysis.

### C3. Hard-Query Tail Control

Status: not implemented; should start after hard-query telemetry is clean.

Core idea: p99 is dominated by hard queries and queueing behind hard queries.
Early stopping improves average IO, but can still leave tail latency worse if
hard queries run a large `L` to recover recall.

Candidate low-risk implementation:

```text
Run first few page-search hops.
Classify query difficulty using runtime-only features:
  pq_ratio trajectory
  n_ios_so_far
  frontier_size
  exact_ratio if available

Easy path:
  aggressive Page ECG/PFM, smaller L

Hard path:
  conservative stop policy, larger L, optional slow lane
```

Why this can be a contribution:

- It attacks p99 directly, not only average IO.
- It explains and addresses the observed gap where Beam+PFM worsens p99 while
  Page+PFM is roughly neutral.
- It can be evaluated with p99-recall Pareto, not only QPS-recall Pareto.

What is still needed:

- Hard/easy telemetry for page search and beam search at matched recall targets.
- A rule-based classifier before attempting learned models.
- A server/batch experiment that measures queueing or at least per-query p99.

### C4. Optional Supporting Components

These are useful, but currently weaker as primary contributions:

| Component | Status | Role |
| --- | --- | --- |
| RAM Pivot Entry Selection | Implemented, modest sift1m gain | Supporting optimization; needs stronger RAM graph/seed sweep. |
| Page-aware beam | Implemented prototype, weak alone | Diagnostic/upper-bound tool, not primary story yet. |
| Learned stop classifier | Offline promising | Use as oracle/upper-bound or calibration tool unless runtime generalization is proven. |
| Sample-based hot cache | Not implemented | Important baseline and possible supporting component. |
| Streaming/memory-safe partitioner | Not implemented | Engineering enabler for 100M Starling page baselines on limited RAM. |

Recommended contribution stack for a paper:

```text
Primary:
  Runtime Page Convergence Control
  Target-Aware Page Search Policy

Secondary:
  Hard-Query Tail Control

Supporting:
  Telemetry framework
  Cross-dataset Pareto analysis
  Optional learned/oracle stop upper bound
```

---

## 3. Measured Results

### 3a. DiskANN system — sift1m (R128_L200_QD128, T=16, W=4, K=10)

Index: `/mnt/diskann_data/index/sift1m/sift1m_oracle_p0/sift1m_R128_L200_QD128/`

**Pareto-style operating points** (PaceANN sweeps both θ and L):

Important fairness note: these numbers use a high-quality DiskANN index
(`R128/L200/QD128`). They are useful for understanding PaceANN on DiskANN, but
they are **not** directly comparable to the current Starling sift1m page index
(`R64/L100/B2/M2`). Use same-index Starling beam vs page experiments when the
goal is isolating search-algorithm effects.

| Method | Recall | QPS | Δ QPS | p50 | p99 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Disk baseline | 0.9960 | 2,240 | — | 6.63ms | 9.20ms |
| Disk + PaceANN | 0.9975 | 3,461 | +55% | 4.19ms | 7.57ms |
| Disk + PaceANN | 0.9998 | 1,624 | +233% | 3.96ms | 4.59ms |

Sweet spot θ=1.12, L=60 vs baseline L=60:
- QPS: 2,650 → 3,320 (+25%)
- p99: 3.63ms → 3.58ms (neutral, −1.5%)

Sweet spot θ=1.12, L=150 vs baseline L=150:
- QPS: 1,307 → 2,028 (+55%)
- p99: 9.20ms → 5.51ms (−40%)

### 3b. DiskANN system — sift100m (R128_L300_B40, T=16, W=4, K=10)

Index: `/mnt/diskann_data/index/sift100m_B40_rebuild/sift100m_R128_L300_B40_M40/`
Output: `scripts/paramAnalysis/gridSearch/outputFiles/search/sift100m_cache/`

**Four-way comparison near 98.6-98.9% recall:**

This table is currently a historical operating-point comparison, not a clean
matched-L or matched-recall Pareto table. The exact `L` used by each row must be
recovered from the original logs before using this in a paper figure.

| Method | QPS | Δ vs disk | p50 | p99 |
| --- | ---: | ---: | ---: | ---: |
| Disk baseline | 2,805 | — | 5.60ms | 7.01ms |
| Cache baseline (BFS 10M nodes) | 3,369 | +20% | 4.70ms | 6.23ms |
| Disk + PaceANN | 3,802 | +36% | 3.96ms | 7.36ms |
| **Cache + PaceANN** | **4,902** | **+75%** | **3.03ms** | **5.29ms** |

Observed finding: PaceANN alone raises p99 slightly (+5%) because hard queries
that never trigger early stop still run full L. Cache alone improves QPS but p99
stays high. The combination appears superlinear, but the mechanism is still a
hypothesis. Before claiming synergy, measure PaceANN stop rate and hard-query IO
with and without cache.

### 3c. Starling system — sift1m (R64_L100_B2_M2, T=16, W=4, K=10)

Index: `/mnt/diskann_data/starling_data/index/sift1m_starling/sift1m_R64_L100_B2_M2/`
Current sweep outputs:
- Page+PFM: `reports/sift1m_pfm_sweep/`
- Beam+PFM: `reports/sift1m_beam_pfm_sweep/`
- Four-way comparison: `reports/sift1m_beam_page_pfm_comparison/analysis/`

Important setting: `page_only` is not frequency-aware and does not use memory
graph/cache/PFM, but it does use Starling's non-frequency graph-partitioned page
layout (`_disk.index` + `_partition.bin`). It is therefore the first structural
target for DiskANN-style beam search, not a raw unrelayouted DiskANN index.

**Best-QPS comparison by recall target:**

| Target recall | Beam | Beam+PFM | Page-only | Page+PFM |
| ---: | ---: | ---: | ---: | ---: |
| ≥99.50% | 3,749 QPS / 4.795ms p99 | 5,706 QPS / 5.005ms p99 | 9,662 QPS / 1.979ms p99 | 10,285 QPS / 2.418ms p99 |
| ≥99.70% | 3,749 QPS / 4.795ms p99 | 4,595 QPS / 6.370ms p99 | 7,428 QPS / 2.510ms p99 | 8,632 QPS / 3.428ms p99 |
| ≥99.80% | 3,128 QPS / 5.641ms p99 | 4,254 QPS / 7.813ms p99 | 6,046 QPS / 3.046ms p99 | 7,485 QPS / 3.022ms p99 |
| ≥99.85% | 2,668 QPS / 6.539ms p99 | 2,668 QPS / 6.539ms p99 | 6,046 QPS / 3.046ms p99 | 6,847 QPS / 4.056ms p99 |

At the key target `Recall@10 >= 99.80%`, the current hard targets are:

```
First target:  beat page_only  = 6045.92 QPS, 3.046ms p99
Strong target: beat page+PFM   = 7484.96 QPS, 3.022ms p99
```

**New finding:** beam+PFM improves QPS over beam, but does not beat page-only and
can make p99 worse. At `Recall@10 >= 99.80%`, beam+PFM reaches 4,253.57 QPS but
p99 rises to 7.813ms. This means simply tuning PFM/DRA is not enough; the missing
piece is structural IO locality and hard-query tail control.

Open tail-latency question: Beam+PFM worsens p99 while Page+PFM is roughly
neutral at the same high-recall target.

| Method pair | p99 change |
| --- | ---: |
| Beam → Beam+PFM | 5.641ms → 7.813ms |
| Page-only → Page+PFM | 3.046ms → 3.022ms |

This is not yet explained. Two plausible mechanisms need telemetry:

1. Page layout makes hard queries less pathological because every IO returns a
   page-local set of candidates, so the hard-query tail is already compressed.
2. Beam+PFM may need a larger `L` to recover recall after early-stop tuning,
   pushing hard queries into a worse tail regime.

Required analysis: compare beam and page hard-query groups at the same recall
target, including mean/p99 IO, hop count, PFM stop rate, and `pq_ratio`
trajectory. Do not choose the next design direction solely from aggregate QPS.

#### 3c.1 Beam-only controller validation — sift1m

Latest focused beam-only run:

```text
reports/sift1m_beam_controller_validation/analysis/
reports/sift1m_beam_pfm_validation/
reports/sift1m_beam_ecg_validation/
```

Setup: sift1m, Starling `R64/L100/B2/M2` index, **classic beam search**
(`use_page_search=0`), `T=16`, `W=4`, `K=10`, no cache, no memory graph.

Best-QPS operating points by target recall:

| Target Recall@10 | Beam baseline | Beam+PFM/DRA | Beam+PFM/DRA+ECG |
| ---: | ---: | ---: | ---: |
| >=99.00% | 5,429 QPS / 3.423ms p99 / 62.6 IO | 6,312 QPS / 4.156ms p99 / 52.5 IO | 6,442 QPS / 4.203ms p99 / 51.4 IO |
| >=99.50% | 3,777 QPS / 4.738ms p99 / 91.9 IO | 5,665 QPS / 5.149ms p99 / 59.1 IO | 5,652 QPS / 5.080ms p99 / 59.1 IO |
| >=99.70% | 3,777 QPS / 4.738ms p99 / 91.9 IO | 4,156 QPS / 6.442ms p99 / 81.6 IO | 4,156 QPS / 6.434ms p99 / 81.6 IO |
| >=99.80% | 3,127 QPS / 5.624ms p99 / 111.7 IO | 3,777 QPS / 8.306ms p99 / 90.0 IO | 3,761 QPS / 8.266ms p99 / 90.0 IO |
| >=99.85% | 2,177 QPS / 7.902ms p99 / 161.1 IO | 3,662 QPS / 8.262ms p99 / 92.7 IO | 3,645 QPS / 9.997ms p99 / 92.9 IO |
| >=99.90% | 1,088 QPS / 44.682ms p99 / 210.8 IO | no tested point | no tested point |

Interpretation:

- PFM/DRA is a real throughput and mean-IO optimization for beam search.
  It gives +16% QPS at >=99.0%, +50% at >=99.5%, +21% at >=99.8%,
  and +68% at >=99.85% under this sweep.
- The gain comes from stopping easy/medium queries early. It does **not**
  solve p99; p99 is usually worse than baseline at the same recall target.
- ECG did not materially improve this sweep. Its best points are nearly
  identical to PFM/DRA, so ECG should be framed as a safety guard or optional
  ablation, not as a main speedup component for beam search.

Representative telemetry:

| Config | Recall@10 | PFM stop | all mean IO | p99 slow mean IO | p99 slow PFM stop | p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `L=80, theta=1.15, dk=0.4` | 99.52% | 88.66% | 59.09 | 86.30 | 39.00% | 5.336ms |
| `L=150, theta=1.20, dk=0.4` | 99.84% | 90.46% | 90.05 | 159.88 | 9.00% | 8.372ms |

Mechanism: fast queries almost always stop early, but p99 slow queries usually
do not. At high recall, the non-stopped tail runs close to the full search
budget. Therefore the beam-search paper story should emphasize recall/QPS and
mean-IO Pareto improvement, while tail latency requires a separate hard-query
or scheduling mechanism.

#### 3c.2 Naive Top-K Stability Stop baseline — sift1m

Implementation status: added as a beam-search-only experimental baseline:

```text
--topk_stability_patience N
```

`N=0` disables the rule. For `N>0`, beam search stops when the PQ-space
`retset[0:K]` ids remain unchanged for `N` consecutive hops.

Outputs:

```text
reports/sift1m_beam_topk_stability/results.csv
reports/sift1m_beam_topk_stability/analysis/
```

Setup: same as Section 3c.1: sift1m, Starling `R64/L100/B2/M2`, classic beam
search (`use_page_search=0`), `T=16`, `W=4`, `K=10`, no cache, no memory graph.

Best-QPS operating points:

| Target Recall@10 | Beam baseline | Top-K Stability |
| ---: | ---: | ---: |
| >=95.0% | 6,123 QPS / 3.969ms p99 / 43.5 IO | P=2: 7,955 QPS / 3.413ms p99 / 33.5 IO |
| >=98.0% | 6,123 QPS / 3.969ms p99 / 43.5 IO | P=5: 5,959 QPS / 4.216ms p99 / 42.4 IO |
| >=99.0% | 4,441 QPS / 5.135ms p99 / 62.6 IO | P=8: 3,886 QPS / 6.803ms p99 / 57.8 IO |
| >=99.5% | 3,085 QPS / 6.783ms p99 / 91.9 IO | P=10: 3,224 QPS / 8.313ms p99 / 69.7 IO |
| >=99.8% | 2,535 QPS / 10.183ms p99 / 111.7 IO | no tested Top-K Stability point |
| >=99.9% | 1,362 QPS / 13.665ms p99 / 210.8 IO | no tested Top-K Stability point |

Interpretation:

- Top-K Stability is a useful **naive early-stop baseline**, not a strong main
  method.
- Aggressive patience (`P=1-3`) stops too early and saturates below 98% recall.
- Conservative patience (`P=8-15`) recovers recall but loses most of the speed
  benefit and often worsens p99.
- It cannot reach the important `>=99.8%` target in this sweep.

This supports the PFM motivation: result-set stability only observes whether the
current top-K output changed, while PFM also checks whether the unexpanded
frontier can still threaten the current K-th result.

### 3d. Starling beam with RAM Pivot Entry Selection — sift1m

Output:
`reports/sift1m_ram_pivot_sweep/analysis/ram_pivot_sift1m_summary.csv`

Code/CLI change:

```
--mem_L N
--mem_search_L N
--mem_seed_count N
```

The old Starling behavior conflated the in-memory navigation graph search depth
and the number of returned seeds:

```
mem_index_->search_with_tags(query, mem_L, mem_L, ...)
```

This has now been split:

- `mem_search_L`: search-list size inside the RAM navigation graph.
- `mem_seed_count`: number of RAM results injected into disk beam/page search.
- If either new parameter is `0`, the old `mem_L` value is used, so existing
  commands remain compatible.

Validation: memory graph is correctly connected to beam search. With `mem_L=0`,
hop 1 starts from the medoid (`n_ios=1`, `frontier=1`, `cur_list_size=65`). With
RAM pivot enabled, hop 1 starts from memory graph seeds (`n_ios=4`, `frontier=4`,
`cur_list_size=80`). Therefore the issue is not missing integration; it is weak
entry quality/limited structural benefit.

Focused sweep: `T=16`, `W=4`, `K=10`, sift1m, Starling R64/L100/B2/M2 index,
random 1% memory graph.

Comparison caution: rows with different `L` and different recall targets should
not be treated as matched-L comparisons. The cleanest current same-L comparison
is `Beam baseline L=100` vs `Beam + RAM pivot L=100`. The `L=120` rows show a
different operating point and need matched-recall interpolation before paper use.

| Method | L | RAM search L | Seed count | PFM θ | Recall@10 | QPS | Mean IOs | p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Beam baseline | 100 | 0 | 0 | 0 | 99.80 | 3,122 | 111.65 | 5.647ms |
| Beam + RAM pivot | 100 | 10 | 1 | 0 | 99.80 | 3,423 | 101.08 | 5.875ms |
| Beam + RAM pivot | 120 | 10 | 1 | 0 | 99.84 | 2,832 | 120.84 | 7.107ms |
| Beam + RAM pivot + PFM | 120 | 10 | 1 | 1.22 | 99.83 | 4,035 | 84.75 | 6.920ms |
| Page-only target | 80 | n/a | n/a | 0 | 99.80 | 6,046 | n/a | 3.046ms |
| Page+PFM target | 80 | n/a | n/a | 1.18 | 99.80 | 7,485 | n/a | 3.022ms |

Conclusion: simple RAM Pivot Entry Selection is real but insufficient. It gives
roughly +9.6% QPS over beam at the 99.8 recall target and saves about 10 disk
IOs/query, but it is still far below page-only. Increasing `mem_search_L` from
10 to 50/100/200 does not improve recall or IO enough to justify the extra RAM
CPU cost; the 1% memory graph appears to saturate quickly on sift1m.

Implication: to beat Starling page-only, entry routing alone must be stronger
than "query → 1% memory graph → top seed." Promising next variants:

- Use a larger or higher-quality RAM pivot graph, not only the existing 1%
  Starling memory graph.
- Add medoid fallback plus RAM pivot (`medoid + top-k RAM seeds`) and compare
  against pure RAM pivot.
- Train/query-adapt the number of injected entries using runtime-only signals
  from the RAM graph result distribution.
- Combine entry routing with page-aware/speculative IO batching; RAM pivot alone
  does not address Starling's main advantage: many useful neighbors per 4KB IO.

### 3e. Multi-thread IO overlap upper bound — sift1m

Output:

```
reports/sift1m_io_overlap/
reports/sift1m_io_overlap/analysis/beam_L80_overlap.txt
reports/sift1m_io_overlap/analysis/page_L80_overlap.txt
reports/sift1m_io_overlap/analysis/beam_L80_overlap_minHop2.txt
reports/sift1m_io_overlap/analysis/page_L80_overlap_minHop2.txt
```

Instrumentation:

- `search_disk_index --telemetry_path <prefix>` now also writes
  `<prefix>_L<L>_io.csv`.
- Each row records `query_id`, `hop`, `read_id`, and `io_key`.
- Beam search uses node-sector key: `NODE_SECTOR_NO(node_id)`.
- Page search uses Starling page key: `id2page_[node_id] + 1`.

Analyzer:

```
python3 scripts/analyze_io_overlap.py <prefix>_L<L>_io.csv --window-size 16
python3 scripts/analyze_io_overlap.py <prefix>_L<L>_io.csv --window-size 16 --min-hop 2
```

The analysis uses a 16-query window as a rough upper bound for T=16 concurrent
coalescing. This is not an exact runtime schedule; it is an optimistic overlap
screening test.

Setup: sift1m, Starling R64/L100/B2/M2, `T=16`, `W=4`, `L=80`, `K=10`.

| Method | Recall@10 | QPS | Mean IOs | Global duplicate ratio | Window duplicate ratio | Window duplicate ratio, hop≥2 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Beam | 99.70 | 3,715 | 91.93 | 79.56% | 5.15% | 4.12% |
| Page-only | 99.86 | 5,999 | 77.03 | 76.24% | 5.10% | 3.93% |

Interpretation:

- Across the whole workload, IO keys repeat heavily. A long-lived shared
  hot-node/hot-page cache has a plausible upper bound.
- Within a T=16-sized concurrent window, repeat IO is only about 4-5% after
  removing the first hop. Pure in-flight request coalescing is unlikely to give a
  large breakthrough on sift1m.
- Page-only also has low concurrent-window duplication, so beating it via shared
  in-flight IO alone is unlikely.

Conclusion: a multi-thread story should focus on **access-driven shared cache**
or **cross-query scheduling with larger batching windows**, not only "threads
waiting on the same in-flight page." The clean next experiment is an oracle
cache simulation over the IO trace: cache the top-N hot `io_key`s and estimate
how many disk reads would be removed at different memory budgets.

### 3f. Oracle hot-cache simulation — sift1m

Output:

```
reports/sift1m_io_overlap/analysis/beam_L80_oracle_cache_minHop2.csv
reports/sift1m_io_overlap/analysis/page_L80_oracle_cache_minHop2.csv
reports/sift1m_io_overlap/analysis/beam_L80_warm10_cache_minHop2.csv
reports/sift1m_io_overlap/analysis/page_L80_warm10_cache_minHop2.csv
```

Analyzer:

```
python3 scripts/analyze_oracle_hot_cache.py <prefix>_L<L>_io.csv --min-hop 2
python3 scripts/analyze_oracle_hot_cache.py <prefix>_L<L>_io.csv --min-hop 2 --warmup-fraction 0.1
```

The oracle version selects top-N hot `io_key`s from the full trace and evaluates
on the same trace. The warmup version selects hot keys from the first 10% queries
and evaluates on the remaining 90%, which is closer to a practical
access-driven cache.

Setup: same as Section 3e (`T=16`, `W=4`, `L=80`, `K=10`), excluding hop 1.

Warmup-10% results:

| Method | Budget keys | Saved read ratio | Estimated mean IO after cache |
| --- | ---: | ---: | ---: |
| Beam | 1k | 7.9% | 84.69 |
| Beam | 5k | 11.2% | 81.62 |
| Beam | 10k | 14.5% | 78.63 |
| Beam | 20k | 20.6% | 72.95 |
| Beam | 50k | 36.5% | 58.39 |
| Beam | 100k | 44.0% | 51.50 |
| Page-only | 1k | 9.1% | 70.01 |
| Page-only | 5k | 12.4% | 67.51 |
| Page-only | 10k | 15.8% | 64.85 |
| Page-only | 20k | 21.3% | 60.60 |
| Page-only | 50k | 37.4% | 48.24 |
| Page-only | 100k | 40.4% | 45.94 |

Interpretation:

- Access-driven hot caching has much higher upper bound than in-flight
  coalescing. With 50k hot keys, the warmup-based estimate removes about 36-37%
  of reads.
- However, page-only benefits almost as much as beam. This makes hot cache a
  **正交加成** rather than a clean way for beam to beat page search by itself.
- A 20k hot-key cache saves only about 20-21% reads. That is useful, but not
  enough to close the current beam-vs-page gap.
- A 50k+ hot-key cache may materially change throughput, but it must be costed
  in memory. For beam, a cached key is one 4KB sector, so 50k keys is roughly
  200MB of raw sector data. For page-only it is also 4KB/page.

Research implication: the multi-thread/shared-cache direction is reasonable if
framed as **adaptive access-driven cache beats topology-driven BFS cache**, but
it is not the strongest standalone path to beat Starling page-only. It should be
combined with entry routing or page-aware batching.

### 3g. Page-aware beam upper bound — sift1m

Output:

```
reports/sift1m_page_aware_beam/analysis/beam_L80_page_aware_upper_bound.txt
reports/sift1m_page_aware_beam/analysis/beam_L100_page_aware_upper_bound.txt
reports/sift1m_page_aware_beam/analysis/beam_memS10_K1_L120_pfm1p22_page_aware_upper_bound.txt
```

Analyzer:

```
python3 scripts/analyze_page_aware_beam_upper_bound.py \
  <beam_io_trace.csv> \
  <index_prefix>_partition.bin \
  --out <per_query_output.csv>
```

This is an analysis-only upper bound. It maps every beam node read to its
Starling page. Within each query, the first read of a page is counted as one page
IO, and any later beam node-read in the same page is counted as covered by that
page. This estimates how much IO could be saved by a per-query page buffer or
dynamic page-aware beam expansion.

It does **not** yet model:

- extra CPU distance computation for additional nodes inside a page,
- different retset evolution caused by early page expansion,
- recall changes from pushing extra page-local neighbors earlier.

Setup: sift1m, Starling R64/L100/B2/M2, `T=16`, `W=4`, `K=10`.

| Trace | Recall@10 | QPS | Original mean IO | Page-aware upper-bound mean IO | IO saving | p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Beam L80 | 99.70 | 3,715 | 91.93 | 76.15 | 17.17% | 4.839ms |
| Beam L100 | 99.80 | 3,119 | 111.65 | 91.23 | 18.28% | 5.646ms |
| Beam + RAM pivot + PFM (`L=120`, `mem_search_L=10`, `seed=1`, `θ=1.22`) | 99.83 | 4,115 | 84.75 | 70.88 | 16.37% | 5.940ms |
| Page-only reference L80 | 99.86 | 5,999 | 77.03 | n/a | n/a | 3.094ms |

Interpretation:

- Raw beam L80 has a strong page-aware upper bound: mean IO can drop from 91.93
  to 76.15, roughly matching page-only's 77.03 mean IO. But its recall is only
  99.70, below the main 99.80 target.
- Raw beam L100 reaches 99.80 recall, but even its page-aware upper bound is
  91.23 mean IO, still above page-only. Page awareness alone is not enough for
  high-recall raw beam.
- The best current combination, `RAM pivot + PFM`, is more promising. At 99.83
  recall, page-aware upper-bound mean IO is 70.88, below page-only's 77.03.
  This suggests the right implementation target is **PageAware + RAM pivot +
  PFM**, not PageAware alone.

Decision: implement a small Dynamic Page-Aware Beam prototype only after keeping
the scope narrow:

```
Beam-selected frontier node
  -> read its Starling page
  -> process the target node exactly as beam does
  -> optionally process top page-local extra nodes by exact distance
  -> push their neighbors into the normal beam retset
```

Initial prototype parameters:

```
--beam_page_aware 1
--beam_page_ratio 0.25|0.5|1.0
--beam_page_max_extra_nodes 0|1|2|4
```

First target configuration:

```
L=120, W=4, T=16, mem_search_L=10, mem_seed_count=1, pfm_theta=1.22
```

Reason: this is the only tested configuration whose page-aware IO upper bound is
below page-only at comparable recall.

### 3h. Dynamic Page-Aware Beam prototype — sift1m

Implementation status: first prototype implemented in Starling beam search.

New CLI:

```
--beam_page_aware 1
--beam_page_ratio FLOAT
--beam_page_max_extra_nodes N
```

Important: `--beam_page_aware 1` must read the Starling page-layout disk file:

```
--disk_file_path <index_prefix>_disk.index
```

Do not use `<index_prefix>_disk_beam_search.index`, because page-aware beam reads
by `id2page[node_id] + 1`.

Prototype behavior:

1. Beam still selects frontier nodes with DiskANN-style retset logic.
2. For each selected frontier node, the reader fetches the whole Starling page.
3. The target node is always expanded.
4. Extra page-local nodes are ranked by exact distance to the query.
5. Up to `beam_page_max_extra_nodes` extra nodes are expanded. `0` means no cap.
6. Their neighbors are inserted into the normal beam retset.

Best no-telemetry results so far, sift1m, `T=16`, `W=4`, `K=10`, `L=120`,
`mem_search_L=10`, `mem_seed_count=1`, `beam_page_ratio=1.0`,
`beam_page_max_extra_nodes=3`, DRA `k=0.3`:

| Method | Recall@10 | QPS | Mean | p50 | p99 | Mean IOs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Page-only target | 99.80 | 6,046 | n/a | n/a | 3.046ms | n/a |
| Page+PFM target | 99.80 | 7,485 | n/a | n/a | 3.022ms | n/a |
| Dynamic PageAware Beam, θ=1.12 | 99.63 | 10,038 | 1.539ms | 1.401ms | 3.894ms | 34.09 |
| Dynamic PageAware Beam, θ=1.14 | 99.77 | 8,430 | 1.834ms | 1.654ms | 4.616ms | 40.83 |
| **Dynamic PageAware Beam, θ=1.15** | **99.81** | **7,739** | **2.001ms** | **1.801ms** | **4.880ms** | **44.67** |
| Dynamic PageAware Beam, θ=1.16 | 99.84 | 7,148 | 2.168ms | 1.954ms | 4.942ms | 48.69 |
| Dynamic PageAware Beam, θ=1.18 | 99.88 | 6,131 | 2.532ms | 2.324ms | 5.141ms | 56.77 |

Key result: at the main `Recall@10 >= 99.80%` target, the prototype reaches
7,739 QPS, which beats both page-only and the current page+PFM target in
throughput.

Current weakness: p99 is still much worse than page-only/page+PFM. The prototype
has converted the IO upper bound into throughput, but the tail is now dominated
by hard queries and extra page-local CPU work.

Immediate next work:

- Add telemetry for how many extra page nodes are expanded per query/hop.
- Tune p99 by adaptive `beam_page_max_extra_nodes`: easy queries can process
  fewer extras, hard queries can process more.
- Try a page-aware PFM rule that accounts for page expansion, because current
  PFM still reasons about node-level frontier distance.
- Compare against Page+PFM+HotCache for fairness after adding cache.

### 3i. Adaptive Page-Aware Beam extra expansion — sift1m

Implementation status: first adaptive extra rule implemented.

New CLI:

```
--beam_page_adaptive_extra 1
--beam_page_easy_extra_nodes N
--beam_page_hard_extra_nodes N
--beam_page_adaptive_ratio_threshold FLOAT
```

Rule:

```
if previous_hop_pq_ratio >= beam_page_adaptive_ratio_threshold:
    use beam_page_easy_extra_nodes
else:
    use beam_page_hard_extra_nodes
```

Intuition: once the frontier looks converged, avoid unnecessary page-local CPU
work. If the query is still hard/non-converged, process more page-local nodes.

Best no-telemetry results so far, same setup as Section 3h:

| Method | θ | easy extra | hard extra | threshold | Recall@10 | QPS | Mean | p99 | Mean IOs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Fixed max-extra=3 | 1.15 | n/a | 3 | n/a | 99.81 | 7,739 | 2.001ms | 4.880ms | 44.67 |
| Adaptive extra | 1.15 | 1 | 3 | 1.10 | 99.81 | 8,331 | 1.864ms | 4.446ms | 44.63 |
| Adaptive extra | 1.15 | 1 | 4 | 1.05 | 99.80 | 8,246 | 1.883ms | 4.354ms | 44.63 |
| Adaptive extra | 1.16 | 1 | 3 | 1.05 | 99.83 | 8,102 | 1.925ms | 4.284ms | 48.64 |

Key result: adaptive extra improves both throughput and p99 over fixed
max-extra=3. The best throughput point at `Recall@10 >= 99.80%` is now:

```
theta=1.15, easy_extra=1, hard_extra=3, threshold=1.10
Recall@10=99.81, QPS=8331, p99=4.446ms
```

This is a stronger result than the fixed version and beats both page-only and
page+PFM in QPS. The p99 still does not beat page-only/page+PFM.

Telemetry for the best adaptive point:

```
reports/sift1m_dynamic_page_aware_beam/adaptive_best_telemetry_L120.csv
```

Summary:

| Group | mean | p99 | mean IOs | p99 IOs | mean hops | p99 hops | recall | PFM stop |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| all | 1.893ms | 4.555ms | 44.63 | 107 | 11.92 | 28 | 99.81 | 98.13% |
| p99 slow | 5.310ms | 8.389ms | 94.33 | 119 | 24.60 | 32 | 99.40 | 46.00% |
| PFM stopped | 1.844ms | 4.189ms | 43.45 | 97 | 11.61 | 25 | 99.81 | 100% |
| not stopped | 4.477ms | 7.852ms | 106.49 | 117 | 27.82 | 31 | 99.47 | 0% |

Interpretation: adaptive extra helped CPU and average latency, but the remaining
p99 is dominated by hard queries that either do not stop or stop very late.
The next tail-latency work should target hard-query control:

- hard-query-specific page-aware PFM,
- adaptive L/budget after the first few hops,
- optional hedged/safe backup only for predicted hard queries.

### 3j. High-recall Pareto view — sift1m

Output:

```
reports/sift1m_high_recall_pareto/analysis/
```

Generated by:

```
python3 scripts/plot_high_recall_pareto.py
```

This section shifts the comparison away from a single 99.80% point. The more
appropriate claim region is `Recall@10 >= 99%`, with emphasis on
`99.7% - 99.85%` where Dynamic PageAware Beam is currently strongest.

Best-QPS points by recall target:

| Target Recall@10 | Beam | Beam+PFM | PageOnly | Page+PFM | Dynamic PageAware Beam |
| ---: | ---: | ---: | ---: | ---: | ---: |
| ≥99.0 | 5,362 QPS / 3.491ms p99 | 6,964 / 4.385ms | 11,252 / 1.739ms | 11,947 / 1.904ms | 10,035 / 3.796ms |
| ≥99.2 | 5,362 / 3.491ms | 6,210 / 4.224ms | 9,662 / 1.979ms | 11,416 / 2.138ms | 10,035 / 3.796ms |
| ≥99.5 | 3,749 / 4.795ms | 5,706 / 5.005ms | 9,662 / 1.979ms | 10,285 / 2.418ms | 10,035 / 3.796ms |
| ≥99.7 | 3,749 / 4.795ms | 4,595 / 6.370ms | 7,428 / 2.510ms | 8,632 / 3.428ms | 9,331 / 3.955ms |
| ≥99.8 | 3,128 / 5.641ms | 4,254 / 7.813ms | 6,046 / 3.046ms | 7,485 / 3.022ms | 8,331 / 4.446ms |
| ≥99.85 | 2,668 / 6.539ms | 2,668 / 6.539ms | 6,046 / 3.046ms | 6,847 / 4.056ms | 7,121 / 4.951ms |
| ≥99.9 | n/a | n/a | 4,431 / 4.141ms | 4,431 / 4.141ms | n/a |

Current claim supported by this Pareto view:

- Dynamic PageAware Beam beats Page+PFM in **QPS** at recall targets
  `>=99.7`, `>=99.8`, and `>=99.85`.
- Page+PFM/PageOnly still dominate Dynamic PageAware Beam in **p99 latency**.
- Page+PFM remains stronger in the lower high-recall region (`99.0-99.5`).
- Dynamic PageAware Beam has no tested `>=99.9` point yet.

Therefore the strongest accurate claim is:

> Above 99.7% recall, Dynamic PageAware Beam currently provides a better
> throughput Pareto than Starling page search, while still exposing a tail
> latency gap that motivates hard-query control.

Do not claim full dominance yet. The current result is throughput dominance over
a high-recall subrange, not p99 dominance.

---

## 4. Key Bottlenecks and Open Problems

### 4a. Beam+PFM does not close the page-only gap

The latest Starling-internal comparison shows that beam+PFM still loses badly to
page-only. At `Recall@10 >= 99.80%`:

| Method | QPS | p99 |
| --- | ---: | ---: |
| Beam | 3,128 | 5.641ms |
| Beam+PFM | 4,254 | 7.813ms |
| Page-only | 6,046 | 3.046ms |
| Page+PFM | 7,485 | 3.022ms |

The QPS gain from beam+PFM is real, but the p99 regression is not acceptable.
This strongly suggests hard queries are either not stopping, stopping too late,
or paying expensive random IO that page layout avoids.

### 4b. p99 degradation for beam search

For Starling beam search (and DiskANN), PFM can improve QPS while worsening p99.
The latest sweep shows this is not only a small-L artifact; at high recall,
beam+PFM may need larger L to recover recall, which increases tail latency.
Fixes to investigate: per-query telemetry, ECG exact safety gate, adaptive-L, and
page-aware IO batching.

### 4c. Hard queries at high recall (≥99.8%)

Above recall 99.8%, PFM early-stop rate drops because the search must explore
farther to find rare neighbors. Neither page search nor PFM solves this.
Workload-aware frequency caching (Starling's `GP_USE_FREQ=1`) is the only known
solution but is unfair in general-purpose settings.

Status: this statement is directionally plausible but not fully quantified. We
currently have stop-rate telemetry for selected settings such as `L=80,
theta=1.15`; we still need stop rate by recall target, especially at `L=150` and
`Recall@10 >= 99.80%`. If stop rate collapses at high recall, PFM should be
framed as a mid-recall/average-IO optimizer rather than a universal high-recall
tail-latency solution.

### 4d. PFM uses PQ distances, which are noisy for low-PQ-quality indexes

PFM uses query-time PQ distances from the in-memory PQ table, so PQ quality
matters for the convergence signal. Do not infer PQ chunk count from the index
directory name alone:

- `B` in names like `B2` or `B40` is the DiskANN build memory budget, not the PQ
  chunk count.
- `QUERY_PQ_BYTES` fixes query-time PQ bytes/chunks.
- `DISK_PQ_BYTES` fixes the compressed vectors stored in the disk layout.

The old note claiming "Starling sift1m uses 34 chunks" was incorrect and should
not be cited. For each experiment, verify the actual chunk count from `build.log`
(`Compressing ... into N bytes per vector`) or from the index PQ metadata before
making claims about PQ quality.

### 4e. BFS cache hard limit of 10%

`cache_bfs_levels()` caps at 10% of total nodes. For sift100m this is 10M nodes
(~6.4GB). The alternative `generate_cache_list_from_sample_queries()` has no cap
but requires a sample query file. For a fair comparison with Starling's memory
graph, the sample-based cache is a better baseline.

Status: sample-based cache has not yet been implemented/measured in this
project. Treat it as a required baseline, not an established result.

---

## 5. Architecture Comparison: DiskANN vs Starling

| Feature | DiskANN | Starling |
| --- | --- | --- |
| IO unit | Single node sector (~640B) | 4KB page (contains ~6 nodes) |
| Graph layout | Unoptimized node order | Graph-partitioned for locality |
| Memory graph | BFS cache (10% limit) | Explicit in-memory navigation graph |
| Frequency tuning | None | Optional visit-frequency aware caching |
| PFM | Implemented (this work) | Implemented (this work, ported) |
| Index default | R128, L200 | R64, L100 |

Starling's core advantage is **page-level locality**: by reordering the graph so
that nearby nodes in the search path share 4KB pages, it gets 3–5× IO reduction
vs naive DiskANN before any algorithmic improvement. PFM then compounds this by
skipping pages entirely when convergence is detected.

---

## 6. Fair Comparison Protocol

When comparing DiskANN+PaceANN vs Starling+PFM, use these rules:

1. **Same dataset, same query file, same groundtruth.**
2. **Same thread count T and beamwidth W.**
3. **Same or explicitly justified index quality.** The cleanest ablation is to
   run beam and page search on the same Starling index. If DiskANN uses a higher
   quality index (`R`, build `L`, disk/search PQ, cache, or memory budget), state
   that the comparison is system-level rather than algorithm-only.
4. **No workload-aware frequency** (`GP_USE_FREQ=0`, `MEM_USE_FREQ=0`) for
   the main comparison.
5. **Report Pareto frontier** over L sweep, not single operating points.
6. **Report both QPS-Recall and p99-Recall Pareto** — a method that wins QPS
   but degrades p99 is not a clean win.
7. **Starling page-only** (`USE_PAGE_SEARCH=1`, `MEM_L=0`) is the primary
   Starling target. Starling with memory graph is a secondary target.
8. If using BFS cache in DiskANN, explicitly state cache size as a fraction of
   total nodes.
9. Report the chosen `L`, `theta`, `divergence_k`, `MIN_EXPLORE_HOPS`, cache
   count, memory graph settings, and PQ bytes for every plotted point.
10. If a component is named as part of PaceANN, include ablation for it. In
    particular, DRA needs `divergence_k=0` vs nonzero comparisons.

---

## 7. What to Implement Next

Priority order for making DiskANN-style beam search beat Starling page-only:

### Required cleanup before paper claims

The following items must be resolved before turning this document into a paper
story:

| Issue | Why it matters | Current action |
| --- | --- | --- |
| Starling sift100m baseline missing | Sift100M claims are not grounded without page-only/Page+PFM numbers. | Building compressed smoke index; full R128/L300 needs more RAM or streaming partitioner. |
| deep1m/gist1m/text2image1m missing | Need cross-dataset evidence; gist stresses high-dimensional PQ, text2image stresses MIPS. | Run same Page-only/Page+PFM/Page ECG suite on existing 1M page indices. |
| DRA ablation missing | DRA is named as a component but its marginal gain is unknown. | Sweep `divergence_k=0` vs `0.2/0.3/0.4`. |
| PFM stop rate by target missing | High-recall usefulness depends on stop rate at high L. | Report stop rate for each recall target and L. |
| `MIN_EXPLORE_HOPS=2` unvalidated | Early/late stop guard may affect recall and p99. | Sweep 1, 2, 3, 4, 5. |
| Cache synergy mechanism unproven | "Superlinear" needs mechanism, not only aggregate QPS. | Compare stop rate, hard-query IO, and p99 with/without cache. |
| Sample-based cache absent | BFS cache is topology-driven and capped at 10%. | Implement sample-query access-frequency cache baseline. |
| RAM pivot sweep too narrow | 1% random graph may understate entry-routing potential. | Test seed counts 2/4/8 and larger/higher-quality RAM indexes. |
| Beam+PFM p99 vs Page+PFM p99 unexplained | Tail behavior determines research direction. | Compare hard-query telemetry between beam and page at same recall target. |
| Page-level PFM status needs clear framing | This used to be a missing design in older notes, but it is now implemented in Starling page search. | Report it as our Page+PFM baseline and include DRA/stop-rate ablations. |

Interpretation rule: if an item above is unresolved, frame the corresponding
text as an observation or hypothesis, not a final claim.

### Step 1: Add beam-search telemetry

Before adding more mechanisms, measure why beam+PFM loses. Add per-query
telemetry for beam search:

```
query_id
L
theta
divergence_k
hop_count / expanded_count
mean_ios or per-query IO count
total_us
stopped_by_pfm
final pq_ratio / effective_theta
```

Goal: identify whether the gap is caused by hard queries not stopping, bad PFM
signals, excessive random IO, or recall recovery requiring larger L.

Status: implemented in Starling `search_disk_index` via:

```
--telemetry_path <output_prefix>
```

This writes one CSV per searched L:

```
<output_prefix>_L<L>.csv
<output_prefix>_L<L>_hops.csv
```

The first file is per-query summary. The second file is per-hop runtime signal
data. The hop CSV is the important file for designing a runtime policy.

Runtime-visible hop features:

```
hop
n_ios
n_expanded
cur_list_size
k
frontier_size
cached_size
n_cmps
best_unexpanded_pq
kth_pq
pq_ratio
delta_ratio
ema_delta
effective_theta
pfm_stopped
```

Post-hoc labels attached for analysis only:

```
final_total_us
final_query_recall_percent
is slow query       (derived by analyzer, not in runtime)
is low recall query (derived by analyzer, not in runtime)
```

Do not use post-hoc labels in an online stopping rule. They are only for finding
patterns in the runtime-visible signals.

Useful analyzer:

```
python3 scripts/analyze_beam_telemetry.py <telemetry_csv>
python3 scripts/analyze_hop_telemetry.py <hop_telemetry_csv>
```

Smoke-test result (`sift1m`, beam+PFM, `L=80`, `theta=1.15`, `dk=0.5`):

| Group | mean | p99 | mean IOs | p99 IOs | mean hops | p99 hops | mean recall | stop rate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| all queries | 2.719ms | 4.549ms | 58.25 | 94 | 15.36 | 25 | 99.50% | 89.25% |
| PFM stopped | 2.536ms | 4.187ms | 54.28 | 89 | 14.32 | 23 | 99.61% | 100% |
| not stopped | 4.237ms | 4.889ms | 91.21 | 96 | 24.00 | 26 | 98.61% | 0% |
| p99 slow queries | 5.182ms | 9.061ms | 90.00 | 97 | 23.85 | 28 | 99.00% | 14% |

Initial interpretation: the slow tail is dominated by high-IO/high-hop queries,
and many of them do not trigger PFM. Non-stopped queries are both slower and less
accurate on average. This supports hard-query-specific mechanisms such as ECG,
adaptive-L, or IO-locality-aware expansion rather than simply lowering theta.

Per-hop smoke-test output:

```
reports/sift1m_beam_telemetry_smoke_hops/beam_pfm_theta1p15_dk0p5_L80_hops.csv
```

It contains 153,619 hop rows for 10,000 sift1m queries. Example interpretation:
some slow queries reach hop 24-25 with `pq_ratio < effective_theta`, so PFM never
fires even though IO is already high. This is a runtime-detectable pattern:

```
high n_ios_so_far
high hop count
pq_ratio repeatedly below effective_theta
frontier still not converged
```

This pattern should drive the next design, for example switching hard queries to
a different budget/policy instead of treating them like ordinary easy queries.

Hop-level fast-vs-slow comparison from the same smoke test:

| Hop | Group | n_ios | pq_ratio | effective_theta | final recall |
| ---: | --- | ---: | ---: | ---: | ---: |
| 2 | fast p1 | 5.00 | 0.6936 | 1.1960 | 99.90% |
| 2 | slow p99 | 5.00 | 0.8081 | 1.1788 | 99.00% |
| 5 | fast p1 | 17.00 | 0.6655 | 1.1574 | 99.90% |
| 5 | slow p99 | 17.00 | 0.8476 | 1.1549 | 99.00% |
| 10 | slow p99 | 37.00 | 1.0060 | 1.1395 | 99.00% |
| 20 | slow p99 | 77.00 | 1.0966 | 1.1464 | 98.94% |

Early observation: slow queries already show a higher `pq_ratio` at hop 2/5 but
still remain below the stop threshold. Later they hover near the threshold
without crossing it, accumulating IO. This suggests a possible runtime hard-query
classifier based on early ratio trajectory, but the first implementation should
still be rule-based and interpretable.

### Step 2: Implement ECG for beam search

PFM currently uses PQ distance. Add an exact-distance convergence gate:

```
if hop_round < MIN_EXPLORE_HOPS: do not stop
if retset has fewer than K results: do not stop
if hop_best_exact > kth_exact * alpha: stop
```

This should reduce unsafe PQ-driven behavior and may help p99 by stopping when
exact distances already show convergence.

Status: first naive ECG implementation is available in Starling beam search:

```
--ecg_alpha <alpha>       # 0 disables ECG
--ecg_min_hops <hops>     # default 2
```

Current condition:

```
if hop_best_exact > kth_exact * ecg_alpha:
    stop
```

Status caution: the "Protected ECG" design should be treated as a proposal until
there are runtime measurements. The current rule needs sensitivity sweeps for
`ecg_alpha`, `ecg_pq_guard`, and `ecg_min_hops`; do not claim it as a proven
component without those ablations.

Smoke-test results on `sift1m`, `L=80`, `theta=1.15`, `dk=0.5`:

| ECG alpha | Recall | mean | p99 | mean IOs | mean hops | PFM stops | ECG stops |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| disabled / 1.15 | 99.50% | 2.726ms | 4.555ms | 58.25 | 15.36 | 8,925 | 0 |
| 1.14 | 99.48% | 2.719ms | 4.571ms | 58.05 | 15.31 | 8,618 | 1,327 |
| 1.12 | 99.28% | 2.541ms | 4.368ms | 54.09 | 14.29 | 5,158 | 6,236 |
| 1.10 | 98.89% | 2.304ms | 3.985ms | 48.71 | 12.93 | 3,178 | 8,250 |

Interpretation: naive ECG has a real speed/IO signal, but the recall-safe band is
too narrow. `alpha=1.10` and `1.12` reduce latency/IO but lose too much recall.
`alpha=1.14` is close to recall-safe but barely improves latency. Therefore ECG
should not be used alone as the next final method. The next useful variant is a
**protected ECG** that only fires when exact convergence and runtime frontier
signals agree, for example:

```
hop >= min_hops
hop_best_exact > kth_exact * alpha
pq_ratio > lower_guard
recent kth_pq improvement is small
```

This keeps ECG interpretable while reducing false positives from exact-distance
noise early in the search.

### Step 3: Add adaptive-L / per-query stopping policy

The ideal method should not use one fixed L for all queries. Easy queries should
stop early; hard queries can consume more budget only when needed. This is likely
necessary to beat page-only by a large margin.

### Step 4: Investigate page-aware batching for beam search

The four-way comparison indicates page layout is the main structural advantage.
If ECG/adaptive-L cannot close the gap, add a beam-search variant that groups
nearby candidates by disk page or sector before issuing reads:

```
same distance-first search semantics
but batch/tie-break candidates by IO locality
```

This aims to keep DiskANN-style beam search while attacking Starling's page IO
advantage directly.

### Step 5: Sample-based cache as an optional upper layer

Do not make memory graph/cache the primary contribution yet. Use it as an
optional upper layer after the beam-search core is improved. If used, prefer a
sample-based hot-node cache over BFS cache, and report cache size explicitly.

For Starling, the memory navigation graph (`build_mem` phase) already serves this
role. To match Starling fairly in DiskANN, sample-based caching is the right
analog.

---

## 8. Recent Negative Results and Direction Change

### Dynamic PageAware Beam is not a strong main contribution

After fixing the page-search distance path for disk-PQ layouts, the corrected
quick Pareto results are in:

```
reports/pageaware_quick_pareto_fixed/analysis/
```

Key corrected `sift1m` points:

| Target | Page+PFM QPS | Dynamic/adaptive QPS | Interpretation |
| ---: | ---: | ---: | --- |
| Recall >= 99.80 | 7,130 | 7,329 | small QPS win, worse p99 |
| Recall >= 99.85 | 6,583 | 6,720 | small QPS win, much worse p99 |
| Recall >= 99.90 | 4,584 | no dynamic point | dynamic does not reach target |

Cross-dataset result:

- `deep1m`: small QPS gain near 99.8, worse p99.
- `text2image1m`: Page+PFM wins clearly.
- `gist1m`: current recall range is only ~73-74%, so it is not a valid
  high-recall comparison yet.

Conclusion: Dynamic PageAware Beam should not be the paper's core idea. It can be
kept as a negative result / ablation showing that simply expanding extra
page-local nodes has low headroom.

### Two-stage Page+PFM has theoretical headroom, but current runtime classifiers are weak

Two-stage setup tested on `sift1m`:

- Low stage: Page+PFM, `L=80`, `theta=1.15`
- High stage candidates: Page+PFM, `L=100/150/200`, `theta=1.20/1.25`
- Output directory:

```
reports/sift1m_two_stage_page_pfm/
reports/sift1m_two_stage_page_pfm_pfmtelemetry/
```

Simple runtime threshold routers using `n_ios`, `n_hops`, or low-stage latency
were not enough:

| Router | Best useful result |
| --- | --- |
| Sequential fallback | too slow; p99 worsens significantly |
| Hedged threshold router | reaches ~99.81 recall with good QPS, but fails to reach 99.85 |
| Thresholds on IO/hops/latency | require escalating too many queries for 99.85 |

Oracle upper bound is much stronger. If we could perfectly identify the ~1% of
queries where high-stage search improves recall, the approximate result is:

| High stage | Target | Escalation rate | QPS est. | p99 | Mean IO |
| --- | ---: | ---: | ---: | ---: | ---: |
| L100 theta=1.20 | 99.85 | 1.1% | 8,979 | 3.03ms | 53.68 |
| L150 theta=1.20 | 99.90 | 1.5% | 8,923 | 3.14ms | 54.31 |
| L150 theta=1.25 | 99.90 | 1.4% | 8,906 | 3.28ms | 54.34 |

This is a useful upper bound: two-stage search can be very strong if hard-query
routing is accurate. The blocker is not the two-stage structure; it is hard-query
identification.

### Current learned router is not enough

A smoke-test GradientBoosting router was trained using runtime-visible features:

- low-stage latency
- IO/hop counts
- PFM stop metadata
- top-k result distance statistics

Result for `L80 theta=1.15 -> L150 theta=1.25`:

```
test AUC = 0.7198
test AP  = 0.0609
positives = 180 / 10000
```

Best test result:

| Target | Result |
| ---: | --- |
| Recall >= 99.80 | reachable, QPS est. ~8,437, p99 ~4.83ms |
| Recall >= 99.85 | no test point |
| Recall >= 99.90 | no test point |

Interpretation: existing coarse runtime features are not enough to reproduce the
oracle. The next useful features must be collected earlier and more directly from
the search process:

- per-hop `pq_ratio` trajectory for Page+PFM, not only final ratio
- kth-PQ improvement over hops
- frontier distance variance / entropy
- page revisit / page fanout behavior
- overlap between low-stage top-k and frontier candidates
- first-hop or second-hop local intrinsic difficulty signals

### Page+PFM hop-level telemetry improves signal but still does not close the gap

Page search now records hop-level telemetry when `--telemetry_path` is set:

```
<prefix>_L<L>_hops.csv
```

The hop CSV includes per-hop runtime signals:

- `best_unexpanded_pq`
- `kth_pq`
- `pq_ratio`
- `delta_ratio`
- `ema_delta`
- `effective_theta`
- current frontier/list sizes
- cumulative IO/expanded/comparison counts

Smoke output:

```
reports/page_pfm_hop_telemetry_smoke/
```

A hop-feature GradientBoosting router was tested using rolling windows over the
first 2/3/4/5/8/12 hops:

```
reports/sift1m_two_stage_page_pfm_pfmtelemetry/analysis/hop_router_L80t115_to_L150t125.csv
```

Best classification quality:

| Hop window | Test AUC | Test AP |
| ---: | ---: | ---: |
| 2 | 0.6780 | 0.0607 |
| 5 | 0.7275 | 0.0832 |
| 12 | 0.7899 | 0.1195 |

Best test-set operating point:

| Target | Result |
| ---: | --- |
| Recall >= 99.80 | reachable, QPS est. ~8,482, p99 ~4.78ms |
| Recall >= 99.85 | no point |
| Recall >= 99.90 | no point |

The all-query split can reach 99.85, but this is not evidence of generalization:

```
all target>=99.85: window=12, top_rate=0.15, QPS est. 7883, p99 4.89ms
```

Interpretation: hop-level PFM trajectory is a better signal than final-only
telemetry, but it still does not approach the oracle. The positive class is very
rare (~1.8%), and the current features do not identify the tiny set of queries
whose recall is fixed by high-L fallback.

Next feature candidates:

1. record the first few raw frontier candidate distances, not only aggregate
   ratios;
2. record page-local candidate distance distribution after each page read;
3. record overlap/stability of top-k result ids across hops;
4. train on multiple datasets jointly to test whether the hard-query signal is
   dataset-specific;
5. if these still fail, stop two-stage routing and pivot to scheduling-level
   methods.

### Rich hop features were tested and still did not reach the oracle

Additional Page+PFM hop telemetry was added:

- selected frontier PQ min/mean/max;
- current retset top-k PQ mean/std/gap;
- page-local candidate rank-distance min/mean/max/count.

Smoke output:

```
reports/page_pfm_rich_hop_telemetry/
```

Rich hop router result:

```
reports/sift1m_two_stage_page_pfm_pfmtelemetry/analysis/rich_hop_router_L80t115_to_L150t125.csv
```

Best observed classification quality:

| Hop window | Test AUC | Test AP |
| ---: | ---: | ---: |
| 8 | 0.7360 | 0.0736 |
| 12 | 0.8046 | 0.1128 |

Best test-set operating point:

| Target | Result |
| ---: | --- |
| Recall >= 99.80 | reachable, QPS est. ~8,352, p99 ~4.78ms |
| Recall >= 99.85 | no point |
| Recall >= 99.90 | no point |

Interpretation: richer aggregate hop features still do not identify the small
set of high-value fallback queries. The oracle remains strong, but the current
runtime feature family is not enough. Continuing to add aggregate features is
unlikely to be the fastest path forward.

Decision: pause the two-stage learned-router line unless we are willing to add
substantially heavier features, such as explicit top-k ID stability or comparing
two independent low-budget searches. For now, shift focus to scheduling-level
methods where the target is tail latency isolation rather than recall rescue.

### Updated research direction

Stop spending time on Dynamic PageAware Beam as a main method. The more promising
path is:

1. Add richer Page+PFM hop-level telemetry.
2. Re-run two-stage oracle and learned-router analysis using only runtime
   features available before fallback.
3. If the router can approach the oracle, implement real hedged/two-stage runtime
   search.
4. If not, pivot from per-query fallback to server-level scheduling:
   easy/hard queues, SRPT-style routing, or IO-priority lanes.

### Scheduling-level simulation: promising under high offered load

A first server-side scheduling simulation was added:

```
scripts/analyze_query_scheduling.py
scripts/plot_query_scheduling.py
reports/sift1m_scheduling_sim/analysis/
```

Input:

```
sift1m Page+PFM L=80 theta=1.15 telemetry
T=16 workers
Poisson arrivals
```

Policies simulated:

- FIFO
- oracle SPT using true service time
- IO-count SPT using low-cost runtime `n_ios`
- hop-count SPT using runtime `n_hops`
- PFM easy-first
- fixed hard-isolation lanes

Representative p99 response latency:

| Offered load | FIFO | IO-count SPT | Oracle SPT | Interpretation |
| ---: | ---: | ---: | ---: | --- |
| 0.70 | 3.08ms | 3.18ms | 3.17ms | little queueing, scheduling does not matter |
| 0.80 | 3.84ms | 4.09ms | 4.07ms | FIFO still fine |
| 0.90 | 8.00ms | 6.76ms | 6.64ms | SPT starts reducing tail |
| 0.95 | 17.23ms | 12.17ms | 11.99ms | clear p99 win |
| 0.98 | 40.25ms | 23.64ms | 22.63ms | strong tail-isolation effect |

Main observations:

1. Scheduling is irrelevant at low load; it matters only once queueing dominates.
2. IO-count SPT nearly matches oracle SPT at high load, which is a good sign:
   a cheap runtime signal can approximate query size.
3. Fixed hard/easy lane isolation performed badly because the hard lane can
   overload and explode p99.
4. The right direction is not static hard-lane partitioning. It is a dynamic
   non-preemptive SRPT/SPT-like scheduler or priority queue driven by cheap
   runtime size estimates.

Plots:

```
reports/sift1m_scheduling_sim/analysis/scheduling_p99_response.png
reports/sift1m_scheduling_sim/analysis/scheduling_p99_queue.png
reports/sift1m_scheduling_sim/analysis/scheduling_mean_response.png
```

Next implementation target:

```
Page+PFM runtime scheduler:
  maintain a shared ready queue
  prioritize queries by observed IO/hop count or predicted remaining work
  avoid fixed hard-lane reservation
  evaluate under open-loop offered load, not only closed-loop batch throughput
```

### Hop-level work-stealing simulation: not promising for p99

A hop-level work-stealing simulator was added:

```
scripts/analyze_hop_work_stealing.py
reports/sift1m_scheduling_sim/analysis/page_pfm_L80t115_hop_work_stealing.csv
```

It converts each query into a sequence of hop-sized work units using the
Page+PFM hop telemetry, then simulates multiple scheduling policies:

- full-query FIFO baseline;
- hop FIFO;
- priority by remaining hop count;
- oracle priority by remaining service;
- attained-service priority.

Important implementation correction: each query's hops must execute serially.
An earlier simulation accidentally allowed multiple hops of the same query to run
in parallel, which produced impossible response times smaller than the query's
own service time. The corrected simulator uses continuation-ready events.

Corrected result:

| Offered load | Query FIFO p99 | Hop FIFO p99 | Remaining-hop p99 | Oracle remaining-service p99 |
| ---: | ---: | ---: | ---: | ---: |
| 0.90 | 5.04ms | 5.04ms | 7.70ms | 7.69ms |
| 0.95 | 6.79ms | 6.79ms | 17.87ms | 17.37ms |
| 0.98 | 8.55ms | 8.55ms | 32.72ms | 25.87ms |

Interpretation:

1. Hop FIFO is effectively the same as query FIFO for this workload.
2. Remaining-work priority reduces mean latency but significantly worsens p99 by
   starving long/hard queries.
3. Hop-level work-stealing is not a good p99-focused main direction unless it is
   paired with explicit fairness/aging.

Decision: do not use hop-level work-stealing as the next main implementation.
The remaining scheduling question is whether cheap runtime query-size estimates
can be used without starving hard queries.

### Query-size predictor: useful signal, wrong scheduler

A query-size predictor was added:

```
scripts/analyze_query_size_predictor.py
reports/sift1m_query_size_predictor/analysis_r20/
```

It trains out-of-fold histogram GBDT models from early Page+PFM hop telemetry and
uses the predicted service size for non-preemptive SPT-like scheduling. Runtime
feature windows are limited to information available after the first N hops, so
this is closer to a deployable scheduler than final-query oracle analysis.

Prediction quality is strong enough to be useful:

| Early-hop window | Service MAE | Long-query AUC | Long-query AP |
| ---: | ---: | ---: | ---: |
| 1 | 393.7us | 0.761 | 0.248 |
| 3 | 358.4us | 0.812 | 0.321 |
| 5 | 307.6us | 0.862 | 0.395 |
| 8 | 232.6us | 0.906 | 0.506 |
| 12 | 131.0us | 0.956 | 0.667 |

However, pure SPT is the wrong p99 objective. At high offered load, it improves
mean response time but worsens p99 by delaying hard queries:

| Offered load | FIFO mean | FIFO p99 | Predicted SPT mean | Predicted SPT p99 | Oracle SPT p99 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 0.90 | 2.121ms | 4.395ms | 2.063ms | 6.811ms | 6.600ms |
| 0.95 | 2.728ms | 6.708ms | 2.519ms | 14.814ms | 12.796ms |
| 0.98 | 3.800ms | 8.643ms | 3.294ms | 31.686ms | 26.413ms |

Interpretation:

1. Early-hop features can predict hard queries, so the signal is not the blocker.
2. Non-preemptive SPT optimizes mean latency, not p99 tail latency.
3. A p99-focused scheduler must include fairness or a deadline/budget guard.

Updated direction:

```
Do not pursue pure SPT as the paper mechanism.
Use query-size prediction only as a component for:
  bounded SPT with aging;
  hedged request trigger;
  adaptive L/budget selection;
  admission control for high-load server experiments.
```

### Oracle stop boundary: early-stop headroom is large

An oracle stop-boundary trace was added for sift1m Page search:

```
reports/sift1m_oracle_stop_trace/analysis/page_L80_nopfm_L80_hops.csv
reports/sift1m_oracle_stop_trace/analysis/page_L80_nopfm_oracle_stop_summary.csv
scripts/analyze_oracle_stop_boundary.py
```

Instrumentation change: hop telemetry now records the current exact top-K ids at
each hop and writes `hop_recall_percent` into the hop CSV. This lets us ask:

```
If the query stopped at hop h, what recall would it have achieved?
```

Setup:

```
sift1m, Starling R64/L100/B2/M2
Page search, L=80, W=4, T=16, K=10
PFM disabled
Final result: Recall@10=99.86%, mean IOs=77.03, p99=3.373ms
```

Oracle result:

| Policy | Achieved Recall@10 | Mean IOs | IO saving |
| --- | ---: | ---: | ---: |
| Full Page L80, no PFM | 99.862% | 77.03 | 0% |
| Strict oracle: earliest hop matching final per-query recall | 99.862% | 30.97 | 59.79% |
| Target oracle: average recall >= 99.80% | 99.812% | 30.69 | 60.16% |
| Target oracle: average recall >= 99.50% | 99.512% | 29.87 | 61.23% |

This is the strongest evidence so far that **early stop still has large
unexploited headroom** even on Starling page search. PFM is far from oracle:

```
Page L80 + PFM theta=1.15:
  Recall@10 = 99.73%
  mean IOs  = 52.92
  PFM stop rate = 87.43%
```

The strict oracle uses only ~31 mean IOs while preserving the no-PFM final
recall. Therefore the problem is not that page search cannot stop earlier; the
problem is identifying the safe boundary without ground truth.

### Learned stop classifier: promising signal, not yet oracle-like

A first offline runtime-feature stop classifier was added:

```
scripts/analyze_learned_stop_classifier.py
reports/sift1m_oracle_stop_trace/analysis/learned_stop_page_L80_nopfm/
```

It trains a histogram GBDT on runtime-visible hop features only. Label:

```
stop_safe = (hop_recall_percent >= final_query_recall_percent)
```

Train/test split is by query, so test-hop rows come from unseen queries.

Model quality:

```
AUC = 0.9970
AP  = 0.9997
positive rate = 0.9222
```

Threshold simulation on test queries:

| Threshold | Recall@10 | Mean IOs | IO saving |
| ---: | ---: | ---: | ---: |
| 0.95 | 99.503% | 43.34 | 43.75% |
| 0.98 | 99.740% | 51.12 | 33.64% |
| 0.99 | 99.830% | 59.98 | 22.15% |
| 0.995 | 99.860% | 69.39 | 9.93% |
| Strict oracle | 99.880% | 31.09 | 59.64% |

Interpretation:

1. Runtime hop features can separate safe vs unsafe states very well.
2. A generic safe/unsafe classifier still triggers too late at high recall
   targets. It leaves most oracle IO savings unused.
3. The next learned-stop design should be **boundary-focused**, not a generic
   classifier over all safe hops. Possible fixes:
   - train on the first safe hop vs nearby unsafe hops;
   - optimize for false-positive-constrained recall, not AUC/AP;
   - combine with PFM monotonic ratio as a guard;
   - add exact-distance ECG features from the current hop;
   - calibrate per target recall (`99.5`, `99.8`, `99.85`) instead of using one
     universal threshold.

Decision: learned early stop is worth continuing, but only if the next
experiment targets the **oracle boundary** directly. Pure PFM tuning and generic
GBDT classification are not enough.

### Boundary-focused learned stop: better objective, still conservative

A boundary-focused classifier was added:

```
scripts/analyze_boundary_stop_classifier.py
reports/sift1m_oracle_stop_trace/analysis/boundary_stop_page_L80_nopfm/
```

Instead of labeling every safe hop as positive, it trains only around each
query's first safe hop:

```
negative examples: up to 5 hops before first-safe
positive examples: first-safe and up to 3 hops after it
```

This directly targets the early-stop boundary rather than the easy distinction
between very-late safe hops and very-early unsafe hops.

Result:

```
boundary AUC = 0.9539
boundary AP  = 0.9343
```

Threshold simulation on unseen test queries:

| Target / threshold | Recall@10 | Mean IOs | IO saving | False-early rate |
| --- | ---: | ---: | ---: | ---: |
| threshold 0.93 | 99.623% | 52.69 | 31.45% | 2.33% |
| threshold 0.97 | 99.817% | 64.96 | 15.50% | 0.37% |
| threshold 0.99 | 99.850% | 74.96 | 2.48% | 0.03% |
| Strict oracle | 99.860% | 30.97 | 59.71% | 0% |

Interpretation:

1. Boundary-focused training is more honest than generic safe/unsafe training.
2. It can produce a usable point around `Recall@10 ≈ 99.8`, but mean IO is still
   ~65, far from the oracle ~31.
3. The high-recall operating point is too conservative because false positives
   are punished per query. Paper metrics care about average recall, so the next
   model should optimize **average-recall-constrained IO saving**, not strict
   per-query safety.

Next learned-stop experiments:

```
1. Average-recall-constrained thresholding:
   choose threshold directly for target Recall@10 >= 99.8/99.85 on validation.

2. Boundary ranking:
   rank hop candidates inside each query and choose earliest hop under a
   validation-calibrated risk budget.

3. Hybrid guard:
   stop only when learned boundary score is high AND PFM/ECG signals agree.

4. Add exact-distance ECG features:
   hop_best_exact, kth_exact, exact_ratio, exact_delta.
```

### 95-99% recall view: learned stop beats page-only, not Page+PFM yet

A 95-99% recall-vs-mean-IO Pareto report was added:

```
scripts/plot_learned_stop_recall_io_pareto.py
reports/sift1m_oracle_stop_trace/analysis/recall_io_pareto_95_99_dense/
```

This compares:

- actual Page-only sweep from `reports/sift1m_pfm_sweep/results.csv`;
- actual Page+PFM sweep from the same file;
- offline Boundary learned stop using the Page L80 full trajectory;
- oracle stop from the same full trajectory.

Important caveat: Boundary learned stop is currently an offline mean-IO
simulation, not a C++ runtime implementation, so the clean comparison metric is
mean IOs, not measured QPS.

Best mean-IO points by target recall:

| Target Recall@10 | Page-only mean IOs | Page+PFM mean IOs | Boundary learned mean IOs | Oracle mean IOs |
| ---: | ---: | ---: | ---: | ---: |
| 95.0 | 38.71 | 29.18 | 28.65 | 29.07 |
| 96.0 | 38.71 | 29.18 | 29.70 | 29.07 |
| 97.0 | 38.71 | 29.18 | 31.58 | 29.07 |
| 98.0 | 38.71 | 32.30 | 34.55 | 29.07 |
| 99.0 | 38.71 | 37.36 | 42.11 | 29.07 |
| 99.5 | 46.39 | 43.93 | 50.20 | 29.87 |
| 99.8 | 77.03 | 61.40 | 63.56 | 30.69 |

Interpretation:

1. The 95-98% range is a valid target range; we do not need to optimize only for
   99.8%+ recall.
2. Boundary learned stop already beats Page-only across 95-98% by a large margin
   in mean IO.
3. Page+PFM is the stronger baseline. In 96-99.8% recall, Page+PFM still has
   lower mean IO than the current learned stop.
4. The oracle curve is dramatically better than Page+PFM. This confirms the
   opportunity is real, but the current learned boundary model has not captured
   it.

Research implication:

```
Do not claim learned stop beats Starling page search yet.
Claim instead:
  Oracle stop shows a large convergence-boundary gap.
  Boundary learned stop beats Page-only but not Page+PFM.
  The next contribution must close the gap to Page+PFM/Oracle,
  likely by adding exact convergence features and target-recall calibration.
```

### Hybrid learned stop with PFM/exact features: first positive result

The Page L80 no-PFM trace was regenerated after adding richer runtime telemetry:

```
reports/sift1m_oracle_stop_trace/analysis/page_L80_nopfm_rich_L80_hops.csv
```

New telemetry fields now populated for Page search:

- PFM frontier signals even when `pfm_theta=0`:
  `best_unexpanded_pq`, `kth_pq`, `pq_ratio`, `delta_ratio`, `ema_delta`.
- Exact convergence signals:
  `hop_best_exact`, `kth_exact`, plus derived model features
  `exact_ratio`, `exact_gap`, `pq_exact_ratio_gap`.

The boundary classifier was updated to use a train/validation/test split:

```
train: model fitting
validation: choose threshold for target recall
test: report recall/IO
```

Output:

```
reports/sift1m_oracle_stop_trace/analysis/boundary_stop_page_L80_nopfm_rich_calibrated/
reports/sift1m_oracle_stop_trace/analysis/recall_io_pareto_95_99_rich_calibrated/
```

Model quality:

```
validation AUC = 0.9801, AP = 0.9698
test AUC       = 0.9799, AP = 0.9704
```

Validation-calibrated test results:

| Target Recall@10 | Page-only mean IOs | Page+PFM mean IOs | Hybrid learned mean IOs | Hybrid test recall | Oracle mean IOs |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 95.0 | 38.71 | 29.18 | 27.09 | 95.79 | 29.07 |
| 96.0 | 38.71 | 29.18 | 27.53 | 96.41 | 29.07 |
| 97.0 | 38.71 | 29.18 | 28.57 | 97.24 | 29.07 |
| 98.0 | 38.71 | 32.30 | 30.44 | 98.00 | 29.07 |
| 99.0 | 38.71 | 37.36 | 35.80 | 98.94 | 29.07 |
| 99.5 | 46.39 | 43.93 | 44.39 | 99.52 | 29.87 |
| 99.8 | 77.03 | 61.40 | 57.61 | 99.84 | 30.69 |

Interpretation:

1. Adding PFM and exact convergence features materially improves the learned
   stop curve.
2. In the 95-98% range, Hybrid learned stop now beats Page+PFM in mean IO.
3. At the 99.8 target, Hybrid learned stop also beats Page+PFM in mean IO
   (`57.61` vs `61.40`) while test recall is `99.84`.
4. The 99.0 validation-calibrated threshold slightly misses target on test
   (`98.94`), so target calibration needs margin or conformal-style guarding.
5. The 99.5 point is roughly tied/slightly worse than Page+PFM.

This is the first result that looks like a real contribution:

```
Learned Convergence Boundary:
  Page search layout unchanged
  Same L80 full trajectory as the oracle source
  Runtime-visible PFM + exact convergence features
  Validation-calibrated thresholds by target recall
  Lower mean IO than Page+PFM at 95-98% and 99.8%
```

Remaining caveats:

- It is still offline simulation. The model has not been embedded into the C++
  search loop.
- QPS/p50/p99 are estimated only indirectly through mean IO; real runtime must
  be measured after implementation.
- The current target calibration can under-shoot; add a validation recall margin
  before claiming target compliance.

Next implementation target:

```
1. Add a small runtime stop-policy hook in Page search.
2. Start with a simple exported thresholded rule or generated tree ensemble.
3. Measure actual QPS/latency, not only mean IO.
4. Compare against Page-only and Page+PFM sweeps at 95, 96, 97, 98, 99, 99.5, 99.8.
```

### Runtime Page ECG rule: implemented and measured

A simple interpretable runtime stop rule was implemented in C++ Page search:

```
--page_ecg_alpha FLOAT
--page_ecg_pq_guard FLOAT
--page_ecg_min_hops INT
```

Runtime condition:

```
exact_ratio = hop_best_exact / kth_exact

if hop >= page_ecg_min_hops
   and exact_ratio >= page_ecg_alpha
   and pq_ratio >= page_ecg_pq_guard:
       stop
```

Default is disabled (`page_ecg_alpha=0`). The implementation also keeps Page
search telemetry useful by computing PFM frontier features even when PFM is not
enabled. Exact `kth_exact` is computed only when the runtime rule or telemetry is
enabled, so normal Page-only/Page+PFM runs are not penalized.

Offline rule sweep:

```
scripts/analyze_rule_stop_sweep.py
reports/sift1m_oracle_stop_trace/analysis/rule_stop_sweep/
```

Runtime no-telemetry measurement:

```
reports/sift1m_page_rule_stop_runtime_no_telemetry/
reports/sift1m_page_rule_stop_runtime_no_telemetry/analysis/runtime_rule_stop_vs_baselines.csv
```

Setup:

```
sift1m, Starling R64/L100/B2/M2
Page search, L=80, W=4, T=16, K=10
PFM disabled
```

Measured comparison against the best Page+PFM points at each target:

| Target Recall@10 | Rule config `(alpha, pq_guard)` | Rule recall | Rule QPS | Page+PFM QPS | QPS ratio | Rule p99 | Page+PFM p99 |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 95.0 | `(0.80, 1.00)` | 95.19 | 15,510 | 14,841 | 1.045x | 1.446ms | 1.538ms |
| 98.0 | `(1.00, 1.00)` | 98.03 | 13,544 | 13,552 | 0.999x | 1.705ms | 1.651ms |
| 99.0 | `(1.05, 1.00)` | 99.01 | 12,015 | 11,947 | 1.006x | 1.910ms | 1.904ms |
| 99.5 | `(1.00, 1.10)` | 99.50 | 10,086 | 10,285 | 0.981x | 2.727ms | 2.418ms |
| 99.8 | `(0.80, 1.15)` | 99.81 | 7,927 | 7,485 | 1.059x | 3.217ms | 3.022ms |

Interpretation:

1. This is now a real runtime mechanism, not only offline simulation.
2. At 95% and 99.8%, the simple rule beats Page+PFM in QPS.
3. At 98-99%, it is roughly tied with Page+PFM in QPS.
4. At 99.5%, it loses to Page+PFM.
5. p99 is still worse than Page+PFM at high recall. The current rule improves
   average IO/QPS more than tail latency.

Research implication:

```
We now have a concrete component:
  Runtime exact/PQ convergence gate for Page search.

It can beat Page+PFM in QPS at selected recall targets, but p99 still needs work.
The next step is not proving feasibility anymore; it is improving tail behavior
and making threshold calibration robust across datasets.
```

Baseline interpretation:

Page+PFM is **not original Starling**. PFM/DRA is our added stopping mechanism.
Therefore results should be reported with two baselines:

1. **Original Starling page-only**: page layout/search without PFM.
2. **Our strong baseline, Page+PFM**: Starling page search plus our PFM/DRA.

Against original Starling page-only, the runtime Page ECG rule is stronger:

| Target Recall@10 | Rule recall | Page-only recall | Rule QPS | Page-only QPS | QPS speedup | Rule p99 | Page-only p99 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 95.0 | 95.19 | 99.15 | 15,510 | 11,252 | 1.378x | 1.446ms | 1.739ms |
| 98.0 | 98.03 | 99.15 | 13,544 | 11,252 | 1.204x | 1.705ms | 1.739ms |
| 99.0 | 99.01 | 99.15 | 12,015 | 11,252 | 1.068x | 1.910ms | 1.739ms |
| 99.5 | 99.50 | 99.50 | 10,086 | 9,662 | 1.044x | 2.727ms | 1.979ms |
| 99.8 | 99.81 | 99.86 | 7,927 | 6,046 | 1.311x | 3.217ms | 3.046ms |

Output:

```
reports/sift1m_page_rule_stop_runtime_no_telemetry/analysis/runtime_rule_stop_vs_page_only.csv
```

Interpretation against original Starling:

1. Runtime Page ECG beats Page-only in QPS at all measured target regions.
2. It is especially strong at 95%, 98%, and 99.8%.
3. p99 improves at 95% and is roughly tied at 98%, but becomes worse at 99%+.
4. This is a valid first contribution claim if framed correctly:

```
Original Starling page search is static-L.
Runtime Page ECG adds adaptive convergence stopping.
It improves QPS/IO at matched recall targets, with a tail-latency tradeoff at
very high recall.
```

The stronger claim, beating **Page+PFM**, is partially true but more nuanced:
QPS wins at 95% and 99.8%, near-ties at 98-99%, and loses at 99.5; p99 still
needs improvement.

---

## 9. Key File Locations

| File | Purpose |
| --- | --- |
| `src/pq_flash_index.cpp` line ~1239 | PFM+DRA implementation in Starling |
| `src/page_search.cpp` | Starling page search IO path |
| `scripts/run_pfm_sweep.sh` | Run beam vs page × theta sweep on sift1m |
| `scripts/plot_pfm_pareto.py` | Plot results from sweep CSV |
| `scripts/plot_beam_page_pfm_comparison.py` | Plot beam/page baseline vs PFM frontier |
| `reports/sift1m_pfm_sweep/results.csv` | Page+PFM fine sweep |
| `reports/sift1m_beam_pfm_sweep/results.csv` | Beam+PFM fine sweep |
| `reports/sift1m_beam_page_pfm_comparison/analysis/` | Four-way comparison |
| `/home/gt/research/DiskANN/src/pq_flash_index.cpp` line ~1239 | PFM in DiskANN |
| DiskANN sift1m results | `DiskANN/scripts/paramAnalysis/gridSearch/outputFiles/search/sift1m_matched_L/` |
| DiskANN sift100m results | `DiskANN/scripts/paramAnalysis/gridSearch/outputFiles/search/sift100m_cache/` |

---

## 10. Numerical Summary for Paper

### sift1m (current Starling-internal targets, T=16/W=4/K=10):

| Target | QPS | p99 | Recall | Config |
| --- | ---: | ---: | ---: | --- |
| Beam baseline | 3,128 | 5.641ms | 99.80% | L=100 |
| Beam+PFM | 4,254 | 7.813ms | 99.80% | L=150, θ=1.18, dk=0.3 |
| Page-only | 6,046 | 3.046ms | 99.86% | L=80 |
| Page+PFM | 7,485 | 3.022ms | 99.80% | L=80, θ=1.18, dk=0.2 |

### sift100m (historical DiskANN operating points; Starling baseline missing):

| System | QPS | p99 | Recall |
| --- | ---: | ---: | ---: |
| DiskANN disk | 2,805 | 7.01ms | 98.87% |
| DiskANN cache (10M) | 3,369 | 6.23ms | 98.87% |
| DiskANN cache + PaceANN | 4,902 | 5.29ms | 98.58% |
| Starling page + PaceANN | TBD | TBD | TBD |

Do not describe this as a same-recall four-way comparison until the exact L and
recall target for each row are recovered. The current table mixes 98.87% and
98.58% recall. Starling page-only/Page+PFM on sift100m is still the largest
missing baseline.

### Core claims supported by data:

1. PaceANN reduces mean IO/QPS cost on measured sift1m and historical DiskANN
   sift100m operating points.
2. At matched or near-matched recall on validated sift1m runs, PaceANN/Page ECG
   improves QPS over the corresponding static baseline.
3. PaceANN + cache on sift100m shows a promising aggregate gain, but the
   "superlinear" mechanism still needs stop-rate and hard-query evidence.
4. Starling page+PFM is a strong upper target: at `Recall@10 >= 99.80%`, it
   reaches 7,485 QPS with 3.022ms p99, beating page-only by +23.8% QPS at
   similar p99.
5. Beam+PFM alone is not sufficient: at `Recall@10 >= 99.80%`, it improves beam
   QPS from 3,128 to 4,254 but worsens p99 from 5.641ms to 7.813ms and remains
   below page-only.
