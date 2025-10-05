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
#define MAX_FREQ 7
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
                auto hashTable = std::unique_ptr<std::atomic<uint64_t>[]>(new std::atomic<uint64_t>[numElem_]);
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

            int contains(uint32_t key) noexcept
            {
                uint32_t bucketIdx = getBucketIdx(key);
                int64_t currTime = numInserts_.load();
                for (int i = 1; i < nItemPerBucket_; i++)
                {
                    uint64_t valInTable = hashTable_[bucketIdx + i].load(std::memory_order_relaxed);
                    int64_t insertionTime = getInsertionTime(valInTable);
                    int64_t age = currTime - insertionTime;
                    if (valInTable == 0)
                    {
                        continue;
                    }
                    if (age > fifoSize_)
                    {
                        int ret = hashTable_[bucketIdx + i].compare_exchange_strong(valInTable, 0, std::memory_order_relaxed);
                        if (ret)
                        {
                            int ori_freq = getFreq(valInTable);
                            for (int i = 2; i <= ori_freq; i++)
                            {
                                freq_distribution_[i].fetch_sub(1, std::memory_order_relaxed);
                            }
                        }
                        continue;
                    }
                    if (matchKey(valInTable, key))
                    {
                        int ori_freq = getFreq(valInTable);
                        return ori_freq+1;
                    }
                }
                return -1;
            }

            void insert(uint32_t key, int freq = 0) noexcept
            {
                int64_t currTime = numInserts_++;
                freq++;
                // update sketch
                //  decay every 64 inserts
                if ((currTime & 0x3f) == 0x3f)
                {
                    int64_t cleanid = ((currTime >> 6) % numElem_) << 2;
                    int val = hashTable_[cleanid].load(std::memory_order_relaxed);
                    val /= 2;
                    hashTable_[cleanid].store(val, std::memory_order_relaxed);
                }
                // reset if overflow
                if (currTime > MAX_VALUE)
                {
                    numInserts_ = 0;
                    currTime = 0;
                }
                size_t bucketIdx = getBucketIdx(key);
                // estimate
                hashTable_[bucketIdx].fetch_add(1, std::memory_order_relaxed);
                // find and increase
                int empty_slot = -1;
                for (int i = 1; i < nItemPerBucket_; i++)
                {
                    uint64_t valInTable = hashTable_[bucketIdx + i].load(std::memory_order_relaxed);
                    int64_t insertionTime = getInsertionTime(valInTable);
                    int64_t age = currTime - insertionTime;
                    if (valInTable == 0)
                    {
                        empty_slot = i;
                        continue;
                    }
                    if (age > fifoSize_)
                    {
                        int ret = hashTable_[bucketIdx + i].compare_exchange_strong(valInTable, 0, std::memory_order_relaxed);
                        if (ret)
                        {
                            int ori_freq = getFreq(valInTable);
                            for (int i = 2; i <= ori_freq; i++)
                            {
                                freq_distribution_[i].fetch_sub(1, std::memory_order_relaxed);
                            }
                            empty_slot = i;
                        }
                        continue;
                    }
                    if (matchKey(valInTable, key))
                    {
                        int overflow = 0;
                        int ori_freq = getFreq(valInTable);
                        if (ori_freq == MAX_FREQ)
                        {
                            return;
                        }
                        int new_freq = std::min(ori_freq + freq, MAX_FREQ);
                        uint64_t newVal = genHashtableVal(key, insertionTime, new_freq);
                        int ret = hashTable_[bucketIdx + i].compare_exchange_strong(valInTable, newVal, std::memory_order_relaxed);
                        if (ret && ori_freq >= 1)
                        { // success update freq only once
                            for (int i = ori_freq + 1; i <= new_freq; i++)
                            {
                                freq_distribution_[i].fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                        return;
                    }
                }
                // not find, insert
                freq--;
                uint64_t hashTableVal = genHashtableVal(key, static_cast<uint32_t>(currTime), freq);
                // attempt only once
                if (empty_slot == -1)
                {
                    size_t evictIdx = key % numElem_;
                    if ((evictIdx & 3) != 0)
                    {
                        numEvicts_++;
                        int ret = 0;
                        uint64_t valInTable = 0;
                        while (ret == 0)
                        {
                            valInTable = hashTable_[evictIdx].load(std::memory_order_relaxed);
                            ret = hashTable_[evictIdx].compare_exchange_strong(valInTable, hashTableVal, std::memory_order_relaxed);
                        }
                        int ori_freq = getFreq(valInTable);
                        for (int i = 2; i <= ori_freq; i++)
                        {
                            freq_distribution_[i].fetch_sub(1, std::memory_order_relaxed);
                        }
                        for (int i = 2; i <= freq; i++)
                        {
                            freq_distribution_[i].fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                else
                {
                    uint64_t emptyVal = 0;
                    int ret = hashTable_[bucketIdx + empty_slot].compare_exchange_strong(emptyVal, hashTableVal, std::memory_order_relaxed);
                    if (ret)
                    {
                        for (int i = 2; i <= freq; i++)
                        {
                            freq_distribution_[i].fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                return;
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
            }

        private:
            size_t getBucketIdx(uint32_t key)
            {
                // TODO: we can use & directly
                size_t bucketIdx = (size_t)key % numElem_;
                bucketIdx = bucketIdx & bucketIdxMask_;
                return bucketIdx;
            }

            bool matchKey(uint64_t hashTableVal, uint32_t key)
            {
                return (hashTableVal & keyMask_) == key;
            }

            uint32_t getInsertionTime(uint64_t hashTableVal)
            {
                return static_cast<uint32_t>((hashTableVal & valueMask_) >> 32);
            }

            uint32_t getFreq(uint64_t hashTableVal)
            {
                return static_cast<uint32_t>((hashTableVal & freqMask_) >> 60);
            }

            uint64_t genHashtableVal(uint32_t key, uint32_t time, uint32_t freq)
            {
                uint64_t uKey = static_cast<uint64_t>(key);
                uint64_t uTime = static_cast<uint64_t>(time);
                uint64_t uFreq = static_cast<uint64_t>(freq);
                return uKey | (uTime << 32) | (uFreq << 60);
            }

            static constexpr size_t loadFactorInv_{4};
            static constexpr size_t nItemPerBucket_{4};
            static constexpr size_t bucketIdxMask_{0xFFFFFFFFFFFFFFFC};

            constexpr static uint64_t keyMask_ = 0x00000000FFFFFFFF;
            constexpr static uint64_t valueMask_ = 0x0FFFFFFF00000000;
            constexpr static uint64_t freqMask_ = 0xF000000000000000;
            constexpr static uint64_t MAX_VALUE = 0x0FFFFFFF;
public:
            std::vector<std::atomic<uint32_t>> freq_distribution_{std::vector<std::atomic<uint32_t>>(MAX_FREQ + 1)};
private:
            size_t numElem_{0};
            size_t fifoSize_{0};
            std::atomic<int64_t> numInserts_{0};
            std::atomic<int64_t> numEvicts_{0};
            alignas(64) std::unique_ptr<std::atomic<uint64_t>[]> hashTable_{nullptr};
        };

    } // namespace cachelib
} // namespace facebook

// #include "cachelib/allocator/datastruct/AtomicFIFOHashTable-inl.h"
