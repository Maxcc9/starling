# Workflows

## End-To-End KNN Experiment

All generated index artifacts from `scripts/run_benchmark.sh` are written under
`/mnt/diskann_data/starling_data/index` by default. The default index prefix is:

```bash
/mnt/diskann_data/starling_data/index/${PREFIX}_starling/${PREFIX}_R${R}_L${BUILD_L}_B${B}_M${M}/${PREFIX}_R${R}_L${BUILD_L}_B${B}_M${M}
```

For running the standard local dataset set (`sift1m`, `deep1m`, `gist1m`,
`text2image1m`, `sift100m`, `deep100m`, `spacev100m`), use
`scripts/run_starling_suite.sh`; see `DATASET_BENCHMARKS.md`.

Search logs, result files, CSV reports, and figures are written under the
repository `reports/` directory. Override with `STARLING_REPORT_ROOT` only when
needed.

Generate analysis CSVs and Pareto figures from search logs with:

```bash
scripts/generate_starling_reports.py \
  --report-dir reports/sift1m_starling/sift1m_R64_L100_B2_M2 \
  --dataset sift1m \
  --method Starling-full \
  --prefix starling_full_sift1m
```

## Starling Feature Ablations

Use feature ablations when comparing against DiskANN improvements. This avoids
collapsing page search, memory navigation, and workload-aware frequency tuning
into one overloaded "Starling" line.

```bash
cd /home/gt/research/starling/scripts
./run_starling_feature_ablation.sh sift1m plan
./run_starling_feature_ablation.sh sift1m prepare
./run_starling_feature_ablation.sh sift1m search
./run_starling_feature_ablation.sh sift1m reports
```

Feature lines:

| Line | Meaning | Fairness |
| --- | --- | --- |
| `beam` | Original beam-search baseline. | General-purpose |
| `page_only` | Page search with plain graph partition, no memory graph. | General-purpose |
| `page_ratio` | `page_only` with `PS_USE_RATIO=0.5`. | General-purpose |
| `page_random_mem` | Page search plus random memory graph. | General-purpose if memory budget is reported |
| `page_freq_mem` | Page search plus frequency-selected memory graph. | Workload-aware |
| `page_freq_gp` | Page search plus frequency-driven graph partition. | Workload-aware |
| `page_freq_gp_random_mem` | Frequency GP plus random memory graph. | Workload-aware because GP uses frequency |
| `page_freq_gp_freq_mem` | Frequency GP plus frequency memory graph. | Workload-aware full setting |

Reports are written under:

```text
reports/sift1m_ablation/<feature_line>/
reports/sift1m_ablation/analysis/
```

The combined analysis directory contains overlay Pareto plots across all
feature lines.

1. Configure dataset and parameters.

```bash
cd scripts
cp config_sample.sh config_local.sh
```

2. Build disk index.

```bash
./run_benchmark.sh release build
```

3. Optionally generate visit frequency.

```bash
./run_benchmark.sh release freq
```

4. Build in-memory navigation graph.

Random sampled nodes:

```bash
MEM_USE_FREQ=0 ./run_benchmark.sh release build_mem
```

Frequency-selected nodes:

```bash
MEM_USE_FREQ=1 ./run_benchmark.sh release build_mem
```

5. Graph partition and relayout for page search.

```bash
./run_benchmark.sh release gp
```

6. Search.

KNN:

```bash
./run_benchmark.sh release search knn
```

Range:

```bash
./run_benchmark.sh release search range
```

## Beam Search

Set:

```bash
USE_PAGE_SEARCH=0
USE_SQ=0
```

The script prefers `<index_prefix>_disk_beam_search.index` if present. If the
index has been graph-partitioned, keep the original beam-search index file
available.

Tunable KNN search parameters:

