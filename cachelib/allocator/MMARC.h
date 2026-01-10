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
#include "cachelib/allocator/datastruct/ARCList.h"
#include "cachelib/allocator/memory/serialize/gen-cpp2/objects_types.h"
#include "cachelib/common/CompilerUtils.h"
#include "cachelib/common/Mutex.h"

namespace facebook::cachelib {

class MMARC {
public:
  static const int kId;

  template <typename T>
  using Hook = DListHook<T>;
  using SerializationType = serialization::MMARCObject;
  using SerializationConfigType = serialization::MMARCConfig;
  using SerializationTypeContainer = serialization::MMARCCollection;

  enum LruType {
    Recency,
    Frequency,
    NumTypes
  };

  struct Config {
    explicit Config(SerializationConfigType configState)
        : Config(*configState.updateOnWrite(),
                 *configState.updateOnRead()) {}

    Config(bool updateOnW, bool updateOnR)
        : Config(updateOnW, updateOnR, 0) {}

    Config(bool updateOnW, bool updateOnR, uint64_t mmReconfigureInterval)
        : updateOnWrite(updateOnW),
          updateOnRead(updateOnR),
          mmReconfigureIntervalSecs(
              std::chrono::seconds(mmReconfigureInterval)) {}

    Config() = default;
    Config(const Config&) = default;
    Config(Config&&) = default;

    Config& operator=(const Config&) = default;
    Config& operator=(Config&&) = default;

    template <typename... Args>
    void addExtraConfig(Args...) {}

    bool updateOnWrite{false};
    bool updateOnRead{true};

    std::chrono::seconds mmReconfigureIntervalSecs{};
    bool useCombinedLockForIterators{false};
  };

  template <typename T, Hook<T> T::*HookPtr>
  struct Container {
   private:
    using LIST = ARCList<T, HookPtr>;
    using Mutex = folly::DistributedMutex;
    using LockHolder = std::unique_lock<Mutex>;
    using PtrCompressor = typename T::PtrCompressor;
    using CompressedPtrType = typename T::CompressedPtrType;
    using RefFlags = typename T::Flags;
    using Time = typename Hook<T>::Time;

   public:
    Container() = default;
    Container(Config c, PtrCompressor compressor)
        : arclist_(std::move(compressor)), config_(std::move(c)) {}

    Container(serialization::MMARCObject object, PtrCompressor compressor);

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;
    void dump(std::ostream& ) {}

    class LockedIterator {
     public:
      LockedIterator(const LockedIterator&) = delete;
      LockedIterator& operator=(const LockedIterator&) = delete;

      LockedIterator(LockedIterator&&) noexcept = default;
      LockedIterator& operator=(LockedIterator&&) noexcept = default;

      LockedIterator& operator++() {
        candidate_ = arclist_->getEvictionCandidate();
        return *this;
      }

      T* operator->() const noexcept { return candidate_; }
      T& operator*() const noexcept { return *candidate_; }
      explicit operator bool() const noexcept { return candidate_ != nullptr; }
      T* get() const noexcept { return candidate_; }

      void reset() noexcept {
        candidate_ = arclist_->getEvictionCandidate();
      }

      void destroy() noexcept {
        candidate_ = nullptr;
      }

      void resetToBegin() noexcept {
        candidate_ = arclist_->getEvictionCandidate();
      }

     private:
      LockedIterator(LockHolder l, LIST* arclist)
          : lock_(std::move(l)), arclist_(arclist) {
        candidate_ = arclist_->getEvictionCandidate();
      }

      LockHolder lock_;
      LIST* arclist_{nullptr};
      T* candidate_{nullptr};

      friend struct Container<T, HookPtr>;
    };

    bool recordAccess(T& node, AccessMode mode, int thread_id = 0) noexcept;
    bool add(T& node, int thread_id = 0) noexcept;
    bool remove(T& node, int thread_id = 0) noexcept;
    void remove(LockedIterator& it, int thread_id = 0) noexcept;
    bool replace(T& oldNode, T& newNode) noexcept;

    LockedIterator getEvictionIterator() noexcept;

    template <typename F>
    void withEvictionIterator(F&& fun, int thread_id = 0);

    template <typename F>
    void withContainerLock(F&& fun);

    Config getConfig() const;
    void setConfig(const Config& newConfig);

    bool isEmpty() const noexcept { return size() == 0; }

    void reconfigureLocked(const Time&) {}

    size_t size() const noexcept {
      return Mutex_->lock_combine([this]() { return arclist_.size(); });
    }

    EvictionAgeStat getEvictionAgeStat(uint64_t projectedLength) const noexcept;
    serialization::MMARCObject saveState() const noexcept;
    MMContainerStat getStats() const noexcept;

    void setRecency(T& node) noexcept {
      node.template setFlag<RefFlags::kMMFlag0>();
    }
    void unSetRecency(T& node) noexcept {
      node.template unSetFlag<RefFlags::kMMFlag0>();
    }
    bool isRecency(const T& node) const noexcept {
      return node.template isFlagSet<RefFlags::kMMFlag0>();
    }
    void setFrequency(T& node) noexcept {
      node.template setFlag<RefFlags::kMMFlag1>();
    }
    void unSetFrequency(T& node) noexcept {
      node.template unSetFlag<RefFlags::kMMFlag1>();
    }
    bool isFrequency(const T& node) const noexcept {
      return node.template isFlagSet<RefFlags::kMMFlag1>();
    }
    LruType getLruType(const T& node) const noexcept {
      if (isRecency(node)) {
        return LruType::Recency;
      } else {
        return LruType::Frequency;
      }
    }

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

