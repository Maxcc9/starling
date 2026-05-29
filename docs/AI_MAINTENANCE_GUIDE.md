# AI Maintenance Guide

## Working Assumptions

- Treat this as a research codebase derived from DiskANN with Starling-specific
  disk page search additions.
- Preserve command-line compatibility unless the user explicitly asks to change
  it.
- Prefer narrow fixes over broad cleanup.
- Do not change benchmark semantics without documenting the measurement impact.

## Before Editing

1. Check `git status --short`.
2. Identify whether submodules are initialized.
3. Read the specific CLI or library path involved in the request.
4. Check whether the change affects both beam search and page search.
5. Check whether the change affects `float`, `int8`, and `uint8` template
   instantiations.

## High-Risk Areas

- `src/pq_flash_index.cpp`: shared disk index load/search state.
- `src/page_search.cpp`: page search logic, SQ logic, and IO/stat accounting.
- `src/range_search.cpp`: custom and iterative range search share stateful
  candidate handling.
- `src/partition_and_pq.cpp`: disk build output contracts.
- `include/pq_flash_index.h`: template API used by several CLI tools.
- `scripts/run_benchmark.sh`: file naming conventions and benchmark output
  aggregation.

## Common Change Checks

When modifying disk search:

- Test with `--use_page_search 0`.
- Test with `--use_page_search 1` after graph partition output exists.
- Test `--mem_L 0` and `--mem_L > 0` if memory graph code is touched.
- Check result id and distance output files.
- Check `QueryStats` accounting if latency/IO metrics are touched.

When modifying memory index:

- Verify build input prefix handling.
- Verify tags remain correct.
- For dynamic indices, search with `--dynamic true --tags true`.

When modifying data utilities:

- Confirm bin headers use two `uint32_t` values.
- Confirm row counts and dimensions are preserved.
- Avoid ad hoc binary parsing when existing utility functions can be reused.

When modifying scripts:

- Keep paths quoted where practical.
- Preserve generated directory/file naming because downstream steps depend on
  suffixes.
- Be careful with commands that remove files or require sudo.

## Verification Ladder

Use the smallest meaningful verification first:

1. `cmake -S . -B <tmp-build> -DCMAKE_BUILD_TYPE=Release`
2. `cmake --build <tmp-build> -j`
3. Direct CLI build/search on checked-in `tests_data` float data.
4. Direct CLI build/search on checked-in `tests_data` uint8 data.
5. `scripts/run_benchmark.sh release build` with CI config.
6. `scripts/run_benchmark.sh release gp` and page search.
7. Dynamic scenario tests if `Index<T, TagT>` was touched.

Current checkout cannot complete step 1 until submodules/CMake compatibility are
fixed.

## Documentation Rule

If a change affects any of these, update docs in this directory:

- command-line arguments
- generated file names
- data formats
- benchmark flow
- supported metrics/types
- known build requirements or blockers