| Variable | Direct CLI option | Effect | Requires rebuild |
| --- | --- | --- | --- |
| `LS` | `-L`, `--search_list` | Search list sizes. Larger values usually improve recall and increase latency/IOs. | No |
| `BM_LIST` | `-W`, `--beamwidth` | Beamwidth. Larger values can improve search robustness and increase IO pressure. `0` lets the executable optimize internally. | No |
| `T_LIST` | `-T`, `--num_threads` | Search worker threads. Higher values may improve QPS but can increase per-query latency. | No |
| `K` | `-K`, `--recall_at` | Top-K results returned and Recall@K target. | No |
| `CACHE` | `--num_nodes_to_cache` | Number of BFS nodes cached around medoids before search. | No |
| `MEM_L` | `--mem_L` | Enables in-memory navigation graph when non-zero. | Requires memory index |
| `MEM_TOPK` | internal script/log field | Top-K from the memory navigation step when supported by the executable path. | Requires memory index |
| `USE_SQ` | `--use_sq` | Enables scalar-quantized disk vectors. Only supported with page search for float data. | Requires SQ conversion |

## Page Search

Set:

```bash
USE_PAGE_SEARCH=1
PS_USE_RATIO=1.0
```

Page search expects a partition file:

```text
<index_prefix>_partition.bin
```

Run the `gp` workflow first.

Page search uses the same KNN search parameters as beam search and adds:

| Variable | Direct CLI option | Effect | Requires rebuild |
| --- | --- | --- | --- |
| `USE_PAGE_SEARCH` | `--use_page_search` | `1` selects page search; `0` selects classic DiskANN beam search. | No |
| `PS_USE_RATIO` | `--use_ratio` | Fraction of vectors in each page to scan, in `[0, 1]`. Lower values can reduce work but may reduce recall. | No |
| `GP_TIMES` | `--ldg_times` in partitioner | Number of LDG graph-partition iterations. Higher values can improve locality but make partitioning slower. | Requires `gp` rerun |
| `GP_T` | `-T` in partitioner | Threads for graph partitioning. | Requires `gp` rerun |
| `GP_USE_FREQ` | `--freq_file` enabled | Uses query frequency data to guide partitioning. | Requires `freq` and `gp` rerun |
| `GP_LOCK_NUMS` | `--lock_nums` | Locks high-frequency nodes during partition initialization. Used with frequency-driven partitioning. | Requires `gp` rerun |
| `GP_CUT` | `--cut` | Degree cut used by frequency-driven partitioning. | Requires `gp` rerun |

Current `sift1m` baseline sweep uses:

```bash
BM_LIST=(4)
T_LIST=(8 16)
LS="50 100 150 200"
PS_USE_RATIO=1.0
GP_TIMES=8
GP_T=16
GP_USE_FREQ=0
GP_LOCK_NUMS=0
GP_CUT=4096
```

To compare beam and page search without rebuilding the disk index:

```bash
USE_PAGE_SEARCH=0 ./run_benchmark.sh release search knn
./run_benchmark.sh release gp
USE_PAGE_SEARCH=1 ./run_benchmark.sh release search knn
```

KNN search logs report `Mean Latency`, `P50 Latency`, `P99 Latency`, and
`99.9 Latency`.

## Disk Index Build Parameters

These parameters affect the disk index and require a new build when changed:

