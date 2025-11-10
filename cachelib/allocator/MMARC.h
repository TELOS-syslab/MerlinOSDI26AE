#pragma once

#include <atomic>
#include <cstring>
#include <unordered_set>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <folly/Format.h>
#pragma GCC diagnostic pop
#include <folly/lang/Aligned.h>
#include <folly/synchronization/DistributedMutex.h>

#include "cachelib/allocator/Cache.h"
#include "cachelib/allocator/CacheStats.h"
#include "cachelib/allocator/Util.h"
#include "cachelib/allocator/datastruct/MultiDList.h"
#include "cachelib/allocator/memory/serialize/gen-cpp2/objects_types.h"
#include "cachelib/common/CompilerUtils.h"
#include "cachelib/common/Mutex.h"

namespace facebook::cachelib {

template <typename MMType>
class MMTypeTest;

// ARC (Adaptive Replacement Cache) implementation
// ARC maintains 4 lists:
// - T1 (Recency): Recently accessed items (first-time access)
// - T2 (Frequency): Frequently accessed items (accessed multiple times)
// - B1 (Ghost Recency): History of items evicted from T1
// - B2 (Ghost Frequency): History of items evicted from T2
//
// The algorithm adaptively balances between recency and frequency by
// adjusting parameter p based on cache hits in the ghost lists.
class MMARC {
 public:
  // unique identifier per MMType
  static const int kId;

  // forward declaration
  template <typename T>
  using Hook = DListHook<T>;
  using SerializationType = serialization::MMARCObject;
  using SerializationConfigType = serialization::MMARCConfig;
  using SerializationTypeContainer = serialization::MMARCCollection;

  // List types for ARC
  enum LruType {
    Recency,   // T1: recent items
    Frequency, // T2: frequent items
    NumTypes
  };

  // Config class for MMARC
  struct Config {
    explicit Config(SerializationConfigType configState)
        : Config(*configState.lruRefreshTime(),
                 *configState.updateOnWrite(),
                 *configState.updateOnRead(),
                 *configState.tryLockUpdate(),
                 *configState.ghostMultiplier()) {}

    Config(uint32_t time, bool updateOnW, bool updateOnR)
        : Config(time, updateOnW, updateOnR, false, 1.0) {}

    Config(uint32_t time,
           bool updateOnW,
           bool updateOnR,
           bool tryLockU,
           double ghostMult)
        : defaultLruRefreshTime(time),
          updateOnWrite(updateOnW),
          updateOnRead(updateOnR),
          tryLockUpdate(tryLockU),
          ghostMultiplier(ghostMult) {
      checkConfig();
    }

    Config() = default;
    Config(const Config& rhs) = default;
    Config(Config&& rhs) = default;

    Config& operator=(const Config& rhs) = default;
    Config& operator=(Config&& rhs) = default;

    void checkConfig() {
      if (ghostMultiplier <= 0.0 || ghostMultiplier > 10.0) {
        throw std::invalid_argument(
            folly::sformat("Invalid ghostMultiplier: {}", ghostMultiplier));
      }
    }

    void addExtraConfig(size_t /* tailSize */) {
      // ARC doesn't use tail tracking like 2Q, but we keep API compatibility
    }

    uint32_t defaultLruRefreshTime{60};
    uint32_t lruRefreshTime{defaultLruRefreshTime};

    bool updateOnWrite{false};
    bool updateOnRead{true};

    bool tryLockUpdate{false};

    double ghostMultiplier{1.0};

    bool useCombinedLockForIterators{false};
  };

  // Container implementation
  template <typename T, Hook<T> T::*HookPtr>
  struct Container {
   private:
    using LruList = MultiDList<T, HookPtr>;
    using Mutex = folly::DistributedMutex;
    using LockHolder = std::unique_lock<Mutex>;
    using PtrCompressor = typename T::PtrCompressor;
    using Time = typename Hook<T>::Time;
    using CompressedPtrType = typename T::CompressedPtrType;
    using RefFlags = typename T::Flags;

    // Optimized ghost list management using circular buffer + hash set
    struct GhostEntry {
      uint32_t hash;
      Time evictTime;
    };

    // Circular buffer for ghost entries - cache-friendly and O(1) operations
    class GhostList {
     public:
      GhostList() = default;

      void setCapacity(size_t cap) {
        capacity_ = cap;
        buffer_.reserve(cap);
        hashSet_.reserve(cap);
      }

      // O(1) check
      bool contains(uint32_t hash) const noexcept {
        return hashSet_.find(hash) != hashSet_.end();
      }

