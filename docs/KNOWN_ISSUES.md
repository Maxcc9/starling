# Known Issues And Sharp Edges

## Current Checkout Cannot Configure Cleanly

Observed on 2026-05-29:

```bash
cmake -S . -B /tmp/starling-cmake-check -DCMAKE_BUILD_TYPE=Release
```

fails because:

- `graph_partition/` is empty and has no `CMakeLists.txt`.
- Top-level `cmake_minimum_required(VERSION 2.8)` is rejected by CMake 4.

Both submodules are uninitialized in this checkout:

```text
-fe85bbdf4cb891a67a8e2109c1c22a33aa958c7e gperftools
-ee8c04d2dbca4306636578f37992ee5c4c8f429f graph_partition
```

The leading `-` means the submodule working trees are missing.

## CMake Ordering

`project(diskann)` is currently before `cmake_minimum_required()`. CMake emits a
developer warning for this. Future CMake cleanup should move
`cmake_minimum_required()` before `project()`.

## Graph Partition Is A Hard Build Dependency

The top-level build calls `add_subdirectory(graph_partition)` unconditionally.
If the submodule is unavailable, even non-partition code cannot configure. A
future improvement would make graph partition optional or fail with a clearer
message.

## Script Uses Sudo During Search

`scripts/run_benchmark.sh search` runs:

```bash
sync; echo 3 | sudo tee /proc/sys/vm/drop_caches
```

This is useful for disk benchmark hygiene but breaks in many non-interactive
environments. Use direct CLI commands or patch the script when running in
sandboxed/CI environments without sudo.

## `unit_tester.sh` Appears Stale

`unit_tester.sh` uses old positional command forms for tools that currently use
Boost named options. Treat it as historical unless verified and updated.

## Memory Index Input Prefix Is Easy To Misuse

`build_memory_index --data_path` is a prefix, not a direct vector file. It
requires both:

- `<prefix>_data.bin`
- `<prefix>_ids.bin`

Passing a normal base `.bin` file directly will fail because the tool appends
suffixes.

## SQ Restrictions

Current code rejects:

- SQ with non-float data.
- SQ with classic beam search.
- SQ with cached nodes.

Use SQ only with float page search, and prefer an in-memory navigation graph over
cache nodes for that path.

## Range Search Mode Restrictions

Custom range search requires `mem_L > 0`. Iterative KNN-to-range mode is less
restrictive and can use page search or beam search.

## Metric Support Differs By Tool

In-memory search supports `l2`, `cosine`, and float-only `mips`/`fast_l2`.
Disk search code accepts `l2`, `mips`, and `cosine`, but inner product is
float-only. Existing workflow docs may lag behind current code.

## CI Workflow Inconsistencies

Some CI steps build an index with one metric prefix and then search another
prefix in the command text. Review `.github/workflows/pr-test.yml` before using
it as a source of exact command examples.

## External Dependencies Are Heavy

MKL and tcmalloc are part of the normal build. This makes local setup sensitive
to OS packaging and compiler/CMake versions.

