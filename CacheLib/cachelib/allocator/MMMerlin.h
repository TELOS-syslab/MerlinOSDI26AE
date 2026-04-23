#pragma once

#include <atomic>
#include <cstring>
#include <iostream>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <folly/Format.h>
#pragma GCC diagnostic pop
#include <folly/container/Array.h>
#include <folly/lang/Aligned.h>
#include <folly/synchronization/DistributedMutex.h>

#include "cachelib/allocator/Cache.h"
#include "cachelib/allocator/CacheStats.h"
#include "cachelib/allocator/Util.h"
#include "cachelib/allocator/datastruct/MerlinList.h"
#include "cachelib/allocator/memory/serialize/gen-cpp2/objects_types.h"
#include "cachelib/common/CompilerUtils.h"
#include "cachelib/common/Mutex.h"

namespace facebook::cachelib
{
    /*
     * MMMerlin adapts MerlinList to CacheLib's memory-management policy
     * interface. CacheLib expects each MMType to expose hooks, a Container, an
     * eviction iterator, serialization, and stats methods. The actual Merlin
     * admission/eviction state machine lives in datastruct/MerlinList.h; this
     * wrapper translates CacheLib item lifecycle events into MerlinList calls.
     */
    class MMMerlin
    {
    public:
        // unique identifier per MMType
        static const int kId;
        // forward declaration;
        template <typename T>
        using Hook = MerlinFIFOHook<T>;
        using SerializationType = serialization::MMMerlinObject;
        using SerializationConfigType = serialization::MMMerlinConfig;
        using SerializationTypeContainer = serialization::MMMerlinCollection;

        // Resident location used for stats/debugging. MerlinList stores the
        // actual queue state in item flags.
        enum LruType
        {
            Filter,
            Core,
            Staging,
            NumTypes
        };

        // Config class for CacheLib's MMType plumbing. updateOnRead and
        // updateOnWrite control whether recordAccess() updates Merlin's
        // frequency signal for reads and writes.
        struct Config
        {
            // create from serialized config
            explicit Config(SerializationConfigType configState)
                : Config(*configState.updateOnWrite(),
                         *configState.updateOnRead()) {}

            // @param udpateOnW   whether to promote the item on write
            // @param updateOnR   whether to promote the item on read
            Config(bool updateOnW, bool updateOnR)
                : Config(updateOnW, updateOnR, 0) {}

            // @param udpateOnW   whether to promote the item on write
            // @param updateOnR   whether to promote the item on read
            // @param mmReconfigureInterval   Time interval for recalculating lru
            //                                refresh time according to the ratio.
            Config(bool updateOnW, bool updateOnR, uint32_t mmReconfigureInterval)
                : updateOnWrite(updateOnW),
                  updateOnRead(updateOnR),
                  mmReconfigureIntervalSecs(
                      std::chrono::seconds(mmReconfigureInterval)) {}

            Config() = default;
            Config(const Config &rhs) = default;
            Config(Config &&rhs) = default;

            Config &operator=(const Config &rhs) = default;
            Config &operator=(Config &&rhs) = default;

            template <typename... Args>
            void addExtraConfig(Args...) {}

            // Whether writes should update Merlin frequency on recordAccess().
            bool updateOnWrite{false};

            // Whether reads should update Merlin frequency on recordAccess().
            bool updateOnRead{true};

            // Number of worker shards used by MerlinList. Each worker also has
            // a paired cuckoo shard to reduce insertion contention.
            int total_thread_num{1};

            // Minimum interval between reconfigurations. If 0, reconfigure is never
            // called.
            std::chrono::seconds mmReconfigureIntervalSecs{};

            // Whether to use combined locking for withEvictionIterator.
            bool useCombinedLockForIterators{false};
        };

        // CacheLib MM container wrapper. T must expose a MerlinFIFOHook member.
        // The container is thread safe through CacheLib's combining lock for
        // top-level operations, while MerlinList uses sharded FIFO queues and
        // per-queue head locks internally.
        template <typename T, Hook<T> T::*HookPtr>
        struct Container
        {
        private:
            using LIST = MerlinList<T, HookPtr>;
            using Mutex = folly::DistributedMutex;
            using LockHolder = std::unique_lock<Mutex>;
            using PtrCompressor = typename T::PtrCompressor;
            using Time = typename Hook<T>::Time;
            using CompressedPtrType = typename T::CompressedPtrType;
            using RefFlags = typename T::Flags;

        public:
            Container() = default;
            Container(Config c, PtrCompressor compressor)
                : merlinlist_(std::move(compressor), c.total_thread_num),
                  config_(std::move(c))
            {
            }
            Container(serialization::MMMerlinObject object, PtrCompressor compressor);