      // O(1) insert
      void insert(uint32_t hash, Time time) noexcept {
        if (capacity_ == 0) {
          return; // nothing to retain; capacity not initialized yet
        }
        if (hashSet_.find(hash) != hashSet_.end()) {
          return; // Already exists
        }

        if (buffer_.size() >= capacity_) {
          // Remove oldest entry
          uint32_t oldHash = buffer_[head_].hash;
          hashSet_.erase(oldHash);

          // Overwrite with new entry
          buffer_[head_] = {hash, time};
          head_ = (head_ + 1) % capacity_;
        } else {
          buffer_.push_back({hash, time});
        }

        hashSet_.insert(hash);
      }

      // O(1) remove
      void remove(uint32_t hash) noexcept {
        hashSet_.erase(hash);
        // lazy deletion in buffer
      }

      size_t size() const noexcept { return hashSet_.size(); }

      void clear() noexcept {
        buffer_.clear();
        hashSet_.clear();
        head_ = 0;
      }

     private:
      std::vector<GhostEntry> buffer_;
      std::unordered_set<uint32_t> hashSet_;
      size_t capacity_{0};
      size_t head_{0};
    };

    uint32_t hashNode(const T& node) const noexcept {
      return static_cast<uint32_t>(
          folly::hasher<folly::StringPiece>()(node.getKey()));
    }

    bool isInGhostRecency(uint32_t hash) const noexcept {
      return ghostRecency_.contains(hash);
    }

    bool isInGhostFrequency(uint32_t hash) const noexcept {
      return ghostFrequency_.contains(hash);
    }

    void addToGhostRecency(uint32_t hash, Time time) noexcept {
      ghostRecency_.insert(hash, time);
    }

    void addToGhostFrequency(uint32_t hash, Time time) noexcept {
      ghostFrequency_.insert(hash, time);
    }

    void removeFromGhost(uint32_t hash) noexcept {
      ghostRecency_.remove(hash);
      ghostFrequency_.remove(hash);
    }

   public:
    Container() = default;
    Container(Config c, PtrCompressor compressor)
        : lru_(LruType::NumTypes, std::move(compressor)),
          config_(std::move(c)) {
      lruRefreshTime_ = config_.lruRefreshTime;
      maxGhostSize_ = 0; // will be set on first add
    }

    Container(const serialization::MMARCObject& object,
              PtrCompressor compressor);

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;

    void dump(std::ostream& /* out */) {
      // Placeholder for debugging output
    }

    using Iterator = typename LruList::Iterator;

    // Locked iterator for eviction
    class LockedIterator : public Iterator {
     public:
      LockedIterator(const LockedIterator&) = delete;
      LockedIterator& operator=(const LockedIterator&) = delete;
      LockedIterator(LockedIterator&&) noexcept = default;

      void destroy() {
        Iterator::reset();
        if (l_.owns_lock()) {
          l_.unlock();
        }
      }

      void resetToBegin() {
        if (!l_.owns_lock()) {
          l_.lock();
        }
        Iterator::resetToBegin();
      }

     private:
      LockedIterator& operator=(LockedIterator&&) noexcept = default;
      LockedIterator(LockHolder l, const Iterator& iter) noexcept;
      friend Container<T, HookPtr>;
      LockHolder l_;
    };

    bool recordAccess(T& node, AccessMode mode) noexcept;

    bool add(T& node) noexcept;

    bool remove(T& node) noexcept;

    void remove(Iterator& it) noexcept;

    bool replace(T& oldNode, T& newNode) noexcept;

    LockedIterator getEvictionIterator() const noexcept;

    template <typename F>
    void withEvictionIterator(F&& f);

    template <typename F>
    void withContainerLock(F&& f);

    Config getConfig() const;
    void setConfig(const Config& newConfig);

    bool isEmpty() const noexcept { return size() == 0; }
    size_t size() const noexcept {
      return lruMutex_->lock_combine([this]() { return lru_.size(); });
    }

    EvictionAgeStat getEvictionAgeStat(uint64_t projectedLength) const noexcept;
    serialization::MMARCObject saveState() const noexcept;
    MMContainerStat getStats() const noexcept;

    LruType getLruType(const T& node) const noexcept;

   private:
    EvictionAgeStat getEvictionAgeStatLocked(
        uint64_t /* projectedLength */) const noexcept;

    static Time getUpdateTime(const T& node) noexcept {
      return (node.*HookPtr).getUpdateTime();
    }

    static void setUpdateTime(T& node, Time time) noexcept {
      (node.*HookPtr).setUpdateTime(time);
    }

    void removeLocked(T& node) noexcept;

