// CARList.h
#pragma once

#include <folly/logging/xlog.h>

#include <algorithm>
#include <atomic>
#include <vector>
#include <unordered_set>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include "cachelib/allocator/serialize/gen-cpp2/objects_types.h"
#pragma GCC diagnostic pop

#include "cachelib/allocator/datastruct/DList.h"
#include "cachelib/common/CompilerUtils.h"

#include <folly/lang/Aligned.h>
#include <folly/synchronization/DistributedMutex.h>

namespace facebook::cachelib {

// CARList: CLOCK with Adaptive Replacement.
// NOTE: Compared to ARC's O(1) tail victim, CLOCK may scan and clear ref bits.
// To keep throughput high, we keep scan bound tight and set ref bits eagerly
// on hot T2 hits (see MMCAR::recordAccess fast-path).
template <typename T, DListHook<T> T::*HookPtr>
class CARList {
 public:
  using Mutex = folly::DistributedMutex;
  using LockHolder = std::unique_lock<Mutex>;
  using CompressedPtrType = typename T::CompressedPtrType;
  using PtrCompressor = typename T::PtrCompressor;
  using DList = facebook::cachelib::DList<T, HookPtr>;
  using RefFlags = typename T::Flags;
  using CARListObject = serialization::CARListObject;

  struct ClockHand {
    T* current{nullptr};
    void reset() { current = nullptr; }
    bool isValid() const { return current != nullptr; }
  };

  CARList() = default;
  CARList(const CARList&) = delete;
  CARList& operator=(const CARList&) = delete;
  ~CARList() = default;

  explicit CARList(PtrCompressor compressor) noexcept
      : listT1_(std::make_unique<DList>(compressor)),
        listT2_(std::make_unique<DList>(compressor)),
        compressor_(compressor) {}

  CARList(const CARListObject& object, PtrCompressor compressor)
      : listT1_(std::make_unique<DList>(*object.listT1(), compressor)),
        listT2_(std::make_unique<DList>(*object.listT2(), compressor)),
        compressor_(compressor),
        partitionSize_(*object.partitionSize()),
        capacity_(*object.capacity()) {
    for (const auto& entry : *object.ghostB1()) {
      ghostB1_.loadInsert(static_cast<uint32_t>(*entry.hash()),
                          static_cast<uint32_t>(*entry.time()));
    }
    for (const auto& entry : *object.ghostB2()) {
      ghostB2_.loadInsert(static_cast<uint32_t>(*entry.hash()),
                          static_cast<uint32_t>(*entry.time()));
    }
  }

  CARListObject saveState() const {
    CARListObject state;
    *state.listT1() = listT1_->saveState();
    *state.listT2() = listT2_->saveState();
    *state.partitionSize() = partitionSize_;
    *state.capacity() = capacity_;
    std::vector<serialization::GhostEntry> b1Entries;
    for (const auto& entry : ghostB1_) {
      serialization::GhostEntry e;
      *e.hash() = static_cast<int32_t>(entry.hash);
      *e.time() = static_cast<int32_t>(entry.time);
      b1Entries.push_back(e);
    }
    *state.ghostB1() = b1Entries;
    std::vector<serialization::GhostEntry> b2Entries;
    for (const auto& entry : ghostB2_) {
      serialization::GhostEntry e;
      *e.hash() = static_cast<int32_t>(entry.hash);
      *e.time() = static_cast<int32_t>(entry.time);
      b2Entries.push_back(e);
    }
    *state.ghostB2() = b2Entries;
    return state;
  }

  // Flags:
  // kMMFlag0: in T1
  // kMMFlag1: in T2
  // kMMFlag2: CLOCK reference bit
  void setT1(T& node) noexcept {
    node.template setFlag<RefFlags::kMMFlag0>();
    node.template unSetFlag<RefFlags::kMMFlag1>();
  }
  void setT2(T& node) noexcept {
    node.template unSetFlag<RefFlags::kMMFlag0>();
    node.template setFlag<RefFlags::kMMFlag1>();
  }
  void unsetAll(T& node) noexcept {
    node.template unSetFlag<RefFlags::kMMFlag0>();
    node.template unSetFlag<RefFlags::kMMFlag1>();
    node.template unSetFlag<RefFlags::kMMFlag2>();
  }
  bool isT1(const T& node) const noexcept {
    return node.template isFlagSet<RefFlags::kMMFlag0>();
  }
  bool isT2(const T& node) const noexcept {
    return node.template isFlagSet<RefFlags::kMMFlag1>();
  }

