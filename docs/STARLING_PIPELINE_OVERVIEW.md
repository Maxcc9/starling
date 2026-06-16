# Starling Pipeline Overview

Purpose: compact map for humans and LLM agents.

Read this file first. Use the detailed docs only when you need implementation depth:

- `docs/STARLING_INDEX_BUILD_FLOW.md`
- `docs/STARLING_SEARCH_FLOW.md`

## Core Idea

Starling has two major phases:

1. Build artifacts.
2. Search using those artifacts.

Search does not use a single "index file". It uses:

- logical prefix: sidecar metadata and compressed data
- physical disk layout file: actual page or beam layout

## Main Entry Points

- Build orchestrator: `scripts/run_benchmark.sh`
- Disk build CLI: `tests/build_disk_index.cpp`
- Memory-index build CLI: `tests/build_memory_index.cpp`
- KNN search CLI: `tests/search_disk_index.cpp`
- Range search CLI: `tests/range_search_disk_index.cpp`
- Disk search engine: `include/pq_flash_index.h`

## Pipeline Graph

```text
BASE DATA
  -> build
     -> _disk.index
     -> _pq_pivots.bin
     -> _pq_compressed.bin
     -> _sample_data.bin / _sample_ids.bin
     -> optional disk-PQ / reorder payload

  -> optional freq
     -> _freq.bin

  -> optional build_mem
     -> mem index

  -> optional gp
     -> _partition.bin
     -> relaid-out _disk.index
     -> preserved _disk_beam_search.index

SEARCH
  -> choose beam or page path
  -> optionally use mem index
  -> optionally use cache nodes
  -> optionally use SQ
```

## Build Stages

## `build`

Input:

- base vectors
- `R`
- `BUILD_L`
- `B`
- `M`
- optional `PQ_disk_bytes`
- optional `append_reorder_data`

Output:

- `<prefix>_disk.index`
- `<prefix>_pq_pivots.bin`
- `<prefix>_pq_compressed.bin`
- `<prefix>_sample_data.bin`
- `<prefix>_sample_ids.bin`
- optional disk-PQ sidecars

Notes:

- `B` controls in-memory PQ compression level for search.
- `M` controls whether graph build is one-shot or sharded.
- the temporary Vamana graph is not the final searchable disk artifact.

## `freq`

Input:

- built disk index
- queries

Output:

- `<freq_path>_freq.bin`

Uses:

- build frequency-based memory index
- bias graph partition

## `build_mem`

Input mode A:

- base data
- `MEM_RAND_SAMPLING_RATE`

Input mode B:

- base data
- `_freq.bin`
- `MEM_FREQ_USE_RATE`

Output:

- memory navigation index

Used only at search time as a seed structure.

## `gp`

Input:

- disk index
- optional `_freq.bin`

Output:

- `<prefix>_partition.bin`
- new `<prefix>_disk.index` for page search
- preserved `<prefix>_disk_beam_search.index` for beam search

## Search Stages

## KNN mode selection

If:

- `USE_PAGE_SEARCH=0`

then runtime path is:

- beam search
- disk file should be `_disk_beam_search.index` if available

If:

- `USE_PAGE_SEARCH=1`
- `USE_SQ=0`

then runtime path is:

- page search
- requires `_partition.bin`
- disk file is `_disk.index`

If:

- `USE_PAGE_SEARCH=1`
- `USE_SQ=1`

then runtime path is:

- page search with SQ
- requires float data
- requires SQ side metadata

## Range mode selection

If:

- `RS_ITER_KNN_TO_RANGE_SEARCH=1`

then runtime path is:

- iterative KNN-to-range
- can use page search or beam search underneath

If:

- `RS_ITER_KNN_TO_RANGE_SEARCH=0`

then runtime path is:

- custom Starling range search
- requires `MEM_L > 0`
- requires `USE_PAGE_SEARCH=1`
- requires `KICKED_SIZE > 0`

## Artifact To Search-Path Map

## Minimal beam-search artifacts

- `<prefix>_pq_pivots.bin`
- `<prefix>_pq_compressed.bin`
- disk layout file, usually `<prefix>_disk_beam_search.index`

## Minimal page-search artifacts

- `<prefix>_pq_pivots.bin`
- `<prefix>_pq_compressed.bin`
- `<prefix>_disk.index`
- `<prefix>_partition.bin`

