# Build, Test, And Run

## Dependencies

Linux builds expect:

- CMake
- GCC/G++
- OpenMP
- Boost program_options
- libaio
- gperftools/tcmalloc
- Intel MKL, preferably `libmkl-full-dev` on Ubuntu

The README suggests:

```bash
apt install build-essential libboost-all-dev make cmake g++ libaio-dev libgoogle-perftools-dev clang-format libboost-all-dev libmkl-full-dev
```

The GitHub Actions configs use:

```bash
sudo apt install cmake g++ libaio-dev libgoogle-perftools-dev clang-format libboost-dev libboost-program-options-dev libmkl-full-dev
```

## Submodules

This repository has two submodules:

```bash
git submodule update --init --recursive
```

Required submodule paths:

- `gperftools`: needed by Windows build logic and vendored tcmalloc workflows.
- `graph_partition`: required because top-level CMake calls
  `add_subdirectory(graph_partition)`.

## Current Local Configure Result

Command run from this checkout:

```bash
cmake -S . -B /tmp/starling-cmake-check -DCMAKE_BUILD_TYPE=Release
```

Result: failed.

Observed blockers:

- `graph_partition` is empty and does not contain `CMakeLists.txt`.
- CMake 4 rejects the top-level `cmake_minimum_required(VERSION 2.8)` policy
  compatibility. The error suggests either raising the minimum version or using
  `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.

There is also a developer warning because `project(diskann)` appears before
`cmake_minimum_required()`.

## Normal Build Commands

After submodules and CMake compatibility are fixed:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Debug:

```bash
cmake -S . -B debug -DCMAKE_BUILD_TYPE=Debug
cmake --build debug -j
```

The benchmark script performs equivalent configure/build steps into `../debug`
or `../release` from inside `scripts/`.

## Benchmark Script

Setup:

```bash
cd scripts
cp config_sample.sh config_local.sh
```

Edit dataset paths in `config_dataset.sh` or define/select a dataset function in
`config_local.sh`.

Usage:

```bash
./run_benchmark.sh [debug/release] [build/build_mem/freq/gp/search/sq] [knn/range]
```

Generated benchmark index artifacts are written under
`/mnt/diskann_data/starling_data/index` by default. See `INDEX_STORAGE.md` for the required
layout and naming convention.

Search outputs are separated from index artifacts. Search logs, result files,
CSV reports, and figures are written under this repository's `reports/`
directory by default. Run direct CLI examples from the repository root when
using relative `reports/...` paths.

The main tunable script variables are documented in `WORKFLOWS.md`:

- disk index build: `R`, `BUILD_L`, `B`, `M`, `BUILD_T`,
  `DISK_PQ_BYTES`, `APPEND_REORDER_DATA`
- KNN search: `LS`, `BM_LIST`, `T_LIST`, `K`, `CACHE`, `MEM_L`
- page search and relayout: `USE_PAGE_SEARCH`, `PS_USE_RATIO`, `GP_TIMES`,
  `GP_T`, `GP_USE_FREQ`, `GP_LOCK_NUMS`, `GP_CUT`
- memory graph and frequency workflows: `MEM_*`, `FREQ_*`

Examples:

```bash
cd /home/gt/research/starling/scripts
./run_benchmark.sh release build
./run_benchmark.sh release build_mem
./run_benchmark.sh release freq
./run_benchmark.sh release gp
./run_benchmark.sh release search knn
./run_benchmark.sh release search range
```

Note: `run_benchmark.sh search` drops the Linux page cache with `sudo tee
/proc/sys/vm/drop_caches`. That step requires privileges and is not appropriate
for unprivileged CI or sandboxed runs without adjustment.

## Small Checked-In Test Data

Available files:

- `tests_data/rand_float_10D_10K_norm1.0.bin`
- `tests_data/l2_rand_float_10D_10K_norm1.0_self_gt10`
- `tests_data/rand_uint8_10D_10K_norm50.0.bin`
- `tests_data/l2_rand_uint8_10D_10K_norm50.0_self_gt10`

The CI benchmark config `scripts/config_ci.sh` uses these files.

## Representative Direct CLI Commands

Build disk index:

```bash
mkdir -p /mnt/diskann_data/starling_data/index/manual/starling_float_R16_L32_B0.00003_M1

