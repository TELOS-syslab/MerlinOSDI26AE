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

        enum LruType
        {
            Small,
            Main,
            Suspicious,
            NumTypes
        };

#define MAX_FREQ 7
#define TypeMask 0x3
#define FreqMask 0x7
        using Value = uint32_t;
        static constexpr Value kTypeMask = (TypeMask << RefFlags::kMMFlag0);
        static constexpr Value kFreqMask = (FreqMask << RefFlags::kMMFlag2);

        void unSetType(T &node) noexcept
        {
            constexpr Value bitMask =
                std::numeric_limits<Value>::max() - kTypeMask;
            __atomic_and_fetch(&node.ref_.refCount_, bitMask, __ATOMIC_ACQ_REL);
        }
        FOLLY_ALWAYS_INLINE LruType getType(const T &node) const noexcept
        {
            int ret = __atomic_load_n(&node.ref_.refCount_, __ATOMIC_RELAXED);
            return static_cast<LruType>((ret >> RefFlags::kMMFlag0) & TypeMask);
        }
        void setType(T &node, int type) noexcept
        {
            // unSetType(node);
            Value bitMask = ((static_cast<Value>(type) & TypeMask) << RefFlags::kMMFlag0);
            __atomic_or_fetch(&node.ref_.refCount_, bitMask, __ATOMIC_ACQ_REL);
        }
        FOLLY_ALWAYS_INLINE int incFreq(T &node) noexcept
        {
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
            if (retval > 1 && retval < MAX_FREQ)
            {
                freq_distribution_[retval].fetch_add(1, std::memory_order_relaxed);
            }
            return retval;
        }
        FOLLY_ALWAYS_INLINE int decFreq(T &node) noexcept
        {
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
            int retval = (res >> RefFlags::kMMFlag2) - 1;
            if (retval > 1 && retval < MAX_FREQ)
            {
                freq_distribution_[retval].fetch_sub(1, std::memory_order_relaxed);
            }
            return retval;
        }
        FOLLY_ALWAYS_INLINE int getFreq(const T &node) const noexcept
        {
            int ret = __atomic_load_n(&node.ref_.refCount_, __ATOMIC_RELAXED);
            return (ret >> RefFlags::kMMFlag2) & (MAX_FREQ);
        }
        void resetFreq(T &node) noexcept
        {
            constexpr Value bitMask =
                std::numeric_limits<Value>::max() - kFreqMask;
            __atomic_and_fetch(&node.ref_.refCount_, bitMask, __ATOMIC_ACQ_REL);
        }
        void setFreq(T &node, int freq) noexcept
        {
            // resetFreq(node);
            Value bitMask = ((static_cast<Value>(freq) & MAX_FREQ) << RefFlags::kMMFlag2);
            __atomic_or_fetch(&node.ref_.refCount_, bitMask, __ATOMIC_ACQ_REL);
        }

        FLEXList() = default;
        FLEXList(const FLEXList &) = delete;
        FLEXList &operator=(const FLEXList &) = delete;
        ~FLEXList()
        {
            stop_ = true;
            if (evThread_ && evThread_->joinable())
            {
                evThread_->join();
            }
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

        size_t size() const noexcept
        {
            return smallfifo_->size() + mainfifo_->size() + susfifo_->size();
        }

        void adjustGuardFreq() noexcept
        {
            int total_size = size();
            int freq2 = freq_distribution_[2].load(std::memory_order_relaxed) + hist_.freq_distribution_[2].load(std::memory_order_relaxed);
            if (freq2 > total_size)
            {
                guard_freq_ = 3;
            }
            else if (freq2 * 2 > total_size)
            {
                guard_freq_ = 2;
            }
            else
            {
                guard_freq_ = 1;
            }
        }

        T *getEvictionCandidate() noexcept
        {
            size_t listSize = size();
            if (listSize == 0)
            {
                return nullptr;
            }
            T *curr = nullptr;
            if (!hist_.initialized())
            {
                LockHolder l(*mtx_);
                if (!hist_.initialized())
                {
                    hist_.setFIFOSize(listSize / 2);
                    hist_.initHashtable();
                }
            }
            while (true)
            {
                if (smallfifo_->size() > size() * smallRatio_)
                {
                    curr = smallfifo_->removeTail();
                    if (curr)
                    {
                        int freq = getFreq(*curr);
                        if (freq > guard_freq_)
                        {
                            unSetType(*curr);
                            mainfifo_->linkAtHead(*curr);
                            setType(*curr, LruType::Main);
                            hist_.estimate(hashNode(*curr));
                            continue;
                        }
                        else
                        {
                            T *tailptr = susfifo_->removeTail();
                            if (tailptr != nullptr)
                            {
                                int tail_freq = decFreq(*tailptr);
                                if (tail_freq == -1)
                                {
                                    // duel
                                    if (hist_.getEstimate(hashNode(*curr)) > hist_.getEstimate(hashNode(*tailptr)))
                                    {
                                        // curr wins
                                        unSetType(*curr);
                                        susfifo_->linkAtHead(*curr);
                                        setType(*curr, LruType::Suspicious);
                                        hist_.estimate(hashNode(*curr));
                                        adjustGuardFreq();
                                        return tailptr;
                                    }
                                    else
                                    {
                                        // tail wins
                                        susfifo_->linkAtHead(*tailptr);
                                        hist_.insert(hashNode(*curr), freq);
                                    }
                                }
                                else
                                {
                                    // tail wins
                                    unSetType(*tailptr);
                                    mainfifo_->linkAtHead(*tailptr);
                                    setType(*tailptr, LruType::Main);
                                    hist_.estimate(hashNode(*tailptr));
                                }
                            }
                            for (int i = 2; i <= freq; i++)
                            {
                                freq_distribution_[i].fetch_sub(1, std::memory_order_relaxed);
                            }
                            hist_.estimate(hashNode(*curr));
                            adjustGuardFreq();
                            return curr;
                        }
                    }
                }
                else
                {
                    while (mainfifo_->size() > size() * mainRatio_)
                    {
                        curr = mainfifo_->removeTail();
                        if (curr == nullptr)
                        {
                            break;
                        }
                        unSetType(*curr);
                        susfifo_->linkAtHead(*curr);
                        setType(*curr, LruType::Suspicious);
                    }
                    curr = susfifo_->removeTail();
                    if (curr)
                    {
                        int retval = decFreq(*curr);
                        if (retval == -1)
                        {
                            adjustGuardFreq();
                            return curr;
                        }
                        unSetType(*curr);
                        mainfifo_->linkAtHead(*curr);
                        setType(*curr, LruType::Main);
                        if (retval < 4)
                        {

                            hist_.estimate(hashNode(*curr));
                        }
                    }
                }
            }
            return nullptr;
        }

        void threadFunc() noexcept
        {
            XLOG(INFO) << "FLEXList thread has started";
            T *curr = nullptr;

            while (!stop_.load())
            {
                while (evictCandidateQueue_.size() <
                       nMaxEvictionCandidates_)
                {
                    // prepareEvictionCandidates();
                }
                // sleep for 1ms
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            XLOG(INFO) << "FLEXList thread is stopping";
        }

        void add(T &node) noexcept
        {
            int hist_freq = -1;
            if (hist_.initialized())
            {
                hist_freq = hist_.contains(hashNode(node));
            }
            if (hist_freq == -1)
            {
                setFreq(node, 0);
                smallfifo_->linkAtHead(node);
                setType(node, LruType::Small);
            }
            else if (hist_freq >= guard_freq_)
            {
                setFreq(node, hist_freq);
                mainfifo_->linkAtHead(node);
                setType(node, LruType::Main);
                for (int i = 2; i <= hist_freq; i++)
                {
                    freq_distribution_[i].fetch_add(1, std::memory_order_relaxed);
                }
            }
            else
            {
                setFreq(node, 0);
                susfifo_->linkAtHead(node);
                setType(node, LruType::Suspicious);
            }
            return;
        }

        void remove(T &node) noexcept
        {
            LruType type = getType(node);
            int freq = getFreq(node);
            
            switch (type)
            {
            case LruType::Small:
                smallfifo_->remove(node);
                break;
            case LruType::Main:
                mainfifo_->remove(node);
                break;
            case LruType::Suspicious:
                susfifo_->remove(node);
                break;
            case LruType::NumTypes:
                XDCHECK(false);
            }
            if (freq > 1 && freq < MAX_FREQ)
            {
                freq_distribution_[freq].fetch_sub(1, std::memory_order_relaxed);
            }
        }

    private:
        static uint32_t hashNode(const T &node) noexcept
        {
            return static_cast<uint32_t>(
                folly::hasher<folly::StringPiece>()(node.getKey()));
        }

        /* different from previous one - we load 1/4 of the nMax */
        size_t nCandidateToPrepare()
        {
            size_t n = 0;
            return n;
        }
        std::unique_ptr<ADList> smallfifo_;

        std::unique_ptr<ADList> mainfifo_;

        std::unique_ptr<ADList> susfifo_;

        mutable folly::cacheline_aligned<Mutex> mtx_;

        std::vector<std::atomic<uint32_t>> freq_distribution_{std::vector<std::atomic<uint32_t>>(MAX_FREQ + 1)};
        int guard_freq_{1};

        constexpr static double smallRatio_ = 0.1;
        constexpr static double susRatio_ = 0.05;
        constexpr static double mainRatio_ = 0.85;

        AtomicSketch hist_;

        constexpr static size_t nMaxEvictionCandidates_ = 64;

        folly::MPMCQueue<T *> evictCandidateQueue_{nMaxEvictionCandidates_};

        std::unique_ptr<std::thread> evThread_{nullptr};

        std::atomic<bool> stop_{false};
    };

} // namespace facebook::cachelib