    // Flag management
    void markRecency(T& node) noexcept {
      node.template setFlag<RefFlags::kMMFlag0>();
      node.template unSetFlag<RefFlags::kMMFlag1>();
    }

    void markFrequency(T& node) noexcept {
      node.template unSetFlag<RefFlags::kMMFlag0>();
      node.template setFlag<RefFlags::kMMFlag1>();
    }

    void unmarkAll(T& node) noexcept {
      node.template unSetFlag<RefFlags::kMMFlag0>();
      node.template unSetFlag<RefFlags::kMMFlag1>();
    }

    bool isRecency(const T& node) const noexcept {
      return node.template isFlagSet<RefFlags::kMMFlag0>();
    }

    bool isFrequency(const T& node) const noexcept {
      return node.template isFlagSet<RefFlags::kMMFlag1>();
    }

    void adaptOnGhostRecencyHit() noexcept {
      // B1 hit: increase p (favor recency)
      uint32_t delta = std::max(
          1u,
          static_cast<uint32_t>(ghostFrequency_.size()) /
              std::max(1u, static_cast<uint32_t>(ghostRecency_.size())));
      uint32_t capacity = lru_.size() + ghostRecency_.size() + ghostFrequency_.size();
      partitionSize_ = std::min(partitionSize_ + delta, capacity);
      ++numGhostRecencyHits_;
    }

    void adaptOnGhostFrequencyHit() noexcept {
      // B2 hit: decrease p (favor frequency)
      uint32_t delta = std::max(
          1u,
          static_cast<uint32_t>(ghostRecency_.size()) /
              std::max(1u, static_cast<uint32_t>(ghostFrequency_.size())));
      partitionSize_ = (partitionSize_ >= delta) ? (partitionSize_ - delta) : 0;
      ++numGhostFrequencyHits_;
    }

    // Standard ARC eviction (LRU tails per list)
    T* evictLocked() noexcept {
      const size_t t1Size = lru_.getList(LruType::Recency).size();
      const size_t t2Size = lru_.getList(LruType::Frequency).size();

      if (t1Size == 0 && t2Size == 0) {
        return nullptr;
      }

      T* victim = nullptr;
      const auto currTime = static_cast<Time>(util::getCurrentTimeSec());

      if (t1Size > 0 && (t1Size >= std::max(1u, partitionSize_) || t2Size == 0)) {
        // Evict from T1
        victim = lru_.getList(LruType::Recency).getTail();
        if (victim) {
          lru_.getList(LruType::Recency).remove(*victim);
          uint32_t hash = hashNode(*victim);
          addToGhostRecency(hash, currTime);
          unmarkAll(*victim);
          ++numRecencyEvictions_;
        }
      } else if (t2Size > 0) {
        // Evict from T2
        victim = lru_.getList(LruType::Frequency).getTail();
        if (victim) {
          lru_.getList(LruType::Frequency).remove(*victim);
          uint32_t hash = hashNode(*victim);
          addToGhostFrequency(hash, currTime);
          unmarkAll(*victim);
          ++numFrequencyEvictions_;
        }
      }

      if (victim) {
        victim->unmarkInMMContainer();
      }
      return victim;
    }

    mutable folly::cacheline_aligned<Mutex> lruMutex_;
    LruList lru_{LruType::NumTypes, PtrCompressor{}};

    // Ghost lists (optimized circular buffer + hash set)
    GhostList ghostRecency_;   // B1
    GhostList ghostFrequency_; // B2

    uint32_t partitionSize_{0};
    size_t maxGhostSize_{0};

    // Statistics
    uint64_t numRecencyHits_{0};
    uint64_t numFrequencyHits_{0};
    uint64_t numGhostRecencyHits_{0};
    uint64_t numGhostFrequencyHits_{0};
    uint64_t numRecencyEvictions_{0};
    uint64_t numFrequencyEvictions_{0};

    std::atomic<uint32_t> lruRefreshTime_{};
    Config config_{};