build/tests/build_disk_index \
  --data_type float \
  --dist_fn l2 \
  --data_path tests_data/rand_float_10D_10K_norm1.0.bin \
  --index_path_prefix /mnt/diskann_data/starling_data/index/manual/starling_float_R16_L32_B0.00003_M1/starling_float_R16_L32_B0.00003_M1 \
  -R 16 -L 32 -B 0.00003 -M 1 -T 16
```

High-dimensional float data can require disk-PQ so that each node fits the
4KB disk-index sector layout:

```bash
release/tests/build_disk_index \
  --data_type float \
  --dist_fn l2 \
  --data_path /home/gt/research/DiskANN/data/gist1m/gist1m_base.bin \
  --index_path_prefix /mnt/diskann_data/starling_data/index/manual/gist1m_R64_L100_B8_M8_DPQ256/gist1m_R64_L100_B8_M8_DPQ256 \
  -R 64 -L 100 -B 8 -M 8 -T 16 \
  --PQ_disk_bytes 256
```

Note that this build CLI does not have a parameter named `QD`. The `-B`
argument controls query-time PQ compression indirectly through the search DRAM
budget, while `--PQ_disk_bytes` controls optional disk-resident vector
compression. See `WORKFLOWS.md` for the exact distinction and formula.

Search disk index:

```bash
cd /home/gt/research/starling

build/tests/search_disk_index \
  --data_type float \
  --dist_fn l2 \
  --index_path_prefix /mnt/diskann_data/starling_data/index/manual/starling_float_R16_L32_B0.00003_M1/starling_float_R16_L32_B0.00003_M1 \
  --disk_file_path /mnt/diskann_data/starling_data/index/manual/starling_float_R16_L32_B0.00003_M1/starling_float_R16_L32_B0.00003_M1_disk.index \
  --query_file tests_data/rand_float_10D_10K_norm1.0.bin \
  --gt_file tests_data/l2_rand_float_10D_10K_norm1.0_self_gt10 \
  --recall_at 5 \
  -L 10 12 14 16 \
  -W 2 \
  --num_nodes_to_cache 0 \
  -T 16 \
  --result_path reports/manual/starling_float_R16_L32_B0.00003_M1/result/result
```

Page search uses the same executable, with a page-relayout disk file and
partition file already generated by the `gp` workflow:

```bash
release/tests/search_disk_index \
  --data_type float \
  --dist_fn l2 \
  --index_path_prefix /mnt/diskann_data/starling_data/index/sift1m_starling/sift1m_R64_L100_B2_M2/sift1m_R64_L100_B2_M2 \
  --disk_file_path /mnt/diskann_data/starling_data/index/sift1m_starling/sift1m_R64_L100_B2_M2/sift1m_R64_L100_B2_M2_disk.index \
  --query_file /home/gt/research/DiskANN/data/sift1m/sift1m_query.bin \
  --gt_file /home/gt/research/DiskANN/data/sift1m/sift1m_groundtruth.bin \
  --recall_at 10 \
  -L 50 100 150 200 \
  -W 4 \
  --num_nodes_to_cache 0 \
  -T 16 \
  --use_page_search 1 \
  --use_ratio 1.0 \
  --result_path reports/sift1m_starling/sift1m_R64_L100_B2_M2/result/result
```

Build memory index from generated sampled data prefix:

```bash
build/tests/build_memory_index \
  --data_type float \
  --dist_fn l2 \
  --data_path /mnt/diskann_data/starling_data/index/manual/starling_float_R16_L32_B0.00003_M1/samples/sample_rate_0.01/sample_rate_0.01 \
  --index_path_prefix /mnt/diskann_data/starling_data/index/manual/starling_float_R16_L32_B0.00003_M1/memory/mem_R16_L32_A1.2/mem_R16_L32_A1.2_index \
  -R 16 -L 32 --alpha 1.2 -T 16
```

`build_memory_index` expects the selected data prefix to have matching
`_data.bin` and `_ids.bin` files.

## Useful Validation Targets

Once the build is healthy:

- Configure with a clean build directory.
- Build all targets.
- Run direct build/search on checked-in float data.
- Run direct build/search on checked-in uint8 data.
- Run `scripts/run_benchmark.sh release build` with `scripts/config_ci.sh`.
- Run `scripts/run_benchmark.sh release search knn` for both beam and page
  search after graph partition output exists.