## Additional artifacts for memory-seeded search

- memory index path, usually `${MEM_INDEX_PATH}_index`

## Additional artifacts for SQ page search

- SQ max/min table
- float data type

## Additional artifacts for reorder refinement

- disk index built with reorder payload
- `use_reorder_data=1` at search time

## Parameter Meaning Cheat Sheet

## Build

- `R`: graph max degree
- `BUILD_L`: disk-build search width
- `B`: search DRAM budget used to derive PQ chunks
- `M`: build RAM budget
- `BUILD_T`: disk-build threads
- `PQ_disk_bytes`: disk payload PQ bytes
- `append_reorder_data`: append full-precision payload for refinement

## Memory index build

- `MEM_R`: memory graph degree
- `MEM_BUILD_L`: memory graph build width
- `MEM_ALPHA`: memory graph density/stretch knob
- `MEM_USE_FREQ`: use `_freq.bin` instead of random sample
- `MEM_RAND_SAMPLING_RATE`: random sample ratio
- `MEM_FREQ_USE_RATE`: top-frequency ratio

## Frequency

- `FREQ_L`: search `L` for visit counting
- `FREQ_BM`: beamwidth for visit counting
- `FREQ_T`: threads for visit counting
- `FREQ_MEM_L`: use memory seeding during visit counting

## Graph partition

- `GP_TIMES`: LDG rounds
- `GP_T`: partition threads
- `GP_USE_FREQ`: use `_freq.bin`
- `GP_LOCK_NUMS`: lock initial nodes
- `GP_CUT`: cap degree seen by partitioner

## Search

- `MEM_L`: number of seeds from memory index
- `CACHE`: number of cached nodes
- `LS`: KNN search `L`
- `BM_LIST`: beamwidth list
- `USE_PAGE_SEARCH`: choose page vs beam path
- `PS_USE_RATIO`: fraction of a page to evaluate
- `USE_SQ`: enable SQ page search

## Hard Constraints

- page search needs `_partition.bin`
- SQ search needs `float`
- SQ search does not support beam-search path
- custom range search needs `MEM_L > 0`
- custom range search needs page search
- reorder refinement only makes sense if reorder payload was built
- inner product search only supports `float`

## Fast Decision Rules

Use beam search when:

- you want the DiskANN-like baseline
- you do not have or do not want partition-based relayout

Use page search when:

- you already ran `gp`
- you want Starling's page-aware IO path

Use memory seeding when:

- you built a memory navigation graph
- disk entry quality matters more than memory overhead

Use SQ when:

- data type is float
- page search is enabled

Use custom range search when:

- you intentionally want Starling's memory-seeded page-expansion method

## Common Runtime Combinations

## Baseline beam KNN

Build:

- `build`

Search:

- `USE_PAGE_SEARCH=0`
- `USE_SQ=0`
- optional `MEM_L=0`

Needs:

- `_disk_beam_search.index` or original disk layout

## Page KNN

Build:

- `build`
- `gp`

Search:

- `USE_PAGE_SEARCH=1`
- `USE_SQ=0`

Needs:

- `_disk.index`
- `_partition.bin`

## Page KNN with memory seeding

Build:

- `build`
- `build_mem`
- optional `freq`
- `gp`

Search:

- `USE_PAGE_SEARCH=1`
- `MEM_L > 0`

Needs:

- memory index
- page-search artifacts

## SQ page KNN

Build:

- `build`
- `sq`
- `gp`

Search:

- `USE_PAGE_SEARCH=1`
- `USE_SQ=1`

Needs:

- float data
- page-search artifacts
- SQ metadata

## Range search via iterative KNN

Build:

- `build`
- optional `build_mem`
- optional `gp`

Search:

- `RS_ITER_KNN_TO_RANGE_SEARCH=1`

Needs:

- beam or page artifacts depending on `USE_PAGE_SEARCH`

## Agent Reading Order

If you need only routing:

1. Read this file.

If you need build internals:

2. Read `docs/STARLING_INDEX_BUILD_FLOW.md`.

If you need runtime search internals:

3. Read `docs/STARLING_SEARCH_FLOW.md`.

If you need source-level truth:

4. Read the matching `tests/*.cpp` entrypoint, then `src/*.cpp`.
