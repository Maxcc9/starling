# Experiment Storage Architecture

This document describes where experiment artifacts are stored, how generated
index files differ from search/report outputs, and how to trace a result back to
the build and search settings that produced it.

## Storage Rule

Generated artifacts are split into two roots:

```text
/mnt/diskann_data/starling_data/index/   persistent index artifacts
<repo>/reports/                          search outputs, logs, CSVs, PNGs
```

Do not put index files under `reports/`. Do not put search CSV/PNG/report files
under `/mnt/diskann_data/starling_data/index/`.

## Index Root

Starling index artifacts live under:

```text
/mnt/diskann_data/starling_data/index/<experiment>/<index_name>/
```

The default benchmark scripts derive:

```bash
INDEX_EXPERIMENT=${PREFIX}_starling
INDEX_NAME=${PREFIX}_R${R}_L${BUILD_L}_B${B}_M${M}${INDEX_QUERY_PQ_SUFFIX}${INDEX_DISK_PQ_SUFFIX}
INDEX_DIR=/mnt/diskann_data/starling_data/index/${INDEX_EXPERIMENT}/${INDEX_NAME}
INDEX_PREFIX_PATH=${INDEX_DIR}/${INDEX_NAME}
```

Examples:

```text
/mnt/diskann_data/starling_data/index/sift1m_starling/sift1m_R64_L100_B2_M2/
/mnt/diskann_data/starling_data/index/deep1m_starling/deep1m_R64_L100_B2_M2/
/mnt/diskann_data/starling_data/index/gist1m_starling/gist1m_R64_L100_B8_M8_DPQ256/
/mnt/diskann_data/starling_data/index/gist1m_starling/gist1m_R128_L200_B8_M8_DPQ256/
```

## Main Index Files

Inside one `<index_dir>`, the core files are:

```text
<index_name>_disk.index                  current disk index used by page search
<index_name>_disk_beam_search.index      original disk index preserved for beam search
<index_name>_partition.bin               page id / graph partition metadata
<index_name>_pq_compressed.bin           query-time in-memory PQ codes
<index_name>_pq_pivots.bin               query-time PQ centroids
<index_name>_disk.index_pq_pivots.bin    disk-PQ centroids, when DPQ is enabled
<index_name>_sample_data.bin             sampled base vectors for memory graph workflows
<index_name>_sample_ids.bin              ids for sampled vectors
build.log                                disk-index build log
```

For page search, both files must exist:

```text
<index_name>_disk.index
<index_name>_partition.bin
```

For classic beam search after a relayout, prefer:

```text
<index_name>_disk_beam_search.index
```

because `<index_name>_disk.index` may have been replaced by the page-relayout
version.

## Derived Index Subdirectories

Index-local workflow artifacts remain inside the same index directory:

```text
<index_dir>/gp/<gp_name>/                graph partition and relayout files
<index_dir>/memory/<mem_name>/           optional in-memory navigation graph
<index_dir>/samples/<sample_name>/       random sampled data for memory graph
<index_dir>/freq/<freq_name>/            visit-frequency files
```

For graph partition:

```text
<index_dir>/gp/<gp_name>/<gp_name>_part.bin
<index_dir>/gp/<gp_name>/<gp_name>_part.bin.log
<index_dir>/gp/<gp_name>/relayout.log
```

After graph partition and relayout complete, the script copies:

```text
<gp_name>_part.bin -> <index_name>_partition.bin
<gp_name>_part_tmp.index -> <index_name>_disk.index
```

If a failed graph partition run is moved aside for debugging, its directory may
have a suffix such as:

```text
gp_times8_lock0_freq0_cut4096.failed_before_dpq_loader_fix_<timestamp>/
```

These failed directories are diagnostic only and should not be used by search.

## Report Root

Search outputs and analysis files live in the repository:

```text
reports/<experiment>/
```

Common report subdirectories:

```text
logs/       raw stdout/stderr logs from search binaries
search/     benchmark script search logs
result/     result id/distance binary files
results/    result files for custom scripts that use plural naming
analysis/   generated CSV summaries and PNG figures
telemetry/  per-query or per-hop telemetry CSV files
```

Examples:

```text
reports/sift1m_selected_pareto_T16W4/analysis/
reports/oracle_beam_side_pareto_T16W4/sift1m/analysis/
reports/oracle_beam_side_pareto_T16W4/deep1m/analysis/
reports/gist1m_r128_l200_dpq256_smoke/
```