| Variable | Direct CLI option | Effect |
| --- | --- | --- |
| `R` | `-R`, `--max_degree` | Maximum graph degree. Higher values usually improve search quality and increase index size/build time. |
| `BUILD_L` | `-L`, `--Lbuild` | Build-time search list. Higher values usually improve graph quality and increase build time. |
| `B` | `-B`, `--search_DRAM_budget` | Search DRAM budget in GB. Indirectly determines the query-time PQ bytes per vector. |
| `M` | `-M`, `--build_DRAM_budget` | Build DRAM budget in GB. Controls memory available while building. |
| `BUILD_T` | `-T`, `--num_threads` | Build threads. |
| `QUERY_PQ_BYTES` | `--PQ_search_bytes` | Explicit query-time PQ bytes per vector. `0` keeps the legacy behavior derived from `B`. Use this for fixed qd-style experiments. |
| `DISK_PQ_BYTES` | `--PQ_disk_bytes` | Compresses vectors stored in the disk index to this many bytes. `0` stores full vectors. Needed for high-dimensional data that cannot fit one node in a 4KB sector. |
| `APPEND_REORDER_DATA` | `--append_reorder_data` | Appends full-precision data for re-ranking when disk-PQ is enabled. Float data only. |
| `DATA_TYPE` | `--data_type` | Dataset type: `float`, `uint8`, or `int8`. |
| `DIST_FN` | `--dist_fn` | Distance function: `l2` or `mips` for build. |
| `BASE_PATH` | `--data_path` | Base vector file. |
| `INDEX_NAME` | derived prefix | Optional explicit name to avoid overwriting or mixing incompatible builds. |

For `gist1m`, this repository sets `DISK_PQ_BYTES=256` because 960-dimensional
float vectors plus graph neighbors exceed the 4KB sector layout when stored
uncompressed.

There is no CLI parameter named `QD` in this codebase. The closest controls are:

- `QUERY_PQ_BYTES`: explicitly controls `<prefix>_pq_compressed.bin`, the
  query-time PQ vectors loaded by `PQFlashIndex`. For example, on 128-dim
  float `sift1m`, `QUERY_PQ_BYTES=128` is equivalent to qd128 and stores 128
  bytes per vector instead of 512 full-precision bytes, a 4x byte compression.

- `B`: when `QUERY_PQ_BYTES=0`, controls `<prefix>_pq_compressed.bin`
  indirectly. The code computes:

  ```text
  pq_bytes_per_vector = floor(search_DRAM_budget_bytes / number_of_points)
  pq_bytes_per_vector = min(pq_bytes_per_vector, DATA_DIM, 256)
  pq_bytes_per_vector = max(pq_bytes_per_vector, 1)
  ```

  For example, on `sift1m` with `B=2`, this is capped by `DATA_DIM=128`, so
  the query-time PQ file stores 128 bytes per vector even without
  `QUERY_PQ_BYTES`.

- `DISK_PQ_BYTES`: controls optional compression of the vectors physically
  stored inside the disk index. This is independent of `B`. Use it for
  high-dimensional datasets such as `gist1m`.

Changing `QUERY_PQ_BYTES` requires rebuilding the disk index because it changes
`<prefix>_pq_compressed.bin` and `<prefix>_pq_pivots.bin`. The generated index
name includes `_QPQ${QUERY_PQ_BYTES}` when this setting is non-zero.

## Memory Navigation Graph Parameters

These parameters affect the optional in-memory graph used when `MEM_L > 0`:

| Variable | Direct CLI option | Effect |
| --- | --- | --- |
| `MEM_R` | `-R` in `build_memory_index` | Memory graph max degree. |
| `MEM_BUILD_L` | `-L` in `build_memory_index` | Memory graph build list size. |
| `MEM_ALPHA` | `--alpha` | Vamana pruning alpha. |
| `MEM_RAND_SAMPLING_RATE` | `gen_random_slice` input | Random sample rate for memory graph data. |
| `MEM_USE_FREQ` | script branch | `0` builds from random sample; `1` builds from frequency-selected points. |
| `MEM_FREQ_USE_RATE` | `parse_freq_file` input | Fraction of frequent points selected for memory graph. |
| `MEM_L` | `--mem_L` in search | Search list for the memory navigation graph. `0` disables it. |

## Frequency Generation Parameters

Frequency files are used by frequency-driven memory graph construction and
frequency-driven graph partitioning:

