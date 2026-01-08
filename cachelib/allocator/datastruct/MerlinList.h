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
#include "cachelib/allocator/datastruct/AtomicSketch.h"
#include "cachelib/allocator/datastruct/AtomicFIFOSketch.h"
#include "cachelib/allocator/datastruct/DList.h"
#include "cachelib/common/BloomFilter.h"
#include "cachelib/common/CompilerUtils.h"
#include "cachelib/common/Mutex.h"
#include "cachelib/allocator/datastruct/merlinconfig.h"

namespace facebook::cachelib
{

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
            alignas(64) std::unique_ptr<ADList> smallfifo_;
            std::unique_ptr<ADList> mainfifo_;
            std::unique_ptr<ADList> susfifo_;
            AtomicFIFOSketch ghost_;
            AtomicSketch sketch_;
            alignas(64) std::atomic<int64_t> freq_distribution_[MAX_FREQ + 1];
            alignas(64) std::atomic<uint64_t> guard_freq_;
            alignas(64) size_t small_size_;
            size_t main_size_;
            size_t sus_size_;
            size_t total_size_;
            char padding_[64 - sizeof(uint64_t)];
            QueueInfo()
            {
                smallfifo_ = nullptr;
                mainfifo_ = nullptr;
                susfifo_ = nullptr;
                for (int i = 0; i < MAX_FREQ; i++)
                {
                    freq_distribution_[i].store(0, std::memory_order_relaxed);
                }
            }
        };
        struct alignas(64) ThreadInfo
        {
            uint64_t numInserts_;
            int retry_count_;
            int target_queue_;
            int total_threads_;
            alignas(64) uint64_t guard_freq_;
            char padding_[64 - sizeof(uint64_t) - sizeof(size_t) * 4 - sizeof(std::vector<std::atomic<uint64_t>>)];
            ThreadInfo()
            {
                numInserts_ = 0;
                retry_count_ = 0;
                target_queue_ = -1;
                guard_freq_ = 1;
                total_threads_ = 1;
            }
        };

        void setSmall(T &node) noexcept
        {
            node.template setFlag<RefFlags::kMMFlag0>();
        }
        void unSetSmall(T &node) noexcept
        {
            node.template unSetFlag<RefFlags::kMMFlag0>();
        }
        bool isSmall(const T &node) const noexcept
        {
            return node.template isFlagSet<RefFlags::kMMFlag0>();
        }
        void setMain(T &node) noexcept
        {
            node.template setFlag<RefFlags::kMMFlag1>();
        }
        void unSetMain(T &node) noexcept
        {
            node.template unSetFlag<RefFlags::kMMFlag1>();
        }
        bool isMain(const T &node) const noexcept
        {
            return node.template isFlagSet<RefFlags::kMMFlag1>();
        }
        void setCook(T &node) noexcept
        {
            node.template setFlag<RefFlags::kMMFlag5>();
        }
        void unSetCook(T &node) noexcept
        {
            node.template unSetFlag<RefFlags::kMMFlag5>();
        }
        bool isCook(const T &node) const noexcept
        {
            return node.template isFlagSet<RefFlags::kMMFlag5>();
        }

        inline int getQueueID(T &node, int thread_id) noexcept
        {
            uint32_t hashed_key = hashNode(node);
            int queue_id = (hashed_key) % thread_info_[thread_id].total_threads_;
            if (isCook(node))
            {
                queue_id += thread_info_[thread_id].total_threads_;
            }
            return queue_id;
        }
        FOLLY_ALWAYS_INLINE int incFreq(T &node, int thread_id) noexcept
        {
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
            // return node.template isFlagSet<RefFlags::kMMFlag2>();
            int ret = __atomic_load_n(&node.ref_.refCount_, __ATOMIC_RELAXED);
            return (ret >> RefFlags::kMMFlag2) & (MAX_FREQ);
        }

        FOLLY_ALWAYS_INLINE int clearFreq(T &node, int thread_id) noexcept
        {
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
            // node.template unSetFlag<RefFlags::kMMFlag2>();
            constexpr Value bitMask =
                std::numeric_limits<Value>::max() - kFreqMask;
            __atomic_and_fetch(&node.ref_.refCount_, bitMask, __ATOMIC_ACQ_REL);
            return;
        }

        void setFreq(T &node, int freq, int thread_id) noexcept
        {
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
            printf("Merlin received thread num %d\n", total_thread_num);
            for (int i = 0; i < QUEUE_NUM; i++)
            {
                queue_info_[i].smallfifo_ = std::make_unique<ADList>(compressor);
                queue_info_[i].mainfifo_ = std::make_unique<ADList>(compressor);
                queue_info_[i].susfifo_ = std::make_unique<ADList>(compressor);
                queue_info_[i].guard_freq_.store(1, std::memory_order_relaxed);
            }
            for (int i = 0; i < THREAD_NUM; i++)
            {
                thread_info_[i].target_queue_ = i % total_thread_num;
                thread_info_[i].guard_freq_ = 1;
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
            for (auto &q : *object.smallfifo())
            {
                queue_info_[i].smallfifo_ = std::make_unique<ADList>(q, compressor);
                i++;
            }
            i = 0;
            for (auto &q : *object.mainfifo())
            {
                queue_info_[i].mainfifo_ = std::make_unique<ADList>(q, compressor);
                i++;
            }
            i = 0;
            for (auto &q : *object.susfifo())
            {
                queue_info_[i].susfifo_ = std::make_unique<ADList>(q, compressor);
                i++;
            }
            i = 0;
            for (auto &f : *object.guard_freq())
            {
                thread_info_[i].guard_freq_ = f;
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
                state.smallfifo()->emplace_back(queue_info_[i].smallfifo_->saveState());
            }
            for (int i = 0; i < QUEUE_NUM; i++)
            {
                state.mainfifo()->emplace_back(queue_info_[i].mainfifo_->saveState());
            }
            for (int i = 0; i < QUEUE_NUM; i++)
            {
                state.susfifo()->emplace_back(queue_info_[i].susfifo_->saveState());
            }
            for (int i = 0; i < THREAD_NUM; i++)
            {
                state.guard_freq()->emplace_back(thread_info_[i].guard_freq_);
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

        // ADList &getListSmall() const noexcept { return *smallfifo_; }
        // ADList &getListMain() const noexcept { return *mainfifo_; }
        // ADList &getListSuspicious() const noexcept { return *susfifo_; }

        inline size_t size() const noexcept
        {
            size_t total_size = 0;
            for (int i = 0; i < QUEUE_NUM; i++)
            {
                total_size += queue_info_[i].smallfifo_->size();
                total_size += queue_info_[i].mainfifo_->size();
                total_size += queue_info_[i].susfifo_->size();
            }
            return total_size;
            // return smallfifo_->size() + mainfifo_->size() + susfifo_->size();
        }

        void adjustGuardFreq(int thread_id) noexcept
        {
            // TODO consider cook queue
        }

        void adjustGuardFreq(int thread_id, int queue_id, size_t queue_size) noexcept
        {
            // queue_id = thread_id;
            int freq1 = queue_info_[queue_id].freq_distribution_[1].load(std::memory_order_relaxed);
            int freq2 = queue_info_[queue_id].freq_distribution_[2].load(std::memory_order_relaxed);
            freq1 += queue_info_[queue_id].ghost_.freq_distribution_[1].load(std::memory_order_relaxed);
            freq2 += queue_info_[queue_id].ghost_.freq_distribution_[2].load(std::memory_order_relaxed);
            if (freq2 > queue_size)
            {
#ifdef ENABLE_GUARD_FREQ_LOG
                if (thread_info_[thread_id].guard_freq_ != 3)
                {
                    printf("set guard freq 3, queue size %zu, frq2 %d, ghost freq2 %d; freq1 %d, ghost freq1 %d\n",
                           queue_size,
                           queue_info_[queue_id].freq_distribution_[2].load(std::memory_order_relaxed),
                           queue_info_[queue_id].ghost_.freq_distribution_[2].load(std::memory_order_relaxed),
                           queue_info_[queue_id].freq_distribution_[1].load(std::memory_order_relaxed),
                           queue_info_[queue_id].ghost_.freq_distribution_[1].load(std::memory_order_relaxed));
                }
#endif
                thread_info_[thread_id].guard_freq_ = 3;
                queue_info_[queue_id].guard_freq_.store(3, std::memory_order_relaxed);
            }
            else if (freq1 > queue_size)
            {
#ifdef ENABLE_GUARD_FREQ_LOG
                if (thread_info_[thread_id].guard_freq_ != 2)
                {

                    printf("set guard freq 2, queue size %zu, frq2 %d, ghost freq2 %d; freq1 %d, ghost freq1 %d\n",
                           queue_size,
                           queue_info_[queue_id].freq_distribution_[2].load(std::memory_order_relaxed),
                           queue_info_[queue_id].ghost_.freq_distribution_[2].load(std::memory_order_relaxed),
                           queue_info_[queue_id].freq_distribution_[1].load(std::memory_order_relaxed),
                           queue_info_[queue_id].ghost_.freq_distribution_[1].load(std::memory_order_relaxed));
                }
#endif
                thread_info_[thread_id].guard_freq_ = 2;
                queue_info_[queue_id].guard_freq_.store(2, std::memory_order_relaxed);
            }
            else
            {
#ifdef ENABLE_GUARD_FREQ_LOG
                if (thread_info_[thread_id].guard_freq_ != 1)
                {

                    printf("set guard freq 1, queue size %zu, frq2 %d, ghost freq2 %d; freq1 %d, ghost freq1 %d\n",
                           queue_size,
                           queue_info_[queue_id].freq_distribution_[2].load(std::memory_order_relaxed),
                           queue_info_[queue_id].ghost_.freq_distribution_[2].load(std::memory_order_relaxed),
                           queue_info_[queue_id].freq_distribution_[1].load(std::memory_order_relaxed),
                           queue_info_[queue_id].ghost_.freq_distribution_[1].load(std::memory_order_relaxed));
                }
#endif
                thread_info_[thread_id].guard_freq_ = 1;
                queue_info_[queue_id].guard_freq_.store(1, std::memory_order_relaxed);
            }
            return;
        }

        void updateLocal(int thread_id) noexcept
        {
            int target_queue = thread_info_[thread_id].target_queue_;
            queue_info_[target_queue].small_size_ = queue_info_[target_queue].smallfifo_->size();
            queue_info_[target_queue].main_size_ = queue_info_[target_queue].mainfifo_->size();
            queue_info_[target_queue].sus_size_ = queue_info_[target_queue].susfifo_->size();
            queue_info_[target_queue].total_size_ = queue_info_[target_queue].small_size_ + queue_info_[target_queue].main_size_ + queue_info_[target_queue].sus_size_;
            int cook_queue = target_queue + thread_info_[thread_id].total_threads_;
            queue_info_[cook_queue].small_size_ = queue_info_[cook_queue].smallfifo_->size();
            queue_info_[cook_queue].main_size_ = queue_info_[cook_queue].mainfifo_->size();
            queue_info_[cook_queue].sus_size_ = queue_info_[cook_queue].susfifo_->size();
            queue_info_[cook_queue].total_size_ = queue_info_[cook_queue].small_size_ + queue_info_[cook_queue].main_size_ + queue_info_[cook_queue].sus_size_;
            int freq1 = queue_info_[target_queue].freq_distribution_[1].load(std::memory_order_relaxed);
            int freq2 = queue_info_[target_queue].freq_distribution_[2].load(std::memory_order_relaxed);
            freq1 += queue_info_[target_queue].ghost_.freq_distribution_[1].load(std::memory_order_relaxed);
            freq2 += queue_info_[target_queue].ghost_.freq_distribution_[2].load(std::memory_order_relaxed);
            freq1 += queue_info_[cook_queue].freq_distribution_[1].load(std::memory_order_relaxed);
            freq2 += queue_info_[cook_queue].freq_distribution_[2].load(std::memory_order_relaxed);
            size_t queue_size = queue_info_[target_queue].total_size_ + queue_info_[cook_queue].total_size_;
            //printf("thread %d total size %zu %zu\n", thread_id, queue_info_[target_queue].total_size_, queue_info_[cook_queue].total_size_);
            if (freq2 > queue_size)
            {
                thread_info_[thread_id].guard_freq_ = 3;
            }
            else if (freq1 > queue_size)
            {
                thread_info_[thread_id].guard_freq_ = 2;
            }
            else
            {
                thread_info_[thread_id].guard_freq_ = 1;
            }
            return;
        }

        T *getEviction(int target_queue, int thread_id, int ori, int cook) noexcept
        {
            std::unique_ptr<ADList> &smallfifo = queue_info_[target_queue].smallfifo_;
            std::unique_ptr<ADList> &mainfifo = queue_info_[target_queue].mainfifo_;
            std::unique_ptr<ADList> &susfifo = queue_info_[target_queue].susfifo_;
            size_t listSize = queue_info_[target_queue].total_size_;

            AtomicSketch &sketch = queue_info_[ori].sketch_;
            AtomicFIFOSketch &ghost = queue_info_[ori].ghost_;
            T *curr = nullptr;
            for (int i = 0; i < MAX_RETRIES; )
            {
                if (queue_info_[target_queue].small_size_ > ((double)(listSize)*smallRatio_))
                {
                    curr = smallfifo->removeTail();
                    if (curr != nullptr)
                    {
                        queue_info_[target_queue].small_size_--;
                        int curr_freq = getFreq(*curr);
                        sketch.estimate(hashNode(*curr));
                        if (curr_freq >= thread_info_[thread_id].guard_freq_)
                        {
                            unSetSmall(*curr);
                            int ret = cookInsert(*curr, queue_info_[ori].mainfifo_, queue_info_[cook].mainfifo_, target_queue == cook);
                            setMain(*curr);
                            clearFreq(*curr, thread_id);
                            if (ret == 0)
                            {
                                queue_info_[ori].main_size_++;
                            }
                            else
                            {
                                queue_info_[cook].main_size_++;
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
                            ghost.insert(hashNode(*curr), curr_freq);

                            T *tailptr = nullptr;
                            if (queue_info_[target_queue].sus_size_ > std::min(32.0, (double)(listSize)*susRatio_))
                            {
                                tailptr = susfifo->removeTail();
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
                                            unSetSmall(*curr);
                                            int ret = cookInsert(*curr, queue_info_[ori].susfifo_, queue_info_[cook].susfifo_, target_queue == cook);

                                            // set type LruType::Suspicious
                                            sketch.estimate(hashNode(*curr));
                                            resetFreq(*curr); //
                                            return tailptr;
                                        }
                                    }
                                }
                                cookInsert(*tailptr, queue_info_[ori].susfifo_, queue_info_[cook].susfifo_, target_queue == cook);
                            }
                            return curr;
                        }
                    }
                    i++;
                }
                if (queue_info_[target_queue].sus_size_ > std::min(32.0, (double)(listSize)*susRatio_))
                {
                    curr = susfifo->removeTail();
                    if (curr != nullptr)
                    {
                        queue_info_[target_queue].sus_size_--;
                        int retval = decFreq(*curr, thread_id);
                        if (retval == -1)
                        {
                            return curr;
                        }
                        else
                        {
                            int ret = cookInsert(*curr, queue_info_[ori].mainfifo_, queue_info_[cook].mainfifo_, target_queue == cook);
                            if (ret == 0)
                            {
                                queue_info_[ori].main_size_++;
                            }
                            else
                            {
                                queue_info_[cook].main_size_++;
                            }
                            setMain(*curr);
                            sketch.estimate(hashNode(*curr));
                            continue;
                        }
                    }
                    i++;
                }
                curr = mainfifo->removeTail();
                if (curr != nullptr)
                {
                    queue_info_[target_queue].main_size_--;
                    int retval = decFreq(*curr, thread_id);
                    if (retval == -1)
                    {
                        return curr;
                    }
                    else
                    {
                        queue_info_[target_queue].main_size_++;
                        int ret = cookInsert(*curr, queue_info_[ori].mainfifo_, queue_info_[cook].mainfifo_, target_queue == cook);
                        if (ret == 0)
                        {
                            queue_info_[ori].main_size_++;
                        }
                        else
                        {
                            queue_info_[cook].main_size_++;
                        }
                        continue;
                    }
                }
                i++;
            }
            return nullptr;
        }

        T *getEviction(int thread_id) noexcept
        {
            int queue_id = thread_info_[thread_id].target_queue_;
            int cook_queue_id = queue_id + thread_info_[thread_id].total_threads_;
            T *curr = nullptr;
            while (true)
            {
                if(queue_info_[queue_id].total_size_ > queue_info_[cook_queue_id].total_size_)
                {
                    curr = getEviction(queue_id, thread_id, queue_id, cook_queue_id);
                }
                else
                {
                    curr = getEviction(cook_queue_id, thread_id, queue_id, cook_queue_id);
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
                    size_t listSize = size();
                    size_t average_size = (listSize / thread_info_[thread_id].total_threads_) + 1;
                    for (int i = 0; i < thread_info_[thread_id].total_threads_; i++)
                    {
                        size_t queue_size = queue_info_[i].smallfifo_->size() + queue_info_[i].mainfifo_->size() + queue_info_[i].susfifo_->size();
                        int cook_queue_id = i + thread_info_[thread_id].total_threads_;
                        size_t cook_queue_size = queue_info_[cook_queue_id].smallfifo_->size() + queue_info_[cook_queue_id].mainfifo_->size() + queue_info_[cook_queue_id].susfifo_->size();
                        queue_size += cook_queue_size;
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
                //printf("Thread %d adjust guard freq\n", thread_id);
                updateLocal(thread_id);
            }
            thread_info_[thread_id].numInserts_++;

            int retry_count = 0;
            while (ret == nullptr)
            {
                ret = getEviction(thread_id);
            }
            return ret;
        }

        int inline cookInsert(T &node, std::unique_ptr<ADList> &fifo, std::unique_ptr<ADList> &cookfifo, bool cookfirst = 0) noexcept
        {
            while (cookfirst == 0)
            {
                if (fifo->try_lock_head())
                {
                    fifo->linkAtHead(node);
                    fifo->unlock_head();
                    return 0;
                }
                if (cookfifo->try_lock_head())
                {
                    cookfifo->linkAtHead(node);
                    cookfifo->unlock_head();
                    setCook(node);
                    return 1;
                }
            }
            while (cookfirst == 1)
            {
                if (cookfifo->try_lock_head())
                {
                    cookfifo->linkAtHead(node);
                    cookfifo->unlock_head();
                    setCook(node);
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
            int queue_id = getQueueID(node, thread_id);
            int cook_queue_id = queue_id + thread_info_[thread_id].total_threads_;
            std::unique_ptr<ADList> &smallfifo = queue_info_[queue_id].smallfifo_;
            std::unique_ptr<ADList> &mainfifo = queue_info_[queue_id].mainfifo_;
            std::unique_ptr<ADList> &susfifo = queue_info_[queue_id].susfifo_;
            std::unique_ptr<ADList> &cooksmall = queue_info_[cook_queue_id].smallfifo_;
            std::unique_ptr<ADList> &cookmain = queue_info_[cook_queue_id].mainfifo_;
            std::unique_ptr<ADList> &cooksus = queue_info_[cook_queue_id].susfifo_;
            auto &ghost_ = queue_info_[queue_id%thread_info_[thread_id].total_threads_].ghost_;
            
            int hist_freq = 0;
            if (ghost_.initialized())
            {
                hist_freq = ghost_.contains(hashNode(node));
            }
            if (hist_freq > 0)
            {
                if (hist_freq >= thread_info_[thread_id].guard_freq_)
                {
                    cookInsert(node, mainfifo, cookmain);
                    setMain(node);
                    unSetSmall(node);
                    setFreq(node, hist_freq, thread_id);
                }
                else
                {
                    cookInsert(node, susfifo, cooksus);
                    unSetMain(node);
                    unSetSmall(node);
                    resetFreq(node);
                }
            }
            else
            {
                cookInsert(node, smallfifo, cooksmall);
                setSmall(node);
                unSetMain(node);
                resetFreq(node);
            }
            return;
        }

        void remove(T &node, int thread_id) noexcept
        {
            printf("remove\n");
            int queue_id = getQueueID(node, thread_id);
            // queue_id = thread_id;
            // queue_id = 0;
            std::unique_ptr<ADList> &smallfifo = queue_info_[queue_id].smallfifo_;
            std::unique_ptr<ADList> &mainfifo = queue_info_[queue_id].mainfifo_;
            std::unique_ptr<ADList> &susfifo = queue_info_[queue_id].susfifo_;
            int freq = getFreq(node);
            if (isSmall(node))
            {
                smallfifo->remove(node);
            }
            else
            {
                if (isMain(node))
                {
                    mainfifo->remove(node);
                }
                else
                {
                    susfifo->remove(node);
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

        constexpr static double smallRatio_ = 0.1;
        constexpr static double susRatio_ = 0.05;
        constexpr static double mainRatio_ = 0.85;

        QueueInfo queue_info_[QUEUE_NUM];

        ThreadInfo thread_info_[THREAD_NUM];
    };

} // namespace facebook::cachelib