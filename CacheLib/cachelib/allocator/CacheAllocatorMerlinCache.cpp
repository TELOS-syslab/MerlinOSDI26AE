#include "cachelib/allocator/CacheAllocator.h"

namespace facebook::cachelib {
// Explicitly instantiate the CacheAllocator for Merlin so the benchmark binary
// can link the Merlin cache trait like CacheLib's built-in policies.
template class CacheAllocator<MerlinCacheTrait>;
}
