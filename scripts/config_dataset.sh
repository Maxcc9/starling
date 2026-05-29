#!/bin/sh

# Switch dataset in the config_local.sh file by calling the desired function

DISKANN_DATA_ROOT=${DISKANN_DATA_ROOT:-/home/gt/research/DiskANN/data}

#################
#   BIGANN10M   #
#################
dataset_bigann10M() {
  BASE_PATH=/data/datasets/BIGANN/base.10M.u8bin
  QUERY_FILE=/data/datasets/BIGANN/query.public.10K.128.u8bin
  GT_FILE=/data/datasets/BIGANN/bigann-10M-gt.bin 
  PREFIX=bigann_10m
  DATA_TYPE=uint8
  DIST_FN=l2
  B=0.3
  K=10
  DATA_DIM=128
  DATA_N=10000000
}

#####################
#   Local DiskANN   #
#####################
dataset_siftsmall() {
  BASE_PATH=${DISKANN_DATA_ROOT}/siftsmall/siftsmall_base.bin
  QUERY_FILE=${DISKANN_DATA_ROOT}/siftsmall/siftsmall_query.bin
  GT_FILE=${DISKANN_DATA_ROOT}/siftsmall/siftsmall_groundtruth.bin
  PREFIX=siftsmall
  DATA_TYPE=float
  DIST_FN=l2
  B=0.00003
  K=10
  DATA_DIM=128
  DATA_N=10000
}

dataset_sift1m() {
  BASE_PATH=${DISKANN_DATA_ROOT}/sift1m/sift1m_base.bin
  QUERY_FILE=${DISKANN_DATA_ROOT}/sift1m/sift1m_query.bin
  GT_FILE=${DISKANN_DATA_ROOT}/sift1m/sift1m_groundtruth.bin
  PREFIX=sift1m
  DATA_TYPE=float
  DIST_FN=l2
  B=2
  K=10
  DATA_DIM=128
  DATA_N=1000000
}

dataset_deep1m() {
  BASE_PATH=${DISKANN_DATA_ROOT}/deep1m/deep1m_base.bin
  QUERY_FILE=${DISKANN_DATA_ROOT}/deep1m/deep1m_query.bin
  GT_FILE=${DISKANN_DATA_ROOT}/deep1m/deep1m_groundtruth.bin
  PREFIX=deep1m
  DATA_TYPE=float
  DIST_FN=l2
  B=2
  K=10
  DATA_DIM=96
  DATA_N=1000000
}

dataset_gist1m() {
  BASE_PATH=${DISKANN_DATA_ROOT}/gist1m/gist1m_base.bin
  QUERY_FILE=${DISKANN_DATA_ROOT}/gist1m/gist1m_query.bin
  GT_FILE=${DISKANN_DATA_ROOT}/gist1m/gist1m_groundtruth.bin
  PREFIX=gist1m
  DATA_TYPE=float
  DIST_FN=l2
  B=8
  K=10
  DATA_DIM=960
  DATA_N=1000000
  DISK_PQ_BYTES=256
}

dataset_text2image1m() {
  BASE_PATH=${DISKANN_DATA_ROOT}/text2image1m/text2image1m_base.bin
  QUERY_FILE=${DISKANN_DATA_ROOT}/text2image1m/text2image1m_query.bin
  GT_FILE=${DISKANN_DATA_ROOT}/text2image1m/text2image1m_groundtruth.bin
  PREFIX=text2image1m
  DATA_TYPE=float
  DIST_FN=mips
  B=2
  K=10
  DATA_DIM=200
  DATA_N=1000000
}

dataset_sift100m() {
  BASE_PATH=${DISKANN_DATA_ROOT}/sift100m/sift100m_base.bin
  QUERY_FILE=${DISKANN_DATA_ROOT}/sift100m/sift100m_query.bin
  GT_FILE=${DISKANN_DATA_ROOT}/sift100m/sift100m_groundtruth.bin
  PREFIX=sift100m
  DATA_TYPE=uint8
  DIST_FN=l2
  B=2
  K=10
  DATA_DIM=128
  DATA_N=100000000
}

dataset_deep100m() {
  BASE_PATH=${DISKANN_DATA_ROOT}/deep100m/deep100m_base.bin
  QUERY_FILE=${DISKANN_DATA_ROOT}/deep100m/deep100m_query.bin
  GT_FILE=${DISKANN_DATA_ROOT}/deep100m/deep100m_groundtruth.bin
  PREFIX=deep100m
  DATA_TYPE=float
  DIST_FN=l2
  B=8
  K=10
  DATA_DIM=96
  DATA_N=100000000
}

dataset_spacev100m() {
  BASE_PATH=${DISKANN_DATA_ROOT}/spacev100m/spacev100m_base.bin
  QUERY_FILE=${DISKANN_DATA_ROOT}/spacev100m/spacev100m_query.bin
  GT_FILE=${DISKANN_DATA_ROOT}/spacev100m/spacev100m_groundtruth.bin
  PREFIX=spacev100m
  DATA_TYPE=uint8
  DIST_FN=l2
  B=2
  K=10
  DATA_DIM=100
  DATA_N=100000000
}

starling_profile_small() {
  R=16
  BUILD_L=32
  M=1
  BUILD_T=8
  MEM_R=16
  MEM_BUILD_L=32
  MEM_RAND_SAMPLING_RATE=0.01
  MEM_FREQ_USE_RATE=0.01
  FREQ_L=32
  FREQ_T=8
  GP_TIMES=5
  GP_T=8
  BM_LIST=(2)
  T_LIST=(8)
  LS="10 20 32"
  RS_LS="32"
}

starling_profile_1m() {
  R=64
  BUILD_L=100
  M=2
  BUILD_T=16
  MEM_R=64
  MEM_BUILD_L=100
  MEM_RAND_SAMPLING_RATE=0.01
  MEM_FREQ_USE_RATE=0.01
  FREQ_L=100
  FREQ_T=16
  GP_TIMES=8
  GP_T=16
  BM_LIST=(4)
  T_LIST=(8 16)
  LS="50 100 150 200"
  RS_LS="80 100"
}

starling_profile_gist1m() {
  starling_profile_1m
  M=8
  BUILD_T=16
  MEM_R=64
  MEM_BUILD_L=100
  LS="100 150 200 300"
}

starling_profile_100m() {
  R=128
  BUILD_L=300
  M=40
  BUILD_T=32
  MEM_R=64
  MEM_BUILD_L=128
  MEM_RAND_SAMPLING_RATE=0.001
  MEM_FREQ_USE_RATE=0.001
  FREQ_L=150
  FREQ_T=32
  GP_TIMES=16
  GP_T=32
  BM_LIST=(4)
  T_LIST=(8 16 32)
  LS="50 100 150 200 300"
  RS_LS="100 150"
}

apply_starling_dataset_profile() {
  case "$PREFIX" in
    siftsmall)
      starling_profile_small
    ;;
    gist1m)
      starling_profile_gist1m
    ;;
    sift100m|deep100m|spacev100m)
      starling_profile_100m
    ;;
    *)
      starling_profile_1m
    ;;
  esac
  FREQ_QUERY_FILE=$QUERY_FILE
}