            Container(const Container &) = delete;
            Container &operator=(const Container &) = delete;
            void dump(std::ostream& out) {}

            // Eviction iterator expected by CacheLib. For Merlin, advancing the
            // iterator asks MerlinList for the next candidate from the calling
            // thread's shard; it is not a linear traversal over one list.
            class LockedIterator
            {
            public:
                // noncopyable but movable.
                LockedIterator(const LockedIterator &) = delete;
                LockedIterator &operator=(const LockedIterator &) = delete;

                LockedIterator(LockedIterator &&) noexcept = default;

                // moves the LockedIterator forward and backward. Calling ++ once the
                // LockedIterator has reached the end is undefined.
                LockedIterator &operator++()
                {
                    // Recompute the next candidate because Merlin may promote,
                    // demote, or reinsert several objects while searching.
                    candidate_ = merlinlist_->getEvictionCandidate(thread_id_);
                    return *this;
                }
                LockedIterator &operator--() { throw std::logic_error("Not implemented"); }

                T *operator->() const noexcept { return candidate_; }
                T &operator*() const noexcept { return *candidate_; }

                explicit operator bool() const noexcept { return candidate_ != nullptr; }

                T *get() const noexcept { return candidate_; }

                // Invalidates this iterator
                void reset() noexcept
                {
                }

                // 1. Invalidate this iterator
                // 2. Unlock
                void destroy()
                {
                }

                // Reset this iterator to the beginning
                void resetToBegin() noexcept
                {
                }

            private:
                LockedIterator &operator=(LockedIterator &&) noexcept = default;

                // create an lru iterator with the lock being held.
                LockedIterator(LIST *merlinlist, int thread_id)
                {
                    merlinlist_ = merlinlist;
                    thread_id_ = thread_id;
                    candidate_ = merlinlist_->getEvictionCandidate(thread_id_);
                }

                LIST *merlinlist_;

                T *candidate_;
                int thread_id_{0};

                // only the container can create iterators
                friend Container<T, HookPtr>;
            };

            // Records item access by increasing Merlin's bounded frequency.
            //
            // @param node  node that we want to mark as relevant/accessed
            // @param mode  the mode for the access operation.
            //
            // @return      True if the access updated Merlin state.
            bool recordAccess(T &node, AccessMode mode, int thread_id = 0) noexcept;

            // Adds a node into Merlin. MerlinList decides whether the node goes
            // to filter, staging, or core based on ghost history.
            //
            // @param node  The node to be added to the container.
            // @return  True if the node was successfully added to the container. False
            //          if the node was already in the contianer. On error state of node
            //          is unchanged.
            bool add(T &node, int thread_id = 0) noexcept;

            // Removes the node from whichever Merlin queue currently owns it.
            //
            // @param node  The node to be removed from the container.
            // @return  True if the node was successfully removed from the container.
            //          False if the node was not part of the container. On error, the
            //          state of node is unchanged.
            bool remove(T &node, int thread_id = 0) noexcept;

            // same as the above but uses an iterator context. The iterator is updated
            // on removal of the corresponding node to point to the next node. The
            // iterator context is responsible for locking.
            //
            // iterator will be advanced to the next node after removing the node
            //
            // @param it    Iterator that will be removed
            void remove(LockedIterator &it, int thread_id = 0) noexcept;

            // replaces one node with another, at the same position
            //
            // @param oldNode   node being replaced
            // @param newNode   node to replace oldNode with
            //
            // @return true  If the replace was successful. Returns false if the
            //               destination node did not exist in the container, or if the
            //               source node already existed.
            bool replace(T &oldNode, T &newNode) noexcept;

            // Obtain an iterator that start from the tail and can be used
            // to search for evictions.
            LockedIterator getEvictionIterator(int thread_id = 0) noexcept;

            // Execute provided function under container lock. Function gets
            // iterator passed as parameter.
            template <typename F>
            void withEvictionIterator(F &&f, int thread_id = 0);

            template <typename F>
            void withContainerLock(F &&fun);

            // get copy of current config
            Config getConfig() const;

            // override the existing config with the new one.
            void setConfig(const Config &newConfig);

            bool isEmpty() const noexcept { return size() == 0; }

            // reconfigure the MMContainer: update refresh time according to current
            // tail age
            void reconfigureLocked(const Time &currTime);

            // returns the number of elements in the container
            size_t size() const noexcept
            {
                return Mutex_->lock_combine([this]()
                                            { return merlinlist_.size(); });
            }

            // Returns the eviction age stats. See CacheStats.h for details
            EvictionAgeStat getEvictionAgeStat(uint64_t projectedLength) const noexcept;

