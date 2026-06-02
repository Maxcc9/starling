#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)

DATASET=${1:-sift1m}
MODE=${2:-plan}
BUILD_TYPE=${BUILD_TYPE:-release}

usage() {
  cat <<'USAGE'
Usage: ./run_starling_feature_ablation.sh <dataset> <plan|prepare|search|reports|full>

Default dataset: sift1m

Feature lines:
  beam
  page_only
  page_ratio
  page_random_mem
  page_freq_mem
  page_freq_gp
  page_freq_gp_random_mem
  page_freq_gp_freq_mem

Outputs:
  Index artifacts: /mnt/diskann_data/starling_data/index/<dataset>_starling/...
  Reports:         reports/<dataset>_ablation/<feature_line>/
USAGE
}

dataset_function_exists() {
  local dataset=$1
  bash -c "source '${SCRIPT_DIR}/config_dataset.sh'; declare -F dataset_${dataset} >/dev/null"
}

make_base_config() {
  local dataset=$1
  local config_path=$2
  local query_pq_bytes=${QUERY_PQ_BYTES:-0}

  cat > "$config_path" <<EOF
source config_sample.sh
dataset_${dataset}
apply_starling_dataset_profile
INDEX_EXPERIMENT="\${PREFIX}_starling"
QUERY_PQ_BYTES=${query_pq_bytes}

BM_LIST=(4)
T_LIST=(16)
LS="30 50 80 100 150 200"
CACHE=0
MEM_TOPK=10

FREQ_QUERY_CNT=0
FREQ_BM=4
FREQ_L=100
FREQ_T=16
FREQ_CACHE=0
FREQ_MEM_L=0

MEM_R=64
MEM_BUILD_L=100
MEM_ALPHA=1.2
MEM_RAND_SAMPLING_RATE=0.01
MEM_FREQ_USE_RATE=0.01

GP_TIMES=8
GP_T=16
GP_LOCK_NUMS=0
GP_CUT=4096
EOF
}

append_overrides() {
  local src=$1
  local dst=$2
  shift 2
  cp "$src" "$dst"
  for override in "$@"; do
    echo "$override" >> "$dst"
  done
}

load_paths() {
  local config_path=$1
  local had_nounset=0
  case "$-" in
    *u*)
      had_nounset=1
      set +u
    ;;
  esac

  # shellcheck disable=SC1090
  source "$config_path"

  if [[ "$had_nounset" -eq 1 ]]; then
    set -u
  fi

  STARLING_INDEX_ROOT="${STARLING_INDEX_ROOT:-/mnt/diskann_data/starling_data/index}"
  DISK_PQ_BYTES="${DISK_PQ_BYTES:-0}"
  QUERY_PQ_BYTES="${QUERY_PQ_BYTES:-0}"
  APPEND_REORDER_DATA="${APPEND_REORDER_DATA:-0}"
  local suffix=""
  if [[ "$QUERY_PQ_BYTES" != "0" ]]; then
    suffix="${suffix}_QPQ${QUERY_PQ_BYTES}"
  fi
  if [[ "$DISK_PQ_BYTES" != "0" ]]; then
    suffix="${suffix}_DPQ${DISK_PQ_BYTES}"
    if [[ "$APPEND_REORDER_DATA" == "1" || "$APPEND_REORDER_DATA" == "true" ]]; then
      suffix="${suffix}_reorder"
    fi
  fi
  INDEX_EXPERIMENT="${INDEX_EXPERIMENT:-${PREFIX}_starling}"
  INDEX_NAME="${INDEX_NAME:-${PREFIX}_R${R}_L${BUILD_L}_B${B}_M${M}${suffix}}"
  INDEX_DIR="${STARLING_INDEX_ROOT}/${INDEX_EXPERIMENT}/${INDEX_NAME}"
  INDEX_PREFIX_PATH="${INDEX_DIR}/${INDEX_NAME}"

  MEM_INDEX_NAME="mem_R${MEM_R}_L${MEM_BUILD_L}_A${MEM_ALPHA}_freq${MEM_USE_FREQ}_rand${MEM_RAND_SAMPLING_RATE}_freq_rate${MEM_FREQ_USE_RATE}"
  MEM_INDEX_DIR="${INDEX_DIR}/memory/${MEM_INDEX_NAME}"
  MEM_INDEX_PATH="${MEM_INDEX_DIR}/${MEM_INDEX_NAME}"

  GP_NAME="gp_times${GP_TIMES}_lock${GP_LOCK_NUMS}_freq${GP_USE_FREQ}_cut${GP_CUT}"
  GP_DIR="${INDEX_DIR}/gp/${GP_NAME}"
  GP_PATH="${GP_DIR}/${GP_NAME}"

  FREQ_NAME="freq_nq${FREQ_QUERY_CNT}_bm${FREQ_BM}_L${FREQ_L}_T${FREQ_T}"
  FREQ_DIR="${INDEX_DIR}/freq/${FREQ_NAME}"
  FREQ_PATH="${FREQ_DIR}/${FREQ_NAME}"
}

