# Starling Index Build Flow

This document explains how Starling builds its index artifacts in the current repository checkout, based on the actual code paths in `scripts/`, `tests/`, `src/`, and `graph_partition/`.

## Overview

Starling's end-to-end indexing pipeline is not a single binary. It is a staged workflow driven by `scripts/run_benchmark.sh`:

1. Build the base disk index.
2. Optionally generate visit-frequency statistics from search traces.
3. Optionally build an in-memory navigation graph from sampled or frequent nodes.
4. Optionally graph-partition and relayout the disk index for page search.

The major command stages are:

- `build`: build the disk index.
- `build_mem`: build the in-memory navigation graph.
- `freq`: generate node/neighbor visit frequency.
- `gp`: graph partition plus relayout.
- `sq`: convert the disk index to the SQ path.

## End-To-End Flow

### Stage 0: Dataset Selection

`scripts/config_local.sh` sources `scripts/config_dataset.sh` and sets:

- `BASE_PATH`: base vector file in DiskANN `.bin` format.
- `QUERY_FILE`: query vector file.
- `GT_FILE`: groundtruth file.
- `DATA_TYPE`: `float`, `int8`, or `uint8`.
- `DIST_FN`: usually `l2` or `mips`.
- `B`: search DRAM budget used to derive PQ compression.
- `K`: search recall target used by search/freq stages.

The dataset file format is:

- first `uint32_t`: number of points
- second `uint32_t`: vector dimension
- remaining payload: `n * d` elements

## Stage 1: Disk Index Build (`build`)

### Entry points

- shell: `scripts/run_benchmark.sh`
- CLI: `tests/build_disk_index.cpp`
- core implementation: `src/aux_utils.cpp::build_disk_index<T>()`

### Command shape

The benchmark script calls:

```bash
tests/build_disk_index \
  --data_type $DATA_TYPE \
  --dist_fn $DIST_FN \
  --data_path $BASE_PATH \
  --index_path_prefix $INDEX_PREFIX_PATH \
  -R $R \
  -L $BUILD_L \
  -B $B \
  -M $M \
  -T $BUILD_T
```

### What actually happens

#### 1. Parse and normalize build settings

`build_disk_index.cpp` passes a compact parameter string into `diskann::build_disk_index<T>()`:

- `R`
- `L`
- `B`
- `M`
- `T`
- optional `PQ_disk_bytes`
- optional `append_reorder_data`

#### 2. Handle MIPS preprocessing if needed

If `DIST_FN=mips`, Starling rewrites the base data into a temporary "prepped base" file using the inner-product to L2 reduction. The rest of the disk-build path then uses the transformed file.

Implication:

- the disk build path for MIPS is intentionally implemented through an L2-style graph build over transformed vectors.

#### 3. Derive the in-memory PQ compression level from `B`

`B` is not the build RAM budget. It is used to decide how aggressively vectors are PQ-compressed for search-time memory use.

The code computes:

```text
num_pq_chunks = floor(final_index_ram_limit / points_num)
```

and then clamps it into:

- at least `1`
- at most `dim`
- at most `MAX_PQ_CHUNKS`

This becomes the number of PQ bytes per vector for the main in-memory compressed representation stored in:

- `<prefix>_pq_pivots.bin`
- `<prefix>_pq_compressed.bin`

#### 4. Sample training data for PQ

Starling draws a random sample from the base file using `gen_random_slice()` to produce PQ training data.

The sampling rate is:

```text
MAX_PQ_TRAINING_SET_SIZE / points_num
```

bounded effectively by the dataset size.

#### 5. Train and write the main PQ model

Starling trains PQ pivots with:

- 256 centers
- `num_pq_chunks` chunks
- `NUM_KMEANS_REPS` repetitions

Then it encodes the base dataset into PQ codes.

#### 6. Optionally build a second PQ representation for disk payload

If `PQ_disk_bytes > 0`, Starling additionally trains another PQ model used for the vectors stored inside the disk index pages themselves:

- `<prefix>_disk.index_pq_pivots.bin`
- `<prefix>_disk.index_pq_compressed.bin`

