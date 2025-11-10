// MMCAR.h
#pragma once

#include <atomic>
#include <cstring>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include <folly/Format.h>
#pragma GCC diagnostic pop
#include <folly/lang/Aligned.h>
#include <folly/synchronization/DistributedMutex.h>

#include "cachelib/allocator/Cache.h"
#include "cachelib/allocator/CacheStats.h"
#include "cachelib/allocator/Util.h"
#include "cachelib/allocator/datastruct/CARList.h"
#include "cachelib/allocator/memory/serialize/gen-cpp2/objects_types.h"
#include "cachelib/common/CompilerUtils.h"
#include "cachelib/common/Mutex.h"

namespace facebook::cachelib {

template <typename MMType>
class MMTypeTest;

// CAR (Clock with Adaptive Replacement)
class MMCAR {
 public:
  static const int kId;

  template <typename T>
  using Hook = DListHook<T>;
  using SerializationType = serialization::MMCARObject;
  using SerializationConfigType = serialization::MMCARConfig;
  using SerializationTypeContainer = serialization::MMCARCollection;

  enum LruType {
    Recency,    // T1
    Frequency,  // T2
    NumTypes
  };

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
    Config(const Config&) = default;
    Config(Config&&) = default;
    Config& operator=(const Config&) = default;
    Config& operator=(Config&&) = default;

    void checkConfig() {
      if (ghostMultiplier <= 0.0 || ghostMultiplier > 10.0) {
        throw std::invalid_argument(
            folly::sformat("Invalid ghostMultiplier: {}", ghostMultiplier));
      }
    }

    void addExtraConfig(size_t /* tailSize */) {}

    uint32_t defaultLruRefreshTime{60};
    uint32_t lruRefreshTime{defaultLruRefreshTime};

    bool updateOnWrite{false};
    bool updateOnRead{true};

    bool tryLockUpdate{false};

    double ghostMultiplier{1.0};

    bool useCombinedLockForIterators{false};
  };

  template <typename T, Hook<T> T::*HookPtr>
  struct Container {
   private:
    using CARListType = CARList<T, HookPtr>;
    using Mutex = folly::DistributedMutex;
    using LockHolder = std::unique_lock<Mutex>;
    using PtrCompressor = typename T::PtrCompressor;
    using Time = typename Hook<T>::Time;
    using CompressedPtrType = typename T::CompressedPtrType;
    using RefFlags = typename T::Flags;

   public:
    Container() = default;
    Container(Config c, PtrCompressor compressor)
        : carList_(compressor), config_(std::move(c)) {
      lruRefreshTime_ = config_.lruRefreshTime;
    }

    Container(const serialization::MMCARObject& object,
              PtrCompressor compressor);

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;

    void dump(std::ostream& /* out */) {}

    // Lightweight iterator that produces victims by calling CAR replace().
    class Iterator {
     public:
      Iterator() = default;
      explicit Iterator(T* node) : current_(node) {}

      T& operator*() { return *current_; }
      T* operator->() { return current_; }
      T* get() const { return current_; }

      Iterator& operator++() {
        current_ = nullptr;
        return *this;
      }

      bool operator==(const Iterator& other) const {
        return current_ == other.current_;
      }
      bool operator!=(const Iterator& other) const {
        return current_ != other.current_;
      }

      void reset() { current_ = nullptr; }
      void resetToBegin() { current_ = nullptr; }

      // allow boolean check
      explicit operator bool() const noexcept { return current_ != nullptr; }
      bool operator!() const noexcept { return current_ == nullptr; }
     private:
      T* current_{nullptr};
    };

    // Locked iterator that **actually drives eviction** by calling carList_.replace(now).
    class LockedIterator : public Iterator {
     public:
      LockedIterator(const LockedIterator&) = delete;
      LockedIterator& operator=(const LockedIterator&) = delete;
      LockedIterator(LockedIterator&&) noexcept = default;

      void destroy() {
        Iterator::reset();
        parent_ = nullptr;
        if (l_.owns_lock()) {
          l_.unlock();
        }
      }

      // IMPORTANT: produce the first victim under lock.
      void resetToBegin() {
        if (!l_.owns_lock()) {
          l_.lock();
        }
        Iterator::reset();
        Iterator::operator++(); // clear any stale
        fetchNextVictim();
      }

