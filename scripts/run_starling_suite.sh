#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

DATASETS=(sift1m deep1m gist1m text2image1m sift100m deep100m spacev100m)
DATASETS_EXCEPT_SIFT100M=(sift1m deep1m gist1m text2image1m deep100m spacev100m)

usage() {
  cat <<'USAGE'
Usage: ./run_starling_suite.sh <dataset|all|all_except_sift100m> <phase> [release|debug]

Datasets:
  sift1m deep1m gist1m text2image1m sift100m deep100m spacev100m all all_except_sift100m

Phases:
  build       Build disk index only.
  gp          Run graph partition and relayout.
  beam        Run beam-search KNN.
  page        Run page-search KNN.
  freq        Generate visit-frequency file.
  build_mem   Build in-memory navigation graph.
  full_beam   build -> beam.
  full_page   build -> gp -> page.
  full        build -> freq -> build_mem -> gp -> page.

Environment overrides:
  STARLING_INDEX_ROOT  default: /mnt/diskann_data/starling_data/index
  DISKANN_DATA_ROOT    default: /home/gt/research/DiskANN/data

Examples:
  ./run_starling_suite.sh sift1m full_page release
  ./run_starling_suite.sh all build release
USAGE
}

dataset_function_exists() {
  local dataset=$1
  bash -c "source '$SCRIPT_DIR/config_dataset.sh'; declare -F dataset_${dataset} >/dev/null"
}

make_config() {
  local dataset=$1
  local config_path=$2

  cat > "$config_path" <<EOF
source config_sample.sh
dataset_${dataset}
apply_starling_dataset_profile
INDEX_EXPERIMENT="\${PREFIX}_starling"
EOF
}

run_phase_for_dataset() {
  local dataset=$1
  local phase=$2
  local build_type=$3
  local config_path="/tmp/starling_${dataset}_config.sh"

  if ! dataset_function_exists "$dataset"; then
    echo "Unknown dataset: $dataset" >&2
    usage
    exit 1
  fi

  make_config "$dataset" "$config_path"
  echo "Using generated config: $config_path"

  case "$phase" in
    build)
      STARLING_CONFIG="$config_path" ./run_benchmark.sh "$build_type" build knn
    ;;
    gp)
      STARLING_CONFIG="$config_path" ./run_benchmark.sh "$build_type" gp knn
    ;;
    beam)
      echo "USE_PAGE_SEARCH=0" >> "$config_path"
      STARLING_CONFIG="$config_path" ./run_benchmark.sh "$build_type" search knn
    ;;
    page)
      echo "USE_PAGE_SEARCH=1" >> "$config_path"
      STARLING_CONFIG="$config_path" ./run_benchmark.sh "$build_type" search knn
    ;;
    freq)
      STARLING_CONFIG="$config_path" ./run_benchmark.sh "$build_type" freq knn
    ;;
    build_mem)
      STARLING_CONFIG="$config_path" ./run_benchmark.sh "$build_type" build_mem knn
    ;;
    full_beam)
      STARLING_CONFIG="$config_path" ./run_benchmark.sh "$build_type" build knn
      echo "USE_PAGE_SEARCH=0" >> "$config_path"
      STARLING_CONFIG="$config_path" ./run_benchmark.sh "$build_type" search knn
    ;;
    full_page)
      STARLING_CONFIG="$config_path" ./run_benchmark.sh "$build_type" build knn
      STARLING_CONFIG="$config_path" ./run_benchmark.sh "$build_type" gp knn
      echo "USE_PAGE_SEARCH=1" >> "$config_path"
      STARLING_CONFIG="$config_path" ./run_benchmark.sh "$build_type" search knn
    ;;
    full)
      STARLING_CONFIG="$config_path" ./run_benchmark.sh "$build_type" build knn
      STARLING_CONFIG="$config_path" ./run_benchmark.sh "$build_type" freq knn
      STARLING_CONFIG="$config_path" ./run_benchmark.sh "$build_type" build_mem knn
      STARLING_CONFIG="$config_path" ./run_benchmark.sh "$build_type" gp knn
      echo "USE_PAGE_SEARCH=1" >> "$config_path"
      echo "MEM_L=10" >> "$config_path"
      STARLING_CONFIG="$config_path" ./run_benchmark.sh "$build_type" search knn
    ;;
    *)
      echo "Unknown phase: $phase" >&2
      usage
      exit 1
    ;;
  esac
}

if [[ $# -lt 2 || $# -gt 3 ]]; then
  usage
  exit 1
fi

DATASET=$1
PHASE=$2
BUILD_TYPE=${3:-release}

cd "$SCRIPT_DIR"

if [[ "$DATASET" == "all" ]]; then
  for dataset in "${DATASETS[@]}"; do
    echo "========================================"
    echo "Dataset: $dataset  Phase: $PHASE"
    echo "========================================"
    run_phase_for_dataset "$dataset" "$PHASE" "$BUILD_TYPE"
  done
elif [[ "$DATASET" == "all_except_sift100m" ]]; then
  for dataset in "${DATASETS_EXCEPT_SIFT100M[@]}"; do
    echo "========================================"
    echo "Dataset: $dataset  Phase: $PHASE"
    echo "========================================"
    run_phase_for_dataset "$dataset" "$PHASE" "$BUILD_TYPE"
  done
else
  run_phase_for_dataset "$DATASET" "$PHASE" "$BUILD_TYPE"
fi
