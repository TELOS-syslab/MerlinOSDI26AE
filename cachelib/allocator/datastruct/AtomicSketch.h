#pragma once

#include <folly/logging/xlog.h>
#include <folly/lang/Aligned.h>

#include <folly/synchronization/DistributedMutex.h>

#include <atomic>

#include "cachelib/common/Mutex.h"

namespace facebook
{
    namespace cachelib
    {
        class AtomicSketch
        {
        public:
            AtomicSketch() = default;

            explicit AtomicSketch(uint32_t fifoSize) noexcept
            {
                fifoSize_ = ((fifoSize >> 3) + 1) << 3;
                numElem_ = fifoSize_ * loadFactorInv_;
                initHashtable();
            }

            ~AtomicSketch() { hashTable_ = nullptr; }

            bool initialized() const noexcept { return hashTable_ != nullptr; }

            void initHashtable() noexcept
            {
                auto hashTable = std::unique_ptr<std::atomic<uint32_t>[]>(new std::atomic<uint32_t>[numElem_]);
                for (size_t i = 0; i < numElem_; ++i)
                {
                    hashTable[i].store(0, std::memory_order_relaxed);
                }
                hashTable_ = std::move(hashTable);
            }

            void setFIFOSize(uint32_t fifoSize) noexcept
            {
                fifoSize_ = ((fifoSize >> 3) + 1) << 3;
                numElem_ = fifoSize_ * loadFactorInv_;
            }

            uint64_t getEstimate(uint32_t key) noexcept
            {
                size_t bucketIdx = getBucketIdx(key);
                return hashTable_[bucketIdx].load(std::memory_order_relaxed);
            }

            void estimate(uint32_t key) noexcept
            {
                size_t bucketIdx = getBucketIdx(key);
                hashTable_[bucketIdx].fetch_add(1, std::memory_order_relaxed);
                int id = numInserts_++;
                if((id & 0x1f) == 0x1f) {
                    int cleanid = ((id >> 5) % numElem_);
                    int val = hashTable_[cleanid].load(std::memory_order_relaxed);
                    val /= 2;
                    hashTable_[cleanid].store(val, std::memory_order_relaxed);
                }
                if(id > MAX_VALUE) {
                    numInserts_ = 0;
                }
            }

        private:
            size_t getBucketIdx(uint32_t key)
            {
                size_t bucketIdx = (size_t)key % numElem_;
                //bucketIdx = bucketIdx & bucketIdxMask_;
                return bucketIdx;
            }

            static constexpr size_t loadFactorInv_{2};
            static constexpr size_t nItemPerBucket_{8};
            static constexpr size_t bucketIdxMask_{0xFFFFFFFFFFFFFFF8};

            constexpr static uint64_t keyMask_ = 0x00000000FFFFFFFF;
            constexpr static uint64_t valueMask_ = 0x0FFFFFFF00000000;
            constexpr static uint64_t freqMask_ = 0xF000000000000000;
            constexpr static uint64_t MAX_VALUE = 0x0FFFFFFF;

        private:
            size_t numElem_{0};
            size_t fifoSize_{0};
            std::atomic<int64_t> numInserts_{0};
            std::atomic<int64_t> numEvicts_{0};
            alignas(64) std::unique_ptr<std::atomic<uint32_t>[]> hashTable_{nullptr};
        };

    } // namespace cachelib
} // namespace facebook

// #include "cachelib/allocator/datastruct/AtomicFIFOHashTable-inl.h"
