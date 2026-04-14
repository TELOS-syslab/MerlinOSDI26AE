#pragma once

#include <folly/logging/xlog.h>
#include <folly/lang/Aligned.h>

#include <folly/synchronization/DistributedMutex.h>

#include <atomic>

#include "cachelib/common/Mutex.h"

namespace facebook {
namespace cachelib {

class AtomicFIFOHashTable {
 public:
  using Mutex = folly::DistributedMutex;
  using LockHolder = std::unique_lock<Mutex>;

  AtomicFIFOHashTable() = default;

  explicit AtomicFIFOHashTable(uint32_t fifoSize) noexcept {
    fifoSize_ = ((fifoSize >> 3) + 1) << 3;
    numElem_ = fifoSize_ * loadFactorInv_;
    initHashtable();
  }

  ~AtomicFIFOHashTable() { hashTable_ = nullptr; }

  bool initialized() const noexcept { return hashTable_ != nullptr; }

  void initHashtable() noexcept {
    auto hashTable = std::unique_ptr<uint64_t[]>(new uint64_t[numElem_]);
    memset(hashTable.get(), 0, numElem_ * sizeof(uint64_t));
    hashTable_ = std::move(hashTable);

    // printf("create table fifoSize_ %zu numElem_ %zu\n", fifoSize_, numElem_);
  }

  void setFIFOSize(uint32_t fifoSize) noexcept {
    fifoSize_ = ((fifoSize >> 3) + 1) << 3;
    numElem_ = fifoSize_ * loadFactorInv_;
    timestampsize_.store(fifoSize_, std::memory_order_release);
  }

  void changeFIFOSize(uint32_t fifoSize) noexcept {
    timestampsize_.store(((fifoSize >> 3) + 1) << 3, std::memory_order_release);
  }

  bool contains(uint32_t key) noexcept;

  void insert(uint32_t key) noexcept;

 private:
  size_t getBucketIdx(uint32_t key) {
    // TODO: we can use & directly
    size_t bucketIdx = (size_t)key % numElem_;
    bucketIdx = bucketIdx & bucketIdxMask_;
    return bucketIdx;
  }

  bool matchKey(uint64_t hashTableVal, uint32_t key) {
    return (hashTableVal & keyMask_) == key;
  }

  uint32_t getInsertionTime(uint64_t hashTableVal) {
    return static_cast<uint32_t>((hashTableVal & valueMask_) >> 32);
  }

  uint64_t genHashtableVal(uint32_t key, uint32_t time) {
    uint64_t uKey = static_cast<uint64_t>(key);
    uint64_t uTime = static_cast<uint64_t>(time);

    return uKey | (uTime << 32);
  }

  static constexpr size_t loadFactorInv_{2};
  static constexpr size_t nItemPerBucket_{8};
  static constexpr size_t bucketIdxMask_{0xFFFFFFFFFFFFFFF8};

  constexpr static uint64_t keyMask_ = 0x00000000FFFFFFFF;
  constexpr static uint64_t valueMask_ = 0xFFFFFFFF00000000;

  size_t numElem_{0};
  // curr time - insert time > FIFO size => not valid
  size_t fifoSize_{0};
  std::atomic<int64_t> numInserts_{0};
  std::atomic<int64_t> numEvicts_{0};
  std::atomic<int64_t> timestampsize_{0};
  alignas(64) std::unique_ptr<uint64_t[]> hashTable_{nullptr};
  mutable folly::cacheline_aligned<Mutex> mtx_;
};

} // namespace cachelib
} // namespace facebook

// #include "cachelib/allocator/datastruct/AtomicFIFOHashTable-inl.h"
