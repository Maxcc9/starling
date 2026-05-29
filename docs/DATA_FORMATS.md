# Data Formats

## Vector Bin Format

Most tools use the DiskANN `.bin` vector format:

- first `uint32_t`: number of points `n`
- second `uint32_t`: dimension `d`
- payload: `n * d` values of the selected type

Supported vector element types:

- `float`
- `int8`
- `uint8`

Many internal loaders allocate aligned dimensions, but the file stores the
logical dimension.

## Truthset Format For KNN

KNN groundtruth files loaded by `load_truthset()` contain:

- first `uint32_t`: number of queries `n`
- second `uint32_t`: number of groundtruth entries per query `k`
- `n * k` ids as `uint32_t`
- `n * k` distances as `float`

Use `tests/utils/compute_groundtruth` to generate this format.

## Truthset Format For Range Search

Range search uses `load_range_truthset()`, where each query can have a variable
number of matching ids. Generate range truthsets with `tests/utils/gen_range`
or the existing range-search utilities rather than reusing KNN truthsets.

## Memory Navigation Index Input

`tests/build_memory_index.cpp` does not consume a single vector file directly.
It receives `--data_path` as a prefix and expects:

- `<data_path>_data.bin`: vector bin file
- `<data_path>_ids.bin`: tag/id bin file

The tag file is read after the same two-word bin header and interpreted as
`uint32_t` ids. The number of data rows and tag rows must match.

These prefix pairs are created by utilities such as:

- `tests/utils/gen_random_slice`
- `tests/utils/parse_freq_file`

## Disk Index Files

Disk index build writes several files sharing `--index_path_prefix`. Important
logical files include:

- `<prefix>_disk.index`: disk graph/layout file used by default search.
- `<prefix>_disk_beam_search.index`: benchmark script backup of the original
  beam-search disk layout before graph partition relayout.
- `<prefix>_pq_pivots.bin`: PQ pivots.
- `<prefix>_pq_compressed.bin`: compressed vectors for in-memory distance
  estimates.
- `<prefix>_sample_data.bin`: sample data used for warmup/cache generation.
- optional reorder/full-precision data when disk PQ and reorder are enabled.

`tests/search_disk_index.cpp` requires both:

- `--index_path_prefix`: logical prefix for sidecar files.
- `--disk_file_path`: concrete disk index file to load.

This split allows the benchmark script to choose either the original beam-search
layout or the relaid-out page-search layout.

## Partition File

Graph partition flow writes:

- `<index_prefix>_partition.bin`

`PQFlashIndex::load_partition_data()` uses this file with the disk index
metadata to map nodes to pages and page layout. The file is produced by the
external `graph_partition/partitioner` submodule plus
`tests/utils/index_relayout`.

## Visit Frequency File

Frequency generation writes:

- `<freq_save_path>_freq.bin`

It is produced by `tests/search_disk_index_save_freq.cpp` through
`PQFlashIndex::generate_node_nbrs_freq()`. It can drive:

- memory navigation graph construction via `tests/utils/parse_freq_file`
- graph partitioning via `graph_partition/partitioner --freq_file`

## Scalar Quantization Files

The benchmark `sq` step runs:

```bash
tests/utils/sq <index_prefix>
```

It transforms the disk index flow for SQ search. In this codebase:

- SQ search is supported only for `float` data.
- SQ is supported with page search.
- SQ is rejected for classic beam search.
- SQ cache nodes are rejected; use a memory navigation graph instead.

