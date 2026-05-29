#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

DATASETS=(sift1m deep1m gist1m text2image1m deep100m spacev100m)
BUILD_TYPE=${BUILD_TYPE:-release}
MODE=${1:-plan}

usage() {
  cat <<'USAGE'
Usage: ./run_starling_best_except_sift100m.sh <plan|prepare|search|full>

Modes:
  plan      Print planned datasets and sweeps.
  prepare   Build disk index, frequency file, memory nav graph, and graph partition.
  search    Run search sweeps. Requires prepare to have completed.
  full      prepare -> search.

Environment overrides:
  BUILD_TYPE            release/debug, default: release
  STARLING_INDEX_ROOT   default: /mnt/diskann_data/starling_data/index
  DISKANN_DATA_ROOT     default: /home/gt/research/DiskANN/data

Target datasets:
  sift1m deep1m gist1m text2image1m deep100m spacev100m
USAGE
}

make_config() {
  local dataset=$1
  local config_path=$2

  cat > "$config_path" <<EOF
source config_sample.sh
dataset_${dataset}
apply_starling_dataset_profile
INDEX_EXPERIMENT="\${PREFIX}_best"
EOF
}

load_paths() {
  local config_path=$1
  # shellcheck disable=SC1090
  source "$config_path"

  STARLING_INDEX_ROOT="${STARLING_INDEX_ROOT:-/mnt/diskann_data/starling_data/index}"
  INDEX_EXPERIMENT="${INDEX_EXPERIMENT:-${PREFIX}_starling}"
  INDEX_NAME="${INDEX_NAME:-${PREFIX}_R${R}_L${BUILD_L}_B${B}_M${M}}"
  INDEX_DIR="${STARLING_INDEX_ROOT}/${INDEX_EXPERIMENT}/${INDEX_NAME}"
  INDEX_PREFIX_PATH="${INDEX_DIR}/${INDEX_NAME}"

  MEM_INDEX_NAME="mem_R${MEM_R}_L${MEM_BUILD_L}_A${MEM_ALPHA}_freq${MEM_USE_FREQ}_rand${MEM_RAND_SAMPLING_RATE}_freq_rate${MEM_FREQ_USE_RATE}"
  MEM_INDEX_DIR="${INDEX_DIR}/memory/${MEM_INDEX_NAME}"
  MEM_INDEX_PATH="${MEM_INDEX_DIR}/${MEM_INDEX_NAME}"
  GP_NAME="gp_times${GP_TIMES}_lock${GP_LOCK_NUMS}_freq${GP_USE_FREQ}_cut${GP_CUT}"
  GP_DIR="${INDEX_DIR}/gp/${GP_NAME}"
  FREQ_NAME="freq_nq${FREQ_QUERY_CNT}_bm${FREQ_BM}_L${FREQ_L}_T${FREQ_T}"
  FREQ_DIR="${INDEX_DIR}/freq/${FREQ_NAME}"
  FREQ_PATH="${FREQ_DIR}/${FREQ_NAME}"
}

run_benchmark_with_config() {
  local config_path=$1
  local action=$2
  local kind=${3:-knn}
  STARLING_CONFIG="$config_path" ./run_benchmark.sh "$BUILD_TYPE" "$action" "$kind"
}

