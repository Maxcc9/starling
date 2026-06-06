// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include "logger.h"
#include "pq_flash_index.h"
#include <malloc.h>
#include "percentile_stats.h"

#include <omp.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <thread>
#include "distance.h"
#include "exceptions.h"
#include "parameters.h"
#include "pq_flash_index_utils.h"
#include "timer.h"
#include "utils.h"

#include "cosine_similarity.h"
#include "tsl/robin_set.h"

namespace diskann {
  template<typename T>
  void PQFlashIndex<T>::init_node_visit_counter() {
    this->node_visit_counter.clear();
    this->node_visit_counter.resize(this->num_points);
    for (_u32 i = 0; i < node_visit_counter.size(); i++) {
      this->node_visit_counter[i].first = i;
      this->node_visit_counter[i].second = 0;
    }
  }

  template<typename T>
  PQFlashIndex<T>::PQFlashIndex(std::shared_ptr<AlignedFileReader> &fileReader,
                                const bool use_page_search,
                                diskann::Metric                     m, bool use_sq)
      : reader(fileReader), metric(m) {
    if (m == diskann::Metric::COSINE || m == diskann::Metric::INNER_PRODUCT) {
      if (std::is_floating_point<T>::value) {
        diskann::cout << "Cosine metric chosen for (normalized) float data."
                         "Changing distance to L2 to boost accuracy."
                      << std::endl;
        m = diskann::Metric::L2;
      } else {
        diskann::cerr << "WARNING: Cannot normalize integral data types."
                      << " This may result in erroneous results or poor recall."
                      << " Consider using L2 distance with integral data types."
                      << std::endl;
      }
    }

    this->dist_cmp.reset(diskann::get_distance_function<T>(m));
    this->dist_cmp_float.reset(diskann::get_distance_function<float>(m));
    this->use_page_search_ = use_page_search;
    this->use_sq_ = use_sq;
  }

  template<typename T>
  PQFlashIndex<T>::~PQFlashIndex() {
#ifndef EXEC_ENV_OLS
    if (data != nullptr) {
      delete[] data;
    }
#endif

    if (centroid_data != nullptr)
      aligned_free(centroid_data);
    // delete backing bufs for nhood and coord cache
    if (nhood_cache_buf != nullptr) {
      delete[] nhood_cache_buf;
      diskann::aligned_free(coord_cache_buf);
    }

    if (load_flag) {
      this->destroy_thread_data();
      reader->close();
    }
    if(use_sq_){
      free(this->frac);
      free(this->mins);
    }
  }

  template<typename T>
  void PQFlashIndex<T>::setup_thread_data(_u64 nthreads) {
    diskann::cout << "Setting up thread-specific contexts for nthreads: "
                  << nthreads << std::endl;
// omp parallel for to generate unique thread IDs
#pragma omp parallel for num_threads((int) nthreads)
    for (_s64 thread = 0; thread < (_s64) nthreads; thread++) {
#pragma omp critical
      {
        this->reader->register_thread();
        IOContext &     ctx = this->reader->get_ctx();
        QueryScratch<T> scratch;
        _u64 coord_alloc_size = ROUND_UP(sizeof(T) * MAX_N_CMPS * this->aligned_dim, 256);
        diskann::alloc_aligned((void **) &scratch.coord_scratch,
                               coord_alloc_size, 256);
        diskann::alloc_aligned((void **) &scratch.sector_scratch,
                               (_u64) MAX_N_SECTOR_READS * (_u64) SECTOR_LEN,
                               SECTOR_LEN);
        diskann::alloc_aligned(
            (void **) &scratch.aligned_pq_coord_scratch,
            (_u64) MAX_GRAPH_DEGREE * (_u64) MAX_PQ_CHUNKS * sizeof(_u8), 256);
        diskann::alloc_aligned((void **) &scratch.aligned_pqtable_dist_scratch,
                               256 * (_u64) MAX_PQ_CHUNKS * sizeof(float), 256);
        diskann::alloc_aligned((void **) &scratch.aligned_dist_scratch,
                               (_u64) MAX_GRAPH_DEGREE * sizeof(float), 256);
        diskann::alloc_aligned((void **) &scratch.aligned_query_T,
                               this->aligned_dim * sizeof(T), 8 * sizeof(T));
        diskann::alloc_aligned((void **) &scratch.aligned_query_float,
                               this->aligned_dim * sizeof(float),
                               8 * sizeof(float));
        scratch.visited = new tsl::robin_set<_u64>(4096);
        scratch.page_visited = new tsl::robin_set<unsigned>(4096);

        memset(scratch.coord_scratch, 0, coord_alloc_size);
        memset(scratch.aligned_query_T, 0, this->aligned_dim * sizeof(T));
        memset(scratch.aligned_query_float, 0,
               this->aligned_dim * sizeof(float));

        ThreadData<T> data;
        data.ctx = ctx;
        data.scratch = scratch;
        this->thread_data.push(data);
      }
    }
    load_flag = true;
  }

  template<typename T>
  void PQFlashIndex<T>::destroy_thread_data() {
    diskann::cout << "Clearing scratch" << std::endl;
    assert(this->thread_data.size() == this->max_nthreads);
    while (this->thread_data.size() > 0) {
      ThreadData<T> data = this->thread_data.pop();
      while (data.scratch.sector_scratch == nullptr) {
        this->thread_data.wait_for_push_notify();
        data = this->thread_data.pop();
      }
      auto &scratch = data.scratch;
      diskann::aligned_free((void *) scratch.coord_scratch);
      diskann::aligned_free((void *) scratch.sector_scratch);
      diskann::aligned_free((void *) scratch.aligned_pq_coord_scratch);
      diskann::aligned_free((void *) scratch.aligned_pqtable_dist_scratch);
      diskann::aligned_free((void *) scratch.aligned_dist_scratch);
      diskann::aligned_free((void *) scratch.aligned_query_float);
      diskann::aligned_free((void *) scratch.aligned_query_T);

      delete scratch.visited;
      if (this->use_page_search_) delete scratch.page_visited;
    }
    this->reader->deregister_all_threads();
  }

