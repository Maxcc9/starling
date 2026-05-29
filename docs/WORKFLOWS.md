# Workflows

## End-To-End KNN Experiment

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

