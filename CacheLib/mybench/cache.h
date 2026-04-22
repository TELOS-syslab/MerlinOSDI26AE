#pragma once

#include "cachelib/allocator/CacheAllocator.h"
#include "log.h"

using namespace facebook::cachelib;
#define BACKEND_LATENCY 10
//#define ENABLE_BACKEND_LATENCY
#if defined(USE_LRU) || defined(USE_STRICTLRU)
using Cache = facebook::cachelib::LruAllocator;
#elif defined(USE_CLOCK)
//using Cache = facebook::cachelib::ClockAllocator;
#elif defined(USE_S3FIFO)
using Cache = S3FIFOAllocator;
#elif defined(USE_TWOQ)
using Cache = Lru2QAllocator;
#elif defined(USE_TINYLFU)
using Cache = TinyLFUAllocator;
#elif defined(USE_MERLIN)
using Cache = MerlinAllocator;
#elif defined(USE_ARC)
using Cache = ARCAllocator;
#elif defined(USE_CAR)
using Cache = CARAllocator;
#endif

void mycache_init(int64_t cache_size_in_mb, unsigned int hashpower,
                  Cache **cache_p, PoolId *pool_p, int n_thread);

int cache_get(Cache *cache, PoolId pool, struct request *req, int thread_id=0);

int cache_set(Cache *cache, PoolId pool, struct request *req, int thread_id=0);

int cache_del(Cache *cache, PoolId pool, struct request *req, int thread_id=0);

double cache_utilization(Cache *cache);