run_benchmark() {
  local config_path=$1
  local phase=$2
  STARLING_CONFIG="$config_path" "${SCRIPT_DIR}/run_benchmark.sh" "$BUILD_TYPE" "$phase" knn
}

activate_gp_layout() {
  local config_path=$1
  load_paths "$config_path"

  local relayout_index="${GP_PATH}_part_tmp.index"
  local partition_file="${GP_PATH}_part.bin"

  if [[ ! -f "$relayout_index" || ! -f "$partition_file" ]]; then
    echo "Missing relayout artifacts for config $config_path" >&2
    echo "Run prepare first, or inspect $GP_DIR" >&2
    exit 1
  fi

  cp "$relayout_index" "${INDEX_PREFIX_PATH}_disk.index"
  cp "$partition_file" "${INDEX_PREFIX_PATH}_partition.bin"
}

prepare() {
  local dataset=$1
  local base="/tmp/starling_ablation_${dataset}_base.sh"
  make_base_config "$dataset" "$base"
  load_paths "$base"

  echo "Preparing shared index artifacts for $dataset"

  if [[ -f "${INDEX_PREFIX_PATH}_disk.index" || -f "${INDEX_PREFIX_PATH}_disk_beam_search.index" ]]; then
    echo "  skip build: disk index exists"
  else
    run_benchmark "$base" build
  fi

  if [[ -f "${FREQ_PATH}_freq.bin" ]]; then
    echo "  skip freq: frequency file exists"
  else
    run_benchmark "$base" freq
  fi

  local random_mem="/tmp/starling_ablation_${dataset}_random_mem.sh"
  append_overrides "$base" "$random_mem" "MEM_USE_FREQ=0"
  load_paths "$random_mem"
  if compgen -G "${MEM_INDEX_PATH}_index*" >/dev/null; then
    echo "  skip random memory index: exists"
  else
    run_benchmark "$random_mem" build_mem
  fi

  local freq_mem="/tmp/starling_ablation_${dataset}_freq_mem.sh"
  append_overrides "$base" "$freq_mem" "MEM_USE_FREQ=1"
  load_paths "$freq_mem"
  if compgen -G "${MEM_INDEX_PATH}_index*" >/dev/null; then
    echo "  skip frequency memory index: exists"
  else
    run_benchmark "$freq_mem" build_mem
  fi

  local gp_plain="/tmp/starling_ablation_${dataset}_gp_plain.sh"
  append_overrides "$base" "$gp_plain" "GP_USE_FREQ=0" "GP_LOCK_NUMS=0"
  load_paths "$gp_plain"
  if [[ -f "${INDEX_PREFIX_PATH}_partition.bin" && -d "$GP_DIR" ]]; then
    echo "  skip plain gp: partition exists"
  else
    run_benchmark "$gp_plain" gp
  fi

  local gp_freq="/tmp/starling_ablation_${dataset}_gp_freq.sh"
  append_overrides "$base" "$gp_freq" "GP_USE_FREQ=1" "GP_LOCK_NUMS=1"
  load_paths "$gp_freq"
  if [[ -d "$GP_DIR" ]]; then
    echo "  skip frequency gp: gp dir exists"
  else
    run_benchmark "$gp_freq" gp
  fi
}

