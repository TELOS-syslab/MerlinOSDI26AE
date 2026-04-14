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

        // This is not applicable for MMLru, just for compile of cache allocator
        enum LruType
        {
            Small,
            Main,
            Suspicious,
            NumTypes
        };

        // Config class for MMLru
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

            // whether the lru needs to be updated on writes for recordAccess. If
            // false, accessing the cache for writes does not promote the cached item
            // to the head of the lru.
            bool updateOnWrite{false};

            // whether the lru needs to be updated on reads for recordAccess. If
            // false, accessing the cache for reads does not promote the cached item
            // to the head of the lru.
            bool updateOnRead{true};

            int total_thread_num{1};

            // Minimum interval between reconfigurations. If 0, reconfigure is never
            // called.
            std::chrono::seconds mmReconfigureIntervalSecs{};

            // Whether to use combined locking for withEvictionIterator.
            bool useCombinedLockForIterators{false};
        };

        // The container object which can be used to keep track of objects of type
        // T. T must have a public member of type Hook. This object is wrapper
        // around DList, is thread safe and can be accessed from multiple threads.
        // The current implementation models an LRU using the above DList
        // implementation.
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
                : // : compressor_(std::move(compressor)),
                  merlinlist_(std::move(compressor), c.total_thread_num),
                  config_(std::move(c))
            {
            }
            Container(serialization::MMMerlinObject object, PtrCompressor compressor);

            Container(const Container &) = delete;
            Container &operator=(const Container &) = delete;
            void dump(std::ostream& out) {}

            // context for iterating the MM container. At any given point of time,
            // there can be only one iterator active since we need to lock the cache for
            // iteration. we can support multiple iterators at same time, by using a
            // shared ptr in the context for the lock holder in the future.
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

            // records the information that the node was accessed. This could bump up
            //
            // @param node  node that we want to mark as relevant/accessed
            // @param mode  the mode for the access operation.
            //
            // @return      True if the information is recorded and bumped the node
            //              to the head of the lru, returns false otherwise
            bool recordAccess(T &node, AccessMode mode, int thread_id = 0) noexcept;

            // adds the given node into the container and marks it as being present in
            // the container. The node is added to the head of the lru.
            //
            // @param node  The node to be added to the container.
            // @return  True if the node was successfully added to the container. False
            //          if the node was already in the contianer. On error state of node
            //          is unchanged.
            bool add(T &node, int thread_id = 0) noexcept;

            // removes the node from the lru and sets it previous and next to nullptr.
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
            // to search for evictions. This iterator holds a lock to this
            // container and only one such iterator can exist at a time
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

            bool isSmall(const T &node) const noexcept
            {
                return node.template isFlagSet<RefFlags::kMMFlag0>();
            }
            bool isMain(const T &node) const noexcept
            {
                return node.template isFlagSet<RefFlags::kMMFlag1>();
            }

            LruType getLruType(const T &node) noexcept
            {
                if (isSmall(node))
                    return LruType::Small;
                if (isMain(node))
                    return LruType::Main;
                return LruType::Suspicious;
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

            // remove node from lru and adjust insertion points
            //
            // @param node          node to remove
            // @param doRebalance     whether to do rebalance in this remove
            void removeLocked(T &node, int thread_id) noexcept;

            // Bit MM_BIT_1 is used to record if the item has been accessed since
            // being written in cache. Unaccessed items are ignored when determining
            // projected update time.

            void incFreq(T &node, int thread_id) noexcept
            {
                //node.template setFlag<RefFlags::kMMFlag2>();
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
            // the merlin cache
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
        //resetFreq(node);
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
        // LockHolder l(lruMutex_);
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
        if(thread_id == 0){
            printf("Error: thread_id is 0 in remove of MMMerlin\n");
            //abort();
        }
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
        if(thread_id == 0){
            printf("remove by iterator in MMMerlin\n");
            //abort();
        }
        T &node = *it;
        XDCHECK(node.isInMMContainer());
        node.unmarkInMMContainer();
    }

    template <typename T, MMMerlin::Hook<T> T::*HookPtr>
    bool MMMerlin::Container<T, HookPtr>::replace(T &oldNode, T &newNode) noexcept
    {
        printf("replace happen\n");
        //abort();
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