  void setRefBit(T& node) noexcept { node.template setFlag<RefFlags::kMMFlag2>(); }
  void clearRefBit(T& node) noexcept { node.template unSetFlag<RefFlags::kMMFlag2>(); }
  bool getRefBit(const T& node) const noexcept {
    return node.template isFlagSet<RefFlags::kMMFlag2>();
  }

  size_t getT1Size() const noexcept { return listT1_->size(); }
  size_t getT2Size() const noexcept { return listT2_->size(); }
  size_t size() const noexcept { return listT1_->size() + listT2_->size(); }

  size_t getB1Size() const noexcept { return ghostB1_.size(); }
  size_t getB2Size() const noexcept { return ghostB2_.size(); }

  uint32_t getPartitionSize() const noexcept { return partitionSize_; }
  void setPartitionSize(uint32_t p) noexcept { partitionSize_ = p; }

  uint32_t getCapacity() const noexcept { return capacity_; }
  void setCapacity(uint32_t c) noexcept {
    capacity_ = c;
    if (partitionSize_ == 0) {
      partitionSize_ = c / 2;
    }
  }

  void addToT1(T& node) noexcept {
    listT1_->linkAtHead(node);
    setT1(node);
    clearRefBit(node);
  }
  void addToT2(T& node) noexcept {
    listT2_->linkAtHead(node);
    setT2(node);
    clearRefBit(node);
  }

  void remove(T& node) noexcept {
    if (isT1(node)) {
      listT1_->remove(node);
    } else if (isT2(node)) {
      listT2_->remove(node);
    }
    unsetAll(node);
  }

  void moveT1ToT2(T& node) noexcept {
    if (isT1(node)) {
      listT1_->remove(node);
      listT2_->linkAtHead(node);
      setT2(node);
    }
    setRefBit(node);
  }

  void markAccessed(T& node) noexcept { setRefBit(node); }

  struct GhostEntry {
    uint32_t hash;
    uint32_t time;
  };

  class GhostList {
   public:
    GhostList() = default;
    auto begin() const noexcept { return buffer_.begin(); }
    auto end() const noexcept { return buffer_.end(); }
    void setCapacity(size_t cap) {
      capacity_ = cap;
      buffer_.reserve(cap);
      hashSet_.reserve(cap);
    }
    void loadInsert(uint32_t hash, uint32_t time) {
      buffer_.push_back({hash, time});
      hashSet_.insert(hash);
    }
    bool contains(uint32_t hash) const noexcept {
      return hashSet_.find(hash) != hashSet_.end();
    }
    void insert(uint32_t hash, uint32_t time) noexcept {
      if (hashSet_.find(hash) != hashSet_.end()) {
        return;
      }
      if (buffer_.size() >= capacity_) {
        uint32_t oldHash = buffer_[head_].hash;
        hashSet_.erase(oldHash);
        buffer_[head_] = {hash, time};
        head_ = (head_ + 1) % capacity_;
      } else {
        buffer_.push_back({hash, time});
      }
      hashSet_.insert(hash);
    }
    void remove(uint32_t hash) noexcept { hashSet_.erase(hash); }
    size_t size() const noexcept { return hashSet_.size(); }
    void clear() noexcept { buffer_.clear(); hashSet_.clear(); head_ = 0; }
   private:
    std::vector<GhostEntry> buffer_;
    std::unordered_set<uint32_t> hashSet_;
    size_t capacity_{0};
    size_t head_{0};
  };

  bool isInGhostB1(uint32_t hash) const noexcept { return ghostB1_.contains(hash); }
  bool isInGhostB2(uint32_t hash) const noexcept { return ghostB2_.contains(hash); }
  void addToGhostB1(uint32_t hash, uint32_t time) noexcept { ghostB1_.insert(hash, time); }
  void addToGhostB2(uint32_t hash, uint32_t time) noexcept { ghostB2_.insert(hash, time); }
  void removeFromGhost(uint32_t hash) noexcept { ghostB1_.remove(hash); ghostB2_.remove(hash); }
  void setMaxGhostSize(size_t size) noexcept {
    maxGhostSize_ = size;
    ghostB1_.setCapacity(size);
    ghostB2_.setCapacity(size);
  }

