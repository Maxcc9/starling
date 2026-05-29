# Index Storage

All generated Starling index-related artifacts must be written under:

```bash
/mnt/diskann_data/starling_data/index
```

Do not write generated index files into the repository, `../indices`, or `/tmp`
except for short-lived manual experiments.

## Naming Convention

The naming follows the structure already used by `/mnt/diskann_data/index`:

```text
/mnt/diskann_data/starling_data/index/<experiment>/<index_name>/<index_name>_disk.index
/mnt/diskann_data/starling_data/index/<experiment>/<index_name>/<index_name>_pq_compressed.bin
/mnt/diskann_data/starling_data/index/<experiment>/<index_name>/<index_name>_pq_pivots.bin
/mnt/diskann_data/starling_data/index/<experiment>/<index_name>/<index_name>_sample_data.bin
/mnt/diskann_data/starling_data/index/<experiment>/<index_name>/<index_name>_sample_ids.bin
```

The benchmark script derives defaults as:

```bash
STARLING_INDEX_ROOT=/mnt/diskann_data/starling_data/index
INDEX_EXPERIMENT=${PREFIX}_starling
INDEX_NAME=${PREFIX}_R${R}_L${BUILD_L}_B${B}_M${M}${INDEX_DISK_PQ_SUFFIX}
INDEX_DIR=${STARLING_INDEX_ROOT}/${INDEX_EXPERIMENT}/${INDEX_NAME}
INDEX_PREFIX_PATH=${INDEX_DIR}/${INDEX_NAME}
```

`INDEX_DISK_PQ_SUFFIX` is empty by default. When `DISK_PQ_BYTES` is non-zero,
the scripts append:

```bash
_DPQ${DISK_PQ_BYTES}
```

If `APPEND_REORDER_DATA=1`, the suffix becomes:

```bash
_DPQ${DISK_PQ_BYTES}_reorder
```

For example, with:

```bash
PREFIX=siftsmall
R=16
BUILD_L=32
B=0.00003
M=1
```

the disk index prefix is:

```bash
/mnt/diskann_data/starling_data/index/siftsmall_starling/siftsmall_R16_L32_B0.00003_M1/siftsmall_R16_L32_B0.00003_M1
```

and the disk index file is:

```bash
/mnt/diskann_data/starling_data/index/siftsmall_starling/siftsmall_R16_L32_B0.00003_M1/siftsmall_R16_L32_B0.00003_M1_disk.index
```

For example, `gist1m` uses `DISK_PQ_BYTES=256`, so the index name is:

```bash
gist1m_R64_L100_B8_M8_DPQ256
```

## Derived Artifacts

Benchmark sub-artifacts stay inside the same index directory:

```text
<index_dir>/memory/<mem_name>/
<index_dir>/samples/<sample_name>/
<index_dir>/freq/<freq_name>/
<index_dir>/gp/<gp_name>/
<index_dir>/search/
<index_dir>/result/
```

The global benchmark summary is:

```bash
/mnt/diskann_data/starling_data/index/summary.log
```

## Overrides

Use these environment or `config_local.sh` variables only when a run needs a
custom layout:

```bash
STARLING_INDEX_ROOT=/mnt/diskann_data/starling_data/index
INDEX_EXPERIMENT=<dataset_or_experiment_group>
INDEX_NAME=<explicit_index_name>
```

`INDEX_PREFIX_PATH` should normally not be set directly; it is derived from the
root, experiment, and index name.
