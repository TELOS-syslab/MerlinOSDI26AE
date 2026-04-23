
#include <glog/logging.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>

#include "bench.h"
#include "cache.h"
#include "cmd.h"
#include "reader.h"
#include "request.h"

int main(int argc, char *argv[]) {
  google::InitGoogleLogging("mybench");

  bench_opts_t opts = parse_cmd(argc, argv);
  struct bench_data bench_data[65];
  for (int i = 0; i <= 64; i++) {
    memset(&bench_data[i], 0, sizeof(bench_data[i]));
  }

  if (opts.n_thread == 1) {
    mycache_init(opts.cache_size_in_mb, opts.hashpower, &bench_data[0].cache,
                 &bench_data[0].pool, opts.n_thread);
    trace_replay_run(&bench_data[0], &opts);
    report_bench_result(&bench_data[0], &opts);
  } else {
    if (bench_use_private_bdata_per_thread()) {
      for (int i = 0; i < opts.n_thread; i++) {
        mycache_init(opts.cache_size_in_mb / opts.n_thread, opts.hashpower,
                     &bench_data[i].cache, &bench_data[i].pool, opts.n_thread);
      }
    } else {
      mycache_init(opts.cache_size_in_mb, opts.hashpower, &bench_data[0].cache,
                   &bench_data[0].pool, opts.n_thread);
    }
    trace_replay_run_mt(bench_data, &opts);
  }

  return 0;
}
