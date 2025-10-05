#include <fstream>
#include <iostream>

#include "cachelib/allocator/CacheAllocator.h"

using Cache = facebook::cachelib::LruAllocator;

using namespace std;
void initializeCache() {

  Cache::Config config;
  config.setCacheSize(200 * 1024 * 1024)
      .setCacheName("My cache")
      .setAccessConfig({25, 10})
      .validate();

  // config.enablePoolRebalancing(rebalanceStrategy,
  //                              std::chrono::seconds(kRebalanceIntervalSecs));

  auto cache = std::make_unique<Cache>(config);
  facebook::cachelib::PoolId pool;
  pool = cache->addPool("default", cache->getCacheMemoryStats().cacheSize);

  string data("new data");
  Cache::ItemHandle item_handle = cache->allocate(pool, "key1", 102400);
  std::memcpy(item_handle->getWritableMemory(), data.data(), data.size());
  cache->insert(item_handle);
  // cache->insertOrReplace(item_handle);

  data = "Repalce the data associated with key key1";
  item_handle = cache->allocate(pool, "key1", data.size());
  std::memcpy(item_handle->getWritableMemory(), data.data(), data.size());
  cache->insertOrReplace(item_handle);

  item_handle = cache->find("key1");
  if (item_handle) {
    auto data = reinterpret_cast<const char *>(item_handle->getMemory());
    std::cout << data << '\n';
  }

  // cache.reset();
}

int main(int argc, char *argv[]) {
  initializeCache();

  printf("Hello World\n");
}
