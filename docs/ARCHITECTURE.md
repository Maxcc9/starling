# Architecture

## Build System

The top-level CMake project is named `diskann`. It configures dependencies,
compiler flags, MKL/OpenMP/tcmalloc linkage, and then adds these subprojects:

- `graph_partition`: external submodule expected to build a `partitioner`
  executable.
- `src`: builds the `diskann` shared/static library from the core sources.
- `tests/utils`: builds dataset conversion, groundtruth, sampling, relayout, and
  other utility executables.
- `tests`: builds benchmark/search/scenario executables.

On Linux, the core library sources are explicitly listed in `src/CMakeLists.txt`:

- `index.cpp`: in-memory index build/search/dynamic behavior.
- `pq_flash_index.cpp`: disk index loading, cache, beam search, memory index
  integration.
- `page_search.cpp`: Starling page search implementations, including SQ.
- `range_search.cpp`: range search implementations.
- `visit_freq.cpp`: visit-frequency generation.
- `partition_and_pq.cpp`: disk index construction, PQ, sharding.
- IO, math, utility, logger, natural number containers, and exception support.

## In-Memory Index Flow

1. `tests/build_memory_index.cpp` receives a data prefix.
2. It expects:
   - `<data_prefix>_data.bin`
   - `<data_prefix>_ids.bin`
3. It reads tags from `_ids.bin`.
4. It builds `diskann::Index<T, uint32_t>` with tags enabled.
5. It saves the index at `--index_path_prefix`.

The benchmark scripts generate these sampled/frequency-derived memory-index
inputs with:

- `tests/utils/gen_random_slice`
- `tests/utils/parse_freq_file`

## Disk Index Build Flow

1. `tests/build_disk_index.cpp` parses data type, metric, graph degree, build
   complexity, search/build RAM budgets, disk PQ bytes, and reorder settings.
2. It dispatches to `diskann::build_disk_index<T>()`.
3. `src/partition_and_pq.cpp` handles graph construction, PQ compression, shard
   handling, and disk layout file emission.
4. Output files share the requested `--index_path_prefix`.

Common generated suffixes include:

- `_disk.index`
- `_pq_pivots.bin`
- `_pq_compressed.bin`
- `_sample_data.bin`
- additional metadata/reorder files depending on settings

## Disk Search Flow

1. `tests/search_disk_index.cpp` loads queries and optional groundtruth.
2. It constructs `PQFlashIndex<T>` with:
   - selected metric
   - `use_page_search`
   - optional SQ mode
3. It calls `PQFlashIndex::load()` with both the logical index prefix and the
   concrete `--disk_file_path`.
4. If `--mem_L > 0`, it loads the in-memory navigation graph from
   `--mem_index_path`.
5. If cache nodes are requested, it generates a cache list from sample queries
   and loads those nodes.
6. For each `-L` value:
   - page search calls `page_search()` or `page_search_sq()`
   - beam search calls `cached_beam_search()`
7. It writes result id and distance bins to `--result_path` with `_<L>` suffixes.

## Page Search

Page search is Starling's main disk-search path. It uses graph partition/layout
metadata to operate at page granularity. Search accepts:

- `mem_L`: number of candidates from the in-memory navigation graph.
- `l_search`: disk search list size.
- `beam_width`: max concurrent IOs per iteration.
- `io_limit`: optional maximum IO count.
- `use_ratio`: fraction of a page to evaluate.
- optional SQ mode for float data.

The code paths are in `src/page_search.cpp` and are exposed through
`PQFlashIndex<T>`.

## Beam Search

Beam search is the DiskANN-style baseline path implemented mainly in
`src/pq_flash_index.cpp`. It can use:

- cached nodes
- optional in-memory navigation graph seeding
- optional reorder data

SQ is explicitly rejected for beam search in `tests/search_disk_index.cpp`.

## Graph Partition And Relayout

The benchmark `gp` step expects an external `graph_partition/partitioner`
executable from the `graph_partition` submodule. It creates a partition file,
then `tests/utils/index_relayout` rewrites the disk index into partitioned page
layout.

The script then copies:

- partition output to `<index_prefix>_partition.bin`
- relayout output to `<index_prefix>_disk.index`
- original disk index to `<index_prefix>_disk_beam_search.index` when needed

## Frequency-Driven Flow

`tests/search_disk_index_save_freq.cpp` performs search and records visit counts.
`src/visit_freq.cpp` writes `<freq_save_path>_freq.bin`. The benchmark flow can
then:

- build a memory index from high-frequency nodes via `parse_freq_file`
- run graph partition using frequency information

## Range Search

`tests/range_search_disk_index.cpp` supports two modes:

- iterative KNN-to-range search
- custom range search

Custom range search requires `mem_L > 0` because it depends on the in-memory
navigation graph. Iterative range search can use page search or beam search.

## Dynamic Indexing

Dynamic behavior is implemented in `diskann::Index<T, TagT>`:

- insertion
- lazy delete
- consolidation
- optional concurrent update/consolidation
- tag-aware search

Dynamic search must be run with `--dynamic true --tags true`.