    friend class MMTypeTest<MMARC>;
  };
};

/* Container Interface Implementation */
template <typename T, MMARC::Hook<T> T::*HookPtr>
MMARC::Container<T, HookPtr>::Container(
    const serialization::MMARCObject& object,
    PtrCompressor compressor)
    : lru_(*object.lrus(), compressor),
      config_(*object.config()),
      partitionSize_(*object.partitionSize()) {
  lruRefreshTime_ = config_.lruRefreshTime;
  maxGhostSize_ = static_cast<size_t>(lru_.size() * config_.ghostMultiplier);
  // NOTE: Ghost ring buffers are sized on first add() to avoid zero-capacity.
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
bool MMARC::Container<T, HookPtr>::recordAccess(T& node,
                                                AccessMode mode) noexcept {
  if ((mode == AccessMode::kWrite && !config_.updateOnWrite) ||
      (mode == AccessMode::kRead && !config_.updateOnRead)) {
    return false;
  }

  const auto curr = static_cast<Time>(util::getCurrentTimeSec());
  if (node.isInMMContainer() &&
      (curr >= getUpdateTime(node) +
                   lruRefreshTime_.load(std::memory_order_relaxed))) {
    auto func = [&]() {
      if (!node.isInMMContainer()) {
        return false;
      }

      if (isRecency(node)) {
        // Hit in T1: promote to T2
        lru_.getList(LruType::Recency).remove(node);
        lru_.getList(LruType::Frequency).linkAtHead(node);
        markFrequency(node);
        ++numRecencyHits_;
      } else if (isFrequency(node)) {
        // Hit in T2: move to head
        lru_.getList(LruType::Frequency).moveToHead(node);
        ++numFrequencyHits_;
      }

      setUpdateTime(node, curr);
      return true;
    };

    if (config_.tryLockUpdate) {
      if (auto lck = LockHolder{*lruMutex_, std::try_to_lock}) {
        return func();
      }
      return false;
    }

    return lruMutex_->lock_combine(func);
  }
  return false;
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
bool MMARC::Container<T, HookPtr>::add(T& node) noexcept {
  const auto currTime = static_cast<Time>(util::getCurrentTimeSec());
  return lruMutex_->lock_combine([this, &node, currTime]() {
    if (node.isInMMContainer()) {
      return false;
    }

    // Initialize max ghost size on first add if not set
    if (maxGhostSize_ == 0) {
      size_t currentSize = lru_.size();
      size_t estimatedCapacity = std::max(currentSize, static_cast<size_t>(1000));
      maxGhostSize_ = static_cast<size_t>(estimatedCapacity * config_.ghostMultiplier);
      partitionSize_ = estimatedCapacity / 2;

      // Set capacity for optimized ghost lists
      ghostRecency_.setCapacity(maxGhostSize_);
      ghostFrequency_.setCapacity(maxGhostSize_);
    }

    const uint32_t hash = hashNode(node);
    bool inGhostRecency = isInGhostRecency(hash);
    bool inGhostFrequency = isInGhostFrequency(hash);

    if (inGhostRecency) {
      // B1 hit: adapt and add to T2
      adaptOnGhostRecencyHit();
      removeFromGhost(hash);
      lru_.getList(LruType::Frequency).linkAtHead(node);
      markFrequency(node);
    } else if (inGhostFrequency) {
      // B2 hit: adapt and add to T2
      adaptOnGhostFrequencyHit();
      removeFromGhost(hash);
      lru_.getList(LruType::Frequency).linkAtHead(node);
      markFrequency(node);
    } else {
      // New item: add to T1
      lru_.getList(LruType::Recency).linkAtHead(node);
      markRecency(node);
    }

    node.markInMMContainer();
    setUpdateTime(node, currTime);
    return true;
  });
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
typename MMARC::Container<T, HookPtr>::LockedIterator
MMARC::Container<T, HookPtr>::getEvictionIterator() const noexcept {
  LockHolder l(*lruMutex_);
  return LockedIterator{std::move(l), lru_.rbegin()};
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
template <typename F>
void MMARC::Container<T, HookPtr>::withEvictionIterator(F&& fun) {
  if (config_.useCombinedLockForIterators) {
    lruMutex_->lock_combine([this, &fun]() { fun(Iterator{lru_.rbegin()}); });
  } else {
    LockHolder lck{*lruMutex_};
    fun(Iterator{lru_.rbegin()});
  }
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
template <typename F>
void MMARC::Container<T, HookPtr>::withContainerLock(F&& fun) {
  lruMutex_->lock_combine([&fun]() { fun(); });
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
void MMARC::Container<T, HookPtr>::removeLocked(T& node) noexcept {
  LruType type = getLruType(node);
  lru_.getList(type).remove(node);
  node.unmarkInMMContainer();
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
bool MMARC::Container<T, HookPtr>::remove(T& node) noexcept {
  return lruMutex_->lock_combine([this, &node]() {
    if (!node.isInMMContainer()) {
      return false;
    }
    removeLocked(node);
    return true;
  });
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
void MMARC::Container<T, HookPtr>::remove(Iterator& it) noexcept {
  T& node = *it;
  XDCHECK(node.isInMMContainer());
  ++it;
  removeLocked(node);
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
bool MMARC::Container<T, HookPtr>::replace(T& oldNode, T& newNode) noexcept {
  return lruMutex_->lock_combine([this, &oldNode, &newNode]() {
    if (!oldNode.isInMMContainer() || newNode.isInMMContainer()) {
      return false;
    }

    const auto updateTime = getUpdateTime(oldNode);
    LruType type = getLruType(oldNode);

    lru_.getList(type).replace(oldNode, newNode);

    // Copy flags
    if (isRecency(oldNode)) {
      markRecency(newNode);
    } else if (isFrequency(oldNode)) {
      markFrequency(newNode);
    }

    oldNode.unmarkInMMContainer();
    newNode.markInMMContainer();
    setUpdateTime(newNode, updateTime);
    return true;
  });
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
typename MMARC::LruType MMARC::Container<T, HookPtr>::getLruType(
    const T& node) const noexcept {
  if (isRecency(node)) {
    return LruType::Recency;
  }
  return LruType::Frequency;
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
void MMARC::Container<T, HookPtr>::setConfig(const Config& newConfig) {
  lruMutex_->lock_combine([this, &newConfig]() {
    config_ = newConfig;
    lruRefreshTime_.store(config_.lruRefreshTime, std::memory_order_relaxed);
    size_t capacity = lru_.size();
    if (capacity > 0) {
      maxGhostSize_ = static_cast<size_t>(capacity * config_.ghostMultiplier);
      ghostRecency_.setCapacity(maxGhostSize_);
      ghostFrequency_.setCapacity(maxGhostSize_);
    }
  });
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
typename MMARC::Config MMARC::Container<T, HookPtr>::getConfig() const {
  return lruMutex_->lock_combine([this]() { return config_; });
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
EvictionAgeStat MMARC::Container<T, HookPtr>::getEvictionAgeStat(
    uint64_t projectedLength) const noexcept {
  return lruMutex_->lock_combine([this, projectedLength]() {
    return getEvictionAgeStatLocked(projectedLength);
  });
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
EvictionAgeStat MMARC::Container<T, HookPtr>::getEvictionAgeStatLocked(
    uint64_t /* projectedLength */) const noexcept {
  const auto currTime = static_cast<Time>(util::getCurrentTimeSec());

  EvictionAgeStat stat;

  // Recency queue stats
  auto recencyTail = lru_.getList(LruType::Recency).getTail();
  stat.hotQueueStat.oldestElementAge =
      recencyTail ? currTime - getUpdateTime(*recencyTail) : 0;
  stat.hotQueueStat.size = lru_.getList(LruType::Recency).size();

  // Frequency queue stats
  auto frequencyTail = lru_.getList(LruType::Frequency).getTail();
  stat.warmQueueStat.oldestElementAge =
      frequencyTail ? currTime - getUpdateTime(*frequencyTail) : 0;
  stat.warmQueueStat.size = lru_.getList(LruType::Frequency).size();

  return stat;
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
serialization::MMARCObject MMARC::Container<T, HookPtr>::saveState()
    const noexcept {
  serialization::MMARCConfig configObject;
  *configObject.lruRefreshTime() =
      lruRefreshTime_.load(std::memory_order_relaxed); // IMPORTANT: load()
  *configObject.updateOnWrite() = config_.updateOnWrite;
  *configObject.updateOnRead() = config_.updateOnRead;
  *configObject.tryLockUpdate() = config_.tryLockUpdate;
  *configObject.ghostMultiplier() = config_.ghostMultiplier;

  serialization::MMARCObject object;
  *object.config() = configObject;
  *object.lrus() = lru_.saveState();
  *object.partitionSize() = static_cast<int32_t>(partitionSize_);
  return object;
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
MMContainerStat MMARC::Container<T, HookPtr>::getStats() const noexcept {
  return lruMutex_->lock_combine([this]() {
    auto* tail = lru_.size() == 0 ? nullptr : lru_.rbegin().get();

    return MMContainerStat{
        lru_.size(),
        tail == nullptr ? 0 : getUpdateTime(*tail),
        lruRefreshTime_.load(std::memory_order_relaxed),
        numRecencyHits_,
        numFrequencyHits_,
        0, // warm accesses (not used in ARC)
        numGhostRecencyHits_ + numGhostFrequencyHits_};
  });
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
MMARC::Container<T, HookPtr>::LockedIterator::LockedIterator(
    LockHolder l, const Iterator& iter) noexcept
    : Iterator(iter), l_(std::move(l)) {}

} // namespace facebook::cachelib