This path is intended for higher-dimensional or more space-constrained disk payloads.

#### 7. Build the Vamana graph

Starling next builds a temporary memory graph at:

- `<prefix>_mem.index`

This is done through `build_merged_vamana_index<T>()`.

There are two cases:

- If the full graph fits inside build RAM budget `M`, it builds the graph in one shot.
- If not, it partitions the dataset into shards, builds per-shard graphs, and merges them.

In the "fit in RAM" path, the build parameters are:

- `L = BUILD_L`
- `R = R`
- `C = 750`
- `alpha = 1.2`
- `num_rnds = 2`
- `saturate_graph = 1`

In the sharded path:

- partitioning uses `partition_with_ram_budget()`
- each shard is built with degree about `2 * floor(R / 3)`
- shard graphs are later merged back to target degree `R`

#### 8. Materialize the disk index layout

Starling converts the graph plus vector payload into:

- `<prefix>_disk.index`

using `create_disk_layout()`.

There are three payload modes:

1. No disk PQ:
   - full vectors are stored in the disk index pages.
2. Disk PQ without reorder data:
   - PQ-compressed vectors are stored in disk pages.
3. Disk PQ with reorder data:
   - PQ-compressed vectors are stored in disk pages
   - full-precision float vectors are appended in dedicated reorder sectors

The reorder path is only allowed for:

- `data_type=float`
- `PQ_disk_bytes > 0`

#### 9. Generate warmup/cache samples

Starling writes:

- `<prefix>_sample_data.bin`
- `<prefix>_sample_ids.bin`

These are sampled from the base and later used for:

- cache-node generation
- search warmup

#### 10. Cleanup

The temporary Vamana file `<prefix>_mem.index` is removed at the end of disk build. If disk-PQ payload files were only needed for constructing `_disk.index`, the temporary compressed payload file is also removed.

### Main outputs of `build`

- `<prefix>_disk.index`
- `<prefix>_pq_pivots.bin`
- `<prefix>_pq_compressed.bin`
- `<prefix>_sample_data.bin`
- `<prefix>_sample_ids.bin`
- optionally `<prefix>_disk.index_pq_pivots.bin`
- optionally reorder payload embedded in `<prefix>_disk.index`

The benchmark script also copies:

- `<prefix>_disk.index` to `<prefix>_disk_beam_search.index`

so that later `gp` can relayout the page-search version without losing the original beam-search layout.

## Stage 2: Frequency Generation (`freq`)

### Entry points

- shell: `scripts/run_benchmark.sh`
- CLI: `tests/search_disk_index_save_freq.cpp`
- core implementation: `src/visit_freq.cpp`

### Purpose

This stage runs search over the disk index and records node-neighbor visitation counts.

It writes:

- `<freq_path>_freq.bin`

### What it is used for

- building a memory navigation graph from the most frequently visited nodes
- biasing graph partition using search-frequency information

### Important behavior

The frequency path always calls `generate_node_nbrs_freq()` and records neighbor visitation maps. It can optionally seed search from an in-memory graph if `FREQ_MEM_L > 0`.

## Stage 3: In-Memory Navigation Graph Build (`build_mem`)

### Entry points

- shell: `scripts/run_benchmark.sh`
- random sample generator: `tests/utils/gen_random_slice.cpp`
- frequency parser: `tests/utils/parse_freq_file.cpp`
- memory graph builder: `tests/build_memory_index.cpp`

### Two input modes

#### Mode A: Random sampling

Used when:

```bash
MEM_USE_FREQ=0
```

The script generates:

- `<sample_prefix>_data.bin`
- `<sample_prefix>_ids.bin`

by sampling each base point independently with probability `MEM_RAND_SAMPLING_RATE`.

#### Mode B: Frequency-based selection

Used when:

```bash
MEM_USE_FREQ=1
```

The script reads `<freq_path>_freq.bin`, sorts points by descending frequency, and selects the top:

```text
nums * MEM_FREQ_USE_RATE
```

points into:

- `<freq_prefix>_data.bin`
- `<freq_prefix>_ids.bin`

### Build step

`tests/build_memory_index.cpp` expects the `--data_path` argument to be a prefix, not a single file. It reads:

