
#ifndef CACHELIB_ALLOCATOR_DATASTRUCT_MERLINCONFIG_H_
#define CACHELIB_ALLOCATOR_DATASTRUCT_MERLINCONFIG_H_
// Maximum per-object Merlin frequency encoded in CacheLib item ref flags.
#define MAX_FREQ 7
// Upper bound for worker-local state. Runtime thread count must not exceed it.
#define THREAD_NUM 65
// Merlin uses one primary queue plus one paired cuckoo queue per worker.
#define QUEUE_NUM 128
// Maximum attempts to find a victim from a shard before retrying.
#define MAX_RETRIES 8
#endif // CACHELIB_ALLOCATOR_DATASTRUCT_MERLINCONFIG_H_
