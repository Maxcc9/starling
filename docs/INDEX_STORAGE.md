# Index Storage

All generated Starling index-related artifacts must be written under:

```bash
/mnt/starling_data/index
```

Do not write generated index files into the repository, `../indices`, or `/tmp`
except for short-lived manual experiments.

## Naming Convention

The naming follows the structure already used by `/mnt/diskann_data/index`:

```text
/mnt/starling_data/index/<experiment>/<index_name>/<index_name>_disk.index
/mnt/starling_data/index/<experiment>/<index_name>/<index_name>_pq_compressed.bin
/mnt/starling_data/index/<experiment>/<index_name>/<index_name>_pq_pivots.bin
/mnt/starling_data/index/<experiment>/<index_name>/<index_name>_sample_data.bin
/mnt/starling_data/index/<experiment>/<index_name>/<index_name>_sample_ids.bin
```

The benchmark script derives defaults as:

```bash
STARLING_INDEX_ROOT=/mnt/starling_data/index
INDEX_EXPERIMENT=${PREFIX}_starling
INDEX_NAME=${PREFIX}_R${R}_L${BUILD_L}_B${B}_M${M}
INDEX_DIR=${STARLING_INDEX_ROOT}/${INDEX_EXPERIMENT}/${INDEX_NAME}
INDEX_PREFIX_PATH=${INDEX_DIR}/${INDEX_NAME}
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
/mnt/starling_data/index/siftsmall_starling/siftsmall_R16_L32_B0.00003_M1/siftsmall_R16_L32_B0.00003_M1
```

and the disk index file is:

```bash
/mnt/starling_data/index/siftsmall_starling/siftsmall_R16_L32_B0.00003_M1/siftsmall_R16_L32_B0.00003_M1_disk.index
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
/mnt/starling_data/index/summary.log
```

## Overrides

Use these environment or `config_local.sh` variables only when a run needs a
custom layout:

```bash
STARLING_INDEX_ROOT=/mnt/starling_data/index
INDEX_EXPERIMENT=<dataset_or_experiment_group>
INDEX_NAME=<explicit_index_name>
```

`INDEX_PREFIX_PATH` should normally not be set directly; it is derived from the
root, experiment, and index name.