- `<data_path>_data.bin`
- `<data_path>_ids.bin`

and builds a tagged `diskann::Index<T, uint32_t>`.

The build parameters are:

- `R = MEM_R`
- `L = MEM_BUILD_L`
- `C = 750`
- `alpha = MEM_ALPHA`
- `saturate_graph = 0`
- `num_threads = default or CLI`

### Main output of `build_mem`

- `<mem_index_path>_index`

This memory graph is later used only as a search-time seeding structure. It is not required for building the disk index itself.

## Stage 4: Graph Partition And Relayout (`gp`)

### Entry points

- shell: `scripts/run_benchmark.sh`
- graph partitioner: `graph_partition/partitioner`
- relayout tool: `tests/utils/index_relayout.cpp`

### Purpose

This stage rearranges the physical node layout in the disk index so page search reads pages that are graph-aware rather than original DiskANN packing.

### Inputs

- original disk index, usually `<prefix>_disk_beam_search.index`
- optional frequency file `<freq_path>_freq.bin`

### Partitioner parameters

The script passes:

- `--index_file`
- `--data_type`
- `--gp_file`
- `-T GP_T`
- `--ldg_times GP_TIMES`
- optionally `--freq_file`
- optionally `--lock_nums GP_LOCK_NUMS`
- optionally `--cut GP_CUT`

The partitioner groups node records into page-sized blocks. `block_size=1` means one 4KB page unless changed directly at the CLI.

### Relayout result

`index_relayout` writes a relaid-out temporary file from the partition assignment and then the script copies:

- `<gp_path>_part_tmp.index` to `<prefix>_disk.index`
- `<gp_path>_part.bin` to `<prefix>_partition.bin`

After this stage:

- `<prefix>_disk.index` is the page-search layout
- `<prefix>_disk_beam_search.index` remains the original beam-search layout

## Stage 5: SQ Conversion (`sq`)

### Entry points

- shell: `scripts/run_benchmark.sh`
- tool: `tests/utils/sq`

### Purpose

This adjusts the disk index for the scalar-quantized page-search path.

### Limitations

- supported only for `float`
- supported with page search
- rejected for beam search
- incompatible with cache-node loading

## Parameter Reference

## Core disk-build parameters

- `DATA_TYPE`
  - Base vector scalar type.
  - Allowed: `float`, `int8`, `uint8`.
- `DIST_FN`
  - Distance mode for build/search.
  - Disk build accepts `l2` and `mips`.
  - Memory build additionally accepts `cosine`.
- `R`
  - Maximum graph degree for the disk build.
  - Larger values usually improve quality and increase build/index cost.
- `BUILD_L`
  - Build-time graph exploration width.
  - Larger values generally improve graph quality and build time.
- `B`
  - Search DRAM budget in GB.
  - In practice, used to derive the number of PQ chunks for compressed vectors.
  - It is not the disk-build RAM budget.
- `M`
  - Disk-build RAM budget in GB.
  - Controls whether the Vamana graph is built in one shot or by sharding and merge.
- `BUILD_T`
  - Number of threads used in disk build.
- `PQ_disk_bytes`
  - Optional alternate PQ byte count for vectors stored inside the disk pages.
  - `0` disables disk-payload PQ.
- `append_reorder_data`
  - Appends full-precision float vectors to the disk index when disk-PQ is enabled.
  - Intended for reordering after approximate candidates are produced.

## Memory navigation graph parameters

- `MEM_R`
  - Max degree for the memory navigation graph.
- `MEM_BUILD_L`
  - Build-time list size for the memory graph.
- `MEM_ALPHA`
  - Graph density/stretch knob for memory graph build.
  - `1.0` is sparser; `1.2` or `1.4` is denser.
- `MEM_RAND_SAMPLING_RATE`
  - Bernoulli sampling probability for random memory-graph seeds.
- `MEM_USE_FREQ`
  - `0`: use random sampling.
  - `1`: use top-frequency nodes from `_freq.bin`.
- `MEM_FREQ_USE_RATE`
  - Fraction of highest-frequency nodes selected when `MEM_USE_FREQ=1`.