  template<typename T>
  void PQFlashIndex<T>::load_cache_list(std::vector<uint32_t> &node_list) {
    diskann::cout << "Loading the cache list into memory.." << std::flush;
    _u64 num_cached_nodes = node_list.size();

    // borrow thread data
    ThreadData<T> this_thread_data = this->thread_data.pop();
    while (this_thread_data.scratch.sector_scratch == nullptr) {
      this->thread_data.wait_for_push_notify();
      this_thread_data = this->thread_data.pop();
    }

    IOContext &ctx = this_thread_data.ctx;

    nhood_cache_buf = new unsigned[num_cached_nodes * (max_degree + 1)];
    memset(nhood_cache_buf, 0, num_cached_nodes * (max_degree + 1));

    _u64 coord_cache_buf_len = num_cached_nodes * aligned_dim;
    diskann::alloc_aligned((void **) &coord_cache_buf,
                           coord_cache_buf_len * sizeof(T), 8 * sizeof(T));
    memset(coord_cache_buf, 0, coord_cache_buf_len * sizeof(T));

    size_t BLOCK_SIZE = 8;
    size_t num_blocks = DIV_ROUND_UP(num_cached_nodes, BLOCK_SIZE);
    for (_u64 block = 0; block < num_blocks; block++) {
      _u64 start_idx = block * BLOCK_SIZE;
      _u64 end_idx = (std::min)(num_cached_nodes, (block + 1) * BLOCK_SIZE);
      std::vector<AlignedRead>             read_reqs;
      std::vector<std::pair<_u32, char *>> nhoods;
      for (_u64 node_idx = start_idx; node_idx < end_idx; node_idx++) {
        auto id = node_list[node_idx];
        std::pair<_u32, char *> fnhood;
        fnhood.first = id;
        char* buf = nullptr;
        alloc_aligned((void **) &buf, SECTOR_LEN, SECTOR_LEN);
        fnhood.second = buf;
        nhoods.push_back(fnhood);
        if(use_page_search_){
            read_reqs.emplace_back(
              (static_cast<_u64>(id2page_[id]+1)) * SECTOR_LEN, SECTOR_LEN,
              fnhood.second);
        }else{
            read_reqs.emplace_back(
              (static_cast<_u64>(NODE_SECTOR_NO(id))) * SECTOR_LEN, SECTOR_LEN,
              fnhood.second);
        }
      }

      reader->read(read_reqs, ctx);

      _u64 node_idx = start_idx;

      for (auto &nhood : nhoods) {
        char* node_buf = nullptr;
        if(use_page_search_){
          char *sector_buf = nhood.second;
          unsigned pid = id2page_[nhood.first];
          for (unsigned j = 0; j < gp_layout_[pid].size(); ++j) {
            unsigned id = gp_layout_[pid][j];
            if (id == nhood.first) {
              node_buf = sector_buf + j * max_node_len;
            }
          }
        }else{
          node_buf = OFFSET_TO_NODE(nhood.second, nhood.first);
        }
        T *   node_coords = OFFSET_TO_NODE_COORDS(node_buf);
        T *   cached_coords = coord_cache_buf + node_idx * aligned_dim;
        memcpy(cached_coords, node_coords, disk_bytes_per_point);
        coord_cache.insert(std::make_pair(nhood.first, cached_coords));

        unsigned *node_nhood = OFFSET_TO_NODE_NHOOD(node_buf);

        auto                        nnbrs = *node_nhood;
        unsigned *                  nbrs = node_nhood + 1;
        std::pair<_u32, unsigned *> cnhood;
        cnhood.first = nnbrs;
        cnhood.second = nhood_cache_buf + node_idx * (max_degree + 1);
        memcpy(cnhood.second, nbrs, nnbrs * sizeof(unsigned));
        nhood_cache.insert(std::make_pair(nhood.first, cnhood));
        aligned_free(nhood.second);
        node_idx++;
      }
    }
    // return thread data
    this->thread_data.push(this_thread_data);
    this->thread_data.push_notify_all();
    diskann::cout << "..done." << std::endl;
  }

#ifdef EXEC_ENV_OLS
  template<typename T>
  void PQFlashIndex<T>::generate_cache_list_from_sample_queries(
      MemoryMappedFiles &files, std::string sample_bin, _u64 l_search,
      _u64 beamwidth, _u64 num_nodes_to_cache, uint32_t nthreads,
      std::vector<uint32_t> &node_list) {
#else
  template<typename T>
  void PQFlashIndex<T>::generate_cache_list_from_sample_queries(
      std::string sample_bin, _u64 l_search, _u64 beamwidth,
      _u64 num_nodes_to_cache, uint32_t nthreads,
      std::vector<uint32_t> &node_list, bool use_pagesearch, const _u32 mem_L) {
#endif
    this->count_visited_nodes = true;
    init_node_visit_counter();

    _u64 sample_num, sample_dim, sample_aligned_dim;
    T *  samples;

#ifdef EXEC_ENV_OLS
    if (files.fileExists(sample_bin)) {
      diskann::load_aligned_bin<T>(files, sample_bin, samples, sample_num,
                                   sample_dim, sample_aligned_dim);
    }
#else
    if (file_exists(sample_bin)) {
      diskann::load_aligned_bin<T>(sample_bin, samples, sample_num, sample_dim,
                                   sample_aligned_dim);
    }
#endif
    else {
      diskann::cerr << "Sample bin file not found. Not generating cache."
                    << std::endl;
      return;
    }

    std::vector<uint64_t> tmp_result_ids_64(sample_num, 0);
    std::vector<float>    tmp_result_dists(sample_num, 0);

    if(use_pagesearch){
      if(use_sq_){
#pragma omp parallel for schedule(dynamic, 1) num_threads(nthreads)
        for (_s64 i = 0; i < (int64_t) sample_num; i++) {
          page_search_sq(
                samples + (i * sample_aligned_dim), 1, mem_L, l_search,
                tmp_result_ids_64.data() + (i * 1),
                tmp_result_dists.data() + (i * 1),
                beamwidth, std::numeric_limits<_u32>::max(), false, 1, nullptr);
        }
      }else{
#pragma omp parallel for schedule(dynamic, 1) num_threads(nthreads)
        for (_s64 i = 0; i < (int64_t) sample_num; i++) {
          page_search(
                samples + (i * sample_aligned_dim), 1, mem_L, l_search,
                tmp_result_ids_64.data() + (i * 1),
                tmp_result_dists.data() + (i * 1),
                beamwidth, std::numeric_limits<_u32>::max(), false, 1, nullptr);
        }
      } 
    }else{
      if(use_sq_){
        std::cout << "current not support diskann sq" << std::endl;
        exit(-1);
      }
#pragma omp parallel for schedule(dynamic, 1) num_threads(nthreads)
      for (_s64 i = 0; i < (int64_t) sample_num; i++) {
        cached_beam_search(samples + (i * sample_aligned_dim), 1, l_search,
                          tmp_result_ids_64.data() + (i * 1),
                          tmp_result_dists.data() + (i * 1),
                          beamwidth, false, nullptr, mem_L);
      }
    }
    std::sort(this->node_visit_counter.begin(), node_visit_counter.end(),
              [](std::pair<_u32, _u32> &left, std::pair<_u32, _u32> &right) {
                return left.second > right.second;
              });
    node_list.clear();
    node_list.shrink_to_fit();
    node_list.reserve(num_nodes_to_cache);
    for (_u64 i = 0; i < num_nodes_to_cache; i++) {
      node_list.push_back(this->node_visit_counter[i].first);
    }
    this->count_visited_nodes = false;

    diskann::aligned_free(samples);
  }

  template<typename T>
  void PQFlashIndex<T>::cache_bfs_levels(_u64 num_nodes_to_cache,
                                         std::vector<uint32_t> &node_list) {
    std::random_device rng;
    std::mt19937       urng(rng());

    node_list.clear();

    // Do not cache more than 10% of the nodes in the index
    _u64 tenp_nodes = (_u64)(std::round(this->num_points * 0.1));
    if (num_nodes_to_cache > tenp_nodes) {
      diskann::cout << "Reducing nodes to cache from: " << num_nodes_to_cache
                    << " to: " << tenp_nodes
                    << "(10 percent of total nodes:" << this->num_points << ")"
                    << std::endl;
      num_nodes_to_cache = tenp_nodes == 0 ? 1 : tenp_nodes;
    }
    diskann::cout << "Caching " << num_nodes_to_cache << "..." << std::endl;

    // borrow thread data
    ThreadData<T> this_thread_data = this->thread_data.pop();
    while (this_thread_data.scratch.sector_scratch == nullptr) {
      this->thread_data.wait_for_push_notify();
      this_thread_data = this->thread_data.pop();
    }

    IOContext &ctx = this_thread_data.ctx;

    std::unique_ptr<tsl::robin_set<unsigned>> cur_level, prev_level;
    cur_level = std::make_unique<tsl::robin_set<unsigned>>();
    prev_level = std::make_unique<tsl::robin_set<unsigned>>();

    for (_u64 miter = 0; miter < num_medoids; miter++) {
      cur_level->insert(medoids[miter]);
    }

    _u64     lvl = 1;
    uint64_t prev_node_list_size = 0;
    while ((node_list.size() + cur_level->size() < num_nodes_to_cache) &&
           cur_level->size() != 0) {
      // swap prev_level and cur_level
      std::swap(prev_level, cur_level);
      // clear cur_level
      cur_level->clear();

      std::vector<unsigned> nodes_to_expand;

      for (const unsigned &id : *prev_level) {
        if (std::find(node_list.begin(), node_list.end(), id) !=
            node_list.end()) {
          continue;
        }
        node_list.push_back(id);
        nodes_to_expand.push_back(id);
      }

      std::shuffle(nodes_to_expand.begin(), nodes_to_expand.end(), urng);

      diskann::cout << "Level: " << lvl << std::flush;
      bool finish_flag = false;

      uint64_t BLOCK_SIZE = 1024;
      uint64_t nblocks = DIV_ROUND_UP(nodes_to_expand.size(), BLOCK_SIZE);
      for (size_t block = 0; block < nblocks && !finish_flag; block++) {
        diskann::cout << "." << std::flush;
        size_t start = block * BLOCK_SIZE;
        size_t end =
            (std::min)((block + 1) * BLOCK_SIZE, nodes_to_expand.size());
        std::vector<AlignedRead>             read_reqs;
        std::vector<std::pair<_u32, char *>> nhoods;
        for (size_t cur_pt = start; cur_pt < end; cur_pt++) {
          char *buf = nullptr;
          alloc_aligned((void **) &buf, SECTOR_LEN, SECTOR_LEN);
          nhoods.push_back(std::make_pair(nodes_to_expand[cur_pt], buf));
          AlignedRead read;
          read.len = SECTOR_LEN;
          read.buf = buf;
          if(this->use_page_search_){
            read.offset= SECTOR_LEN * (id2page_[nodes_to_expand[cur_pt]] + 1);
          }else{
            read.offset = NODE_SECTOR_NO(nodes_to_expand[cur_pt]) * SECTOR_LEN;
          }
          read_reqs.push_back(read);
        }

        // issue read requests
        reader->read(read_reqs, ctx);

        // process each nhood buf
        for (_u32 i = 0; i < read_reqs.size(); i++) {
#if defined(_WINDOWS) && \
    defined(USE_BING_INFRA)  // this block is to handle read failures in
                             // production settings
          if ((*ctx.m_pRequestsStatus)[i] != IOContext::READ_SUCCESS) {
            continue;
          }
#endif
          auto &nhood = nhoods[i];
          char* node_buf = nullptr;

          // insert node coord into coord_cache
          if(use_page_search_){
            char *sector_buf = nhood.second;
            unsigned pid = id2page_[nhood.first];
            for (unsigned j = 0; j < gp_layout_[pid].size(); ++j) {
              unsigned id = gp_layout_[pid][j];
              if (id == nhood.first) {
                node_buf = sector_buf + j * max_node_len;
              }
            }
          }else{
            node_buf = OFFSET_TO_NODE(nhood.second, nhood.first);
          }
          unsigned *node_nhood = OFFSET_TO_NODE_NHOOD(node_buf);
          _u64      nnbrs = (_u64) *node_nhood;
          unsigned *nbrs = node_nhood + 1;
          // explore next level
          for (_u64 j = 0; j < nnbrs && !finish_flag; j++) {
            if (std::find(node_list.begin(), node_list.end(), nbrs[j]) ==
                node_list.end()) {
              cur_level->insert(nbrs[j]);
            }
            if (cur_level->size() + node_list.size() >= num_nodes_to_cache) {
              finish_flag = true;
            }
          }
          aligned_free(nhood.second);
        }
      }

      diskann::cout << ". #nodes: " << node_list.size() - prev_node_list_size
                    << ", #nodes thus far: " << node_list.size() << std::endl;
      prev_node_list_size = node_list.size();
      lvl++;
    }

    std::vector<uint32_t> cur_level_node_list;
    for (const unsigned &p : *cur_level)
      cur_level_node_list.push_back(p);

    std::shuffle(cur_level_node_list.begin(), cur_level_node_list.end(), urng);
    size_t residual = num_nodes_to_cache - node_list.size();

    for (size_t i = 0; i < (std::min)(residual, cur_level_node_list.size());
         i++)
      node_list.push_back(cur_level_node_list[i]);

    diskann::cout << "Level: " << lvl << std::flush;
    diskann::cout << ". #nodes: " << node_list.size() - prev_node_list_size
                  << ", #nodes thus far: " << node_list.size() << std::endl;

    // return thread data
    this->thread_data.push(this_thread_data);
    this->thread_data.push_notify_all();

    diskann::cout << "done" << std::endl;
  }

  template<typename T>
  void PQFlashIndex<T>::use_medoids_data_as_centroids() {
    if (centroid_data != nullptr)
      aligned_free(centroid_data);
    alloc_aligned(((void **) &centroid_data),
                  num_medoids * aligned_dim * sizeof(float), 32);
    std::memset(centroid_data, 0, num_medoids * aligned_dim * sizeof(float));

    // borrow ctx
    ThreadData<T> data = this->thread_data.pop();
    while (data.scratch.sector_scratch == nullptr) {
      this->thread_data.wait_for_push_notify();
      data = this->thread_data.pop();
    }
    IOContext &ctx = data.ctx;
    diskann::cout << "Loading centroid data from medoids vector data of "
                  << num_medoids << " medoid(s)" << std::endl;
    for (uint64_t cur_m = 0; cur_m < num_medoids; cur_m++) {
      auto medoid = medoids[cur_m];
      // read medoid nhood
      char *medoid_buf = nullptr;
      alloc_aligned((void **) &medoid_buf, SECTOR_LEN, SECTOR_LEN);
      std::vector<AlignedRead> medoid_read(1);
      medoid_read[0].len = SECTOR_LEN;
      medoid_read[0].buf = medoid_buf;
      medoid_read[0].offset = NODE_SECTOR_NO(medoid) * SECTOR_LEN;
      reader->read(medoid_read, ctx);

      // all data about medoid
      char *medoid_node_buf = OFFSET_TO_NODE(medoid_buf, medoid);

      // add medoid coords to `coord_cache`
      T *medoid_coords = new T[data_dim];
      T *medoid_disk_coords = OFFSET_TO_NODE_COORDS(medoid_node_buf);
      memcpy(medoid_coords, medoid_disk_coords, disk_bytes_per_point);

      if (!use_disk_index_pq) {
        for (uint32_t i = 0; i < data_dim; i++)
          centroid_data[cur_m * aligned_dim + i] = medoid_coords[i];
      } else {
        disk_pq_table.inflate_vector((_u8 *) medoid_coords,
                                     (centroid_data + cur_m * aligned_dim));
      }

      aligned_free(medoid_buf);
      delete[] medoid_coords;
    }

    // return ctx
    this->thread_data.push(data);
    this->thread_data.push_notify_all();
  }

  template<typename T>
  void PQFlashIndex<T>::load_mem_index(Metric metric, const size_t query_dim, 
      const std::string& mem_index_path, const _u32 num_threads,
      const _u32 mem_L) {
      if (mem_index_path.empty()) {
        diskann::cerr << "mem_index_path is needed" << std::endl;
        exit(1);
      }
      mem_index_ = std::make_unique<diskann::Index<T, uint32_t>>(metric, query_dim, 0, false, true);
      mem_index_->load(mem_index_path.c_str(), num_threads, mem_L);
  }

#ifdef EXEC_ENV_OLS
  template<typename T>
  int PQFlashIndex<T>::load(MemoryMappedFiles &files, uint32_t num_threads,
                            const char *index_prefix) {
#else
  template<typename T>
  int PQFlashIndex<T>::load(uint32_t num_threads, const char *index_prefix, const std::string& disk_index_path) {
#endif
    std::string pq_table_bin = std::string(index_prefix) + "_pq_pivots.bin";
    std::string pq_compressed_vectors =
        std::string(index_prefix) + "_pq_compressed.bin";
    std::string disk_index_file = disk_index_path; 
    std::string medoids_file = std::string(disk_index_file) + "_medoids.bin";
    std::string centroids_file =
        std::string(disk_index_file) + "_centroids.bin";

    size_t pq_file_dim, pq_file_num_centroids;
#ifdef EXEC_ENV_OLS
    get_bin_metadata(files, pq_table_bin, pq_file_num_centroids, pq_file_dim,
                     METADATA_SIZE);
#else
    get_bin_metadata(pq_table_bin, pq_file_num_centroids, pq_file_dim,
                     METADATA_SIZE);
#endif

    this->disk_index_file = disk_index_file;

    if (pq_file_num_centroids != 256) {
      diskann::cout << "Error. Number of PQ centroids is not 256. Exitting."
                    << std::endl;
      return -1;
    }

    this->data_dim = pq_file_dim;
    // will reset later if we use PQ on disk
    this->disk_data_dim = this->data_dim;
    // will change later if we use PQ on disk or if we are using
    // inner product without PQ
    this->disk_bytes_per_point = this->data_dim * sizeof(T);
    if(use_sq_){
      this->disk_bytes_per_point = this->data_dim * sizeof(uint8_t);
      std::cout << "disk bytes per point "<<this->disk_bytes_per_point << std::endl;
    }
    this->aligned_dim = ROUND_UP(pq_file_dim, 8);

    size_t npts_u64, nchunks_u64;
#ifdef EXEC_ENV_OLS
    diskann::load_bin<_u8>(files, pq_compressed_vectors, this->data, npts_u64,
                           nchunks_u64);
#else
    diskann::load_bin<_u8>(pq_compressed_vectors, this->data, npts_u64,
                           nchunks_u64);
#endif

    this->num_points = npts_u64;
    this->n_chunks = nchunks_u64;

#ifdef EXEC_ENV_OLS
    pq_table.load_pq_centroid_bin(files, pq_table_bin.c_str(), nchunks_u64);
#else
    pq_table.load_pq_centroid_bin(pq_table_bin.c_str(), nchunks_u64);
#endif

    diskann::cout
        << "Loaded PQ centroids and in-memory compressed vectors. #points: "
        << num_points << " #dim: " << data_dim
        << " #aligned_dim: " << aligned_dim << " #chunks: " << n_chunks
        << std::endl;

    if (n_chunks > MAX_PQ_CHUNKS) {
      std::stringstream stream;
      stream << "Error loading index. Ensure that max PQ bytes for in-memory "
                "PQ data does not exceed "
             << MAX_PQ_CHUNKS << std::endl;
      throw diskann::ANNException(stream.str(), -1, __FUNCSIG__, __FILE__,
                                  __LINE__);
    }

    std::string disk_pq_pivots_path = this->disk_index_file + "_pq_pivots.bin";
    if (file_exists(disk_pq_pivots_path)) {
      use_disk_index_pq = true;
#ifdef EXEC_ENV_OLS
      // giving 0 chunks to make the pq_table infer from the
      // chunk_offsets file the correct value
      disk_pq_table.load_pq_centroid_bin(files, disk_pq_pivots_path.c_str(), 0);
#else
      // giving 0 chunks to make the pq_table infer from the
      // chunk_offsets file the correct value
      disk_pq_table.load_pq_centroid_bin(disk_pq_pivots_path.c_str(), 0);
#endif
      disk_pq_n_chunks = disk_pq_table.get_num_chunks();
      disk_bytes_per_point =
          disk_pq_n_chunks *
          sizeof(_u8);  // revising disk_bytes_per_point since DISK PQ is used.
      std::cout << "Disk index uses PQ data compressed down to "
                << disk_pq_n_chunks << " bytes per point." << std::endl;
    }

// read index metadata
#ifdef EXEC_ENV_OLS
    // This is a bit tricky. We have to read the header from the
    // disk_index_file. But  this is now exclusively a preserve of the
    // DiskPriorityIO class. So, we need to estimate how many
    // bytes are needed to store the header and read in that many using our
    // 'standard' aligned file reader approach.
    reader->open(disk_index_file);
    this->setup_thread_data(num_threads);
    this->max_nthreads = num_threads;

    char *                   bytes = getHeaderBytes();
    ContentBuf               buf(bytes, HEADER_SIZE);
    std::basic_istream<char> index_metadata(&buf);
#else
    std::ifstream index_metadata(disk_index_file, std::ios::binary);
#endif
    _u32 nr, nc;  // metadata itself is stored as bin format (nr is number of
                  // metadata, nc should be 1)
    READ_U32(index_metadata, nr);
    READ_U32(index_metadata, nc);

    _u64 disk_nnodes;
    _u64 disk_ndims;  // can be disk PQ dim if disk_PQ is set to true
    READ_U64(index_metadata, disk_nnodes);
    READ_U64(index_metadata, disk_ndims);

    if (disk_nnodes != num_points) {
      diskann::cout << "Mismatch in #points for compressed data file and disk "
                       "index file: "
                    << disk_nnodes << " vs " << num_points << std::endl;
      return -1;
    }

    size_t medoid_id_on_file;
    READ_U64(index_metadata, medoid_id_on_file);
    READ_U64(index_metadata, max_node_len);
    READ_U64(index_metadata, nnodes_per_sector);
    max_degree = ((max_node_len - disk_bytes_per_point) / sizeof(unsigned)) - 1;

    std::cout << "max node len "<<max_node_len <<" disk bytes "<<disk_bytes_per_point << std::endl;
    if (max_degree > MAX_GRAPH_DEGREE) {
      std::stringstream stream;
      stream << "Error loading index. Ensure that max graph degree (R) does "
                "not exceed "
             << MAX_GRAPH_DEGREE << std::endl;
      throw diskann::ANNException(stream.str(), -1, __FUNCSIG__, __FILE__,
                                  __LINE__);
    }

    // setting up concept of frozen points in disk index for streaming-DiskANN
    READ_U64(index_metadata, this->num_frozen_points);
    _u64 file_frozen_id;
    READ_U64(index_metadata, file_frozen_id);
    if (this->num_frozen_points == 1)
      this->frozen_location = file_frozen_id;
    if (this->num_frozen_points == 1) {
      diskann::cout << " Detected frozen point in index at location "
                    << this->frozen_location
                    << ". Will not output it at search time." << std::endl;
    }

    READ_U64(index_metadata, this->reorder_data_exists);
    if (this->reorder_data_exists) {
      if (this->use_disk_index_pq == false) {
        throw ANNException(
            "Reordering is designed for used with disk PQ compression option",
            -1, __FUNCSIG__, __FILE__, __LINE__);
      }
      READ_U64(index_metadata, this->reorder_data_start_sector);
      READ_U64(index_metadata, this->ndims_reorder_vecs);
      READ_U64(index_metadata, this->nvecs_per_sector);
    }

    diskann::cout << "Disk-Index File Meta-data: ";
    diskann::cout << "# nodes per sector: " << nnodes_per_sector;
    diskann::cout << ", max node len (bytes): " << max_node_len;
    diskann::cout << ", max node degree: " << max_degree << std::endl;

#ifdef EXEC_ENV_OLS
    delete[] bytes;
#else
    index_metadata.close();
#endif

  if (use_page_search_) {
    this->load_partition_data(index_prefix, nnodes_per_sector, num_points);
  }
  if(use_sq_){
    float* maxs = (float*)aligned_alloc(32, aligned_dim * sizeof(float));
    this->mins =(float*)aligned_alloc(32, aligned_dim * sizeof(float));
    this->frac = maxs;
    uint32_t max_min_dims= 0, tmp = 0;
    std::string max_min_file = std::string(index_prefix)+"_sq_max_min.bin";
    auto max_min_reader = std::ifstream(max_min_file);
    max_min_reader.read((char*)&max_min_dims, 4);
    max_min_reader.read((char*)&tmp, 4);
    std::cout << "max min file "<<max_min_file<< " dims "<<max_min_dims << std::endl;
    max_min_reader.read((char*)maxs, max_min_dims/2 * sizeof(float));
    max_min_reader.read((char*)mins, max_min_dims/2 * sizeof(float));
    for(uint32_t i=0; i < max_min_dims/2; i++){
      this->frac[i] = (maxs[i] - this->mins[i]) / std::numeric_limits<uint8_t>::max();
    }
    for(uint32_t i=max_min_dims/2; i<aligned_dim; i++){
      this->frac[i] = 0;
      this->mins[i] = 0; 
    }
  }

#ifndef EXEC_ENV_OLS
    // open AlignedFileReader handle to index_file
    std::string index_fname(disk_index_file);
    reader->open(index_fname);
    this->setup_thread_data(num_threads);
    this->max_nthreads = num_threads;

#endif

#ifdef EXEC_ENV_OLS
    if (files.fileExists(medoids_file)) {
      size_t tmp_dim;
      diskann::load_bin<uint32_t>(files, medoids_file, medoids, num_medoids,
                                  tmp_dim);
#else
    if (file_exists(medoids_file)) {
      size_t tmp_dim;
      diskann::load_bin<uint32_t>(medoids_file, medoids, num_medoids, tmp_dim);
#endif

      if (tmp_dim != 1) {
        std::stringstream stream;
        stream << "Error loading medoids file. Expected bin format of m times "
                  "1 vector of uint32_t."
               << std::endl;
        throw diskann::ANNException(stream.str(), -1, __FUNCSIG__, __FILE__,
                                    __LINE__);
      }
#ifdef EXEC_ENV_OLS
      if (!files.fileExists(centroids_file)) {
#else
      if (!file_exists(centroids_file)) {
#endif
        diskann::cout
            << "Centroid data file not found. Using corresponding vectors "
               "for the medoids "
            << std::endl;
        use_medoids_data_as_centroids();
      } else {
        size_t num_centroids, aligned_tmp_dim;
#ifdef EXEC_ENV_OLS
        diskann::load_aligned_bin<float>(files, centroids_file, centroid_data,
                                         num_centroids, tmp_dim,
                                         aligned_tmp_dim);
#else
        diskann::load_aligned_bin<float>(centroids_file, centroid_data,
                                         num_centroids, tmp_dim,
                                         aligned_tmp_dim);
#endif
        if (aligned_tmp_dim != aligned_dim || num_centroids != num_medoids) {
          std::stringstream stream;
          stream << "Error loading centroids data file. Expected bin format of "
                    "m times data_dim vector of float, where m is number of "
                    "medoids "
                    "in medoids file.";
          diskann::cerr << stream.str() << std::endl;
          throw diskann::ANNException(stream.str(), -1, __FUNCSIG__, __FILE__,
                                      __LINE__);
        }
      }
    } else {
      num_medoids = 1;
      medoids = new uint32_t[1];
      medoids[0] = (_u32)(medoid_id_on_file);
      use_medoids_data_as_centroids();
    }

    std::string norm_file = std::string(disk_index_file) + "_max_base_norm.bin";

    if (file_exists(norm_file) && metric == diskann::Metric::INNER_PRODUCT) {
      _u64   dumr, dumc;
      float *norm_val;
      diskann::load_bin<float>(norm_file, norm_val, dumr, dumc);
      this->max_base_norm = norm_val[0];
      std::cout << "Setting re-scaling factor of base vectors to "
                << this->max_base_norm << std::endl;
      delete[] norm_val;
    }
    diskann::cout << "done.." << std::endl;
    return 0;
  }

#ifdef USE_BING_INFRA
  bool getNextCompletedRequest(const IOContext &ctx, size_t size,
                               int &completedIndex) {
    bool waitsRemaining = false;
    for (int i = 0; i < size; i++) {
      auto ithStatus = (*ctx.m_pRequestsStatus)[i];
      if (ithStatus == IOContext::Status::READ_SUCCESS) {
        completedIndex = i;
        return true;
      } else if (ithStatus == IOContext::Status::READ_WAIT) {
        waitsRemaining = true;
      }
    }
    completedIndex = -1;
    return waitsRemaining;
  }
#endif

  template<typename T>
  void PQFlashIndex<T>::cached_beam_search(const T *query1, const _u64 k_search,
                                           const _u64 l_search, _u64 *indices,
                                           float *     distances,
                                           const _u64  beam_width,
                                           const bool  use_reorder_data,
                                           QueryStats *stats,
                                           const _u32 mem_L,
                                           const _u32 mem_search_L,
                                           const _u32 mem_seed_count,
                                           const float pfm_theta,
                                           const float divergence_k,
                                           const float ecg_alpha,
                                           const unsigned ecg_min_hops,
                                           const float ecg_pq_guard,
                                           const bool beam_page_aware,
                                           const float beam_page_ratio,
                                           const unsigned beam_page_max_extra_nodes,
                                           const bool beam_page_adaptive_extra,
                                           const unsigned beam_page_easy_extra_nodes,
                                           const unsigned beam_page_hard_extra_nodes,
                                           const float beam_page_adaptive_ratio_threshold,
                                           const unsigned topk_stability_patience,
                                           const bool collect_query_telemetry) {
    cached_beam_search(query1, k_search, l_search, indices, distances,
                       beam_width, std::numeric_limits<_u32>::max(),
                       use_reorder_data, stats, mem_L, mem_search_L, mem_seed_count,
                       pfm_theta, divergence_k,
                       ecg_alpha, ecg_min_hops, ecg_pq_guard,
                       beam_page_aware, beam_page_ratio, beam_page_max_extra_nodes,
                       beam_page_adaptive_extra, beam_page_easy_extra_nodes,
                       beam_page_hard_extra_nodes, beam_page_adaptive_ratio_threshold,
                       topk_stability_patience,
                       collect_query_telemetry);
  }

  template<typename T>
  void PQFlashIndex<T>::cached_beam_search(
      const T *query1, const _u64 k_search, const _u64 l_search, _u64 *indices,
      float *distances, const _u64 beam_width, const _u32 io_limit,
      const bool use_reorder_data, QueryStats *stats, const _u32 mem_L,
      const _u32 mem_search_L, const _u32 mem_seed_count,
      const float pfm_theta, const float divergence_k,
      const float ecg_alpha, const unsigned ecg_min_hops,
      const float ecg_pq_guard,
      const bool beam_page_aware, const float beam_page_ratio,
      const unsigned beam_page_max_extra_nodes,
      const bool beam_page_adaptive_extra,
      const unsigned beam_page_easy_extra_nodes,
      const unsigned beam_page_hard_extra_nodes,
      const float beam_page_adaptive_ratio_threshold,
      const unsigned topk_stability_patience,
      const bool collect_query_telemetry) {
    ThreadData<T> data = this->thread_data.pop();
    while (data.scratch.sector_scratch == nullptr) {
      this->thread_data.wait_for_push_notify();
      data = this->thread_data.pop();
    }

    if (beam_width > MAX_N_SECTOR_READS)
      throw ANNException("Beamwidth can not be higher than MAX_N_SECTOR_READS",
                         -1, __FUNCSIG__, __FILE__, __LINE__);

    // copy query to thread specific aligned and allocated memory (for distance
    // calculations we need aligned data)
    float        query_norm = 0;
    const T *    query = data.scratch.aligned_query_T;
    const float *query_float = data.scratch.aligned_query_float;

    uint32_t query_dim = metric == diskann::Metric::INNER_PRODUCT ? this-> data_dim - 1: this-> data_dim;

    for (uint32_t i = 0; i < query_dim; i++) {
      data.scratch.aligned_query_float[i] = query1[i];
      data.scratch.aligned_query_T[i] = query1[i];
      query_norm += query1[i] * query1[i];
    }

    // if inner product, we laso normalize the query and set the last coordinate
    // to 0 (this is the extra coordindate used to convert MIPS to L2 search)
    if (metric == diskann::Metric::INNER_PRODUCT) {
      query_norm = std::sqrt(query_norm);
      data.scratch.aligned_query_T[this->data_dim - 1] = 0;
      data.scratch.aligned_query_float[this->data_dim - 1] = 0;
      for (uint32_t i = 0; i < this->data_dim - 1; i++) {
        data.scratch.aligned_query_T[i] /= query_norm;
        data.scratch.aligned_query_float[i] /= query_norm;
      }
    }

    IOContext &ctx = data.ctx;
    auto       query_scratch = &(data.scratch);

    // reset query
    query_scratch->reset();

    // pointers to buffers for data
    T *   data_buf = query_scratch->coord_scratch;
    _u64 &data_buf_idx = query_scratch->coord_idx;
    _mm_prefetch((char *) data_buf, _MM_HINT_T1);

    // sector scratch
    char *sector_scratch = query_scratch->sector_scratch;
    _u64 &sector_scratch_idx = query_scratch->sector_idx;

    // query <-> PQ chunk centers distances
    float *pq_dists = query_scratch->aligned_pqtable_dist_scratch;
    pq_table.populate_chunk_distances(query_float, pq_dists);

    // query <-> neighbor list
    float *dist_scratch = query_scratch->aligned_dist_scratch;
    _u8 *  pq_coord_scratch = query_scratch->aligned_pq_coord_scratch;

    // lambda to batch compute query<-> node distances in PQ space
    auto compute_dists = [this, pq_coord_scratch, pq_dists](const unsigned *ids,
                                                            const _u64 n_ids,
                                                            float *dists_out) {
      pq_flash_index_utils::aggregate_coords(ids, n_ids, this->data, this->n_chunks,
                         pq_coord_scratch);
      pq_flash_index_utils::pq_dist_lookup(pq_coord_scratch, n_ids, this->n_chunks, pq_dists,
                       dists_out);
    };
    Timer                 query_timer, io_timer, cpu_timer;
    std::vector<Neighbor> retset(l_search + 1);
    tsl::robin_set<_u64> &visited = *(query_scratch->visited);
    tsl::robin_set<unsigned> *page_visited =
        beam_page_aware ? query_scratch->page_visited : nullptr;

    std::vector<Neighbor> full_retset;
    full_retset.reserve(4096);
    _u32                        best_medoid = 0;
    float                       best_dist = (std::numeric_limits<float>::max)();
    std::vector<SimpleNeighbor> medoid_dists;
    for (_u64 cur_m = 0; cur_m < num_medoids; cur_m++) {
      float cur_expanded_dist = dist_cmp_float->compare(
          query_float, centroid_data + aligned_dim * cur_m,
          (unsigned) aligned_dim);
      if (cur_expanded_dist < best_dist) {
        best_medoid = medoids[cur_m];
        best_dist = cur_expanded_dist;
      }
    }

    unsigned cur_list_size = 0;
    auto compute_and_add_to_retset = [&](const unsigned *node_ids, const _u64 n_ids) {
      compute_dists(node_ids, n_ids, dist_scratch);
      for (_u64 i = 0; i < n_ids; ++i) {
        retset[cur_list_size++] = {node_ids[i], dist_scratch[i], true, node_ids[i]};
        visited.insert(node_ids[i]);
      }
    };

    if (mem_L) {
      const _u32 actual_mem_search_L = mem_search_L ? mem_search_L : mem_L;
      const _u32 actual_mem_seed_count = mem_seed_count ? mem_seed_count : mem_L;
      const _u32 actual_mem_k = std::min(actual_mem_seed_count, actual_mem_search_L);
      std::vector<unsigned> mem_tags(actual_mem_k);
      std::vector<T*> res = std::vector<T*>();
      mem_index_->search_with_tags(query, actual_mem_k, actual_mem_search_L, mem_tags.data(), nullptr, nullptr, res);
      compute_and_add_to_retset(mem_tags.data(), std::min(actual_mem_k, (unsigned)l_search));
    } else {
      compute_and_add_to_retset(&best_medoid, 1);
    }

    std::sort(retset.begin(), retset.begin() + cur_list_size);

    unsigned cmps = 0;
    unsigned hops = 0;
    unsigned num_ios = 0;
    unsigned k = 0;

    // PFM+DRA state
    static constexpr unsigned PFM_MIN_EXPLORE_HOPS = 2;
    static constexpr float    PFM_DRA_ALPHA         = 0.3f;
    float pfm_prev_ratio    = 1.0f;
    float pfm_ema_delta     = 0.0f;
    float pfm_last_ratio    = 0.0f;
    float pfm_last_effective_theta = 0.0f;
    bool  pfm_stopped       = false;
    bool  ecg_stopped       = false;
    bool  topk_stability_stopped = false;
    float ecg_last_hop_best_exact = 0.0f;
    float ecg_last_kth_exact = 0.0f;
    std::vector<unsigned> prev_topk_ids;
    prev_topk_ids.reserve(k_search);
    unsigned topk_stability_count = 0;

    // cleared every iteration
    std::vector<unsigned> frontier;
    frontier.reserve(2 * beam_width);
    std::vector<std::pair<unsigned, char *>> frontier_nhoods;
    frontier_nhoods.reserve(2 * beam_width);
    std::vector<AlignedRead> frontier_read_reqs;
    frontier_read_reqs.reserve(2 * beam_width);
    std::vector<std::pair<unsigned, std::pair<unsigned, unsigned *>>>
        cached_nhoods;
    cached_nhoods.reserve(2 * beam_width);

    if (stats != nullptr && collect_query_telemetry) {
      stats->hop_stats.clear();
      stats->hop_stats.reserve(l_search);
      stats->io_traces.clear();
      stats->io_traces.reserve(l_search);
    }

    while (k < cur_list_size && num_ios < io_limit) {
      auto nk = cur_list_size;
      // clear iteration state
      frontier.clear();
      frontier_nhoods.clear();
      frontier_read_reqs.clear();
      cached_nhoods.clear();
      sector_scratch_idx = 0;
      float hop_best_exact = (std::numeric_limits<float>::max)();
      // find new beam
      _u32 marker = k;
      _u32 num_seen = 0;
      while (marker < cur_list_size && frontier.size() < beam_width &&
             num_seen < beam_width) {
        if (retset[marker].flag) {
          if (beam_page_aware) {
            const unsigned pid = id2page_[retset[marker].id];
            if (page_visited->find(pid) != page_visited->end()) {
              retset[marker].flag = false;
              marker++;
              continue;
            }
          }
          num_seen++;
          auto iter = nhood_cache.find(retset[marker].id);
          if (iter != nhood_cache.end()) {
            cached_nhoods.push_back(
                std::make_pair(retset[marker].id, iter->second));
            if (stats != nullptr) {
              stats->n_cache_hits++;
            }
          } else {
            frontier.push_back(retset[marker].id);
            if (beam_page_aware) {
              page_visited->insert(id2page_[retset[marker].id]);
            }
          }
          retset[marker].flag = false;
          if (this->count_visited_nodes) {
#pragma omp critical
            {
              auto &cnt = this->node_visit_counter[retset[marker].id].second;
              ++cnt;
              if (this->count_visited_nbrs) {
                unsigned r_id = retset[marker].rev_id;
                unsigned id = retset[marker].id;
                if (r_id != id) {
                  ++(this->nbrs_freq_counter_[r_id][id]);
                }
              }
            }
          }
        }
        marker++;
      }

      // read nhoods of frontier ids
      if (!frontier.empty()) {
        if (stats != nullptr)
          stats->n_hops++;
        for (_u64 i = 0; i < frontier.size(); i++) {
          auto                    id = frontier[i];
          std::pair<_u32, char *> fnhood;
          fnhood.first = id;
          fnhood.second = sector_scratch + sector_scratch_idx * SECTOR_LEN;
          sector_scratch_idx++;
          frontier_nhoods.push_back(fnhood);
          const uint64_t io_key = beam_page_aware
              ? static_cast<uint64_t>(id2page_[id] + 1)
              : NODE_SECTOR_NO(((size_t) id));
          frontier_read_reqs.emplace_back(io_key * SECTOR_LEN, SECTOR_LEN,
                                          fnhood.second);
          if (stats != nullptr && collect_query_telemetry) {
            stats->io_traces.push_back({hops + 1, id, io_key});
          }
          if (stats != nullptr) {
            stats->n_4k++;
            stats->n_ios++;
          }
          num_ios++;
        }
        io_timer.reset();
#ifdef USE_BING_INFRA
        reader->read(frontier_read_reqs, ctx, true);  // async reader windows.
#else
        reader->read(frontier_read_reqs, ctx);  // synchronous IO linux
#endif
        if (stats != nullptr) {
          stats->io_us += (double) io_timer.elapsed();
        }
      }

      // process cached nhoods
      for (auto &cached_nhood : cached_nhoods) {
        auto  global_cache_iter = coord_cache.find(cached_nhood.first);
        T *   node_fp_coords_copy = global_cache_iter->second;
        float cur_expanded_dist;
        if (!use_disk_index_pq) {
          cur_expanded_dist = dist_cmp->compare(query, node_fp_coords_copy,
                                                (unsigned) aligned_dim);
        } else {
          if (metric == diskann::Metric::INNER_PRODUCT)
            cur_expanded_dist = disk_pq_table.inner_product(
                query_float, (_u8 *) node_fp_coords_copy);
          else
            cur_expanded_dist = disk_pq_table.l2_distance(
                query_float, (_u8 *) node_fp_coords_copy);
        }
        full_retset.push_back(
            Neighbor((unsigned) cached_nhood.first, cur_expanded_dist, true));
        if (cur_expanded_dist < hop_best_exact) {
          hop_best_exact = cur_expanded_dist;
        }

        _u64      nnbrs = cached_nhood.second.first;
        unsigned *node_nbrs = cached_nhood.second.second;

        // compute node_nbrs <-> query dists in PQ space
        cpu_timer.reset();
        compute_dists(node_nbrs, nnbrs, dist_scratch);
        if (stats != nullptr) {
          stats->n_cmps += (double) nnbrs;
          stats->cpu_us += (double) cpu_timer.elapsed();
        }

        // process prefetched nhood
        for (_u64 m = 0; m < nnbrs; ++m) {
          unsigned id = node_nbrs[m];
          if (visited.find(id) != visited.end()) {
            continue;
          } else {
            visited.insert(id);
            cmps++;
            float dist = dist_scratch[m];
            if (dist >= retset[cur_list_size - 1].distance &&
                (cur_list_size == l_search))
              continue;
            Neighbor nn(id, dist, true, (unsigned)cached_nhood.first);
            // Return position in sorted list where nn inserted.
            auto r = InsertIntoPool(retset.data(), cur_list_size, nn);
            if (cur_list_size < l_search)
              ++cur_list_size;
            if (r < nk)
              // nk logs the best position in the retset that was
              // updated due to neighbors of n.
              nk = r;
          }
        }
      }
#ifdef USE_BING_INFRA
      // process each frontier nhood - compute distances to unvisited nodes
      int completedIndex = -1;
      // If we issued read requests and if a read is complete or there are reads
      // in wait state, then enter the while loop.
      while (frontier_read_reqs.size() > 0 &&
             getNextCompletedRequest(ctx, frontier_read_reqs.size(),
                                     completedIndex)) {
        if (completedIndex == -1) {  // all reads are waiting
          continue;
        }
        auto &frontier_nhood = frontier_nhoods[completedIndex];
        (*ctx.m_pRequestsStatus)[completedIndex] = IOContext::PROCESS_COMPLETE;
#else
      for (auto &frontier_nhood : frontier_nhoods) {
#endif
        std::vector<std::pair<unsigned, char *>> nodes_to_process;
        nodes_to_process.reserve(max_degree);
        if (beam_page_aware) {
          const unsigned pid = id2page_[frontier_nhood.first];
          const auto &page_nodes = gp_layout_[pid];
          const unsigned page_size = (unsigned) page_nodes.size();
          unsigned extra_budget = page_size > 0
              ? (unsigned) std::floor(std::max(0.0f, std::min(1.0f, beam_page_ratio)) *
                                      (float) (page_size - 1))
              : 0;
          unsigned effective_max_extra_nodes = beam_page_max_extra_nodes;
          if (beam_page_adaptive_extra) {
            const bool easy_frontier =
                hops >= PFM_MIN_EXPLORE_HOPS &&
                pfm_last_ratio >= beam_page_adaptive_ratio_threshold;
            effective_max_extra_nodes = easy_frontier
                ? beam_page_easy_extra_nodes
                : beam_page_hard_extra_nodes;
          }
          if (effective_max_extra_nodes > 0) {
            extra_budget = std::min(extra_budget, effective_max_extra_nodes);
          }

          for (unsigned j = 0; j < page_size; ++j) {
            const unsigned id = page_nodes[j];
            if (id == frontier_nhood.first) {
              nodes_to_process.insert(nodes_to_process.begin(),
                                      {id, frontier_nhood.second + j * max_node_len});
            }
          }
          if (extra_budget > 0) {
            std::vector<std::pair<float, std::pair<unsigned, char *>>> page_extra_candidates;
            page_extra_candidates.reserve(page_size);
            for (unsigned j = 0; j < page_size; ++j) {
              const unsigned id = page_nodes[j];
              if (id == frontier_nhood.first) {
                continue;
              }
              char *node_disk_buf = frontier_nhood.second + j * max_node_len;
              T *node_fp_coords = OFFSET_TO_NODE_COORDS(node_disk_buf);
              float dist;
              if (!use_disk_index_pq) {
                dist = dist_cmp->compare(query, node_fp_coords, (unsigned) aligned_dim);
              } else if (metric == diskann::Metric::INNER_PRODUCT) {
                dist = disk_pq_table.inner_product(query_float, (_u8 *) node_fp_coords);
              } else {
                dist = disk_pq_table.l2_distance(query_float, (_u8 *) node_fp_coords);
              }
              page_extra_candidates.push_back({dist, {id, node_disk_buf}});
            }
            std::sort(page_extra_candidates.begin(), page_extra_candidates.end(),
                      [](const auto &a, const auto &b) { return a.first < b.first; });
            const unsigned n_extra =
                std::min(extra_budget, (unsigned) page_extra_candidates.size());
            for (unsigned j = 0; j < n_extra; ++j) {
              nodes_to_process.push_back(page_extra_candidates[j].second);
            }
          }
        } else {
          nodes_to_process.push_back({
              frontier_nhood.first,
              OFFSET_TO_NODE(frontier_nhood.second, frontier_nhood.first)});
        }

        for (auto &node_to_process : nodes_to_process) {
        const unsigned expanded_id = node_to_process.first;
        char *node_disk_buf = node_to_process.second;
        unsigned *node_buf = OFFSET_TO_NODE_NHOOD(node_disk_buf);
        _u64      nnbrs = (_u64)(*node_buf);
        T *       node_fp_coords = OFFSET_TO_NODE_COORDS(node_disk_buf);
        //        assert(data_buf_idx < MAX_N_CMPS);
        if (data_buf_idx == MAX_N_CMPS)
          data_buf_idx = 0;

        T *node_fp_coords_copy = data_buf + (data_buf_idx * aligned_dim);
        data_buf_idx++;
        memcpy(node_fp_coords_copy, node_fp_coords, disk_bytes_per_point);
        float cur_expanded_dist;
        if (!use_disk_index_pq) {
          cur_expanded_dist = dist_cmp->compare(query, node_fp_coords_copy,
                                                (unsigned) aligned_dim);
        } else {
          if (metric == diskann::Metric::INNER_PRODUCT)
            cur_expanded_dist = disk_pq_table.inner_product(
                query_float, (_u8 *) node_fp_coords_copy);
          else
            cur_expanded_dist = disk_pq_table.l2_distance(
                query_float, (_u8 *) node_fp_coords_copy);
        }
        full_retset.push_back(
            Neighbor(expanded_id, cur_expanded_dist, true));
        if (cur_expanded_dist < hop_best_exact) {
          hop_best_exact = cur_expanded_dist;
        }
        unsigned *node_nbrs = (node_buf + 1);
        // compute node_nbrs <-> query dist in PQ space
        cpu_timer.reset();
        compute_dists(node_nbrs, nnbrs, dist_scratch);
        if (stats != nullptr) {
          stats->n_cmps += (double) nnbrs;
          stats->cpu_us += (double) cpu_timer.elapsed();
        }

        cpu_timer.reset();
        // process prefetch-ed nhood
        for (_u64 m = 0; m < nnbrs; ++m) {
          unsigned id = node_nbrs[m];
          if (visited.find(id) != visited.end()) {
            continue;
          } else {
            visited.insert(id);
            cmps++;
            float dist = dist_scratch[m];
            if (stats != nullptr) {
              stats->n_cmps++;
            }
            if (dist >= retset[cur_list_size - 1].distance &&
                (cur_list_size == l_search))
              continue;
            Neighbor nn(id, dist, true, expanded_id);
            auto     r = InsertIntoPool(
                retset.data(), cur_list_size,
                nn);  // Return position in sorted list where nn inserted.
            if (cur_list_size < l_search)
              ++cur_list_size;
            if (r < nk)
              nk = r;  // nk logs the best position in the retset that was
                       // updated due to neighbors of n.
          }
        }

        if (stats != nullptr) {
          stats->cpu_us += (double) cpu_timer.elapsed();
        }
        }
      }

      // update best inserted position
      if (nk <= k)
        k = nk;  // k is the best position in retset updated in this round.
      else
        ++k;

      hops++;

      bool can_eval_pfm = hops >= PFM_MIN_EXPLORE_HOPS &&
          cur_list_size >= k_search && k < cur_list_size;
      bool no_unexpanded_left = false;
      float best_unexpanded_pq = 0.0f;
      float kth_result_pq = 0.0f;
      float pq_ratio = 0.0f;
      float delta_ratio = 0.0f;
      float effective_theta = 0.0f;

      if (can_eval_pfm) {
        unsigned pfm_k = k;
        while (pfm_k < cur_list_size && !retset[pfm_k].flag) pfm_k++;
        if (pfm_k >= cur_list_size) {
          no_unexpanded_left = true;
        } else {
          best_unexpanded_pq = retset[pfm_k].distance;
          kth_result_pq      = retset[k_search - 1].distance;
          if (kth_result_pq > 0.0f) {
            pq_ratio          = best_unexpanded_pq / kth_result_pq;
            delta_ratio       = pq_ratio - pfm_prev_ratio;
            pfm_ema_delta     = PFM_DRA_ALPHA * delta_ratio + (1.0f - PFM_DRA_ALPHA) * pfm_ema_delta;
            effective_theta   = std::max(1.0f, pfm_theta - divergence_k * pfm_ema_delta);
            pfm_last_ratio    = pq_ratio;
            pfm_last_effective_theta = effective_theta;
            pfm_prev_ratio    = pq_ratio;
            if (pfm_theta > 0.0f && pq_ratio > effective_theta) {
              pfm_stopped = true;
            }
          }
        }
      }

      if (ecg_alpha > 0.0f && hops >= ecg_min_hops &&
          full_retset.size() >= k_search &&
          hop_best_exact < (std::numeric_limits<float>::max)()) {
        std::vector<float> exact_dists;
        exact_dists.reserve(full_retset.size());
        for (const auto &neighbor : full_retset) {
          exact_dists.push_back(neighbor.distance);
        }
        std::nth_element(exact_dists.begin(),
                         exact_dists.begin() + (k_search - 1),
                         exact_dists.end());
        ecg_last_kth_exact = exact_dists[k_search - 1];
        ecg_last_hop_best_exact = hop_best_exact;
        bool ecg_pq_guard_ok = ecg_pq_guard <= 0.0f ||
            (pq_ratio > 0.0f && pq_ratio >= ecg_pq_guard);
        if (ecg_last_kth_exact > 0.0f && ecg_pq_guard_ok &&
            hop_best_exact > ecg_last_kth_exact * ecg_alpha) {
          ecg_stopped = true;
        }
      }

      if (topk_stability_patience > 0 && cur_list_size >= k_search) {
        std::vector<unsigned> current_topk_ids;
        current_topk_ids.reserve(k_search);
        for (_u64 i = 0; i < k_search; ++i) {
          current_topk_ids.push_back(retset[i].id);
        }
        if (!prev_topk_ids.empty() && current_topk_ids == prev_topk_ids) {
          ++topk_stability_count;
          if (topk_stability_count >= topk_stability_patience) {
            topk_stability_stopped = true;
          }
        } else {
          topk_stability_count = 0;
          prev_topk_ids = std::move(current_topk_ids);
        }
      }

      if (stats != nullptr && collect_query_telemetry) {
        QueryHopStats hop_stat;
        hop_stat.hop = hops;
        hop_stat.n_ios = num_ios;
        hop_stat.n_expanded = (unsigned) full_retset.size();
        hop_stat.cur_list_size = cur_list_size;
        hop_stat.k = k;
        hop_stat.frontier_size = (unsigned) frontier.size();
        hop_stat.cached_size = (unsigned) cached_nhoods.size();
        hop_stat.n_cmps = cmps;
        hop_stat.best_unexpanded_pq = best_unexpanded_pq;
        hop_stat.kth_pq = kth_result_pq;
        hop_stat.pq_ratio = pq_ratio;
        hop_stat.delta_ratio = delta_ratio;
        hop_stat.ema_delta = pfm_ema_delta;
        hop_stat.effective_theta = effective_theta;
        hop_stat.pfm_stopped = pfm_stopped;
        hop_stat.hop_best_exact = ecg_last_hop_best_exact;
        hop_stat.kth_exact = ecg_last_kth_exact;
        hop_stat.ecg_alpha = ecg_alpha;
        hop_stat.ecg_pq_guard = ecg_pq_guard;
        hop_stat.ecg_stopped = ecg_stopped;
        if (!full_retset.empty()) {
          std::vector<Neighbor> hop_results = full_retset;
          std::sort(hop_results.begin(), hop_results.end(),
                    [](const Neighbor &left, const Neighbor &right) {
                      return left.distance < right.distance;
                    });
          hop_stat.top_ids.reserve(k_search);
          for (const auto &neighbor : hop_results) {
            bool duplicate = false;
            for (const auto id : hop_stat.top_ids) {
              if (id == neighbor.id) {
                duplicate = true;
                break;
              }
            }
            if (!duplicate) {
              hop_stat.top_ids.push_back(neighbor.id);
              if (hop_stat.top_ids.size() >= k_search) {
                break;
              }
            }
          }
        }
        stats->hop_stats.push_back(hop_stat);
      }

      if (no_unexpanded_left || pfm_stopped || ecg_stopped ||
          topk_stability_stopped)
        break;
    }

    // re-sort by distance
    std::sort(full_retset.begin(), full_retset.end(),
              [](const Neighbor &left, const Neighbor &right) {
                return left.distance < right.distance;
              });

    if (use_reorder_data) {
      if (!(this->reorder_data_exists)) {
        throw ANNException(
            "Requested use of reordering data which does not exist in index "
            "file",
            -1, __FUNCSIG__, __FILE__, __LINE__);
      }

      std::vector<AlignedRead> vec_read_reqs;

      if (full_retset.size() > k_search * FULL_PRECISION_REORDER_MULTIPLIER)
        full_retset.erase(
            full_retset.begin() + k_search * FULL_PRECISION_REORDER_MULTIPLIER,
            full_retset.end());

      for (size_t i = 0; i < full_retset.size(); ++i) {
        vec_read_reqs.emplace_back(
            VECTOR_SECTOR_NO(((size_t) full_retset[i].id)) * SECTOR_LEN,
            SECTOR_LEN, sector_scratch + i * SECTOR_LEN);

        if (stats != nullptr) {
          stats->n_4k++;
          stats->n_ios++;
        }
      }

      io_timer.reset();
#ifdef USE_BING_INFRA
      reader->read(vec_read_reqs, ctx, false);  // sync reader windows.
#else
      reader->read(vec_read_reqs, ctx);  // synchronous IO linux
#endif
      if (stats != nullptr) {
        stats->io_us += io_timer.elapsed();
      }

      for (size_t i = 0; i < full_retset.size(); ++i) {
        auto id = full_retset[i].id;
        auto location =
            (sector_scratch + i * SECTOR_LEN) + VECTOR_SECTOR_OFFSET(id);
        full_retset[i].distance =
            dist_cmp->compare(query, (T *) location, this->data_dim);
      }

      std::sort(full_retset.begin(), full_retset.end(),
                [](const Neighbor &left, const Neighbor &right) {
                  return left.distance < right.distance;
                });
    }

    // copy k_search values
    for (_u64 i = 0; i < k_search; i++) {
      indices[i] = full_retset[i].id;
      if (distances != nullptr) {
        distances[i] = full_retset[i].distance;
        if (metric == diskann::Metric::INNER_PRODUCT) {
          // flip the sign to convert min to max
          distances[i] = (-distances[i]);
          // rescale to revert back to original norms (cancelling the effect of
          // base and query pre-processing)
          if (max_base_norm != 0)
            distances[i] *= (max_base_norm * query_norm);
        }
      }
    }

    this->thread_data.push(data);
    this->thread_data.push_notify_all();

    // std::cout << num_ios << " " <<stats << std::endl;

    if (stats != nullptr) {
      stats->total_us = (double) query_timer.elapsed();
      stats->n_expanded = (unsigned) full_retset.size();
      stats->frontier_size = cur_list_size;
      stats->pfm_stopped = pfm_stopped;
      stats->pfm_stop_hop = pfm_stopped ? hops : 0;
      stats->pfm_last_ratio = pfm_last_ratio;
      stats->pfm_last_effective_theta = pfm_last_effective_theta;
      stats->pfm_last_ema_delta = pfm_ema_delta;
      stats->ecg_stopped = ecg_stopped;
      stats->ecg_stop_hop = ecg_stopped ? hops : 0;
      stats->ecg_hop_best_exact = ecg_last_hop_best_exact;
      stats->ecg_kth_exact = ecg_last_kth_exact;
      stats->ecg_alpha = ecg_alpha;
      stats->ecg_pq_guard = ecg_pq_guard;
    }
  }

  // instantiations
  template class PQFlashIndex<_u8>;
  template class PQFlashIndex<_s8>;
  template class PQFlashIndex<float>;

}  // namespace diskann