      // ++ should get the next victim (still under lock).
      LockedIterator& operator++() {
        fetchNextVictim();
        return *this;
      }

      explicit operator bool() const noexcept {
        return Iterator::operator bool();
      }
      bool operator!() const noexcept { return Iterator::operator!(); }

     private:
      LockedIterator& operator=(LockedIterator&&) noexcept = default;

      LockedIterator(LockHolder l,
                     const Iterator& iter,
                     Container* parent) noexcept
          : Iterator(iter), l_(std::move(l)), parent_(parent) {}

      void fetchNextVictim() {
        if (!parent_) {
          Iterator::reset();
          return;
        }
        const auto now = static_cast<Time>(util::getCurrentTimeSec());
        T* v = parent_->carList_.replace(static_cast<uint32_t>(now));
        if (v) {
          Iterator tmp(v);
          static_cast<Iterator&>(*this) = tmp;
          // mark as not in container; remove() expects that
          // NOTE: carList_.replace() already called unset + ghost push.
          v->unmarkInMMContainer();
        } else {
          Iterator::reset();
        }
      }

      friend Container<T, HookPtr>;
      LockHolder l_;
      Container* parent_{nullptr};
    };

    bool recordAccess(T& node, AccessMode mode) noexcept;

    bool add(T& node) noexcept;

    bool remove(T& node) noexcept;

    void remove(Iterator& it) noexcept;

    bool replace(T& oldNode, T& newNode) noexcept;

    // Return a locked iterator that yields victims by CLOCK replace.
    LockedIterator getEvictionIterator() const noexcept;

    template <typename F>
    void withEvictionIterator(F&& f);

    template <typename F>
    void withContainerLock(F&& f);

    Config getConfig() const;
    void setConfig(const Config& newConfig);

    bool isEmpty() const noexcept { return size() == 0; }
    size_t size() const noexcept {
      return lruMutex_->lock_combine([this]() { return carList_.size(); });
    }

    EvictionAgeStat getEvictionAgeStat(uint64_t projectedLength) const noexcept;
    serialization::MMCARObject saveState() const noexcept;
    MMContainerStat getStats() const noexcept;

    LruType getLruType(const T& node) const noexcept;

   private:
    EvictionAgeStat getEvictionAgeStatLocked(
        uint64_t projectedLength) const noexcept;

    static Time getUpdateTime(const T& node) noexcept {
      return (node.*HookPtr).getUpdateTime();
    }
    static void setUpdateTime(T& node, Time time) noexcept {
      (node.*HookPtr).setUpdateTime(time);
    }

    void removeLocked(T& node) noexcept;

    mutable folly::cacheline_aligned<Mutex> lruMutex_;
    CARListType carList_;

    uint64_t numT1Hits_{0};
    uint64_t numT2Hits_{0};
    uint64_t numB1Hits_{0};
    uint64_t numB2Hits_{0};
    uint64_t numT1Evictions_{0};
    uint64_t numT2Evictions_{0};

    std::atomic<uint32_t> lruRefreshTime_{};
    Config config_{};

    friend class MMTypeTest<MMCAR>;
  };
};

/* Container Interface Implementation */
template <typename T, MMCAR::Hook<T> T::*HookPtr>
MMCAR::Container<T, HookPtr>::Container(
    const serialization::MMCARObject& object,
    PtrCompressor compressor)
    : carList_(*object.carList(), compressor), config_(*object.config()) {
  lruRefreshTime_ = config_.lruRefreshTime;
}

