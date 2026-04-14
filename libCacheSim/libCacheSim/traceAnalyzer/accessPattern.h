#pragma once

/**
 * sample specified number of objects and record their accesses
 * we can plot the access patterns and this can show scan-type requests
 *
 *
 * it does not use an adaptive object selection to sample a given number of
 * objects, because changing the sample ratio at different time will cause
 * a bias when we plot the access pattern
 * so we use a static sample ratio
 *
 */


#include "../include/libCacheSim/logging.h"
#include "../include/libCacheSim/request.h"
#include "../include/libCacheSim/cache.h"
#include "cache_init.h"
#include "struct.h"
#include <set>

using namespace std;

namespace traceAnalyzer {
class AccessPattern {
 public:
  /**
   *
   * @param n_req the total number of requests in the trace
   * @param n_obj the number of objects we would like to sample
   */
  explicit AccessPattern(int sample_ratio = 1001, char* cache_name=nullptr, size_t cache_size=0, char* eviction_params = nullptr)
      : sample_ratio_(sample_ratio) {
        if(cache_name != nullptr && strlen(cache_name) > 0){
            printf("create cache %s for access pattern analysis size %d\n", cache_name,cache_size);
            cache = create_cache(cache_name, cache_size, eviction_params, false);
        }

    if (sample_ratio_ < 1) {
      ERROR(
          "sample_ratio samples 1/sample_ratio objects, and should be at least "
          "1 (no sampling), current value: %d\n",
          sample_ratio_);
      sample_ratio_ = 1;
    }
  };

  ~AccessPattern() = default;

  friend ostream &operator<<(ostream &os, const AccessPattern &pattern) {
    return os;
  }

  void add_req(const request_t *req);

  void dump(string &path_base);

 private:
  int64_t n_obj_ = 0;
  int64_t n_seen_req_ = 0;
  int sample_ratio_ = 1001;
  cache_t *cache= nullptr;

  int64_t start_rtime_ = -1;
  set<const request_t *> cache_contents;
  unordered_map<obj_id_t, vector<uint32_t>> access_rtime_map_;
  unordered_map<obj_id_t, vector<uint32_t>> access_vtime_map_;
  unordered_map<obj_id_t, vector<uint32_t>> evict_rtime_map_;
  unordered_map<obj_id_t, vector<uint32_t>> evict_vtime_map_;
};
}  // namespace traceAnalyzer
