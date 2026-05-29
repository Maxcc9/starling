# Project Context

## Purpose

Starling is a C++ implementation of the paper "Starling: An I/O-Efficient
Disk-Resident Graph Index Framework for High-Dimensional Vector Similarity
Search on Data Segment." It builds on DiskANN-style graph indices and adds
experiments around page-level disk layout, graph partitioning, in-memory
navigation graphs, visit-frequency driven sampling, and scalar quantization.

The repository supports:

- Building in-memory graph indices.
- Building disk-resident graph indices.
- Searching disk indices with classic beam search.
- Searching disk indices with Starling page search.
- Optional in-memory navigation graph seeding during disk search.
- Optional cache node loading.
- Optional graph partition based disk relayout.
- Range search variants.
- Dynamic/incremental in-memory index scenarios.

## Top-Level Layout

- `include/`: public and internal headers for the core library.
- `src/`: core library implementation.
- `tests/`: command-line tools and scenario tests.
- `tests/utils/`: conversion, groundtruth, partition, relayout, SQ, and sampling
  utilities.
- `scripts/`: benchmark orchestration and dataset configuration.
- `workflows/`: existing user-facing usage notes inherited from DiskANN and
  extended for this repository.
- `tests_data/`: small checked-in float and uint8 datasets with L2 truthsets.
- `.github/workflows/`: CI build and benchmark pipelines.
- `gperftools/`, `graph_partition/`: git submodule paths.

## Key Entry Points

- `tests/build_memory_index.cpp`: builds tagged in-memory navigation indices.
- `tests/search_memory_index.cpp`: searches in-memory indices and dynamic index
  snapshots.
- `tests/build_disk_index.cpp`: builds disk-resident indices.
- `tests/search_disk_index.cpp`: searches disk indices with beam search or page
  search.
- `tests/search_disk_index_save_freq.cpp`: runs disk search and writes visit
  frequency data.
- `tests/range_search_disk_index.cpp`: range search over disk indices.
- `tests/test_insert_deletes_consolidate.cpp`: dynamic insert/delete scenario.
- `tests/test_streaming_scenario.cpp`: sliding-window dynamic index scenario.
- `scripts/run_benchmark.sh`: end-to-end experiment orchestration.

## Main C++ Library Types

- `diskann::Index<T, TagT>` in `include/index.h`: in-memory graph index with
  build, search, tags, dynamic insertion, lazy delete, and consolidation.
- `diskann::PQFlashIndex<T>` in `include/pq_flash_index.h`: disk-resident search
  index. It owns disk layout metadata, PQ data, optional memory index, cache
  state, and page/beam/range search methods.
- `diskann::Parameters` in `include/parameters.h`: typed parameter bag used by
  build and dynamic operations.
- `AlignedFileReader` implementations: platform-specific aligned and async IO
  abstraction for disk search.

## Current Checkout State

This checkout has uninitialized submodules:

- `gperftools`
- `graph_partition`

`git submodule status` reports both with a leading `-`, and the directories are
empty. As a result, a clean CMake configure currently fails before compilation.
See `BUILD_TEST_RUN.md` and `KNOWN_ISSUES.md`.

