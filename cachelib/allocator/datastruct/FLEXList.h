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

#include "cachelib/allocator/datastruct/AtomicDList.h"
#include "cachelib/allocator/datastruct/AtomicFIFOHashTable.h"
#include "cachelib/allocator/datastruct/AtomicSketch.h"
#include "cachelib/allocator/datastruct/AtomicFIFOSketch.h"
#include "cachelib/allocator/datastruct/DList.h"
#include "cachelib/common/BloomFilter.h"
#include "cachelib/common/CompilerUtils.h"
#include "cachelib/common/Mutex.h"

namespace facebook::cachelib
{

    template <typename T, AtomicDListHook<T> T::*HookPtr>
    class FLEXList
    {
    public:
        using Mutex = folly::DistributedMutex;
        using LockHolder = std::unique_lock<Mutex>;
        using CompressedPtrType = typename T::CompressedPtrType;
        using PtrCompressor = typename T::PtrCompressor;
        using ADList = AtomicDList<T, HookPtr>;
        using RefFlags = typename T::Flags;
        using FLEXListObject = serialization::FLEXListObject;

        #define SCATTER_FREQ1 16
#define MAX_FREQ 7
#define TypeMask 0x3
#define FreqMask 0x7
        using Value = uint32_t;
        static constexpr Value kTypeMask = (TypeMask << RefFlags::kMMFlag0);
        static constexpr Value kFreqMask = (FreqMask << RefFlags::kMMFlag2);

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
                freq_distribution_[retval].fetch_add(1, std::memory_order_relaxed);
            }
            if(retval == 1){
                freq_distribution_[retval].fetch_add(1, std::memory_order_relaxed);
                //scatter_freq1_[retval].fetch_add(1, std::memory_order_relaxed);
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
                freq_distribution_[retval].fetch_sub(1, std::memory_order_relaxed);
            }
            if(retval == 1){
                freq_distribution_[retval].fetch_sub(1, std::memory_order_relaxed);
            }
            return retval - 1;
        }
        FOLLY_ALWAYS_INLINE int getFreq(const T &node) const noexcept
        {
            // return node.template isFlagSet<RefFlags::kMMFlag2>();
            int ret = __atomic_load_n(&node.ref_.refCount_, __ATOMIC_RELAXED);
            return (ret >> RefFlags::kMMFlag2) & (MAX_FREQ);
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

            resetFreq(node);
            Value bitMask = ((static_cast<Value>(freq) & MAX_FREQ) << RefFlags::kMMFlag2);
            __atomic_or_fetch(&node.ref_.refCount_, bitMask, __ATOMIC_ACQ_REL);
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
            smallfifo_ = std::make_unique<ADList>(compressor);
            mainfifo_ = std::make_unique<ADList>(compressor);
            susfifo_ = std::make_unique<ADList>(compressor);
        }

        // Restore FLEXList from saved state.
        //
        // @param object              Save FLEXList object
        // @param compressor          PtrCompressor object
        FLEXList(const FLEXListObject &object, PtrCompressor compressor)
        {
            smallfifo_ = std::make_unique<ADList>(*object.smallfifo(), compressor);
            mainfifo_ = std::make_unique<ADList>(*object.mainfifo(), compressor);
            susfifo_ = std::make_unique<ADList>(*object.susfifo(), compressor);
            guard_freq_ = *object.guard_freq();
            int i = 0;
            for (auto f : *object.freq_distribution())
            {
                freq_distribution_[i++] = f;
            }
        }

        /**
         * Exports the current state as a thrift object for later restoration.
         */
        FLEXListObject saveState() const
        {
            FLEXListObject state;
            *state.smallfifo() = smallfifo_->saveState();
            *state.mainfifo() = mainfifo_->saveState();
            *state.susfifo() = susfifo_->saveState();
            *state.guard_freq() = guard_freq_;
            for (auto &f : freq_distribution_)
            {
                state.freq_distribution()->emplace_back(f.load());
            }
            return state;
        }

        ADList &getListSmall() const noexcept { return *smallfifo_; }
        ADList &getListMain() const noexcept { return *mainfifo_; }
        ADList &getListSuspicious() const noexcept { return *susfifo_; }

        inline size_t size() const noexcept
        {
            return smallfifo_->size() + mainfifo_->size() + susfifo_->size();
        }

        void adjustGuardFreq() noexcept
        {
            if ((numInserts_ & 0xcff) != 0)
            {
                return;
            }

            int total_size = size();
            int freq1 = freq_distribution_[1].load(std::memory_order_relaxed);
            freq1 += ghost_.freq_distribution_[1].load(std::memory_order_relaxed);
            int freq2 = freq_distribution_[2].load(std::memory_order_relaxed);
            freq2 += ghost_.freq_distribution_[2].load(std::memory_order_relaxed);
            
            if(freq2 > total_size * 0.8){
                if(guard_freq_!=3){
                    guard_freq_ = 3;
                }
            }else if(freq1 > total_size * 0.8){
                if(guard_freq_!=2){
                    guard_freq_ = 2;
                }
            } else{
                if(guard_freq_!=1){
                    guard_freq_ = 1;
                }
            }
            return;
            if (freq2 * 2 > total_size)
            {
                if(guard_freq_!=3){
                    printf("set guard freq 3, cache size %d, frq2 %d, ghost freq2 %d; freq1 %d, ghost freq1 %d\n",
                            total_size,
                           freq_distribution_[2].load(std::memory_order_relaxed),
                           ghost_.freq_distribution_[2].load(std::memory_order_relaxed),
                           freq_distribution_[1].load(std::memory_order_relaxed),
                           ghost_.freq_distribution_[1].load(std::memory_order_relaxed));
                    guard_freq_ = 3;
                }
            }
            else if (freq2 * 3 > total_size)
            {
                if(guard_freq_!=2){
                    printf("set guard freq 3, cache size %d, frq2 %d, ghost freq2 %d; freq1 %d, ghost freq1 %d\n",
                            total_size,
                           freq_distribution_[2].load(std::memory_order_relaxed),
                           ghost_.freq_distribution_[2].load(std::memory_order_relaxed),
                           freq_distribution_[1].load(std::memory_order_relaxed),
                           ghost_.freq_distribution_[1].load(std::memory_order_relaxed));
                    guard_freq_ = 2;
                }
            }
            else
            {
                if(guard_freq_!=1){
                    printf("set guard freq 3, cache size %d, frq2 %d, ghost freq2 %d; freq1 %d, ghost freq1 %d\n",
                            total_size,
                           freq_distribution_[2].load(std::memory_order_relaxed),
                           ghost_.freq_distribution_[2].load(std::memory_order_relaxed),
                           freq_distribution_[1].load(std::memory_order_relaxed),
                           ghost_.freq_distribution_[1].load(std::memory_order_relaxed));
                    guard_freq_ = 1;
                }
            }
            return;
            
            
        }

        T *getEvictionCandidate() noexcept
        {//cannot return nullptr if cache is not empty
            size_t listSize = size();
            if (listSize == 0)
            {
                return nullptr;
            }
            T *curr = nullptr;
            if (!sketch_.initialized())
            {
                LockHolder l(*mtx_);
                if (!sketch_.initialized())
                {
                    sketch_.setFIFOSize(listSize / 2);
                    sketch_.initHashtable();
                }
            }
            if (!ghost_.initialized())
            {
                LockHolder l(*mtx_);
                if (!ghost_.initialized())
                {
                    ghost_.setFIFOSize(listSize / 2);
                    ghost_.initHashtable();
                }
            }
            int retry = 0;
            while (true)
            {
                while (smallfifo_->size() > ((double)(listSize)*smallRatio_))
                {
                    T *curr = nullptr;
                    curr = smallfifo_->removeTail();
                    if (curr == nullptr)
                    {
                        break;
                    }

                    int freq = getFreq(*curr);
                    int guard = guard_freq_;
                    if (freq >= guard)
                    {
                        unSetSmall(*curr);
                        mainfifo_->linkAtHead(*curr);
                        setMain(*curr);
                        // sketch_.estimate(hashNode(*curr));
                    }
                    else
                    {
                        T *tailptr = nullptr;
                        if(susfifo_->size()>((double)(listSize)*0.01)){
                           tailptr = susfifo_->removeTail();
                        }
                        int tail_freq = MAX_FREQ + 1;
                        if (tailptr != nullptr)
                        {
                            tail_freq = decFreq(*tailptr);
                            if (tail_freq == -1)
                            {
                                // duel
                                int est_curr = sketch_.getEstimate(hashNode(*curr));
                                if(est_curr < 0x3f){
                                    int est_tail = sketch_.getEstimate(hashNode(*tailptr));
                                    if(est_curr > est_tail){
                                        unSetSmall(*curr);
                                        susfifo_->linkAtHead(*curr);
                                        // set type LruType::Suspicious
                                        sketch_.estimate(hashNode(*curr));
                                        adjustGuardFreq();
                                        return tailptr;
                                    }
                                }
                                susfifo_->linkAtHead(*tailptr);
                            }
                            else
                            {
                                // sus wins
                                mainfifo_->linkAtHead(*tailptr);
                                setMain(*tailptr);
                                if (tail_freq < 2)
                                {
                                    sketch_.estimate(hashNode(*tailptr));
                                }
                            }
                        }
                        if (freq >= 2)
                        {
                            freq_distribution_[2].fetch_sub(1, std::memory_order_relaxed);
                        }
                        if(freq >= 1){
                            freq_distribution_[1].fetch_sub(1, std::memory_order_relaxed);
                        }
                        sketch_.estimate(hashNode(*curr));
                        adjustGuardFreq();
                        ghost_.insert(hashNode(*curr));
                        return curr;
                    }
                    retry++;
                    if (retry > 10)
                    {
                        listSize = size();
                        retry = 0;
                    }
                }
                // printf("%s %d small size %d main size %d sus size %d\n", __func__, __LINE__, smallfifo_->size(), mainfifo_->size(), susfifo_->size());
                while(smallfifo_->size() <= ((double)(listSize)*smallRatio_)){
                    T *curr = nullptr;
                    if(mainfifo_->size() > (double)(listSize)*mainRatio_){
                        curr = mainfifo_->removeTail();
                        if (curr != nullptr)
                        {
                            int retval = decFreq(*curr);
                            if (retval == -1)
                            {
                                //susfifo_->linkAtHead(*curr);
                                adjustGuardFreq();
                                return curr;
                            }
                            else
                            {
                                mainfifo_->linkAtHead(*curr);
                            }
                        }
                        
                    }else{
                        curr = susfifo_->removeTail();
                        if (curr != nullptr)
                        {
                            int retval = decFreq(*curr);
                            if (retval == -1)
                            {
                                adjustGuardFreq();
                                return curr;
                            }
                            else
                            {
                                mainfifo_->linkAtHead(*curr);
                                setMain(*curr);
                            }
                            if (retval < 2)
                            {
                                sketch_.estimate(hashNode(*curr));
                            }
                        }
                    }
                    retry++;
                    if (retry > 10)
                    {
                        listSize = size();
                        retry = 0;
                    }
                }
            }
            return nullptr;
        }

        void add(T &node) noexcept
        {
            int hist_freq = 0;
            if (ghost_.initialized())
            {
                hist_freq = ghost_.contains(hashNode(node));
            }
            if (hist_freq)
            {
                if(hist_freq>=guard_freq_){
                    mainfifo_->linkAtHead(node);
                    setMain(node);
                    unSetSmall(node);
                }else{
                    susfifo_->linkAtHead(node);
                    unSetMain(node);
                    unSetSmall(node);
                }
                
            }
            else
            {
                smallfifo_->linkAtHead(node);
                setSmall(node);
                unSetMain(node);
            }
            numInserts_++;
            if (numInserts_ > MAX_VALUE)
            {
                numInserts_ = 0;
            }
            return;
        }

        void remove(T &node) noexcept
        {
            //printf("MMFLEX::Container::remove(T&) called\n");
            int freq = getFreq(node);
            if (isSmall(node))
            {
                smallfifo_->remove(node);
            }
            else
            {
                if (isMain(node))
                {
                    mainfifo_->remove(node);
                }
                else
                {
                    susfifo_->remove(node);
                }
            }
            if (freq >= 2)
            {
                freq_distribution_[2].fetch_sub(1, std::memory_order_relaxed);
            }
            if(freq >= 1){
                freq_distribution_[1].fetch_sub(1, std::memory_order_relaxed);
            }
            return;
        }

    private:
        static uint32_t hashNode(const T &node) noexcept
        {
            return static_cast<uint32_t>(
                folly::hasher<folly::StringPiece>()(node.getKey()));
        }

        std::unique_ptr<ADList> smallfifo_;

        std::unique_ptr<ADList> mainfifo_;

        std::unique_ptr<ADList> susfifo_;

        mutable folly::cacheline_aligned<Mutex> mtx_;

        std::atomic<uint32_t> insert_count_{0};

        std::vector<std::atomic<uint32_t>> freq_distribution_{std::vector<std::atomic<uint32_t>>(MAX_FREQ + 1)};
        std::vector<std::atomic<uint32_t>> scatter_freq1_{std::vector<std::atomic<uint32_t>>(SCATTER_FREQ1 + 1)};
        int32_t guard_freq_{1};
        std::atomic<int64_t> numInserts_{0};
        constexpr static uint64_t MAX_VALUE = 0x0FFFFFFF;

        constexpr static double smallRatio_ = 0.1;
        constexpr static double susRatio_ = 0.05;
        constexpr static double mainRatio_ = 0.85;

        AtomicSketch sketch_;
        //AtomicFIFOHashTable ghost_;
        AtomicFIFOSketch ghost_;
    };

} // namespace facebook::cachelib