
#include <assert.h>
#include <errno.h>
#include <glog/logging.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <sys/time.h>
#include <sysexits.h>
#include <unistd.h>

#include <atomic>
#include <thread>
#include <vector>

#include "cache.h"
#include "bench.h"
#include "reader.h"
#include "request.h"

#include <sstream>

using namespace std;

static atomic<bool> STOP_FLAG = true;

struct alignas(64) thread_res {
  int64_t n_get;
  int64_t n_set;
  int64_t n_get_miss;
  int64_t n_del;

  int64_t trace_time;
  char padding[64 - 5 * sizeof(int64_t)];
};
static_assert(sizeof(thread_res) == 64);

static void pin_thread_to_core(int core_id) {
#if !defined(__APPLE__)
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id * 2, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

  LOG(INFO) << "pin thread " << pthread_self() << " to core " << core_id;
#endif
}

static void trace_replay_run_thread(struct bench_data *bdata,
                                    bench_opts_t *opts, int thread_id,
                                    struct thread_res *res) {
  pin_thread_to_core(thread_id - 1);

  struct request *req = new_request();
  struct reader *reader =
      open_trace(opts->trace_path, opts->trace_type, thread_id);

  res->n_get = res->n_set = res->n_get_miss = res->n_del = 0;

  int status = read_trace(reader, req);
  assert(status == 0);
  LOG(INFO) << "thread " << thread_id << " read " << *(uint64_t *)req->key
            << ", wait to start";

  while (STOP_FLAG.load()) {
    ;
  }

  LOG(INFO) << "thread " << thread_id << " start";
  while (read_trace(reader, req) == 0) {
    if (res->n_get % 1000000 == 0 && thread_id == 1) {
      util::setCurrentTimeSec(req->timestamp);
    }
    status = cache_go(bdata->cache, bdata->pool, req, &res->n_get, &res->n_set,
                      &res->n_del, &res->n_get_miss, thread_id);

    if (res->n_get % 1000000 == 0) {
      if (STOP_FLAG.load()) {
        break;
      }
      res->trace_time = req->timestamp;
    }
  }

  res->trace_time = req->timestamp;
  STOP_FLAG.store(true);
  LOG(INFO) << "thread " << thread_id << " finishes";
}

static void aggregate_results(struct bench_data *bdata, bench_opts_t *opts,
                              struct thread_res *res) {
  int n_thread = opts->n_thread;

  bdata->n_get = bdata->n_set = bdata->n_get_miss = bdata->n_del = 0;
  bdata->trace_time = 0;

  int64_t min_trace_time = INT64_MAX, max_trace_time = INT64_MIN;

  for (int i = 0; i < n_thread; i++) {
    bdata->n_get += res[i].n_get;
    bdata->n_set += res[i].n_set;
    bdata->n_get_miss += res[i].n_get_miss;
    bdata->n_del += res[i].n_del;

    if (res[i].trace_time < min_trace_time) {
      min_trace_time = res[i].trace_time;
    }
    if (res[i].trace_time > max_trace_time) {
      max_trace_time = res[i].trace_time;
    }
  }
  bdata->trace_time = max_trace_time;
  util::setCurrentTimeSec(min_trace_time);
}

void andlysis_log(struct bench_data *bdata) {
  printf("collecting andlysis log\n");
  std::stringstream ss;
  bdata->cache->dump(ss);
  printf("operation time");
  analysis(ss);
  ss.clear();
  dump(ss);
  printf("response time");
  analysis(ss);
  return;
}

void trace_replay_run_mt(struct bench_data *bdata, bench_opts_t *opts) {
  int n_thread = opts->n_thread;
  struct thread_res *res = new struct thread_res[n_thread];

  std::vector<std::thread> threads;
  for (int i = 0; i < n_thread; i++) {
    struct bench_data *thread_bdata =
        bench_use_private_bdata_per_thread() ? &bdata[i] : &bdata[0];
    threads.push_back(std::thread(trace_replay_run_thread, thread_bdata, opts,
                                  i + 1, &res[i]));
  }

  sleep(2);
  STOP_FLAG.store(false);
  gettimeofday(&bdata[0].start_time, NULL);

  int counter = 0;
  while (!STOP_FLAG.load()) {
    sleep(1);
    aggregate_results(&bdata[0], opts, res);
    counter++;
    if (counter % 10 == 0) {
      report_bench_result(&bdata[0], opts);
    }
    if (counter >= 600) {
      break;
    }
  }
  aggregate_results(&bdata[0], opts, res);
  report_bench_result(&bdata[0], opts);
  for (int i = 0; i < n_thread; i++) {
    threads[i].join();
  }

#ifdef DUMP_TIME
  andlysis_log(&bdata[0]);
#endif
  delete[] res;
}
