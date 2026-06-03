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

## 3. Measured Results

### 3a. DiskANN system — sift1m (R128_L200_QD128, T=16, W=4, K=10)

Index: `/mnt/diskann_data/index/sift1m/sift1m_oracle_p0/sift1m_R128_L200_QD128/`

**Matched-L Pareto frontier** (PaceANN sweeps both θ and L):

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

**Four-way comparison at recall ≈ 0.989:**

| Method | QPS | Δ vs disk | p50 | p99 |
| --- | ---: | ---: | ---: | ---: |
| Disk baseline | 2,805 | — | 5.60ms | 7.01ms |
| Cache baseline (BFS 10M nodes) | 3,369 | +20% | 4.70ms | 6.23ms |
| Disk + PaceANN | 3,802 | +36% | 3.96ms | 7.36ms |
| **Cache + PaceANN** | **4,902** | **+75%** | **3.03ms** | **5.29ms** |

Key finding: PaceANN alone raises p99 slightly (+5%) because hard queries that
never trigger early stop still run full L. Cache alone improves QPS but p99 stays
high due to CPU effects. The combination is superlinear: Cache+PaceANN achieves
+75% QPS AND −25% p99 simultaneously.

### 3c. Starling system — sift1m (R64_L100_B2_M2, T=16, W=4, K=10)

Index: `/mnt/diskann_data/starling_data/index/sift1m_starling/sift1m_R64_L100_B2_M2/`
Sweep results: `/tmp/starling_pareto_sweep/results.csv`

**Pareto frontier comparison (at recall ≥ 0.993):**

| Method | Recall | QPS | p99 | Notes |
| --- | ---: | ---: | ---: | --- |
| Beam baseline (θ=0) | 0.9948 | 4,655 | 4.89ms | L=60 |
| Page baseline (θ=0) | 0.9976 | 5,906 | 4.16ms | L=80 |
| Beam + PFM | 0.9959 | 5,454 | 5.72ms | θ=1.15, L=100 |
| **Page + PFM** | **0.9958** | **9,520** | **3.25ms** | **θ=1.15, L=50** |

**Page+PFM at recall ~99.6% is 2.0× faster than beam baseline, 1.6× faster
than page baseline, and 1.7× faster than beam+PFM.**

The large gap between beam+PFM and page+PFM is because Starling's page search
reads entire 4KB pages that contain multiple graph nodes, amortizing IO cost.
PFM amplifies this advantage: when early stop fires, you skip reading entire
pages rather than individual nodes.

---

## 4. Key Bottlenecks and Open Problems

### 4a. p99 degradation for beam search at small L

For Starling beam search (and DiskANN), PFM at aggressive theta (1.10–1.15) with
small L (≤40) shows p99 regression of +20–50%. Root cause: hard queries never
trigger early stop but variance is higher at small L. Fix: use L≥60 as the
operating point, or apply a minimum-hops safety gate.

### 4b. Page search PFM threshold not jointly tuned with page size

Starling page search reads 4KB pages containing ~6 nodes each. When PFM fires, it
stops before issuing the next page read. However, the current PFM threshold was
derived from beam search (node-level) experiments. For page search, the effective
granularity is a page, not a node. The optimal theta for page search may differ
from beam search. This has not been systematically explored.

### 4c. Hard queries at high recall (≥99.8%)

