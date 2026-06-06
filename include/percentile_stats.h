// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <algorithm>
#ifdef _WINDOWS
#include <numeric>
#endif
#include <string>
#include <vector>

#include "distance.h"
#include "parameters.h"

namespace diskann {
  struct QueryHopStats {
    unsigned hop = 0;
    unsigned n_ios = 0;
    unsigned n_expanded = 0;
    unsigned cur_list_size = 0;
    unsigned k = 0;
    unsigned frontier_size = 0;
    unsigned cached_size = 0;
    unsigned n_cmps = 0;

    unsigned selected_count = 0;
    float selected_min_pq = 0;
    float selected_mean_pq = 0;
    float selected_max_pq = 0;

    float top1_pq = 0;
    float topk_mean_pq = 0;
    float topk_std_pq = 0;
    float topk_gap_pq = 0;

    unsigned page_rank_count = 0;
    float page_rank_min = 0;
    float page_rank_mean = 0;
    float page_rank_max = 0;

    float best_unexpanded_pq = 0;
    float kth_pq = 0;
    float pq_ratio = 0;
    float delta_ratio = 0;
    float ema_delta = 0;
    float effective_theta = 0;
    bool pfm_stopped = false;

    float hop_best_exact = 0;
    float kth_exact = 0;
    float ecg_alpha = 0;
    float ecg_pq_guard = 0;
    bool ecg_stopped = false;

    std::vector<unsigned> top_ids;          // current exact top-k ids at this hop
  };

  struct QueryIOTrace {
    unsigned hop = 0;
    unsigned read_id = 0;      // node id selected by the query frontier
    uint64_t io_key = 0;       // disk sector/page key used for overlap analysis
  };

  struct QueryStats {
    float total_us = 0;  // total time to process query in micros
    float io_us = 0;     // total time spent in IO
    float cpu_us = 0;    // total time spent in CPU

    unsigned n_4k = 0;          // # of 4kB reads
    unsigned n_8k = 0;          // # of 8kB reads
    unsigned n_12k = 0;         // # of 12kB reads
    unsigned n_ios = 0;         // total # of IOs issued
    unsigned read_size = 0;     // total # of bytes read
    unsigned n_cmps_saved = 0;  // # cmps saved
    unsigned n_cmps = 0;        // # cmps
    unsigned n_cache_hits = 0;  // # cache_hits
    unsigned n_hops = 0;        // # search hops
    unsigned n_expanded = 0;    // # expanded nodes with exact/vector distance
    unsigned frontier_size = 0; // final candidate list size

    bool pfm_stopped = false;              // true if PFM/DRA stopped this query
    unsigned pfm_stop_hop = 0;             // hop at which PFM/DRA stopped
    float pfm_last_ratio = 0;              // last best_unexpanded_pq / kth_pq
    float pfm_last_effective_theta = 0;    // last adaptive threshold
    float pfm_last_ema_delta = 0;          // last DRA EMA(delta)

    bool ecg_stopped = false;              // true if exact convergence gate stopped this query
    unsigned ecg_stop_hop = 0;             // hop at which ECG stopped
    float ecg_hop_best_exact = 0;          // last hop's best expanded exact distance
    float ecg_kth_exact = 0;               // last current kth exact distance
    float ecg_alpha = 0;                   // ECG threshold used for this query
    float ecg_pq_guard = 0;                // required pq_ratio guard used for ECG

    std::vector<QueryHopStats> hop_stats;  // runtime-visible per-hop signals
    std::vector<QueryIOTrace> io_traces;   // issued 4KB reads, collected only when telemetry is enabled
  };

  template<typename T>
  inline T get_percentile_stats(
      QueryStats *stats, uint64_t len, float percentile,
      const std::function<T(const QueryStats &)> &member_fn) {
    std::vector<T> vals(len);
    for (uint64_t i = 0; i < len; i++) {
      vals[i] = member_fn(stats[i]);
    }

    std::sort(vals.begin(), vals.end(),
              [](const T &left, const T &right) { return left < right; });

    auto retval = vals[(uint64_t)(percentile * len)];
    vals.clear();
    return retval;
  }

  template<typename T>
  inline double get_mean_stats(
      QueryStats *stats, uint64_t len,
      const std::function<T(const QueryStats &)> &member_fn) {
    double avg = 0;
    for (uint64_t i = 0; i < len; i++) {
      avg += (double) member_fn(stats[i]);
    }
    return avg / len;
  }

  // The following two functions are used when getting statistics while range searching on only queries with
  // non-zero gt lengths
  template<typename T>
  inline T get_percentile_stats_gt(
      QueryStats *stats, uint64_t len, float percentile,
      const std::function<T(const QueryStats &)> &member_fn, std::vector<std::vector<uint32_t>> &gt) {
    std::vector<T> vals;
    for (uint64_t i = 0; i < len; i++) {
      if (gt[i].size()) vals.push_back(member_fn(stats[i]));
    }

    std::sort(vals.begin(), vals.end(),
              [](const T &left, const T &right) { return left < right; });

    auto retval = vals[(uint64_t)(percentile * vals.size())];
    vals.clear();
    return retval;
  }

  template<typename T>
  inline double get_mean_stats_gt(
      QueryStats *stats, uint64_t len,
      const std::function<T(const QueryStats &)> &member_fn, std::vector<std::vector<uint32_t>> &gt) {
    uint32_t cnt = 0;
    double avg = 0;
    for (uint64_t i = 0; i < len; i++) {
      if (gt[i].size()) {
        ++cnt;
        avg += (double) member_fn(stats[i]);
      }
    }
    return avg / cnt;
  }
}  // namespace diskann
