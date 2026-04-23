#pragma once

#include <folly/MPMCQueue.h>
#include <folly/logging/xlog.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include "cachelib/allocator/serialize/gen-cpp2/objects_types.h"
#pragma GCC diagnostic pop

#include "cachelib/allocator/datastruct/DList.h"
#include "cachelib/common/CompilerUtils.h"

#include <folly/lang/Aligned.h>
#include <folly/synchronization/DistributedMutex.h>

#include "cachelib/allocator/datastruct/MerlinFIFO.h"
#include "cachelib/allocator/datastruct/AtomicFIFOHashTable.h"
#include "cachelib/allocator/datastruct/MerlinSketch.h"
#include "cachelib/allocator/datastruct/MerlinGhost.h"
#include "cachelib/allocator/datastruct/DList.h"
#include "cachelib/common/BloomFilter.h"
#include "cachelib/common/CompilerUtils.h"
#include "cachelib/common/Mutex.h"
#include "cachelib/allocator/datastruct/merlinconfig.h"

namespace facebook::cachelib
{

    /*
     * CacheLib-side Merlin implementation.
     *
     * MerlinList keeps three resident FIFO queues per shard:
     *   filter  - first-touch queue for new items.
     *   staging - probationary queue for items with weak but nonzero evidence.
     *   core    - protected queue for items above the adaptive hotness threshold.
     *
     * Each primary queue has a paired "cuckoo" queue. Insertions first try the
     * primary queue and then the paired queue to reduce head-lock contention in
     * multi-threaded CacheLib benchmarks. Ghost and sketch metadata are kept per
     * primary shard so historical popularity can be consulted without storing
     * evicted objects themselves.
     */
    template <typename T, MerlinFIFOHook<T> T::*HookPtr>
    class MerlinList
    {
    public:
        using Mutex = folly::DistributedMutex;
        using LockHolder = std::unique_lock<Mutex>;
        using CompressedPtrType = typename T::CompressedPtrType;
        using PtrCompressor = typename T::PtrCompressor;
        using ADList = MerlinFIFO<T, HookPtr>;
        using RefFlags = typename T::Flags;
        using MerlinListObject = serialization::MerlinListObject;

#define TypeMask 0x3
#define FreqMask 0x7
        using Value = uint32_t;
#define kTypeMask (TypeMask << RefFlags::kMMFlag0)
#define kFreqMask (FreqMask << RefFlags::kMMFlag2)
        struct alignas(64) QueueInfo
        {
            // Per-shard queues and metadata. Keeping these independent reduces
            // contention and lets each shard adapt its threshold locally.
            alignas(64) std::unique_ptr<ADList> filterfifo_;
            std::unique_ptr<ADList> corefifo_;
            std::unique_ptr<ADList> stagingfifo_;
            MerlinGhost ghost_;
            MerlinSketch sketch_;
            // Cumulative frequency counters used to approximate how many
            // objects meet each hotness threshold in this shard.
            alignas(64) std::atomic<int64_t> freq_distribution_[MAX_FREQ + 1];
            alignas(64) std::atomic<uint64_t> hotness_threshold_;
            alignas(64) size_t filter_size_;
            size_t core_size_;
            size_t staging_size_;
            size_t total_size_;
            char padding_[64 - sizeof(uint64_t)];
            QueueInfo()
            {
                filterfifo_ = nullptr;
                corefifo_ = nullptr;
                stagingfifo_ = nullptr;
                for (int i = 0; i < MAX_FREQ; i++)
                {
                    freq_distribution_[i].store(0, std::memory_order_relaxed);
                }
            }
        };

