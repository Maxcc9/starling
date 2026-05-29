#!/bin/bash
source "${STARLING_CONFIG:-config_local.sh}"

STARLING_INDEX_ROOT="${STARLING_INDEX_ROOT:-/mnt/diskann_data/starling_data/index}"
DISK_PQ_BYTES="${DISK_PQ_BYTES:-0}"
APPEND_REORDER_DATA="${APPEND_REORDER_DATA:-0}"
INDEX_DISK_PQ_SUFFIX=""
if [ "${DISK_PQ_BYTES}" != "0" ]; then
  INDEX_DISK_PQ_SUFFIX="_DPQ${DISK_PQ_BYTES}"
  if [ "${APPEND_REORDER_DATA}" = "1" ] || [ "${APPEND_REORDER_DATA}" = "true" ]; then
    INDEX_DISK_PQ_SUFFIX="${INDEX_DISK_PQ_SUFFIX}_reorder"
  fi
fi
INDEX_EXPERIMENT="${INDEX_EXPERIMENT:-${PREFIX}_starling}"
INDEX_NAME="${INDEX_NAME:-${PREFIX}_R${R}_L${BUILD_L}_B${B}_M${M}${INDEX_DISK_PQ_SUFFIX}}"
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

print_usage_and_exit(){
    echo "Usage: ./unset.sh [compile/index_file/gp/mem_index/freq/sample_file/index_dir/relayout] [release/debug]"
    exit -1;
}
case $1 in 
    compile)
        echo "remove all compiled file."
        rm -rf ../debug ../release
    ;;
    index_file)
        echo "copy the un-gp disk index to disk index"
        OLD_INDEX_FILE=${INDEX_PREFIX_PATH}_disk_beam_search.index
        INDEX_FILE=${INDEX_PREFIX_PATH}_disk.index
        if [ -f $OLD_INDEX_FILE ]; then
            cp $OLD_INDEX_FILE $INDEX_FILE
        else 
            echo "Wrong! make sure you have the old index file copy."
        fi
    ;;
    gp)
        echo "remove gp dir and reset the gp index to no-gp index."
        echo "unset index_file."
        OLD_INDEX_FILE=${INDEX_PREFIX_PATH}_disk_beam_search.index
        INDEX_FILE=${INDEX_PREFIX_PATH}_disk.index
        if [ -f $OLD_INDEX_FILE ]; then
            echo "copy the un-gp disk index to disk index..."
            cp $OLD_INDEX_FILE $INDEX_FILE
        fi
        echo ""
        rm -rf $GP_PATH
        rm -f ${INDEX_PREFIX_PATH}_partition.bin
    ;;
    mem_index)
        echo "remove mem index dir."
        rm -rf ${MEM_INDEX_DIR}
    ;;
    freq)
        echo "remove freq dir."
        rm -rf ${FREQ_DIR}
    ;;
    sample_file)
        echo "remove sample data dir."
        rm -rf ${MEM_SAMPLE_DIR}
    ;;
    index_dir)
        echo "remove index file dir."
        rm -rf ${INDEX_DIR}
    ;;
    relayout)
        case $2 in
            debug)
                cmake -DCMAKE_BUILD_TYPE=Debug .. -B ../debug
                EXE_PATH=../debug
            ;;
            release)
                cmake -DCMAKE_BUILD_TYPE=Release .. -B ../release
                EXE_PATH=../release
            ;;
            *)
                print_usage_and_exit
            ;;
        esac
        pushd $EXE_PATH
        make -j
        popd
    
        echo "will relayout the index file using the gpfile in gp dir."
        echo "unset index_file."
        OLD_INDEX_FILE=${INDEX_PREFIX_PATH}_disk_beam_search.index
        INDEX_FILE=${INDEX_PREFIX_PATH}_disk.index
        if [ ! -f $INDEX_FILE ]; then
            echo "ERRO! no disk index file!"
            exit 1; 
        fi

        if [ ! -f $OLD_INDEX_FILE ]; then
            echo "no old file, will copy the index to old index file."
            cp $INDEX_FILE $OLD_INDEX_FILE
        fi

        if [ ! -d ${GP_DIR} ]; then
            echo "ERRO! no gp dir, maybe you should run './run_benchmark.sh release gp knn' first."
            exit 1;
        fi

        if [ ! -f ${GP_PATH}_part.bin ]; then
            echo "ERRO! no gp file in gp dir, maybe you should run './run_benchmark.sh release gp knn' first."
            exit 1;
        fi
        echo ${EXE_PATH}
        time ${EXE_PATH}/tests/utils/index_relayout ${OLD_INDEX_FILE} ${GP_PATH}_part.bin > "${GP_DIR}/relayout.log"
        cp ${GP_PATH}_part_tmp.index ${INDEX_PREFIX_PATH}_disk.index
        cp ${GP_PATH}_part.bin ${INDEX_PREFIX_PATH}_partition.bin
    ;;
    search)
        echo rm "${INDEX_DIR}/search"
        rm "${INDEX_DIR}"/search/*
    ;;
esac
