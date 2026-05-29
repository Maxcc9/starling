# Dataset Benchmarks

This project can reuse the prepared datasets under:

```bash
/home/gt/research/DiskANN/data
```

The Starling benchmark scripts default to that path through:

```bash
DISKANN_DATA_ROOT=/home/gt/research/DiskANN/data
```

Generated index artifacts are written under:

```bash
/mnt/diskann_data/starling_data/index
```

Before running 100M-scale datasets, verify that `/mnt/diskann_data/starling_data` is backed by
large storage. A normal root partition is usually not enough for all generated
disk indices, partitioned layouts, frequency files, memory graph samples, logs,
and repeated search results.

## Dataset Matrix

| Dataset | Base file | Type | Dim | Dist | Default profile |
| --- | --- | --- | ---: | --- | --- |
| `sift1m` | `sift1m/sift1m_base.bin` | `float` | 128 | `l2` | `starling_profile_1m` |
| `deep1m` | `deep1m/deep1m_base.bin` | `float` | 96 | `l2` | `starling_profile_1m` |
| `gist1m` | `gist1m/gist1m_base.bin` | `float` | 960 | `l2` | `starling_profile_gist1m` |
| `text2image1m` | `text2image1m/text2image1m_base.bin` | `float` | 200 | `mips` | `starling_profile_1m` |
| `sift100m` | `sift100m/sift100m_base.bin` | `uint8` | 128 | `l2` | `starling_profile_100m` |
| `deep100m` | `deep100m/deep100m_base.bin` | `float` | 96 | `l2` | `starling_profile_100m` |
| `spacev100m` | `spacev100m/spacev100m_base.bin` | `uint8` | 100 | `l2` | `starling_profile_100m` |

`sift1m` in the local DiskANN data directory is stored as `float`, while
`sift100m` is stored as `uint8`.

## Dataset Configs

Dataset functions are defined in `scripts/config_dataset.sh`:

```bash
dataset_sift1m
dataset_deep1m
dataset_gist1m
dataset_text2image1m
dataset_sift100m
dataset_deep100m
dataset_spacev100m
```

Each function sets:

- `BASE_PATH`
- `QUERY_FILE`
- `GT_FILE`
- `PREFIX`
- `DATA_TYPE`
- `DIST_FN`
- `B`
- `K`
- `DATA_DIM`
- `DATA_N`

Profile functions set build/search defaults:

```bash
starling_profile_1m
starling_profile_gist1m
starling_profile_100m
apply_starling_dataset_profile
```

## Suite Runner

Use `scripts/run_starling_suite.sh` for dataset-level runs:

```bash
cd /home/gt/research/starling/scripts
./run_starling_suite.sh <dataset|all> <phase> [release|debug]
```

Datasets:

```text
sift1m deep1m gist1m text2image1m sift100m deep100m spacev100m all
```

Phases:

```text
build       Build disk index only.
gp          Run graph partition and relayout.
beam        Run beam-search KNN.
page        Run page-search KNN.
freq        Generate visit-frequency file.
build_mem   Build in-memory navigation graph.
full_beam   build -> beam.
full_page   build -> gp -> page.
full        build -> freq -> build_mem -> gp -> page.
```

Examples:

```bash
./run_starling_suite.sh sift1m full_page release
./run_starling_suite.sh deep1m build release
./run_starling_suite.sh all build release
```

The runner generates a temporary config at:

```bash
/tmp/starling_<dataset>_config.sh
```

and passes it to `run_benchmark.sh` with `STARLING_CONFIG`.

## Best-Performance Sweep Excluding `sift100m`

Use `scripts/run_starling_best_except_sift100m.sh` when the goal is to compare
Starling's best observed search performance across:

```text
sift1m deep1m gist1m text2image1m deep100m spacev100m
```

`sift100m` is intentionally excluded.

```bash
cd /home/gt/research/starling/scripts
./run_starling_best_except_sift100m.sh plan
```

Modes:

```text
plan      Print the planned datasets and sweep ranges.
prepare   Build disk index, frequency file, random/frequency memory graph, and graph partition.
search    Run search sweeps. Requires prepare to have completed.
full      prepare -> search.
```

Recommended execution:

```bash
./run_starling_best_except_sift100m.sh prepare
./run_starling_best_except_sift100m.sh search
```

The sweep compares:

- beam search baseline
- page search
- page search with `PS_USE_RATIO` variants
- page search with cache for 100M datasets
- page search with in-memory navigation graph
- random and frequency-based memory navigation graph (`MEM_USE_FREQ=0/1`)

The script uses `INDEX_EXPERIMENT=${PREFIX}_best`, so output paths are separate
from quick/default runs. Example:

```bash
/mnt/diskann_data/starling_data/index/sift1m_best/sift1m_R64_L100_B2_M2/
```

For 1M datasets, the default sweep includes:

```text
BM_LIST: 2, 4, 8
T_LIST: 8, 16
PS_USE_RATIO: 1.0, 0.75, 0.5
MEM_L: 10, 50
MEM_USE_FREQ: 0, 1
```

For `deep100m` and `spacev100m`, the sweep is narrower:

```text
BM_LIST: 4, 8
T_LIST: 8, 16, 32
PS_USE_RATIO: 1.0, 0.75
CACHE: 0, 100000
MEM_L: 10
MEM_USE_FREQ: 0, 1
```

The 100M runs can take a long time and may generate large index artifacts. Run
`prepare` and `search` separately so failed or interrupted runs can resume from
existing artifacts.

## Recommended Order

For initial validation:

```bash
./run_starling_suite.sh sift1m full_page release
./run_starling_suite.sh deep1m full_page release
./run_starling_suite.sh gist1m full_page release
./run_starling_suite.sh text2image1m full_page release
```

For 100M datasets, run one phase at a time:

```bash
./run_starling_suite.sh sift100m build release
./run_starling_suite.sh sift100m gp release
./run_starling_suite.sh sift100m page release
```

Repeat for:

```text
deep100m
spacev100m
```

Avoid `all full` on the 100M datasets unless the machine has enough time,
scratch space, and thermal headroom.

## Output Layout

For `sift1m` with the default 1M profile, the index prefix is:

```bash
/mnt/diskann_data/starling_data/index/sift1m_starling/sift1m_R64_L100_B2_M2/sift1m_R64_L100_B2_M2
```

Representative outputs:

```text
/mnt/diskann_data/starling_data/index/sift1m_starling/sift1m_R64_L100_B2_M2/sift1m_R64_L100_B2_M2_disk.index
/mnt/diskann_data/starling_data/index/sift1m_starling/sift1m_R64_L100_B2_M2/sift1m_R64_L100_B2_M2_disk_beam_search.index
/mnt/diskann_data/starling_data/index/sift1m_starling/sift1m_R64_L100_B2_M2/sift1m_R64_L100_B2_M2_partition.bin
/mnt/diskann_data/starling_data/index/sift1m_starling/sift1m_R64_L100_B2_M2/search/
/mnt/diskann_data/starling_data/index/sift1m_starling/sift1m_R64_L100_B2_M2/result/
```

## Notes

- `run_benchmark.sh search` still clears OS page cache with `sudo tee
  /proc/sys/vm/drop_caches`.
- Page search requires `gp` to have completed first.
- `full` builds a random/frequency memory graph and searches with `MEM_L=10`.
- Direct `run_benchmark.sh` use is still supported by setting
  `STARLING_CONFIG=<path>`.