  void adaptOnB1Hit() noexcept {
    uint32_t delta = std::max(
        1u,
        static_cast<uint32_t>(ghostB2_.size()) /
            std::max(1u, static_cast<uint32_t>(ghostB1_.size())));
    partitionSize_ = std::min(partitionSize_ + delta, capacity_);
  }
  void adaptOnB2Hit() noexcept {
    uint32_t delta = std::max(
        1u,
        static_cast<uint32_t>(ghostB1_.size()) /
            std::max(1u, static_cast<uint32_t>(ghostB2_.size())));
    partitionSize_ = (partitionSize_ >= delta) ? (partitionSize_ - delta) : 0;
  }

  // CLOCK victim in T1
  T* findVictimInT1() noexcept {
    if (listT1_->size() == 0) return nullptr;
    if (!handT1_.isValid() || !isT1(*handT1_.current)) {
      handT1_.current = listT1_->getTail();
    }
    size_t scanned = 0;
    const size_t maxScan = listT1_->size() + listT1_->size() / 2; // tighter bound
    while (scanned < maxScan) {
      if (!handT1_.current || !isT1(*handT1_.current)) {
        handT1_.current = listT1_->getTail();
        if (!handT1_.current) return nullptr;
      }
      if (!getRefBit(*handT1_.current)) {
        T* victim = handT1_.current;
        auto* next = listT1_->getPrev(*handT1_.current);
        handT1_.current = next ? next : listT1_->getTail();
        return victim;
      }
      clearRefBit(*handT1_.current);
      handT1_.current = listT1_->getPrev(*handT1_.current);
      if (!handT1_.current) handT1_.current = listT1_->getTail();
      ++scanned;
    }
    return listT1_->getTail();
  }

  // CLOCK victim in T2
  T* findVictimInT2() noexcept {
    if (listT2_->size() == 0) return nullptr;
    if (!handT2_.isValid() || !isT2(*handT2_.current)) {
      handT2_.current = listT2_->getTail();
    }
    size_t scanned = 0;
    const size_t maxScan = listT2_->size() + listT2_->size() / 2; // tighter bound
    while (scanned < maxScan) {
      if (!handT2_.current || !isT2(*handT2_.current)) {
        handT2_.current = listT2_->getTail();
        if (!handT2_.current) return nullptr;
      }
      if (!getRefBit(*handT2_.current)) {
        T* victim = handT2_.current;
        auto* next = listT2_->getPrev(*handT2_.current);
        handT2_.current = next ? next : listT2_->getTail();
        return victim;
      }
      clearRefBit(*handT2_.current);
      handT2_.current = listT2_->getPrev(*handT2_.current);
      if (!handT2_.current) handT2_.current = listT2_->getTail();
      ++scanned;
    }
    return listT2_->getTail();
  }

  // Return victim and push its hash into B1/B2 accordingly.
  T* replace(uint32_t time) noexcept {
    const size_t t1Size = listT1_->size();
    const size_t t2Size = listT2_->size();
    if (t1Size == 0 && t2Size == 0) {
      return nullptr;
    }
    T* victim = nullptr;
    if (t1Size >= std::max(1u, partitionSize_)) {
      victim = findVictimInT1();
      if (victim) {
        uint32_t hash = hashNode(*victim);
        listT1_->remove(*victim);
        addToGhostB1(hash, time);
        unsetAll(*victim);
      }
    } else if (t2Size > 0) {
      victim = findVictimInT2();
      if (victim) {
        uint32_t hash = hashNode(*victim);
        listT2_->remove(*victim);
        addToGhostB2(hash, time);
        unsetAll(*victim);
      }
    }
    return victim;
  }

  static uint32_t hashNode(const T& node) noexcept {
    return static_cast<uint32_t>(
        folly::hasher<folly::StringPiece>()(node.getKey()));
  }

 private:
  std::unique_ptr<DList> listT1_;
  std::unique_ptr<DList> listT2_;

  ClockHand handT1_;
  ClockHand handT2_;

  GhostList ghostB1_;
  GhostList ghostB2_;

  uint32_t partitionSize_{0};
  uint32_t capacity_{0};
  size_t maxGhostSize_{0};

  PtrCompressor compressor_;
};

}  // namespace facebook::cachelib