            // for saving the state of the lru
            //
            // precondition:  serialization must happen without any reader or writer
            // present. Any modification of this object afterwards will result in an
            // invalid, inconsistent state for the serialized data.
            //
            serialization::MMMerlinObject saveState() const noexcept;

            // return the stats for this container.
            MMContainerStat getStats() const noexcept;

            bool isFilter(const T &node) const noexcept
            {
                return node.template isFlagSet<RefFlags::kMMFlag0>();
            }
            bool isCore(const T &node) const noexcept
            {
                return node.template isFlagSet<RefFlags::kMMFlag1>();
            }

            LruType getLruType(const T &node) noexcept
            {
                // CacheLib callers may ask for the resident class even though
                // Merlin is not an LRU policy.
                if (isFilter(node))
                    return LruType::Filter;
                if (isCore(node))
                    return LruType::Core;
                return LruType::Staging;
            }
        private:
            EvictionAgeStat
            getEvictionAgeStatLocked(
                uint64_t projectedLength) const noexcept;

            static Time getUpdateTime(const T &node) noexcept
            {
                return (node.*HookPtr).getUpdateTime();
            }

            static void setUpdateTime(T &node, Time time) noexcept
            {
                (node.*HookPtr).setUpdateTime(time);
            }

            // Remove node from MerlinList and clear the CacheLib MM membership
            // bit.
            //
            // @param node          node to remove
            // @param doRebalance     whether to do rebalance in this remove
            void removeLocked(T &node, int thread_id) noexcept;

            void incFreq(T &node, int thread_id) noexcept
            {
                merlinlist_.incFreq(node, thread_id);
                return;
            }

            void resetFreq(T &node) noexcept
            {
                //node.template unSetFlag<RefFlags::kMMFlag2>();
                //merlinlist_.resetFreq(node);
                return;
            }

            mutable folly::cacheline_aligned<Mutex> Mutex_;
            // Sharded Merlin queues and metadata.
            LIST merlinlist_{};
            Config config_{};
        };
    };

    template <typename T, MMMerlin::Hook<T> T::*HookPtr>
    MMMerlin::Container<T, HookPtr>::Container(serialization::MMMerlinObject object, PtrCompressor compressor)
        : merlinlist_(*object.merlinlist(), compressor), config_(*object.config())
    {
    }

    template <typename T, MMMerlin::Hook<T> T::*HookPtr>
    bool MMMerlin::Container<T, HookPtr>::recordAccess(T &node,
                                                     AccessMode mode, int thread_id) noexcept
    {
        if ((mode == AccessMode::kWrite && !config_.updateOnWrite) ||
            (mode == AccessMode::kRead && !config_.updateOnRead))
        {
            return false;
        }
        if(thread_id == 0){
            printf("Error: thread_id is 0 in recordAccess of MMMerlin\n");
            //abort();
        }
        const auto curr = static_cast<Time>(util::getCurrentTimeSec());
        // check if the node is still being memory managed
        if (node.isInMMContainer())
        {
            // Merlin does not move the item on every hit. It only increments
            // the encoded frequency; queue movement happens during eviction.
            incFreq(node, thread_id);
            setUpdateTime(node, curr);
            return true;
        }
        return false;
    }

    template <typename T, MMMerlin::Hook<T> T::*HookPtr>
    cachelib::EvictionAgeStat MMMerlin::Container<T, HookPtr>::getEvictionAgeStat(
        uint64_t projectedLength) const noexcept
    {
        return Mutex_->lock_combine([this, projectedLength]()
                                    { return getEvictionAgeStatLocked(projectedLength); });
    }

    template <typename T, MMMerlin::Hook<T> T::*HookPtr>
    cachelib::EvictionAgeStat
    MMMerlin::Container<T, HookPtr>::getEvictionAgeStatLocked(
        uint64_t projectedLength) const noexcept
    {
        // Merlin's eviction decision is not a single LRU tail age, so the
        // CacheLib age-stat interface is intentionally left empty.
        EvictionAgeStat stat{};
        const auto currTime = static_cast<Time>(util::getCurrentTimeSec());
        return stat;
    }

    template <typename T, MMMerlin::Hook<T> T::*HookPtr>
    void MMMerlin::Container<T, HookPtr>::setConfig(const Config &newConfig)
    {
        Mutex_->lock_combine([this, newConfig]()
                             { config_ = newConfig; });
    }

    template <typename T, MMMerlin::Hook<T> T::*HookPtr>
    typename MMMerlin::Config MMMerlin::Container<T, HookPtr>::getConfig() const
    {
        return Mutex_->lock_combine([this]()
                                    { return config_; });
    }

