#pragma once

#include <cassert>

#include "bench.h"
#include "reader.h"
#include "string.h"

// extern struct reader *reader;


static bench_opts_t parse_cmd(int argc, char *argv[]) {
  bench_opts_t opts = create_default_bench_opts();

  if (argc < 3) {
    printf(
        "usage: %s trace_path cache_size_in_MB [hashpower] [n_thread]\n",
        argv[0]);
    exit(1);
  }

  strncpy(opts.trace_path, argv[1], MAX_TRACE_PATH_LEN);
  opts.cache_size_in_mb = atoll(argv[2]);
  if (argc >= 4) {
    opts.hashpower = atoll(argv[3]);
  }
  if (argc >= 5) {
    opts.n_thread = atoi(argv[4]);
  }

  return opts;
}