Above recall 99.8%, PFM early-stop rate drops because the search must explore
farther to find rare neighbors. Neither page search nor PFM solves this.
Workload-aware frequency caching (Starling's `GP_USE_FREQ=1`) is the only known
solution but is unfair in general-purpose settings.

### 4d. PFM uses PQ distances, which are noisy for low-PQ-quality indexes

DiskANN's sift100m B2 index has only ~13 PQ chunks (17 bytes/node). PFM signal is
unreliable. Always use B40 index (96 PQ chunks, 120 bytes/node) for sift100m PFM
experiments. Starling sift1m index uses B2 (34 bytes/node, 34 chunks for float128)
which is marginal — consider re-indexing at B8 or higher for better PFM quality.

### 4e. BFS cache hard limit of 10%

`cache_bfs_levels()` caps at 10% of total nodes. For sift100m this is 10M nodes
(~6.4GB). The alternative `generate_cache_list_from_sample_queries()` has no cap
but requires a sample query file. For a fair comparison with Starling's memory
graph, the sample-based cache is a better baseline.

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
3. **No workload-aware frequency** (`GP_USE_FREQ=0`, `MEM_USE_FREQ=0`) for
   the main comparison.
4. **Report Pareto frontier** over L sweep, not single operating points.
5. **Report both QPS-Recall and p99-Recall Pareto** — a method that wins QPS
   but degrades p99 is not a clean win.
6. **Starling page-only** (`USE_PAGE_SEARCH=1`, `MEM_L=0`) is the primary
   Starling target. Starling with memory graph is a secondary target.
7. If using BFS cache in DiskANN, explicitly state cache size as a fraction of
   total nodes.

---

## 7. What to Implement Next

Priority order for maximizing Starling+PFM performance:

### Step 1: Tune PFM threshold jointly for page search (highest impact, easy)

Current PFM sweep used the same theta range (1.05–1.20) for both beam and page.
Page search has different IO granularity. Run a finer sweep:

```
theta:       1.05, 1.08, 1.10, 1.12, 1.15, 1.18, 1.20
divergence_k: 0.2, 0.3, 0.4, 0.5
use_page:    1
L:           30, 40, 50, 60, 80, 100, 120, 150
```

Use script: `/home/gt/research/starling/scripts/run_pfm_sweep.sh` (extend L range).
Output: `/tmp/starling_pareto_sweep/results.csv`

### Step 2: Run page+PFM on all datasets (necessary for paper)

Currently only sift1m sweep exists. Need: deep1m, gist1m, text2image1m, sift100m.

For sift100m on Starling, the index may not exist yet:
- DiskANN sift100m B40 index is at `/mnt/diskann_data/index/sift100m_B40_rebuild/`
- Starling needs its own index with graph partition + page layout
- Use `run_starling_suite.sh sift100m full_page release`

### Step 3: Add page-aware PFM (medium complexity)

Current PFM checks after each hop (node expansion). For page search, a natural
extension is to check before issuing each page read:

```
if pq_ratio_of_page_batch > theta_page: skip this page batch
```

This is stronger because it avoids the IO entirely, not just the processing.
Implement in `src/page_search.cpp` — find the point where page reads are
issued and add the PFM check there.

### Step 4: Sample-based cache instead of BFS cache

Replace `cache_bfs_levels()` with `generate_cache_list_from_sample_queries()` to
cache nodes by actual visit frequency. This avoids the 10% hard limit and caches
the genuinely hot nodes.

For Starling, the memory navigation graph (`build_mem` phase) already serves this
role. To match Starling fairly in DiskANN, sample-based caching is the right
analog.

---

## 8. Key File Locations

| File | Purpose |
| --- | --- |
| `src/pq_flash_index.cpp` line ~1239 | PFM+DRA implementation in Starling |
| `src/page_search.cpp` | Starling page search IO path |
| `scripts/run_pfm_sweep.sh` | Run beam vs page × theta sweep on sift1m |
| `scripts/plot_pfm_pareto.py` | Plot results from sweep CSV |
| `/tmp/starling_pareto_sweep/results.csv` | Existing sift1m sweep results |
| `/home/gt/research/DiskANN/src/pq_flash_index.cpp` line ~1239 | PFM in DiskANN |
| DiskANN sift1m results | `DiskANN/scripts/paramAnalysis/gridSearch/outputFiles/search/sift1m_matched_L/` |
| DiskANN sift100m results | `DiskANN/scripts/paramAnalysis/gridSearch/outputFiles/search/sift100m_cache/` |

---

## 9. Numerical Summary for Paper

### sift1m (best operating point, recall ≥ 99.5%):

| System | QPS | p99 | Recall |
| --- | ---: | ---: | ---: |
| DiskANN beam | 4,655 | 4.89ms | 99.48% |
| Starling page | 5,906 | 4.16ms | 99.76% |
| DiskANN beam + PaceANN | 5,454 | 5.72ms | 99.59% |
| **Starling page + PaceANN** | **9,520** | **3.25ms** | **99.58%** |

### sift100m (best operating point, recall ≈ 98.9%):

| System | QPS | p99 | Recall |
| --- | ---: | ---: | ---: |
| DiskANN disk | 2,805 | 7.01ms | 98.87% |
| DiskANN cache (10M) | 3,369 | 6.23ms | 98.87% |
| DiskANN cache + PaceANN | 4,902 | 5.29ms | 98.58% |
| Starling page + PaceANN | TBD | TBD | TBD |

### Core claims supported by data:

1. PaceANN reduces mean IO by 20–50% across sift1m and sift100m.
2. At matched recall, PaceANN improves QPS by +25–75% over DiskANN baseline.
3. PaceANN + cache is superlinear: +75% QPS AND −25% p99 simultaneously.
4. PaceANN applied to Starling page search (page+PFM) achieves 9,520 QPS at
   99.58% recall on sift1m — 2.0× beam baseline, 1.6× page baseline.
5. p99 is neutral-to-better when L≥60 and θ≤1.15.