## Current Important Experiment Groups

### Starling selected Pareto, sift1m

Purpose: selected beam-side and page-side comparisons on the Starling page index.

Index:

```text
/mnt/diskann_data/starling_data/index/sift1m_starling/sift1m_R64_L100_B2_M2/
```

Reports:

```text
reports/sift1m_selected_pareto_T16W4/analysis/
```

Key outputs:

```text
beam_side_5panel_pareto.png
page_side_5panel_pareto.png
beam_side_target_summary.csv
page_side_target_summary.csv
```

### Oracle beam-side Pareto

Purpose: run beam-side methods on DiskANN-style oracle indexes.

Report root:

```text
reports/oracle_beam_side_pareto_T16W4/
```

Dataset outputs:

```text
reports/oracle_beam_side_pareto_T16W4/sift1m/beam_side_raw.csv
reports/oracle_beam_side_pareto_T16W4/sift1m/analysis/sift1m_beam_side_5panel_pareto.png
reports/oracle_beam_side_pareto_T16W4/deep1m/beam_side_raw.csv
reports/oracle_beam_side_pareto_T16W4/deep1m/analysis/deep1m_beam_side_5panel_pareto.png
```

`gist1m_R128_L200_QD960` is blocked for the current Starling search binary
because its full node record exceeds one 4KB sector. Use the DPQ256 Starling
index below for page-search-compatible gist1m experiments.

### gist1m R128/L200 DPQ256

Purpose: high-quality `gist1m` Starling page-search index with disk PQ so each
node fits inside a 4KB sector.

Config:

```text
scripts/config_gist1m_r128_l200_dpq256.sh
```

Index:

```text
/mnt/diskann_data/starling_data/index/gist1m_starling/gist1m_R128_L200_B8_M8_DPQ256/
```

Important build facts:

```text
R=128
BUILD_L=200
B=8
M=8
DISK_PQ_BYTES=256
QUERY_PQ_BYTES=0
max_node_len=772 bytes
nodes_per_sector=5
```

Smoke report:

```text
reports/gist1m_r128_l200_dpq256_smoke/
```

The smoke test confirmed that page search can load and query the index. The
single tested point was:

```text
L=100, W=4, T=16, K=10
Recall@10=74.83
Mean IOs=101.24
QPS=495.04
```

This is only a load/search sanity check. It is not a tuned Pareto result.

## Script and Config Files

Workflow scripts live under:

```text
scripts/
```

Important examples:

```text
scripts/run_benchmark.sh
scripts/config_dataset.sh
scripts/config_gist1m_r128_l200_dpq256.sh
scripts/run_oracle_beam_side_pareto.sh
scripts/plot_oracle_beam_side_pareto.py
scripts/generate_starling_reports.py
scripts/generate_starling_ablation_reports.py
```

For repeatability, prefer a dedicated config file for long-running index builds
instead of exporting many ad-hoc variables in the shell.

## How To Trace A Result

Given a report file:

```text
reports/<experiment>/<dataset-or-index>/analysis/<file>.csv
```

Trace it in this order:

1. Check the analysis CSV or PNG title for dataset, build, and search settings.
2. Check the raw log in `logs/` or `search/`.
3. Identify the index prefix used by the command.
4. Inspect the matching index directory under
   `/mnt/diskann_data/starling_data/index/`.
5. Read `build.log` and `gp/<gp_name>/*.log` for build and relayout details.

For page search, verify the command used:

```text
--use_page_search 1
--disk_file_path <index_name>_disk.index
```

For classic beam search, verify the command used:

```text
--use_page_search 0
--disk_file_path <index_name>_disk_beam_search.index
```

## Common Pitfalls

- Do not compare a Starling page index against a DiskANN oracle index unless the
  comparison is explicitly labeled as cross-index.
- Do not treat smoke-test outputs as Pareto results.
- Do not use failed `gp/*.failed_*` directories for search.
- For DPQ indexes, graph partition must parse the disk node payload as byte-sized
  PQ codes, not as full `dim * sizeof(float)` vectors.
- For high-dimensional datasets such as `gist1m`, full-vector disk nodes may
  exceed 4KB. Use `DISK_PQ_BYTES` when building Starling page-search indexes.
