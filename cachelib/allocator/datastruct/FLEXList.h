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
            unSetSmall(node);
            unSetMain(node);
            return;
            constexpr Value bitMask =
                std::numeric_limits<Value>::max() - kTypeMask;
            __atomic_and_fetch(&node.ref_.refCount_, bitMask, __ATOMIC_ACQ_REL);
        }
        FOLLY_ALWAYS_INLINE int getType(const T &node) const noexcept
        {

            int ret = __atomic_load_n(&node.ref_.refCount_, __ATOMIC_RELAXED);
            return (ret >> RefFlags::kMMFlag0) & TypeMask;
        }
        void setType(T &node, int type) noexcept
        {
            return;
            // unSetType(node);
            Value bitMask = ((static_cast<Value>(type) & TypeMask) << RefFlags::kMMFlag0);
            __atomic_or_fetch(&node.ref_.refCount_, bitMask, __ATOMIC_ACQ_REL);
        }
        FOLLY_ALWAYS_INLINE int incFreq(T &node) noexcept
        {
            node.template setFlag<RefFlags::kMMFlag2>();
            return 1;
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
            if (retval > 1 && retval <= MAX_FREQ)
            {
                freq_distribution_[retval].fetch_add(1, std::memory_order_relaxed);
            }
            return retval;
        }
        FOLLY_ALWAYS_INLINE int decFreq(T &node) noexcept
        {
            node.template unSetFlag<RefFlags::kMMFlag2>();
            return 0;
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
            if (retval > 1 && retval <= MAX_FREQ)
            {
                freq_distribution_[retval].fetch_sub(1, std::memory_order_relaxed);
            }
            return retval - 1;
        }
        FOLLY_ALWAYS_INLINE int getFreq(const T &node) const noexcept
        {
            return node.template isFlagSet<RefFlags::kMMFlag2>();
            int ret = __atomic_load_n(&node.ref_.refCount_, __ATOMIC_RELAXED);
            return (ret >> RefFlags::kMMFlag2) & (MAX_FREQ);
        }
        void resetFreq(T &node) noexcept
        {
            node.template unSetFlag<RefFlags::kMMFlag2>();
            return;
            constexpr Value bitMask =
                std::numeric_limits<Value>::max() - kFreqMask;
            __atomic_and_fetch(&node.ref_.refCount_, bitMask, __ATOMIC_ACQ_REL);
        }
        void setFreq(T &node, int freq) noexcept
        {
            node.template setFlag<RefFlags::kMMFlag2>();
            return;
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
                //freq_distribution_[i++] = f;
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
                //state.freq_distribution()->emplace_back(f.load());
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
            if (!hashTable_.initialized())
            {
                LockHolder l(*mtx_);
                hashTable_.setFIFOSize(listSize / 2);
                hashTable_.initHashtable();
            }
            while (true)
            {
                if (smallfifo_->size() > (double)(size()) * smallRatio_)
                {
                    curr = smallfifo_->removeTail();
                    if (curr)
                    {
                        int freq = getFreq(*curr);
                        if (freq >= guard_freq_)
                        {
                            resetFreq(*curr);
                            unSetType(*curr);
                            mainfifo_->linkAtHead(*curr);
                            setMain(*curr);
                            continue;
                        }
                        else
                        {
                            hashTable_.insert(hashNode(*curr));
                            return curr;
                        }
                    }
                }
                else
                {
                    curr = mainfifo_->removeTail();
                    if (curr == nullptr)
                    {
                        continue;
                    }
                    else
                    {
                        int freq = getFreq(*curr);
                        if (freq > 0)
                        {
                            resetFreq(*curr);
                            mainfifo_->linkAtHead(*curr);
                            continue;
                        }
                        else
                        {
                            return curr;
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
            if (hist_.initialized() && hist_.contains(hashNode(node)))
            {
                mainfifo_->linkAtHead(node);
                unSetType(node);
                setMain(node);
            }
            else
            {
                smallfifo_->linkAtHead(node);
                unSetType(node);
                setSmall(node);
            }
            return;
        }

        void remove(T &node) noexcept
        {
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
        AtomicFIFOHashTable hashTable_;

        constexpr static size_t nMaxEvictionCandidates_ = 64;

        folly::MPMCQueue<T *> evictCandidateQueue_{nMaxEvictionCandidates_};

        std::unique_ptr<std::thread> evThread_{nullptr};

        std::atomic<bool> stop_{false};
    };

} // namespace facebook::cachelib