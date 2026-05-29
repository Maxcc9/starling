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

Examples:

```bash
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
build/tests/build_disk_index \
  --data_type float \
  --dist_fn l2 \
  --data_path tests_data/rand_float_10D_10K_norm1.0.bin \
  --index_path_prefix /tmp/starling_float \
  -R 16 -L 32 -B 0.00003 -M 1 -T 16
```

Search disk index:

```bash
build/tests/search_disk_index \
  --data_type float \
  --dist_fn l2 \
  --index_path_prefix /tmp/starling_float \
  --disk_file_path /tmp/starling_float_disk.index \
  --query_file tests_data/rand_float_10D_10K_norm1.0.bin \
  --gt_file tests_data/l2_rand_float_10D_10K_norm1.0_self_gt10 \
  --recall_at 5 \
  -L 10 12 14 16 \
  -W 2 \
  --num_nodes_to_cache 0 \
  -T 16 \
  --result_path /tmp/starling_res
```

Build memory index from generated sampled data prefix:

```bash
build/tests/build_memory_index \
  --data_type float \
  --dist_fn l2 \
  --data_path /tmp/mem_sample_prefix \
  --index_path_prefix /tmp/mem_index \
  -R 16 -L 32 --alpha 1.2 -T 16
```

`build_memory_index` expects `/tmp/mem_sample_prefix_data.bin` and
`/tmp/mem_sample_prefix_ids.bin`.

## Useful Validation Targets

Once the build is healthy:

- Configure with a clean build directory.
- Build all targets.
- Run direct build/search on checked-in float data.
- Run direct build/search on checked-in uint8 data.
- Run `scripts/run_benchmark.sh release build` with `scripts/config_ci.sh`.
- Run `scripts/run_benchmark.sh release search knn` for both beam and page
  search after graph partition output exists.