    mutable folly::cacheline_aligned<Mutex> Mutex_;
    LIST arclist_{};
    Config config_{};
  };
};

template <typename T, MMARC::Hook<T> T::*HookPtr>
MMARC::Container<T, HookPtr>::Container(serialization::MMARCObject object,
                                        PtrCompressor compressor)
    : arclist_(*object.arclist(), compressor),
      config_(*object.config()) {}

template <typename T, MMARC::Hook<T> T::*HookPtr>
bool MMARC::Container<T, HookPtr>::recordAccess(T& node,
                                                AccessMode mode, int thread_id) noexcept {
  if ((mode == AccessMode::kWrite && !config_.updateOnWrite) ||
      (mode == AccessMode::kRead && !config_.updateOnRead)) {
    return false;
  }

  const auto curr = static_cast<Time>(util::getCurrentTimeSec());

  return Mutex_->lock_combine([this, &node, curr]() {
    if (node.isInMMContainer()) {
      setUpdateTime(node, curr);
      return true;
    }
    return false;
  });
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
EvictionAgeStat MMARC::Container<T, HookPtr>::getEvictionAgeStat(
    uint64_t projectedLength) const noexcept {
  return Mutex_->lock_combine(
      [this, projectedLength]() { return getEvictionAgeStatLocked(projectedLength); });
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
EvictionAgeStat
MMARC::Container<T, HookPtr>::getEvictionAgeStatLocked(
    uint64_t /*projectedLength*/) const noexcept {
  EvictionAgeStat stat{};
  return stat;
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
void MMARC::Container<T, HookPtr>::setConfig(const Config& newConfig) {
  Mutex_->lock_combine([this, newConfig]() { config_ = newConfig; });
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
typename MMARC::Config MMARC::Container<T, HookPtr>::getConfig() const {
  return Mutex_->lock_combine([this]() { return config_; });
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
bool MMARC::Container<T, HookPtr>::add(T& node, int thread_id) noexcept {
  const auto currTime = static_cast<Time>(util::getCurrentTimeSec());

  return Mutex_->lock_combine([this, &node, currTime]() {
    if (node.isInMMContainer()) {
      return false;
    }
    arclist_.add(node);
    node.markInMMContainer();
    setUpdateTime(node, currTime);
    return true;
  });
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
typename MMARC::Container<T, HookPtr>::LockedIterator
MMARC::Container<T, HookPtr>::getEvictionIterator() noexcept {
  LockHolder l(*Mutex_);
  return LockedIterator{std::move(l), &arclist_};
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
template <typename F>
void MMARC::Container<T, HookPtr>::withEvictionIterator(F&& fun, int thread_id) {
  // 简化版本：总是用 RAII 锁，和 MMLru 的非 combined 分支类似
  LockHolder l(*Mutex_);
  LockedIterator it{std::move(l), &arclist_};
  fun(it);
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
template <typename F>
void MMARC::Container<T, HookPtr>::withContainerLock(F&& fun) {
  Mutex_->lock_combine([&fun]() { fun(); });
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
void MMARC::Container<T, HookPtr>::removeLocked(T& node) noexcept {
  arclist_.remove(node);
  node.unmarkInMMContainer();
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
bool MMARC::Container<T, HookPtr>::remove(T& node, int thread_id) noexcept {
  return Mutex_->lock_combine([this, &node]() {
    if (!node.isInMMContainer()) {
      return false;
    }
    removeLocked(node);
    return true;
  });
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
void MMARC::Container<T, HookPtr>::remove(LockedIterator& it, int thread_id) noexcept {
  T& node = *it;
  XDCHECK(node.isInMMContainer());
  node.unmarkInMMContainer();
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
bool MMARC::Container<T, HookPtr>::replace(T& oldNode, T& newNode) noexcept {
  return Mutex_->lock_combine([this, &oldNode, &newNode]() {
    if (!oldNode.isInMMContainer() || newNode.isInMMContainer()) {
      return false;
    }
    const auto updateTime = getUpdateTime(oldNode);

    LruType type = getLruType(oldNode);
    switch (type) {
    case LruType::Recency:
      setRecency(newNode);
      arclist_.getRecency().replace(oldNode, newNode);
      break;
    case LruType::Frequency:
      setFrequency(newNode);
      arclist_.getFrequency().replace(oldNode, newNode);
      break;
    case LruType::NumTypes:
      XDCHECK(false);
    }

    oldNode.unmarkInMMContainer();
    newNode.markInMMContainer();
    setUpdateTime(newNode, updateTime);
    return true;
  });
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
serialization::MMARCObject
MMARC::Container<T, HookPtr>::saveState() const noexcept {
  return Mutex_->lock_combine([this]() {
    serialization::MMARCObject state;
    *state.arclist() = arclist_.saveState();
    auto configState = serialization::MMARCConfig();
    *configState.updateOnWrite() = config_.updateOnWrite;
    *configState.updateOnRead() = config_.updateOnRead;
    *state.config() = configState;
    return state;
  });
}

template <typename T, MMARC::Hook<T> T::*HookPtr>
MMContainerStat MMARC::Container<T, HookPtr>::getStats() const noexcept {
  auto stat = Mutex_->lock_combine(
      [this]() { return folly::make_array(arclist_.size()); });
  return {stat[0], 0, 0, 0, 0, 0, 0};
}

} // namespace facebook::cachelib