        struct alignas(64) ThreadInfo
        {
            // Thread-local admission state. The hotness threshold is refreshed
            // periodically from the target shard and its paired cuckoo shard.
            uint64_t numInserts_;
            int retry_count_;
            int target_queue_;
            int total_threads_;
            alignas(64) uint64_t hotness_threshold_;
            char padding_[64 - sizeof(uint64_t) - sizeof(size_t) * 4 - sizeof(std::vector<std::atomic<uint64_t>>)];
            ThreadInfo()
            {
                numInserts_ = 0;
                retry_count_ = 0;
                target_queue_ = -1;
                hotness_threshold_ = 1;
                total_threads_ = 1;
            }
        };

        void setFilter(T &node) noexcept
        {
            node.template setFlag<RefFlags::kMMFlag0>();
        }
        void unSetFilter(T &node) noexcept
        {
            node.template unSetFlag<RefFlags::kMMFlag0>();
        }
        bool isFilter(const T &node) const noexcept
        {
            return node.template isFlagSet<RefFlags::kMMFlag0>();
        }
        void setCore(T &node) noexcept
        {
            node.template setFlag<RefFlags::kMMFlag1>();
        }
        void unSetCore(T &node) noexcept
        {
            node.template unSetFlag<RefFlags::kMMFlag1>();
        }
        bool isCore(const T &node) const noexcept
        {
            return node.template isFlagSet<RefFlags::kMMFlag1>();
        }
        void setCuckoo(T &node) noexcept
        {
            // cuckoo the power of two queues
            // to avoid contention on the same queue
            node.template setFlag<RefFlags::kMMFlag5>();
        }
        void unSetCuckoo(T &node) noexcept
        {
            node.template unSetFlag<RefFlags::kMMFlag5>();
        }
        bool isCuckoo(const T &node) const noexcept
        {
            return node.template isFlagSet<RefFlags::kMMFlag5>();
        }

        inline int getQueueID(T &node, int thread_id) noexcept
        {
            // The first half of queue_info_ stores primary shards; the second
            // half stores paired cuckoo shards. A node keeps the cuckoo bit when
            // it was inserted into the paired shard so removal can find it.
            uint32_t hashed_key = hashNode(node);
            int queue_id = (hashed_key) % thread_info_[thread_id].total_threads_;
            if (isCuckoo(node))
            {
                queue_id += thread_info_[thread_id].total_threads_;
            }
            return queue_id;
        }

        FOLLY_ALWAYS_INLINE int incFreq(T &node, int thread_id) noexcept
        {
            // Increment the bounded Merlin frequency encoded in CacheLib's ref
            // flags and update the shard-level cumulative counters.
            int queue_id = getQueueID(node, thread_id);
            int res = 0;
            auto predicate = [&res](const Value curValue)
            {
                res = curValue & kFreqMask;
                if (res == kFreqMask)
                {
                    return false;
                }
                return true;
            };

            auto newValue = [](const Value curValue)
            {
                return (curValue + (static_cast<Value>(1) << RefFlags::kMMFlag2));
            };

            node.ref_.template atomicUpdateValue(predicate, newValue);
            int retval = (res >> RefFlags::kMMFlag2) + 1;
            if (retval == 2)
            {
                queue_info_[queue_id].freq_distribution_[retval].fetch_add(1, std::memory_order_relaxed);
            }
            if (retval == 1)
            {
                queue_info_[queue_id].freq_distribution_[retval].fetch_add(1, std::memory_order_relaxed);
            }
            return retval;
        }

        FOLLY_ALWAYS_INLINE int decFreq(T &node, int thread_id) noexcept
        {
            // Decrement the encoded frequency. The caller uses -1 to mean the
            // object has fallen below probation and can be evicted.
            int queue_id = getQueueID(node, thread_id);
            int res = 0;
            auto predicate = [&res](const Value curValue)
            {
                res = curValue & kFreqMask;
                if (res == 0)
                {
                    return false;
                }
                return true;
            };

            auto newValue = [](const Value curValue)
            {
                return (curValue - (static_cast<Value>(1) << RefFlags::kMMFlag2));
            };

            node.ref_.template atomicUpdateValue(predicate, newValue);
            int retval = (res >> RefFlags::kMMFlag2);
            if (retval == 2)
            {
                queue_info_[queue_id].freq_distribution_[retval].fetch_sub(1, std::memory_order_relaxed);
            }
            if (retval == 1)
            {
                queue_info_[queue_id].freq_distribution_[retval].fetch_sub(1, std::memory_order_relaxed);
            }
            return retval - 1;
        }

