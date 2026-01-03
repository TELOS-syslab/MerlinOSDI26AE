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

#include "cachelib/allocator/datastruct/MerlinAtomicDList.h"
#include "cachelib/allocator/datastruct/AtomicFIFOHashTable.h"
#include "cachelib/allocator/datastruct/AtomicSketch.h"
#include "cachelib/allocator/datastruct/AtomicFIFOSketch.h"
#include "cachelib/allocator/datastruct/DList.h"
#include "cachelib/common/BloomFilter.h"
#include "cachelib/common/CompilerUtils.h"
#include "cachelib/common/Mutex.h"

namespace facebook::cachelib
{

    template <typename T, MerlinAtomicDListHook<T> T::*HookPtr>
    class FLEXList
    {
    public:
        using Mutex = folly::DistributedMutex;
        using LockHolder = std::unique_lock<Mutex>;
        using CompressedPtrType = typename T::CompressedPtrType;
        using PtrCompressor = typename T::PtrCompressor;
        using ADList = MerlinAtomicDList<T, HookPtr>;
        using DEBUGList = MerlinAtomicDList<T, HookPtr>;
        using RefFlags = typename T::Flags;
        using FLEXListObject = serialization::FLEXListObject;

#define MAX_FREQ 7
#define TypeMask 0x3
#define FreqMask 0x7
#define THREAD_NUM 65
#define QUEUE_NUM 128
#define MAX_RETRIES 8
        using Value = uint32_t;
#define kTypeMask (TypeMask << RefFlags::kMMFlag0)
#define kFreqMask (FreqMask << RefFlags::kMMFlag2)
        struct alignas(64) QueueInfo
        {
            alignas(64) std::unique_ptr<ADList> smallfifo_;
            std::unique_ptr<ADList> mainfifo_;
            std::unique_ptr<ADList> susfifo_;
            alignas(64) std::atomic<uint64_t> freq_distribution_[MAX_FREQ];
            char padding_[64 - sizeof(std::unique_ptr<ADList>) * 3];
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
            size_t relaxed_small_size_;
            size_t relaxed_main_size_;
            size_t relaxed_sus_size_;
            int retry_count_;
            int target_queue_;
            alignas(64) uint64_t guard_freq_;
            char padding_[64 - sizeof(uint64_t) - sizeof(size_t) * 4 - sizeof(std::vector<std::atomic<uint64_t>>)];
            ThreadInfo()
            {
                numInserts_ = 0;
                relaxed_small_size_ = 0;
                relaxed_main_size_ = 0;
                relaxed_sus_size_ = 0;
                retry_count_ = 0;
                target_queue_ = -1;
                guard_freq_ = 1;
            }
        };
        alignas(64) std::unique_ptr<DEBUGList> debugsmallfifo_;
        std::unique_ptr<DEBUGList> debugmainfifo_;
        std::unique_ptr<DEBUGList> debugsusfifo_;

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
        void unSetType(T &node) noexcept
        {
            constexpr Value bitMask =
                std::numeric_limits<Value>::max() - kTypeMask;
            __atomic_and_fetch(&node.ref_.refCount_, bitMask, __ATOMIC_ACQ_REL);
        }
        FOLLY_ALWAYS_INLINE int getType(const T &node) const noexcept
        {
            // not used
            assert(0);
            int ret = __atomic_load_n(&node.ref_.refCount_, __ATOMIC_RELAXED);
            return (ret >> RefFlags::kMMFlag0) & TypeMask;
        }
        void setType(T &node, int type) noexcept
        {
            // not used
            return;
            // unSetType(node);
            Value bitMask = ((static_cast<Value>(type) & TypeMask) << RefFlags::kMMFlag0);
            __atomic_or_fetch(&node.ref_.refCount_, bitMask, __ATOMIC_ACQ_REL);
        }
        FOLLY_ALWAYS_INLINE int incFreq(T &node) noexcept
        {
            // node.template setFlag<RefFlags::kMMFlag2>();
            // return 1;
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
                // freq_distribution_[retval].fetch_add(1, std::memory_order_relaxed);
            }
            if (retval == 1)
            {
                // freq_distribution_[retval].fetch_add(1, std::memory_order_relaxed);
            }
            return retval;
        }
        FOLLY_ALWAYS_INLINE int decFreq(T &node) noexcept
        {
            // int ret = node.template isFlagSet<RefFlags::kMMFlag2>();
            // node.template unSetFlag<RefFlags::kMMFlag2>();
            // return ret - 1;
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
                // freq_distribution_[retval].fetch_sub(1, std::memory_order_relaxed);
            }
            if (retval == 1)
            {
                // freq_distribution_[retval].fetch_sub(1, std::memory_order_relaxed);
            }
            return retval - 1;
        }
        FOLLY_ALWAYS_INLINE int getFreq(const T &node) const noexcept
        {
            // return node.template isFlagSet<RefFlags::kMMFlag2>();
            int ret = __atomic_load_n(&node.ref_.refCount_, __ATOMIC_RELAXED);
            return (ret >> RefFlags::kMMFlag2) & (MAX_FREQ);
        }
        FOLLY_ALWAYS_INLINE int clearFreq(T &node) noexcept
        {
            // int ret = node.template isFlagSet<RefFlags::kMMFlag2>();
            // node.template unSetFlag<RefFlags::kMMFlag2>();
            // return ret - 1;
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
                // freq_distribution_[2].fetch_sub(1, std::memory_order_relaxed);
            }
            if (retval >= 1)
            {
                // freq_distribution_[1].fetch_sub(1, std::memory_order_relaxed);
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
        void setFreq(T &node, int freq) noexcept
        {
            // node.template setFlag<RefFlags::kMMFlag2>();
            // assert ori freq = 0;
            resetFreq(node);
            Value bitMask = ((static_cast<Value>(freq) & MAX_FREQ) << RefFlags::kMMFlag2);
            __atomic_or_fetch(&node.ref_.refCount_, bitMask, __ATOMIC_ACQ_REL);
            if (freq >= 2)
            {
                // freq_distribution_[2].fetch_add(1, std::memory_order_relaxed);
            }
            if (freq >= 1)
            {
                // freq_distribution_[1].fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }

        FLEXList() = default;
        FLEXList(const FLEXList &) = delete;
        FLEXList &operator=(const FLEXList &) = delete;
        ~FLEXList()
        {
        }

        FLEXList(PtrCompressor compressor) noexcept
        {
            for (int i = 0; i < QUEUE_NUM; i++)
            {
                queue_info_[i].smallfifo_ = std::make_unique<ADList>(compressor);
                queue_info_[i].mainfifo_ = std::make_unique<ADList>(compressor);
                queue_info_[i].susfifo_ = std::make_unique<ADList>(compressor);
            }
            debugsmallfifo_ = std::make_unique<DEBUGList>(compressor);
            debugmainfifo_ = std::make_unique<DEBUGList>(compressor);
            debugsusfifo_ = std::make_unique<DEBUGList>(compressor);
            for (int i = 0; i < THREAD_NUM; i++)
            {
                thread_info_[i].target_queue_ = i;
                //thread_info_[i].target_queue_ = 0;
            }
        }

        // Restore FLEXList from saved state.
        //
        // @param object              Save FLEXList object
        // @param compressor          PtrCompressor object
        FLEXList(const FLEXListObject &object, PtrCompressor compressor)
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
        FLEXListObject saveState() const
        {
            FLEXListObject state;
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

        inline size_t debugsize() const noexcept
        {
            size_t total_size = 0;
            total_size += debugsmallfifo_->size();
            total_size += debugmainfifo_->size();
            total_size += debugsusfifo_->size();
            return total_size;
        }

        void adjustGuardFreq(int thread_id, int queue_id) noexcept
        {
            return;
            int total_size = size();
        }

        T *getEviction(ThreadInfo &thread_info, int queue_id, int thread_id) noexcept
        {
            std::unique_ptr<ADList> &smallfifo = queue_info_[queue_id].smallfifo_;
            std::unique_ptr<ADList> &mainfifo = queue_info_[queue_id].mainfifo_;
            std::unique_ptr<ADList> &susfifo = queue_info_[queue_id].susfifo_;
            // size_t listSize = thread_info.relaxed_main_size_ + thread_info.relaxed_small_size_ + thread_info.relaxed_sus_size_;
            size_t listSize = smallfifo->size() + mainfifo->size() + susfifo->size();
            if (listSize == 0)
            {
                return nullptr;
            }
            int retry = 0;
            T *curr = nullptr;
            while (true)
            {
                if (retry++ > MAX_RETRIES)
                {
                    // printf("thread_id %d queue_id %d retry %d exceeded MAX_RETRIES %d, listSize %zu, small size %zu, main size %zu, sus size %zu, guard freq %lu\n",thread_id, //thread_info.target_queue_, retry, MAX_RETRIES, listSize, smallfifo->size(), mainfifo->size(), susfifo->size(), thread_info.guard_freq_);
                    return nullptr;
                }
                // if (thread_info.relaxed_small_size_ > ((double)(listSize)*smallRatio_))
                if (smallfifo->size() > ((double)(listSize)*smallRatio_))
                {
                    curr = smallfifo->removeTail();
                    if (curr != nullptr)
                    {
                        if (getFreq(*curr) >= thread_info.guard_freq_)
                        {
                            unSetSmall(*curr);
                            mainfifo->linkAtHead(*curr);
                            setMain(*curr);
                            clearFreq(*curr);
                            continue;
                        }
                        else
                        {
                            ghost_.insert(hashNode(*curr), 0);
                            return curr;
                        }
                    }
                }
                curr = mainfifo->removeTail();
                if (curr == nullptr)
                {
                    continue;
                }
                int retval = decFreq(*curr);
                if (retval == -1)
                {
                    return curr;
                }
                else
                {
                    mainfifo->linkAtHead(*curr);
                }
            }
            return nullptr;
        }

        void switchToNest(int thread_id) noexcept
        {
            //return;
            do
            {
                thread_info_[thread_id].target_queue_ = (thread_info_[thread_id].target_queue_ * 73 + thread_id * 2 + 1) % QUEUE_NUM;
                int target_queue = thread_info_[thread_id].target_queue_;
                thread_info_[thread_id].relaxed_small_size_ = queue_info_[target_queue].smallfifo_->size();
                thread_info_[thread_id].relaxed_main_size_ = queue_info_[target_queue].mainfifo_->size();
                thread_info_[thread_id].relaxed_sus_size_ = queue_info_[target_queue].susfifo_->size();
            } while (thread_info_[thread_id].relaxed_small_size_ + thread_info_[thread_id].relaxed_main_size_ + thread_info_[thread_id].relaxed_sus_size_ == 0);
            //printf("thread %d switch to queue %d, small size %zu, main size %zu, sus size %zu\n", thread_id, thread_info_[thread_id].target_queue_, thread_info_[thread_id].relaxed_small_size_, thread_info_[thread_id].relaxed_main_size_, thread_info_[thread_id].relaxed_sus_size_);
            return;
        }

        T *debugEviction(int thread_id) noexcept
        {
            size_t listSize = debugsize();
            if (listSize == 0)
            {
                return nullptr;
            }

            T *curr = nullptr;
            if (!ghost_.initialized())
            {
                LockHolder l(*mtx_);
                if (!ghost_.initialized())
                {
                    size_t listSize = size();
                    ghost_.setFIFOSize(listSize);
                    ghost_.initHashtable();
                }
            }

            while (true)
            {
                if (debugsmallfifo_->size() > ((double)(listSize)*smallRatio_))
                {
                    curr = debugsmallfifo_->removeTail();
                    if (curr != nullptr)
                    {
                        if (getFreq(*curr) >= 1)
                        {
                            unSetSmall(*curr);
                            debugmainfifo_->linkAtHead(*curr);
                            setMain(*curr);
                            clearFreq(*curr);
                            continue;
                        }
                        else
                        {
                            ghost_.insert(hashNode(*curr), 0);
                            return curr;
                        }
                    }
                }
                curr = debugmainfifo_->removeTail();
                if (curr == nullptr)
                {
                    continue;
                }
                int retval = decFreq(*curr);
                if (retval == -1)
                {
                    return curr;
                }
                else
                {
                    debugmainfifo_->linkAtHead(*curr);
                }
            }
        }

        T *getEvictionCandidate(int thread_id) noexcept
        {
            //return debugEviction(thread_id);
            // printf("thread id %d\n",thread_id);
            /*
            size_t listSize = smallfifo_->size() + mainfifo_->size() + susfifo_->size();
            if (listSize == 0)
            {
                return nullptr;
            }
            */
            T *ret = nullptr;
            if (!sketch_.initialized())
            {
                LockHolder l(*mtx_);
                if (!sketch_.initialized())
                {
                    size_t listSize = size();
                    sketch_.setFIFOSize(listSize * 2);
                    sketch_.initHashtable();
                }
            }
            if (!ghost_.initialized())
            {
                LockHolder l(*mtx_);
                if (!ghost_.initialized())
                {
                    size_t listSize = size();
                    ghost_.setFIFOSize(listSize);
                    ghost_.initHashtable();
                }
            }

            thread_info_[thread_id].numInserts_++;
            if (thread_info_[thread_id].numInserts_ % 16384 == 0)
            {
                // adjustGuardFreq(thread_id);
                switchToNest(thread_id);
            }
            int retry_count = 0;
            while (ret == nullptr)
            {
                ret = getEviction(thread_info_[thread_id], thread_info_[thread_id].target_queue_, thread_id);
                if (ret == nullptr)
                {
                    switchToNest(thread_id);
                    retry_count++;
                    if (retry_count > MAX_RETRIES)
                    {
                        size_t totalsize = size();
                        if(totalsize == 0){
                            printf("total size is 0 in getEvictionCandidate %s %d\n", __func__, __LINE__);
                            return nullptr;
                        }
                    }
                }
            }
            return ret;
        }

        void debugadd(T &node) noexcept
        {
            if (ghost_.initialized() && ghost_.contains(hashNode(node)))
            {
                debugmainfifo_->linkAtHead(node);
                unSetSmall(node);
                setMain(node);
            }
            else
            {
                debugsmallfifo_->linkAtHead(node);
                setSmall(node);
                unSetMain(node);
            }
            return;
        }

        void add(T &node) noexcept
        {
            //return debugadd(node);
            uint32_t hashed_key = hashNode(node);
            int queue_id = (hashed_key) % QUEUE_NUM;
            //queue_id = 0;
            std::unique_ptr<ADList> &smallfifo = queue_info_[queue_id].smallfifo_;
            std::unique_ptr<ADList> &mainfifo = queue_info_[queue_id].mainfifo_;
            std::unique_ptr<ADList> &susfifo = queue_info_[queue_id].susfifo_;
            if (ghost_.initialized() && ghost_.contains(hashed_key))
            {
                mainfifo->linkAtHead(node);
                unSetSmall(node);
                setMain(node);
            }
            else
            {
                smallfifo->linkAtHead(node);
                setSmall(node);
                unSetMain(node);
            }
            return;
        }

        void debugremove(T &node, int thread_id) noexcept
        {
            int freq = getFreq(node);
            if (isSmall(node))
            {
                debugsmallfifo_->remove(node);
            }
            else
            {
                if (isMain(node))
                {
                    debugmainfifo_->remove(node);
                }
                else
                {
                    debugsusfifo_->remove(node);
                }
            }
            return;
        }

        void remove(T &node, int thread_id) noexcept
        {
            //return debugremove(node, thread_id);
            uint32_t hashed_key = hashNode(node);
            int queue_id = (hashed_key) % QUEUE_NUM;
            //queue_id = 0;
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
                // freq_distribution_[2].fetch_sub(1, std::memory_order_relaxed);
            }
            if (freq >= 1)
            {
                // freq_distribution_[1].fetch_sub(1, std::memory_order_relaxed);
            }
            return;
        }

    private:
        static uint32_t hashNode(const T &node) noexcept
        {
            return static_cast<uint32_t>(
                folly::hasher<folly::StringPiece>()(node.getKey()));
        }

        alignas(64) AtomicSketch sketch_;
        alignas(64) AtomicFIFOHashTable ghost_;
        // alignas(64) AtomicFIFOSketch ghost_;

        mutable folly::cacheline_aligned<Mutex> mtx_;

        constexpr static uint64_t MAX_VALUE = 0x0FFFFFFF;

        constexpr static double smallRatio_ = 0.1;
        constexpr static double susRatio_ = 0.05;
        constexpr static double mainRatio_ = 0.85;

        QueueInfo queue_info_[QUEUE_NUM];

        ThreadInfo thread_info_[THREAD_NUM];
    };

} // namespace facebook::cachelib