line_config() {
  local dataset=$1
  local line=$2
  local config=$3
  local base="/tmp/starling_ablation_${dataset}_base.sh"
  make_base_config "$dataset" "$base"

  local report_experiment="${dataset}_ablation"

  case "$line" in
    beam)
      append_overrides "$base" "$config" \
        "USE_PAGE_SEARCH=0" \
        "MEM_L=0" \
        "MEM_USE_FREQ=0" \
        "GP_USE_FREQ=0" \
        "REPORT_EXPERIMENT=${report_experiment}" \
        "REPORT_NAME=beam"
    ;;
    page_only)
      append_overrides "$base" "$config" \
        "USE_PAGE_SEARCH=1" \
        "PS_USE_RATIO=1.0" \
        "MEM_L=0" \
        "MEM_USE_FREQ=0" \
        "GP_USE_FREQ=0" \
        "GP_LOCK_NUMS=0" \
        "REPORT_EXPERIMENT=${report_experiment}" \
        "REPORT_NAME=page_only"
    ;;
    page_ratio)
      append_overrides "$base" "$config" \
        "USE_PAGE_SEARCH=1" \
        "PS_USE_RATIO=0.5" \
        "MEM_L=0" \
        "MEM_USE_FREQ=0" \
        "GP_USE_FREQ=0" \
        "GP_LOCK_NUMS=0" \
        "REPORT_EXPERIMENT=${report_experiment}" \
        "REPORT_NAME=page_ratio"
    ;;
    page_random_mem)
      append_overrides "$base" "$config" \
        "USE_PAGE_SEARCH=1" \
        "PS_USE_RATIO=1.0" \
        "MEM_L=10" \
        "MEM_USE_FREQ=0" \
        "GP_USE_FREQ=0" \
        "GP_LOCK_NUMS=0" \
        "REPORT_EXPERIMENT=${report_experiment}" \
        "REPORT_NAME=page_random_mem"
    ;;
    page_freq_mem)
      append_overrides "$base" "$config" \
        "USE_PAGE_SEARCH=1" \
        "PS_USE_RATIO=1.0" \
        "MEM_L=10" \
        "MEM_USE_FREQ=1" \
        "GP_USE_FREQ=0" \
        "GP_LOCK_NUMS=0" \
        "REPORT_EXPERIMENT=${report_experiment}" \
        "REPORT_NAME=page_freq_mem"
    ;;
    page_freq_gp)
      append_overrides "$base" "$config" \
        "USE_PAGE_SEARCH=1" \
        "PS_USE_RATIO=1.0" \
        "MEM_L=0" \
        "MEM_USE_FREQ=0" \
        "GP_USE_FREQ=1" \
        "GP_LOCK_NUMS=1" \
        "REPORT_EXPERIMENT=${report_experiment}" \
        "REPORT_NAME=page_freq_gp"
    ;;
    page_freq_gp_random_mem)
      append_overrides "$base" "$config" \
        "USE_PAGE_SEARCH=1" \
        "PS_USE_RATIO=1.0" \
        "MEM_L=10" \
        "MEM_USE_FREQ=0" \
        "GP_USE_FREQ=1" \
        "GP_LOCK_NUMS=1" \
        "REPORT_EXPERIMENT=${report_experiment}" \
        "REPORT_NAME=page_freq_gp_random_mem"
    ;;
    page_freq_gp_freq_mem)
      append_overrides "$base" "$config" \
        "USE_PAGE_SEARCH=1" \
        "PS_USE_RATIO=1.0" \
        "MEM_L=10" \
        "MEM_USE_FREQ=1" \
        "GP_USE_FREQ=1" \
        "GP_LOCK_NUMS=1" \
        "REPORT_EXPERIMENT=${report_experiment}" \
        "REPORT_NAME=page_freq_gp_freq_mem"
    ;;
    *)
      echo "Unknown feature line: $line" >&2
      exit 1
    ;;
  esac
}

