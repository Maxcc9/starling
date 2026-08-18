#!/bin/bash

set -e
# set -x

source config_local.sh

STARLING_INDEX_ROOT="${STARLING_INDEX_ROOT:-/mnt/starling_data/index}"
INDEX_EXPERIMENT="${INDEX_EXPERIMENT:-${PREFIX}_starling}"
INDEX_NAME="${INDEX_NAME:-${PREFIX}_R${R}_L${BUILD_L}_B${B}_M${M}}"
INDEX_DIR="${STARLING_INDEX_ROOT}/${INDEX_EXPERIMENT}/${INDEX_NAME}"
INDEX_PREFIX_PATH="${INDEX_DIR}/${INDEX_NAME}"

MEM_SAMPLE_NAME="sample_rate_${MEM_RAND_SAMPLING_RATE}"
MEM_SAMPLE_DIR="${INDEX_DIR}/samples/${MEM_SAMPLE_NAME}"
MEM_SAMPLE_PATH="${MEM_SAMPLE_DIR}/${MEM_SAMPLE_NAME}"
MEM_INDEX_NAME="mem_R${MEM_R}_L${MEM_BUILD_L}_A${MEM_ALPHA}_freq${MEM_USE_FREQ}_rand${MEM_RAND_SAMPLING_RATE}_freq_rate${MEM_FREQ_USE_RATE}"
MEM_INDEX_DIR="${INDEX_DIR}/memory/${MEM_INDEX_NAME}"
MEM_INDEX_PATH="${MEM_INDEX_DIR}/${MEM_INDEX_NAME}"
GP_NAME="gp_times${GP_TIMES}_lock${GP_LOCK_NUMS}_freq${GP_USE_FREQ}_cut${GP_CUT}"
GP_DIR="${INDEX_DIR}/gp/${GP_NAME}"
GP_PATH="${GP_DIR}/${GP_NAME}"
FREQ_NAME="freq_nq${FREQ_QUERY_CNT}_bm${FREQ_BM}_L${FREQ_L}_T${FREQ_T}"
FREQ_DIR="${INDEX_DIR}/freq/${FREQ_NAME}"
FREQ_PATH="${FREQ_DIR}/${FREQ_NAME}"

SUMMARY_FILE_PATH="${STARLING_INDEX_ROOT}/summary.log"

print_usage_and_exit() {
  echo "Usage: ./run_benchmark.sh [debug/release] [build/build_mem/freq/gp/search] [knn/range]"
  exit 1
}

check_dir_and_make_if_absent() {
  local dir=$1
  if [ -d "$dir" ]; then
    echo "Directory $dir is already exit. Remove or rename it and then re-run."
    exit 1
  else
    mkdir -p "$dir"
  fi
}

case $1 in
  debug)
    cmake -DCMAKE_BUILD_TYPE=Debug .. -B ../debug
    EXE_PATH=../debug
  ;;
  release)
    EXE_PATH=../release
  ;;
  *)
    print_usage_and_exit
  ;;
esac
# 跳過 cmake/make:binary 已在 build/ 建好(release symlink→build);CMakeCache 卡遷移前舊路徑不能重編

mkdir -p "$STARLING_INDEX_ROOT"

