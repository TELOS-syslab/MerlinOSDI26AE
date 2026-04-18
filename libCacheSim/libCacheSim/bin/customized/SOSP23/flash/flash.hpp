
#include "../../../../include/libCacheSim/cache.h"
#include "../../../../include/libCacheSim/reader.h"
#include <vector>
#include "../../../../utils/include/mymath.h"
#include "../../../../utils/include/mystr.h"
#include "../../../../utils/include/mysys.h"
#include "../../../cachesim/internal.h"

typedef struct {
  cache_t *ram;
  cache_t *disk;

  int64_t n_obj_admit_to_ram;
  int64_t n_obj_admit_to_disk;
  int64_t n_byte_admit_to_ram;
  int64_t n_byte_admit_to_disk;

  double ram_size_ratio;
  double disk_admit_prob;
  int inv_prob;

  char ram_cache_type[32];
  request_t *req_local;
} flashProb_params_t;

typedef enum { RETAIN_POLICY_RECENCY = 0, RETAIN_POLICY_FREQUENCY, RETAIN_POLICY_BELADY, RETAIN_NONE } retain_policy_t;

typedef struct FIFO_Reinsertion_params {
  cache_obj_t *q_head;
  cache_obj_t *q_tail;

  // points to the eviction position
  cache_obj_t *next_to_merge;
  // the number of object to examine at each eviction
  int n_exam_obj;
  // of the n_exam_obj, we keep n_keep_obj and evict the rest
  int n_keep_obj;
  // used to sort the n_exam_obj objects
  struct sort_list_node *metric_list;
  // the policy to determine the n_keep_obj objects
  retain_policy_t retain_policy;

  int64_t n_obj_rewritten;
  int64_t n_byte_rewritten;
} FIFO_Reinsertion_params_t;

typedef struct {
  cache_t *fifo;
  cache_t *fifo_ghost;
  cache_t *main_cache;
  bool hit_on_ghost;

  int64_t n_obj_admit_to_fifo;
  int64_t n_obj_admit_to_main;
  int64_t n_obj_move_to_main;
  int64_t n_obj_rewritten_in_main;
  int64_t n_byte_admit_to_fifo;
  int64_t n_byte_admit_to_main;
  int64_t n_byte_move_to_main;
  int64_t n_byte_rewritten_in_main;

  double fifo_size_ratio;
  double ghost_size_ratio;
  char main_cache_type[32];

  request_t *req_local;
} QDLPv1_params_t;

typedef struct WTinyLFU_params {
  cache_t *LRU;         // LRU as windowed LRU
  cache_t *main_cache;  // any eviction policy
  double window_size;
  int64_t n_admit_bytes;
  struct minimalIncrementCBF *CBF;
  size_t max_request_num;
  size_t request_counter;
  char main_cache_type[32];

  request_t *req_local;
} WTinyLFU_params_t;

typedef struct {
  cache_t *fifo;
  cache_t *fifo_ghost;
  cache_t *main_cache;
  bool hit_on_ghost;

  int64_t n_obj_admit_to_fifo;
  int64_t n_obj_admit_to_main;
  int64_t n_obj_move_to_main;
  int64_t n_obj_reinsert_to_main;
  int64_t n_byte_admit_to_fifo;
  int64_t n_byte_admit_to_main;
  int64_t n_byte_move_to_main;
  int64_t n_byte_reinsert_to_main;

  int move_to_main_threshold;
  double fifo_size_ratio;
  double ghost_size_ratio;
  char main_cache_type[32];

  request_t *req_local;
} S3FIFO_params_t;

typedef struct ARCfix_params {
  // L1_data is T1 in the paper, L1_ghost is B1 in the paper
  int64_t L1_data_size;
  int64_t L2_data_size;
  int64_t L1_ghost_size;
  int64_t L2_ghost_size;

  cache_obj_t *L1_data_head;
  cache_obj_t *L1_data_tail;
  cache_obj_t *L1_ghost_head;
  cache_obj_t *L1_ghost_tail;

  cache_obj_t *L2_data_head;
  cache_obj_t *L2_data_tail;
  cache_obj_t *L2_ghost_head;
  cache_obj_t *L2_ghost_tail;

  double p;
  double track_p;
  bool curr_obj_in_L1_ghost;
  bool curr_obj_in_L2_ghost;
  int64_t vtime_last_req_in_ghost;
  request_t *req_local;
  
  int64_t n_byte_write_in_L2_data;
} ARCfix_params_t;

typedef struct Cacheus_params {
  cache_t *LRU;        // LRU
  cache_t *LRU_g;      // eviction history of LRU
  cache_t *LFU;        // LFU
  cache_t *LFU_g;      // eviction history of LFU
  double w_lru;        // Weight for LRU
  double w_lfu;        // Weight for LFU
  double lr;           // learning rate
  double lr_previous;  // previous learning rate

  double ghost_list_factor;  // size(ghost_list)/size(cache), default 1
  int64_t unlearn_count;

  int64_t num_hit;
  double hit_rate_prev;

  uint64_t update_interval;
  request_t *req_local;
} Cacheus_params_t;

typedef struct SR_LRU_params {
  cache_t *SR_list;    // Scan Resistant list
  cache_t *R_list;     // Churn Resistant List
  cache_t *H_list;     // History
  uint64_t C_demoted;  // count of demoted object in cache
  uint64_t C_new;      // count of new item in history
  cache_t *other_cache;
  request_t *req_local;
  double p;

  int64_t n_byte_write_in_R;
} SR_LRU_params_t;

typedef struct {
  int64_t timer = 0;
  request_t *req_local;
  cache_t *filter;
  cache_t *core;
  cache_t *staging;
  cache_t *ghost;
  uint64_t filter_limit;
  uint64_t core_limit;
  uint64_t staging_limit;
  uint64_t ghost_limit;
  double filter_size_ratio;
  double staging_size_ratio;
  double ghost_size_ratio;
  struct minimalIncrementCBF *CBF;
  int32_t epoch_count;
  int32_t epoch_update;
  double sketch_scale;
  //
  int32_t guard_freq;
  int32_t track_freq;
  std::vector<int32_t> hotdistribution;
  int32_t evictobj_num;
  int32_t seq_miss;
  int32_t ghost_freq;
  int32_t hit_on_ghost;
  int32_t move_to_core;
  request_t *req_staging;
  //
  int32_t filter_hitnum;
  int32_t staging_hitnum;
  int32_t core_hitnum;
  int32_t ghost_hitnum;
  int32_t compareguard;

  int32_t ghost2core;
  int32_t ghost2staging;
  int32_t filter2staging;
  int32_t filter2core;
  int32_t filter2ghost;
  int32_t staging2core;
  int32_t evict_staging_ghost;

  int64_t n_byte_admit_to_core;
  int64_t n_byte_move_to_core;
} merlin_params_t;