## Frequency generation parameters

- `FREQ_QUERY_FILE`
  - Query file used when generating visitation frequency.
- `FREQ_QUERY_CNT`
  - Number of queries to use.
  - `0` means use all queries.
- `FREQ_BM`
  - Beamwidth used during the frequency run.
- `FREQ_L`
  - Search list size used during the frequency run.
  - Current flow expects one value.
- `FREQ_T`
  - Thread count for frequency generation.
- `FREQ_CACHE`
  - Number of cache nodes loaded during the frequency run.
- `FREQ_MEM_L`
  - If non-zero, seed disk search from the memory navigation graph during frequency generation.

## Graph partition parameters

- `GP_TIMES`
  - Number of LDG partition passes.
- `GP_T`
  - Thread count for graph partition.
- `GP_LOCK_NUMS`
  - Number of initially locked nodes that do not participate in later LDG placement.
- `GP_USE_FREQ`
  - `0`: partition without frequency guidance.
  - `1`: also pass the `_freq.bin` file into the partitioner.
- `GP_CUT`
  - Maximum adjacency length exposed to the partitioner.
  - Can be used to cap effective graph degree for partitioning.

## Search-related parameters that affect frequency or downstream usage

- `MEM_L`
  - Number of candidates obtained from the in-memory navigation graph before disk search continues.
  - Search-time parameter only.
- `USE_PAGE_SEARCH`
  - `1`: use Starling page search.
  - `0`: use DiskANN-style beam search.
- `PS_USE_RATIO`
  - Fraction of nodes inside a page evaluated during page search.
- `CACHE`
  - Number of cached nodes loaded into memory for search.
- `LS`
  - Search-time `L` values.
- `BM_LIST`
  - Search-time beamwidth list.

## Parameters that appear in config but are not clearly wired into the active build path

- `FREQ_MEM_TOPK`
  - Present in config naming, but not used by the core frequency generation path.
- `MEM_TOPK`
  - Appears in log-file naming only.
  - Not passed into the search binary by `run_benchmark.sh`.

## Artifact Map

Given:

- `INDEX_DIR=${STARLING_INDEX_ROOT}/${INDEX_EXPERIMENT}/${INDEX_NAME}`
- `INDEX_PREFIX_PATH=${INDEX_DIR}/${INDEX_NAME}`

the main artifacts are:

- `${INDEX_PREFIX_PATH}_disk.index`
- `${INDEX_PREFIX_PATH}_disk_beam_search.index`
- `${INDEX_PREFIX_PATH}_pq_pivots.bin`
- `${INDEX_PREFIX_PATH}_pq_compressed.bin`
- `${INDEX_PREFIX_PATH}_sample_data.bin`
- `${INDEX_PREFIX_PATH}_sample_ids.bin`
- `${INDEX_PREFIX_PATH}_partition.bin`
- `${MEM_INDEX_PATH}_index`
- `${FREQ_PATH}_freq.bin`

Subdirectories created by the script include:

- `samples/`
- `memory/`
- `freq/`
- `gp/`
- `search/`
- `result/`

## Practical Interpretation

If you only care about "what do I need for a usable Starling page-search index?", the practical minimum is:

1. Run `build` to create the base disk index.
2. Optionally run `freq`.
3. Optionally run `build_mem` if you want a memory navigation graph.
4. Run `gp` to create the page-aware relayout and partition metadata.

At that point:

- page search uses `<prefix>_disk.index` plus `<prefix>_partition.bin`
- beam search uses `<prefix>_disk_beam_search.index`
- memory-seeded search additionally uses `<mem_index_path>_index`

## Source Pointers

- `scripts/run_benchmark.sh`
- `tests/build_disk_index.cpp`
- `tests/build_memory_index.cpp`
- `tests/search_disk_index_save_freq.cpp`
- `tests/utils/gen_random_slice.cpp`
- `tests/utils/parse_freq_file.cpp`
- `tests/utils/index_relayout.cpp`
- `src/aux_utils.cpp`
- `src/visit_freq.cpp`
- `graph_partition/src/partitioner.cpp`