search() {
  local dataset=$1
  local lines=(
    beam
    page_only
    page_ratio
    page_random_mem
    page_freq_mem
    page_freq_gp
    page_freq_gp_random_mem
    page_freq_gp_freq_mem
  )

  for line in "${lines[@]}"; do
    echo "Running feature line: $line"
    local config="/tmp/starling_ablation_${dataset}_${line}.sh"
    line_config "$dataset" "$line" "$config"
    case "$line" in
      beam)
        # Beam search reads _disk_beam_search.index when available.
      ;;
      page_only|page_ratio|page_random_mem|page_freq_mem)
        local plain_gp="/tmp/starling_ablation_${dataset}_activate_plain_gp.sh"
        line_config "$dataset" page_only "$plain_gp"
        activate_gp_layout "$plain_gp"
      ;;
      page_freq_gp|page_freq_gp_random_mem|page_freq_gp_freq_mem)
        local freq_gp="/tmp/starling_ablation_${dataset}_activate_freq_gp.sh"
        line_config "$dataset" page_freq_gp "$freq_gp"
        activate_gp_layout "$freq_gp"
      ;;
    esac
    run_benchmark "$config" search
  done
}

reports() {
  local dataset=$1
  local report_root="${REPO_ROOT}/reports/${dataset}_ablation"
  local lines=(
    beam
    page_only
    page_ratio
    page_random_mem
    page_freq_mem
    page_freq_gp
    page_freq_gp_random_mem
    page_freq_gp_freq_mem
  )

  for line in "${lines[@]}"; do
    local dir="${report_root}/${line}"
    if [[ -d "${dir}/search" ]]; then
      "${REPO_ROOT}/scripts/generate_starling_reports.py" \
        --report-dir "$dir" \
        --dataset "$dataset" \
        --method "$line" \
        --prefix "${dataset}_${line}"
    fi
  done

  "${REPO_ROOT}/scripts/generate_starling_ablation_reports.py" \
    --ablation-dir "$report_root" \
    --dataset "$dataset"
}

plan() {
  cat <<EOF
Dataset: $DATASET
Build type: $BUILD_TYPE

Feature lines:
  beam                         original beam-search baseline
  page_only                    page search, no mem graph, no frequency GP
  page_ratio                   page_only with PS_USE_RATIO=0.5
  page_random_mem              page search + random memory graph
  page_freq_mem                page search + frequency memory graph (workload-aware)
  page_freq_gp                 page search + frequency graph partition (workload-aware)
  page_freq_gp_random_mem      frequency GP + random memory graph
  page_freq_gp_freq_mem        frequency GP + frequency memory graph (workload-aware full)

Prepare creates shared build/freq/memory/gp artifacts under STARLING_INDEX_ROOT.
Search writes separate report directories under reports/${DATASET}_ablation/.
EOF
}

if [[ "$DATASET" == "-h" || "$DATASET" == "--help" ]]; then
  usage
  exit 0
fi

if ! dataset_function_exists "$DATASET"; then
  echo "Unknown dataset: $DATASET" >&2
  usage
  exit 1
fi

cd "$SCRIPT_DIR"

case "$MODE" in
  plan)
    plan
  ;;
  prepare)
    prepare "$DATASET"
  ;;
  search)
    search "$DATASET"
  ;;
  reports)
    reports "$DATASET"
  ;;
  full)
    prepare "$DATASET"
    search "$DATASET"
    reports "$DATASET"
  ;;
  *)
    usage
    exit 1
  ;;
esac