        FOLLY_ALWAYS_INLINE int getFreq(const T &node) const noexcept
        {
            // Frequency is packed into the same atomic ref-count word used by
            // CacheLib item metadata.
            int ret = __atomic_load_n(&node.ref_.refCount_, __ATOMIC_RELAXED);
            return (ret >> RefFlags::kMMFlag2) & (MAX_FREQ);
        }

        FOLLY_ALWAYS_INLINE int clearFreq(T &node, int thread_id) noexcept
        {
            // Clear frequency to zero and subtract the old contribution from
            // the cumulative counters.
            int queue_id = getQueueID(node, thread_id);
            int res = 0;
            auto predicate = [&res](const Value curValue)
            {
                res = curValue & kFreqMask;
                if (res == 0)
                {
                    return false;
                }
                return true;
            };

            auto newValue = [](const Value curValue)
            {
                return (curValue & (~kFreqMask));
            };

            node.ref_.template atomicUpdateValue(predicate, newValue);
            int retval = (res >> RefFlags::kMMFlag2);
            if (retval >= 2)
            {
                queue_info_[queue_id].freq_distribution_[2].fetch_sub(1, std::memory_order_relaxed);
            }
            if (retval >= 1)
            {
                queue_info_[queue_id].freq_distribution_[1].fetch_sub(1, std::memory_order_relaxed);
            }
            return retval - 1;
        }

        void resetFreq(T &node) noexcept
        {
            // Reset a new or reinserted node's frequency without touching
            // counters; callers use this when the old counter state was already
            // accounted for elsewhere.
            constexpr Value bitMask =
                std::numeric_limits<Value>::max() - kFreqMask;
            __atomic_and_fetch(&node.ref_.refCount_, bitMask, __ATOMIC_ACQ_REL);
            return;
        }