template <typename T, MMCAR::Hook<T> T::*HookPtr>
bool MMCAR::Container<T, HookPtr>::recordAccess(T& node,
                                                AccessMode mode) noexcept {
  if ((mode == AccessMode::kWrite && !config_.updateOnWrite) ||
      (mode == AccessMode::kRead && !config_.updateOnRead)) {
    return false;
  }

  const auto curr = static_cast<Time>(util::getCurrentTimeSec());

  // NOTE: Keep refresh gating for heavy write reduction; but allow a light
  // tryLock fast-path to set ref bit on hot T2 hits even if we skip a full move.
  if (node.isInMMContainer() &&
      (curr >= getUpdateTime(node) +
               lruRefreshTime_.load(std::memory_order_relaxed))) {

    auto func = [&]() {
      if (!node.isInMMContainer()) {
        return false;
      }
      if (carList_.isT1(node)) {
        carList_.moveT1ToT2(node);
        ++numT1Hits_;
      } else if (carList_.isT2(node)) {
        carList_.markAccessed(node); // set ref bit
        ++numT2Hits_;
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
  } else {
    // Lightweight best-effort: if it's already in T2, try to set ref bit
    if (config_.tryLockUpdate && node.isInMMContainer()) {
      if (auto lck = LockHolder{*lruMutex_, std::try_to_lock}) {
        if (carList_.isT2(node)) {
          carList_.markAccessed(node);
        }
      }
    }
  }
  return false;
}

template <typename T, MMCAR::Hook<T> T::*HookPtr>
bool MMCAR::Container<T, HookPtr>::add(T& node) noexcept {
  const auto currTime = static_cast<Time>(util::getCurrentTimeSec());
  return lruMutex_->lock_combine([this, &node, currTime]() {
    if (node.isInMMContainer()) {
      return false;
    }

    // Initialize capacity and ghost limits once at first add.
    if (carList_.getCapacity() == 0) {
      size_t estimatedCapacity = std::max(carList_.size() + 1,
                                          static_cast<size_t>(1000));
      carList_.setCapacity(static_cast<uint32_t>(estimatedCapacity));
      carList_.setMaxGhostSize(
          static_cast<size_t>(estimatedCapacity * config_.ghostMultiplier));
    }

    const uint32_t hash = CARListType::hashNode(node);
    bool inB1 = carList_.isInGhostB1(hash);
    bool inB2 = carList_.isInGhostB2(hash);

    if (inB1) {
      carList_.adaptOnB1Hit();
      carList_.removeFromGhost(hash);
      carList_.addToT2(node);
      ++numB1Hits_;
    } else if (inB2) {
      carList_.adaptOnB2Hit();
      carList_.removeFromGhost(hash);
      carList_.addToT2(node);
      ++numB2Hits_;
    } else {
      carList_.addToT1(node);
    }

    node.markInMMContainer();
    setUpdateTime(node, currTime);
    return true;
  });
}

template <typename T, MMCAR::Hook<T> T::*HookPtr>
typename MMCAR::Container<T, HookPtr>::LockedIterator
MMCAR::Container<T, HookPtr>::getEvictionIterator() const noexcept {
  // IMPORTANT: Hold the lock and feed victims by calling CAR replace().
  LockHolder l(*lruMutex_);
  return LockedIterator{std::move(l), Iterator{}, const_cast<Container*>(this)};
}

template <typename T, MMCAR::Hook<T> T::*HookPtr>
template <typename F>
void MMCAR::Container<T, HookPtr>::withEvictionIterator(F&& fun) {
  if (config_.useCombinedLockForIterators) {
    lruMutex_->lock_combine([this, &fun]() {
      LockedIterator it{LockHolder{*lruMutex_}, Iterator{}, this};
      it.resetToBegin();
      fun(it);
      it.destroy();
    });
  } else {
    LockHolder lck{*lruMutex_};
    LockedIterator it{std::move(lck), Iterator{}, this};
    it.resetToBegin();
    fun(it);
    it.destroy();
  }
}

template <typename T, MMCAR::Hook<T> T::*HookPtr>
template <typename F>
void MMCAR::Container<T, HookPtr>::withContainerLock(F&& fun) {
  lruMutex_->lock_combine([&fun]() { fun(); });
}

template <typename T, MMCAR::Hook<T> T::*HookPtr>
void MMCAR::Container<T, HookPtr>::removeLocked(T& node) noexcept {
  carList_.remove(node);
  node.unmarkInMMContainer();
}

template <typename T, MMCAR::Hook<T> T::*HookPtr>
bool MMCAR::Container<T, HookPtr>::remove(T& node) noexcept {
  return lruMutex_->lock_combine([this, &node]() {
    if (!node.isInMMContainer()) {
      return false;
    }
    removeLocked(node);
    return true;
  });
}

template <typename T, MMCAR::Hook<T> T::*HookPtr>
void MMCAR::Container<T, HookPtr>::remove(Iterator& it) noexcept {
  if (!it) {
    return;
  }
  T& node = *it;
  XDCHECK(node.isInMMContainer());
  ++it;
  removeLocked(node);
}

template <typename T, MMCAR::Hook<T> T::*HookPtr>
bool MMCAR::Container<T, HookPtr>::replace(T& oldNode, T& newNode) noexcept {
  return lruMutex_->lock_combine([this, &oldNode, &newNode]() {
    if (!oldNode.isInMMContainer() || newNode.isInMMContainer()) {
      return false;
    }
    const auto updateTime = getUpdateTime(oldNode);
    bool wasT1 = carList_.isT1(oldNode);
    bool wasT2 = carList_.isT2(oldNode);
    bool refBit = carList_.getRefBit(oldNode);

    carList_.remove(oldNode);

    if (wasT1) {
      carList_.addToT1(newNode);
    } else if (wasT2) {
      carList_.addToT2(newNode);
    }
    if (refBit) {
      carList_.setRefBit(newNode);
    }

    oldNode.unmarkInMMContainer();
    newNode.markInMMContainer();
    setUpdateTime(newNode, updateTime);
    return true;
  });
}

template <typename T, MMCAR::Hook<T> T::*HookPtr>
typename MMCAR::LruType MMCAR::Container<T, HookPtr>::getLruType(
    const T& node) const noexcept {
  if (carList_.isT1(node)) {
    return LruType::Recency;
  }
  return LruType::Frequency;
}

template <typename T, MMCAR::Hook<T> T::*HookPtr>
void MMCAR::Container<T, HookPtr>::setConfig(const Config& newConfig) {
  lruMutex_->lock_combine([this, &newConfig]() {
    config_ = newConfig;
    lruRefreshTime_.store(config_.lruRefreshTime, std::memory_order_relaxed);
    size_t capacity = carList_.getCapacity();
    if (capacity > 0) {
      carList_.setMaxGhostSize(
          static_cast<size_t>(capacity * config_.ghostMultiplier));
    }
  });
}

template <typename T, MMCAR::Hook<T> T::*HookPtr>
typename MMCAR::Config MMCAR::Container<T, HookPtr>::getConfig() const {
  return lruMutex_->lock_combine([this]() { return config_; });
}

template <typename T, MMCAR::Hook<T> T::*HookPtr>
EvictionAgeStat MMCAR::Container<T, HookPtr>::getEvictionAgeStat(
    uint64_t projectedLength) const noexcept {
  return lruMutex_->lock_combine([this, projectedLength]() {
    return getEvictionAgeStatLocked(projectedLength);
  });
}

template <typename T, MMCAR::Hook<T> T::*HookPtr>
EvictionAgeStat MMCAR::Container<T, HookPtr>::getEvictionAgeStatLocked(
    uint64_t /* projectedLength */) const noexcept {
  EvictionAgeStat stat;
  stat.hotQueueStat.size = carList_.getT1Size();
  stat.warmQueueStat.size = carList_.getT2Size();
  return stat;
}

template <typename T, MMCAR::Hook<T> T::*HookPtr>
serialization::MMCARObject MMCAR::Container<T, HookPtr>::saveState()
    const noexcept {
  serialization::MMCARConfig configObject;
  *configObject.lruRefreshTime() = lruRefreshTime_;
  *configObject.updateOnWrite() = config_.updateOnWrite;
  *configObject.updateOnRead() = config_.updateOnRead;
  *configObject.tryLockUpdate() = config_.tryLockUpdate;
  *configObject.ghostMultiplier() = config_.ghostMultiplier;

  serialization::MMCARObject object;
  *object.config() = configObject;
  *object.carList() = carList_.saveState();
  return object;
}

template <typename T, MMCAR::Hook<T> T::*HookPtr>
MMContainerStat MMCAR::Container<T, HookPtr>::getStats() const noexcept {
  return lruMutex_->lock_combine([this]() {
    return MMContainerStat{
        carList_.size(),
        0,  // CAR doesn't report tail age
        lruRefreshTime_.load(std::memory_order_relaxed),
        numT1Hits_,
        numT2Hits_,
        0,  // warm accesses (unused)
        numB1Hits_ + numB2Hits_};
  });
}

}  // namespace facebook::cachelib