| Variable | Direct CLI option | Effect |
| --- | --- | --- |
| `FREQ_QUERY_FILE` | `--query_file` | Query file used to estimate node visit frequency. |
| `FREQ_QUERY_CNT` | `--expected_query_num` | Number of queries to use. `0` uses all queries. |
| `FREQ_BM` | `-W` | Beamwidth during frequency generation. |
| `FREQ_L` | `-L` | Search list during frequency generation. |
| `FREQ_T` | `-T` | Threads during frequency generation. |
| `FREQ_CACHE` | `--num_nodes_to_cache` | Cached medoid BFS nodes during frequency generation. |
| `FREQ_MEM_L` | `--mem_L` | Optional memory graph search list during frequency generation. |
| `FREQ_MEM_TOPK` | script/log field | Memory graph top-K setting when supported. |

## Page Search With In-Memory Navigation Graph

Set:

```bash
MEM_L=5
MEM_INDEX_PATH=<built memory index prefix>
```

Through `run_benchmark.sh`, `MEM_INDEX_PATH` is derived from the configured
memory graph parameters. Direct CLI users must pass `--mem_L` and
`--mem_index_path`.

## Frequency-Driven Memory Graph

1. Generate frequency:

```bash
./run_benchmark.sh release freq
```

2. Build memory graph from frequency:

```bash
MEM_USE_FREQ=1 ./run_benchmark.sh release build_mem
```

3. Search with `MEM_L > 0`.

## Frequency-Driven Graph Partition

Set:

```bash
GP_USE_FREQ=1
GP_LOCK_NUMS=<value>
GP_CUT=<max_degree_cut>
```

Then run:

```bash
./run_benchmark.sh release gp
```

The partitioner receives `--freq_file <freq_path>_freq.bin`.

## Scalar Quantization Page Search

1. Build the disk index.
2. Run SQ conversion.

```bash
./run_benchmark.sh release sq
```

3. Search with:

```bash
USE_SQ=1
USE_PAGE_SEARCH=1
```

Restrictions from current code:

- data type must be `float`
- classic beam search plus SQ is rejected
- SQ plus cache nodes is rejected

## Range Search

Set:

```bash
USE_PAGE_SEARCH=1
RS_ITER_KNN_TO_RANGE_SEARCH=1
RS_LS="80"
RADIUS=<threshold>
```

For custom range search:

```bash
RS_ITER_KNN_TO_RANGE_SEARCH=0
MEM_L=<positive value>
KICKED_SIZE=<optional>
RS_CUSTOM_ROUND=<optional>
```

Custom range search requires an in-memory navigation graph.

## Dynamic In-Memory Scenarios

Insertion/delete/consolidation:

```bash
build/tests/test_insert_deletes_consolidate \
  --data_type float \
  --dist_fn l2 \
  --data_path <base.bin> \
  --index_path_prefix <prefix> \
  -R 64 -L 300 --alpha 1.2 -T 8 \
  --points_to_skip 0 \
  --max_points_to_insert 7500 \
  --beginning_index_size 0 \
  --points_per_checkpoint 1000 \
  --checkpoints_per_snapshot 0 \
  --points_to_delete_from_beginning 2500 \
  --start_deletes_after 5000 \
  --do_concurrent true \
  --start_point_norm 3.2
```

Streaming scenario:

```bash
build/tests/test_streaming_scenario \
  --data_type float \
  --dist_fn l2 \
  --data_path <base.bin> \
  --index_path_prefix <prefix> \
  -R 64 -L 600 --alpha 1.2 \
  --insert_threads 4 \
  --consolidate_threads 4 \
  --max_points_to_insert 10000 \
  --active_window 4000 \
  --consolidate_interval 2000 \
  --start_point_norm 3.2
```

Search dynamic outputs with:

```bash
build/tests/search_memory_index \
  --data_type float \
  --dist_fn l2 \
  --index_path_prefix <dynamic_output_prefix> \
  --query_file <query.bin> \
  --gt_file <gt> \
  --recall_at 10 \
  --result_path <result_prefix> \
  -L 20 40 60 80 100 \
  --dynamic true \
  --tags true
```