        void setFreq(T &node, int freq, int thread_id) noexcept
        {
            // Install a historical frequency, usually after a ghost hit, and
            // add its contribution to the shard counters.
            int queue_id = getQueueID(node, thread_id);
            resetFreq(node);
            Value bitMask = ((static_cast<Value>(freq) & MAX_FREQ) << RefFlags::kMMFlag2);
            __atomic_or_fetch(&node.ref_.refCount_, bitMask, __ATOMIC_ACQ_REL);
            if (freq >= 2)
            {
                queue_info_[queue_id].freq_distribution_[2].fetch_add(1, std::memory_order_relaxed);
            }
            if (freq >= 1)
            {
                queue_info_[queue_id].freq_distribution_[1].fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }

        MerlinList() = default;
        MerlinList(const MerlinList &) = delete;
        MerlinList &operator=(const MerlinList &) = delete;
        ~MerlinList()
        {
        }

        MerlinList(PtrCompressor compressor, int total_thread_num) noexcept
        {
            assert(total_thread_num <= THREAD_NUM);
            assert(total_thread_num * 2 <= QUEUE_NUM);
            for (int i = 0; i < QUEUE_NUM; i++)
            {
                queue_info_[i].filterfifo_ = std::make_unique<ADList>(compressor);
                queue_info_[i].corefifo_ = std::make_unique<ADList>(compressor);
                queue_info_[i].stagingfifo_ = std::make_unique<ADList>(compressor);
                queue_info_[i].hotness_threshold_.store(1, std::memory_order_relaxed);
            }
            for (int i = 0; i < THREAD_NUM; i++)
            {
                thread_info_[i].target_queue_ = i % total_thread_num;
                thread_info_[i].hotness_threshold_ = 1;
                thread_info_[i].total_threads_ = total_thread_num;
            }
        }

        // Restore MerlinList from saved state.
        //
        // @param object              Save MerlinList object
        // @param compressor          PtrCompressor object
        MerlinList(const MerlinListObject &object, PtrCompressor compressor)
        {
            int i = 0;
            for (auto &q : *object.filterfifo())
            {
                queue_info_[i].filterfifo_ = std::make_unique<ADList>(q, compressor);
                i++;
            }
            i = 0;
            for (auto &q : *object.corefifo())
            {
                queue_info_[i].corefifo_ = std::make_unique<ADList>(q, compressor);
                i++;
            }
            i = 0;
            for (auto &q : *object.stagingfifo())
            {
                queue_info_[i].stagingfifo_ = std::make_unique<ADList>(q, compressor);
                i++;
            }
            i = 0;
            for (auto &f : *object.hotness_threshold())
            {
                thread_info_[i].hotness_threshold_ = f;
                thread_info_[i].target_queue_ = i;
                i++;
            }
            i = 0;
            int j = 0;
            for (auto &f : *object.freq_distribution())
            {
                queue_info_[i].freq_distribution_[j].store(f, std::memory_order_relaxed);
                j++;
                if (j > MAX_FREQ)
                {
                    j = 0;
                    i++;
                }
            }
        }

        /**
         * Exports the current state as a thrift object for later restoration.
         */
        MerlinListObject saveState() const
        {
            MerlinListObject state;
            for (int i = 0; i < QUEUE_NUM; i++)
            {
                state.filterfifo()->emplace_back(queue_info_[i].filterfifo_->saveState());
            }
            for (int i = 0; i < QUEUE_NUM; i++)
            {
                state.corefifo()->emplace_back(queue_info_[i].corefifo_->saveState());
            }
            for (int i = 0; i < QUEUE_NUM; i++)
            {
                state.stagingfifo()->emplace_back(queue_info_[i].stagingfifo_->saveState());
            }
            for (int i = 0; i < THREAD_NUM; i++)
            {
                state.hotness_threshold()->emplace_back(thread_info_[i].hotness_threshold_);
            }
            for (int i = 0; i < QUEUE_NUM; i++)
            {
                for (int j = 0; j < MAX_FREQ; j++)
                {
                    state.freq_distribution()->emplace_back(queue_info_[i].freq_distribution_[j].load(std::memory_order_relaxed));
                }
            }
            return state;
        }

        inline size_t size() const noexcept
        {
            size_t total_size = 0;
            for (int i = 0; i < QUEUE_NUM; i++)
            {
                total_size += queue_info_[i].filterfifo_->size();
                total_size += queue_info_[i].corefifo_->size();
                total_size += queue_info_[i].stagingfifo_->size();
            }
            return total_size;
        }

        void updateLocal(int thread_id) noexcept
        {
            // Refresh the target shard's cached sizes and choose a local
            // hotness threshold from the resident and ghost frequency
            // distributions. The paired cuckoo shard is included because this
            // thread may insert there when the primary head lock is contended.
            int target_queue = thread_info_[thread_id].target_queue_;
            queue_info_[target_queue].filter_size_ = queue_info_[target_queue].filterfifo_->size();
            queue_info_[target_queue].core_size_ = queue_info_[target_queue].corefifo_->size();
            queue_info_[target_queue].staging_size_ = queue_info_[target_queue].stagingfifo_->size();
            queue_info_[target_queue].total_size_ = queue_info_[target_queue].filter_size_ + queue_info_[target_queue].core_size_ + queue_info_[target_queue].staging_size_;
            //also include the cuckoo queue info to make a better decision on the threshold
            int cuckoo_queue = target_queue + thread_info_[thread_id].total_threads_;
            queue_info_[cuckoo_queue].filter_size_ = queue_info_[cuckoo_queue].filterfifo_->size();
            queue_info_[cuckoo_queue].core_size_ = queue_info_[cuckoo_queue].corefifo_->size();
            queue_info_[cuckoo_queue].staging_size_ = queue_info_[cuckoo_queue].stagingfifo_->size();
            queue_info_[cuckoo_queue].total_size_ = queue_info_[cuckoo_queue].filter_size_ + queue_info_[cuckoo_queue].core_size_ + queue_info_[cuckoo_queue].staging_size_;
            // Ghost frequencies count recent evictions, so they influence the
            // threshold even when the objects are no longer resident.
            int freq1 = queue_info_[target_queue].freq_distribution_[1].load(std::memory_order_relaxed);
            int freq2 = queue_info_[target_queue].freq_distribution_[2].load(std::memory_order_relaxed);
            freq1 += queue_info_[target_queue].ghost_.freq_distribution_[1].load(std::memory_order_relaxed);
            freq2 += queue_info_[target_queue].ghost_.freq_distribution_[2].load(std::memory_order_relaxed);
            freq1 += queue_info_[cuckoo_queue].freq_distribution_[1].load(std::memory_order_relaxed);
            freq2 += queue_info_[cuckoo_queue].freq_distribution_[2].load(std::memory_order_relaxed);
            size_t queue_size = queue_info_[target_queue].total_size_ + queue_info_[cuckoo_queue].total_size_;
            //printf("thread %d total size %zu %zu\n", thread_id, queue_info_[target_queue].total_size_, queue_info_[cuckoo_queue].total_size_);
            // update the hotness threshold based on the freq distribution and the total size of the queue
            if (freq2 > queue_size)
            {
                thread_info_[thread_id].hotness_threshold_ = 3;
            }
            else if (freq1 > queue_size)
            {
                thread_info_[thread_id].hotness_threshold_ = 2;
            }
            else
            {
                thread_info_[thread_id].hotness_threshold_ = 1;
            }
            return;
        }

        T *getEviction_fromqueue(int target_queue, int thread_id, int ori, int cuckoo) noexcept
        {
            // Merlin's eviction state machine for one shard. It first manages
            // an oversized filter, then staging, then core. Returned nodes are
            // real eviction victims; other nodes may be reinserted into staging
            // or core as their frequency evidence changes.
            std::unique_ptr<ADList> &filterfifo = queue_info_[target_queue].filterfifo_;
            std::unique_ptr<ADList> &corefifo = queue_info_[target_queue].corefifo_;
            std::unique_ptr<ADList> &stagingfifo = queue_info_[target_queue].stagingfifo_;
            size_t listSize = queue_info_[target_queue].total_size_;

            MerlinSketch &sketch = queue_info_[ori].sketch_;
            MerlinGhost &ghost = queue_info_[ori].ghost_;
            T *curr = nullptr;
            for (int i = 0; i < MAX_RETRIES; )
            {
                if (queue_info_[target_queue].filter_size_ > ((double)(listSize)*filterRatio_))
                {
                    // Filter is above its configured share. Its FIFO tail must
                    // either graduate, move to staging, or leave only metadata.
                    curr = filterfifo->removeTail();
                    if (curr != nullptr)
                    {
                        queue_info_[target_queue].filter_size_--;
                        int curr_freq = getFreq(*curr);
                        // record the node in sketch for popularity estimation
                        sketch.estimate(hashNode(*curr)); 
                        if (curr_freq >= thread_info_[thread_id].hotness_threshold_)
                        {
                            // Hot filter victims bypass staging and enter core.
                            unSetFilter(*curr);
                            int ret = cuckooInsert(*curr, queue_info_[ori].corefifo_, queue_info_[cuckoo].corefifo_, target_queue == cuckoo);
                            setCore(*curr);
                            clearFreq(*curr, thread_id);
                            if (ret == 0)
                            {
                                queue_info_[ori].core_size_++;
                            }
                            else
                            {
                                queue_info_[cuckoo].core_size_++;
                            }
                            continue;
                        }
                        else
                        {
                            if (curr_freq >= 2)
                            {
                                queue_info_[target_queue].freq_distribution_[2].fetch_sub(1, std::memory_order_relaxed);
                            }
                            if (curr_freq >= 1)
                            {
                                queue_info_[target_queue].freq_distribution_[1].fetch_sub(1, std::memory_order_relaxed);
                            }
                            // Cold filter victims are recorded in ghost before
                            // eviction so future reappearances can be treated
                            // as history-aware insertions.
                            ghost.insert(hashNode(*curr), curr_freq);
                            
                            // If staging is also oversized, compare the filter
                            // victim against a cold staging tail. The sketch
                            // decides whether the filter victim deserves the
                            // probationary slot instead.
                            T *tailptr = nullptr;
                            if (queue_info_[target_queue].staging_size_ > std::min(32.0, (double)(listSize)*stagingRatio_))
                            {
                                tailptr = stagingfifo->removeTail();
                            }
                            int tail_freq = MAX_FREQ + 1;
                            if (tailptr != nullptr)
                            {
                                tail_freq = getFreq(*tailptr);
                                if (tail_freq == 0)
                                {
                                    int est_curr = sketch.getEstimate(hashNode(*curr));
                                    if (est_curr < 0x3f)
                                    {
                                        int est_tail = sketch.getEstimate(hashNode(*tailptr));
                                        if (est_curr > est_tail)
                                        {
                                            // Filter victim wins the sketch
                                            // comparison and replaces the unpopular
                                            // staging tail.
                                            unSetFilter(*curr);
                                            int ret = cuckooInsert(*curr, queue_info_[ori].stagingfifo_, queue_info_[cuckoo].stagingfifo_, target_queue == cuckoo);
                                            sketch.estimate(hashNode(*curr));
                                            resetFreq(*curr);
                                            return tailptr;
                                        }
                                    }
                                }
                                // The staging tail wins or cannot be compared;
                                // restore it and evict the filter victim.
                                cuckooInsert(*tailptr, queue_info_[ori].stagingfifo_, queue_info_[cuckoo].stagingfifo_, target_queue == cuckoo);
                            }
                            return curr;
                        }
                    }
                    i++;
                }
                // Staging is probationary. Objects with remaining frequency
                // budget graduate to core; fully cooled objects are evicted.
                if (queue_info_[target_queue].staging_size_ > std::min(32.0, (double)(listSize)*stagingRatio_))
                {
                    curr = stagingfifo->removeTail();
                    if (curr != nullptr)
                    {
                        queue_info_[target_queue].staging_size_--;
                        int retval = decFreq(*curr, thread_id);
                        if (retval == -1)
                        {
                            return curr;
                        }
                        else
                        {
                            int ret = cuckooInsert(*curr, queue_info_[ori].corefifo_, queue_info_[cuckoo].corefifo_, target_queue == cuckoo);
                            if (ret == 0)
                            {
                                queue_info_[ori].core_size_++;
                            }
                            else
                            {
                                queue_info_[cuckoo].core_size_++;
                            }
                            setCore(*curr);
                            sketch.estimate(hashNode(*curr));
                            continue;
                        }
                    }
                    i++;
                }
                // Core protects hot objects. A core tail gets one frequency
                // decrement; only objects whose frequency drops below zero are
                // returned as victims.
                curr = corefifo->removeTail();
                if (curr != nullptr)
                {
                    queue_info_[target_queue].core_size_--;
                    int retval = decFreq(*curr, thread_id);
                    if (retval == -1)
                    {
                        return curr;
                    }
                    else
                    {
                        queue_info_[target_queue].core_size_++;
                        int ret = cuckooInsert(*curr, queue_info_[ori].corefifo_, queue_info_[cuckoo].corefifo_, target_queue == cuckoo);
                        if (ret == 0)
                        {
                            queue_info_[ori].core_size_++;
                        }
                        else
                        {
                            queue_info_[cuckoo].core_size_++;
                        }
                        continue;
                    }
                }
                i++;
            }
            return nullptr;
        }

        T *getEviction_threadlocal(int thread_id) noexcept
        {
            // Evict from the larger of the primary shard and its cuckoo pair to
            // keep the two queues balanced.
            int queue_id = thread_info_[thread_id].target_queue_;
            int cuckoo_queue_id = queue_id + thread_info_[thread_id].total_threads_;
            T *curr = nullptr;
            while (true)
            {
                if(queue_info_[queue_id].total_size_ > queue_info_[cuckoo_queue_id].total_size_)
                {
                    curr = getEviction_fromqueue(queue_id, thread_id, queue_id, cuckoo_queue_id);
                }
                else
                {
                    curr = getEviction_fromqueue(cuckoo_queue_id, thread_id, queue_id, cuckoo_queue_id);
                }
                if (curr != nullptr)
                {
                    return curr;
                }
            }
            return nullptr;
        }

        T *getEvictionCandidate(int thread_id) noexcept
        {
            T *ret = nullptr;
            if (!initialized_gs)
            {
                LockHolder l(*mtx_);
                if (!initialized_gs)
                {
                    // Lazily size ghost and sketch metadata from the current
                    // resident population. This avoids allocating large
                    // metadata tables before the cache has warmed up.
                    size_t listSize = size();
                    size_t average_size = (listSize / thread_info_[thread_id].total_threads_) + 1;
                    for (int i = 0; i < thread_info_[thread_id].total_threads_; i++)
                    {
                        size_t queue_size = queue_info_[i].filterfifo_->size() + queue_info_[i].corefifo_->size() + queue_info_[i].stagingfifo_->size();
                        int cuckoo_queue_id = i + thread_info_[thread_id].total_threads_;
                        size_t cuckoo_queue_size = queue_info_[cuckoo_queue_id].filterfifo_->size() + queue_info_[cuckoo_queue_id].corefifo_->size() + queue_info_[cuckoo_queue_id].stagingfifo_->size();
                        queue_size += cuckoo_queue_size;
                        queue_info_[i].ghost_.setFIFOSize(std::max(average_size, queue_size));
                        queue_info_[i].ghost_.initHashtable();
                        queue_info_[i].sketch_.setFIFOSize(std::max(average_size, queue_size));
                        queue_info_[i].sketch_.initHashtable();
                    }
                    initialized_gs.store(true, std::memory_order_release);
                }
            }

            
            if (thread_info_[thread_id].numInserts_ % 1024 == 0)
            {
                // Refresh thresholds periodically instead of on every request.
                updateLocal(thread_id);
            }
            thread_info_[thread_id].numInserts_++;

            while (ret == nullptr)
            {
                ret = getEviction_threadlocal(thread_id);
            }
            return ret;
        }

        int inline cuckooInsert(T &node, std::unique_ptr<ADList> &fifo, std::unique_ptr<ADList> &cuckoofifo, bool cuckoofirst = 0) noexcept
        {
            while (cuckoofirst == 0)
            {
                // Try the preferred queue first, then the paired cuckoo queue.
                // The cuckoo bit records where the node actually landed.
                if (fifo->try_lock_head())
                {
                    fifo->linkAtHead(node);
                    fifo->unlock_head();
                    return 0;
                }
                if (cuckoofifo->try_lock_head())
                {
                    cuckoofifo->linkAtHead(node);
                    cuckoofifo->unlock_head();
                    setCuckoo(node);
                    return 1;
                }
            }
            while (cuckoofirst == 1)
            {
                if (cuckoofifo->try_lock_head())
                {
                    cuckoofifo->linkAtHead(node);
                    cuckoofifo->unlock_head();
                    setCuckoo(node);
                    return 1;
                }
                if (fifo->try_lock_head())
                {
                    fifo->linkAtHead(node);
                    fifo->unlock_head();
                    return 0;
                }
            }
            return -1;
        }

        void add(T &node, int thread_id) noexcept
        {
            // Admission path. Ghost hits restore historical frequency and may
            // enter staging or core; true first-seen objects enter filter.
            int queue_id = getQueueID(node, thread_id);
            int cuckoo_queue_id = queue_id + thread_info_[thread_id].total_threads_;
            std::unique_ptr<ADList> &filterfifo = queue_info_[queue_id].filterfifo_;
            std::unique_ptr<ADList> &corefifo = queue_info_[queue_id].corefifo_;
            std::unique_ptr<ADList> &stagingfifo = queue_info_[queue_id].stagingfifo_;
            std::unique_ptr<ADList> &cuckoofilter = queue_info_[cuckoo_queue_id].filterfifo_;
            std::unique_ptr<ADList> &cuckoocore = queue_info_[cuckoo_queue_id].corefifo_;
            std::unique_ptr<ADList> &cuckoostaging = queue_info_[cuckoo_queue_id].stagingfifo_;
            auto &ghost_ = queue_info_[queue_id%thread_info_[thread_id].total_threads_].ghost_;
            
            int hist_freq = 0;
            if (ghost_.initialized())
            {
                // Query recent eviction history before deciding placement.
                hist_freq = ghost_.contains(hashNode(node));
            }
            if (hist_freq > 0)
            {
                if (hist_freq >= thread_info_[thread_id].hotness_threshold_)
                {
                    cuckooInsert(node, corefifo, cuckoocore);
                    setCore(node);
                    unSetFilter(node);
                    setFreq(node, hist_freq, thread_id);
                }
                else
                {
                    cuckooInsert(node, stagingfifo, cuckoostaging);
                    unSetCore(node);
                    unSetFilter(node);
                    resetFreq(node);
                }
            }
            else
            {
                cuckooInsert(node, filterfifo, cuckoofilter);
                setFilter(node);
                unSetCore(node);
                resetFreq(node);
            }
            return;
        }

        void remove(T &node, int thread_id) noexcept
        {
            printf("remove\n");
            int queue_id = getQueueID(node, thread_id);
            std::unique_ptr<ADList> &filterfifo = queue_info_[queue_id].filterfifo_;
            std::unique_ptr<ADList> &corefifo = queue_info_[queue_id].corefifo_;
            std::unique_ptr<ADList> &stagingfifo = queue_info_[queue_id].stagingfifo_;
            int freq = getFreq(node);
            if (isFilter(node))
            {
                filterfifo->remove(node);
            }
            else
            {
                if (isCore(node))
                {
                    corefifo->remove(node);
                }
                else
                {
                    stagingfifo->remove(node);
                }
            }
            if (freq >= 2)
            {
                queue_info_[queue_id].freq_distribution_[2].fetch_sub(1, std::memory_order_relaxed);
            }
            if (freq >= 1)
            {
                queue_info_[queue_id].freq_distribution_[1].fetch_sub(1, std::memory_order_relaxed);
            }
            return;
        }

    private:
        static uint32_t hashNode(const T &node) noexcept
        {
            return static_cast<uint32_t>(
                folly::hasher<folly::StringPiece>()(node.getKey()));
        }

        alignas(64) std::atomic<bool> initialized_gs{false};

        mutable folly::cacheline_aligned<Mutex> mtx_;

        constexpr static uint64_t MAX_VALUE = 0x0FFFFFFF;

        // configure the queue size.
        constexpr static double filterRatio_ = 0.1;
        constexpr static double coreRatio_ = 0.85;
        constexpr static double stagingRatio_ = 0.05;

        QueueInfo queue_info_[QUEUE_NUM];

        ThreadInfo thread_info_[THREAD_NUM];
    };

} // namespace facebook::cachelib