    template <typename T, MMMerlin::Hook<T> T::*HookPtr>
    bool MMMerlin::Container<T, HookPtr>::add(T &node, int thread_id) noexcept
    {
        const auto currTime = static_cast<Time>(util::getCurrentTimeSec());
        if (node.isInMMContainer())
        {
            return false;
        }
        // MerlinList performs admission using ghost history and sets the item
        // flags that identify filter/staging/core placement.
        merlinlist_.add(node, thread_id);
        node.markInMMContainer();
        setUpdateTime(node, currTime);
        return true;
    }

    template <typename T, MMMerlin::Hook<T> T::*HookPtr>
    typename MMMerlin::Container<T, HookPtr>::LockedIterator
    MMMerlin::Container<T, HookPtr>::getEvictionIterator(int thread_id) noexcept
    {
        return LockedIterator{&merlinlist_, thread_id};
    }

    template <typename T, MMMerlin::Hook<T> T::*HookPtr>
    template <typename F>
    void MMMerlin::Container<T, HookPtr>::withEvictionIterator(F &&fun, int thread_id)
    {
        fun(getEvictionIterator(thread_id));
    }

    template <typename T, MMMerlin::Hook<T> T::*HookPtr>
    template <typename F>
    void MMMerlin::Container<T, HookPtr>::withContainerLock(F &&fun)
    {
        // MerlinList handles its own sharded synchronization for the hot path.
        fun();
    }

    template <typename T, MMMerlin::Hook<T> T::*HookPtr>
    void MMMerlin::Container<T, HookPtr>::removeLocked(T &node, int thread_id) noexcept
    {
        merlinlist_.remove(node, thread_id);
        node.unmarkInMMContainer();
        return;
    }

    template <typename T, MMMerlin::Hook<T> T::*HookPtr>
    bool MMMerlin::Container<T, HookPtr>::remove(T &node, int thread_id) noexcept
    {
        return Mutex_->lock_combine([this, &node,thread_id]()
                                    {
        if (!node.isInMMContainer()) {
        return false;
        }
        removeLocked(node,thread_id);
        return true; });
    }

    template <typename T, MMMerlin::Hook<T> T::*HookPtr>
    void MMMerlin::Container<T, HookPtr>::remove(LockedIterator &it, int thread_id) noexcept
    {
        // CacheLib calls this after consuming the eviction candidate. MerlinList
        // already unlinked the candidate while producing it.
        T &node = *it;
        XDCHECK(node.isInMMContainer());
        node.unmarkInMMContainer();
    }

    template <typename T, MMMerlin::Hook<T> T::*HookPtr>
    bool MMMerlin::Container<T, HookPtr>::replace(T &oldNode, T &newNode) noexcept
    {
        return Mutex_->lock_combine([this, &oldNode, &newNode]()
                                    {
    if (!oldNode.isInMMContainer() || newNode.isInMMContainer()) {
      return false;
    }
    const auto updateTime = getUpdateTime(oldNode);

    oldNode.unmarkInMMContainer();
    newNode.markInMMContainer();
    setUpdateTime(newNode, updateTime);
    return true; });
    }

    template <typename T, MMMerlin::Hook<T> T::*HookPtr>
    serialization::MMMerlinObject MMMerlin::Container<T, HookPtr>::saveState()
        const noexcept
    {
        // Persist resident queues and basic config. Ghost/sketch metadata is
        // runtime-only and is rebuilt lazily by MerlinList after restore.
        serialization::MMMerlinConfig configObject;
        *configObject.updateOnWrite() = config_.updateOnWrite;
        *configObject.updateOnRead() = config_.updateOnRead;

        serialization::MMMerlinObject object;
        *object.config() = configObject;
        *object.merlinlist() = merlinlist_.saveState();
        return object;
    }

    template <typename T, MMMerlin::Hook<T> T::*HookPtr>
    MMContainerStat MMMerlin::Container<T, HookPtr>::getStats() const noexcept
    {
        auto stat = Mutex_->lock_combine([this]()
                                         {
            // we return by array here because DistributedMutex is fastest when the
            // output data fits within 48 bytes.  And the array is exactly 48 bytes, so
            // it can get optimized by the implementation.
            //
            // the rest of the parameters are 0, so we don't need the critical section
            // to return them
            return folly::make_array(merlinlist_.size()); });
        return {stat[0] /* lru size */,
                // stat[1] /* tail time */,
                0, 0 /* refresh time */, 0, 0, 0, 0};
    }

    template <typename T, MMMerlin::Hook<T> T::*HookPtr>
    void MMMerlin::Container<T, HookPtr>::reconfigureLocked(const Time &currTime)
    {
        // not used
    }

} // namespace facebook::cachelib
