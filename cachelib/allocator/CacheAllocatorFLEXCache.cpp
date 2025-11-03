#include "cachelib/allocator/CacheAllocator.h"

namespace facebook::cachelib {
template class CacheAllocator<FLEXCacheTrait>;
template class CacheAllocator<FLEXCachedumpTrait>;
}