date
case $2 in
  build)
    check_dir_and_make_if_absent "$INDEX_DIR"
    echo "Building disk index..."
    time ${EXE_PATH}/tests/build_disk_index \
      --data_type $DATA_TYPE \
      --dist_fn $DIST_FN \
      --data_path $BASE_PATH \
      --index_path_prefix $INDEX_PREFIX_PATH \
      -R $R \
      -L $BUILD_L \
      -B $B \
      -M $M \
      --PQ_disk_bytes $PQ_DISK_BYTES \
      -T $BUILD_T > "${INDEX_DIR}/build.log"
    cp ${INDEX_PREFIX_PATH}_disk.index ${INDEX_PREFIX_PATH}_disk_beam_search.index
  ;;
  sq)
    cp  ${INDEX_PREFIX_PATH}_disk_beam_search.index ${INDEX_PREFIX_PATH}_disk.index 
    time ${EXE_PATH}/tests/utils/sq ${INDEX_PREFIX_PATH} > "${INDEX_DIR}/sq.log"
  ;;
  build_mem)
    if [ ${MEM_USE_FREQ} -eq 1 ]; then
      if [ ! -d ${FREQ_DIR} ]; then
        echo "Seems you have not gen the freq file, run this script again: ./run_benchmark.sh [debug/release] freq [knn/range]"
        exit 1;
      fi
      echo "Parsing freq file..."
      time ${EXE_PATH}/tests/utils/parse_freq_file ${DATA_TYPE} ${BASE_PATH} ${FREQ_PATH}_freq.bin ${FREQ_PATH} ${MEM_FREQ_USE_RATE} 
      MEM_DATA_PATH=${FREQ_PATH}
    else
      mkdir -p ${MEM_SAMPLE_DIR}
      echo "Generating random slice..."
      time ${EXE_PATH}/tests/utils/gen_random_slice $DATA_TYPE $BASE_PATH $MEM_SAMPLE_PATH $MEM_RAND_SAMPLING_RATE > "${MEM_SAMPLE_DIR}/sample.log"
      MEM_DATA_PATH=${MEM_SAMPLE_PATH}
    fi
    echo "Building memory index..."
    check_dir_and_make_if_absent ${MEM_INDEX_DIR}
    time ${EXE_PATH}/tests/build_memory_index \
      --data_type ${DATA_TYPE} \
      --dist_fn ${DIST_FN} \
      --data_path ${MEM_DATA_PATH} \
      --index_path_prefix ${MEM_INDEX_PATH}_index \
      -R ${MEM_R} \
      -L ${MEM_BUILD_L} \
      --alpha ${MEM_ALPHA} > "${MEM_INDEX_DIR}/build.log"
  ;;
  freq)
    check_dir_and_make_if_absent ${FREQ_DIR}
    FREQ_LOG="${FREQ_DIR}/freq.log"

    DISK_FILE_PATH=${INDEX_PREFIX_PATH}_disk_beam_search.index
    if [ ! -f $DISK_FILE_PATH ]; then
      DISK_FILE_PATH=${INDEX_PREFIX_PATH}_disk.index
    fi

    echo "Generating frequency file... ${FREQ_LOG}"
    time ${EXE_PATH}/tests/search_disk_index_save_freq \
              --data_type $DATA_TYPE \
              --dist_fn $DIST_FN \
              --index_path_prefix $INDEX_PREFIX_PATH \
              --freq_save_path $FREQ_PATH \
              --query_file $FREQ_QUERY_FILE \
              --expected_query_num $FREQ_QUERY_CNT \
              --gt_file $GT_FILE \
              -K $K \
              --result_path ${FREQ_DIR}/result \
              --num_nodes_to_cache ${FREQ_CACHE} \
              -T $FREQ_T \
              -L $FREQ_L \
              -W $FREQ_BM \
              --mem_L ${FREQ_MEM_L} \
              --use_page_search 0 \
              --disk_file_path ${DISK_FILE_PATH} > ${FREQ_LOG}
  ;;
  gp)
    check_dir_and_make_if_absent ${GP_DIR}
    OLD_INDEX_FILE=${INDEX_PREFIX_PATH}_disk_beam_search.index
    if [ ! -f "$OLD_INDEX_FILE" ]; then
      OLD_INDEX_FILE=${INDEX_PREFIX_PATH}_disk.index
    fi
    #using sq index file to gp
    GP_DATA_TYPE=$DATA_TYPE
    if [ $USE_SQ -eq 1 ]; then 
      OLD_INDEX_FILE=${INDEX_PREFIX_PATH}_disk.index
      GP_DATA_TYPE=uint8
    fi
    GP_FILE_PATH=${GP_PATH}_part.bin
    echo "Running graph partition... ${GP_FILE_PATH}.log"
    if [ ${GP_USE_FREQ} -eq 1 ]; then
      time ${EXE_PATH}/graph_partition/partitioner --index_file ${OLD_INDEX_FILE} \
        --data_type $GP_DATA_TYPE --gp_file $GP_FILE_PATH -T $GP_T --ldg_times $GP_TIMES --freq_file ${FREQ_PATH}_freq.bin --lock_nums ${GP_LOCK_NUMS} --cut ${GP_CUT} > ${GP_FILE_PATH}.log
    else
      time ${EXE_PATH}/graph_partition/partitioner --index_file ${OLD_INDEX_FILE} \
        --data_type $GP_DATA_TYPE --gp_file $GP_FILE_PATH -T $GP_T --ldg_times $GP_TIMES > ${GP_FILE_PATH}.log
    fi

    echo "Running relayout... ${GP_DIR}/relayout.log"
    time ${EXE_PATH}/tests/utils/index_relayout ${OLD_INDEX_FILE} ${GP_FILE_PATH} > "${GP_DIR}/relayout.log"
    if [ ! -f "${INDEX_PREFIX_PATH}_disk_beam_search.index" ]; then
      mv $OLD_INDEX_FILE ${INDEX_PREFIX_PATH}_disk_beam_search.index
    fi
    #TODO: Use only one index file
    cp ${GP_PATH}_part_tmp.index ${INDEX_PREFIX_PATH}_disk.index
    cp ${GP_FILE_PATH} ${INDEX_PREFIX_PATH}_partition.bin
  ;;
  search)
    if [ ! -f "${INDEX_PREFIX_PATH}_disk.index" ] && [ ! -f "${INDEX_PREFIX_PATH}_disk_beam_search.index" ]; then
      echo "Disk index not found under $INDEX_DIR. Build it first?"
      exit 1
    fi
    mkdir -p "${INDEX_DIR}/search"
    mkdir -p "${INDEX_DIR}/result"

    # choose the disk index file by settings
    DISK_FILE_PATH=${INDEX_PREFIX_PATH}_disk.index
    if [ $USE_PAGE_SEARCH -eq 1 ]; then
      if [ ! -f ${INDEX_PREFIX_PATH}_partition.bin ]; then
        echo "Partition file not found. Run the script with gp option first."
        exit 1
      fi
      echo "Using Page Search"
    else
      OLD_INDEX_FILE=${INDEX_PREFIX_PATH}_disk_beam_search.index
      if [ -f ${OLD_INDEX_FILE} ]; then
        DISK_FILE_PATH=$OLD_INDEX_FILE
      else
        echo "make sure you have not gp the index file"
      fi
      echo "Using Beam Search"
    fi

    log_arr=()
    case $3 in
      knn)
        for BW in ${BM_LIST[@]}
        do
          for T in ${T_LIST[@]}
          do
            SEARCH_LOG="${INDEX_DIR}/search/search_SQ${USE_SQ}_K${K}_CACHE${CACHE}_BW${BW}_T${T}_MEML${MEM_L}_MEMK${MEM_TOPK}_MEM_USE_FREQ${MEM_USE_FREQ}_PS${USE_PAGE_SEARCH}_USE_RATIO${PS_USE_RATIO}_GP_USE_FREQ${GP_USE_FREQ}_GP_LOCK_NUMS${GP_LOCK_NUMS}_GP_CUT${GP_CUT}.log"
            echo "Searching... log file: ${SEARCH_LOG}"
            sync; echo 3 | sudo tee /proc/sys/vm/drop_caches; ${EXE_PATH}/tests/search_disk_index --data_type $DATA_TYPE \
              --dist_fn $DIST_FN \
              --index_path_prefix $INDEX_PREFIX_PATH \
              --query_file $QUERY_FILE \
              --gt_file $GT_FILE \
              -K $K \
              --result_path "${INDEX_DIR}/result/result" \
              --num_nodes_to_cache $CACHE \
              -T $T \
              -L ${LS} \
              -W $BW \
              --mem_L ${MEM_L} \
              --mem_index_path ${MEM_INDEX_PATH}_index \
              --use_page_search ${USE_PAGE_SEARCH} \
              --use_ratio ${PS_USE_RATIO} \
              --disk_file_path ${DISK_FILE_PATH} \
              --use_sq ${USE_SQ}       > ${SEARCH_LOG} 
            log_arr+=( ${SEARCH_LOG} )
          done
        done
      ;;
      range)
        for BW in ${BM_LIST[@]}
        do
          for T in ${T_LIST[@]}
          do
            SEARCH_LOG="${INDEX_DIR}/search/search_RADIUS${RADIUS}_CACHE${CACHE}_BW${BW}_T${T}_PS${USE_PAGE_SEARCH}_PS_RATIO${PS_USE_RATIO}_ITER_KNN${RS_ITER_KNN_TO_RANGE_SEARCH}_MEM_L${MEM_L}.log"
            echo "Searching... log file: ${SEARCH_LOG}"
            sync; echo 3 | sudo tee /proc/sys/vm/drop_caches; ${EXE_PATH}/tests/range_search_disk_index \
              --data_type $DATA_TYPE \
              --dist_fn $DIST_FN \
              --index_path_prefix $INDEX_PREFIX_PATH \
              --num_nodes_to_cache $CACHE \
              -T $T \
              -W $BW \
              --query_file $QUERY_FILE \
              --gt_file $GT_FILE \
              --range_threshold $RADIUS \
              -L $RS_LS \
              --disk_file_path ${DISK_FILE_PATH} \
              --use_page_search ${USE_PAGE_SEARCH} \
              --iter_knn_to_range_search ${RS_ITER_KNN_TO_RANGE_SEARCH} \
              --use_ratio ${PS_USE_RATIO} \
              --mem_index_path ${MEM_INDEX_PATH}_index \
              --mem_L ${MEM_L} \
              --custom_round_num ${RS_CUSTOM_ROUND} \
              --kicked_size ${KICKED_SIZE} \
              > ${SEARCH_LOG}
            log_arr+=( ${SEARCH_LOG} )
          done
        done
      ;;
      *)
        print_usage_and_exit
      ;;
    esac
    if [ ${#log_arr[@]} -ge 1 ]; then
      TITLES=$(cat ${log_arr[0]} | grep -E "^\s+L\s+")
      for f in "${log_arr[@]}"
      do
        printf "$f\n" | tee -a $SUMMARY_FILE_PATH
        printf "${TITLES}\n" | tee -a $SUMMARY_FILE_PATH
        cat $f | grep -E "([0-9]+(\.[0-9]+\s+)){5,}" | tee -a $SUMMARY_FILE_PATH
        printf "\n\n" >> $SUMMARY_FILE_PATH
      done
    fi
  ;;
  *)
    print_usage_and_exit
  ;;
esac
