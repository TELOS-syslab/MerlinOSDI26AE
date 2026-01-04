#pragma once

#include <folly/logging/xlog.h>
#include <folly/lang/Aligned.h>

#include <folly/synchronization/DistributedMutex.h>

#include <atomic>

#include "cachelib/common/Mutex.h"

#include "cachelib/allocator/datastruct/merlinconfig.h"

namespace facebook
{
    namespace cachelib
    {
        class AtomicFIFOSketch
        {
        public:
            AtomicFIFOSketch() = default;

            explicit AtomicFIFOSketch(uint32_t fifoSize) noexcept
            {
                fifoSize_ = ((fifoSize >> 3) + 1) << 3;
                numElem_ = fifoSize_ * loadFactorInv_;
                initHashtable();
            }

            ~AtomicFIFOSketch() { hashTable_ = nullptr; }

            inline bool initialized() const noexcept { return initialized_.load(std::memory_order_acquire); }

            void initHashtable() noexcept
            {
                initialized_.store(true, std::memory_order_release);
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
                for(int i = 0; i < nItemPerBucket_; i++){
                    uint64_t valInTable = hashTable_[bucketIdx + i].load(std::memory_order_relaxed);
                    if(valInTable == 0){
                        continue;
                    }
                    int64_t insertionTime = getInsertionTime(valInTable);
                    int64_t age = currTime - insertionTime;

                    if (age > fifoSize_)
                    {
                        int ret = hashTable_[bucketIdx + i].compare_exchange_weak(valInTable, 0, std::memory_order_relaxed);
                        if(ret){
                            int ori_freq = getFreq(valInTable);
                            if(ori_freq >= 2){
                                freq_distribution_[2].fetch_sub(1, std::memory_order_relaxed);
                            }
                            if(ori_freq >= 1){
                                freq_distribution_[1].fetch_sub(1, std::memory_order_relaxed);
                            }
                        }
                        continue;
                    }
                    if (matchKey(valInTable, key))
                    {
                        int ori_freq = getFreq(valInTable);
                        if(ori_freq == MAX_FREQ){
                            return MAX_FREQ;
                        }
                        int new_freq = std::min(ori_freq + 1, MAX_FREQ);
                        uint64_t newVal = genHashtableVal(key, insertionTime, new_freq);
                        int ret = hashTable_[bucketIdx + i].compare_exchange_weak(valInTable, newVal, std::memory_order_relaxed);
                        if(ret){
                            if(new_freq ==2){
                                freq_distribution_[2].fetch_add(1, std::memory_order_relaxed);
                            }
                            if(new_freq ==1){
                                freq_distribution_[1].fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                        return ori_freq + 1;
                    }
                }
                return 0;
            }

            void insert2(uint32_t key, int freq = 0) noexcept
            {
                int64_t currTime = numInserts_++;
                if (currTime > MAX_VALUE)
                {
                    numInserts_ = 0;
                    currTime = 0;
                }
                size_t bucketIdx = getBucketIdx(key);
                uint64_t hashTableVal = genHashtableVal(key, currTime, 0);

                 for (size_t i = 0; i < nItemPerBucket_; i++) {
                    uint64_t valInTable = hashTable_[bucketIdx + i].load(std::memory_order_relaxed);
                    if (valInTable == 0) {
                        if(hashTable_[bucketIdx + i].compare_exchange_weak(valInTable, hashTableVal, std::memory_order_relaxed)) {
                            return;
                        }
                    }
                }
                // we do not find an empty slots, random choose and overwrite one
                hashTable_[key % numElem_].store(hashTableVal, std::memory_order_relaxed);
                //__atomic_store_n(&hashTable_[key % numElem_], hashTableVal, __ATOMIC_RELAXED);
                return;
            }

            void insert(uint32_t key, int freq = 0) noexcept
            {
                int64_t currTime = numInserts_;
                freq++;
                // reset if overflow
                if (currTime > MAX_VALUE)
                {
                    numInserts_ = 0;
                    currTime = 0;
                }
                size_t bucketIdx = getBucketIdx(key);
                // find and increase
                int empty_slot = -1;
                for (int i = 0; i < nItemPerBucket_; i++)
                {//find a match or an empty slot
                    uint64_t valInTable = hashTable_[bucketIdx + i].load(std::memory_order_relaxed);
                    if (valInTable == 0)
                    {
                        empty_slot = i;
                        continue;
                    }
                    int64_t insertionTime = getInsertionTime(valInTable);
                    int64_t age = currTime - insertionTime;

                    if (age > fifoSize_)
                    {
                        int ret = hashTable_[bucketIdx + i].compare_exchange_weak(valInTable, 0, std::memory_order_relaxed);
                        if (ret)
                        {
                            int ori_freq = getFreq(valInTable);
                            if(ori_freq >= 2){
                                freq_distribution_[2].fetch_sub(1, std::memory_order_relaxed);
                            }
                            if(ori_freq >= 1){
                                freq_distribution_[1].fetch_sub(1, std::memory_order_relaxed);
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
                        int ret = hashTable_[bucketIdx + i].compare_exchange_weak(valInTable, newVal, std::memory_order_relaxed);
                        if (ret)
                        { // success update freq only once
                            if(ori_freq< 2 &&new_freq >=2){
                                freq_distribution_[2].fetch_add(1, std::memory_order_relaxed);
                            }
                            if(ori_freq< 1 &&new_freq >=1){
                                freq_distribution_[1].fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                        return;
                    }
                }
                // not find, insert
                freq--;
                currTime = numInserts_++;
                uint64_t hashTableVal = genHashtableVal(key, static_cast<uint32_t>(currTime), freq);
                // attempt only once
                if (empty_slot == -1)
                {//random evict
                    size_t evictIdx = key % numElem_;
                    bool success = false;
                    uint64_t valInTable = hashTable_[evictIdx].load(std::memory_order_relaxed);
                    success = hashTable_[evictIdx].compare_exchange_weak(valInTable, hashTableVal, std::memory_order_relaxed);
                    if(success){
                        int ori_freq = getFreq(valInTable);
                        if(ori_freq >= 2){
                            freq_distribution_[2].fetch_sub(1, std::memory_order_relaxed);
                        }
                        if(ori_freq >= 1){
                            freq_distribution_[1].fetch_sub(1, std::memory_order_relaxed);
                        }
                        if(freq >= 2){
                            freq_distribution_[2].fetch_add(1, std::memory_order_relaxed);
                        }
                        if(freq >= 1){
                            freq_distribution_[1].fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                else
                {
                    uint64_t emptyVal = 0;
                    bool ret = hashTable_[bucketIdx + empty_slot].compare_exchange_weak(emptyVal, hashTableVal, std::memory_order_relaxed);
                    if (ret)
                    {
                        if(freq >= 2){
                            freq_distribution_[2].fetch_add(1, std::memory_order_relaxed);
                        }
                        if(freq >= 1){
                            freq_distribution_[1].fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                return;
            }

        private:
            size_t getBucketIdx(uint32_t key)
            {
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

            static constexpr size_t loadFactorInv_{2};
            static constexpr size_t nItemPerBucket_{8};
            static constexpr size_t bucketIdxMask_{0xFFFFFFFFFFFFFFF8};

            constexpr static uint64_t keyMask_ = 0x00000000FFFFFFFF;
            constexpr static uint64_t valueMask_ = 0x0FFFFFFF00000000;
            constexpr static uint64_t freqMask_ = 0xF000000000000000;
            constexpr static uint64_t MAX_VALUE = 0x0FFFFFFF;

        public:
            alignas(64) std::atomic<size_t> freq_distribution_[MAX_FREQ + 1];

            alignas(64) std::atomic<bool> initialized_{false};

        private:
            alignas(64) size_t numElem_{0};
            size_t fifoSize_{0};
            alignas(64) std::atomic<int64_t> numInserts_{0};
            alignas(64) std::unique_ptr<std::atomic<uint64_t>[]> hashTable_{nullptr};
        };

    } // namespace cachelib
} // namespace facebook

// #include "cachelib/allocator/datastruct/AtomicFIFOHashTable-inl.h"