prepare_dataset() {
  local dataset=$1
  local config_path="/tmp/starling_best_${dataset}_base.sh"
  make_config "$dataset" "$config_path"
  load_paths "$config_path"

  echo "Prepare dataset=$dataset index=$INDEX_DIR"

  if [[ -f "${INDEX_PREFIX_PATH}_disk.index" || -f "${INDEX_PREFIX_PATH}_disk_beam_search.index" ]]; then
    echo "  skip build: disk index already exists"
  else
    run_benchmark_with_config "$config_path" build
  fi

  if [[ -f "${FREQ_PATH}_freq.bin" ]]; then
    echo "  skip freq: frequency file already exists"
  else
    run_benchmark_with_config "$config_path" freq
  fi

  if compgen -G "${MEM_INDEX_PATH}_index*" >/dev/null; then
    echo "  skip build_mem random: memory index already exists"
  else
    run_benchmark_with_config "$config_path" build_mem
  fi

  local freq_mem_config="/tmp/starling_best_${dataset}_freq_mem.sh"
  append_overrides "$config_path" "$freq_mem_config" "MEM_USE_FREQ=1"
  load_paths "$freq_mem_config"
  if compgen -G "${MEM_INDEX_PATH}_index*" >/dev/null; then
    echo "  skip build_mem freq: memory index already exists"
  else
    run_benchmark_with_config "$freq_mem_config" build_mem
  fi

  load_paths "$config_path"

  if [[ -f "${INDEX_PREFIX_PATH}_partition.bin" ]]; then
    echo "  skip gp: partition file already exists"
  else
    run_benchmark_with_config "$config_path" gp
  fi
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

search_dataset() {
  local dataset=$1
  local base_config="/tmp/starling_best_${dataset}_base.sh"
  make_config "$dataset" "$base_config"
  load_paths "$base_config"

  echo "Search sweeps dataset=$dataset index=$INDEX_DIR"

  local bm_values=(2 4 8)
  local thread_values=(8 16)
  local cache_values=(0)
  local mem_l_values=(0 10 50)
  local ratio_values=(1.0 0.75 0.5)

  if [[ "$dataset" == deep100m || "$dataset" == spacev100m ]]; then
    bm_values=(4 8)
    thread_values=(8 16 32)
    cache_values=(0 100000)
    mem_l_values=(0 10)
    ratio_values=(1.0 0.75)
  fi

  if [[ "$dataset" == gist1m ]]; then
    mem_l_values=(0 10)
    ratio_values=(1.0 0.75)
  fi

  local run_id=0

  for bm in "${bm_values[@]}"; do
    for threads in "${thread_values[@]}"; do
      local config="/tmp/starling_best_${dataset}_beam_${run_id}.sh"
      append_overrides "$base_config" "$config" \
        "USE_PAGE_SEARCH=0" \
        "BM_LIST=($bm)" \
        "T_LIST=($threads)" \
        "CACHE=0" \
        "MEM_L=0"
      run_benchmark_with_config "$config" search
      run_id=$((run_id + 1))
    done
  done

  for bm in "${bm_values[@]}"; do
    for threads in "${thread_values[@]}"; do
      for ratio in "${ratio_values[@]}"; do
        for cache in "${cache_values[@]}"; do
          local config="/tmp/starling_best_${dataset}_page_${run_id}.sh"
          append_overrides "$base_config" "$config" \
            "USE_PAGE_SEARCH=1" \
            "BM_LIST=($bm)" \
            "T_LIST=($threads)" \
            "CACHE=$cache" \
            "MEM_L=0" \
            "PS_USE_RATIO=$ratio"
          run_benchmark_with_config "$config" search
          run_id=$((run_id + 1))
        done
      done
    done
  done

  for bm in "${bm_values[@]}"; do
    for threads in "${thread_values[@]}"; do
      for mem_l in "${mem_l_values[@]}"; do
        [[ "$mem_l" == 0 ]] && continue
        for mem_use_freq in 0 1; do
          local config="/tmp/starling_best_${dataset}_page_mem_${run_id}.sh"
          append_overrides "$base_config" "$config" \
            "USE_PAGE_SEARCH=1" \
            "BM_LIST=($bm)" \
            "T_LIST=($threads)" \
            "CACHE=0" \
            "MEM_L=$mem_l" \
            "MEM_USE_FREQ=$mem_use_freq" \
            "PS_USE_RATIO=1.0"
          run_benchmark_with_config "$config" search
          run_id=$((run_id + 1))
        done
      done
    done
  done
}

print_plan() {
  echo "Target datasets: ${DATASETS[*]}"
  echo
  echo "Per dataset:"
  echo "  prepare: build -> freq -> build_mem -> gp"
  echo "  search:"
  echo "    beam: BM_LIST in {2,4,8}, T_LIST in {8,16}"
  echo "    page: BM_LIST in {2,4,8}, T_LIST in {8,16}, PS_USE_RATIO in {1.0,0.75,0.5}"
  echo "    page+mem: MEM_L in {10,50}, MEM_USE_FREQ in {0,1}"
  echo
  echo "100M datasets use a smaller sweep:"
  echo "  BM_LIST in {4,8}, T_LIST in {8,16,32}, PS_USE_RATIO in {1.0,0.75}, CACHE in {0,100000}, MEM_L in {10}, MEM_USE_FREQ in {0,1}"
  echo
  echo "Output root: ${STARLING_INDEX_ROOT:-/mnt/diskann_data/starling_data/index}"
}

cd "$SCRIPT_DIR"

case "$MODE" in
  plan)
    print_plan
  ;;
  prepare)
    for dataset in "${DATASETS[@]}"; do
      prepare_dataset "$dataset"
    done
  ;;
  search)
    for dataset in "${DATASETS[@]}"; do
      search_dataset "$dataset"
    done
  ;;
  full)
    for dataset in "${DATASETS[@]}"; do
      prepare_dataset "$dataset"
      search_dataset "$dataset"
    done
  ;;
  *)
    usage
    exit 1
  ;;
esac
